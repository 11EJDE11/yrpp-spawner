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

#include "NetDiagnostics.h"
#include "FastRetransmit.h"
#include "PacketRedundancy.h"
#include "ProtocolZero.h"
#include "ProtocolZero.LatencyLevel.h"
#include "FrameGate.h"
#include "RenderSkip.h"
#include "Spawner.h"

#include <windows.h>

#include <Helpers/Macro.h>
#include <IPXManagerClass.h>
#include <ConnectionClass.h>
#include <IPXConnClass.h>
#include <CommBufferClass.h>
#include <HouseClass.h>
#include <SessionClass.h>
#include <Unsorted.h>
#include <Utilities/Debug.h>

#include <cstdio>

bool NetDiagnostics::Enabled = true;

namespace
{
	// Wait_For_Players bumps these once per stalled frame, for the peer it is
	// waiting on (these are engine accounting counters, not elapsed time).
	// FrameSyncStalls means "this peer's reported frame is too far
	// behind us"; CommandCoundStalls means "this peer says it sent commands we
	// have not received". Stride and bases read out of Wait_For_Players.
	int StallCount(int connectionIndex, bool commandStall)
	{
		if (connectionIndex < 0 || connectionIndex >= 8)
			return 0;
		const DWORD base = commandStall ? 0xA8B5CCu : 0xA8B5C8u;
		return *reinterpret_cast<int*>(base + 104 * connectionIndex);
	}

	DWORD g_lastTick = 0;
	bool  g_headerLogged = false;

	// Frame-dwell tracking. The heartbeat is once a second, which is too coarse
	// now that freezes should be sub-second, so stall length is measured from
	// how long CurrentFrame sits unchanged - this runs on every Service call.
	int   g_lastFrame = -1;
	// High-water marks, so a removed connection cannot make totals fall.
	int g_peakResends = 0, g_peakLost = 0, g_peakSentAck = 0;
	int g_peakSentNoAck = 0, g_peakRecvAck = 0, g_peakRecvNoAck = 0;
	// Per-peer inbound silence tracking, for pinning down the exact moment a
	// link dies. Wall clock, so several logs can be lined up against each other.
	// Keyed by connection ID, which is stable. Connection[] is compacted when a
	// player leaves, so an index means a different peer afterwards and the
	// tracker would silently attribute one player's silence to another.
	int   g_silenceId[8] = {};
	int   g_lastRecvTotal[8] = {};
	DWORD g_lastRecvTick[8] = {};
	bool  g_peerSilent[8] = {};

	int SilenceSlot(int id)
	{
		for (int k = 0; k < 8; ++k)
			if (g_silenceId[k] == id)
				return k;
		for (int k = 0; k < 8; ++k)
			if (g_silenceId[k] == 0)
			{
				g_silenceId[k] = id;
				return k;
			}
		return -1;
	}
	DWORD g_frameEnteredTick = 0;
	// Only stalls a player would actually notice and report. Anything shorter
	// is ordinary network waiting and would fill a release log.
	const DWORD StallReportMs = 2000;

	int   g_frameSyncCount = 0;

	bool g_waiting = false;
	DWORD g_waitStarted = 0;
	DWORD g_waitReported = 0;
	DWORD g_waitTotal = 0;
	unsigned int g_waitCount = 0;
	int g_waitFrame = -1;
	bool g_waitFirstFrameBlocked = false;
	int g_waitFirstCommandPeer = -1;
	bool g_waitSawFrameBlock = false;
	bool g_waitSawCommandBlock = false;

	// Run-length histogram of consecutive unreliable-packet loss, per connection.
	// Buckets: 1, 2, 3, 4, 5, 6-9, 10+.
	const int RunBuckets = 7;
	struct NoAckTracker
	{
		const ConnectionClass* connection;
		int lastId;
		int runs[RunBuckets];
		int lost;
		int received;
	};
	NoAckTracker g_noack[8] = {};

	NoAckTracker* TrackerFor(const ConnectionClass* connection)
	{
		NoAckTracker* freeSlot = nullptr;
		for (int i = 0; i < 8; ++i)
		{
			if (g_noack[i].connection == connection)
				return &g_noack[i];
			if (!g_noack[i].connection && !freeSlot)
				freeSlot = &g_noack[i];
		}
		if (!freeSlot)
			return nullptr;
		*freeSlot = NoAckTracker {};
		freeSlot->connection = connection;
		freeSlot->lastId = -1;
		return freeSlot;
	}

	int BucketFor(int run)
	{
		if (run <= 5)
			return run - 1;
		return run <= 9 ? 5 : 6;
	}

	int ConnectionCount()
	{
		int nconn = static_cast<int>(IPXManagerClass::Instance.NumConnections);
		const int arraySize = sizeof(IPXManagerClass::Instance.Connection) / sizeof(IPXManagerClass::Instance.Connection[0]);
		if (nconn < 0) nconn = 0;
		if (nconn > arraySize) nconn = arraySize;
		return nconn;
	}

	// The spawn.ini player index minus one - the key PacketRedundancy and
	// NetHack use. Spawner writes each node's address as its player index.
	int SpawnSlot(const IPXConnClass* conn)
	{
		DWORD slot = 0;
		memcpy(&slot, conn->Address.NodeAddress, sizeof(slot));
		return static_cast<int>(slot) - 1;
	}

	int CurrentFrame()
	{
		return static_cast<int>(Unsorted::CurrentFrame);
	}

	// Logs the per-player identity and the connection-index -> house/spawn-slot
	// mapping once, so heartbeats from different machines can be matched up.
	void LogHeader()
	{
		const auto* cfg = Spawner::GetConfig();
		const int nconn = ConnectionCount();

		Debug::Log("[NetDiag] init frame=%d conns=%d cfg FastRetransmit=%d Backoff=%d Redundancy=%d copies=%d adaptive=%d acks=%d framegate=%d protocol=%d\n",
			CurrentFrame(), nconn,
			(int)FastRetransmit::Enabled, (int)FastRetransmit::Backoff,
			(int)PacketRedundancy::Enabled, PacketRedundancy::Copies,
			(int)PacketRedundancy::Adaptive, (int)PacketRedundancy::Acks,
			(int)FrameGate::Enabled, cfg ? cfg->Protocol : -1);

		// Every option this build can change, on one line, so a baseline run and a
		// configured run can be diffed without guessing what was active.
		Debug::Log("[Audit] config protocolzero=%d descent=%d timeoutfloor=%d maxlatency=%d"
			" | retransmit=%d backoff=%d"
			" | redundancy=%d copies=%d adaptive=%d acks=%d"
			" | framegate=%d renderskip=%d rsminms=%d rsshare=%d\n",
			(int)ProtocolZero::Enable, (int)LatencyLevel::AllowDescent,
			ProtocolZero::ConnectionTimeoutFloor,
			(int)ProtocolZero::MaxLatencyLevel,
			(int)FastRetransmit::Enabled, (int)FastRetransmit::Backoff,
			(int)PacketRedundancy::Enabled, PacketRedundancy::Copies,
			(int)PacketRedundancy::Adaptive, (int)PacketRedundancy::Acks,
			(int)FrameGate::Enabled, (int)RenderSkip::Enabled,
			RenderSkip::MinProcessMs, RenderSkip::RenderSharePercent);

		{
			// GetTickCount is machine uptime; it cannot align two logs. A UTC
			// stamp taken once, next to the tick it corresponds to, lets every
			// later tick value be converted to a common timeline.
			SYSTEMTIME utc {};
			GetSystemTime(&utc);
			Debug::Log("[Audit] clock utc=%04u-%02u-%02uT%02u:%02u:%02u.%03uZ tick=%u\n",
				utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute,
				utc.wSecond, utc.wMilliseconds, GetTickCount());
		}

		Debug::Log("[NetDiag] schema=4 build=%s %s retry_policy=captured-base-clean-recovery engine_tick_ms=16 wait_log_min_ms=16\n", __DATE__, __TIME__);

		if (HouseClass::CurrentPlayer)
		{
			Debug::Log("[NetDiag] self house=%d name=%ls\n",
				HouseClass::CurrentPlayer->ArrayIndex, HouseClass::CurrentPlayer->UIName);
		}

		for (int i = 0; i < nconn; ++i)
		{
			const IPXConnClass* conn = IPXManagerClass::Instance.Connection[i];
			if (!conn)
				continue;
			Debug::Log("[NetDiag] peer conn=%d id=%d spawnslot=%d name=%ls\n",
				i, conn->ID, SpawnSlot(conn), conn->Name);
		}
	}
}

void NetDiagnostics::Reset()
{
	g_lastTick = 0;
	g_headerLogged = false;
	g_frameSyncCount = 0;
	g_lastFrame = -1;
	g_frameEnteredTick = 0;
	g_waiting = false;
	g_waitStarted = g_waitReported = g_waitTotal = 0;
	g_waitCount = 0;
	g_waitFrame = -1;
	for (int i = 0; i < 8; ++i)
		g_noack[i] = NoAckTracker {};
}

void NetDiagnostics::NoteWait(int commandPeer, int minimumFrame, int maxAhead)
{
	if (!Enabled)
		return;
	const DWORD now = GetTickCount();
	const bool frameBlocked = CurrentFrame() >= minimumFrame + maxAhead;
	if (!g_waiting)
	{
		g_waiting = true;
		g_waitFrame = CurrentFrame();
		g_waitStarted = now;
		g_waitReported = now;
		g_waitFirstFrameBlocked = frameBlocked;
		g_waitFirstCommandPeer = commandPeer;
		g_waitSawFrameBlock = false;
		g_waitSawCommandBlock = false;
	}
	g_waitSawFrameBlock |= frameBlocked;
	g_waitSawCommandBlock |= commandPeer >= 0;
	if (now - g_waitReported >= 250)
	{
		g_waitReported = now;
	}
}

void NetDiagnostics::EndWait()
{
	if (!Enabled || !g_waiting)
		return;
	const DWORD now = GetTickCount();
	const DWORD duration = now - g_waitStarted;
	g_waitTotal += duration;
	++g_waitCount;
	if (duration >= 16)
	g_waiting = false;
}

void NetDiagnostics::NoteNoAckPacket(const ConnectionClass* connection, int packetId)
{
	if (!Enabled || packetId < 0)
		return;

	NoAckTracker* t = TrackerFor(connection);
	if (!t)
		return;

	++t->received;

	// Only forward progress tells us anything; a repeat or reorder does not.
	if (t->lastId >= 0 && packetId > t->lastId + 1)
	{
		const int run = packetId - t->lastId - 1;
		t->lost += run;
		++t->runs[BucketFor(run)];

		if (run >= InterestingRun)
		{
			int index = -1;
			const int nconn = ConnectionCount();
			for (int i = 0; i < nconn; ++i)
				if (IPXManagerClass::Instance.Connection[i] == connection)
				{
					index = i;
					break;
				}
		}
	}

	if (packetId > t->lastId)
		t->lastId = packetId;
}

void NetDiagnostics::LogEvent(const ConnectionClass* connection, const char* what, int from, int to)
{
	if (!Enabled)
		return;

	int index = -1;
	const int nconn = ConnectionCount();
	for (int i = 0; i < nconn; ++i)
	{
		if (IPXManagerClass::Instance.Connection[i] == connection)
		{
			index = i;
			break;
		}
	}

}

void NetDiagnostics::Tick()
{
	// Periodic scorecard. Every 5000 frames is roughly every 80 seconds of
	// simulation - frequent enough to bracket an incident, rare enough to stay
	// out of the way.
	{
		static int lastSummaryFrame = 0;
		const int frame = CurrentFrame();
		if (frame - lastSummaryFrame >= 5000)
		{
			lastSummaryFrame = frame;
			LogSummary("periodic");
		}
	}

	if (!Enabled)
		return;

	const int nconn = ConnectionCount();
	if (nconn <= 0)
		return;

	if (!g_headerLogged)
	{
		g_headerLogged = true;
		LogHeader();
	}

	const DWORD now = GetTickCount();
	const int frame = CurrentFrame();

	if (frame != g_lastFrame)
	{
		if (g_lastFrame >= 0 && g_frameEnteredTick != 0)
		{
			const DWORD dwell = now - g_frameEnteredTick;
			if (dwell >= StallReportMs)
				Debug::Log("[NetDiag] stall frame=%d ms=%u\n", g_lastFrame, dwell);
		}
		g_lastFrame = frame;
		g_frameEnteredTick = now;
	}

	if (g_lastTick != 0 && (now - g_lastTick) < static_cast<DWORD>(PeriodMs))
		return;
	g_lastTick = now;


	// Live engine state against what this client intended, every tick. Catches a
	// bad pair however it arose - including one produced by a path we do not
	// hook - and catches our own intent drifting away from what the engine
	// actually holds, which is exactly how Game 31 broke: MaxAhead came from our
	// timing-event hook while FrameSendRate was still the engine's stale local.
	{
		const int liveMaxAhead = Game::Network::MaxAhead;
		const int liveRate = Game::Network::FrameSendRate;
		const int intendedRate = (int)LatencyLevel::NewFrameSendRate;
		const int intendedMaxAhead = LatencyLevel::TargetMaxAhead;
		const bool divides = liveRate > 0 && (liveMaxAhead % liveRate) == 0;
		const bool agreed = liveMaxAhead == intendedMaxAhead && liveRate == intendedRate;

		// Before the first Commit there is no intent to compare against, so the
		// mismatch at startup is meaningless - it fired six times a match saying
		// nothing.
		if (intendedMaxAhead > 0 && (!divides || !agreed))
		{
			Debug::Log("[Audit] state frame=%d engine maxahead=%d fsr=%d | intended maxahead=%d fsr=%d | divides=%d agreed=%d%s\n",
				CurrentFrame(), liveMaxAhead, liveRate, intendedMaxAhead, intendedRate,
				(int)divides, (int)agreed,
				divides ? "  (settling)" : "  <<<< INVARIANT VIOLATED IN LIVE STATE");
		}
	}


	int extraDatagrams = 0, extraFailed = 0;
	PacketRedundancy::GetCostStats(extraDatagrams, extraFailed);

	for (int i = 0; i < nconn; ++i)
	{
		const IPXConnClass* conn = IPXManagerClass::Instance.Connection[i];
		if (!conn)
			continue;

		const int slot = SpawnSlot(conn);

		// Inbound silence watchdog. Says, with a wall clock, the exact moment this
		// client stopped hearing from a peer and the moment it resumed - which is
		// what lets six logs be lined up to see whether one machine went quiet or
		// everyone stopped talking to it at once.
		{
			const int recvTotal = conn->NumRecAck + conn->NumRecNoAck;
			const DWORD nowTick = GetTickCount();
			const int sl = SilenceSlot(conn->ID + 1);
			if (sl >= 0)
			{
				if (recvTotal != g_lastRecvTotal[sl])
				{
					if (g_peerSilent[sl])
					{
						Debug::Log("[Audit] peer-resumed frame=%d conn=%d slot=%d silent_for=%ums t=%u\n",
							frame, i, slot, nowTick - g_lastRecvTick[sl], nowTick);
						g_peerSilent[sl] = false;
					}
					g_lastRecvTotal[sl] = recvTotal;
					g_lastRecvTick[sl] = nowTick;
				}
				else if (!g_peerSilent[sl] && g_lastRecvTick[sl] != 0 && (nowTick - g_lastRecvTick[sl]) >= 2000)
				{
					g_peerSilent[sl] = true;
					Debug::Log("[Audit] peer-silent frame=%d conn=%d slot=%d no_inbound_for=%ums | we have sent %d/%d ack/noack to it t=%u\n",
						frame, i, slot, nowTick - g_lastRecvTick[sl],
						conn->NumSendAck, conn->NumSendNoAck, nowTick);
				}
			}
		}
		// Per-connection detail was removed for release: it logged every service
		// call and walked each send queue to do it. What remains below fires only
		// when something is actually wrong.
	}
}

// IPXManagerClass::Service. Runs once per frame and on every iteration of the
// Wait_For_Players stall spin, so the heartbeat keeps ticking while frames do not.
void NetDiagnostics::LogResponseDecision(int engineTicks, int cleanTicks, int chosenTicks,
	int wireTicks, int engineLevel, int sentLevel, bool fastWon)
{
	if (!Enabled)
		return;

	// headroom is what remains before the signed byte saturates at 126. A run
	// of games that never gets near it says the saturation guard is insurance;
	// a run that reaches it says the reported figure itself is the problem.
}

namespace
{
	int g_sendFailures = 0;
	int g_lastSendError = 0;
	DWORD g_sendFailLastLog = 0;
	int g_badConnTrips = 0;
	int g_badConnWorstAge = 0;
	DWORD g_badConnLastLog = 0;
}

void NetDiagnostics::LogSendFailure(int peer, int wsaError)
{
	if (!Enabled)
		return;

	++g_sendFailures;
	g_lastSendError = wsaError;

	const DWORD now = GetTickCount();
	if (g_sendFailLastLog != 0 && (now - g_sendFailLastLog) < 1000)
		return;
	g_sendFailLastLog = now;

	Debug::Log("[Audit] sendfail frame=%d peer=%d wsa_error=%d total=%d t=%u\n",
		CurrentFrame(), peer, wsaError, g_sendFailures, now);
}

void NetDiagnostics::LogBadConnection(const ConnectionClass* connection, int packetAgeTicks,
	int timeoutTicks, int sendCount, int retryDelta)
{
	if (!Enabled)
		return;

	++g_badConnTrips;
	if (packetAgeTicks > g_badConnWorstAge)
		g_badConnWorstAge = packetAgeTicks;

	// At most one line a second: this trips hundreds of times in a bad match and
	// the totals in the summary are what actually matter.
	const DWORD now = GetTickCount();
	if (g_badConnLastLog != 0 && (now - g_badConnLastLog) < 1000)
		return;
	g_badConnLastLog = now;

	int index = -1;
	const int nconn = ConnectionCount();
	for (int i = 0; i < nconn; ++i)
		if (IPXManagerClass::Instance.Connection[i] == connection)
		{
			index = i;
			break;
		}

	Debug::Log("[Audit] badconn frame=%d conn=%d packet_age=%d timeout=%d retrydelta=%d sends=%d | trips=%d worst_age=%d | engine marks the connection bad; nothing consumes it\n",
		CurrentFrame(), index, packetAgeTicks, timeoutTicks, retryDelta, sendCount,
		g_badConnTrips, g_badConnWorstAge);
}

// One block per client that can be diffed run to run. This is the primary
// artefact for comparing a configured run against a baseline: totals only, no
// per-frame noise.
void NetDiagnostics::LogSummary(const char* reason)
{
	if (!Enabled)
		return;

	int caps = 0, worstOvershoot = 0, lastUncapped = 0;
	FastRetransmit::GetCapStats(caps, worstOvershoot, lastUncapped);
	int extraDatagrams = 0, extraFailed = 0;
	PacketRedundancy::GetCostStats(extraDatagrams, extraFailed);

	// Totals are high-water marks, not a live sum. The engine compacts
	// Connection[] when a player is removed, so summing the surviving entries
	// makes cumulative counters go DOWN mid-match and silently discards
	// everything the departed peer contributed.
	int resends = 0, lost = 0, sentAck = 0, sentNoAck = 0, recvAck = 0, recvNoAck = 0;
	const int nconn = ConnectionCount();
	for (int i = 0; i < nconn; ++i)
	{
		const auto* c = IPXManagerClass::Instance.Connection[i];
		if (!c) continue;
		resends += c->NumResends;
		lost += c->NumLost;
		sentAck += c->NumSendAck;
		sentNoAck += c->NumSendNoAck;
		recvAck += c->NumRecAck;
		recvNoAck += c->NumRecNoAck;
	}
	if (resends   > g_peakResends)   g_peakResends   = resends;   else resends   = g_peakResends;
	if (lost      > g_peakLost)      g_peakLost      = lost;      else lost      = g_peakLost;
	if (sentAck   > g_peakSentAck)   g_peakSentAck   = sentAck;   else sentAck   = g_peakSentAck;
	if (sentNoAck > g_peakSentNoAck) g_peakSentNoAck = sentNoAck; else sentNoAck = g_peakSentNoAck;
	if (recvAck   > g_peakRecvAck)   g_peakRecvAck   = recvAck;   else recvAck   = g_peakRecvAck;
	if (recvNoAck > g_peakRecvNoAck) g_peakRecvNoAck = recvNoAck; else recvNoAck = g_peakRecvNoAck;

	// Real game speed since the previous summary, by wall clock, so MaxAhead can
	// be read in time rather than frames: 36 frames is 0.6s at 60fps, 1.2s at 30.
	static DWORD s_lastTick = 0;
	static int s_lastFrame = 0;
	const DWORD tick = GetTickCount();
	const int frame = CurrentFrame();
	int fps10 = 0;
	if (s_lastTick != 0 && tick != s_lastTick && frame > s_lastFrame)
		fps10 = (int)((long long)(frame - s_lastFrame) * 10000 / (DWORD)(tick - s_lastTick));
	s_lastTick = tick;
	s_lastFrame = frame;

	// Each player's reported average frame cost (NodeNameType::Time, 0x73; the
	// PROCESS_TIME event every 128 frames, -1 before the first). The same table
	// on every client, and what Queue_AI_Multiplayer divides into 1000 for the
	// frame rate the slowest machine can sustain.
	char proc[160] = "";
	int procLen = 0;
	int procMax = -1;
	const auto& nodes = NodeNameType::Array;
	for (int i = 0; i < nodes.Count && procLen < (int)sizeof(proc) - 16; ++i)
	{
		const auto* node = nodes.Items[i];
		if (!node)
			continue;
		if (node->Time > procMax)
			procMax = node->Time;
		procLen += std::snprintf(proc + procLen, sizeof(proc) - procLen, " h%d=%d", node->HouseIndex, node->Time);
	}

	Debug::Log("[Audit] SUMMARY %s frame=%d conns=%d | level=%d maxahead=%d fsr=%d"
		" | sent_ack=%d sent_noack=%d recv_ack=%d recv_noack=%d resends=%d lost=%d"
		" | extra_datagrams=%d failed=%d | retry_caps=%d worst_overshoot=%d"
		" | badconn_trips=%d worst_packet_age=%d sendfail=%d last_wsa=%d"
		" | t=%lu fps=%d.%d | proc_ms max=%d%s\n",
		reason, frame, nconn,
		(int)LatencyLevel::CurentLatencyLevel, (int)Game::Network::MaxAhead,
		(int)Game::Network::FrameSendRate,
		sentAck, sentNoAck, recvAck, recvNoAck, resends, lost,
		extraDatagrams, extraFailed, caps, worstOvershoot,
		g_badConnTrips, g_badConnWorstAge, g_sendFailures, g_lastSendError,
		(unsigned long)tick, fps10 / 10, fps10 % 10, procMax, proc);
}

DEFINE_HOOK(0x541820, IPXManagerClass_Service_NetDiagnostics, 0x6)
{
	NetDiagnostics::Tick();
	return 0;
}

// ConnectionClass::Receive_Packet entry. __thiscall, so ECX is the connection
// and the packet sits at [esp+4] before the prologue runs. IPXGlobalConnClass
// routes through here too; TrackerFor keys on the connection so the global and
// private channels are counted separately.
// The 0x48C040 hook now lives in PacketRedundancy.cpp, which calls
// NoteNoAckPacket below: the loss signal must outlive these diagnostics.



