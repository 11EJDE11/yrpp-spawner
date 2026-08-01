/**
*  yrpp-spawner
*
*  NetTelemetry - lag-investigation logging (TESTING ONLY).
*
*  Writes a self-contained plain-text log ("spawner_netlog.txt", next to the
*  running game) that backs up the netcode claims in the lag write-up. It does
*  NOT depend on the game's own WWDebugString / debug.log being enabled, so it
*  works on a stock build. Intended for a two-machine test with something like
*  Clumsy inducing loss on one side.
*
*  Enabled by default. Set NetTelemetry::Enabled = false (or edit the default
*  below) to silence it.
*
*  Every hook this drives is observe-only: each returns 0 so the original code
*  runs untouched. The instrumented sites (all verified in gamemd_rest.i64):
*
*    0x649589  Wait_For_Players  - the frame-advance gate (the centrepiece)
*    0x64A0C0  Process_Receive_Packet - inbound packets (beacons vs data)
*    0x5414C0  IPXManager::Send_Private_Message - the N-1 fan-out
*    0x48C3E0  ConnectionClass::Service_Send_Queue - retransmits
*    0x647FB9  after Send_Packets - commands sent per send-period
*    0x64A000  Send_FrameSync - our outbound keepalive beacon
*    0x6497DC  Wait_For_Players - the MPStats[-1] mis-index bug, observed live
*/

#pragma once
#include <windows.h>

class NetTelemetry
{
public:
	// On by default. Flip to false to disable all logging with no other change.
	static bool Enabled;

	// Timestamped line writer (own file, flushed each line).
	static void Log(const char* pFormat, ...);

	// Hook callbacks (see addresses above).
	static void OnGateEval();                                   // frame-advance gate
	static void OnReceivePacket(const unsigned char* pkt);      // inbound packet header
	static void OnFanout(int connId, const unsigned char* buf, int len);
	static void OnServiceSendQueue(const unsigned char* conn);  // retransmit accounting
	static void OnSendPeriod(int rawCommands);                  // Send_Packets result
	static void OnFrameSyncSent(int mySent);                    // outbound beacon
	static void OnCommandStallMisindex(int idx);               // the MPStats[-1] bug
};
