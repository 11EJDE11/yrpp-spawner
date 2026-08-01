/**
*  yrpp-spawner
*
*  NetTelemetry - lag-investigation logging (TESTING ONLY). See NetTelemetry.h
*  for the list of instrumented addresses and what each proves.
*/

#include "NetTelemetry.h"
#include "FrameGate.h"
#include "PacketRedundancy.h"          // so stall accounting matches the real gate

#include <Helpers/Macro.h>      // DEFINE_HOOK, REGISTERS
#include <Fundamentals.h>       // Unsorted::CurrentFrame
#include <Unsorted.h>           // Game::Network::MaxAhead / FrameSendRate
#include <IPXManagerClass.h>    // IPXManagerClass::Instance
#include <EventClass.h>         // OutList / DoList (.Count)

#include <cstdio>
#include <cstdarg>
#include <cstring>

bool NetTelemetry::Enabled = true;

namespace
{
	// ----------------------------------------------------------------------
	// Raw layouts read out of gamemd (documented in the lag write-up).
	// ----------------------------------------------------------------------

	// Per-peer sync state. Array base 0xAFA358, stride 0x18. The frame gate
	// reads exactly these fields.
	struct TheirSync
	{
		int frame;      // +0x00 last frame this peer reports having reached
		int cmd_sent;   // +0x04 highest cumulative "commands I have sent" it has told us
		int cmd_recv;   // +0x08 how many of its commands we have actually decoded
		int f0C;        // +0x0C
		int f10;        // +0x10
		int f14;        // +0x14
	};
	static_assert(sizeof(TheirSync) == 0x18, "theirStruct stride must be 0x18");
	TheirSync* const TheirSyncs = reinterpret_cast<TheirSync*>(0xAFA358);

	// Packet header (EventClass): Type@0, HouseIndex@2, Frame@3,
	// cumulative-sent count (FrameInfo.CommandCount) @11 (u16).
	inline unsigned char  PktType (const unsigned char* p) { return p[0]; }
	inline char           PktHouse(const unsigned char* p) { return static_cast<char>(p[2]); }
	inline int            PktFrame(const unsigned char* p) { return *reinterpret_cast<const int*>(p + 3); }
	inline unsigned short PktCum  (const unsigned char* p) { return *reinterpret_cast<const unsigned short*>(p + 11); }

	// The mis-indexed stall counter and where index -1 lands.
	int* const ProcessingTicks  = reinterpret_cast<int*>(0xA8B560);
	int* const ProcessingFrames = reinterpret_cast<int*>(0xA8B564); // == &MPStats[-1].CommandCoundStalls
	const unsigned CommandStall0 = 0xA8B5CC;                        // &MPStats[0].CommandCoundStalls, stride 104

	// ----------------------------------------------------------------------
	// File writer
	// ----------------------------------------------------------------------
	HANDLE g_file = INVALID_HANDLE_VALUE;
	DWORD  g_base = 0;

	void EnsureOpen()
	{
		if (g_file != INVALID_HANDLE_VALUE)
			return;

		// One fresh file per game launch (pid in the name), so runs never
		// append onto each other - append-mode contamination made early test
		// captures hard to read.
		char fname[64];
		_snprintf_s(fname, sizeof(fname), _TRUNCATE, "spawner_netlog_%lu.txt", GetCurrentProcessId());
		g_file = CreateFileA(fname, GENERIC_WRITE,
			FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		g_base = GetTickCount();

		if (g_file != INVALID_HANDLE_VALUE)
		{
			SYSTEMTIME st;
			GetLocalTime(&st);
			char banner[192];
			int n = _snprintf_s(banner, sizeof(banner), _TRUNCATE,
				"\r\n===== NetTelemetry session %04d-%02d-%02d %02d:%02d:%02d  pid=%lu =====\r\n"
				"legend: sent=peer's cumulative claim, recv=what we've decoded; a gap (sent>recv) is a missing packet\r\n",
				st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
				GetCurrentProcessId());
			DWORD w;
			WriteFile(g_file, banner, static_cast<DWORD>(n), &w, nullptr);
		}
	}

	// ----------------------------------------------------------------------
	// Gate-eval state (stall tracking)
	// ----------------------------------------------------------------------
	bool  s_stall = false;
	DWORD s_stallStart = 0;
	int   s_checks = 0;
	char  s_sig[256] = "";
	DWORD s_lastHeartbeat = 0;

	void DumpPeers(int nconn)
	{
		for (int i = 0; i < nconn; ++i)
		{
			TheirSync& p = TheirSyncs[i];
			int gap = p.cmd_sent - p.cmd_recv;
			NetTelemetry::Log("     peer%d: frame=%d sent=%d recv=%d  %s",
				i, p.frame, p.cmd_sent, p.cmd_recv,
				gap > 0 ? "<-- MISSING (gap)" : "ok");
		}
	}

	// Per-source throttles.
	unsigned short s_lastBeaconCum[8] = { 0 };
	int            s_lastMySent = -1;

	struct ResendSlot { const void* conn; int resends; };
	ResendSlot s_resend[8] = {};
}

// --------------------------------------------------------------------------
void NetTelemetry::Log(const char* pFormat, ...)
{
	if (!Enabled)
		return;
	EnsureOpen();
	if (g_file == INVALID_HANDLE_VALUE)
		return;

	char line[1024];
	DWORD t = GetTickCount() - g_base;
	int head = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%8lu] ", t);
	if (head < 0) head = 0;

	va_list args;
	va_start(args, pFormat);
	int body = _vsnprintf_s(line + head, sizeof(line) - head - 3, _TRUNCATE, pFormat, args);
	va_end(args);

	int len = head + (body > 0 ? body : 0);
	line[len++] = '\r';
	line[len++] = '\n';

	DWORD w;
	WriteFile(g_file, line, static_cast<DWORD>(len), &w, nullptr);
	FlushFileBuffers(g_file);
}

// --------------------------------------------------------------------------
// The frame-advance gate. Fires once per Wait_For_Players iteration while
// in-game. We recompute the exact gate the engine uses (all inputs are the
// globals below) and report the verdict, so a stall and its cause show up
// plainly - including that MaxAhead has margin to spare during a loss stall.
void NetTelemetry::OnGateEval()
{
	if (!Enabled)
		return;

	int nconn = static_cast<int>(IPXManagerClass::Instance.NumConnections);
	if (nconn < 1 || nconn > 7)
		return; // not a live multiplayer net game

	const int frame = Unsorted::CurrentFrame;
	const int ma    = Game::Network::MaxAhead;
	const int fsr   = Game::Network::FrameSendRate;

	int  minFrame = 0x7FFFFFFF;
	bool test1    = true;    // command-count: every peer recv >= sent
	int  gapPeer  = -1;
	int  gapAmt   = 0;
	for (int i = 0; i < nconn; ++i)
	{
		TheirSync& p = TheirSyncs[i];
		if (p.frame < minFrame)
			minFrame = p.frame;
		// A peer only blocks if it is short AND the frame-aware gate does not
		// already cover it - this keeps the stall log matching what the real
		// gate does once the fix is enabled.
		if (p.cmd_recv < p.cmd_sent && !FrameGate::RelaxCovers(i, frame) && test1)
		{
			gapPeer = i;
			gapAmt  = p.cmd_sent - p.cmd_recv;
			test1   = false;
		}
	}
	const int  limit   = ma + minFrame;
	const bool test2   = frame < limit;    // MaxAhead speed limit
	const int  margin  = limit - frame;
	const bool advance = test1 && test2;

	if (advance)
	{
		if (s_stall)
		{
			Log("STALL END   frame=%d  held %lu ms over %d gate-checks",
				frame, GetTickCount() - s_stallStart, s_checks);
			s_stall = false;
		}
		DWORD now = GetTickCount();
		if (now - s_lastHeartbeat > 2000)
		{
			s_lastHeartbeat = now;
			Log("hb          frame=%d MaxAhead=%d FSR=%d outQ=%d doList=%d resp=%d sendOvf=%lu recvOvf=%lu",
				frame, ma, fsr,
				EventClass::OutList.Count, EventClass::DoList.Count,
				IPXManagerClass::Instance.ResponseTime(),
				IPXManagerClass::Instance.SendOverflows,
				IPXManagerClass::Instance.ReceiveOverflows);
		}
		return;
	}

	// -- stalling --
	if (!s_stall)
	{
		s_stall      = true;
		s_stallStart = GetTickCount();
		s_checks     = 0;
		Log("STALL BEGIN frame=%d MaxAhead=%d  cause=%s",
			frame, ma, test1 ? "MAXAHEAD (slow peer)" : "COMMAND-COUNT (missing packet)");
		DumpPeers(nconn);
		if (!test1)
		{
			Log("     Test1 command-count : FAIL  (peer%d missing %d cmds)", gapPeer, gapAmt);
			Log("     Test2 MaxAhead      : %s  (frame %d %s limit %d = MaxAhead %d + slowestPeer %d)",
				test2 ? "PASS" : "FAIL", frame, test2 ? "<" : ">=", limit, ma, minFrame);
			if (test2)
				Log("     *** MaxAhead runway UNUSED: %d frames of headroom, but the gate blocks on command-count. Raising MaxAhead cannot help this stall. ***", margin);
		}
		else
		{
			Log("     Test2 MaxAhead      : FAIL  (frame %d >= limit %d = MaxAhead %d + slowestPeer %d) - waiting on a slow peer, not loss",
				frame, limit, ma, minFrame);
		}
	}
	++s_checks;

	// While the stall persists, log only when peer state actually changes -
	// this is where you see a beacon push 'sent' up while 'recv' stays flat,
	// then 'recv' catch up when the retransmit finally lands.
	char sig[256];
	int off = 0;
	for (int i = 0; i < nconn && off < static_cast<int>(sizeof(sig)) - 32; ++i)
		off += _snprintf_s(sig + off, sizeof(sig) - off, _TRUNCATE, "%d:%d/%d;",
			TheirSyncs[i].frame, TheirSyncs[i].cmd_sent, TheirSyncs[i].cmd_recv);

	if (strcmp(sig, s_sig) != 0)
	{
		bool first = (s_sig[0] == '\0') || (s_checks <= 1);
		strncpy_s(s_sig, sig, _TRUNCATE);
		if (!first)
		{
			Log("  ...state change during stall (frame=%d):", frame);
			DumpPeers(nconn);
		}
	}
}

// --------------------------------------------------------------------------
void NetTelemetry::OnReceivePacket(const unsigned char* pkt)
{
	if (!Enabled || !pkt)
		return;

	unsigned char type = PktType(pkt);
	if (type == 25) // FRAMESYNC keepalive beacon
	{
		int            h   = PktHouse(pkt) & 7;
		unsigned short cum = PktCum(pkt);
		if (cum != s_lastBeaconCum[h]) // only when the cumulative count moves
		{
			s_lastBeaconCum[h] = cum;
			Log("RX beacon   house=%d cumulative-sent=%d (frameHint=%d)  <- pushes this peer's 'sent' up even with a packet missing",
				PktHouse(pkt), cum, PktFrame(pkt));
		}
	}
	else if (type == 28) // FRAMEINFO data block
	{
		Log("RX data     house=%d stamp=%d cumulative-sent=%d",
			PktHouse(pkt), PktFrame(pkt), PktCum(pkt));
	}
}

// --------------------------------------------------------------------------
void NetTelemetry::OnFanout(int connId, const unsigned char* buf, int len)
{
	if (!Enabled)
		return;
	if (connId != -1)
		return; // targeted send (e.g. a per-peer ACK), not a broadcast fan-out

	int n = static_cast<int>(IPXManagerClass::Instance.NumConnections);
	Log("FANOUT      1 logical msg (type=%d len=%d) -> %d datagrams on the wire",
		buf ? buf[0] : -1, len, n);
}

// --------------------------------------------------------------------------
void NetTelemetry::OnServiceSendQueue(const unsigned char* conn)
{
	if (!Enabled || !conn)
		return;

	int resends = *reinterpret_cast<const int*>(conn + 8); // ConnectionClass::__resends

	int slot = -1, freeSlot = -1;
	for (int i = 0; i < 8; ++i)
	{
		if (s_resend[i].conn == conn) { slot = i; break; }
		if (!s_resend[i].conn && freeSlot < 0) freeSlot = i;
	}
	if (slot < 0)
	{
		slot = (freeSlot >= 0 ? freeSlot : 0);
		s_resend[slot].conn = conn;
		s_resend[slot].resends = resends;
		return; // first sighting - just record the baseline
	}
	if (resends > s_resend[slot].resends)
	{
		int delta = resends - s_resend[slot].resends;
		s_resend[slot].resends = resends;
		PacketRedundancy::NoteResend(); // loss signal for adaptive redundancy
		Log("RESEND      conn#%d retransmitted (+%d, total=%d)  frame=%d",
			slot, delta, resends, Unsorted::CurrentFrame);
	}
}

// --------------------------------------------------------------------------
void NetTelemetry::OnSendPeriod(int rawCommands)
{
	if (!Enabled)
		return;
	int cmds = rawCommands & 0xFFFF;
	if (cmds == 0)
		return;
	Log("TX period   frame=%d stampFor=%d commands=%d outQ=%d doList=%d",
		Unsorted::CurrentFrame, Unsorted::CurrentFrame + Game::Network::MaxAhead,
		cmds, EventClass::OutList.Count, EventClass::DoList.Count);
}

// --------------------------------------------------------------------------
void NetTelemetry::OnFrameSyncSent(int mySent)
{
	if (!Enabled)
		return;
	if (mySent == s_lastMySent) // beacons repeat the same count while idle
		return;
	s_lastMySent = mySent;
	Log("TX beacon   frame=%d my-cumulative-sent=%d", Unsorted::CurrentFrame, mySent);
}

// --------------------------------------------------------------------------
void NetTelemetry::OnCommandStallMisindex(int idx)
{
	if (!Enabled)
		return;
	static int seen = 0;
	if (idx == -1)
	{
		if ((seen++ % 100) == 0)
			Log("BUG         command-count stall recorded with index=%d -> writes MPStats[%d].CommandCoundStalls at 0x%08X, which IS ProcessingFrames (now %d, ticks %d). Corrupts frame-rate negotiation.",
				idx, idx, CommandStall0 + static_cast<unsigned>(idx * 104),
				*ProcessingFrames, *ProcessingTicks);
	}
	else if ((seen++ % 500) == 0)
	{
		Log("stall-idx   command-count stall recorded with index=%d (valid peer)", idx);
	}
}

// ==========================================================================
// Hooks - all observe-only (return 0). Addresses verified in gamemd_rest.i64.
// ==========================================================================

// Frame-advance gate. 0x649589 is the fall-through init of the gate block
// (mov edx, [Frame]); one instruction, not a branch target, fires once per
// wait iteration in-game.
DEFINE_HOOK(0x649589, WaitForPlayers_FrameGate_NetLog, 0x6)
{
	NetTelemetry::OnGateEval();
	return 0;
}

// Process_Receive_Packet entry. EDX = packet (fastcall 2nd arg).
DEFINE_HOOK(0x64A0C0, ProcessReceivePacket_NetLog, 0x6)
{
	NetTelemetry::OnReceivePacket(reinterpret_cast<const unsigned char*>(R->EDX()));
	return 0;
}

// IPXManagerClass::Send_Private_Message entry (thiscall). Stack: a2@+4,
// a3(len)@+8, a5(connId)@+0x10. connId == -1 is the broadcast fan-out.
DEFINE_HOOK(0x5414C0, IPXSendPrivateMessage_Fanout_NetLog, 0x5)
{
	NetTelemetry::OnFanout(
		R->Stack<int>(0x10),
		reinterpret_cast<const unsigned char*>(R->Stack<DWORD>(0x4)),
		R->Stack<int>(0x8));
	return 0;
}

// ConnectionClass::Service_Send_Queue entry (thiscall). ECX = this.
DEFINE_HOOK(0x48C3E0, ServiceSendQueue_NetLog, 0x5)
{
	NetTelemetry::OnServiceSendQueue(reinterpret_cast<const unsigned char*>(R->ECX()));
	return 0;
}

// Immediately after Send_Packets returns. EAX = commands sent this period.
DEFINE_HOOK(0x647FB9, SendPackets_Period_NetLog, 0x5)
{
	NetTelemetry::OnSendPeriod(R->EAX<int>());
	return 0;
}

// Send_FrameSync entry (fastcall). EDX = my_sent (our cumulative count).
DEFINE_HOOK(0x64A000, SendFrameSync_NetLog, 0x5)
{
	NetTelemetry::OnFrameSyncSent(static_cast<short>(R->EDX() & 0xFFFF));
	return 0;
}

// The mis-indexed CommandCoundStalls increment. EBP = the index the engine
// uses (v79); it is -1 in the common packet-loss case. We only observe -
// the original inc still runs on return 0, so the bug is left intact.
DEFINE_HOOK(0x6497DC, WaitForPlayers_CommandStallMisindex_NetLog, 0x7)
{
	NetTelemetry::OnCommandStallMisindex(R->EBP<int>());
	return 0;
}
