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

// FrameSendRate for the periodic timing event. Must match the MaxAhead below:
// FRAMEINFO checks are only correct when MaxAhead % FrameSendRate == 0.
DEFINE_HOOK(0x647DDB, QueueAIMultiplayer_TimingSendRate_ProtocolZero, 0xA)
{
	enum { Resume = 0x647DE5 };

	if (!ProtocolZero::Enable)
		return 0;

	R->AL(LatencyLevel::NewFrameSendRate);
	return Resume;
}

// MaxAhead for the periodic timing event. Carries the target rather than the
// current value, otherwise every event re-broadcasts the old MaxAhead and a
// descent never takes effect.
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

// Opens the command rescheduling window on every timing change, not only on an
// increase. Commands issued under the old MaxAhead are otherwise dropped after
// a decrease. The window covers the larger of the old and new horizons.
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

	// Only a real change needs a rescheduling window.
	if (newMaxAhead == oldMaxAhead && newSendRate == oldSendRate)
	{
		// Don't clear a window that is still open.
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

	// MaxAhead must be a multiple of FrameSendRate and at least twice it.
	const bool divides = (newMaxAhead % newSendRate) == 0;
	const bool meetsFloor = newMaxAhead >= 2 * newSendRate;

	if (!meetsFloor)
	{
		Debug::Log("[Audit] INVARIANT maxahead=%d below floor 2*fsr=%d\n",
			newMaxAhead, 2 * newSendRate);
	}


	if (!divides)
	{
		Debug::Log("[Audit] INVARIANT maxahead=%d %% fsr=%d = %d | FRAMEINFO crc index will be off by %d frames | expect a false out-of-sync\n"
			, newMaxAhead, newSendRate, newMaxAhead % newSendRate
			, newSendRate - (newMaxAhead % newSendRate));
	}

	// Rounded up to a send frame under the new rate.
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

// Minimum connection timeout. The engine floors it at 120 ticks (1.92s), which
// can drop a client that stalls briefly. Applied at both places the engine
// floors it; the first is the one used during a match.
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
