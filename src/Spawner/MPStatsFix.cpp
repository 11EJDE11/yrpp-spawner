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
*  MPStatsFix - correct the command-count stall counter in Wait_For_Players.
*
*  The engine indexes this counter with the wrong peer, often -1, which writes
*  into ProcessingFrames and corrupts the frame rate negotiation.
*/

#include <Helpers/Macro.h>
#include <IPXManagerClass.h>
#include <SessionClass.h>
#include "FrameGate.h"
#include <Unsorted.h>

// Replaces the mis-indexed CommandCoundStalls increment with the peer that is
// actually behind. Skipped if no peer is behind.
DEFINE_HOOK(0x6497DC, WaitForPlayers_CommandStallStat_Fix, 0x7)
{
	enum { Continue = 0x6497E3 };

	int nconn = static_cast<int>(IPXManagerClass::Instance.NumConnections);
	if (nconn > FrameGate::MaxPeers)
		nconn = FrameGate::MaxPeers;

	const TheirSync* their = FrameGate::Peers();

	// Prefer a peer that is behind and not covered by FrameGate; otherwise the
	// first peer behind.
	int culprit = -1;
	int fallback = -1;
	for (int i = 0; i < nconn; ++i)
	{
		if (static_cast<unsigned int>(their[i].CommandsReceived) >= static_cast<unsigned int>(their[i].CommandsSent))
			continue;

		if (fallback < 0)
			fallback = i;

		if (FrameGate::GetSafeThrough(i) < (int)Unsorted::CurrentFrame)
		{
			culprit = i;
			break;
		}
	}

	if (culprit < 0)
		culprit = fallback;

	if (culprit >= 0)
		++SessionClass::Instance.MPStats[culprit].CommandCoundStalls;

	return Continue;
}
