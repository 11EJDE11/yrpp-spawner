/**
*  yrpp-spawner
*
*  PacketRedundancy - outbound redundancy for reliable command packets (FIX #3).
*
*  Reliable command packets (CommHeader Code == PACKET_DATA_ACK) are sent
*  several times on the wire so an isolated loss on the lossy hop needs no
*  retransmit at all - the peer's ARQ delivers the first copy to arrive and
*  discards the rest for free. It rides below the ARQ, so a total loss still
*  falls back to the (now fast) retransmit, and it carries no determinism risk.
*
*  Only command-bearing packets are duplicated; the every-period
*  heartbeats/ACKs are left alone, so the extra bandwidth stays small and does
*  not scale with player count.
*
*  Adaptive mode: only duplicate while recent loss is actually being observed
*  (fed by NoteResend), so a clean link pays nothing.
*
*  Driven from the send path in NetHack.cpp / Replacements/NetHack.cpp.
*/

#pragma once
#include <stddef.h>

class PacketRedundancy
{
public:
	// Master switch and copy count (set from [Settings] PacketRedundancy /
	// RedundancyCopies, default on / 2).
	static bool Enabled;
	static int  Copies;
	// When true, only duplicate while recent packet loss is being observed.
	static bool Adaptive;

	// Number of times this outbound datagram should be sent (1 == send once, no
	// duplication). buf points at the on-wire game bytes ([CRC(4)][CommHeader...]).
	static int CopiesFor(const char* buf, size_t len);

	// Loss signal for adaptive mode: called when a retransmit is detected.
	static void NoteResend();
};
