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
*  NetDiagnostics - network logging to debug.log: stalls, silences, loss and
*  periodic totals. Lines carry the game frame so logs from several machines
*  can be lined up.
*/

#pragma once

class ConnectionClass;

class NetDiagnostics
{
public:
	static bool Enabled;

	// Wall clock, because frames stop advancing during stalls.
	static const int PeriodMs = 1000;

	static void Reset();
	static void NoteWait(int commandPeer, int minimumFrame, int maxAhead);
	static void EndWait();

	// Runs every frame and during the Wait_For_Players stall loop.
	static void Tick();

	// Records one received unreliable packet. Gaps in the sequence may be reordering.
	static void NoteNoAckPacket(const ConnectionClass* connection, int packetId);

	// Longest loss run worth logging individually.
	static const int InterestingRun = 4;

	// One-line event records.
	static void LogEvent(const ConnectionClass* connection, const char* what, int from, int to);

	// How the reported response time was chosen: both sources, the result and the
	// saturated value sent.
	static void LogBadConnection(const ConnectionClass* connection, int packetAgeTicks,
		int timeoutTicks, int sendCount, int retryDelta);

	// Totals for this client, logged periodically and at teardown.
	static void LogSummary(const char* reason);

	// One failed sendto with the winsock error. Rate-limited.
	static void LogSendFailure(int peer, int wsaError);

	static void LogResponseDecision(int engineTicks, int cleanTicks, int chosenTicks,
		int wireTicks, int engineLevel, int sentLevel, bool fastWon);
};
