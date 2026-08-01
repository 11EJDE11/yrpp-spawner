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
 *  Stops the engine's stall counters corrupting its frame-rate negotiation.
 *
 *  Wait_For_Players records a stall against the peer that caused it:
 *
 *      0x649714  lea edx, [ebp+ebp*2]      ; eax = ebp * 13
 *      0x649718  lea eax, [ebp+edx*4]
 *      0x64971C  inc [eax*8 + 0xA8B5C8]    ; MPStats[ebp].FrameSyncStalls
 *      0x6497DC  inc [eax*8 + 0xA8B5CC]    ; MPStats[ebp].CommandCoundStalls
 *
 *  MPStats has a 104-byte stride, so index -1 addresses 104 bytes below
 *  MPStats[0]:
 *
 *      0xA8B5C8 - 104 = 0xA8B560 = ProcessingTicks
 *      0xA8B5CC - 104 = 0xA8B564 = ProcessingFrames
 *
 *  ...and the engine sets `ebp = -1` at 0x64961F precisely when the wait is
 *  caused by the command-count condition and *not* by MaxAhead -- which is the
 *  ordinary packet-loss stall.
 *
 *  Those two globals are the engine's own measurement of how long a frame takes
 *  to process (Main_Loop accumulates them at 0x55DE13). They are published as
 *  EVENT_PROCESS_TIME and feed the master's frame-rate and MaxAhead negotiation.
 *  Inflating ProcessingFrames without inflating ProcessingTicks makes each frame
 *  look cheaper than it is, so the match negotiates a rate it cannot sustain.
 *
 *  The damage scales with loss, because Wait_For_Players spins many iterations
 *  per stall and every one of them increments.
 *
 *  The fix is simply to skip the increment when the index is out of range. The
 *  stall is then unattributed -- exactly as it is today -- but the timing
 *  measurement is left alone. No simulation state is involved: these counters are
 *  local statistics, and the negotiated timing they influence is distributed as a
 *  TIMING event that every client executes identically.
 */
class StallCounters
{
public:
	static bool Enable;

	//!< Increments suppressed, i.e. corrupting writes prevented.
	static int Suppressed;

	static void Reset();
};
