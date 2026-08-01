/**
*  yrpp-spawner
*
*  Copyright(C) 2026-present CnCNet
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

#pragma once

#include <stddef.h>

/**
 *  Sends reliable command packets more than once, so an isolated loss costs
 *  nothing instead of costing a retransmit round trip.
 *
 *  The command stream is tiny -- a few hundred bytes a second -- so a second copy
 *  is cheap next to what a stall costs everyone. The receiving engine discards
 *  the duplicate for free: ConnectionClass::Receive_Packet detects a repeated
 *  PacketID at 0x48C288, re-ACKs it and does not re-queue.
 *
 *  That last detail also keeps this safe for MEGAMISSION, which uses a
 *  cross-packet compression dictionary (a wire unit-count of 0 means "reuse the
 *  cached unit list for this slot"). Duplicates are dropped by the ARQ before
 *  Breakup_Receive_Packet ever sees them, so the dictionary is untouched.
 *
 *  Only PACKET_DATA_ACK is duplicated. The engine sets that code exactly when a
 *  packet carries commands (Send_Packets computes ack_req from OutList.Count at
 *  0x649CF9), so beacons and pure ACKs -- which are already idempotent and
 *  frequent -- are left alone.
 */
class PacketRedundancy
{
public:
	static bool Enabled;

	//!< Total times each qualifying packet is put on the wire. 1 disables.
	static int Copies;

	/**
	 *  Only duplicate while loss is actually being observed.
	 *
	 *  Off by default: duplicating unconditionally is what has been measured to
	 *  work, and the adaptive gauge can only ever remove copies. Turn it on to
	 *  stop clean links paying for bandwidth they do not need.
	 */
	static bool Adaptive;

	//!< Duplicate datagrams actually emitted, for the end-of-match summary.
	static int Emitted;

	static void Reset();

	/**
	 *  How many times to send this packet. `buf`/`len` are the plain game bytes
	 *  as handed to Tunnel::SendTo -- before any obfuscation or tunnel header,
	 *  since the packet code is read out of them.
	 */
	static int CopiesFor(const char* buf, size_t len);

	/**
	 *  Loss signal for adaptive mode. Called when a retransmit is observed;
	 *  FastRetransmit::SampleRTT is the natural place, because Karn's rule makes
	 *  it discard exactly those samples anyway.
	 */
	static void NoteResend();
};
