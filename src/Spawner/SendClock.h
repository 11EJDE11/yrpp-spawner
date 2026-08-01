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
 *  Decouples the clock we *submit* commands on from the clock we *execute* on.
 *
 *  The engine stamps outgoing commands `Frame + MaxAhead` using the local
 *  execution frame (Send_Packets 0x649D2D, Add_Compressed_Events 0x64B9BF,
 *  Send_FrameSync 0x64A04A). Peers derive `their[i].frame` from that stamp, so
 *  the whole lobby ends up bound to the slowest *executor* rather than the
 *  slowest *submitter*. A client that cannot render or simulate fast enough drags
 *  everyone down with it even though its packets are arriving on time.
 *
 *  Ramboplay avoids this by having a server gate on submission alone. The
 *  peer-to-peer equivalent is to add a drift to the stamp: report, and stamp,
 *  `Frame + Drift`, while continuing to execute at `Frame`. Peers then run up to
 *  Drift frames further ahead of us and stop waiting.
 *
 *  The cost lands entirely on the struggling client: its input is stamped further
 *  into the future relative to what is on its screen, so the game feels laggier
 *  for that player and unchanged for everyone else. That is the intended trade.
 *
 *  Determinism is unaffected -- every client still executes the same event at the
 *  same frame number; only the wall-clock moment we reach that frame moves.
 *
 *  Two rules keep it safe, and both are load-bearing:
 *
 *  1. `Frame + Drift` must never go backwards. Peers only ever *raise*
 *     `their[i].frame` (the guard at 0x64A3C7), so a lowered stamp would leave
 *     them believing we are further ahead than we are, and they would advance past
 *     frames we had not stamped commands for. That is a late arrival, i.e. a
 *     desync. Drift therefore only ever shrinks as fast as Frame grows.
 *
 *  2. Drift has to stay well inside the DoList limits. Running behind means
 *     peers' commands pile up unexecuted, and *both* send paths throttle on
 *     DoList occupancy -- Send_Packets caps at `8192 - DoList.Count` (0x649CD0)
 *     and Add_Compressed_Events hard-stops at 0x4000 (0x64BA01). A client that
 *     drifts too far stops sending its own commands and stalls the entire lobby,
 *     which is worse than never having drifted at all. Overflow at 16384 is fatal.
 */
class SendClock
{
public:
	static bool Enable;

	//!< Hard ceiling on how far the send clock may run ahead of execution.
	static int MaxDrift;

	//!< Back off once DoList occupancy passes this. Well under the 8192 point at
	//!< which the engine starts refusing to pack our own commands.
	static int DoListLimit;

	//!< Frames the send clock is currently ahead of the execution clock.
	static int Drift;

	//!< Highest drift reached, and highest DoList occupancy seen while drifting.
	static int PeakDrift;
	static int PeakDoList;

	static void Reset();

	//!< Once per Main_Loop iteration.
	static void Tick();

	/**
	 *  The frame number to stamp outgoing commands and beacons with.
	 *  Returns the plain execution frame when disabled.
	 */
	static int SendFrame();
};
