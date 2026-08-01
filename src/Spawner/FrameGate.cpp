/**
*  yrpp-spawner
*
*  FrameGate - frame-aware advance gate. See FrameGate.h for the rationale.
*/

#include "FrameGate.h"
#include "NetTelemetry.h"

#include <Helpers/Macro.h>       // DEFINE_HOOK, REGISTERS
#include <Fundamentals.h>        // Unsorted::CurrentFrame
#include <Unsorted.h>            // Game::Network::MaxAhead / FrameSendRate
#include <SessionClass.h>        // SessionClass::Instance.GameMode
#include <GeneralDefinitions.h>  // GameMode
#include <IPXManagerClass.h>     // IPXManagerClass::Instance.NumConnections

#include <climits>

bool FrameGate::Enabled = true;

namespace
{
	// Per-peer sync state (array base 0xAFA358, stride 0x18). frame@0, sent@4, recv@8.
	struct TheirSync { int frame, cmd_sent, cmd_recv, f0C, f10, f14; };
	static_assert(sizeof(TheirSync) == 0x18, "theirStruct stride must be 0x18");
	TheirSync* const Their = reinterpret_cast<TheirSync*>(0xAFA358);
	const unsigned TheirBase = 0xAFA358;

	// Highest frame stamp F for which we hold peer i's complete command prefix
	// (all commands stamped <= F are in DoList). INT_MIN = nothing established.
	int  SafeThrough[8];
	bool Inited = false;

	// Epoch tracking. MaxAhead/FrameSendRate changes re-stamp future commands,
	// so on any change we drop the watermarks and fall back to vanilla until
	// in-flight old-epoch commands have drained. Today the ladder only grows
	// MaxAhead (the safe direction); this keeps us correct if a bidirectional
	// ladder ever shrinks it.
	int  lastMaxAhead = -1;
	int  lastFSR = -1;
	int  guardUntilFrame = 0;

	void InitOnce()
	{
		if (Inited)
			return;
		for (int i = 0; i < 8; ++i)
			SafeThrough[i] = INT_MIN;
		Inited = true;
	}

	// Re-arm the epoch guard and clear watermarks only when timing SHRINKS.
	//
	// A watermark SafeThrough[i]=F means "peer i's commands stamped <= F are all
	// in hand", which relies on stamp order matching command order. Growing
	// MaxAhead/FrameSendRate keeps that order intact (later commands stamp
	// higher), so every existing watermark stays valid - we must NOT reset on
	// growth, or the gate would sit disabled for the whole game while
	// ProtocolZero's latency ladder ramps MaxAhead UP under loss, which is
	// exactly when the gate is needed. Only a DECREASE can give a later command
	// a lower stamp than an older one, breaking the invariant; then we clear and
	// fall back to vanilla until fresh packets re-establish the watermarks.
	void CheckEpoch(int frame, int ma, int fsr)
	{
		bool shrink = (lastMaxAhead >= 0 && ma < lastMaxAhead)
		           || (lastFSR      >= 0 && fsr < lastFSR);
		if (shrink)
		{
			for (int i = 0; i < 8; ++i)
				SafeThrough[i] = INT_MIN;
			guardUntilFrame = frame + lastMaxAhead + fsr + 4;
		}
		lastMaxAhead = ma;
		lastFSR = fsr;
	}
}

bool FrameGate::RelaxCovers(int peer, int frame)
{
	if (!Enabled || peer < 0 || peer >= 8)
		return false;
	if (frame < guardUntilFrame)
		return false;
	return SafeThrough[peer] >= frame;
}

bool FrameGate::AllCommandsSatisfied(int* gapIndex)
{
	InitOnce();
	*gapIndex = -1;

	static bool announced = false;
	if (!announced)
	{
		announced = true;
		NetTelemetry::Log("FrameGate   loaded (fix1: frame-aware gate), Enabled=%d", Enabled ? 1 : 0);
	}

	int nconn = static_cast<int>(IPXManagerClass::Instance.NumConnections);
	if (nconn < 0) nconn = 0;
	if (nconn > 8) nconn = 8;

	const int frame = Unsorted::CurrentFrame;
	const int ma    = Game::Network::MaxAhead;
	const int fsr   = Game::Network::FrameSendRate;

	CheckEpoch(frame, ma, fsr);
	const bool relaxOK = Enabled && frame >= guardUntilFrame;

	bool allSat = true;
	bool relaxedAGap = false;
	int  minFrame = INT_MAX;

	for (int i = 0; i < nconn; ++i)
	{
		int f = Their[i].frame;
		if (f < minFrame)
			minFrame = f;

		if (Their[i].cmd_recv >= Their[i].cmd_sent)
			continue;                                   // vanilla-satisfied

		if (relaxOK && SafeThrough[i] >= frame)
		{
			relaxedAGap = true;                          // frame-aware: safe to skip
			continue;
		}

		allSat = false;                                  // genuinely blocked
		if (*gapIndex < 0)
			*gapIndex = i;
	}

	// GATE-EARLY: count frames the frame-aware gate lets through that the
	// vanilla count test would have stalled. Only when a gap was relaxed AND
	// the MaxAhead check downstream will actually let the frame advance.
	if (allSat && relaxedAGap && frame < ma + minFrame)
		NetTelemetry::Log("GATE-EARLY  frame=%d (frame-aware gate advanced where the old command-count gate would have stalled)", frame);

	return allSat;
}

void FrameGate::OnReceive(unsigned int theirEntry, const unsigned char* ev)
{
	InitOnce();
	if (!ev || theirEntry < TheirBase)
		return;
	unsigned idx = (theirEntry - TheirBase) / sizeof(TheirSync);
	if (idx >= 8)
		return;

	// Suppress watermark updates during the post-timing-change guard window, so
	// a stamp computed under the old MaxAhead is never trusted under the new one.
	if (Unsorted::CurrentFrame < guardUntilFrame)
		return;

	const int F = *reinterpret_cast<const int*>(ev + 3);            // event->Frame (the stamp)
	const int C = *reinterpret_cast<const unsigned short*>(ev + 11); // cumulative count BEFORE this block

	// C is the sender's cumulative count *before* this block's own commands, so
	// commands 1..C were all sent in earlier send periods, whose stamps are all
	// strictly less than this packet's stamp F (each period gets a distinct,
	// increasing stamp). If we hold them (recv >= C) we therefore hold EVERY
	// command stamped < F -> it is safe to execute any frame up to F-1.
	//
	// We must NOT claim F itself: one send period can emit several blocks that
	// all carry stamp F (a large burst of orders), and a later same-stamp block
	// can still be missing. Claiming F would then let us execute frame F without
	// that block's commands -> desync. F-1 excludes exactly that uncertainty and
	// costs only a single frame of runway.
	const int recv = reinterpret_cast<const TheirSync*>(theirEntry)->cmd_recv;
	if (C <= recv && (F - 1) > SafeThrough[idx])
		SafeThrough[idx] = F - 1;
}

// ==========================================================================
// Hooks
// ==========================================================================

// Replaces the vanilla command-count loop (loop B) in Wait_For_Players.
// 0x6495D5 is the single 7-byte 'mov ecx,[esp+748h]' that loads `their` and
// begins loop B; it is reached only when NumConnections>0. loop A has already
// set EBX=min-peer-frame and EBP=culprit-index, which the downstream vanilla
// code needs - we preserve both and return its own continuation addresses:
//   0x6495F9 = all commands satisfied -> vanilla MaxAhead check
//   0x649610 = a peer is genuinely missing a needed command -> stall path
// Returning 0 executes the stolen mov and falls into vanilla loop B unchanged
// (used for non-internet games, where NumConnections is not the Ipx count).
DEFINE_HOOK(0x6495D5, WaitForPlayers_FrameAwareGate, 0x7)
{
	// CnCNet games actually run as GameMode::LAN (see Spawner.cpp: the mode is
	// set to Internet then "HACK: set to LAN later"). Both LAN and Internet use
	// the engine's Ipx/UDP net path, so Ipx.NumConnections is valid for both;
	// only campaign/skirmish must fall back to the vanilla loop.
	const GameMode gm = SessionClass::Instance.GameMode;
	if (gm != GameMode::LAN && gm != GameMode::Internet)
		return 0;

	int gap = -1;
	if (FrameGate::AllCommandsSatisfied(&gap))
		return 0x6495F9;

	R->ESI(gap);       // blocking peer index, for the engine's stall bookkeeping
	return 0x649610;
}

// Records the per-peer watermark. 0x64A3F9 is the convergence point in
// Process_Receive_Packet for both data (type 28) and framesync (type 25),
// reached after frame/__send are updated and before the type split. EBP =
// &their[index], EDI = the packet. Observe-only (returns 0).
DEFINE_HOOK(0x64A3F9, ProcessReceivePacket_FrameGateRecord, 0x9)
{
	FrameGate::OnReceive(R->EBP(), reinterpret_cast<const unsigned char*>(R->EDI()));
	return 0;
}
