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

#pragma once

/**
 *  Measures why a multiplayer match stalls.
 *
 *  The engine already tracks everything interesting and then throws it away:
 *  SessionClass::MPStats[] carries per-peer Resends/Lost/RoundTrip and, crucially,
 *  separate FrameSyncStalls and CommandCoundStalls counters -- but nothing ever
 *  reads them. This adds the missing readout plus wall-clock stall attribution
 *  taken directly at the engine's frame-advance gate.
 *
 *  A frame may advance only when BOTH of these hold (Wait_For_Players, 0x6495C8):
 *    1. every peer satisfies their[i].Recv >= their[i].Sent
 *    2. Frame < MaxAhead + min(their[i].Frame)
 *
 *  Condition 1 is not frame-qualified, so one lost packet blocks the whole lobby
 *  even when the missing commands are stamped for a frame nobody needs yet. The
 *  Commands-vs-MaxAhead split below is what tells us how much of the stall time
 *  that costs, and therefore what a frame-aware gate would buy.
 */
class NetTelemetry
{
public:
	enum class StallReason
	{
		None = 0,      //!< the gate passed; the frame advances
		Commands,      //!< blocked on their[i].Recv < their[i].Sent
		MaxAhead,      //!< blocked on Frame >= MaxAhead + min peer frame
	};

	struct GateState
	{
		int  NumConnections;
		int  MinPeerFrame;
		int  BlockingPeer;      //!< peer holding us up, or -1
		bool BlockedByCommands;
		bool BlockedByMaxAhead;

		StallReason Reason() const
		{
			// Matches the engine's own precedence: the command-count test at
			// 0x6495E6 jumps past the MaxAhead check entirely.
			if (BlockedByCommands)
				return StallReason::Commands;
			if (BlockedByMaxAhead)
				return StallReason::MaxAhead;
			return StallReason::None;
		}
	};

	static bool Enable;

	/**
	 *  Times CommBufferClass::Queue_Send refused a packet because the 32-deep
	 *  per-peer send queue was full.
	 *
	 *  This silently drops a game command block: Queue_Send returns 0, so does
	 *  ConnectionClass::Send_Packet, and Send_Packets ignores the return value
	 *  entirely (0x649F94). Nothing anywhere notices. Any non-zero value here is
	 *  a real stall and desync source that has never been measured.
	 */
	static int SendQueueDrops;

	/**
	 *  Reads the engine's peer sync table and evaluates both gate conditions.
	 *  Touches globals only, so it is safe to call from anywhere in the wait loop.
	 */
	static void EvaluateGate(GateState& state);

	/**
	 *  Writes a timestamped line to `netlog_<TestName>_<pid>.txt` in the game
	 *  directory.
	 *
	 *  This does NOT go through Debug::Log. That routes to WWDebugString at
	 *  0x4068E0, which is an empty stub in the retail executable -- every
	 *  Debug::Log call in the spawner is silently discarded there. Telemetry has
	 *  to own its output or there is nothing to collect.
	 */
	static void Log(const char* pFormat, ...);

	//!< Identifies the run in the log filename, from [Settings] NetTestName.
	static const char* TestName;

	static void Reset();
	static void ObserveGate(const GateState& state);
	static void Tick();

	//!< One line per active fix, emitted once at match start so a log identifies
	//!< its own configuration without needing the spawn.ini alongside it.
	static void LogConfiguration();

	//!< The comparable block. Emitted every 30s and at match end, so a hard exit
	//!< still leaves a usable measurement behind.
	static void Dump(const char* pReason);
};
