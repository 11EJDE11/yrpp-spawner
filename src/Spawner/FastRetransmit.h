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

/**
*  FastRetransmit - per-connection RTT-adaptive retransmit timer.
*
*  The engine recomputes one global RetryDelta as Response_Time() + 10 every
*  128 frames, where Response_Time() is the maximum over connections of a
*  running mean that includes the ACK delay of retransmitted packets. Loss
*  therefore inflates the estimate that controls the next retransmit wait, and
*  the single shared timer lets the worst peer set the pace for everyone.
*
*  This keeps a per-connection estimate using Karn's algorithm and feeds it
*  into a captured timer for each packet in Service_Send_Queue, so
*  each peer retransmits on its own measured round trip. Connections without an
*  estimate - the global/lobby channels, and private peers that have not been
*  measured yet - keep the engine's own RetryDelta untouched.
*
*  Karn has two halves and both are needed:
*    1. RTT samples from retransmitted packets are ambiguous and are discarded,
*       so loss cannot inflate the estimate.
*    2. Because of (1) a link that has become slower than the estimate produces
*       no eligible samples at all, so the timeout itself is backed off on each
*       qualifying retransmission for future packets until samples flow again.
*       Already-sent packets can shorten their base on clean samples, but never
*       inherit an increased peer timeout. Without peer
*       growth the estimate latches and every new packet retries spuriously.
*
*  https://en.wikipedia.org/wiki/Karn's_algorithm
*/

#pragma once

class ConnectionClass;
struct SendQueueType;

class FastRetransmit
{
public:
	// The engine allows at most 7 private connections; one spare slot.
	static const int MaxPeers = 8;

	static bool Enabled;

	// Added to the smoothed round trip to absorb ordinary jitter.
	static const int MarginTicks = 2;
	// Never arm the timer shorter than this (guards the near-zero-RTT/LAN case).
	static const int MinTicks = 2;
	// Drop absurd / clock-glitch samples and cap computed RTOs to a sane range.
	static const int MaxTicks = 250;

	// Gentle per-packet retransmit backoff. Requires both FastRetransmit and
	// RetransmitBackoff to be enabled before it changes retransmit decisions.
	static bool Backoff;

	static const int BackoffStepHalves = 1; // +1/2 of base per prior retry
	static const int BackoffCap = 4;        // max prior-retries counted (up to 3x base)

	// Snapshot of one peer's estimator, for diagnostics.
	struct PeerStats
	{
		int Srtt;
		int RttVar;
		int Rto;
		int CleanSamples;
		int Backoffs;
		bool Provisional;
	};

	static bool GetStats(const ConnectionClass* connection, PeerStats& out);

	static void Reset();

	// Feeds one acknowledgement into that connection's estimate. sendCount is
	// the number of times the acknowledged entry was transmitted.
	static void SampleRTT(const ConnectionClass* connection, int delayTicks, int sendCount);

	// Karn's timer backoff: called when a packet sent under capturedTicks had to be
	// retransmitted. Growth applies to future packets, without changing armed
	// deadlines. `nowTicks` is the engine clock, used to limit growth to one
	// step per retransmission round.
	static void NoteRetransmit(const ConnectionClass* connection, int capturedTicks, int nowTicks);

	// This connection's measured retransmit timeout in ticks, or 0 when it has
	// no estimate and the engine's own RetryDelta should be used instead.
	static int RetryTicks(const ConnectionClass* connection);

	// Packet timers capture their base on first transmission and arm the next
	// interval after each send. Clean samples can shorten an armed interval,
	// never postpone it. A newly available estimate can adopt packets
	// already in flight, once. Zero means the engine still owns the timer.
	static int PacketRetryTicks(const ConnectionClass* connection, const SendQueueType* entry);
	static void NoteSend(const ConnectionClass* connection, const SendQueueType* entry, int nowTicks);

	struct PacketStats
	{
		int BaseTicks;
		int DelayTicks;
	};
	static bool GetPacketStats(const ConnectionClass* connection, const SendQueueType* entry, PacketStats& out);

	// Largest clean smoothed round trip across measured peers, in ticks, or -1
	// when no peer has one yet. Converges within a couple of seconds, where the
	// engine's Avg_Response_Time needs minutes.
	// Worst fast-EWMA delivery delay across measured peers, in ticks, or -1 when
	// no peer has one. Tracks the same quantity as the engine's
	// Avg_Response_Time - retransmit waiting included - but converges in about
	// ten acknowledgements instead of a few hundred. This is what drives the
	// latency level; WorstSmoothedRTT is Karn-filtered and is diagnostics only.
	static int WorstDeliveryDelay();

	static int WorstSmoothedRTT();

	// How often the vanilla ceiling bound an armed timer, the largest amount it
	// trimmed, and the last raw value it trimmed. Diagnostics only.
	static void GetCapStats(int& caps, int& worstOvershoot, int& lastUncapped);

	// Diagnostics.
	static int ActivePeers();
	static int InitializedPeers();
	static int CleanSamples();
};
