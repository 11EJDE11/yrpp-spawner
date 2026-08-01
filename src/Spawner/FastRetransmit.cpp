/**
*  yrpp-spawner
*
*  FastRetransmit - RTT-adaptive retransmit timer. See FastRetransmit.h.
*/

#include "FastRetransmit.h"
#include "NetTelemetry.h"

#include <Helpers/Macro.h>       // DEFINE_HOOK, REGISTERS
#include <SessionClass.h>        // GameMode guard
#include <GeneralDefinitions.h>  // GameMode
#include <IPXManagerClass.h>     // Instance.NumConnections

bool FastRetransmit::Enabled = true;
bool FastRetransmit::Backoff = true;

namespace
{
	// Clean smoothed RTT in 16 ms ticks, 0 = no sample yet.
	//
	// We must NOT use the engine's Avg_Response_Time / IPXManagerClass::
	// Response_Time here: those average in the round-trip of *retransmitted*
	// packets, whose measured delay includes the (old, large) retry wait. Under
	// loss that feeds back into itself and reports RTTs of 25-60+ ticks when the
	// real network RTT is a handful of ticks - so a timer built on it barely
	// shrinks. Instead we track RTT with Karn's algorithm: sample only packets
	// that were ACK'd on their first send (SendCount <= 1), never resends.
	int  g_srtt = 0;

	inline bool InNetGame()
	{
		const GameMode gm = SessionClass::Instance.GameMode;
		return (gm == GameMode::LAN || gm == GameMode::Internet)
		    && IPXManagerClass::Instance.NumConnections >= 1;
	}
}

// static
int FastRetransmit::SmoothedRTT() { return g_srtt; }

// static
void FastRetransmit::SampleRTT(int delayTicks, int sendCount)
{
	if (!Enabled)
		return;
	if (sendCount > 1)          // Karn: ignore retransmitted packets
		return;
	if (delayTicks < 0 || delayTicks > 250)  // drop absurd / clock-glitch samples
		return;

	if (g_srtt == 0)
		g_srtt = delayTicks;
	else
		g_srtt = (g_srtt * 7 + delayTicks) / 8;   // EWMA, alpha = 1/8
}

// ConnectionClass::Service_Send_Queue, at the point an ACK'd PACKET_DATA_ACK
// entry's round-trip is about to be folded into the queue's response time.
// EBX = send_entry (SendQueueType: FirstTime@+4, SendCount@+0x0C),
// EBP = current tick count (Accumulated). delay = Accumulated - FirstTime.
// Observe-only (returns 0); the stolen bytes then compute the same delay for
// the engine's own Add_Delay.
DEFINE_HOOK(0x48C436, ServiceSendQueue_RTTSample_FastRetransmit, 0x8)
{
	if (FastRetransmit::Enabled)
	{
		const unsigned char* entry = reinterpret_cast<const unsigned char*>(R->EBX());
		int firstTime = *reinterpret_cast<const int*>(entry + 4);
		int sendCount = *reinterpret_cast<const int*>(entry + 0x0C);
		int delay = static_cast<int>(R->EBP()) - firstTime;
		FastRetransmit::SampleRTT(delay, sendCount);
	}
	return 0;
}

// IPXManagerClass::Set_Timing1 entry. thiscall; at 0x540C60 (before any push)
// the arguments sit at [esp+4]=retrydelta, [esp+8]=maxretries, [esp+C]=timeout.
// We overwrite retrydelta in place from our clean RTT, then let the stolen
// 'mov eax,[esp+4]' read our value. Only shortens, never lengthens; only in a
// live multiplayer game; only once we have a real RTT sample.
DEFINE_HOOK(0x540C60, IPXSetTiming_FastRetransmit, 0x8)
{
	if (!FastRetransmit::Enabled || !InNetGame())
		return 0;

	int rtt = FastRetransmit::SmoothedRTT();
	if (rtt <= 0)
		return 0; // no clean sample yet - leave the engine's value alone

	int retryDelta = rtt + FastRetransmit::MarginTicks;
	if (retryDelta < FastRetransmit::MinTicks)
		retryDelta = FastRetransmit::MinTicks;

	int original = R->Stack<int>(0x4);
	if (retryDelta < original) // only ever shorten the wait
	{
		R->Stack<int>(0x4, retryDelta);

		static int lastLogged = -1;
		if (retryDelta != lastLogged)
		{
			lastLogged = retryDelta;
			NetTelemetry::Log("FastRetransmit RetryDelta %d -> %d ticks (cleanRTT=%d)", original, retryDelta, rtt);
		}
	}

	return 0;
}

// ConnectionClass::Service_Send_Queue, at the per-entry retransmit decision.
// Stolen: 'mov eax,[edi+28h]' (eax = this->RetryDelta) + 'mov edx,ebp' (edx =
// current tick). We scale the effective RetryDelta up by the entry's prior
// retransmit count, then hand control to 'sub edx,ecx; cmp edx,eax; jbe' with
// eax = backed-off delay. EDI = this (RetryDelta @+0x28), ESI = send_entry
// (SendCount @+0x0C), EBP = current tick, ECX = entry->LastTime (already loaded).
DEFINE_HOOK(0x48C4AE, ServiceSendQueue_Backoff_FastRetransmit, 0x5)
{
	if (!FastRetransmit::Backoff)
		return 0; // vanilla: run the stolen movs unchanged

	const int retryDelta = *reinterpret_cast<const int*>(R->EDI() + 0x28);
	const int sendCount  = *reinterpret_cast<const int*>(R->ESI() + 0x0C);

	int prior = sendCount - 1;          // retransmits already made for this entry
	if (prior < 0) prior = 0;
	if (prior > FastRetransmit::BackoffCap) prior = FastRetransmit::BackoffCap;

	// effective = base * (1 + prior/2): 1x, 1.5x, 2x, ... capped. First retry (prior 0) is unchanged.
	const int eff = retryDelta + (retryDelta * prior * FastRetransmit::BackoffStepHalves) / 2;

	R->EAX(eff);
	R->EDX(R->EBP()); // replicate the stolen 'mov edx,ebp'
	return 0x48C4B3;  // continue at 'sub edx,ecx'
}
