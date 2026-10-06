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

	// Milliseconds of protection left, decaying over time and topped up by loss.
	const int  GaugeBump = 4000;
	const int  GaugeCap = 20000;
	const int  GaugeMsPerUnit = 1;

	// An inbound gap shows incoming loss, not that the peer missed what was sent, so it
	// buys much less than a resend. It is the earliest signal available.
	const int  InboundGapBump = 500;
	const int  InboundGapRunCap = 3;

	// Last NOACK packet id seen from each peer, for gap detection.
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

	// The peer index for a connection (spawn.ini player index minus one), or -1
	// for the global channels.
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

// Shared channels are ignored.
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

// Current loss gauge for peer.
int PacketRedundancy::LossGauge(int peer)
{
	if (!ValidPeer(peer))
		return 0;

	DecayGauge(peer);
	return g_gauge[peer];
}

void PacketRedundancy::NoteExtraSend(int sendResult)
{
	// Extra datagrams sent beyond what the engine would send.
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

void PacketRedundancy::NoteInboundPacket(const ConnectionClass* connection, int packetId)
{
	if (!Enabled || packetId < 0)
		return;

	const int peer = PeerIndexForConnection(connection);
	if (!ValidPeer(peer))
		return;

	if (g_lastNoAckValid[peer] && packetId > g_lastNoAckId[peer] + 1)
	{
		// Only forward progress counts.
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
		// A late id was reordered, not lost: refund what the gap charged.
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

// ConnectionClass::Receive_Packet.
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

	// A lost ACK costs a full retransmit timeout, and an ACK is small.
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
