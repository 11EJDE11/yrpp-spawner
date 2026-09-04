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

class TechnoClass;

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
			std::vector<uint32_t> SelectionTriggerObjectIDs;
			std::vector<SideChannelRecord> SideChannelEvents;
			uint32_t GameCRC = 0;
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
			// Appended to during the frame rather than sampled once, because the spring happens in
			// the input pass after this capture is made and before Queue_AI writes it out.
			std::vector<uint32_t> SelectionTriggerObjectIDs;
			// Filled in later than the rest of the capture: the engine has not computed the hash
			// when this struct is created. See CaptureGameCRCForCurrentFrame.
			uint32_t GameCRC = 0;
			bool HasGameCRC = false;
			int32_t GameSpeed = 0;
			bool HasGameSpeed = false;
		};

		struct ReplayRuntimeState
		{
			bool Recording = false;
			bool Playback = false;
			bool InitRandomHandled = false;
			bool RecordingFinishedForSession = false;

			bool ShroudEnabled = false;
			bool LockViewport = true;
			bool SelectUnits = true;
			bool SpectatorView = false;
			// Playback only; recording keeps this data unconditionally.
			bool ShowChatAndBeacons = true;

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

			uint32_t ExpectedGameCRC = 0;
			bool HasExpectedGameCRC = false;
			// The game speed as last written into the stream, so only changes are recorded. -1
			// forces the first frame to establish the baseline.
			int LastRecordedGameSpeed = -1;
			bool DivergenceReported = false;

			// CRC comparison summary.
			int CheckedFrameCount = 0;
			int MismatchedFrameCount = 0;
			int LoggedMismatchCount = 0;
			int FirstMismatchFrame = -1;
			int LastMismatchFrame = -1;

			HANDLE ReplayFile = INVALID_HANDLE_VALUE;
			// Only one of these is ever active: recording deflates, playback inflates.
			Replay::DeflateWriter Writer;
			Replay::InflateReader Reader;

			char PlaybackPath[MAX_PATH] = { 0 };
			std::deque<PendingRecordedFrameCapture> PendingFrameStates;
			std::deque<SideChannelRecord> PendingSideChannelEvents;
			int CapturedFrameEventsFrame = -1;
			// Events executed during the current frame.
			std::vector<EventClass> CapturedFrameEvents;
			ReplayHeader PlaybackHeader = {};
			bool HasPlaybackHeader = false;

			bool HasPendingPlaybackFrame = false;
			bool PlaybackStreamEnded = false;
			int PreparedPlaybackFrame = -1;
			int LastReadPlaybackFrame = -1;

			uint64_t PlaybackStreamOffset = 0;
			// The furthest frame playback has reached, which stands in for the replay's length
			// when the recording died before it could stamp one into the header.
			int HighestPlayedFrame = 0;

			// Last recorded viewport position, re-applied between sparse frame records.
			Point2D LockedViewportPos = { 0, 0 };
			bool HasLockedViewportPos = false;
			std::vector<uint32_t> LockedSelectionIDs;
			bool HasLockedSelection = false;
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
		bool RepositionPlaybackStreamToFrame(int targetFrame);
		void CaptureGameCRCForCurrentFrame();
		// Applies the latest sticky viewer state immediately after its live toggle is enabled.
		void ApplyLockedViewport();
		void ApplyCurrentPlaybackSelection();
		void RecordSelectionTriggerSpring(TechnoClass* pTechno);
		void SpringRecordedSelectionTriggers(const PlaybackFrameRecord& frameRecord);
		void ComputeAndCaptureGameCRCForCurrentFrame();
		int GetPlaybackTargetFPS();
		double PlaybackClockMilliseconds();
		void ApplyPlaybackFramePacing();
		void CaptureEventsForCurrentFrame();
		void RecordExecutedEvent(const EventClass* pEvent);
		void RecordCapturedEventsForCurrentFrame();
		void RemoveReplayGameplayEventsFromDoList();
		void PlaybackFrameEvents();

		bool PlaybackWantsFullMapReveal();
	}
}
