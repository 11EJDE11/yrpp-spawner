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

#include "FastRetransmit.h"
#include "PacketRedundancy.h"

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>
#include <Misc/NetTelemetry.h>

#include <GeneralDefinitions.h>
#include <IPXManagerClass.h>
#include <SessionClass.h>

bool FastRetransmit::Enabled = true;
bool FastRetransmit::Backoff = true;

namespace
{
	//!< Smoothed round trip in 1/60 s ticks. 0 = no clean sample yet.
	int SmoothedTicks = 0;

	//!< SendQueueType field offsets, from ConnectionClass::Service_Send_Queue.
	constexpr int EntryFirstTime = 0x04;
	constexpr int EntrySendCount = 0x0C;

	//!< ConnectionClass::RetryDelta.
	constexpr int ConnRetryDelta = 0x28;

	bool InNetworkGame()
	{
		const GameMode mode = SessionClass::Instance.GameMode;
		return (mode == GameMode::LAN || mode == GameMode::Internet)
			&& IPXManagerClass::Instance.NumConnections >= 1;
	}
}

int FastRetransmit::SmoothedRTT()
{
	return SmoothedTicks;
}

void FastRetransmit::Reset()
{
	SmoothedTicks = 0;
}

void FastRetransmit::SampleRTT(int delayTicks, int sendCount)
{
	if (!Enabled)
		return;

	// Karn: a retransmitted packet's measured delay includes the retry wait, so
	// folding it in would inflate the estimate that set that wait in the first
	// place. Discard it -- but it is still the cleanest evidence we get that this
	// link is losing packets, so pass it on.
	if (sendCount > 1)
	{
		PacketRedundancy::NoteResend();
		return;
	}

	// Guard against clock glitches and absurd outliers.
	if (delayTicks < 0 || delayTicks > 250)
		return;

	if (SmoothedTicks == 0)
		SmoothedTicks = delayTicks;
	else
		SmoothedTicks = (SmoothedTicks * 7 + delayTicks) / 8;   // EWMA, alpha 1/8
}

/**
 *  ConnectionClass::Service_Send_Queue, where an ACKed entry's round trip is
 *  about to be folded into the queue's response time.
 *
 *  0x48C436  8b 4b 04   mov ecx, [ebx+4]    ; entry->FirstTime
 *  0x48C439  2b e9      sub ebp, ecx        ; ebp = now - FirstTime
 *  0x48C43B  8b 4f 04   mov ecx, [edi+4]
 *
 *  EBX = the send-queue entry, EBP = the current tick count -- still the raw
 *  value, because the `sub` above has not run yet. Observe-only.
 */
DEFINE_HOOK(0x48C436, ServiceSendQueue_FastRetransmit_Sample, 0x8)
{
	if (FastRetransmit::Enabled)
	{
		const unsigned char* entry = reinterpret_cast<const unsigned char*>(R->EBX());
		const int firstTime = *reinterpret_cast<const int*>(entry + EntryFirstTime);
		const int sendCount = *reinterpret_cast<const int*>(entry + EntrySendCount);

		FastRetransmit::SampleRTT((int)R->EBP() - firstTime, sendCount);
	}

	return 0;
}

/**
 *  IPXManagerClass::Set_Timing entry, before any push, so the arguments are at
 *  [esp+4] retrydelta, [esp+8] maxretries, [esp+C] timeout.
 *
 *  0x540C60  8b 44 24 04   mov eax, [esp+4]
 *  0x540C64  8b 54 24 0c   mov edx, [esp+0Ch]
 *
 *  We rewrite the argument in place and let the stolen load pick up our value.
 *  Only ever shortens, only in a live network game, and only once a clean sample
 *  exists -- otherwise the engine's own figure stands.
 */
DEFINE_HOOK(0x540C60, IPXManagerSetTiming_FastRetransmit, 0x8)
{
	if (!FastRetransmit::Enabled || !InNetworkGame())
		return 0;

	const int rtt = FastRetransmit::SmoothedRTT();
	if (rtt <= 0)
		return 0;

	int retryDelta = rtt + FastRetransmit::MarginTicks;
	if (retryDelta < FastRetransmit::MinTicks)
		retryDelta = FastRetransmit::MinTicks;

	const int original = R->Stack<int>(0x4);
	if (retryDelta < original)
	{
		R->Stack<int>(0x4, retryDelta);

		static int lastLogged = -1;
		if (retryDelta != lastLogged)
		{
			lastLogged = retryDelta;
			NetTelemetry::Log("[FastRetransmit] RetryDelta %d -> %d ticks (clean RTT %d,"
				" engine's polluted estimate would have kept %d)\n",
				original, retryDelta, rtt, original);
		}
	}

	return 0;
}

/**
 *  ConnectionClass::Service_Send_Queue, at the per-entry retransmit decision.
 *
 *  0x48C4AE  8b 47 28   mov eax, [edi+28h]   ; this->RetryDelta
 *  0x48C4B1  8b d5      mov edx, ebp         ; edx = now
 *
 *  Continues at 0x48C4B3 `sub edx, ecx` / `cmp edx, eax` / `jbe`, with ECX
 *  already holding entry->LastTime. We hand back a scaled RetryDelta in EAX and
 *  replicate the stolen `mov edx, ebp`.
 *
 *  EDI = the connection, ESI = the send-queue entry.
 */
DEFINE_HOOK(0x48C4AE, ServiceSendQueue_FastRetransmit_Backoff, 0x5)
{
	if (!FastRetransmit::Backoff)
		return 0;

	const int retryDelta = *reinterpret_cast<const int*>(R->EDI() + ConnRetryDelta);
	const int sendCount = *reinterpret_cast<const int*>(R->ESI() + EntrySendCount);

	int prior = sendCount - 1;              // attempts already made for this entry
	if (prior < 0)
		prior = 0;
	if (prior > FastRetransmit::BackoffCap)
		prior = FastRetransmit::BackoffCap;

	// 1x on the first retry, then 1.5x, 2x, ... capped. Aggressive when a single
	// packet was dropped, patient when the link is genuinely congested.
	const int effective = retryDelta
		+ (retryDelta * prior * FastRetransmit::BackoffStepHalves) / 2;

	R->EAX(effective);
	R->EDX(R->EBP());

	return 0x48C4B3;
}
