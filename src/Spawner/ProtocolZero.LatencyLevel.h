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

	// The MaxAhead this client wants in force, carried by the periodic timing event.
	static int TargetMaxAhead;

	// Vanilla only ever raises the level. This also lowers it: raises are
	// immediate, a descent needs headroom, a cooldown and several agreeing
	// evaluations, and goes one rung at a time. Everything is driven by the event
	// stream and game frame so all clients decide identically.
	static bool AllowDescent;

	static const int EvaluationIntervalFrames = 256;
	static const int ChangeCooldownFrames = 256;
	static const int GoodEvaluationsRequired = 3;
	// Headroom: the improvement must still hold with the measurement inflated by half.
	static const int HeadroomNumerator = 3;
	static const int HeadroomDenominator = 2;

	// A raise blocks the next descent for longer than other changes.
	static const int RaiseCooldownFrames = 1024;

	// A descent undone within this many frames counts as a failed attempt.
	static const int ReversalWindowFrames = 2048;
	// ...and that level is then refused for this long.
	static const int FlapCooldownFrames = 4096;


	// Called with the worst reported level and response time. The house slots are
	// for logging only.
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
