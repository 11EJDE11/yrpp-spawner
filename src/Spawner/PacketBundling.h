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
*  PacketBundling - copies of unacknowledged packets ride along with later ones.
*
*  A reliable packet that is lost waits a whole retransmit timeout before it is sent again. Here
*  every datagram to a peer also carries copies of the reliable packets still waiting for that
*  peer's ACK, and of this client's recent ACKs, so one lost datagram is made up by the next one
*  sent - typically a single send interval later rather than a round trip and a margin. That is
*  time diversity the existing PacketRedundancy (two copies back to back) does not give against a
*  burst of loss.
*
*  The engine already copes with what arrives: a reliable packet seen before is acknowledged and
*  dropped, one ahead of the next expected ID is queued until the gap fills (Receive_Packet,
*  0x48C040), and an ACK for an already acknowledged packet changes nothing. A copy of a packet this
*  side has already received is dropped here instead, so it does not make the engine send an ACK
*  for every copy.
*
*  A bundle is not a datagram an older spawner understands - it would fail the CRC check and lose
*  the packet inside - so each side bundles to a peer only after hearing a bundle or a small hello
*  from that peer. The receiving side always understands them.
*
*  On the wire, in place of one engine datagram (uint32 CRC, then the packet):
*    uint32 'YRBN', uint8 kind (1 hello, 2 bundle), uint8 count,
*    then count times: uint16 length, that many bytes of an engine datagram.
*  The datagram being sent is the first; the copies follow.
*/

#pragma once

#include <winsock.h>

class PacketBundling
{
public:
	static const int MaxPeers = 8;

	static bool Enabled;

	static void Reset();

	// For NetHack::SendTo: the datagram to send to this peer in place of buf, which is buf itself
	// when there is nothing to add. Also sends the peer a hello while it has not yet said it
	// understands bundles.
	static const char* PrepareSend(int peer, const char* buf, size_t len, size_t& outLen,
		int sockfd, int flags, sockaddr_in* dest, int addrlen);

	// For NetHack::RecvFrom: one datagram from the socket, of datagramLen bytes, from this peer
	// (-1 when it matched no player). Writes the first engine datagram to hand on into buf and
	// returns its length; 0 when there is none, as for a hello. Copies past the first are queued.
	static int Receive(int sockfd, const char* datagram, int datagramLen, int peer,
		char* buf, size_t len, const sockaddr_in* src, int addrlen);

	// A copy queued by Receive, ahead of reading the socket again. Returns its length, or -1 when
	// none is queued.
	static int TakeQueued(char* buf, size_t len, sockaddr_in* src, int* addrlen);

	// Datagrams sent as bundles, copies carried in them, copies received that this side had not
	// already had (what bundling made up for), and copies received that it had.
	static void GetStats(int& bundles, int& copiesSent, int& recovered, int& redundant);

	// GetTickCount when anything last arrived from this peer, 0 if nothing has.
	static unsigned long LastReceived(int peer);
};
