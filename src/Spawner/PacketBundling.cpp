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

#include "PacketBundling.h"
#include "NetHack.h"

#include <ConnectionClass.h>
#include <Unsorted.h>
#include <Utilities/Debug.h>

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

bool PacketBundling::Enabled = true;

namespace
{
	constexpr uint32_t BundleMagic = 0x4E425259; // 'YRBN'
	constexpr uint8_t KindHello = 1;
	constexpr uint8_t KindBundle = 2;
	constexpr size_t BundleHeaderBytes = 6;   // magic, kind, count
	constexpr size_t ItemHeaderBytes = 2;     // length

	// UDPInterfaceClass::Message_Handler reads each datagram into a 640-byte buffer (0x7B3EEC), so no
	// engine datagram is longer.
	constexpr size_t MaxDatagram = 640;
	// The CnCNet tunnel receives into 1024 bytes (TunnelV3), of which Tunnel::SendTo's header takes 4.
	constexpr size_t MaxBundleBytes = 1020;

	// An engine datagram: uint32 CRC, then CommHeaderType.
	constexpr size_t CrcBytes = 4;
	constexpr size_t MagicOffset = CrcBytes + offsetof(CommHeaderType, MagicNumber);
	constexpr size_t CodeOffset = CrcBytes + offsetof(CommHeaderType, Code);
	constexpr size_t IdOffset = CrcBytes + offsetof(CommHeaderType, PacketID);
	constexpr size_t MinDatagram = IdOffset + sizeof(int);

	// A copy goes out only once the original has had a moment to arrive on its own, so the two are
	// not lost to the same burst; and not for ever - the engine's own retransmit takes over.
	constexpr DWORD MinCopyAgeMs = 10;
	constexpr DWORD MaxDataAgeMs = 4000;
	constexpr DWORD MaxAckAgeMs = 400;
	constexpr int MaxCopiesPerData = 4;
	constexpr int MaxCopiesPerAck = 2;
	constexpr int MaxCopiesPerBundle = 8;

	constexpr int StoredPerPeer = 24;

	// Hellos go out with sends to a peer, a few at the start and more while that peer is not heard.
	constexpr DWORD HelloIntervalMs = 250;
	constexpr int MinHellos = 8;
	constexpr int MaxHellos = 40;

	// The message WSAAsyncSelect posts for the primary socket, which Message_Handler answers with a
	// recvfrom (0x7B3AB7 checks it).
	constexpr UINT PrimarySocketMessage = 1140;
	constexpr size_t QueueCapacity = 64;

	// Reliable packets received from each peer, to tell a copy of one already had from a new one.
	constexpr int SeenPerPeer = 256;

	struct Stored
	{
		bool Used;
		bool IsAck;
		short Magic;
		int Id;
		DWORD FirstSent;
		int Copies;
		uint16_t Len;
		char Data[MaxDatagram];
	};

	struct SeenId
	{
		short Magic;
		int Id;
	};

	struct Queued
	{
		int Len;
		char Data[MaxDatagram];
		sockaddr_in Src;
		int AddrLen;
	};

	struct Peer
	{
		Stored Entries[StoredPerPeer];
		SeenId Seen[SeenPerPeer];
		int SeenNext;
		int SeenCount;
		bool Understands;
		int HellosSent;
		DWORD LastHello;
	};

	Peer g_peers[PacketBundling::MaxPeers];

	Queued g_queue[QueueCapacity];
	size_t g_queueHead = 0;
	size_t g_queueCount = 0;

	int g_bundles = 0;
	int g_copiesSent = 0;
	int g_recovered = 0;
	int g_redundant = 0;
	bool g_loggedFirstBundle[PacketBundling::MaxPeers] = {};

	char g_out[MaxBundleBytes];

	bool ValidPeer(int peer)
	{
		return peer >= 0 && peer < PacketBundling::MaxPeers;
	}

	ConnectionEnum CodeOf(const char* datagram)
	{
		return static_cast<ConnectionEnum>(static_cast<unsigned char>(datagram[CodeOffset]));
	}

	short MagicOf(const char* datagram)
	{
		short magic;
		memcpy(&magic, datagram + MagicOffset, sizeof(magic));
		return magic;
	}

	int IdOf(const char* datagram)
	{
		int id;
		memcpy(&id, datagram + IdOffset, sizeof(id));
		return id;
	}

	Stored* FindStored(Peer& peer, bool isAck, short magic, int id)
	{
		for (auto& entry : peer.Entries)
		{
			if (entry.Used && entry.IsAck == isAck && entry.Magic == magic && entry.Id == id)
				return &entry;
		}
		return nullptr;
	}

	void ExpireStored(Peer& peer, DWORD now)
	{
		for (auto& entry : peer.Entries)
		{
			if (entry.Used && now - entry.FirstSent > (entry.IsAck ? MaxAckAgeMs : MaxDataAgeMs))
				entry.Used = false;
		}
	}

	// Keeps what this client sends a peer: its reliable packets until they are acknowledged, and its
	// ACKs for a short while.
	void NoteSent(Peer& peer, const char* datagram, size_t len, DWORD now)
	{
		const ConnectionEnum code = CodeOf(datagram);
		const bool isAck = code == ConnectionEnum::PACKET_ACK;
		if (code != ConnectionEnum::PACKET_DATA_ACK && !isAck)
			return;

		const short magic = MagicOf(datagram);
		const int id = IdOf(datagram);
		if (FindStored(peer, isAck, magic, id))
			return; // the engine's own retransmit; the first send's time stands

		Stored* pSlot = nullptr;
		for (auto& entry : peer.Entries)
		{
			if (!entry.Used)
			{
				pSlot = &entry;
				break;
			}
			if (!pSlot || entry.FirstSent < pSlot->FirstSent)
				pSlot = &entry;
		}

		pSlot->Used = true;
		pSlot->IsAck = isAck;
		pSlot->Magic = magic;
		pSlot->Id = id;
		pSlot->FirstSent = now;
		pSlot->Copies = 0;
		pSlot->Len = static_cast<uint16_t>(len);
		memcpy(pSlot->Data, datagram, len);
	}

	bool HasSeen(const Peer& peer, short magic, int id)
	{
		for (int i = 0; i < peer.SeenCount; ++i)
		{
			if (peer.Seen[i].Magic == magic && peer.Seen[i].Id == id)
				return true;
		}
		return false;
	}

	// Follows what arrives from a peer: an ACK settles the packet it names, and a reliable packet is
	// remembered so a later copy of it can be told apart.
	void NoteReceived(int peerIndex, const char* datagram, size_t len)
	{
		if (!ValidPeer(peerIndex) || len < MinDatagram)
			return;

		Peer& peer = g_peers[peerIndex];
		const ConnectionEnum code = CodeOf(datagram);
		const short magic = MagicOf(datagram);
		const int id = IdOf(datagram);

		if (code == ConnectionEnum::PACKET_ACK)
		{
			if (Stored* pEntry = FindStored(peer, false, magic, id))
				pEntry->Used = false;
		}
		else if (code == ConnectionEnum::PACKET_DATA_ACK && !HasSeen(peer, magic, id))
		{
			peer.Seen[peer.SeenNext] = { magic, id };
			peer.SeenNext = (peer.SeenNext + 1) % SeenPerPeer;
			peer.SeenCount = (std::min)(peer.SeenCount + 1, SeenPerPeer);
		}
	}

	void SendHello(Peer& peer, DWORD now, int sockfd, int flags, sockaddr_in* dest, int addrlen)
	{
		const bool wanted = peer.HellosSent < MinHellos || (!peer.Understands && peer.HellosSent < MaxHellos);
		if (!wanted || (peer.HellosSent > 0 && now - peer.LastHello < HelloIntervalMs))
			return;

		char hello[BundleHeaderBytes];
		memcpy(hello, &BundleMagic, sizeof(BundleMagic));
		hello[4] = static_cast<char>(KindHello);
		hello[5] = 0;
		Tunnel::SendTo(sockfd, hello, sizeof(hello), flags, dest, addrlen);

		peer.HellosSent++;
		peer.LastHello = now;
	}

	void Enqueue(const char* datagram, size_t len, const sockaddr_in* src, int addrlen, int sockfd)
	{
		// Only ever a copy, so one that cannot be queued is dropped; the engine's retransmit still
		// covers it.
		if (g_queueCount == QueueCapacity)
			return;

		// One more read for Message_Handler, which reads one datagram per message. Every queued copy
		// has its own message: Winsock posts FD_READ again only after a recvfrom, so a real FD_READ
		// answered from the queue leaves the socket to this one.
		if (!PostMessageA(Game::hWnd, PrimarySocketMessage, static_cast<WPARAM>(sockfd), FD_READ))
			return;

		Queued& slot = g_queue[(g_queueHead + g_queueCount) % QueueCapacity];
		slot.Len = static_cast<int>(len);
		memcpy(slot.Data, datagram, len);
		slot.Src = *src;
		slot.AddrLen = addrlen;
		++g_queueCount;
	}
}

void PacketBundling::Reset()
{
	memset(g_peers, 0, sizeof(g_peers));
	g_queueHead = 0;
	g_queueCount = 0;
	g_bundles = 0;
	g_copiesSent = 0;
	g_recovered = 0;
	g_redundant = 0;
	memset(g_loggedFirstBundle, 0, sizeof(g_loggedFirstBundle));
}

const char* PacketBundling::PrepareSend(int peerIndex, const char* buf, size_t len, size_t& outLen,
	int sockfd, int flags, sockaddr_in* dest, int addrlen)
{
	outLen = len;
	if (!Enabled || !ValidPeer(peerIndex) || !buf || len < MinDatagram || len > MaxDatagram)
		return buf;

	Peer& peer = g_peers[peerIndex];
	const DWORD now = GetTickCount();

	ExpireStored(peer, now);
	NoteSent(peer, buf, len, now);
	SendHello(peer, now, sockfd, flags, dest, addrlen);

	if (!peer.Understands)
		return buf;

	const bool sendingAck = CodeOf(buf) == ConnectionEnum::PACKET_ACK;
	const short sendingMagic = MagicOf(buf);
	const int sendingId = IdOf(buf);

	// Reliable packets first, oldest first: those are what a lost datagram costs a timeout for.
	Stored* candidates[StoredPerPeer];
	int count = 0;
	for (auto& entry : peer.Entries)
	{
		if (!entry.Used || now - entry.FirstSent < MinCopyAgeMs)
			continue;
		if (entry.IsAck == sendingAck && entry.Magic == sendingMagic && entry.Id == sendingId)
			continue;
		if (entry.Copies >= (entry.IsAck ? MaxCopiesPerAck : MaxCopiesPerData))
			continue;
		candidates[count++] = &entry;
	}

	if (count == 0)
		return buf;

	std::sort(candidates, candidates + count, [](const Stored* a, const Stored* b)
	{
		if (a->IsAck != b->IsAck)
			return !a->IsAck;
		return a->FirstSent < b->FirstSent;
	});

	size_t pos = BundleHeaderBytes;
	auto append = [&pos](const char* data, size_t size)
	{
		const uint16_t length = static_cast<uint16_t>(size);
		memcpy(g_out + pos, &length, sizeof(length));
		memcpy(g_out + pos + ItemHeaderBytes, data, size);
		pos += ItemHeaderBytes + size;
	};

	if (BundleHeaderBytes + ItemHeaderBytes + len > MaxBundleBytes)
		return buf;
	append(buf, len);

	int items = 1;
	for (int i = 0; i < count && items <= MaxCopiesPerBundle; ++i)
	{
		Stored* pEntry = candidates[i];
		if (pos + ItemHeaderBytes + pEntry->Len > MaxBundleBytes)
			continue;

		append(pEntry->Data, pEntry->Len);
		pEntry->Copies++;
		++items;
	}

	if (items == 1)
		return buf;

	memcpy(g_out, &BundleMagic, sizeof(BundleMagic));
	g_out[4] = static_cast<char>(KindBundle);
	g_out[5] = static_cast<char>(items);

	++g_bundles;
	g_copiesSent += items - 1;
	if (!g_loggedFirstBundle[peerIndex])
	{
		g_loggedFirstBundle[peerIndex] = true;
		Debug::Log("[PacketBundling] Bundling to peer %d.\n", peerIndex);
	}

	outLen = pos;
	return g_out;
}

int PacketBundling::Receive(int sockfd, const char* datagram, int datagramLen, int peerIndex,
	char* buf, size_t len, const sockaddr_in* src, int addrlen)
{
	const size_t size = static_cast<size_t>((std::max)(datagramLen, 0));

	uint32_t magic = 0;
	if (size >= BundleHeaderBytes)
		memcpy(&magic, datagram, sizeof(magic));

	if (magic != BundleMagic)
	{
		// One engine datagram. Message_Handler's buffer holds 640 bytes; anything longer could
		// never pass its CRC check, so it is dropped rather than written past the buffer.
		if (size > len)
			return 0;

		NoteReceived(peerIndex, datagram, size);
		memcpy(buf, datagram, size);
		return static_cast<int>(size);
	}

	const uint8_t kind = static_cast<uint8_t>(datagram[4]);
	const int items = static_cast<uint8_t>(datagram[5]);

	// Walked once to check it before anything is handed on.
	size_t pos = BundleHeaderBytes;
	for (int i = 0; i < items; ++i)
	{
		uint16_t length = 0;
		if (pos + ItemHeaderBytes > size)
			return 0;
		memcpy(&length, datagram + pos, sizeof(length));
		if (length < MinDatagram || length > MaxDatagram || pos + ItemHeaderBytes + length > size)
			return 0;
		pos += ItemHeaderBytes + length;
	}
	if (pos != size || (kind == KindHello) != (items == 0) || (kind != KindHello && kind != KindBundle))
		return 0;

	if (ValidPeer(peerIndex) && !g_peers[peerIndex].Understands)
	{
		g_peers[peerIndex].Understands = true;
		Debug::Log("[PacketBundling] Peer %d understands bundles.\n", peerIndex);
	}

	if (items == 0)
		return 0;

	int firstLen = 0;
	pos = BundleHeaderBytes;
	for (int i = 0; i < items; ++i)
	{
		uint16_t length = 0;
		memcpy(&length, datagram + pos, sizeof(length));
		const char* item = datagram + pos + ItemHeaderBytes;
		pos += ItemHeaderBytes + length;

		if (i == 0)
		{
			if (length > len)
				continue;

			NoteReceived(peerIndex, item, length);
			memcpy(buf, item, length);
			firstLen = length;
			continue;
		}

		// A copy of a reliable packet already had is dropped here: the engine would only answer
		// it with another ACK.
		if (ValidPeer(peerIndex) && CodeOf(item) == ConnectionEnum::PACKET_DATA_ACK)
		{
			if (HasSeen(g_peers[peerIndex], MagicOf(item), IdOf(item)))
			{
				++g_redundant;
				continue;
			}
			++g_recovered;
		}

		NoteReceived(peerIndex, item, length);
		Enqueue(item, length, src, addrlen, sockfd);
	}

	return firstLen;
}

int PacketBundling::TakeQueued(char* buf, size_t len, sockaddr_in* src, int* addrlen)
{
	while (g_queueCount > 0)
	{
		const Queued& slot = g_queue[g_queueHead];
		g_queueHead = (g_queueHead + 1) % QueueCapacity;
		--g_queueCount;

		if (static_cast<size_t>(slot.Len) > len)
			continue;

		memcpy(buf, slot.Data, slot.Len);
		if (src)
			*src = slot.Src;
		if (addrlen)
			*addrlen = slot.AddrLen;
		return slot.Len;
	}

	return -1;
}

void PacketBundling::GetStats(int& bundles, int& copiesSent, int& recovered, int& redundant)
{
	bundles = g_bundles;
	copiesSent = g_copiesSent;
	recovered = g_recovered;
	redundant = g_redundant;
}
