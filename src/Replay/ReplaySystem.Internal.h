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
// GameCRC - the engine's own per-frame desync hash, as Compute_Game_CRC (0x64DAB0) leaves it.
// Queue_AI_Multiplayer calls that once per frame and then stores the result into CRC[Frame & 0xFF]
// for the network sync check; the replay system reads the same value and never computes its own.
constexpr uintptr_t GAME_CRC_ADDRESS = 0xAC51FC;
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
	bool EndOfStream = false;
};

// One frame's worth of visible state, captured in the main loop and written out once
// Queue_AI_Multiplayer knows how many events the frame carried.
struct PendingRecordedFrameCapture
{
	int FrameNumber = 0;
	Point2D TacticalPos { 0, 0 };
	std::vector<uint32_t> SelectedObjectIDs;
	// Filled in later than the rest of the capture: the hash only exists once the engine has
	// computed it, which happens after this struct is created. See CaptureGameCRCForCurrentFrame.
	uint32_t GameCRC = 0;
	bool HasGameCRC = false;
};

struct ReplayRuntimeState
{
	bool Recording = false;
	bool Playback = false;
	bool InitRandomHandled = false;
	// Set once the load-progress bar has been force-completed for this scenario (playback,
	// skirmish, or campaign) instead of animating. See WaitForPlayers_SkipProgressAnimation.
	bool ProgressBarForcedComplete = false;

	bool ShroudEnabled = false;
	bool LockViewport = true;
	bool SelectUnits = true;
	bool SpectatorView = false;
	// Playback-only; recording keeps this data unconditionally.
	bool ShowChatAndBeacons = true;
	int PlaybackSpeedIndex = -1;

	int ExpectedEventsThisFrame = 0;
	uint64_t BytesAtLastDiskFlush = 0;
	int LastSyncFlushFrame = 0;

	// Desync detection. The recorded hash for the frame being played back, consumed once by the
	// comparison and cleared again every frame, so a frame the recording has no record for is
	// simply not checked rather than checked against a stale value.
	uint32_t ExpectedGameCRC = 0;
	bool HasExpectedGameCRC = false;
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

// Why a replay could not be opened for playback. Playback failure is fatal - StartScenario has
// already skipped CreateConnections, so there is no live session to fall back on - which makes the
// message the last thing anyone sees. Version trouble is the failure a format change produces most
// of, so it says so rather than being folded into a generic read error.
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
void ApplyPlaybackInitialState();
bool ReadReplayHeaderFromPath(const char* replayPath, ReplayHeader& outHeader);

// Per-frame work, driven from the main loop and from Queue_AI_Multiplayer.
void RecordFrameState();
void RestoreFrameState();
void CaptureGameCRCForCurrentFrame();
void ApplyReplayTimingFromCurrentGameSpeed();
void RecordEventsForCurrentFrame();
void RemoveReplayGameplayEventsFromDoList();
void PlaybackFrameEvents();

// True while playback is showing the whole map rather than the recording player's shroud.
bool PlaybackWantsFullMapReveal();

} // namespace Internal

} // namespace ReplaySystem
