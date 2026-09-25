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

#include <HouseClass.h>
#include <MessageListClass.h>
#include <Utilities/Debug.h>
#include <Unsorted.h>

LatencyLevelEnum LatencyLevel::CurentLatencyLevel = LatencyLevelEnum::LATENCY_LEVEL_INITIAL;
unsigned char LatencyLevel::NewFrameSendRate = 3;
bool LatencyLevel::AllowDescent = false;
int LatencyLevel::TargetMaxAhead = 0;

namespace
{
	int  g_lastChangeFrame = 0;
	int  g_lastEvaluationFrame = 0;
	bool g_hasEvaluated = false;
	bool g_hasChanged = false;
	int  g_goodEvaluations = 0;
	bool g_descentStreak = false;

	// Flap memory: the level a descent was pushed back off, and until when it
	// stays refused.
	int  g_blockedBelow = 0;
	int  g_blockedUntilFrame = 0;
	int  g_lastDescentFrame = 0;
	int  g_lastDescentFrom = 0;
	int  g_lastRaiseFrame = 0;

	int  g_reportRtt = 0;
	int  g_reportRttSlot = -1;
	int  g_reportLevel = 0;
	int  g_reportLevelSlot = -1;
	int  g_upOnlyLevel = 0;
}

void LatencyLevel::Apply(LatencyLevelEnum newLatencyLevel, int eventFrame)
{
	if (newLatencyLevel > LatencyLevelEnum::LATENCY_LEVEL_MAX)
		newLatencyLevel = LatencyLevelEnum::LATENCY_LEVEL_MAX;

	auto maxLatencyLevel = static_cast<LatencyLevelEnum>(ProtocolZero::MaxLatencyLevel);
	if (newLatencyLevel > maxLatencyLevel)
		newLatencyLevel = maxLatencyLevel;

	if (newLatencyLevel <= CurentLatencyLevel)
		return;

	Debug::Log("Player %ls, Loss mode (%d, %d) Frame = %d\n"
		, HouseClass::CurrentPlayer->UIName
		, newLatencyLevel
		, CurentLatencyLevel
		, (int)Unsorted::CurrentFrame
	);

	Commit(newLatencyLevel, eventFrame);
}

// Applies one agreed level. Two engine constraints govern what may be applied:
// commands are stamped at roundup(Frame + MaxAhead, FrameSendRate) and protocol
// 2 only executes on frames divisible by the rate (0x647F36), so MaxAhead must
// stay an exact multiple of FrameSendRate; and a decrease needs the timing
// window at 0x4C8033 to reschedule what is already queued.
void LatencyLevel::Commit(LatencyLevelEnum newLatencyLevel, int eventFrame)
{
	const int previousLevel = (int)CurentLatencyLevel;
	const int previousTarget = TargetMaxAhead;

	CurentLatencyLevel = newLatencyLevel;
	Game::Network::PreCalcFrameRate = 60;

	// The rate follows the level in both directions. The vanilla ladder is then
	// self-consistent at every rung - MaxAhead 4/6/12/16/20/24/28/32/36 is an
	// exact multiple of its own level - which is the invariant the engine needs.
	NewFrameSendRate = static_cast<unsigned char>(newLatencyLevel);

	const int rate = NewFrameSendRate < 1 ? 1 : (int)NewFrameSendRate;
	int target = GetMaxAhead(newLatencyLevel);

	// Westwood's documented floor, from the protocol comment in queue.cpp: the
	// minimum MaxAhead is "n * 2, to give both sides some breathing room in case
	// a FRAMEINFO packet gets missed". The ladder already clears it at every
	// level; this guards the arithmetic, not the table.
	if (target < 2 * rate)
		target = 2 * rate;

	if ((target % rate) != 0 || target < 2 * rate)
	{
		Debug::Log("[Audit] INVARIANT Commit produced maxahead=%d fsr=%d for level %d (divides=%d floor_ok=%d) - this pair must never be sent\n"
			, target, rate, (int)newLatencyLevel
			, (int)((target % rate) == 0), (int)(target >= 2 * rate));
	}

	TargetMaxAhead = target;
	Game::Network::PreCalcMaxAhead = target;

	Debug::Log("[Audit] latency frame=%d (event) lvl %d->%d | target maxahead %d->%d | engine maxahead=%d fsr=%d | multiple=%d | worst rtt=%d slot=%d, worst lvl=%d slot=%d | up-only lvl=%d maxahead=%d\n"
		, eventFrame
		, previousLevel, (int)newLatencyLevel
		, previousTarget, target
		, (int)Game::Network::MaxAhead, rate
		, (target % rate == 0)
		, g_reportRtt, g_reportRttSlot, g_reportLevel, g_reportLevelSlot
		, g_upOnlyLevel, GetMaxAhead((LatencyLevelEnum)g_upOnlyLevel)
	);

	if ((int)newLatencyLevel > previousLevel)
	{
		g_lastRaiseFrame = eventFrame;

		// Undoing a descent this quickly means the level we dropped to was not
		// actually supportable. Refuse it for a while rather than trying again
		// on the same evidence.
		if (g_lastDescentFrame && (eventFrame - g_lastDescentFrame) <= ReversalWindowFrames)
		{
			g_blockedBelow = g_lastDescentFrom;
			g_blockedUntilFrame = eventFrame + FlapCooldownFrames;
			Debug::Log("[Audit] latency frame=%d descent to %d reversed after %d frames - refusing below %d until %d\n"
				, eventFrame, (int)CurentLatencyLevel, eventFrame - g_lastDescentFrame
				, g_blockedBelow, g_blockedUntilFrame);
		}
	}
	else if ((int)newLatencyLevel < previousLevel)
	{
		g_lastDescentFrame = eventFrame;
		g_lastDescentFrom = previousLevel;
	}

	g_lastChangeFrame = eventFrame;
	g_hasChanged = true;

	MessageListClass::Instance.PrintMessage(GetLatencyMessage(newLatencyLevel), (int)(RulesClass::Instance->MessageDelay * 900), ColorScheme::White, true);
}

void LatencyLevel::ResetDescent()
{
	g_goodEvaluations = 0;
	g_descentStreak = false;
}

void LatencyLevel::Update(LatencyLevelEnum desired, int worstResponseTime,
	int worstRttSlot, int worstLevelSlot, int eventFrame)
{
	g_reportRtt = worstResponseTime;
	g_reportRttSlot = worstRttSlot;
	g_reportLevel = (int)desired;
	g_reportLevelSlot = worstLevelSlot;

	// Vanilla keeps the highest level ever requested, capped like Apply caps it.
	int upOnly = (int)desired;
	if (upOnly > (int)LatencyLevelEnum::LATENCY_LEVEL_MAX)
		upOnly = (int)LatencyLevelEnum::LATENCY_LEVEL_MAX;
	if (upOnly > (int)ProtocolZero::MaxLatencyLevel)
		upOnly = (int)ProtocolZero::MaxLatencyLevel;
	if (upOnly > g_upOnlyLevel)
		g_upOnlyLevel = upOnly;

	if (desired > CurentLatencyLevel)
	{
		// Worsening is never gated. Under-provisioning stalls everyone.
		Apply(desired, eventFrame);
		ResetDescent();
		return;
	}

	if (!AllowDescent || desired >= CurentLatencyLevel || CurentLatencyLevel <= LatencyLevelEnum::LATENCY_LEVEL_1)
	{
		ResetDescent();
		return;
	}

	// The clock is the event stream, never Unsorted::CurrentFrame. This handler
	// runs when a ResponseTime2 event executes, and a late one is deliberately
	// let through by the hook at 0x64C598 rather than rejected - so the local
	// frame at which it runs differs between machines. Gating on that frame is
	// what let one machine take a descent step the others never took.
	const int frame = eventFrame;
	if (g_hasEvaluated && (frame - g_lastEvaluationFrame) < EvaluationIntervalFrames)
		return;

	g_hasEvaluated = true;
	g_lastEvaluationFrame = frame;

	// The improvement must still hold with the measurement inflated, so a
	// marginal reading never triggers a step. This is what stops oscillation.
	int inflated = worstResponseTime <= 0 ? 0 : (worstResponseTime * HeadroomNumerator) / HeadroomDenominator;
	if (inflated > 255)
		inflated = 255;

	const auto headroomLevel = FromResponseTime(static_cast<uint8_t>(inflated));
	if (headroomLevel >= CurentLatencyLevel)
	{
		ResetDescent();
		return;
	}

	// A rung that was just pushed back up stays refused for a while.
	if (frame < g_blockedUntilFrame && (int)CurentLatencyLevel <= g_blockedBelow)
		return;

	// A raise blocks the next descent for longer than an ordinary change.
	if (g_lastRaiseFrame && (frame - g_lastRaiseFrame) < RaiseCooldownFrames)
		return;

	// A streak that is already descending keeps going; the first step waits for
	// the cooldown to expire.
	if (g_hasChanged && !g_descentStreak && (frame - g_lastChangeFrame) < ChangeCooldownFrames)
		return;

	++g_goodEvaluations;
	if (g_goodEvaluations < (g_descentStreak ? 1 : GoodEvaluationsRequired))
		return;

	// One rung per evaluation.
	//
	// Descending straight to the level the measurement supports is safe - the
	// hook at 0x4C8033 repairs a 9->1 transition exactly as it does 9->8 - but
	// it is not stable. A full-depth drop lands at the bottom of the ladder on a
	// single good measurement and is then pulled straight back: measured as
	// 3->1 reversed 238 frames later, and again 3->1 reversed after 641. Each
	// reversal is a visible MaxAhead swing and a message to every player.
	//
	// Stepping one rung costs descent speed - a 9->1 recovery walks down over
	// several evaluation intervals rather than one - and buys far fewer
	// reversals. If the slow descent matters more than the churn, the middle
	// option is a bounded step (two or three rungs) rather than either extreme.
	int target = static_cast<int>(CurentLatencyLevel) - 1;
	if (target < static_cast<int>(headroomLevel))
		target = static_cast<int>(headroomLevel);
	if (target < static_cast<int>(LatencyLevelEnum::LATENCY_LEVEL_1))
		target = static_cast<int>(LatencyLevelEnum::LATENCY_LEVEL_1);

	const auto next = static_cast<LatencyLevelEnum>(target);

	Debug::Log("Player %ls, Latency descent %d -> %d (desired %d, headroom lvl %d from %d, rtt %d) Frame = %d\n"
		, HouseClass::CurrentPlayer->UIName
		, (int)CurentLatencyLevel
		, (int)next
		, (int)desired
		, (int)headroomLevel
		, inflated
		, worstResponseTime
		, frame
	);

	Commit(next, eventFrame);
	g_descentStreak = true;
	g_goodEvaluations = 0;
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
