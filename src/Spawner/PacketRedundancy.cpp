/**
*  yrpp-spawner
*
*  PacketRedundancy - see PacketRedundancy.h.
*/

#include "PacketRedundancy.h"
#include "NetTelemetry.h"

#include <windows.h>

bool PacketRedundancy::Enabled  = true;
int  PacketRedundancy::Copies   = 2;
bool PacketRedundancy::Adaptive = false;

namespace
{
	// On-wire layout at the send hook: [CRC(4)][CommHeaderType...]; CommHeader is
	// MagicNumber(u16)@0, Code(u8)@2, so the packet Code sits at byte 6. Code 0 ==
	// PACKET_DATA_ACK == a reliable, command-bearing packet (verified in
	// ConnectionClass::Send_Packet 0x48BF40).
	const int           WireCodeOffset = 6;
	const unsigned char Packet_Data_Ack = 0;

	// Adaptive loss gauge: each detected resend bumps it; it decays with wall
	// time. "Loss present" = gauge above zero, i.e. a resend within ~the last
	// few seconds. Slow decay + accumulation gives hysteresis so it does not
	// flap on/off packet-to-packet.
	const int  GaugeBump   = 1000;   // per resend
	const int  GaugeCap    = 5000;   // ~5s of memory
	const int  GaugeMsPerUnit = 1;   // decay 1000 units/sec
	int   g_gauge    = 0;
	DWORD g_lastTick = 0;

	void DecayGauge()
	{
		DWORD now = GetTickCount();
		if (g_lastTick == 0) { g_lastTick = now; return; }
		DWORD dt = now - g_lastTick;
		g_lastTick = now;
		int dec = (int)dt / GaugeMsPerUnit;
		g_gauge = (dec >= g_gauge) ? 0 : (g_gauge - dec);
	}
}

void PacketRedundancy::NoteResend()
{
	DecayGauge();
	g_gauge += GaugeBump;
	if (g_gauge > GaugeCap)
		g_gauge = GaugeCap;
}

int PacketRedundancy::CopiesFor(const char* buf, size_t len)
{
	// TEMP diagnostics: log the branch taken for the first 30 calls so we can
	// confirm this runs and see why it does / does not duplicate.
	static int diag = 0;
	const bool log = (diag < 30);
	if (log) ++diag;

	if (!Enabled)
	{
		if (log) NetTelemetry::Log("Redun[%d]: once - Enabled=0", diag);
		return 1;
	}
	if (Copies < 2 || !buf || len <= (size_t)WireCodeOffset)
	{
		if (log) NetTelemetry::Log("Redun[%d]: once - Copies=%d len=%u", diag, Copies, (unsigned)len);
		return 1;
	}

	const unsigned char code = (unsigned char)buf[WireCodeOffset];
	if (code != Packet_Data_Ack)
	{
		if (log) NetTelemetry::Log("Redun[%d]: once - code@6=%u (not DATA_ACK) len=%u", diag, code, (unsigned)len);
		return 1;
	}

	int copies = Copies;
	if (Adaptive)
	{
		DecayGauge();
		if (g_gauge <= 0)
			copies = 1; // clean link - do not duplicate
	}

	if (log) NetTelemetry::Log("Redun[%d]: DATA_ACK len=%u -> %d copies (adaptive=%d gauge=%d)",
		diag, (unsigned)len, copies, Adaptive ? 1 : 0, g_gauge);
	return copies;
}

/* ==========================================================================
 * PRIVATE-SUBMODULE CHANGE (apply by hand before the PR)
 * --------------------------------------------------------------------------
 * The actual send path that duplicates packets lives in the CnCNet-private
 * submodule (Private/Replacements/NetHack.cpp), which replaces
 * src/Spawner/NetHack.cpp in Hardened builds. That file is NOT committed on
 * this branch (we don't want to touch CnCNet's private repo yet), so the
 * change is recorded here. When ready, apply the following to
 * Private/Replacements/NetHack.cpp:
 *
 * 1) Add includes near the top:
 *        #include <Spawner/PacketRedundancy.h>
 *
 * 2) In `int WINAPI Tunnel::SendTo(...)`, decide the copy count on the plain
 *    game bytes BEFORE they are xor-obfuscated into acBuf, then send the extra
 *    copies after the normal sendto:
 *
 *        // fix3: reliable command packets get sent extra times so an isolated
 *        // loss needs no retransmit (peer's ARQ discards the duplicate).
 *        const int copies = PacketRedundancy::CopiesFor(buf, len);
 *
 *        ... existing xormemcpy / crc / header / sendto ...
 *        int ret = sendto(sockfd, (char*)&acBuf, len + 8, flags,
 *                         (struct sockaddr*)dest_addr, addrlen);
 *
 *        // Extra identical copies (same bytes / PacketID -> deduped by peer).
 *        for (int i = 1; i < copies; ++i)
 *            sendto(sockfd, (char*)&acBuf, len + 8, flags,
 *                   (struct sockaddr*)dest_addr, addrlen);
 *        return ret;
 *
 * (The non-Hardened src/Spawner/NetHack.cpp copy already contains the
 *  equivalent change and IS committed on this branch, for reference.)
 * ========================================================================== */
