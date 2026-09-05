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

#include "ReplayFrameCodec.h"
#include "ReplayFile.h"

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

			int ExpectedEventsThisFrame = 0;
			int LastSyncFlushFrame = 0;

			uint32_t ExpectedGameCRC = 0;
			bool HasExpectedGameCRC = false;
			bool DivergenceReported = false;

			// CRC comparison summary.
			int CheckedFrameCount = 0;
			int MismatchedFrameCount = 0;
			int LoggedMismatchCount = 0;
			int FirstMismatchFrame = -1;
			int LastMismatchFrame = -1;

			Replay::File File;
			Replay::FrameWriter FrameWriter;
			Replay::FrameReader FrameReader;

			char PlaybackPath[MAX_PATH] = { 0 };
			std::deque<RecordedFrameCapture> PendingFrameStates;
			int CapturedFrameEventsFrame = -1;
			// Events executed during the current frame.
			std::vector<EventClass> CapturedFrameEvents;
			ReplayHeader PlaybackHeader = {};
			bool HasPlaybackHeader = false;

			bool HasPendingPlaybackFrame = false;
			bool PlaybackStreamEnded = false;
			int PreparedPlaybackFrame = -1;

			// The furthest frame playback has reached, which stands in for the replay's length
			// when the recording died before it could stamp one into the header.
			int HighestPlayedFrame = 0;

			// Last recorded viewport position, re-applied between sparse frame records.
			Point2D LockedViewportPos = { 0, 0 };
			bool HasLockedViewportPos = false;
			std::vector<uint32_t> LockedSelectionIDs;
			bool HasLockedSelection = false;
			PlaybackFrameRecord PendingPlaybackFrame = {};

			// Scratch buffers reused every frame so recording does not allocate on the game thread.
			std::vector<EventClass> PreservedEventsScratch;
		};

		extern ReplayRuntimeState ReplayState;

		const SpawnerConfig* GetConfig();

		// Lifecycle.
		void StartReplayRecording();
		void StartReplayPlayback(const char* replayPath);
		void StopReplaySystem();
		void FinishRecordingAtMissionEnd();
		void ApplyPlaybackInitialState();

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
		void CaptureEventsForCurrentFrame();
		void RecordExecutedEvent(const EventClass* pEvent);
		void RecordCapturedEventsForCurrentFrame();
		void RemoveReplayGameplayEventsFromDoList();
		void PlaybackFrameEvents();

		bool PlaybackWantsFullMapReveal();
	}
}
