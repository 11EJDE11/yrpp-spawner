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

// Runtime state and the entry points the replay hooks drive. Only src/Replay includes this; the
// public surface is ReplaySystem.h.

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

		// How often the compressed stream is given a decodable point. A recording cut short by a
		// crash loses at most this many frames.
		constexpr int SyncFlushFrameInterval = 60;
		// Forced disk commits stall the game thread, so they run on a byte count rather than the
		// frame cadence above.
		constexpr uint64_t DiskFlushIntervalBytes = 50ull * 1024 * 1024;
		constexpr const char* DefaultRecordingPath = "replay.yrrp";

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

		// One frame's worth of visible state, captured in the main loop and written out once the
		// queue hook knows how many events the frame carried.
		struct PendingRecordedFrameCapture
		{
			int FrameNumber = 0;
			Point2D TacticalPos { 0, 0 };
			std::vector<uint32_t> SelectedObjectIDs;
			// Filled in later than the rest of the capture: the engine has not computed the hash
			// when this struct is created. See CaptureGameCRCForCurrentFrame.
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
			// Latched once a campaign mission's recording has been closed out, and refuses every
			// later StartReplayRecording for the rest of the game. Cleared once per game by
			// ReplaySystem::OnGameStartReset, not by ResetRuntimeFlagsForScenario, which runs for
			// every scenario including the suppressed ones. See FinishRecordingAtMissionEnd.
			bool RecordingFinishedForSession = false;
			// Set once the load-progress bar has been forced complete for this scenario instead of
			// animating. See WaitForPlayers_SkipProgressAnimation.
			bool ProgressBarForcedComplete = false;

			bool ShroudEnabled = false;
			bool LockViewport = true;
			bool SelectUnits = true;
			bool SpectatorView = false;
			// Playback only; recording keeps this data unconditionally.
			bool ShowChatAndBeacons = true;

			// Playback pacing, as a target frame rate rather than a game-speed index: the ladder
			// the viewer hotkeys walk runs past 60 FPS, which no game-speed index can express.
			// 0 until playback starts. See ReplayControls.h.
			int PlaybackFPS = 0;
			// Performance-counter deadline for the next playback frame, in milliseconds. Zero
			// means the pacing has not started yet, or needs resyncing to now.
			double PlaybackNextFrameDue = 0.0;
			// Set by the pause hotkey. While it is set the main loop still renders, scrolls and
			// takes input, but LogicClass::AI, Queue_AI and the frame counter are all held.
			bool PlaybackPaused = false;

			int ExpectedEventsThisFrame = 0;
			uint64_t BytesAtLastDiskFlush = 0;
			int LastSyncFlushFrame = 0;

			// The recorded hash for the frame being played back, consumed once by the comparison
			// and cleared again every frame, so a frame the recording has no record for is not
			// checked rather than checked against a stale value.
			uint32_t ExpectedGameCRC = 0;
			bool HasExpectedGameCRC = false;
			FrameObjectCensus ExpectedCensus { 0, 0 };
			bool HasExpectedCensus = false;
			bool CensusMismatchReported = false;
			// The game speed as last written into the stream, so only changes are recorded. -1
			// forces the first frame to establish the baseline.
			int LastRecordedGameSpeed = -1;
			bool DivergenceReported = false;

			// Divergence diagnostics, logged as a summary when playback ends.
			int CheckedFrameCount = 0;
			int MismatchedFrameCount = 0;
			int LoggedMismatchCount = 0;
			int LoggedRecoveryCount = 0;
			int FirstMismatchFrame = -1;
			int LastMismatchFrame = -1;
			// Whether the previous checked frame mismatched, so a return to matching is reported.
			bool LastCheckMismatched = false;

			HANDLE ReplayFile = INVALID_HANDLE_VALUE;
			// Only one of these is ever active: recording deflates, playback inflates.
			Replay::DeflateWriter Writer;
			Replay::InflateReader Reader;

			char PlaybackPath[MAX_PATH] = { 0 };
			std::deque<PendingRecordedFrameCapture> PendingFrameStates;
			std::deque<SideChannelRecord> PendingSideChannelEvents;
			int CapturedFrameEventsFrame = -1;
			// This frame's events, appended by RecordExecutedEvent as Execute_DoList consumes
			// each one.
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

		// Why a replay could not be opened for playback. Failure is fatal and the message is the
		// last thing the player sees, so a version mismatch says so on its own.
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

		// Per-frame work, driven from the main loop and from the queue event hooks.
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
		void RecordExecutedEvent(const EventClass* pEvent);
		void RecordCapturedEventsForCurrentFrame();
		void RemoveReplayGameplayEventsFromDoList();
		void PlaybackFrameEvents();

		// True while playback shows the whole map rather than the recording player's shroud. The
		// reveal is applied by the draw-time hooks in ReplaySystem.Hook.cpp and never writes cell
		// state, so the simulation sees the recording player's real shroud at all times.
		bool PlaybackWantsFullMapReveal();
	}
}
