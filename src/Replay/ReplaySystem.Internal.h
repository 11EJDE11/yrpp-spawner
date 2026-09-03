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
			FrameObjectCensus Census { 0, 0 };
			FrameRandomState RandomState { 0, 0 };
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
			FrameObjectCensus Census { 0, 0 };
			bool HasCensus = false;
			FrameRandomState RandomState { 0, 0 };
			bool HasRandomState = false;
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

			bool ShroudEnabled = false;
			bool LockViewport = true;
			bool SelectUnits = true;
			// Initialized from ReplayDiagnostics and live-toggleable during playback.
			bool DiagnosticsEnabled = false;
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
			FrameRandomState ExpectedRandomState { 0, 0 };
			bool HasExpectedRandomState = false;
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
			// RestoreFrameState may run more than once while the same simulation frame is held
			// paused. Remember which frame has already consumed its replay record so resuming does
			// not either skip that record or interpret its event bytes as another frame header.
			int PreparedPlaybackFrame = -1;

			// Where the deflated frame stream starts in the file, so a seek can restart the
			// decompressor from the top. Deflate has no random access, and the alternative - an
			// index of decodable points - would have to go in the file. See ReplaySeek.h.
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
		// Rewinds the frame stream and walks it forward to the first record at or after the frame,
		// re-applying the sticky state - viewport, game speed - carried by the records stepped
		// over. Used by seeking; see ReplaySeek.h.
		bool RepositionPlaybackStreamToFrame(int targetFrame);
		void CaptureGameCRCForCurrentFrame();
		// Divergence hunting. Remembers which code asked for each scenario-randomiser draw, by frame,
		// and checks a frame replayed after a seek against what it drew the first time round. The
		// first difference names the code whose state the keyframe load did not carry across. Always
		// on during playback; see docs/replay-format.md.
		void TraceRandomDraw(const void* randomiser, const void* caller);
		void ResetRandomDrawTrace();
		// The same trick for mission changes, which move an object's behaviour without touching
		// the randomiser and so are invisible to the draw trace.
		// The originator is the caller above a thin virtual override, where there is one, and the
		// caller itself otherwise. See MissionClass_AssignMission_TraceForDivergence.
		void TraceMissionAssignment(const void* object, int mission, const void* caller,
			const void* originator);
		void ResetMissionTrace();
		// Every route request, so two runs can be checked for asking the pathfinder the same
		// questions in the same order before anyone blames the pathfinder for the answers.
		void TracePathRequest(const void* object, int cell, int pathOffset, int avoidance);
		void ResetPathRequestTrace();
		// AircraftClass::New_LZ, recorded on entry rather than at its draw, so a run that never
		// reached the draw can be told from one that reached it and turned back.
		void TraceLandingZoneCell(int site, const TechnoClass* pAircraft, int cellX, int cellY,
			const AbstractClass* pOther = nullptr, const void* caller = nullptr,
			const void* engineCaller = nullptr, int inRadar = -1, int isInPlayfield = -1,
			int isALoaner = -1, int mission = -1, const AbstractClass* pTarget = nullptr,
			int mapWidth = -1, int mapHeight = -1);
		void ResetLandingZoneTrace();
		// The order LogicClass::AI hands objects to their own AI, which is the order everything
		// downstream of it draws from the randomiser in.
		void TraceObjectUpdate(const void* object);
		void ResetUpdateOrderTrace();
		// Run once a frame so a frame that stops asking is caught; see ServiceTraces.
		void ServiceTraces();
		// Each trace reports its first difference and then stays quiet. Called by every keyframe load
		// so that is once per seek rather than once per replay; see RestartDriftReporting.
		void RestartTraceReporting();

		// Every trace and watch keeps what it records, and the game is a 32-bit process. Each store
		// had its own cap and nobody had added them up: the cell watch alone kept a whole 25MB table
		// per keyframe, uncounted, and a long replay ran the process out of address space and died
		// on a std::bad_alloc that no catch would have helped.
		//
		// The budget that fixed that bought a prefix of the replay - record until it is gone, then
		// stop - and a seek is never in the prefix. It now buys a window on the recent past instead:
		// a frame that does not fit is paid for by forgetting the oldest frame held. See the budget
		// in ReplaySystem.cpp, and the watches' own share of it in ReplaySeek.cpp.
		bool ChargeDiagnosticMemory(size_t bytes);
		void ResetDiagnosticMemory();
		// Applies the latest sticky viewer state immediately after its live toggle is enabled.
		void ApplyLockedViewport();
		void ApplyCurrentPlaybackSelection();
		// Selecting a unit is local input, except that TechnoClass::Select raises
		// TriggerEvent::SelectedByPlayer on the object's tag, and a map can hang the simulation on
		// that - RA2's own Boot Camp does. So the spring is recorded and replayed in its own right
		// rather than left to fall out of whatever the viewer's selection happens to be. See the
		// hook on TechnoClass::Select in ReplaySystem.Hook.cpp.
		void RecordSelectionTriggerSpring(TechnoClass* pTechno);
		void SpringRecordedSelectionTriggers(const PlaybackFrameRecord& frameRecord);
		// Names the object behind the next draw, for call sites where knowing it matters.
		void SetRandomDrawContext(const TechnoClass* pTechno);
		FrameObjectCensus CurrentObjectCensus();
		void CheckObjectCensusForCurrentFrame();
		// What an object is, in the words the rules use for it. The result points at a buffer reused
		// by the next call, so print it before calling again.
		const char* DescribeAbstract(const AbstractClass* pAbstract);
		// A code address as module and offset, so a caller inside a relocatable DLL is resolvable.
		const char* DescribeCodeAddress(uint32_t address, char* buffer, size_t size);
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
