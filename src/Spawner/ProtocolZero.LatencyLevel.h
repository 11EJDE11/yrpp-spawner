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

#pragma once
#include <stdint.h>

enum class LatencyLevelEnum : uint8_t
{
	LATENCY_LEVEL_INITIAL = 0,

	LATENCY_LEVEL_1 = 1,
	LATENCY_LEVEL_2 = 2,
	LATENCY_LEVEL_3 = 3,
	LATENCY_LEVEL_4 = 4,
	LATENCY_LEVEL_5 = 5,
	LATENCY_LEVEL_6 = 6,
	LATENCY_LEVEL_7 = 7,
	LATENCY_LEVEL_8 = 8,
	LATENCY_LEVEL_9 = 9,

	LATENCY_LEVEL_MAX = LATENCY_LEVEL_9,
	LATENCY_SIZE = 1 + LATENCY_LEVEL_MAX
};
class LatencyLevel
{
public:
	static LatencyLevelEnum CurentLatencyLevel;
	static uint8_t NewFrameSendRate;

	// The MaxAhead this client wants in force. The periodic timing event carries
	// it, so a descent actually reaches the engine instead of being overwritten
	// by the next event re-broadcasting the current value.
	static int TargetMaxAhead;

	// Vanilla only ever climbs, so one bad stretch pins a whole match at the
	// worst rung. This allows stepping back down, on the pattern OpenTS settled
	// on (nettiming.cpp, BalancedTimingPolicy::Evaluate): worsening is immediate,
	// improvement must clear a headroom margin, a cooldown and several
	// consecutive agreeing evaluations, and then descends one rung at a time.
	//
	// Every input is taken from the event stream and every deadline from the
	// game frame, never from local counters or wall clock - each client must
	// reach the identical decision on the identical frame or they diverge.
	static bool AllowDescent;

	static const int EvaluationIntervalFrames = 256;
	static const int ChangeCooldownFrames = 256;
	static const int GoodEvaluationsRequired = 3;
	// The improvement has to survive inflating the measurement. Measured at 5/4
	// this cost roughly a whole rung of delay per step, because the engine's
	// Avg_Response_Time unwinds over minutes - it is a 256-sample mean fed only
	// by acknowledged reliable packets, which are sparse, and vanilla's periodic
	// Reset_Response_Time only runs for GAME_INTERNET so it never fires here.
	// 50% headroom, not 12.5%. A descent must be justified by a measurement
	// inflated by half before it is allowed, because the delivery-delay
	// estimator now tracks a step change in about ten acknowledgements - it is
	// far more responsive, and correspondingly noisier, than the 256-sample mean
	// this gate was originally tuned against. At 12.5% a third of all level
	// changes were reversals.
	static const int HeadroomNumerator = 3;
	static const int HeadroomDenominator = 2;

	// A raise is evidence the link is bad, so it blocks the next descent for
	// longer than an ordinary change does.
	static const int RaiseCooldownFrames = 1024;

	// A descent undone within this many frames counts as a failed attempt.
	static const int ReversalWindowFrames = 2048;
	// ...and that level is then refused for this long, so the same rung is not
	// retried on the same evidence that just failed.
	static const int FlapCooldownFrames = 4096;

	// Descent steps one rung per evaluation, and any rung is legal. FrameSendRate
	// follows the level in both directions, so every rung's MaxAhead is an exact
	// multiple of its own rate, and the hook at 0x4C8033 opens the rescheduling
	// window on a decrease too, so commands already queued on the old cadence
	// are moved rather than dropped.

	// Called once per timing report with the worst level and worst response time
	// any player has reported, and the house slots that reported them (logging
	// only). Raises immediately, descends only on the gates.
	static void Update(LatencyLevelEnum desired, int worstResponseTime,
		int worstRttSlot, int worstLevelSlot, int eventFrame);
	static void ResetDescent();

	static void Apply(LatencyLevelEnum newLatencyLevel, int eventFrame);
	static void __forceinline Apply(uint8_t newLatencyLevel, int eventFrame)
	{
		Apply(static_cast<LatencyLevelEnum>(newLatencyLevel), eventFrame);
	}

	static void Commit(LatencyLevelEnum newLatencyLevel, int eventFrame);
	static int GetMaxAhead(LatencyLevelEnum latencyLevel);
	static const wchar_t* GetLatencyMessage(LatencyLevelEnum latencyLevel);
	static LatencyLevelEnum FromResponseTime(uint8_t rspTime);
};
