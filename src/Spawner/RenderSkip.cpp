/**
*  yrpp-spawner
*
*  Copyright(C) 2023-present CnCNet
*
*  This program is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation, either version 3 of the License, or
*  (at your option) any later version.
*
*  This program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with this program.If not, see <http://www.gnu.org/licenses/>.
*/

#include "RenderSkip.h"

#include <windows.h>

#include <Helpers/Macro.h>
#include <GScreenClass.h>
#include <SessionClass.h>
#include <Unsorted.h>
#include <Utilities/Debug.h>

bool RenderSkip::Enabled = true;
int  RenderSkip::MinProcessMs = 20;
int  RenderSkip::RenderSharePercent = 25;

namespace
{
	// Draw two frames, then drop one - two frames in three.
	const int DrawRun = 2;

	// Engage fast, release slow. Flapping costs more than staying engaged a
	// little too long, and both inputs sit on coarse integers - at MaxAhead 4
	// the "behind" figure only takes five distinct values, so a threshold with
	// no band around it is crossed every frame.
	const int EngageFrames = 2;

	// Two seconds of sustained slack before letting go. At 30 frames the
	// throttle chattered 16 times in one match, because the process figure sits
	// a couple of milliseconds either side of the budget and every crossing
	// flipped it - which reads as the frame rate surging and dropping.
	const int ReleaseFrames = 120;

	// Render cost on its own, so the release decision can ask what a frame would
	// cost if every one were rendered. The process figure alone cannot answer
	// that - throttling is part of what it measures, so a release threshold
	// below the engage threshold can be permanently unreachable.
	int   g_renderCostUs = -1;   // EWMA, alpha = 1/4

	// Rendered and skipped frames within the engine's current ProcessingTicks
	// window, so the skip fraction describes the same frames as the average it
	// corrects. Queue_AI_Multiplayer zeroes that window every 128 frames; a drop
	// in ProcessingFrames is how the reset is seen from here.
	int   g_windowRendered = 0;
	int   g_windowSkipped = 0;
	int   g_lastWindowFrames = 0;

	// The average is only trusted once the window holds this many frames. At
	// four frames, one 80ms hitch - an explosion, a stall on disk - averages out
	// above the budget and engaged the throttle on a machine that keeps up fine.
	const int MinWindowFrames = 32;
	int   g_engagedAtFrame = 0;

	// Hard ceiling on one engagement. Even if the projection is wrong, the
	// throttle re-probes reality instead of latching.
	const int MaxEngagedFrames = 3600;

	bool  g_active = false;
	int   g_drawRun = 0;
	int   g_engageRun = 0;
	int   g_releaseRun = 0;

	int FrameBudgetMs()
	{
		const int fps = Game::Network::RequestedFPS;
		const int derived = fps > 0 ? (1000 / fps) : 16;
		return derived < RenderSkip::MinProcessMs ? RenderSkip::MinProcessMs : derived;
	}

	// Average cost of the frames in the current 128-frame accounting window.
	// Meaningless for the first few frames after each reset.
	int AverageProcessMs()
	{
		const int frames = Game::Network::ProcessingFrames;
		return frames >= 4 ? (Game::Network::ProcessingTicks / frames) : 0;
	}
}

void RenderSkip::NoteRenderCost(int us)
{
	if (us < 0 || us > 1000000)
		return;
	g_renderCostUs = (g_renderCostUs < 0) ? us : (g_renderCostUs + ((us - g_renderCostUs) >> 2));
}

void RenderSkip::Reset()
{
	g_renderCostUs = -1;
	g_windowRendered = 0;
	g_windowSkipped = 0;
	g_lastWindowFrames = 0;
	g_engagedAtFrame = 0;
	g_active = false;
	g_drawRun = 0;
	g_engageRun = 0;
	g_releaseRun = 0;
}

bool RenderSkip::ThrottleActive()
{
	return Enabled && g_active && !SessionClass::IsSingleplayer();
}

bool RenderSkip::ShouldRenderThisFrame()
{
	// Multiplayer only, and checked here rather than relying on configuration.
	// The settings are applied from Spawner::InitNetwork, which campaign and
	// skirmish never reach, so a single-player game would otherwise run on the
	// compile-time defaults and drop frames even with RenderSkip=no. The feature
	// exists to stop one slow client stalling its peers through the MaxAhead
	// gate, which has no meaning outside a network game.
	if (!Enabled || SessionClass::IsSingleplayer())
		return true;

	const int windowFrames = Game::Network::ProcessingFrames;
	if (windowFrames < g_lastWindowFrames)
	{
		g_windowRendered = 0;
		g_windowSkipped = 0;
	}
	g_lastWindowFrames = windowFrames;

	const int budget = FrameBudgetMs();
	const int average = AverageProcessMs();

	// Engages when the average frame reaches the budget AND rendering is at
	// least RenderSharePercent of it; releases on the projected full-render cost
	// falling below three quarters of the budget.
	//
	// Release below the engage point, judged on the projected full-render cost
	// rather than the throttled figure - the throttle itself lowers the process
	// figure, so judging that would guarantee oscillation: engage, cost drops,
	// release, cost rises. Against the projection a quarter-budget band is
	// enough. Half the budget was unreachable in any large battle, so one
	// engagement lasted until the forced re-probe and re-engaged straight after.
	const int releaseBudget = budget * 3 / 4;

	// There is no "is anyone waiting on us" test. The peers' frames are only
	// known from FRAMEINFO, sent every FrameSendRate frames and stamped up to
	// MaxAhead ahead, so our lag behind the slowest peer sits near -MaxAhead
	// even when every client is healthy (measured -13 to -35 at level 9) and
	// cannot say who is slow.
	//
	// Process time alone is the honest signal, and it is self-limiting: it only
	// exceeds the budget when this client genuinely cannot render and simulate a
	// frame inside the target frame time. Under lockstep the slowest simulator
	// sets the rate for everyone, so cutting our own cost is the one thing that
	// can raise it - and skipping a render is sync-neutral either way.
	// What a fully-rendered frame would cost: the measured average plus the
	// render we are currently not paying for.
	int projectedUs = average * 1000;
	if (g_renderCostUs > 0)
	{
		const int total = g_windowRendered + g_windowSkipped;
		if (total > 0 && g_windowSkipped > 0)
			projectedUs += static_cast<int>(static_cast<long long>(g_renderCostUs) * g_windowSkipped / total);
	}
	const int projected = projectedUs / 1000;

	// Only intervene when rendering is a real share of the frame. A client whose
	// time goes to simulation gains nothing from dropped frames - measured: a
	// client at 18-20ms per frame with a render cost rounding to zero. Checked
	// in microseconds; whole milliseconds read a 4.9ms render as 4 and failed a
	// 25% share of a 20ms frame. No sample yet (-1) does not block, so the
	// first engagement can happen before a render is timed.
	const bool renderIsTheCost = g_renderCostUs < 0
		|| static_cast<long long>(g_renderCostUs) * 100 >= static_cast<long long>(RenderSharePercent) * average * 1000;

	// Early in a window the average is a handful of frames and one hitch
	// dominates it. Hold both runs there rather than let it decide - neither
	// counting toward a change nor resetting progress already made.
	if (windowFrames >= MinWindowFrames)
	{
		if (!g_active)
			g_engageRun = (average >= budget && renderIsTheCost) ? g_engageRun + 1 : 0;
		else
			g_releaseRun = (projectedUs < releaseBudget * 1000) ? g_releaseRun + 1 : 0;
	}

	// Re-probe rather than latch: if we have been throttling for a long time,
	// drop it and let the next few frames say whether it is still needed.
	if (g_active && (int)Unsorted::CurrentFrame - g_engagedAtFrame > MaxEngagedFrames)
	{
		Debug::Log("[Audit] render re-probe frame=%d after %d frames engaged (process=%dms render=%dus projected=%dms budget=%dms)\n"
			, (int)Unsorted::CurrentFrame, (int)Unsorted::CurrentFrame - g_engagedAtFrame
			, average, g_renderCostUs, projected, budget);
		g_releaseRun = ReleaseFrames;
	}

	const bool throttle = g_active ? (g_releaseRun < ReleaseFrames) : (g_engageRun >= EngageFrames);

	if (!throttle)
	{
		if (g_active)
		{
			g_active = false;
			g_engageRun = 0;
			Debug::Log("[RenderSkip] released at frame %d (process=%dms projected=%dms budget=%dms)\n",
				(int)Unsorted::CurrentFrame, average, projected, budget);
		}
		// A full run, so the first throttled frame drops immediately.
		g_drawRun = DrawRun;
		++g_windowRendered;
		return true;
	}

	if (!g_active)
	{
		g_active = true;
		g_releaseRun = 0;
		g_engagedAtFrame = (int)Unsorted::CurrentFrame;
		Debug::Log("[RenderSkip] engaged at frame %d (process=%dms render=%dus share=%d%% budget=%dms)\n",
			(int)Unsorted::CurrentFrame, average, g_renderCostUs, RenderSharePercent, budget);
	}

	// DrawRun frames drawn, then one dropped.
	if (g_drawRun < DrawRun)
	{
		++g_drawRun;
		// Counted, or the projection treats every throttled frame as skipped
		// and overstates the full-render cost by up to the whole render.
		++g_windowRendered;
		return true;
	}

	g_drawRun = 0;
	++g_windowSkipped;
	return false;
}

// Main_Loop's render. This replaces the call rather than letting Syringe
// relocate it - the patched region is a relative call, which cannot be moved.
// ECX already holds &Map from the instruction before.
DEFINE_HOOK(0x55D8F2, MainLoop_Render_RenderSkip, 0x5)
{
	enum { Resume = 0x55D8F7 };

	GET(GScreenClass*, screen, ECX);

	if (RenderSkip::ShouldRenderThisFrame())
	{
		// QueryPerformanceCounter rather than the millisecond clocks: a render
		// costs single-digit milliseconds, which GetTickCount's ~15ms tick
		// cannot resolve at all, and timeGetTime would pull in winmm.
		LARGE_INTEGER freq {}, before {}, after {};
		const bool timed = QueryPerformanceFrequency(&freq) && freq.QuadPart > 0
			&& QueryPerformanceCounter(&before);

		screen->Render();

		if (timed && QueryPerformanceCounter(&after))
		{
			const long long us = ((after.QuadPart - before.QuadPart) * 1000000LL) / freq.QuadPart;
			RenderSkip::NoteRenderCost((int)us);
		}
	}

	return Resume;
}

// Sync_Delay's opportunistic block, entered only when at least 10ms of the
// frame budget is left over. It is Input + Keyboard_Process + Tactical::AI +
// Render, and it exists purely to spend that slack on drawing.
//
// Suppressing only its Render was wrong. Skipping Main_Loop's render leaves
// more slack behind, so this block starts being entered on frames where the
// engine would previously have fallen through to Sleep(0) - meaning
// Tactical::AI ran more often than before while the screen updated less often.
// View scrolling stepped faster and was drawn rarer, which is exactly the
// "jumps a lot more" symptom.
//
// Skipping the whole block restores the engine's own no-slack behaviour: jump
// to the Sleep(0) the `jle` at 0x55E23D would have taken.
DEFINE_HOOK(0x55E23F, SyncDelay_Opportunistic_RenderSkip, 0x8)
{
	enum { SkipBlock = 0x55E278 };

	return RenderSkip::ThrottleActive() ? SkipBlock : 0;
}
