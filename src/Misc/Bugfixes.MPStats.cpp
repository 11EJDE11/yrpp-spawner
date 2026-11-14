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

#include <Utilities/Macro.h>
#include <IPXManagerClass.h>
#include <SessionClass.h>

#include <algorithm>

// While Wait_For_Players stalls, it counts the stall against a peer: a frame-sync stall against the
// peer furthest behind in frames, a command-count stall against the first peer whose commands have
// not all arrived. The command-count one is indexed with the frame-sync peer instead, which is -1
// whenever frames are not what is holding the game up - the usual case when packets are lost. So
// ++MPStats[-1].CommandCoundStalls lands on SessionClass::ProcessFrames (+0x32C), the frame count
// the game measures its own frame rate with for the timing it reports to the others, and every
// stall shown in the MP debug stats goes to the wrong player or nowhere.
//
// The register holding the command peer (ESI) is reused by the MP debug drawing just before, so it
// is found again the way the game found it: the first connection that has received fewer commands
// than were sent to it.
namespace
{
	// Wait_For_Players' `their` argument: per connection, the frame and command counts it last heard.
	struct TheirSync
	{
		int Frame;
		int CommandsSent;
		int CommandsReceived;
		int ResponseTime;
		int RouterResponseTime;
		int LastHeardTime;
	};
	static_assert(sizeof(TheirSync) == 0x18);

	constexpr int MaxConnections = 7;
	TheirSync* const Their = reinterpret_cast<TheirSync*>(0xAFA358);
}

// `inc [eax*8 + 0xA8B5CC]` = ++MPStats[frame peer].CommandCoundStalls
DEFINE_HOOK(0x6497DC, WaitForPlayers_CommandStallStat_Fix, 0x7)
{
	enum { Continue = 0x6497E3 };

	const int count = std::min(static_cast<int>(IPXManagerClass::Instance.NumConnections), MaxConnections);
	for (int i = 0; i < count; ++i)
	{
		if (Their[i].CommandsReceived < Their[i].CommandsSent)
		{
			++SessionClass::Instance.MPStats[i].CommandCoundStalls;
			break;
		}
	}

	return Continue;
}
