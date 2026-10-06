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
#include <Replay/ReplaySeek.h>

bool RenderSkip::Enabled = true;
int  RenderSkip::MinProcessMs = 20;
int  RenderSkip::RenderSharePercent = 25;

namespace
{
	// Draw two frames, then drop one - two frames in three.
	const int DrawRun = 2;

	// Engage fast, release slow; the inputs are coarse, so a single threshold flaps.
	const int EngageFrames = 2;

	// Two seconds of slack before releasing.
	const int ReleaseFrames = 120;

	// Render cost on its own, to project what an unthrottled frame would cost.
	int   g_renderCostUs = -1;   // EWMA, alpha = 1/4

	// Rendered and skipped frames in the engine's current 128-frame ProcessingTicks window.
	int   g_windowRendered = 0;
	int   g_windowSkipped = 0;
	int   g_lastWindowFrames = 0;

	// Only trust the average once the window holds this many frames.
	const int MinWindowFrames = 32;
	int   g_engagedAtFrame = 0;

	// Limit on one engagement, so the throttle re-checks instead of latching.
	const int MaxEngagedFrames = 3600;

	bool  g_active = false;
	int   g_drawRun = 0;
	int   g_engageRun = 0;
	int   g_releaseRun = 0;

	DWORD     g_engagedTick = 0;
	int       g_engagedSamples = 0;
	long long g_engagedProcessMsSum = 0;
	long long g_engagedFullUsSum = 0;
	int       g_engagedSkipped = 0;
	int       g_matchEngagedFrames = 0;
	int       g_matchSkipped = 0;

	void LogEngagementSummary(int frame)
	{
		const int frames = frame - g_engagedAtFrame;
		g_matchEngagedFrames += frames;

		const DWORD elapsedMs = GetTickCount() - g_engagedTick;
		const int realFps10 = elapsedMs > 0 ? (int)((long long)frames * 10000 / elapsedMs) : 0;
		const int matchPct = frame > 0 ? (int)((long long)g_matchEngagedFrames * 100 / frame) : 0;

		if (g_engagedSamples <= 0)
		{
			Debug::Log("[RenderSkip] summary frames=%d-%d (%d) | no samples | real=%d.%dfps skipped=%d | match engaged=%d%% skipped=%d\n"
				, g_engagedAtFrame, frame, frames, realFps10 / 10, realFps10 % 10, g_engagedSkipped, matchPct, g_matchSkipped);
			return;
		}

		// Tenths of a millisecond throughout, so a saving under 1ms is visible.
		const int process10 = (int)(g_engagedProcessMsSum * 10 / g_engagedSamples);
		const int full10 = (int)(g_engagedFullUsSum / g_engagedSamples / 100);
		const int saved10 = full10 - process10;
		const int limitSkip = process10 > 0 ? 10000 / process10 : 0;
		const int limitFull = full10 > 0 ? 10000 / full10 : 0;

		Debug::Log("[RenderSkip] summary frames=%d-%d (%d) | process=%d.%dms full=%d.%dms saved=%d.%dms/frame | limit %d->%dfps real=%d.%dfps | skipped=%d | match engaged=%d%% skipped=%d\n"
			, g_engagedAtFrame, frame, frames
			, process10 / 10, process10 % 10, full10 / 10, full10 % 10
			, saved10 / 10, saved10 % 10
			, limitFull, limitSkip, realFps10 / 10, realFps10 % 10
			, g_engagedSkipped, matchPct, g_matchSkipped);
	}

	int FrameBudgetMs()
	{
		const int fps = Game::Network::RequestedFPS;
		const int derived = fps > 0 ? (1000 / fps) : 16;
		return derived < RenderSkip::MinProcessMs ? RenderSkip::MinProcessMs : derived;
	}

	// Average frame cost in the current window. Unreliable for the first few frames.
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
	g_engagedTick = 0;
	g_engagedSamples = 0;
	g_engagedProcessMsSum = 0;
	g_engagedFullUsSum = 0;
	g_engagedSkipped = 0;
	g_matchEngagedFrames = 0;
	g_matchSkipped = 0;
}

bool RenderSkip::ThrottleActive()
{
	return Enabled && g_active && !SessionClass::IsSingleplayer();
}

bool RenderSkip::ShouldRenderThisFrame()
{
	// Multiplayer only. Single player never runs InitNetwork, so the settings
	// would otherwise stay at their defaults.
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

	// Engages when the average frame reaches the budget and rendering is at least
	// RenderSharePercent of it. Releases when the projected unthrottled cost falls
	// below three quarters of the budget.
	// engagement lasted until the forced re-probe and re-engaged straight after.
	const int releaseBudget = budget * 3 / 4;

	// What a frame would cost if every frame were rendered.
	int projectedUs = average * 1000;
	if (g_renderCostUs > 0)
	{
		const int total = g_windowRendered + g_windowSkipped;
		if (total > 0 && g_windowSkipped > 0)
			projectedUs += static_cast<int>(static_cast<long long>(g_renderCostUs) * g_windowSkipped / total);
	}
	const int projected = projectedUs / 1000;

	// Only intervene when rendering is a real share of the frame. No sample yet
	// does not block.
	const bool renderIsTheCost = g_renderCostUs < 0
		|| static_cast<long long>(g_renderCostUs) * 100 >= static_cast<long long>(RenderSharePercent) * average * 1000;

	// Only count toward a change once the window has enough frames.
	if (windowFrames >= MinWindowFrames)
	{
		if (!g_active)
			g_engageRun = (average >= budget && renderIsTheCost) ? g_engageRun + 1 : 0;
		else
			g_releaseRun = (projectedUs < releaseBudget * 1000) ? g_releaseRun + 1 : 0;
	}

	// Drop the throttle after a long engagement and re-check.
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
			LogEngagementSummary((int)Unsorted::CurrentFrame);
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
		g_engagedTick = GetTickCount();
		g_engagedSamples = 0;
		g_engagedProcessMsSum = 0;
		g_engagedFullUsSum = 0;
		g_engagedSkipped = 0;
		Debug::Log("[RenderSkip] engaged at frame %d (process=%dms render=%dus share=%d%% budget=%dms)\n",
			(int)Unsorted::CurrentFrame, average, g_renderCostUs, RenderSharePercent, budget);
	}

	if (windowFrames >= MinWindowFrames)
	{
		++g_engagedSamples;
		g_engagedProcessMsSum += average;
		g_engagedFullUsSum += projectedUs;
	}

	// DrawRun frames drawn, then one dropped.
	if (g_drawRun < DrawRun)
	{
		++g_drawRun;
		// Counted, so the projection doesn't treat this frame as skipped.
		++g_windowRendered;
		return true;
	}

	g_drawRun = 0;
	++g_windowSkipped;
	++g_engagedSkipped;
	++g_matchSkipped;
	return false;
}

// Replaces Main_Loop's render call. Also thins out rendering during a replay seek.
DEFINE_HOOK(0x55D8F2, MainLoop_Render_RenderSkip, 0x5)
{
	enum { Resume = 0x55D8F7 };

	GET(GScreenClass*, screen, ECX);

	if (ReplaySystem::Seek::IsSeeking())
	{
		// Seeking draws only every so often.
		const bool skip = ReplaySystem::Seek::ShouldSkipRenderThisFrame();
		ReplaySystem::Seek::CountRenderedFrame();

		if (!skip)
			screen->Render();

		return Resume;
	}

	if (RenderSkip::ShouldRenderThisFrame())
	{
		// QueryPerformanceCounter: GetTickCount is too coarse for a render.
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

// Sync_Delay's extra Input/Tactical::AI/Render block, used when the frame has
// slack. Skipped entirely while throttling, otherwise it runs more often and
// scrolling speeds up while drawing less.
DEFINE_HOOK(0x55E23F, SyncDelay_Opportunistic_RenderSkip, 0x8)
{
	enum { SkipBlock = 0x55E278 };

	return RenderSkip::ThrottleActive() ? SkipBlock : 0;
}
