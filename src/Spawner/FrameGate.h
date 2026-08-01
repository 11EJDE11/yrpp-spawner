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

/**
 *  Makes the engine's frame-advance gate frame-aware.
 *
 *  Vanilla (Wait_For_Players, 0x6495DC) refuses to advance while ANY peer has
 *  `their[i].Recv < their[i].Sent` -- "I have not decoded everything you claim to
 *  have sent". That test is not qualified by *when* the missing commands are due.
 *  Commands are stamped `Frame + MaxAhead` at the sender, so a lost packet blocks
 *  the whole lobby immediately even though nobody needs those commands for up to
 *  MaxAhead more frames, and the stall lasts a full retransmit round trip.
 *
 *  We replace that precondition with the exact one it was approximating: a peer
 *  only blocks us if it has commands outstanding AND those commands are due at or
 *  before the frame we are about to execute. This is strictly weaker than
 *  vanilla; the MaxAhead condition downstream is left untouched. The set of
 *  events executed at each frame number does not change, only the wall-clock
 *  moment a client reaches that frame -- so a patched and an unpatched client can
 *  sit in the same lobby, with no wire change.
 *
 *  ---------------------------------------------------------------------------
 *  Deriving the watermark
 *
 *  Every inbound packet header carries the stamp `F` its commands execute on and
 *  `C`, the sender's cumulative command count *before* this block. Delivery of
 *  command-bearing packets is reliable and in-order (PACKET_DATA_ACK, LastSeqID
 *  walks a contiguous prefix at 0x48C24A), so our own `Recv` counts a contiguous
 *  prefix too. Therefore:
 *
 *      C <= Recv   =>   we hold every command stamped strictly below F
 *
 *  because all commands stamped below F belong to earlier send periods and so sit
 *  within the sender's first C commands.
 *
 *  We deliberately claim `F - 1` and not `F`. One send period can emit several
 *  blocks that all carry the same stamp (the break_when loop at 0x649FC8), so a
 *  later same-stamp block may still be missing. Claiming F would let us execute
 *  frame F without it -- a desync. F-1 excludes exactly that uncertainty and
 *  costs a single frame of runway.
 *
 *  ---------------------------------------------------------------------------
 *  Timing epochs
 *
 *  A MaxAhead / FrameSendRate change moves where future commands are stamped.
 *  Growth is harmless: later commands still stamp higher, so stamp order still
 *  matches command order and every existing watermark stays valid. It is
 *  important *not* to reset on growth, because ProtocolZero's ladder raises
 *  MaxAhead under loss -- resetting there would disable the gate at exactly the
 *  moment it is needed.
 *
 *  Only a decrease can give a later command a lower stamp than an older one. Then
 *  we clear the watermarks and fall back to vanilla for a guard window long
 *  enough for old-epoch commands to drain.
 */
class FrameGate
{
public:
	//!< theirsyncs[7] @ 0xAFA358, bounded by the local `my_sent` word at 0xAFA400.
	//!< Indexed defensively to 8 because the engine's own arrays are 8 wide.
	static constexpr int MaxPeers = 8;

	/**
	 *  A peer's entry in the engine's `theirsyncs` array.
	 *
	 *  Sent is effectively 16-bit (assigned from the header's `unsigned short
	 *  CommandCount`) while Recv accumulates unbounded, so the two are only
	 *  comparable the way the engine compares them -- unsigned, at 0x6495E6.
	 */
	struct TheirSync
	{
		int Frame;       //!< peer's execution frame, from ev->Frame - ev->Delay
		int Sent;        //!< commands the peer claims to have sent (cumulative)
		int Recv;        //!< commands we have actually decoded from them
		int TimingC;
		int Unknown10;
		int Timing14;
	};
	static_assert(sizeof(TheirSync) == 24, "theirsyncs stride is 24 bytes");

	static TheirSync* Peers()
	{
		return reinterpret_cast<TheirSync*>(0xAFA358u);
	}

	static bool Enable;

	//!< Frames advanced that the vanilla command-count test would have stalled.
	static int Saves;

	//!< Times we blocked anyway, i.e. the runway was too short to cover the
	//!< retransmit. This is the outcome signal the latency ladder needs: it
	//!< measures whether the chosen MaxAhead was actually sufficient, which
	//!< round-trip time alone cannot show.
	static int Blocks;

	//!< Commands that arrived stamped at or before the current frame. Must stay
	//!< zero: the engine's own "Packet received too late!" reporter (0x652070) is
	//!< dead code with no callers, so this is the only detector we have.
	static int LateArrivals;

	static void Reset();

	//!< Records a peer's watermark from an inbound packet header.
	//!< `theirEntry` is &their[index]; `ev` is the packet.
	static void OnReceive(unsigned int theirEntry, const unsigned char* ev);

	//!< True if no peer is missing a command that is due by the current frame.
	//!< `gapIndex` receives the blocking peer, matching the value vanilla leaves
	//!< in ESI for its MPStats bookkeeping.
	static bool AllCommandsSatisfied(int* gapIndex);
};
