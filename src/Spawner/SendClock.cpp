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

#include "SendClock.h"

#include <Misc/RenderThrottle.h>

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>
#include <Misc/NetTelemetry.h>

#include <SessionClass.h>

bool SendClock::Enable = false;
int  SendClock::MaxDrift = 120;
int  SendClock::DoListLimit = 4000;
int  SendClock::Drift = 0;
int  SendClock::PeakDrift = 0;
int  SendClock::PeakDoList = 0;

namespace
{
	#define CurrentFrame Make_Global<int>(0xA8ED84u)
	#define DoListCount  Make_Global<int>(0x8B41F8u)

	//!< Re-evaluate growth this often. Shrinking is checked every frame because
	//!< it is bounded by elapsed frames.
	constexpr int GrowIntervalFrames = 30;

	int LastFrame = -1;
	int NextGrowFrame = 0;
}

void SendClock::Reset()
{
	Drift = 0;
	PeakDrift = 0;
	PeakDoList = 0;
	LastFrame = -1;
	NextGrowFrame = 0;
}

int SendClock::SendFrame()
{
	return CurrentFrame + (Enable ? Drift : 0);
}

void SendClock::Tick()
{
	if (!Enable)
	{
		Drift = 0;
		return;
	}

	if (!SessionClass::IsMultiplayer())
	{
		Drift = 0;
		return;
	}

	const int frame = CurrentFrame;
	if (frame == LastFrame)
		return;

	const int advanced = (LastFrame < 0) ? 0 : (frame - LastFrame);
	LastFrame = frame;

	if (advanced <= 0)
		return;   // frame went backwards (load/restart): leave drift alone

	const int doList = DoListCount;
	if (doList > PeakDoList)
		PeakDoList = doList;

	// Shrink first, and never by more than the frames that just elapsed. This is
	// what keeps `Frame + Drift` monotonic: peers latch the highest frame we have
	// ever reported, so a stamp that moved backwards would let them run past
	// frames we have not stamped commands for.
	const bool overloaded = doList > DoListLimit;
	const bool recovered = !RenderThrottle::IsBottleneck();

	if (overloaded || recovered)
	{
		const int shrink = (advanced < Drift) ? advanced : Drift;
		if (shrink > 0)
		{
			Drift -= shrink;
			if (Drift == 0)
				NetTelemetry::Log("[SendClock] drift back to 0 at frame %d (%s)\n",
					frame, overloaded ? "DoList pressure" : "no longer the bottleneck");
		}
		return;
	}

	if (frame < NextGrowFrame)
		return;

	NextGrowFrame = frame + GrowIntervalFrames;

	// We are the client everyone is waiting on and the queue has room, so buy
	// them some headroom. One send-period-ish step at a time; there is no hurry
	// and every frame of drift is a frame of extra input latency for us.
	if (Drift < MaxDrift)
	{
		Drift += 4;
		if (Drift > MaxDrift)
			Drift = MaxDrift;

		if (Drift > PeakDrift)
		{
			PeakDrift = Drift;
			NetTelemetry::Log("[SendClock] drift now %d frames at frame %d (DoList=%d)\n",
				Drift, frame, doList);
		}
	}
}

// ============================================================
// Hooks
// ============================================================
//
// Each of the three send-path stamp formulas loads Frame from 0xA8ED84, once per
// PacketProtocol branch. We displace that load and hand back Frame + Drift, which
// leaves the surrounding arithmetic -- including the FrameSendRate rounding --
// exactly as the engine wrote it.
//
// Every site is a plain `mov reg, [abs]`: position-independent, so returning an
// explicit continuation is safe.

/**
*  Send_Packets, the FRAMEINFO header stamp. PacketProtocol == 2 branch.
*  0x649D16  a1 84 ed a8 00   mov eax, Frame
*/
DEFINE_HOOK(0x649D16, SendPackets_SendClock_Stamp, 0x5)
{
	R->EAX(SendClock::SendFrame());
	return 0x649D1B;
}

/**
*  Send_Packets, uncompressed branch.
*  0x649D32  a1 84 ed a8 00   mov eax, Frame
*/
DEFINE_HOOK(0x649D32, SendPackets_SendClock_StampRaw, 0x5)
{
	R->EAX(SendClock::SendFrame());
	return 0x649D37;
}

/**
*  Add_Compressed_Events, the per-event stamp written into OutList (and from
*  there into our own DoList). Must match the header above or our local copy
*  would execute on a different frame from everyone else's.
*  0x64B989  a1 84 ed a8 00   mov eax, Frame
*/
DEFINE_HOOK(0x64B989, AddCompressedEvents_SendClock_Stamp, 0x5)
{
	R->EAX(SendClock::SendFrame());
	return 0x64B98E;
}

/**
*  Add_Compressed_Events, uncompressed branch.
*  0x64B9A5  a1 84 ed a8 00   mov eax, Frame
*/
DEFINE_HOOK(0x64B9A5, AddCompressedEvents_SendClock_StampRaw, 0x5)
{
	R->EAX(SendClock::SendFrame());
	return 0x64B9AA;
}

/**
*  Send_FrameSync, the beacon that carries our frame to peers between data
*  packets. Note this one loads into EDX, not EAX.
*  0x64A02F  8b 15 84 ed a8 00   mov edx, Frame
*/
DEFINE_HOOK(0x64A02F, SendFrameSync_SendClock_Stamp, 0x6)
{
	R->EDX(SendClock::SendFrame());
	return 0x64A035;
}

/**
*  Send_FrameSync, uncompressed branch.
*  0x64A057  a1 84 ed a8 00   mov eax, Frame
*/
DEFINE_HOOK(0x64A057, SendFrameSync_SendClock_StampRaw, 0x5)
{
	R->EAX(SendClock::SendFrame());
	return 0x64A05C;
}
