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
#include "FrameGate.h"

#include <HouseClass.h>
#include <MessageListClass.h>
#include <Utilities/Debug.h>
#include <Unsorted.h>

#include <limits>

LatencyLevelEnum LatencyLevel::CurentLatencyLevel = LatencyLevelEnum::LATENCY_LEVEL_INITIAL;
unsigned char LatencyLevel::NewFrameSendRate = 3;
bool LatencyLevel::Bidirectional = true;

namespace
{
	//!< Wait this long after raising the level before considering a drop, so a
	//!< burst of spikes does not oscillate the whole lobby's input latency.
	constexpr int DownshiftCooldownFrames = 60 * 10;

	//!< ...and then require conditions to stay good for this long before acting.
	constexpr int DownshiftConfirmFrames = 60 * 6;

	int LastUpshiftFrame = (std::numeric_limits<int>::min)();
	int PendingDownshiftFrame = -1;
	LatencyLevelEnum PendingDownshiftLevel = LatencyLevelEnum::LATENCY_LEVEL_INITIAL;

	void ApplyInternal(LatencyLevelEnum newLatencyLevel, const char* pLogTag)
	{
		Debug::Log("Player %ls, %s (%d, %d) Frame = %d\n"
			, HouseClass::CurrentPlayer->UIName
			, pLogTag
			, newLatencyLevel
			, LatencyLevel::CurentLatencyLevel
			, (int)Unsorted::CurrentFrame
		);

		LatencyLevel::CurentLatencyLevel = newLatencyLevel;
		LatencyLevel::NewFrameSendRate = static_cast<unsigned char>(newLatencyLevel);
		Game::Network::PreCalcFrameRate = 60;
		Game::Network::PreCalcMaxAhead = LatencyLevel::GetMaxAhead(newLatencyLevel);

		MessageListClass::Instance.PrintMessage(
			LatencyLevel::GetLatencyMessage(newLatencyLevel),
			(int)(RulesClass::Instance->MessageDelay * 900),
			ColorScheme::White,
			true
		);
	}
}

void LatencyLevel::Reset()
{
	CurentLatencyLevel = LatencyLevelEnum::LATENCY_LEVEL_INITIAL;
	NewFrameSendRate = 3;

	LastUpshiftFrame = (std::numeric_limits<int>::min)();
	PendingDownshiftFrame = -1;
	PendingDownshiftLevel = LatencyLevelEnum::LATENCY_LEVEL_INITIAL;
}

/**
 *  The ladder used to be a one-way ratchet: `if (new <= current) return;`. One
 *  transient spike pinned a match at MaxAhead 36 -- roughly 600ms of input
 *  latency -- for the rest of the game, long after the network recovered.
 *
 *  Raising is still immediate, because being short of runway costs everyone a
 *  stall. Lowering is deliberately slow: a cooldown after the last raise, then a
 *  sustained-good-conditions confirmation, then one rung at a time.
 */
void LatencyLevel::Apply(LatencyLevelEnum newLatencyLevel)
{
	if (newLatencyLevel > LatencyLevelEnum::LATENCY_LEVEL_MAX)
		newLatencyLevel = LatencyLevelEnum::LATENCY_LEVEL_MAX;

	auto maxLatencyLevel = static_cast<LatencyLevelEnum>(ProtocolZero::MaxLatencyLevel);
	if (newLatencyLevel > maxLatencyLevel)
		newLatencyLevel = maxLatencyLevel;

	if (newLatencyLevel == CurentLatencyLevel)
		return;

	// Vanilla behaviour: only ever climb. One transient spike then pins the match
	// at MaxAhead 36 for the rest of the game.
	if (!Bidirectional)
	{
		if (newLatencyLevel < CurentLatencyLevel)
			return;

		ApplyInternal(newLatencyLevel, "Loss mode");
		return;
	}

	const int currentFrame = (int)Unsorted::CurrentFrame;

	if (newLatencyLevel > CurentLatencyLevel)
	{
		PendingDownshiftFrame = -1;
		PendingDownshiftLevel = LatencyLevelEnum::LATENCY_LEVEL_INITIAL;
		LastUpshiftFrame = currentFrame;
		ApplyInternal(newLatencyLevel, "Loss mode");
		return;
	}

	if (currentFrame < LastUpshiftFrame + DownshiftCooldownFrames)
		return;

	if (PendingDownshiftFrame < 0 || PendingDownshiftLevel != newLatencyLevel)
	{
		PendingDownshiftFrame = currentFrame;
		PendingDownshiftLevel = newLatencyLevel;
		return;
	}

	if (currentFrame - PendingDownshiftFrame < DownshiftConfirmFrames)
		return;

	// One rung per confirmation, so recovery is gradual and re-tests conditions
	// at each step instead of dropping straight back to the bottom.
	const auto stepLevel = static_cast<LatencyLevelEnum>(
		static_cast<uint8_t>(CurentLatencyLevel) - 1);

	PendingDownshiftFrame = -1;
	PendingDownshiftLevel = LatencyLevelEnum::LATENCY_LEVEL_INITIAL;
	ApplyInternal(stepLevel, "Recovery mode");
}

int LatencyLevel::GetMaxAhead(LatencyLevelEnum latencyLevel)
{
	static const int maxAhead[] =
	{
		/* 0 */ 1

		/* 1 */ ,4
		/* 2 */ ,6
		/* 3 */ ,12
		/* 4 */ ,16
		/* 5 */ ,20
		/* 6 */ ,24
		/* 7 */ ,28
		/* 8 */ ,32
		/* 9 */ ,36
	};

	return maxAhead[(int)latencyLevel];
}

const wchar_t* LatencyLevel::GetLatencyMessage(LatencyLevelEnum latencyLevel)
{
	static const wchar_t* message[] =
	{
		/* 0 */ L"CnCNet: Latency mode set to: 0 - Initial" // Players should never see this, if it doesn't then it's a bug

		/* 1 */ ,L"CnCNet: Latency mode set to: 1 - Best"
		/* 2 */ ,L"CnCNet: Latency mode set to: 2 - Super"
		/* 3 */ ,L"CnCNet: Latency mode set to: 3 - Excellent"
		/* 4 */ ,L"CnCNet: Latency mode set to: 4 - Very Good"
		/* 5 */ ,L"CnCNet: Latency mode set to: 5 - Good"
		/* 6 */ ,L"CnCNet: Latency mode set to: 6 - Good"
		/* 7 */ ,L"CnCNet: Latency mode set to: 7 - Default"
		/* 8 */ ,L"CnCNet: Latency mode set to: 8 - Default"
		/* 9 */ ,L"CnCNet: Latency mode set to: 9 - Default"
	};

	return message[(int)latencyLevel];
}

LatencyLevelEnum LatencyLevel::FromResponseTime(unsigned char rspTime)
{
	for (auto i = LatencyLevelEnum::LATENCY_LEVEL_1; i < LatencyLevelEnum::LATENCY_LEVEL_MAX; i = static_cast<LatencyLevelEnum>(1 + static_cast<char>(i)))
	{
		if (rspTime <= GetMaxAhead(i))
			return static_cast<LatencyLevelEnum>(i);
	}

	return LatencyLevelEnum::LATENCY_LEVEL_MAX;
}

LatencyLevelEnum LatencyLevel::FromConditions(unsigned char rspTime, bool needRetransmitCover)
{
	int needed = rspTime;

	// Extra runway only helps if something actually uses it. Without the
	// frame-aware gate the engine stalls the instant a peer has anything
	// outstanding, no matter how far ahead it was stamped -- so a bigger MaxAhead
	// would buy nothing for loss and cost everyone input latency. Only pay for
	// retransmit cover when the gate is there to spend it.
	if (needRetransmitCover && FrameGate::Enable)
	{
		// One retransmit round trip: the sender waits RetryDelta (= RTT + 10ms,
		// recomputed at 0x6476D0) and the resend then takes about half an RTT.
		// rspTime is already in frames, so this is 1.5x plus a frame of slack.
		needed += (rspTime * 3) / 2 + 1;
	}

	if (needed > 255)
		needed = 255;

	return FromResponseTime(static_cast<unsigned char>(needed));
}
