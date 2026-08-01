/**
*  yrpp-spawner
*
*  FastRetransmit - RTT-adaptive retransmit timer (FIX #2).
*
*  Vanilla RA2 resends an unacked packet only after RetryDelta ms with no
*  backoff and no fast path. With ProtocolZero the periodic timing update sets
*  RetryDelta from WorstMaxAhead+10 ticks (up to ~46 ticks = ~740 ms), so a lost
*  command sits undelivered for the better part of a second before the first
*  resend. Measured on real 10% games: retransmit waits had a ~281 ms median and
*  a 1750 ms max, and that latency is the dominant cost left after the
*  frame-aware gate (fix1) runs the runway out.
*
*  This retargets RetryDelta at the actual measured round-trip
*  (IPXManagerClass::Response_Time, the max per-peer Avg_Response_Time, in
*  16 ms ticks) plus a small jitter margin, so a resend fires roughly one RTT
*  after the packet instead of waiting out the inflated timer. It is a pure
*  transport-timing change: no wire-format change, no new packet type, no effect
*  on which events execute or when they execute - so it carries no determinism
*  risk and interoperates with unpatched clients.
*
*  A dropped-but-not-lost packet whose ACK is merely slow may be resent once
*  unnecessarily; the receiver's ARQ already discards the duplicate for free, so
*  the only cost is a few spare bytes on a small stream.
*
*  Instrumented address (verified in gamemd_rest.i64):
*    0x540C60  IPXManagerClass::Set_Timing1 - overrides the retrydelta argument
*/

#pragma once

class FastRetransmit
{
public:
	// Master switch (set from [Settings] FastRetransmit, default on).
	static bool Enabled;

	// Extra ticks added to the measured RTT when arming the resend timer
	// (1 tick = 16 ms). Absorbs normal jitter without waiting out the old timer.
	static const int MarginTicks = 2;
	// Never arm the timer shorter than this (guards the near-zero-RTT/LAN case).
	static const int MinTicks = 2;

	// Clean RTT tracking (Karn's algorithm - first-try ACKs only), in ticks.
	static int  SmoothedRTT();
	static void SampleRTT(int delayTicks, int sendCount);

	// Gentle retransmit backoff. Karn's RTT filtering (above) can leave the
	// timer stuck too short after a sustained latency rise, causing a spurious
	// retransmit storm; a growing timeout is the standard cure. We keep it gentle
	// and speed-safe: the FIRST retransmit still fires at the normal fast timer,
	// and only repeated failures of the same packet grow the interval by +50%
	// each, capped - so normal recovery is unaffected and only a genuinely bad
	// link is throttled.
	static bool Backoff;
	static const int BackoffStepHalves = 1; // +1/2 of base per prior retry
	static const int BackoffCap = 4;        // max prior-retries counted (=> up to 3x base)
};
