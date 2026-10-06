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

/**
*  FrameGate - frame-aware advance gate.
*
*  The engine waits until every peer's received command count matches its sent
*  count, so one lost packet stalls everyone even when the missing commands
*  are for a later frame. This allows the frame to run once every peer's
*  commands up to that frame are in hand. A packet stamped F carries the
*  cumulative count from before its own commands, so frames through F - 1 are
*  safe once that count is reached. Falls back to the engine's test if
*  connections change.
*/

#pragma once

#include <TheirSync.h>

class FrameGate
{
public:
	static const int MaxPeers = sizeof(TheirSync::Array) / sizeof(TheirSync);

	static bool Enabled;

	static TheirSync* Peers()
	{
		return TheirSync::Array;
	}

	static void Reset();

	// Diagnostic snapshot; never used to change the gate.
	static int GetSafeThrough(int peer);

	// Replaces the engine's command-count loop. On false, *gapIndex is the first blocking peer.
	static bool AllCommandsSatisfied(TheirSync* peers, int* gapIndex);

	// Records the watermark for one received data/framesync packet.
	// theirEntry = &their[index]; ev = the packet header.
	static void OnReceive(unsigned int theirEntry, const unsigned char* ev);
};
