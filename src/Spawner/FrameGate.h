/**
*  yrpp-spawner
*
*  FrameGate - frame-aware advance gate (FIX #1).
*
*  Vanilla RA2 gates frame advance on a raw per-peer command COUNT
*  (their[i].__recv >= their[i].__send). That test carries no frame
*  information, so a single lost command packet stalls the whole lobby even
*  though the missing commands are stamped for a frame nobody has reached yet -
*  and MaxAhead provides no protection because its check is skipped once the
*  count test fails. See the lag write-up, sections 2-3.
*
*  This replaces the count test with the one it was approximating: we may
*  execute the current frame if, for every peer, all of that peer's commands
*  stamped for frames <= the current frame are already in hand. We prove that
*  from the packet stream: a packet stamped F carrying cumulative count C means
*  "commands 1..C are all stamped <= F"; once our __recv >= C we hold them, so
*  every frame <= F is safe with respect to that peer.
*
*  Determinism is untouched: the set of events executed on each frame number is
*  identical to vanilla; only the wall-clock moment we reach a frame changes. We
*  never advance past a frame whose commands we do not hold - the exact
*  invariant vanilla maintains, stated precisely instead of conservatively.
*
*  Purely local: changes only when THIS client advances, never what it sends or
*  executes. A patched and an unpatched client can share a lobby.
*
*  Instrumented addresses (verified in gamemd_rest.i64):
*    0x6495D5  Wait_For_Players - replaces the command-count loop (loop B)
*    0x64A3F9  Process_Receive_Packet - records the per-peer watermark
*/

#pragma once

class FrameGate
{
public:
	// Master switch. Default on. When false the gate behaves EXACTLY like
	// vanilla (the relaxation is disabled), so flipping this gives a clean
	// before/after in the same build.
	static bool Enabled;

	// Called in place of the vanilla command-count loop. Returns true if every
	// peer's commands for the current frame are in hand (vanilla recv>=send OR
	// the frame-aware relaxation). On false, *gapIndex is the first blocking
	// peer, for the engine's existing stall bookkeeping.
	static bool AllCommandsSatisfied(int* gapIndex);

	// Records the watermark for one received data/framesync packet.
	// theirEntry = &their[index]; ev = the packet header.
	static void OnReceive(unsigned int theirEntry, const unsigned char* ev);

	// For the telemetry module, so its stall accounting matches the real gate:
	// does the frame-aware relaxation currently cover this peer at this frame?
	static bool RelaxCovers(int peer, int frame);
};
