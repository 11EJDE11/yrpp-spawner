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
*  NetDiagnostics - test instrumentation for the retransmit/redundancy work.
*
*  Emits a periodic "[NetDiag]" heartbeat to debug.log with, per peer, both the
*  engine's own (retransmit-poisoned) response time and our clean estimate, plus
*  retransmit, duplicate and stall counters. Every line carries the game frame,
*  which is the only clock shared by all players, so logs from several machines
*  can be aligned against each other.
*
*  This is diagnostic scaffolding, not a feature. Turn NetDiagnostics off (or
*  drop this file) before merging.
*/

#pragma once

class ConnectionClass;

class NetDiagnostics
{
public:
	static bool Enabled;

	// Heartbeat period. Wall clock rather than frames, because frames stop
	// advancing during exactly the stalls we want to observe.
	static const int PeriodMs = 1000;

	static void Reset();
	static void NoteWait(int commandPeer, int minimumFrame, int maxAhead);
	static void EndWait();

	// Called from IPXManagerClass::Service, which runs both once per frame and
	// on every iteration of the Wait_For_Players stall spin.
	static void Tick();

	// Records one received unreliable (PACKET_DATA_NOACK) packet. Gaps in the
	// per-connection PacketID sequence are apparent gaps, not confirmed loss:
	// reordered packets may fill them later. Reliable packets also carry frame
	// progress, so these gaps alone cannot establish the cause of a stall.
	static void NoteNoAckPacket(const ConnectionClass* connection, int packetId);

	// Longest run worth logging individually, so runs can be correlated with stalls.
	static const int InterestingRun = 4;

	// One-line event records, for things too rare to need rate limiting.
	static void LogEvent(const ConnectionClass* connection, const char* what, int from, int to);

	// Audits the one decision this PR makes about the reported response time:
	// both candidate sources, which one won, the value that reached the wire
	// after saturation, and the latency level each source would have asked for.
	// Everything needed to tell an inflated report caused by the engine's own
	// slow mean apart from one caused by this PR's estimator.
	// One engine "connection gone bad" trip, with the packet age and timeout that
	// caused it. Rate-limited internally; these can fire hundreds of times.
	static void LogBadConnection(const ConnectionClass* connection, int packetAgeTicks,
		int timeoutTicks, int sendCount, int retryDelta);

	// Cumulative totals for this client, for comparing whole runs against each
	// other. Emitted periodically and once at teardown.
	static void LogSummary(const char* reason);

	// One failed sendto, with the winsock error. Rate-limited; counted in the
	// summary. Proves whether this machine's packets ever reached the wire.
	static void LogSendFailure(int peer, int wsaError);

	static void LogResponseDecision(int engineTicks, int cleanTicks, int chosenTicks,
		int wireTicks, int engineLevel, int sentLevel, bool fastWon);
};
