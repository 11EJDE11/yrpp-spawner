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

#include "FrameGate.h"

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>
#include <Misc/NetTelemetry.h>

#include <GeneralDefinitions.h>
#include <IPXManagerClass.h>
#include <SessionClass.h>
#include <Unsorted.h>

#include <climits>

bool FrameGate::Enable = false;
int  FrameGate::Saves = 0;
int  FrameGate::Blocks = 0;
int  FrameGate::LateArrivals = 0;

namespace
{
	#define CurrentFrame   Make_Global<int>(0xA8ED84u)
	#define MaxAheadGlobal Make_Global<int>(0xA8B550u)
	#define FrameSendRate  Make_Global<int>(0xA8B554u)

	constexpr unsigned int TheirBase = 0xAFA358u;

	//!< Highest stamp F for which we hold peer i's complete command prefix, i.e.
	//!< every command stamped <= F is already in DoList. INT_MIN = nothing known.
	int SafeThrough[FrameGate::MaxPeers];

	int LastMaxAhead = -1;
	int LastFrameSendRate = -1;

	//!< While CurrentFrame is below this, the watermarks are untrustworthy: we
	//!< neither consult nor update them and the vanilla condition carries us.
	int GuardUntilFrame = 0;

	void ClearWatermarks()
	{
		for (int i = 0; i < FrameGate::MaxPeers; ++i)
			SafeThrough[i] = INT_MIN;
	}

	/**
	 *  Growth is safe -- later commands still stamp higher, so stamp order still
	 *  matches command order and existing watermarks remain valid. Resetting on
	 *  growth would be actively harmful: ProtocolZero raises MaxAhead under loss,
	 *  so the gate would switch itself off exactly when it is needed.
	 *
	 *  Only a decrease can hand a later command a lower stamp than an older one.
	 */
	void CheckEpoch(int frame, int maxAhead, int frameSendRate)
	{
		const bool shrank =
			   (LastMaxAhead >= 0 && maxAhead < LastMaxAhead)
			|| (LastFrameSendRate >= 0 && frameSendRate < LastFrameSendRate);

		if (shrank)
		{
			ClearWatermarks();

			// Long enough for anything stamped under the old epoch to arrive.
			GuardUntilFrame = frame + LastMaxAhead + frameSendRate + 4;

			NetTelemetry::Log("[FrameGate] timing shrank (MaxAhead %d -> %d, FrameSendRate %d -> %d),"
				" falling back to vanilla until frame %d\n",
				LastMaxAhead, maxAhead, LastFrameSendRate, frameSendRate, GuardUntilFrame);
		}

		LastMaxAhead = maxAhead;
		LastFrameSendRate = frameSendRate;
	}
}

void FrameGate::Reset()
{
	ClearWatermarks();

	LastMaxAhead = -1;
	LastFrameSendRate = -1;
	GuardUntilFrame = 0;

	Saves = 0;
	Blocks = 0;
	LateArrivals = 0;
}

void FrameGate::OnReceive(unsigned int theirEntry, const unsigned char* ev)
{
	if (!ev || theirEntry < TheirBase)
		return;

	const unsigned int offset = theirEntry - TheirBase;
	if (offset % sizeof(TheirSync) != 0)
		return;

	const unsigned int index = offset / sizeof(TheirSync);
	if (index >= (unsigned int)MaxPeers)
		return;

	// Only the two header types carry a usable (stamp, count) pair: FRAMEINFO
	// (28) at the head of a data packet, and the FRAMESYNC (25) beacon. Beacons
	// are the more frequent of the two, which is why recording here rather than
	// on the data path alone is worth it.
	const unsigned char type = ev[0];
	if (type != 28 && type != 25)
		return;

	const int frame = CurrentFrame;

	// A stamp computed under the previous epoch must never be trusted under the
	// new one.
	if (frame < GuardUntilFrame)
		return;

	// EventClass: +0 Type, +1 IsExec, +2 ID, +3 Frame, then the payload union.
	// For both header types the cumulative command count is a u16 at +11.
	const int stamp = *reinterpret_cast<const int*>(ev + 3);
	const int count = *reinterpret_cast<const unsigned short*>(ev + 11);

	// Canary. Commands are stamped Frame + MaxAhead by the sender, so a stamp at
	// or before our current frame means we already executed the frame it belongs
	// to -- the gate ran too far. Nothing else in the engine notices this.
	if (stamp <= frame)
	{
		if (LateArrivals < 16)
		{
			NetTelemetry::Log("[FrameGate] LATE COMMAND from peer %u stamped %d at frame %d"
				" (MaxAhead=%d FrameSendRate=%d) -- gate advanced too far\n",
				index, stamp, frame, (int)MaxAheadGlobal, (int)FrameSendRate);
		}
		++LateArrivals;
	}

	// `Recv` here is still the count from before this packet's own commands are
	// folded in at 0x64A590, which is exactly what `count` is measured against.
	const int recv = reinterpret_cast<const TheirSync*>(theirEntry)->Recv;

	if (count <= recv && (stamp - 1) > SafeThrough[index])
		SafeThrough[index] = stamp - 1;
}

bool FrameGate::AllCommandsSatisfied(int* gapIndex)
{
	*gapIndex = -1;

	int connections = (int)IPXManagerClass::Instance.NumConnections;
	if (connections < 0)
		connections = 0;
	if (connections > MaxPeers)
		connections = MaxPeers;

	const int frame = CurrentFrame;
	const int maxAhead = MaxAheadGlobal;

	CheckEpoch(frame, maxAhead, FrameSendRate);

	const bool watermarksUsable = Enable && frame >= GuardUntilFrame;

	const TheirSync* peers = Peers();
	bool satisfied = true;
	bool relaxed = false;
	int minPeerFrame = INT_MAX;

	for (int i = 0; i < connections; ++i)
	{
		if (peers[i].Frame < minPeerFrame)
			minPeerFrame = peers[i].Frame;

		// Unsigned, matching the engine's `jb` at 0x6495E6.
		if ((unsigned int)peers[i].Recv >= (unsigned int)peers[i].Sent)
			continue;                            // vanilla-satisfied

		// Outstanding, but everything due by this frame is already in DoList.
		if (watermarksUsable && SafeThrough[i] >= frame)
		{
			relaxed = true;
			continue;
		}

		satisfied = false;
		if (*gapIndex < 0)
			*gapIndex = i;
	}

	if (!satisfied)
	{
		++Blocks;
	}
	else if (relaxed && frame < maxAhead + minPeerFrame)
	{
		// Only count it when the MaxAhead check downstream will actually let the
		// frame through, otherwise we would be crediting ourselves for frames
		// that stall anyway.
		++Saves;
	}

	return satisfied;
}

/**
 *  Wait_For_Players, replacing the per-peer command-count loop.
 *
 *  0x6495D5  8b 8c 24 48 07 00 00   mov ecx, [esp+748h]   ; `their`
 *
 *  Seven position-independent bytes that load the peer array and begin loop B.
 *  Reached only when NumConnections > 0. Loop A has already set EBX = min peer
 *  frame and EBP = that peer's index, both of which the downstream vanilla code
 *  needs, so we leave them alone and return its own continuations:
 *
 *    0x6495F9 -- satisfied, fall into the vanilla MaxAhead check
 *    0x649610 -- a peer is genuinely missing a command that is due now
 *
 *  Returning 0 runs the stolen load and falls into vanilla loop B unchanged,
 *  which is what non-network sessions get.
 */
DEFINE_HOOK(0x6495D5, WaitForPlayers_FrameGate_Gate, 0x7)
{
	// CnCNet matches run as GameMode::LAN (Spawner.cpp switches away from
	// Internet during setup). Both LAN and Internet use the Ipx/UDP path, so
	// Ipx.NumConnections is meaningful for either; campaign and skirmish must
	// fall through to the vanilla loop.
	const GameMode mode = SessionClass::Instance.GameMode;
	if (mode != GameMode::LAN && mode != GameMode::Internet)
		return 0;

	int gap = -1;
	if (FrameGate::AllCommandsSatisfied(&gap))
		return 0x6495F9;

	R->ESI(gap);
	return 0x649610;
}

/**
 *  Process_Receive_Packet, the convergence point for both a data packet's
 *  FRAMEINFO header and a FRAMESYNC beacon -- after the peer's frame and Sent
 *  count are updated, and before the type split at 0x64A409.
 *
 *  0x64A3F9  8b 4d 04        mov ecx, [ebp+4]     ; their[index].Sent
 *  0x64A3FC  33 c0           xor eax, eax
 *  0x64A3FE  66 8b 47 0b     mov ax, [edi+0Bh]    ; event->CommandCount
 *
 *  Nine position-independent bytes. EBP = &their[index], EDI = the packet, and
 *  Recv is still pre-update. Observe-only.
 */
DEFINE_HOOK(0x64A3F9, ProcessReceivePacket_FrameGate_Record, 0x9)
{
	if (FrameGate::Enable)
		FrameGate::OnReceive(R->EBP(), reinterpret_cast<const unsigned char*>(R->EDI()));

	return 0;
}
