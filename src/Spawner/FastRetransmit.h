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
*  FastRetransmit - per-connection retransmit timer.
*
*  The engine uses one RetryDelta for every connection, derived from the worst
*  peer's response time including retransmit delays. This keeps a round trip
*  estimate per connection (Karn's algorithm) and retransmits each packet on
*  its own peer's timer. Connections without an estimate keep RetryDelta.
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

	// Per-packet retransmit backoff. Only applies while FastRetransmit is enabled.
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

	// sendCount is how many times the acknowledged entry was sent.
	static void SampleRTT(const ConnectionClass* connection, int delayTicks, int sendCount);

	// Backs off the timer for future packets after a retransmission, once per round.
	static void NoteRetransmit(const ConnectionClass* connection, int capturedTicks, int nowTicks);

	// Retransmit timeout in ticks, or 0 to use the engine's RetryDelta.
	static int RetryTicks(const ConnectionClass* connection);

	// The timeout for one queued packet, or 0 to use the engine's RetryDelta.
	static int PacketRetryTicks(const ConnectionClass* connection, const SendQueueType* entry);
	static void NoteSend(const ConnectionClass* connection, const SendQueueType* entry, int nowTicks);

	struct PacketStats
	{
		int BaseTicks;
		int DelayTicks;
	};
	static bool GetPacketStats(const ConnectionClass* connection, const SendQueueType* entry, PacketStats& out);

	// Worst delivery delay across peers in ticks, or -1 if none. Measures the same
	// thing as the engine's Avg_Response_Time but converges much faster. Drives
	// the latency level.
	static int WorstDeliveryDelay();

	// Worst Karn-filtered round trip across peers in ticks, or -1. Diagnostics only.
	static int WorstSmoothedRTT();

	// Diagnostics for the RetryDelta ceiling.
	static void GetCapStats(int& caps, int& worstOvershoot, int& lastUncapped);

	// Diagnostics.
	static int ActivePeers();
	static int InitializedPeers();
	static int CleanSamples();
};
