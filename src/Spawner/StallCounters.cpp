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

#include "StallCounters.h"

#include <Utilities/Macro.h>

bool StallCounters::Enable = true;
int  StallCounters::Suppressed = 0;

namespace
{
	//!< MPStats[] runs 0..7; anything else lands outside the array.
	inline bool IndexIsValid(int index)
	{
		return index >= 0 && index < 8;
	}
}

void StallCounters::Reset()
{
	Suppressed = 0;
}

/**
 *  MPStats[ebp].FrameSyncStalls
 *
 *  0x64971C  ff 04 c5 c8 b5 a8 00   inc dword [eax*8 + 0xA8B5C8]
 *
 *  At ebp == -1 this is ProcessingTicks (0xA8B560). Seven position-independent
 *  bytes, so returning the following address simply skips the increment.
 */
DEFINE_HOOK(0x64971C, WaitForPlayers_FrameSyncStall_IndexGuard, 0x7)
{
	if (StallCounters::Enable && !IndexIsValid(R->EBP<int>()))
	{
		++StallCounters::Suppressed;
		return 0x649723;
	}

	return 0;
}

/**
 *  MPStats[ebp].CommandCoundStalls
 *
 *  0x6497DC  ff 04 c5 cc b5 a8 00   inc dword [eax*8 + 0xA8B5CC]
 *
 *  At ebp == -1 this is ProcessingFrames (0xA8B564), and ebp is -1 for exactly
 *  the packet-loss stall.
 */
DEFINE_HOOK(0x6497DC, WaitForPlayers_CommandStall_IndexGuard, 0x7)
{
	if (StallCounters::Enable && !IndexIsValid(R->EBP<int>()))
	{
		++StallCounters::Suppressed;
		return 0x6497E3;
	}

	return 0;
}
