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

// Runtime state and the internal entry points the replay hooks drive. Split out of
// ReplaySystem.cpp so that ReplaySystem.Hook.cpp can reach them; nothing outside src/Replay should
// include this. The public surface is ReplaySystem.h.

#include "ReplayFormat.h"
#include "ReplayStream.h"

#include <Spawner/Spawner.Config.h>

#include <EventClass.h>
#include <GeneralStructures.h>

#include <deque>
#include <vector>

namespace ReplaySystem
{

namespace Internal
{

using namespace Replay;

// How often the compressed stream is given a decodable point. A recording cut short by a crash
// loses at most this many frames - one second of play - and the cadence costs about 1% of ratio.
constexpr int REPLAY_SYNC_FLUSH_FRAME_INTERVAL = 60;
// Flush rarely; forced disk commits can stall the game thread.
constexpr uint64_t REPLAY_FLUSH_INTERVAL_BYTES = 50ull * 1024 * 1024;
constexpr const char* DEFAULT_RECORDING_PATH = "replay.dat";

// Engine entry points and globals the replay system reaches directly. They belong in YRpp; until
// they are declared there, keep every raw address in one place rather than inline at the call sites.
// Queue_AI_Multiplayer's network delay budget.
constexpr uintptr_t NETWORK_DELAY_TIME_ADDRESS = 0x00AFA458;
// BeaconManagerClass::Message - sets the text of an existing beacon.
constexpr uintptr_t BEACON_MANAGER_MESSAGE_ADDRESS = 0x431450;
// Taunts - resolves a taunt command byte and plays the matching sound.
constexpr uintptr_t TAUNTS_ADDRESS = 0x752B70;
// TacticalClass::RecalculateViewport - recomputes TacticalPos and the visible area.
constexpr uintptr_t TACTICAL_RECALCULATE_VIEWPORT_ADDRESS = 0x6D8B30;
// Tactical::AI - commits the desired viewport position into the one that gets drawn. Normally
// reached from LogicClass::AI, which a paused playback frame skips.
constexpr uintptr_t TACTICAL_AI_ADDRESS = 0x6D2540;
// Compute_Game_CRC - the engine's own per-frame desync hash calculator.
constexpr uintptr_t COMPUTE_GAME_CRC_ADDRESS = 0x64DAB0;
// GameCRC - the engine's own per-frame desync hash. A networked game fills it once a frame for
// the sync check; a skirmish never does, so the replay hooks compute it themselves.
constexpr uintptr_t GAME_CRC_ADDRESS = 0xAC51FC;
// The two frame timers Sync_Delay sleeps on: DelayTime in 60Hz ticks for a skirmish,
// Accumulated in milliseconds for a networked session. Playback writes these directly - see
// ApplyPlaybackFramePacing.
constexpr uintptr_t FRAME_TIMER_DELAY_TIME_ADDRESS = 0x887350;
constexpr uintptr_t NFT_TIMER_ACCUMULATED_ADDRESS = 0x887330;
// BeaconClass::Bitfield flag marking the beacon the local player placed.
constexpr int BEACON_FLAG_LOCAL = 2;
// Taunt commands are the low nibble of the command byte, so 16 of them.
constexpr int MAX_TAUNT_COMMAND_COUNT = 16;

struct PlaybackFrameRecord
{
	int32_t FrameNumber = 0;
	int32_t EventCountThisFrame = 0;
	uint32_t Flags = FrameRecordFlag_None;
	Point2D TacticalPos = { 0, 0 };
	int32_t SelectedObjectCount = 0;
	std::vector<uint32_t> SelectedObjectIDs;
	std::vector<SideChannelRecord> SideChannelEvents;
	uint32_t GameCRC = 0;
	FrameObjectCensus Census { 0, 0 };
	int32_t GameSpeed = 0;
	bool EndOfStream = false;
};

// One frame's worth of visible state, captured in the main loop and written out once
// the active queue hook knows how many events the frame carried.
struct PendingRecordedFrameCapture
{
	int FrameNumber = 0;
	Point2D TacticalPos { 0, 0 };
	std::vector<uint32_t> SelectedObjectIDs;
	// Filled in later than the rest of the capture: the hash only exists once the engine has
	// computed it, which happens after this struct is created. See CaptureGameCRCForCurrentFrame.
	uint32_t GameCRC = 0;
	bool HasGameCRC = false;
	FrameObjectCensus Census { 0, 0 };
	bool HasCensus = false;
	int32_t GameSpeed = 0;
	bool HasGameSpeed = false;
};

struct ReplayRuntimeState
{
	bool Recording = false;
	bool Playback = false;
	bool InitRandomHandled = false;
	// Latched once a campaign mission's recording has been closed out, and refuses every later
	// StartReplayRecording for the rest of the game - see FinishRecordingAtMissionEnd. Deliberately
	// not cleared by ResetRuntimeFlagsForScenario, which runs for every scenario including the
	// suppressed ones; ReplaySystem::OnGameStartReset clears it, once per game.
	bool RecordingFinishedForSession = false;
	// Set once the load-progress bar has been force-completed for this scenario (playback,
	// skirmish, or campaign) instead of animating. See WaitForPlayers_SkipProgressAnimation.
	bool ProgressBarForcedComplete = false;

	bool ShroudEnabled = false;
	bool LockViewport = true;
	bool SelectUnits = true;
	bool SpectatorView = false;
	// Playback-only; recording keeps this data unconditionally.
	bool ShowChatAndBeacons = true;

	// Playback pacing, as a target frame rate rather than a game-speed index: the ladder the
	// viewer hotkeys walk runs past 60 FPS, which no game-speed index can express. 0 until
	// playback starts. See Replay/ReplayControls.h.
	int PlaybackFPS = 0;
	// Performance-counter deadline for the next playback frame, in milliseconds. Zero means
	// the pacing has not started yet, or needs resyncing to now.
	double PlaybackNextFrameDue = 0.0;
	// Set by the pause hotkey. While it is set the main loop still renders, scrolls and takes
	// input, but LogicClass::AI, Queue_AI and the frame counter are all held.
	bool PlaybackPaused = false;

	int ExpectedEventsThisFrame = 0;
	uint64_t BytesAtLastDiskFlush = 0;
	int LastSyncFlushFrame = 0;

	// Desync detection. The recorded hash for the frame being played back, consumed once by the
	// comparison and cleared again every frame, so a frame the recording has no record for is
	// simply not checked rather than checked against a stale value.
	uint32_t ExpectedGameCRC = 0;
	bool HasExpectedGameCRC = false;
	FrameObjectCensus ExpectedCensus { 0, 0 };
	bool HasExpectedCensus = false;
	bool CensusMismatchReported = false;
	// The game speed as last written into the stream, so only changes are recorded. -1 forces
	// the first frame to establish the baseline.
	int LastRecordedGameSpeed = -1;
	bool DivergenceReported = false;

	// Divergence diagnostics, all logged as a summary when playback ends.
	int CheckedFrameCount = 0;
	int MismatchedFrameCount = 0;
	int LoggedMismatchCount = 0;
	int LoggedRecoveryCount = 0;
	int FirstMismatchFrame = -1;
	int LastMismatchFrame = -1;
	// Whether the previous checked frame mismatched, so that a return to matching can be reported.
	bool LastCheckMismatched = false;

	HANDLE ReplayFile = INVALID_HANDLE_VALUE;
	// Only one of these is ever active: recording deflates, playback inflates.
	Replay::DeflateWriter Writer;
	Replay::InflateReader Reader;

	char PlaybackPath[MAX_PATH] = { 0 };
	std::deque<PendingRecordedFrameCapture> PendingFrameStates;
	std::deque<SideChannelRecord> PendingSideChannelEvents;
	int CapturedFrameEventsFrame = -1;
	// The unconsumed DoList entries seen before Execute_DoList ran; afterwards, the ones it marked
	// executed are this frame's events. Pointers rather than queue positions on purpose: the queue's
	// backing array is a fixed member of a static object, so an entry's address never changes, which
	// a position relative to the head would not survive if anything ever moved it.
	std::vector<const EventClass*> CandidateEvents;
	std::vector<EventClass> CapturedFrameEvents;
	ReplayHeader PlaybackHeader = {};
	bool HasPlaybackHeader = false;

	bool HasPendingPlaybackFrame = false;
	bool PlaybackStreamEnded = false;

	// Last recorded viewport position, re-applied between sparse frame records.
	Point2D LockedViewportPos = { 0, 0 };
	bool HasLockedViewportPos = false;
	PlaybackFrameRecord PendingPlaybackFrame = {};

	bool HasLastWrittenFrameState = false;
	int32_t LastWrittenFrameNumber = 0;
	Point2D LastRecordedTacticalPos = { 0, 0 };
	std::vector<uint32_t> LastRecordedSelectionIDs;

	// Scratch buffers reused every frame so recording does not allocate on the game thread.
	std::vector<SideChannelRecord> SideChannelScratch;
	std::vector<EventClass> PreservedEventsScratch;
};

extern ReplayRuntimeState ReplayState;

const SpawnerConfig* GetConfig();

// Why a replay could not be opened for playback. Failure is fatal and the message is the last
// thing the player sees, so a version mismatch - the likeliest cause - says so on its own.
enum class ReplayOpenFailure
{
	None,
	// The file could not be opened, or ended inside the header.
	Unreadable,
	// No magic number: not a replay at all.
	NotAReplay,
	// A replay, from a layout generation this build does not know.
	UnsupportedVersion,
	// Right generation, but the header does not describe a file this shape.
	Malformed
};

const char* DescribeReplayOpenFailure(ReplayOpenFailure failure);

// Lifecycle.
void StartReplayRecording();
void StartReplayPlayback(const char* replayPath);
void StopReplaySystem();
void FinishRecordingAtMissionEnd();
void ApplyPlaybackInitialState();
bool ReadReplayHeaderFromPath(const char* replayPath, ReplayHeader& outHeader);

// Per-frame work, driven from the main loop and from the active queue event hook.
void RecordFrameState();
void RestoreFrameState();
void CaptureGameCRCForCurrentFrame();
FrameObjectCensus CurrentObjectCensus();
void CheckObjectCensusForCurrentFrame();
void ComputeAndCaptureGameCRCForCurrentFrame();
int GetPlaybackTargetFPS();
double PlaybackClockMilliseconds();
void ApplyPlaybackFramePacing();
void CaptureEventsForCurrentFrame();
void RecordCapturedEventsForCurrentFrame();
void RemoveReplayGameplayEventsFromDoList();
void PlaybackFrameEvents();

// True while playback is showing the whole map rather than the recording player's shroud.
bool PlaybackWantsFullMapReveal();

} // namespace Internal

} // namespace ReplaySystem
