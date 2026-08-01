/**
*  yrpp-spawner
*
*  Copyright(C) 2026-present CnCNet
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

#pragma once

/**
*  Local render throttling with simulation catch-up.
*
*  In lockstep the whole match advances at the speed of the slowest client's
*  Main_Loop, because every peer blocks in Sync_Delay waiting for that client's
*  events. A client that is render-bound therefore taxes everybody.
*
*  This decouples the two: when we detect that we are the client holding the
*  match up, we stop drawing the tactical view and raise DesiredFrameRate so
*  the freed CPU goes into simulation until we have caught back up. We pay for
*  our own slowness in visual frames instead of exporting it to the lobby.
*
*  Nothing here touches simulation state, event ordering, FrameSendRate or
*  MaxAhead, so it cannot itself cause a desync:
*
*    - The tactical skip reuses the exact path the engine already takes while a
*      chrono screen effect is playing (GScreenClass::Render, 0x4F44C2 ->
*      0x4F451B). Mouse, buttons, message list, tooltips and Blit_Display all
*      still run.
*    - The voxel shadow skip returns from FootClass_Voxel_Shadow before it has
*      touched the stack, which is stack-correct and draw-only.
*    - DesiredFrameRate is local pacing. The lockstep contract is FrameSendRate
*      and MaxAhead, which are left alone.
*
*  A note on what "behind" means here, because it is NOT the same quantity a
*  server-authoritative design uses.
*
*  Under peer lockstep you cannot fall an arbitrary number of frames behind:
*  MaxAhead clamps every client to within a few frames of every other, by
*  construction. There is no backlog to burn through. What actually happens is
*  that the slowest client's Main_Loop iterates less often, so it produces its
*  events later, so everyone else sits in Sync_Delay waiting for it.
*
*  So the goal is not "catch up on a backlog", it is "stop being the rate
*  limiter": raise our sustained frame rate until we are no longer the client
*  everybody is waiting on.
*
*  Shortfall below is therefore an accumulated *rate deficit* - how far under
*  the configured frame rate we have been running while nobody was making us
*  wait - not a count of frames we are behind. It acts as a debounce: at 60fps
*  baseline, running at 40fps takes about two seconds to cross the default
*  engage threshold. The signal is time spent in Sync_Delay, because a client
*  that never blocks there is by definition the one everyone else blocks on.
*/
class RenderThrottle
{
public:
	static bool Enable;

	//!< Measure and log, but never actually skip a frame or touch DesiredFrameRate.
	//!< Recommended for the first few games on a new build: it tells you whether the
	//!< bottleneck estimate fires when it should, at zero risk.
	static bool ObserveOnly;

	//!< Accumulated rate shortfall at which we start skipping the tactical view.
	static int EngageFrames;

	//!< Shortfall at which we stop again. Lower than EngageFrames, for hysteresis.
	static int ReleaseFrames;

	//!< Hard cap on consecutive skipped frames, so the view can never freeze
	//!< indefinitely if the estimate is wrong.
	static int MaxConsecutiveSkips;

	static int MinFrameRate;
	static int MaxFrameRate;

	//!< True while we are actively catching up.
	static bool IsEngaged();

	//!< True when we barely blocked in Sync_Delay over the last window, i.e. we
	//!< are the client everyone else is waiting on. Shared with SendClock, which
	//!< needs the same signal to decide whether buying peers headroom is warranted.
	static bool IsBottleneck();

	//!< Called on entry to Sync_Delay.
	static void BeginSyncDelay();

	//!< Called once Main_Loop's Sync_Delay call has returned.
	static void EndSyncDelay();

	//!< Called once per GScreenClass::Render. Returns true if the tactical view
	//!< and sidebar should be skipped this frame.
	static bool ShouldSkipTactical();

	//!< Drop back to the captured baseline frame rate and clear state.
	static void Reset();

private:
	static bool Engaged;
	static int  Shortfall;
	static int  ConsecutiveSkips;
	static int  BaselineFrameRate;

	//!< The rate we want the engine pacing at while engaged.
	//!<
	//!< Kept separately from DesiredFrameRate because that global is not ours
	//!< alone: 0xA8B558 is simultaneously DesiredFrameRate and
	//!< Game::Network::RequestedFPS, and EventClass::Execute's TIMING handler
	//!< writes it from the event payload at 0x4C807D every time the master
	//!< republishes timing. Reading our own boost back out of it would feed the
	//!< network layer's value into the controller, and setting it once a second
	//!< would leave the boost reverted for most of that second.
	static int  TargetFrameRate;

	static long long SyncDelayStart;
	static long long SyncWaitTicks;
	static long long WindowStart;
	static int       WindowStartFrame;

	static bool Bottleneck;

	static bool Applicable();
	static void UpdateWindow();
};
