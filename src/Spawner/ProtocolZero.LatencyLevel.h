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

	//!< false restores the vanilla one-way ratchet, for A/B testing.
	static bool Bidirectional;

	static void Apply(LatencyLevelEnum newLatencyLevel);
	static void __forceinline Apply(uint8_t newLatencyLevel)
	{
		Apply(static_cast<LatencyLevelEnum>(newLatencyLevel));
	}

	static void Reset();

	static int GetMaxAhead(LatencyLevelEnum latencyLevel);
	static const wchar_t* GetLatencyMessage(LatencyLevelEnum latencyLevel);
	static LatencyLevelEnum FromResponseTime(uint8_t rspTime);

	/**
	 *  Picks a level from measured round-trip time plus whether the runway we
	 *  already chose turned out to be long enough.
	 *
	 *  `rspTime` is IPXManagerClass::ResponseTime(), which is in SystemTimer ticks
	 *  -- i.e. already in frames -- so it compares directly against MaxAhead.
	 *
	 *  `needRetransmitCover` says the frame-aware gate blocked on missing commands
	 *  since we last looked, which means a retransmit did not land inside the
	 *  runway. That is the only direct evidence available that MaxAhead is too
	 *  short; round-trip time alone never shows it.
	 */
	static LatencyLevelEnum FromConditions(uint8_t rspTime, bool needRetransmitCover);
};
