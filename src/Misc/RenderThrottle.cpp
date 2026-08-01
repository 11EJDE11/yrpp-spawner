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

#include "RenderThrottle.h"

#include <Spawner/SendClock.h>

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

#include <Helpers/CompileTime.h>
#include <Unsorted.h>
#include <FPSCounter.h>
#include <SessionClass.h>
#include <YRMath.h>

#include <Windows.h>

bool RenderThrottle::Enable = false;
bool RenderThrottle::ObserveOnly = false;
int  RenderThrottle::EngageFrames = 40;
int  RenderThrottle::ReleaseFrames = 8;
int  RenderThrottle::MaxConsecutiveSkips = 30;
int  RenderThrottle::MinFrameRate = 25;
int  RenderThrottle::MaxFrameRate = 300;

bool RenderThrottle::Engaged = false;
int  RenderThrottle::Shortfall = 0;
int  RenderThrottle::ConsecutiveSkips = 0;
int  RenderThrottle::BaselineFrameRate = 0;
int  RenderThrottle::TargetFrameRate = 0;
bool RenderThrottle::Bottleneck = false;

long long RenderThrottle::SyncDelayStart = 0;
long long RenderThrottle::SyncWaitTicks = 0;
long long RenderThrottle::WindowStart = 0;
int       RenderThrottle::WindowStartFrame = 0;

//!< DesiredFrameRate. Main_Loop paces the logic frame rate against this.
#define DesiredFrameRate Make_Global<int>(0xA8B558u)

namespace
{
	long long QpcNow()
	{
		LARGE_INTEGER value;
		QueryPerformanceCounter(&value);
		return value.QuadPart;
	}

	long long QpcFrequency()
	{
		static long long frequency = []() -> long long
		{
			LARGE_INTEGER value;
			QueryPerformanceFrequency(&value);
			return value.QuadPart ? value.QuadPart : 1;
		}();

		return frequency;
	}
}

bool RenderThrottle::IsEngaged()
{
	return Engaged && !ObserveOnly;
}

bool RenderThrottle::IsBottleneck()
{
	return Bottleneck;
}

void RenderThrottle::Reset()
{
	if (Engaged && BaselineFrameRate > 0)
		DesiredFrameRate = BaselineFrameRate;

	Engaged = false;
	Shortfall = 0;
	ConsecutiveSkips = 0;
	TargetFrameRate = 0;
	SyncWaitTicks = 0;
	WindowStart = 0;
	WindowStartFrame = 0;
}

bool RenderThrottle::Applicable()
{
	// SendClock drives its drift from the same "am I the one holding everyone up"
	// signal, so keep measuring for it even when the render action itself is off.
	if (!Enable && !SendClock::Enable)
		return false;

	// Only a live multiplayer match can be held up by us.
	if (!SessionClass::IsMultiplayer())
		return false;

	// An observer is not the client anyone is waiting on.
	if (Game::ObserverMode)
		return false;

	// Same warm-up guard the engine's own frame accounting needs: do not react
	// to the load spike at the start of a match.
	if (FPSCounter::TotalFramesElapsed < 1800)
		return false;

	return true;
}

void RenderThrottle::BeginSyncDelay()
{
	SyncDelayStart = QpcNow();
}

void RenderThrottle::EndSyncDelay()
{
	if (SyncDelayStart != 0)
	{
		SyncWaitTicks += QpcNow() - SyncDelayStart;
		SyncDelayStart = 0;
	}
}

/**
*  Re-evaluated once a second. Everything here is an estimate: without a server
*  frame to compare against we infer the rate shortfall from how far under the
*  configured frame rate we ran while nobody was making us wait.
*/
void RenderThrottle::UpdateWindow()
{
	const long long now = QpcNow();

	if (WindowStart == 0)
	{
		WindowStart = now;
		WindowStartFrame = Unsorted::CurrentFrame;
		SyncWaitTicks = 0;
		return;
	}

	const long long frequency = QpcFrequency();
	const long long elapsed = now - WindowStart;

	if (elapsed < frequency)
		return;

	const int framesRun = Unsorted::CurrentFrame - WindowStartFrame;
	const double seconds = static_cast<double>(elapsed) / static_cast<double>(frequency);
	const double waitSeconds = static_cast<double>(SyncWaitTicks) / static_cast<double>(frequency);

	WindowStart = now;
	WindowStartFrame = Unsorted::CurrentFrame;
	SyncWaitTicks = 0;

	// Capture the game's own pacing target while we are not overriding it.
	if (!Engaged && DesiredFrameRate > 0)
		BaselineFrameRate = DesiredFrameRate;

	if (BaselineFrameRate <= 0 || framesRun < 0)
		return;

	// If we barely blocked waiting for peers, nobody was holding us up, so any
	// shortfall against the target rate is ours and everyone else is paying for
	// it. 5% of the window is a generous "barely".
	const bool bottleneck = waitSeconds < (seconds * 0.05);
	Bottleneck = bottleneck;

	const int expected = static_cast<int>(BaselineFrameRate * seconds);
	const int deficit = expected - framesRun;

	if (bottleneck && deficit > 0)
		Shortfall += deficit;
	else if (deficit < 0)
		Shortfall += deficit; // running at or above baseline again: drain
	else if (!bottleneck)
		Shortfall = 0;        // somebody else is the slow one, not our problem

	Shortfall = Math::clamp(Shortfall, 0, 600);

	// One line per second while we are the slow client, so the estimate can be
	// validated against real matches before anyone arms the action.
	if (bottleneck || Engaged)
	{
		Debug::Log("[RenderThrottle] bottleneck=%d ran=%d/%d fps wait=%dms shortfall=%d engaged=%d%s\n"
			, bottleneck ? 1 : 0
			, framesRun
			, BaselineFrameRate
			, static_cast<int>(waitSeconds * 1000.0)
			, Shortfall
			, Engaged ? 1 : 0
			, ObserveOnly ? " (observe-only)" : "");
	}

	// Everything above is measurement, which SendClock also consumes. Everything
	// below acts on it, and only the render throttle proper may do that.
	if (!Enable)
		return;

	if (!Engaged && Shortfall >= EngageFrames)
	{
		Engaged = true;

		Debug::Log("[RenderThrottle] engaging, shortfall=%d, baseline=%d fps\n"
			, Shortfall, BaselineFrameRate);
	}
	else if (Engaged && Shortfall <= ReleaseFrames)
	{
		Debug::Log("[RenderThrottle] releasing, shortfall=%d\n", Shortfall);

		Engaged = false;
		ConsecutiveSkips = 0;
		TargetFrameRate = 0;

		if (!ObserveOnly)
			DesiredFrameRate = BaselineFrameRate;

		return;
	}

	if (!Engaged || ObserveOnly)
		return;

	// Same shape of controller Ramboplay drives from its server frame lead:
	// fast attack, smoothed decay back out.
	double target = Shortfall * 1.5 + BaselineFrameRate;
	target = Math::clamp(target, static_cast<double>(MinFrameRate), static_cast<double>(MaxFrameRate));

	// Decay against our own last target, not against DesiredFrameRate. A TIMING
	// event may have reset that global to the master's RequestedFPS since we last
	// wrote it, and feeding that back in would drag the controller down towards
	// the baseline every time the network layer republished timing.
	if (Shortfall < EngageFrames && TargetFrameRate > 0)
		target = target * 0.28 + TargetFrameRate * 0.72;

	TargetFrameRate = static_cast<int>(target);
	DesiredFrameRate = TargetFrameRate;
}

bool RenderThrottle::ShouldSkipTactical()
{
	if (!Applicable())
	{
		if (Engaged)
			Reset();

		return false;
	}

	UpdateWindow();

	if (!Engaged || ObserveOnly)
	{
		ConsecutiveSkips = 0;
		return false;
	}

	// Re-assert every frame. The controller only recomputes once a second, but
	// EventClass::Execute's TIMING handler overwrites DesiredFrameRate at
	// 0x4C807D whenever the master republishes timing -- which with ProtocolZero
	// happens on every ladder change, since LatencyLevel sets PreCalcFrameRate.
	// Without this the boost silently disappears for the rest of the window.
	if (TargetFrameRate > 0 && DesiredFrameRate != TargetFrameRate)
		DesiredFrameRate = TargetFrameRate;

	// Safety valve: never let the view stay frozen indefinitely, however wrong
	// the shortfall estimate turns out to be.
	if (ConsecutiveSkips >= MaxConsecutiveSkips)
	{
		ConsecutiveSkips = 0;
		return false;
	}

	++ConsecutiveSkips;
	return true;
}

// ============================================================
// Hooks
// ============================================================

/**
*  Main_Loop - 0x55DE9A, `call Sync_Delay`.
*
*  Wrapping the call is deliberate. The obvious alternative -- hook 0x55DE9F, the
*  instruction after it -- would steal `e8 cc 7d 1c 00`, a rel32 call, and there
*  is no evidence Syringe relocates relative displacements when it replays stolen
*  bytes. Returning an explicit address skips the stolen bytes entirely, so we
*  issue the call ourselves and land on 0x55DE9F with the timer closed.
*
*  Sync_Delay takes no arguments and cleans nothing, so a plain void() call is
*  correct for either cdecl or stdcall.
*/
DEFINE_HOOK(0x55DE9A, MainLoop_RenderThrottle_SyncDelay, 0x5)
{
	RenderThrottle::BeginSyncDelay();

	reinterpret_cast<void(*)()>(0x55E160u)();

	RenderThrottle::EndSyncDelay();

	return 0x55DE9F;
}

/**
*  GScreenClass::Render - 0x4F44C2, the call to
*  IonStormClass::ChronoScreenEffect::Is_Active2 that decides whether the
*  tactical view and sidebar are drawn at all this frame.
*
*  0x4F451B is the engine's own "skip the map this frame" label.
*/
DEFINE_HOOK(0x4F44C2, GScreenClass_Render_RenderThrottle, 0x5)
{
	if (RenderThrottle::ShouldSkipTactical())
		return 0x4F451B;

	// Is_Active2 is just `Status != 0`; inline it so the `test al, al` at
	// 0x4F44C7 sees exactly what it would have.
	R->EAX(Make_Global<int>(0xA9FAB0) != 0 ? 1 : 0);

	return 0x4F44C7;
}

/**
*  FootClass_Voxel_Shadow - 0x4DB0D0. 0x4DB19A is its `retn 2Ch`; jumping there
*  without having run the prologue leaves the stack balanced.
*/
DEFINE_HOOK(0x4DB0D0, FootClass_VoxelShadow_RenderThrottle, 0x6)
{
	return RenderThrottle::IsEngaged()
		? 0x4DB19A
		: 0;
}
