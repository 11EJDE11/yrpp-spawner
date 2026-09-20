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
#include <IPXManagerClass.h>
#include <TheirSync.h>
#include <SessionClass.h>
#include <Unsorted.h>
#include <Utilities/Debug.h>
#include "NetDiagnostics.h"

bool RenderSkip::Enabled = true;
int  RenderSkip::MaxConsecutive = 4;
int  RenderSkip::BudgetMs = 0;

namespace
{


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
	int   g_renderCostMs = -1;   // EWMA, alpha = 1/4
	int   g_windowRendered = 0;
	int   g_windowSkipped = 0;
	int   g_engagedAtFrame = 0;

	// Hard ceiling on one engagement. Even if the projection is wrong, the
	// throttle re-probes reality instead of latching.
	const int MaxEngagedFrames = 3600;

	bool  g_active = false;
	int   g_consecutiveSkips = 0;
	int   g_engageRun = 0;
	int   g_releaseRun = 0;
	int   g_skipped = 0;
	int   g_rendered = 0;
	DWORD g_lastSummaryTick = 0;

	int ConnectionCount()
	{
		int nconn = static_cast<int>(IPXManagerClass::Instance.NumConnections);
		const int arraySize = sizeof(IPXManagerClass::Instance.Connection) / sizeof(IPXManagerClass::Instance.Connection[0]);
		if (nconn < 0) nconn = 0;
		if (nconn > arraySize) nconn = arraySize;
		return nconn;
	}

	// How far the slowest peer has run ahead of us. Positive means the others
	// are waiting on this client, which is the only case where dropping our own
	// render buys the game anything.
	int FramesBehindPeers()
	{
		const int nconn = ConnectionCount();
		if (nconn <= 0)
			return 0;

		int slowest = TheirSync::Array[0].Frame;
		for (int i = 1; i < nconn; ++i)
			if (TheirSync::Array[i].Frame < slowest)
				slowest = TheirSync::Array[i].Frame;

		return slowest - static_cast<int>(Unsorted::CurrentFrame);
	}

	// Below this there is no point intervening: rendering is not what is making
	// the client slow, and dropping frames would cost picture quality for nothing.
	const int MinimumBudgetMs = 20;

	int FrameBudgetMs()
	{
		if (RenderSkip::BudgetMs > 0)
			return RenderSkip::BudgetMs;

		const int fps = Game::Network::RequestedFPS;
		const int derived = fps > 0 ? (1000 / fps) : 16;
		return derived < MinimumBudgetMs ? MinimumBudgetMs : derived;
	}

	// Average cost of the frames in the current 128-frame accounting window.
	// Meaningless for the first few frames after each reset.
	int AverageProcessMs()
	{
		const int frames = Game::Network::ProcessingFrames;
		return frames >= 4 ? (Game::Network::ProcessingTicks / frames) : 0;
	}

	void Summarise()
	{
		const DWORD now = GetTickCount();
		if (g_lastSummaryTick != 0 && (now - g_lastSummaryTick) < 5000)
			return;
		g_lastSummaryTick = now;

		if (g_skipped == 0 && g_rendered == 0)
			return;

		g_windowRendered = 0;
		g_windowSkipped = 0;


		g_skipped = 0;
		g_rendered = 0;
	}
}

void RenderSkip::NoteRenderCost(int ms)
{
	if (ms < 0 || ms > 1000)
		return;
	g_renderCostMs = (g_renderCostMs < 0) ? ms : (g_renderCostMs + ((ms - g_renderCostMs) >> 2));
}

void RenderSkip::Reset()
{
	g_renderCostMs = -1;
	g_windowRendered = 0;
	g_windowSkipped = 0;
	g_engagedAtFrame = 0;
	g_active = false;
	g_consecutiveSkips = 0;
	g_engageRun = 0;
	g_releaseRun = 0;
	g_skipped = 0;
	g_rendered = 0;
	g_lastSummaryTick = 0;
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

	Summarise();

	const int budget = FrameBudgetMs();
	const int average = AverageProcessMs();
	const int behind = FramesBehindPeers();
	const int maxAhead = Game::Network::MaxAhead;

	// Two conditions, deliberately. A slow client in a game where nobody is
	// waiting should keep drawing - dropping frames there costs the player
	// picture quality and buys the match nothing. Only when this client is the
	// one the others are blocked on does trading render for simulation pay for
	// itself.
	//
	// Each gets a hysteresis band: starting needs the full threshold, continuing
	// only needs to stay above a lower one.
	const int engageBehind = maxAhead / 2 < 2 ? 2 : maxAhead / 2;
	// Release well below the engage point, not just under it. The throttle
	// itself lowers the process figure, so a release threshold close to the
	// budget guarantees oscillation: engage, cost drops, release, cost rises.
	const int releaseBudget = budget / 2;

	// The "is anyone waiting on us" test has been removed, because the figure it
	// used cannot answer that question.
	//
	// FramesBehindPeers subtracts our frame from the slowest peer's LAST REPORTED
	// frame, and those reports only arrive with FRAMEINFO every FrameSendRate
	// frames, carrying a frame stamped up to MaxAhead ahead. So the figure sits
	// structurally near -MaxAhead even when every client is perfectly healthy:
	// measured at level 9 it ran -13 to -35 against an engage threshold of +18,
	// which is unreachable. That is why this throttle has never once engaged in
	// any recorded game - it was gated on reporting lag, not on who is slow.
	//
	// Process time alone is the honest signal, and it is self-limiting: it only
	// exceeds the budget when this client genuinely cannot render and simulate a
	// frame inside the target frame time. Under lockstep the slowest simulator
	// sets the rate for everyone, so cutting our own cost is the one thing that
	// can raise it - and skipping a render is sync-neutral either way.
	// What a fully-rendered frame would cost: the measured average plus the
	// render we are currently not paying for.
	int projected = average;
	if (g_renderCostMs > 0)
	{
		const int total = g_windowRendered + g_windowSkipped;
		if (total > 0 && g_windowSkipped > 0)
			projected = average + (g_renderCostMs * g_windowSkipped) / total;
	}

	if (!g_active)
		g_engageRun = (average >= budget) ? g_engageRun + 1 : 0;
	else
		g_releaseRun = (projected < releaseBudget) ? g_releaseRun + 1 : 0;

	// Re-probe rather than latch: if we have been throttling for a long time,
	// drop it and let the next few frames say whether it is still needed.
	if (g_active && (int)Unsorted::CurrentFrame - g_engagedAtFrame > MaxEngagedFrames)
	{
		Debug::Log("[Audit] render re-probe frame=%d after %d frames engaged (process=%dms render=%dms projected=%dms budget=%dms)\n"
			, (int)Unsorted::CurrentFrame, (int)Unsorted::CurrentFrame - g_engagedAtFrame
			, average, g_renderCostMs, projected, budget);
		g_releaseRun = ReleaseFrames;
	}

	const bool throttle = g_active ? (g_releaseRun < ReleaseFrames) : (g_engageRun >= EngageFrames);

	if (!throttle)
	{
		if (g_active)
		{
			g_active = false;
			g_engageRun = 0;
			Debug::Log("[RenderSkip] released at frame %d (process=%dms budget=%dms behind=%d)\n",
				(int)Unsorted::CurrentFrame, average, budget, behind);
		}
		g_consecutiveSkips = 0;
		++g_rendered;
		++g_windowRendered;
		return true;
	}

	if (!g_active)
	{
		g_active = true;
		g_releaseRun = 0;
		g_engagedAtFrame = (int)Unsorted::CurrentFrame;
		Debug::Log("[RenderSkip] engaged at frame %d (process=%dms budget=%dms behind=%d/%d max=%d)\n",
			(int)Unsorted::CurrentFrame, average, budget, behind, engageBehind, MaxConsecutive);
	}

	// Floor on the visible frame rate: after MaxConsecutive drops, draw one.
	if (g_consecutiveSkips >= MaxConsecutive)
	{
		g_consecutiveSkips = 0;
		++g_rendered;
		return true;
	}

	++g_consecutiveSkips;
	++g_skipped;
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
			const long long us = ((after.QuadPart - before.QuadPart) * 1000LL) / freq.QuadPart;
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
