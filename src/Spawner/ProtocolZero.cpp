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
#include <Utilities/Debug.h>
#include <HouseClass.h>
#include <SessionClass.h>
#include <IPXManagerClass.h>
#include "FastRetransmit.h"
#include "NetDiagnostics.h"

bool ProtocolZero::Enable = false;
int ProtocolZero::ConnectionTimeoutFloor = 300;
int ProtocolZero::NextSendFrame = -1;
int ProtocolZero::WorstMaxAhead = LatencyLevel::GetMaxAhead(LatencyLevelEnum::LATENCY_LEVEL_6);
unsigned char ProtocolZero::MaxLatencyLevel = std::numeric_limits<unsigned char>::max();

void ProtocolZero::SendResponseTime2()
{
	if (SessionClass::IsSingleplayer())
		return;

	int currentFrame = Unsorted::CurrentFrame;

	if (ProtocolZero::NextSendFrame < 0)
	{
		ProtocolZero::NextSendFrame = currentFrame + Game::Network::FrameSendRate + ProtocolZero::SendResponseTimeFrame;
		return;
	}

	if (ProtocolZero::NextSendFrame >= currentFrame)
		return;

	const int engineResponseTime = IPXManagerClass::Instance.ResponseTime();
	if (engineResponseTime <= -1)
		return;

	int ipxResponseTime = engineResponseTime;

	// Use the measured delivery delay when it is worse than the engine's slow mean.
	const int cleanResponseTime = FastRetransmit::WorstDeliveryDelay();
	if (cleanResponseTime > ipxResponseTime)
		ipxResponseTime = cleanResponseTime;

	// Both fields are single bytes; saturate rather than wrap, since a wrapped
	// value is discarded by the receiver.
	const int cappedForField = ipxResponseTime > 126 ? 126 : ipxResponseTime;
	const int cappedForLevel = ipxResponseTime > 255 ? 255 : ipxResponseTime;

	EventExt event;
	event.Type = EventTypeExt::ResponseTime2;
	event.HouseIndex = (char)HouseClass::CurrentPlayer->ArrayIndex;
	event.Frame = currentFrame + Game::Network::MaxAhead;
	event.ResponseTime2.MaxAhead = (char)(cappedForField + 1);
	event.ResponseTime2.LatencyLevel = (uint8_t)LatencyLevel::FromResponseTime((uint8_t)cappedForLevel);

	if (event.AddEvent())
	{
		ProtocolZero::NextSendFrame = currentFrame + ProtocolZero::SendResponseTimeInterval;

		Debug::Log("Player %d sending response time of %d (raw %d), LatencyMode = %d, Frame = %d\n"
			, event.HouseIndex
			, event.ResponseTime2.MaxAhead
			, ipxResponseTime
			, event.ResponseTime2.LatencyLevel
			, currentFrame
		);
	}
	else
	{
		++ProtocolZero::NextSendFrame;
	}
}

void ProtocolZero::HandleResponseTime2(EventExt* event)
{
	if (ProtocolZero::Enable == false || SessionClass::IsSingleplayer())
		return;

	if (event->ResponseTime2.MaxAhead == 0)
	{
		Debug::Log("Returning because event->MaxAhead == 0\n");
		return;
	}

	static int32_t PlayerMaxAheads[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	static uint8_t PlayerLatencyMode[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	static int32_t PlayerLastTimingFrame[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

	int32_t houseIndex = event->HouseIndex;
	PlayerMaxAheads[houseIndex] = (int32_t)event->ResponseTime2.MaxAhead;
	PlayerLatencyMode[houseIndex] = event->ResponseTime2.LatencyLevel;
	PlayerLastTimingFrame[houseIndex] = event->Frame;

	uint8_t setLatencyMode = 0;
	int maxMaxAheads = 0;
	int worstRttSlot = -1;
	int worstLevelSlot = -1;

	// Age against the newest frame in the reports, not the local frame, so every
	// client reaches the same result.
	int newestReportFrame = 0;
	for (char i = 0; i < (char)std::size(PlayerLastTimingFrame); ++i)
	{
		if (PlayerLastTimingFrame[i] > newestReportFrame)
			newestReportFrame = PlayerLastTimingFrame[i];
	}

	for (char i = 0; i < (char)std::size(PlayerMaxAheads); ++i)
	{
		if (newestReportFrame >= (PlayerLastTimingFrame[i] + (ProtocolZero::SendResponseTimeFrame / 2)))
		{
			PlayerMaxAheads[i] = 0;
			PlayerLatencyMode[i] = 0;
		}
		else
		{
			if (PlayerMaxAheads[i] > maxMaxAheads)
			{
				maxMaxAheads = PlayerMaxAheads[i];
				worstRttSlot = i;
			}
			if (PlayerLatencyMode[i] > setLatencyMode)
			{
				setLatencyMode = PlayerLatencyMode[i];
				worstLevelSlot = i;
			}
		}
	}

	ProtocolZero::WorstMaxAhead = maxMaxAheads;

	// maxMaxAheads is the worst reported response time plus one; back that out so
	// the descent gate sees the measurement itself.
	LatencyLevel::Update(static_cast<LatencyLevelEnum>(setLatencyMode), maxMaxAheads > 0 ? maxMaxAheads - 1 : 0,
		worstRttSlot, worstLevelSlot, newestReportFrame);
}
