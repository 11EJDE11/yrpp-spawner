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
*  PacketRedundancy - outbound redundancy for reliable command packets.
*
*  Reliable command packets and their acknowledgements are sent twice, so a
*  single loss doesn't wait for a retransmit. The engine ignores the duplicate.
*/

#pragma once
#include <stddef.h>

class ConnectionClass;

class PacketRedundancy
{
public:
	static const int MaxPeers = 8;

	static bool Enabled;
	static int  Copies;
	// When true, only duplicate to peers where recent packet loss is observed.
	static bool Adaptive;
	// When true, acknowledgements are duplicated as well as commands.
	static bool Acks;

	static void Reset();
	static int ClampCopies(int copies);

	// How many times to send this datagram (1 = no duplication). buf is the
	// on-wire game data.
	static int CopiesFor(const char* buf, size_t len, int peer);

	// Loss signal for adaptive mode.
	static void NoteResend(const ConnectionClass* connection);

	// Feeds one received unreliable packet id; a gap raises that peer's loss gauge.
	static void NoteInboundPacket(const ConnectionClass* connection, int packetId);
	static int LossGauge(int peer);
	// Duplicate datagrams sent to this peer.
	static int Duplicates(int peer);

	// Logs failed duplicate sends at most once per second.
	static void NoteExtraSend(int sendResult);

	// Extra datagrams sent, and how many of those failed.
	static void GetCostStats(int& extraDatagrams, int& extraFailed);
};
