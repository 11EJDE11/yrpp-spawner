/*
* Copyright (c) 2012, 2013, 2014 Toni Spets <toni.spets@iki.fi>
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
* WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
* MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
* ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
* WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
* ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
* OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include "NetHack.h"
#include "Spawner.h"
#include "PacketRedundancy.h"
#include "PacketBundling.h"
#include "NetDiagnostics.h"

#include <windows.h>
#include <stdint.h>
#include <winsock.h>
#pragma comment(lib, "wsock32.lib")

ListAddress ListAddress::Array[8] = {};

bool NetHack::RequirePortMatch = false;

u_short Tunnel::Id = 0;
u_long  Tunnel::Ip = 0;
u_short Tunnel::Port = 0;


int WINAPI NetHack::SendTo(
	int sockfd,
	char* buf,
	size_t len,
	int flags,
	sockaddr_in* dest_addr,
	int addrlen
)
{
	const int index = dest_addr->sin_addr.s_addr - 1;

	// validate index
	if (index >= 8 || index < 0)
		return -1;

	const auto player = ListAddress::Array[index];
	sockaddr_in tempDest;
	tempDest.sin_family = AF_INET;
	tempDest.sin_port = player.Port;
	tempDest.sin_addr.S_un.S_addr = player.Ip;

	const int copies = PacketRedundancy::CopiesFor(buf, len, index);

	// The engine's datagram, or a bundle of it and copies of what the peer has yet to acknowledge.
	size_t sendLen = len;
	char* sendBuf = const_cast<char*>(PacketBundling::PrepareSend(index, buf, len, sendLen,
		sockfd, flags, &tempDest, addrlen));

	int ret = Tunnel::SendTo(sockfd, sendBuf, sendLen, flags, &tempDest, addrlen);
	const int firstError = ret == SOCKET_ERROR ? WSAGetLastError() : 0;

	// A failed sendto means this machine lost its link, rather than packets being dropped upstream.
	if (ret == SOCKET_ERROR)
		NetDiagnostics::LogSendFailure(index, firstError);
	for (int i = 1; i < copies; ++i)
	{
		const int extra = Tunnel::SendTo(sockfd, sendBuf, sendLen, flags, &tempDest, addrlen);
		PacketRedundancy::NoteExtraSend(extra);
		if (ret == SOCKET_ERROR && extra != SOCKET_ERROR)
			ret = extra;
	}
	// Preserve the first socket error if every copy failed.
	if (ret == SOCKET_ERROR)
	{
		WSASetLastError(firstError);
		return ret;
	}

	// What the engine asked to send, whatever went on the wire.
	return static_cast<int>(len);
}

int WINAPI NetHack::RecvFrom(
	int sockfd,
	char* buf,
	size_t len,
	int flags,
	sockaddr_in* src_addr,
	int* addrlen
)
{
	// A copy that arrived in an earlier bundle comes first; its own FD_READ message was posted
	// when it was queued.
	const int queued = PacketBundling::TakeQueued(buf, len, src_addr, addrlen);
	if (queued >= 0)
		return queued;

	// Read into a buffer of our own: a bundle is longer than the engine's 640 bytes, and so could
	// be anything a peer sends.
	char datagram[2048];
	int ret = Tunnel::RecvFrom(sockfd, datagram, sizeof(datagram), flags, src_addr, addrlen);

	// bail out if error
	if (ret == -1)
		return ret;

	int peer = -1;

	// now, we need to map src_addr ip/port to index by reversing the search!
	for (char i = 0; i < (char)std::size(ListAddress::Array); ++i)
	{
		const auto player = ListAddress::Array[i];
		// compare ip
		if (src_addr->sin_addr.S_un.S_addr != player.Ip)
			continue;

		// compare port
		if (NetHack::RequirePortMatch && src_addr->sin_port != player.Port)
			continue;

		// found it, set this index to source addr
		src_addr->sin_addr.s_addr = i + 1;
		src_addr->sin_port = 0;
		peer = i;
		break;
	}

	return PacketBundling::Receive(sockfd, datagram, ret, peer, buf, len, src_addr, *addrlen);
}

int WINAPI Tunnel::SendTo(
	int sockfd,
	char* buf,
	size_t len,
	int flags,
	sockaddr_in* dest_addr,
	int addrlen
)
{
	char TempBuf[1024 + 4];
	uint16_t* BufFrom = reinterpret_cast<uint16_t*>(TempBuf);
	uint16_t* BufTo = reinterpret_cast<uint16_t*>(TempBuf + 2);

	// no processing if no tunnel
	if (!Tunnel::Port)
		return sendto(sockfd, buf, len, flags, (struct sockaddr*)dest_addr, addrlen);

	// copy packet to our buffer
	memcpy(TempBuf + 4, buf, len);

	// pull dest port to header
	*BufFrom = Tunnel::Id;
	*BufTo = dest_addr->sin_port;

	sockaddr_in sendDest = *dest_addr;
	sendDest.sin_port = Tunnel::Port;
	sendDest.sin_addr.S_un.S_addr = Tunnel::Ip;

	return sendto(sockfd, TempBuf, len + 4, flags, (struct sockaddr*)&sendDest, addrlen);
}

int WINAPI Tunnel::RecvFrom(
	int sockfd,
	char* buf,
	size_t len,
	int flags,
	sockaddr_in* src_addr,
	int* addrlen
)
{
	char TempBuf[1024 + 4];
	uint16_t* BufFrom = reinterpret_cast<uint16_t*>(TempBuf);
	uint16_t* BufTo = reinterpret_cast<uint16_t*>(TempBuf + 2);

	// no processing if no tunnel
	if (!Tunnel::Port)
		return recvfrom(sockfd, buf, len, flags, (struct sockaddr*)src_addr, addrlen);

	// call recvfrom first to get the packet
	int ret = recvfrom(sockfd, TempBuf, sizeof TempBuf, flags, (struct sockaddr*)src_addr, addrlen);

	// no processing if returning error or less than 5 bytes of data
	if (ret < 5 || *BufTo != Tunnel::Id)
		return -1;

	memcpy(buf, TempBuf + 4, ret - 4);

	src_addr->sin_port = *BufFrom;
	src_addr->sin_addr.s_addr = 0;

	return ret - 4;
}
