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

#include "PacketRedundancy.h"
#include "NetDiagnostics.h"

#include <Helpers/Macro.h>

#include <windows.h>
#include <IPXManagerClass.h>
#include <ConnectionClass.h>
#include <IPXConnClass.h>
#include <Utilities/Debug.h>

bool PacketRedundancy::Enabled  = true;
int  PacketRedundancy::Copies   = 2;
bool PacketRedundancy::Adaptive = true;
bool PacketRedundancy::Acks     = true;

namespace
{
	int g_extraDatagrams = 0;
	int g_extraFailed = 0;

	constexpr size_t HeaderOffset = 4;
	constexpr size_t CodeOffset = HeaderOffset + offsetof(CommHeaderType, Code);

	// Milliseconds of protection remaining, decayed in real time and topped up
	// by evidence of loss. A resend is strong evidence but arrives one RTO late;
	// an inbound sequence gap is weaker but immediate.
	const int  GaugeBump = 4000;
	const int  GaugeCap = 20000;
	const int  GaugeMsPerUnit = 1;

	// An inbound gap is weaker evidence than a resend (it proves the path is
	// lossy, not that this peer missed something we sent) so it buys less, but
	// it arrives an entire retransmit timeout earlier. A longer run of missing
	// ids means a worse burst and buys proportionally more.
	// Deliberately much weaker than a resend: a gap proves loss coming TOWARD
	// us, while redundancy protects what we SEND. It stays as a trigger because
	// it is the only evidence available before a retransmit timeout, but
	// sustained coverage has to be earned by an actual resend.
	const int  InboundGapBump = 500;
	const int  InboundGapRunCap = 3;

	// Last NOACK packet id seen from each peer, for gap detection. Tracked here
	// rather than borrowed from NetDiagnostics so redundancy keeps working when
	// the diagnostics are stripped.
	int g_lastNoAckId[PacketRedundancy::MaxPeers];
	bool g_lastNoAckValid[PacketRedundancy::MaxPeers];

	int   g_gauge[PacketRedundancy::MaxPeers] = {};
	int   g_duplicates[PacketRedundancy::MaxPeers] = {};
	DWORD g_lastTick[PacketRedundancy::MaxPeers] = {};
	DWORD g_lastErrorLogTick = 0;
	bool g_errorLogged = false;

	bool ValidPeer(int peer)
	{
		return peer >= 0 && peer < PacketRedundancy::MaxPeers;
	}

	void DecayGauge(int peer)
	{
		if (!ValidPeer(peer))
			return;

		DWORD now = GetTickCount();
		if (g_lastTick[peer] == 0)
		{
			g_lastTick[peer] = now;
			return;
		}

		DWORD dt = now - g_lastTick[peer];
		g_lastTick[peer] = now;
		int dec = static_cast<int>(dt) / GaugeMsPerUnit;
		g_gauge[peer] = (dec >= g_gauge[peer]) ? 0 : (g_gauge[peer] - dec);
	}

	void BumpGauge(int peer)
	{
		if (!ValidPeer(peer))
			return;

		DecayGauge(peer);
		g_gauge[peer] += GaugeBump;
		if (g_gauge[peer] > GaugeCap)
			g_gauge[peer] = GaugeCap;
	}

	// Maps a connection to the peer index CopiesFor() is keyed by - the
	// ListAddress slot, i.e. the spawn.ini player index minus one. Spawner sets
	// each node's address to its player index, so the first four bytes of
	// NodeAddress hold that index.
	//
	// Returns -1 for anything that is not a live private connection, including
	// the global, router and multicast channels, which share
	// ConnectionClass::Service_Send_Queue but do not belong to any one peer.
	int PeerIndexForConnection(const ConnectionClass* connection)
	{
		if (!connection)
			return -1;

		int nconn = static_cast<int>(IPXManagerClass::Instance.NumConnections);
		const int arraySize = sizeof(IPXManagerClass::Instance.Connection) / sizeof(IPXManagerClass::Instance.Connection[0]);
		if (nconn > arraySize) nconn = arraySize;

		for (int i = 0; i < nconn; ++i)
		{
			const IPXConnClass* conn = IPXManagerClass::Instance.Connection[i];
			if (conn != connection)
				continue;

			DWORD slot = 0;
			memcpy(&slot, conn->Address.NodeAddress, sizeof(slot));

			const int peer = static_cast<int>(slot) - 1;
			return ValidPeer(peer) ? peer : -1;
		}

		return -1;
	}
}

void PacketRedundancy::GetCostStats(int& extraDatagrams, int& extraFailed)
{
	extraDatagrams = g_extraDatagrams;
	extraFailed = g_extraFailed;
}

void PacketRedundancy::Reset()
{
	g_lastErrorLogTick = 0;
	g_errorLogged = false;
	for (int i = 0; i < MaxPeers; ++i)
	{
		g_gauge[i] = 0;
		g_lastNoAckId[i] = 0;
		g_lastNoAckValid[i] = false;
		g_lastTick[i] = 0;
		g_duplicates[i] = 0;
	}
}

int PacketRedundancy::ClampCopies(int copies)
{
	return copies == 1 ? 1 : 2;
}

// Bumps the loss gauge for the peer this resend belongs to. A connection that
// does not resolve to a peer is a shared channel, not evidence about any one
// player, so it is ignored rather than raising every peer's gauge.
void PacketRedundancy::NoteResend(const ConnectionClass* connection)
{
	const int peer = PeerIndexForConnection(connection);
	if (ValidPeer(peer))
		BumpGauge(peer);
}

int PacketRedundancy::Duplicates(int peer)
{
	return ValidPeer(peer) ? g_duplicates[peer] : 0;
}

// Current loss gauge for peer, decayed up to now.
int PacketRedundancy::LossGauge(int peer)
{
	if (!ValidPeer(peer))
		return 0;

	DecayGauge(peer);
	return g_gauge[peer];
}

void PacketRedundancy::NoteExtraSend(int sendResult)
{
	// Every extra datagram this feature puts on the wire that vanilla would not
	// have sent. The reviewer's arithmetic is worth confirming in the field:
	// with ACK duplication on, one delivered command can cost two data
	// datagrams plus up to four ACKs where stock sends one of each, so this
	// counter is the honest bandwidth price of the loss recovery.
	++g_extraDatagrams;
	if (sendResult == -1)
	{
		++g_extraFailed;
		const DWORD now = GetTickCount();
		if (!g_errorLogged || now - g_lastErrorLogTick >= 1000)
		{
			g_errorLogged = true;
			g_lastErrorLogTick = now;
			Debug::Log("[PacketRedundancy] extra sendto() for a duplicate copy failed\n");
		}
	}
}

// Decides how many times to send this outbound datagram. Only reliable
// (PACKET_DATA_ACK) packets and, when enabled, their acknowledgements
// duplicate, and only while enabled and - if Adaptive - actual loss is being
// observed for this peer.
// One received unreliable packet. A forward jump in the id means the packets in
// between were lost on the way here, which is the earliest loss signal
// available anywhere in the stack - it costs no timeout and no retransmit.
void PacketRedundancy::NoteInboundPacket(const ConnectionClass* connection, int packetId)
{
	if (!Enabled || packetId < 0)
		return;

	const int peer = PeerIndexForConnection(connection);
	if (!ValidPeer(peer))
		return;

	if (g_lastNoAckValid[peer] && packetId > g_lastNoAckId[peer] + 1)
	{
		// Only forward progress is evidence; a repeat or a reorder is not.
		int run = packetId - g_lastNoAckId[peer] - 1;
		if (run > InboundGapRunCap)
			run = InboundGapRunCap;

		DecayGauge(peer);
		g_gauge[peer] += InboundGapBump * run;
		if (g_gauge[peer] > GaugeCap)
			g_gauge[peer] = GaugeCap;
	}

	else if (g_lastNoAckValid[peer] && packetId < g_lastNoAckId[peer])
	{
		// The missing id turning up late means it was reordered, not lost. Give
		// back what the gap charged for it, otherwise a path that merely
		// reorders - which costs the game nothing, since the engine sequences
		// these itself - reads as loss and pins redundancy on permanently.
		DecayGauge(peer);
		g_gauge[peer] -= InboundGapBump;
		if (g_gauge[peer] < 0)
			g_gauge[peer] = 0;
	}

	if (!g_lastNoAckValid[peer] || packetId > g_lastNoAckId[peer])
	{
		g_lastNoAckId[peer] = packetId;
		g_lastNoAckValid[peer] = true;
	}
}

// ConnectionClass::Receive_Packet. Owned here rather than by the diagnostics so
// the loss signal survives stripping them; NetDiagnostics is still fed for its
// run-length histogram when it is compiled in.
DEFINE_HOOK(0x48C040, ConnectionClass_ReceivePacket_Redundancy, 0x8)
{
	GET(const ConnectionClass*, conn, ECX);
	GET_STACK(const CommHeaderType*, buf, 0x4);

	if (buf && buf->MagicNumber == static_cast<short>(conn->MagicNum)
		&& buf->Code == ConnectionEnum::PACKET_DATA_NOACK)
	{
		PacketRedundancy::NoteInboundPacket(conn, buf->PacketID);
		NetDiagnostics::NoteNoAckPacket(conn, buf->PacketID);
	}

	return 0;
}

int PacketRedundancy::CopiesFor(const char* buf, size_t len, int peer)
{
	if (!Enabled || !buf || len <= CodeOffset)
		return 1;

	if (Copies < 2)
		return 1;

	const auto* header = reinterpret_cast<const CommHeaderType*>(buf + HeaderOffset);

	// A lost ACK costs as much as a lost command: the sender waits out its whole
	// retransmit timeout and resends a packet that already arrived, which also
	// destroys the RTT sample for it. An ACK is 18 bytes on the wire, so
	// duplicating one is far cheaper than the retransmission it prevents.
	const bool reliable = header->Code == ConnectionEnum::PACKET_DATA_ACK;
	const bool ack = Acks && header->Code == ConnectionEnum::PACKET_ACK;
	if (!reliable && !ack)
		return 1;

	if (Adaptive && LossGauge(peer) <= 0)
		return 1;

	if (ValidPeer(peer))
		g_duplicates[peer] += Copies - 1;

	return Copies;
}
