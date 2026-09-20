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

#include "ProtocolZero.h"
#include "ProtocolZero.LatencyLevel.h"
#include "Spawner.h"

#include <Ext/Event/Body.h>
#include <Helpers/Macro.h>
#include <Utilities/Debug.h>
#include <Unsorted.h>
#include <EventClass.h>

DEFINE_HOOK(0x55DDA0, MainLoop_AfterRender__ProtocolZero, 0x5)
{
	if (ProtocolZero::Enable)
		ProtocolZero::SendResponseTime2();

	return 0;
}

DEFINE_HOOK(0x647BEB, QueueAIMultiplayer__ProtocolZero1, 0x9)
{
	if (ProtocolZero::Enable)
		return 0x647BF4;

	return (R->ESI() >= 5)
		? 0x647BF4
		: 0x647F36;
}

DEFINE_HOOK(0x647EB4, QueueAIMultiplayer__ProtocolZero2, 0x8)
{
	if (ProtocolZero::Enable)
	{
		R->AL(LatencyLevel::NewFrameSendRate);
		R->ECX((DWORD)Unsorted::CurrentFrame);

		return 0x647EBE;
	}

	return 0;
}

// Generate_Real_Timing_Event's FrameSendRate field.
//
// The hook below supplies this event's MaxAhead, so the send rate has to come
// from the same place or the two disagree. A mismatched pair breaks the
// FRAMEINFO sync check: Send_Packets stamps at roundup(Frame + MaxAhead,
// FrameSendRate) but stores Delay = MaxAhead (0x649D5E), and the receiver
// recovers the checksummed frame as Frame - Delay. Those agree only when the
// round-up does not round, i.e. when MaxAhead % FrameSendRate == 0 - otherwise
// the receiver compares the wrong CRC slot and reports a false out-of-sync.
DEFINE_HOOK(0x647DDB, QueueAIMultiplayer_TimingSendRate_ProtocolZero, 0xA)
{
	enum { Resume = 0x647DE5 };

	if (!ProtocolZero::Enable)
		return 0;

	// The skipped bytes also computed EDX for the MaxAhead field, which the
	// hook below replaces outright, so losing them costs nothing.
	R->AL(LatencyLevel::NewFrameSendRate);
	return Resume;
}

// Generate_Real_Timing_Event builds a periodic TIMING event, and this supplies
// its MaxAhead field.
//
// It used to supply Game::Network::MaxAhead - the value already in force. That
// is why a descent never stuck: the level and PreCalcMaxAhead moved, but the
// very next periodic event re-broadcast the old MaxAhead and EventClass::Execute
// put it straight back. In Game 25 the level reached 3 (and 1 on one client)
// while the engine sat at MaxAhead 36 from frame 256 to the end.
//
// Carrying the target instead is what makes any latency change actually reach
// the engine. TargetMaxAhead is always a multiple of the live FrameSendRate, so
// the stamp horizon stays exactly MaxAhead ahead.
DEFINE_HOOK(0x647DF2, QueueAIMultiplayer__ProtocolZero3, 0x5)
{
	if (ProtocolZero::Enable)
	{
		const int target = LatencyLevel::TargetMaxAhead;
		const int carry = target > 0 ? target : (int)Game::Network::MaxAhead;

		R->EDX((DWORD)carry & 0xffff);

		return 0x647DF2 + 0x5;
	}

	return 0;
}

// EventClass::Execute, TIMING case: open the command-rescheduling window on
// every timing change, not only on an increase.
//
// Commands issued after a TIMING event is created but before it executes carry
// the OLD MaxAhead, so the engine re-schedules anything caught in that window
// to the end of it. Vanilla arms that only when MaxAhead or FrameSendRate
// rises (two `ja` tests here and at 0x4C8043) and otherwise zeroes the window
// at 0x4C8045, which makes Execute_DoList's `Frame > F1 && Frame < F2` test
// unsatisfiable - so on a descent nothing is repaired and off-cadence stamps
// are dropped at 0x64C5B4. That is the only reason descending was unsafe.
//
// The window is sized here rather than at 0x4C804E because that path uses the
// NEW horizon; on a decrease the vulnerable stamps reach as far as the OLD one,
// so the larger is used. Inputs are the event's own fields plus the pre-change
// global, so every machine computes the same window.
DEFINE_HOOK(0x4C8033, EventClassExecute_TimingWindow_ProtocolZero, 0x9)
{
	enum { StoreFrame2 = 0x4C8070 };

	if (!ProtocolZero::Enable)
		return 0;

	GET(EventClass* const, event, ESI);

	const int eventFrame = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(event) + 3);
	const int newMaxAhead = *reinterpret_cast<const unsigned short*>(reinterpret_cast<const char*>(event) + 9);
	int newSendRate = *reinterpret_cast<const unsigned char*>(reinterpret_cast<const char*>(event) + 0x0B);

	if (newSendRate < 1)
		newSendRate = 1;

	const int oldMaxAhead = Game::Network::MaxAhead;
	const int oldSendRate = Game::Network::FrameSendRate;

	// Only a real transition needs a repair window. Most timing events restate
	// the current settings, and opening a window for those drags every queued
	// command forward for nothing. Vanilla clears it here (0x4C8045) and is
	// right to; what it gets wrong is the DECREASE case, handled below.
	if (newMaxAhead == oldMaxAhead && newSendRate == oldSendRate)
	{
		// It must not create a window, but it must not tear down a live one
		// either: a command that crossed a real transition can still be in
		// flight, and clearing the window early loses it at 0x64C5B4. Leave the
		// window alone until it has expired.
		const int liveFrame2 = Game::Network::NewMaxAheadFrame2;
		if (liveFrame2 > 0 && static_cast<int>(Unsorted::CurrentFrame) < liveFrame2)
		{
			R->EAX(liveFrame2); // rewrite the same value; Frame1 is untouched
			return StoreFrame2;
		}

		Game::Network::NewMaxAheadFrame1 = 0;
		R->EAX(0);                             // 0x4C8070 stores it as Frame2
		return StoreFrame2;
	}

	const int span = newMaxAhead > oldMaxAhead ? newMaxAhead : oldMaxAhead;

	// MaxAhead % FrameSendRate must be zero, and MaxAhead must be at least
	// 2 * FrameSendRate (Westwood's own documented minimum). Breaking either
	// drops commands off the execution cadence, stamps them past what the frame
	// gate covers, or compares a FRAMEINFO checksum against the wrong CRC slot.
	// Silent unless violated; costs one modulo per timing event.
	const bool divides = (newMaxAhead % newSendRate) == 0;
	const bool meetsFloor = newMaxAhead >= 2 * newSendRate;

	if (!meetsFloor)
	{
		Debug::Log("[Audit] INVARIANT maxahead=%d below floor 2*fsr=%d\n",
			newMaxAhead, 2 * newSendRate);
	}


	if (!divides)
	{
		// Name the consequence, so whoever reads the log does not have to
		// rediscover it. The stamp is roundup(Frame + MaxAhead, FrameSendRate)
		// while FRAMEINFO's Delay field carries MaxAhead unrounded, so the
		// receiver recovers the wrong frame by exactly this much.
		Debug::Log("[Audit] INVARIANT maxahead=%d %% fsr=%d = %d | FRAMEINFO crc index will be off by %d frames | expect a false out-of-sync\n"
			, newMaxAhead, newSendRate, newMaxAhead % newSendRate
			, newSendRate - (newMaxAhead % newSendRate));
	}

	// End of the vulnerable period, rounded up to a whole send period so it is
	// an execution frame under the new rate.
	const int frame2 = ((eventFrame + span + newSendRate - 1) / newSendRate) * newSendRate;

	Game::Network::NewMaxAheadFrame1 = eventFrame;
	R->EAX(frame2);                                 // 0x4C8070 stores it as Frame2
	return StoreFrame2;
}

DEFINE_HOOK(0x4C8011, EventClassExecute__ProtocolZero, 0x8)
{
	if (ProtocolZero::Enable)
		return 0x4C8024;

	return 0;
}

DEFINE_HOOK(0x64C598, ExecuteDoList__ProtocolZero, 0x6)
{
	if (ProtocolZero::Enable)
	{
		auto dl = (uint8_t)R->DL();

		if (dl == (uint8_t)EventType::Empty)
			return 0x64C63D;

		if (dl == (uint8_t)EventType::ProcessTime)
			return 0x64C63D;

		if (dl == (uint8_t)EventTypeExt::ResponseTime2)
			return 0x64C63D;
	}

	return 0;
}

// Connection timeout floor.
//
// Queue_AI_Multiplayer derives the per-connection timeout as
// 8 * Response_Time() + 15, floored at 120 ticks (1.92s). On a fast link the
// measured response is ~0, so that floor is what applies - and this PR pushes
// it lower still, because retransmitting sooner shrinks the Add_Delay samples
// feeding Avg_Response_Time. Raising the floor restores tolerance for a client
// that briefly stops servicing its socket. The cost is that a genuinely dead
// connection takes longer to be declared dead; the reconnect dialog runs on
// its own timer and is unaffected.
//
// The engine floors it in TWO places, with the same instruction pattern:
//   647707  cmp eax, 78h / jnb 647711 / mov eax, 78h   <- periodic path
//   647E56  cmp eax, 78h / jge 647E60 / mov eax, 78h   <- precalculated path
// The periodic one is the one that actually runs during a match; patching only
// the other left the floor with no effect at all, which the logs showed as
// timeouts still sitting at 159-183 ticks against a configured floor of 300.
DEFINE_HOOK(0x647707, QueueAIMultiplayer_TimeoutFloorPeriodic_ProtocolZero, 0x5)
{
	enum { Resume = 0x647711 };

	if (!ProtocolZero::Enable || ProtocolZero::ConnectionTimeoutFloor <= 0)
		return 0;

	const int derived = R->EAX();
	if (derived < ProtocolZero::ConnectionTimeoutFloor)
		R->EAX(ProtocolZero::ConnectionTimeoutFloor);

	return Resume;
}

DEFINE_HOOK(0x647E56, QueueAIMultiplayer_TimeoutFloor_ProtocolZero, 0x5)
{
	enum { Resume = 0x647E60 };

	if (!ProtocolZero::Enable || ProtocolZero::ConnectionTimeoutFloor <= 0)
		return 0;

	const int derived = R->EAX();
	const int floored = derived < ProtocolZero::ConnectionTimeoutFloor
		? ProtocolZero::ConnectionTimeoutFloor
		: derived;

	R->EAX(floored);
	return Resume;
}


DEFINE_HOOK_AGAIN(0x6476CB, QueueAIMultiplayer__ProtocolZero_ResponseTime, 0x5)
DEFINE_HOOK(0x647CC5, QueueAIMultiplayer__ProtocolZero_ResponseTime, 0x5)
{
	if (ProtocolZero::Enable)
	{
		R->EAX(ProtocolZero::WorstMaxAhead);
		return R->Origin() + 0x5;
	}

	return 0;
}
