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

#include "NetTelemetry.h"

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>

#include <Spawner/FrameGate.h>
#include <Spawner/FastRetransmit.h>
#include <Spawner/PacketRedundancy.h>
#include <Spawner/SendClock.h>
#include <Spawner/StallCounters.h>
#include <Misc/RenderThrottle.h>

#include <IPXManagerClass.h>
#include <SessionClass.h>

#include <Windows.h>
#include <algorithm>

bool NetTelemetry::Enable = false;
int  NetTelemetry::SendQueueDrops = 0;
const char* NetTelemetry::TestName = "run";

namespace
{
	HANDLE LogFile = INVALID_HANDLE_VALUE;
	DWORD  LogBaseTick = 0;

	void EnsureLogOpen()
	{
		if (LogFile != INVALID_HANDLE_VALUE)
			return;

		// One file per process, named after the run, so a test matrix collects
		// itself and repeat runs never append onto each other.
		char name[128];
		_snprintf_s(name, sizeof(name), _TRUNCATE, "netlog_%s_%lu.txt",
			NetTelemetry::TestName, GetCurrentProcessId());

		LogFile = CreateFileA(name, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

		LogBaseTick = GetTickCount();

		if (LogFile == INVALID_HANDLE_VALUE)
			return;

		SYSTEMTIME time;
		GetLocalTime(&time);

		char banner[256];
		const int length = _snprintf_s(banner, sizeof(banner), _TRUNCATE,
			"===== netcode test '%s'  %04d-%02d-%02d %02d:%02d:%02d  pid=%lu =====\r\n",
			NetTelemetry::TestName,
			time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
			GetCurrentProcessId());

		DWORD written = 0;
		WriteFile(LogFile, banner, (DWORD)length, &written, nullptr);
	}
}

void NetTelemetry::Log(const char* pFormat, ...)
{
	if (!Enable)
		return;

	EnsureLogOpen();
	if (LogFile == INVALID_HANDLE_VALUE)
		return;

	char body[1024];
	va_list args;
	va_start(args, pFormat);
	_vsnprintf_s(body, sizeof(body), _TRUNCATE, pFormat, args);
	va_end(args);

	// Callers keep the engine's trailing-newline convention; we supply CRLF.
	for (int i = (int)strlen(body) - 1; i >= 0 && (body[i] == '\n' || body[i] == '\r'); --i)
		body[i] = '\0';

	const DWORD elapsed = GetTickCount() - LogBaseTick;

	char line[1200];
	const int length = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%3lu.%03lu] %s\r\n",
		elapsed / 1000, elapsed % 1000, body);

	DWORD written = 0;
	WriteFile(LogFile, line, (DWORD)length, &written, nullptr);
}

namespace
{
	#define CurrentFrame  Make_Global<int>(0xA8ED84u)
	#define MaxAhead      Make_Global<int>(0xA8B550u)
	#define FrameSendRate Make_Global<int>(0xA8B554u)

	//!< QueueClass::Count sits at offset 0 of both queues.
	#define DoListCount   Make_Global<int>(0x8B41F8u)
	#define OutListCount  Make_Global<int>(0xA802C8u)

	constexpr long long WindowMicroseconds = 1000000;

	long long QpcNow()
	{
		LARGE_INTEGER value;
		QueryPerformanceCounter(&value);
		return value.QuadPart;
	}

	long long QpcFrequency()
	{
		static long long frequency = []() -> long long
		{
			LARGE_INTEGER value;
			QueryPerformanceFrequency(&value);
			return value.QuadPart ? value.QuadPart : 1;
		}();

		return frequency;
	}

	long long ElapsedMicroseconds(long long from, long long to)
	{
		return ((to - from) * 1000000) / QpcFrequency();
	}

	struct PeerSnapshot
	{
		int Resends;
		int Lost;
		int FrameSyncStalls;
		int CommandCoundStalls;
	};

	struct Window
	{
		long long Start;
		int  GateChecks;
		int  GatePasses;
		int  BlockedOnCommands;
		int  BlockedOnMaxAhead;
		long long StallCommandsUs;
		long long StallMaxAheadUs;
		int  PeakDoList;
		int  PeakOutList;
	};

	Window Current;
	Window Total;

	//!< Open stall: set on the first blocked gate check, cleared when the gate passes.
	long long StallStart = 0;
	NetTelemetry::StallReason StallKind = NetTelemetry::StallReason::None;
	int StallCulprit = -1;

	PeerSnapshot LastStats[8];
	DWORD LastSendOverflows = 0;
	DWORD LastReceiveOverflows = 0;

	//!< Start of the measured run, for frames-per-wall-second.
	long long RunStart = 0;
	int RunStartFrame = 0;
	bool ConfigurationLogged = false;

	//!< Emit the result block this often as well as at match end, so a run that
	//!< is killed rather than exited cleanly still leaves a measurement.
	constexpr long long ResultIntervalUs = 30000000;
	long long NextResultAt = 0;

	void ResetWindow(Window& window)
	{
		window = Window {};
		window.Start = QpcNow();
	}

	void CloseStall(long long now)
	{
		if (!StallStart)
			return;

		const long long elapsed = ElapsedMicroseconds(StallStart, now);

		if (StallKind == NetTelemetry::StallReason::Commands)
		{
			Current.StallCommandsUs += elapsed;
			Total.StallCommandsUs += elapsed;
		}
		else
		{
			Current.StallMaxAheadUs += elapsed;
			Total.StallMaxAheadUs += elapsed;
		}

		StallStart = 0;
		StallKind = NetTelemetry::StallReason::None;
		StallCulprit = -1;
	}
}

void NetTelemetry::EvaluateGate(GateState& state)
{
	const int connections = std::min((int)IPXManagerClass::Instance.NumConnections, FrameGate::MaxPeers);

	state.NumConnections = connections;
	state.MinPeerFrame = CurrentFrame + 1000;   // the engine's own sentinel
	state.BlockingPeer = -1;
	state.BlockedByCommands = false;
	state.BlockedByMaxAhead = false;

	if (connections <= 0)
		return;

	const FrameGate::TheirSync* peers = FrameGate::Peers();

	for (int i = 0; i < connections; ++i)
	{
		const FrameGate::TheirSync& peer = peers[i];

		if (peer.Frame < state.MinPeerFrame)
			state.MinPeerFrame = peer.Frame;

		// Unsigned compare, matching the engine's `jb` at 0x6495E6.
		if (!state.BlockedByCommands && (unsigned int)peer.Recv < (unsigned int)peer.Sent)
		{
			state.BlockedByCommands = true;
			state.BlockingPeer = i;
		}
	}

	if (CurrentFrame >= MaxAhead + state.MinPeerFrame)
		state.BlockedByMaxAhead = true;
}

void NetTelemetry::ObserveGate(const GateState& state)
{
	const long long now = QpcNow();
	const StallReason reason = state.Reason();

	++Current.GateChecks;
	++Total.GateChecks;

	if (reason == StallReason::None)
	{
		++Current.GatePasses;
		++Total.GatePasses;
		CloseStall(now);
		return;
	}

	if (reason == StallReason::Commands)
	{
		++Current.BlockedOnCommands;
		++Total.BlockedOnCommands;
	}
	else
	{
		++Current.BlockedOnMaxAhead;
		++Total.BlockedOnMaxAhead;
	}

	// A stall that changes reason mid-flight is attributed in two parts rather
	// than to whichever reason happened to be last.
	if (StallStart && StallKind != reason)
		CloseStall(now);

	if (!StallStart)
	{
		StallStart = now;
		StallKind = reason;
		StallCulprit = state.BlockingPeer;
	}
}

void NetTelemetry::Tick()
{
	if (!Enable || !SessionClass::IsMultiplayer())
		return;

	const int doList = DoListCount;
	const int outList = OutListCount;

	if (doList > Current.PeakDoList)  Current.PeakDoList = doList;
	if (outList > Current.PeakOutList) Current.PeakOutList = outList;
	if (doList > Total.PeakDoList)    Total.PeakDoList = doList;
	if (outList > Total.PeakOutList)  Total.PeakOutList = outList;

	const long long now = QpcNow();
	if (!Current.Start)
		Current.Start = now;

	if (!RunStart)
	{
		RunStart = now;
		RunStartFrame = CurrentFrame;
		NextResultAt = now;
	}

	if (!ConfigurationLogged)
	{
		ConfigurationLogged = true;
		LogConfiguration();
	}

	if (ElapsedMicroseconds(NextResultAt, now) >= ResultIntervalUs)
	{
		NextResultAt = now;
		Dump("30s checkpoint");
	}

	if (ElapsedMicroseconds(Current.Start, now) < WindowMicroseconds)
		return;

	auto& session = SessionClass::Instance;

	NetTelemetry::Log("[NetTelemetry] frame=%d stall_cmd=%lldms stall_ahead=%lldms"
		" blocked_cmd=%d blocked_ahead=%d passes=%d"
		" dolist=%d/%d outlist=%d maxahead=%d fsr=%d overflow=%u/%u\n",
		(int)CurrentFrame,
		Current.StallCommandsUs / 1000,
		Current.StallMaxAheadUs / 1000,
		Current.BlockedOnCommands,
		Current.BlockedOnMaxAhead,
		Current.GatePasses,
		Current.PeakDoList, doList,
		Current.PeakOutList,
		(int)MaxAhead,
		(int)FrameSendRate,
		IPXManagerClass::Instance.SendOverflows - LastSendOverflows,
		IPXManagerClass::Instance.ReceiveOverflows - LastReceiveOverflows);

	LastSendOverflows = IPXManagerClass::Instance.SendOverflows;
	LastReceiveOverflows = IPXManagerClass::Instance.ReceiveOverflows;

	// Per-peer deltas. The engine maintains all of these and never reads them back.
	for (int i = 0; i < session.MPlayerCount && i < 8; ++i)
	{
		const auto& stats = session.MPStats[i];
		PeerSnapshot& last = LastStats[i];

		const int resends = stats.Resends - last.Resends;
		const int lost = stats.Lost - last.Lost;
		const int syncStalls = stats.FrameSyncStalls - last.FrameSyncStalls;
		const int cmdStalls = stats.CommandCoundStalls - last.CommandCoundStalls;

		if (resends || lost || syncStalls || cmdStalls)
		{
			NetTelemetry::Log("[NetTelemetry]   peer%d resends=%d lost=%d(%d%%) rtt_max=%d"
				" syncstalls=%d cmdstalls=%d\n",
				i, resends, lost, stats.PercentLost, stats.MaxRoundTrip,
				syncStalls, cmdStalls);
		}

		last.Resends = stats.Resends;
		last.Lost = stats.Lost;
		last.FrameSyncStalls = stats.FrameSyncStalls;
		last.CommandCoundStalls = stats.CommandCoundStalls;
	}

	ResetWindow(Current);
}

void NetTelemetry::Reset()
{
	ResetWindow(Current);
	ResetWindow(Total);

	StallStart = 0;
	StallKind = StallReason::None;
	StallCulprit = -1;

	for (auto& entry : LastStats)
		entry = PeerSnapshot {};

	LastSendOverflows = 0;
	LastReceiveOverflows = 0;
	SendQueueDrops = 0;

	RunStart = 0;
	RunStartFrame = 0;
	ConfigurationLogged = false;
	NextResultAt = 0;
}

void NetTelemetry::LogConfiguration()
{
	NetTelemetry::Log("[NetTest] ---- configuration ----\n");
	NetTelemetry::Log("[NetTest] FrameAwareGate=%d FastRetransmit=%d RetransmitBackoff=%d\n",
		FrameGate::Enable ? 1 : 0,
		FastRetransmit::Enabled ? 1 : 0,
		FastRetransmit::Backoff ? 1 : 0);
	NetTelemetry::Log("[NetTest] PacketRedundancy=%d copies=%d adaptive=%d\n",
		PacketRedundancy::Enabled ? 1 : 0,
		PacketRedundancy::Copies,
		PacketRedundancy::Adaptive ? 1 : 0);
	NetTelemetry::Log("[NetTest] StallCounterFix=%d RenderThrottle=%d SendClockDrift=%d\n",
		StallCounters::Enable ? 1 : 0,
		RenderThrottle::Enable ? 1 : 0,
		SendClock::Enable ? 1 : 0);
	NetTelemetry::Log("[NetTest] MaxAhead=%d FrameSendRate=%d (the latency ladder only runs when Protocol=0)\n",
		(int)MaxAhead, (int)FrameSendRate);
	NetTelemetry::Log("[NetTest] -----------------------\n");
}

void NetTelemetry::Dump(const char* pReason)
{
	if (!Enable)
		return;

	const long long now = QpcNow();
	const long long elapsedUs = RunStart ? ElapsedMicroseconds(RunStart, now) : 0;
	const int frames = (int)CurrentFrame - RunStartFrame;

	// The headline number: frames actually completed per wall second. That is
	// what the player experiences and it compares directly between runs.
	const int fpsX100 = (elapsedUs > 0)
		? (int)((long long)frames * 100000000LL / elapsedUs)
		: 0;

	const long long stallTotal = Total.StallCommandsUs + Total.StallMaxAheadUs;
	const int commandShare = stallTotal > 0 ? (int)((Total.StallCommandsUs * 100) / stallTotal) : 0;
	const int stallShare = elapsedUs > 0 ? (int)((stallTotal * 100) / elapsedUs) : 0;

	NetTelemetry::Log("[NetTest] ===== RESULT (%s) =====\n", pReason);
	NetTelemetry::Log("[NetTest] frames=%d over %dms => %d.%02d frames/sec\n",
		frames, (int)(elapsedUs / 1000), fpsX100 / 100, fpsX100 % 100);
	NetTelemetry::Log("[NetTest] blocked %lldms (%d%% of run): commands %lldms (%d%%) maxahead %lldms (%d%%)\n",
		stallTotal / 1000, stallShare,
		Total.StallCommandsUs / 1000, commandShare,
		Total.StallMaxAheadUs / 1000, 100 - commandShare);
	NetTelemetry::Log("[NetTest] gate: checks=%d passes=%d blocked_cmd=%d blocked_ahead=%d\n",
		Total.GateChecks, Total.GatePasses, Total.BlockedOnCommands, Total.BlockedOnMaxAhead);
	NetTelemetry::Log("[NetTest] framegate: saves=%d blocks=%d late=%d\n",
		FrameGate::Saves, FrameGate::Blocks, FrameGate::LateArrivals);
	NetTelemetry::Log("[NetTest] retransmit: cleanRTT=%dticks redundant_datagrams=%d send_queue_drops=%d\n",
		FastRetransmit::SmoothedRTT(), PacketRedundancy::Emitted, SendQueueDrops);
	NetTelemetry::Log("[NetTest] queues: peak DoList=%d (throttle 8192, fatal 16384) peak OutList=%d (cap 128)\n",
		Total.PeakDoList, Total.PeakOutList);
	NetTelemetry::Log("[NetTest] sendclock: drift=%d peak=%d | stall_counter_writes_suppressed=%d\n",
		SendClock::Drift, SendClock::PeakDrift, StallCounters::Suppressed);

	auto& session = SessionClass::Instance;
	for (int i = 0; i < session.MPlayerCount && i < 8; ++i)
	{
		const auto& stats = session.MPStats[i];
		NetTelemetry::Log("[NetTest] peer%d resends=%d lost=%d(%d%%) rtt_max=%d syncstalls=%d cmdstalls=%d\n",
			i, stats.Resends, stats.Lost, stats.PercentLost, stats.MaxRoundTrip,
			stats.FrameSyncStalls, stats.CommandCoundStalls);
	}

	if (SendQueueDrops != 0)
		NetTelemetry::Log("[NetTest] *** %d command blocks dropped by a full send queue -- invisible to the engine ***\n", SendQueueDrops);

	// Must be zero. Anything else means a frame advanced past commands that had
	// not arrived: a determinism bug, not a performance one.
	if (FrameGate::LateArrivals != 0)
		NetTelemetry::Log("[NetTest] *** %d LATE COMMAND ARRIVALS -- A GATE IS WRONG, DO NOT SHIP ***\n", FrameGate::LateArrivals);

	NetTelemetry::Log("[NetTest] ===============================\n");

	// So a run that is killed rather than exited cleanly still leaves the block
	// on disk.
	if (LogFile != INVALID_HANDLE_VALUE)
		FlushFileBuffers(LogFile);
}

/**
 *  Main_Loop, at `++ProcessingFrames` -- once per loop iteration.
 *
 *  0x55DE3A  89 0d 64 b5 a8 00   mov [0xA8B564], ecx
 *
 *  Shared per-frame tick for the netcode modules. Kept as one hook rather than
 *  one per module so there is only ever a single patch at this address.
 */
DEFINE_HOOK(0x55DE3A, MainLoop_NetTelemetry_Tick, 0x6)
{
	NetTelemetry::Tick();
	SendClock::Tick();
	return 0;
}

/**
 *  CommBufferClass::Queue_Send, on the "No room in send queue" path.
 *
 *  0x48B531  68 14 d2 81 00   push offset "Queue_Send - No room in send queue"
 *
 *  The engine drops the packet here and every caller ignores the failure, so this
 *  counter is the only way to see it happen.
 */
DEFINE_HOOK(0x48B531, QueueSend_NetTelemetry_Overflow, 0x5)
{
	++NetTelemetry::SendQueueDrops;
	return 0;
}

/**
 *  Queue_Exit -- the match is ending.
 *
 *  0x6471A0  83 ec 74           sub esp, 74h
 *  0x6471A3  a1 38 b2 a8 00     mov eax, Session
 */
DEFINE_HOOK(0x6471A0, QueueExit_NetTelemetry_Dump, 0x8)
{
	NetTelemetry::Dump("match ended");
	return 0;
}

/**
 *  Print_CRCs_All_Players -- the engine detected a player CRC mismatch.
 *
 *  0x6516F0  81 ec a4 03 00 00  sub esp, 3A4h
 */
DEFINE_HOOK(0x6516F0, PrintCRCsAllPlayers_NetTelemetry_Dump, 0x6)
{
	NetTelemetry::Dump("player CRC mismatch");
	return 0;
}
