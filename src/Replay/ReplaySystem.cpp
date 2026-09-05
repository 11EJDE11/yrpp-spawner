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

#include "ReplayControls.h"
#include "ReplayFile.h"
#include "ReplayFrameCodec.h"
#include "ReplayOverlay.h"
#include "ReplaySeek.h"
#include "ReplaySystem.h"
#include "ReplaySystem.Internal.h"

#include <Spawner/Spawner.h>
#include <Utilities/Debug.h>
#include <Ext/Event/Body.h>

#include <AbstractClass.h>
#include <BeaconManagerClass.h>
#include <ColorScheme.h>
#include <EventClass.h>
#include <GameModeOptionsClass.h>
#include <GameOptionsClass.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <MessageListClass.h>
#include <ObjectClass.h>
#include <RulesClass.h>
#include <SessionClass.h>
#include <MouseClass.h>
#include <TagClass.h>
#include <TechnoClass.h>
#include <TacticalClass.h>
#include <Timer.h>
#include <Unsorted.h>
#include <VocClass.h>
#include <VoxClass.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ReplaySystem
{
	namespace Internal
	{
		ReplayRuntimeState ReplayState;

		void StopReplaySystem();
		void AbortReplaySystem();
		void ApplyPlaybackInitialState();

		const SpawnerConfig* GetConfig()
		{
			return Spawner::GetConfig();
		}

		// The playback frame rate, in frames per second: whatever the viewer's speed controls last
		// asked for, falling back to the rate the recorded game speed implies.
		int GetPlaybackTargetFPS()
		{
			if (ReplayState.PlaybackFPS > 0)
				return ReplayState.PlaybackFPS;

			return GetReplayFPSFromGameSpeed(GameOptionsClass::Instance.GameSpeed);
		}

		// Milliseconds off the performance counter. The engine's own clocks are 60Hz ticks or whole
		// milliseconds, and neither can express the faster end of the playback speed ladder.
		double PlaybackClockMilliseconds()
		{
			static double ticksPerMillisecond = 0.0;
			if (ticksPerMillisecond == 0.0)
			{
				LARGE_INTEGER frequency {};
				if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart == 0)
					return 0.0;

				ticksPerMillisecond = static_cast<double>(frequency.QuadPart) / 1000.0;
			}

			LARGE_INTEGER counter {};
			if (!QueryPerformanceCounter(&counter))
				return 0.0;

			return static_cast<double>(counter.QuadPart) / ticksPerMillisecond;
		}

		void ApplyPlaybackFramePacing()
		{
			if (!ReplayState.Playback)
				return;

			// A seek is not being watched, so it runs as fast as the machine manages. The engine
			// still has to be told not to wait, which is what the two timers below do.
			if (ReplaySystem::Seek::IsSeeking())
			{
				Unsorted::GameFrameTimer.TimeLeft = 0;
				Unsorted::NetworkFrameTimer.TimeLeft = 0;
				return;
			}

			if (ReplayState.PlaybackFPS <= 0)
				return;

			const double frameMilliseconds = 1000.0 / ReplayState.PlaybackFPS;
			const double now = PlaybackClockMilliseconds();

			if (now > 0.0)
			{
				if (ReplayState.PlaybackNextFrameDue == 0.0
					|| now > ReplayState.PlaybackNextFrameDue + frameMilliseconds)
				{
					ReplayState.PlaybackNextFrameDue = now;
				}

				for (double remaining = ReplayState.PlaybackNextFrameDue - PlaybackClockMilliseconds();
					remaining > 0.0;
					remaining = ReplayState.PlaybackNextFrameDue - PlaybackClockMilliseconds())
				{
					// Sleep(1) can overshoot by a whole scheduler tick, so it covers the bulk and the
					// last couple of milliseconds are given up with Sleep(0).
					Sleep(remaining > 2.0 ? 1 : 0);
				}

				ReplayState.PlaybackNextFrameDue += frameMilliseconds;
			}

			Unsorted::GameFrameTimer.TimeLeft = 0;
			Unsorted::NetworkFrameTimer.TimeLeft = 0;
		}

		void SyncFlushRecordingStream()
		{
			if (!ReplayState.File.SyncFlush())
			{
				Debug::Log("[Replay] Failed to flush the replay stream; stopping the recording.\n");
				AbortReplaySystem();
			}
		}

		void ResetRuntimeFlagsForScenario()
		{
			ReplayState.Recording = false;
			ReplayState.Playback = false;
			ReplayState.SpectatorView = false;
			ReplayState.PlaybackFPS = 0;
			ReplayState.PlaybackNextFrameDue = 0.0;
			ReplayState.PlaybackPaused = false;
			ReplayState.ExpectedEventsThisFrame = 0;
			ReplayState.LastSyncFlushFrame = 0;
			ReplayState.ExpectedGameCRC = 0;
			ReplayState.HasExpectedGameCRC = false;
			ReplayState.DivergenceReported = false;
			ReplayState.CheckedFrameCount = 0;
			ReplayState.MismatchedFrameCount = 0;
			ReplayState.LoggedMismatchCount = 0;
			ReplayState.FirstMismatchFrame = -1;
			ReplayState.LastMismatchFrame = -1;
			ReplayState.PendingFrameStates.clear();
			ReplayState.PendingSideChannelEvents.clear();
			ReplayState.CapturedFrameEventsFrame = -1;
			ReplayState.CapturedFrameEvents.clear();
			ReplayState.HasPlaybackHeader = false;
			memset(&ReplayState.PlaybackHeader, 0, sizeof(ReplayState.PlaybackHeader));
			ReplayState.HasPendingPlaybackFrame = false;
			ReplayState.PlaybackStreamEnded = false;
			ReplayState.PreparedPlaybackFrame = -1;
			ReplayState.FrameReader.Reset();
			ReplayState.HighestPlayedFrame = 0;
			ReplayState.PendingPlaybackFrame = {};
			ReplayState.LockedViewportPos = { 0, 0 };
			ReplayState.HasLockedViewportPos = false;
			ReplayState.LockedSelectionIDs.clear();
			ReplayState.HasLockedSelection = false;
			ReplayState.FrameWriter.Reset();
		}

		void AbortReplaySystem()
		{
			ReplaySystem::Seek::OnPlaybackStopped();
			ResetRuntimeFlagsForScenario();
			ReplayState.File.Close();
		}

		void ApplyPlaybackInitialState()
		{
			if (!ReplayState.HasPlaybackHeader)
				return;

			Game::Seed = ReplayState.PlaybackHeader.Seed;
			if (ScenarioClass::Instance)
			{
				ScenarioClass::Instance->Random.Next1 = ReplayState.PlaybackHeader.RandomNext1;
				ScenarioClass::Instance->Random.Next2 = ReplayState.PlaybackHeader.RandomNext2;
				memcpy(ScenarioClass::Instance->Random.Table, ReplayState.PlaybackHeader.RandomizerTable, sizeof(ReplayState.PlaybackHeader.RandomizerTable));
				ScenarioClass::Instance->UniqueID = ReplayState.PlaybackHeader.UniqueIDCounter;
			}
		}

		int ResolveViewPlayerIndex()
		{
			const auto* pConfig = GetConfig();
			if (!pConfig || !ReplaySystem::IsPlaybackRequested())
				return 0;

			// A campaign recording has one player slot, so there is nothing to switch to.
			if (pConfig->IsCampaign)
				return 0;

			const int requested = pConfig->ReplayViewPlayer;
			if (requested <= 0 || requested >= static_cast<int>(std::size(pConfig->Players)))
				return 0;

			// AI slots have no [OtherN] section and so never get a NodeNameType, which is what the house
			// lookup in ApplyViewPlayerToCurrentPlayer walks. Only human slots can be watched from.
			return pConfig->Players[requested].IsHuman ? requested : 0;
		}

		// StartScenario creates one NodeNameType per human spawn.ini slot, in slot order, so a slot's node
		// is preceded by exactly the human slots below it.
		int NodeIndexForPlayerSlot(int playerSlot)
		{
			const auto* pConfig = GetConfig();
			if (!pConfig)
				return -1;

			int nodeIndex = 0;
			for (int slot = 0; slot < playerSlot; ++slot)
			{
				if (pConfig->Players[slot].IsHuman)
					++nodeIndex;
			}

			return nodeIndex;
		}

		void ApplyViewPlayerToCurrentPlayer()
		{
			const auto* const pConfig = GetConfig();
			const int requested = pConfig ? pConfig->ReplayViewPlayer : -1;
			const int playerSlot = ResolveViewPlayerIndex();

			if (playerSlot <= 0)
			{
				// -1 and 0 both mean the recording player and are silent. Anything else named a slot
				// that cannot be watched from, so report it.
				if (requested > 0 && ReplaySystem::IsPlaybackRequested())
				{
					Debug::Log("[Replay] ReplayViewPlayer=%d is not a human player slot; watching from the "
						"recording player.\n", requested);
				}

				return;
			}

			auto& nodes = NodeNameType::Array;
			const int nodeIndex = NodeIndexForPlayerSlot(playerSlot);

			if (nodeIndex < 0 || nodeIndex >= nodes.Count)
			{
				Debug::Log("[Replay] ReplayViewPlayer=%d has no player node; watching from the recording player.\n",
					playerSlot);
				return;
			}

			const auto* const pNode = nodes.GetItem(nodeIndex);
			if (!pNode || pNode->HouseIndex < 0 || pNode->HouseIndex >= HouseClass::Array.Count)
			{
				Debug::Log("[Replay] ReplayViewPlayer=%d did not resolve to a house; watching from the recording player.\n",
					playerSlot);
				return;
			}

			auto* const pHouse = HouseClass::Array.GetItem(pNode->HouseIndex);
			if (!pHouse || !pHouse->IsHumanPlayer)
			{
				Debug::Log("[Replay] ReplayViewPlayer=%d resolved to a house that is not a human player; "
					"watching from the recording player.\n", playerSlot);
				return;
			}

			// Moved rather than just set: Assign_Houses gives both to the recording player's house,
			// and leaving IsInPlayerControl behind would describe a state no peer ever had.
			if (HouseClass::CurrentPlayer)
				HouseClass::CurrentPlayer->IsInPlayerControl = false;

			HouseClass::CurrentPlayer = pHouse;
			pHouse->IsInPlayerControl = true;

			Debug::Log("[Replay] Watching from spawn.ini player slot %d (%ls), house index %d.\n",
				playerSlot, pHouse->UIName, pNode->HouseIndex);
		}

		// Network/timing events are recorded for diagnostics but not replayed.
		bool IsTimingEvent(EventType eventType)
		{
			// Keep extension timing events out of playback without guessing numeric ranges.
			if (EventExt::IsValidType(static_cast<EventTypeExt>(eventType)))
				return true;

			switch (eventType)
			{
			case EventType::Empty:
			case EventType::ResponseTime:
			case EventType::FrameInfo:
			case EventType::Timing:
			case EventType::ProcessTime:
			case EventType::PacketTiming:
			case EventType::MegaFrameInfo:
			case EventType::FrameSync:
				return true;
			default:
				return false;
			}
		}

		bool IsReplayableGameplayEvent(const EventClass& event)
		{
			return event.Type != EventType::Options && !IsTimingEvent(event.Type);
		}
		bool IsLocalPlaybackControlEvent(const EventClass& event)
		{
			switch (event.Type)
			{
			case EventType::Options:
			case EventType::Exit:
			case EventType::SaveGame:
				return true;
			default:
				return false;
			}
		}

		template <typename Predicate>
		void RemoveDoListEvents(Predicate shouldRemove)
		{
			auto& doList = EventClass::DoList;
			const int originalCount = doList.Count;
			if (originalCount <= 0)
				return;

			// DoList holds up to MAX_EVENTS * 128 events of 111 bytes and playback runs this every
			// frame, usually with nothing to remove, so scan first and only copy when needed.
			bool removedAny = false;
			for (int i = 0; i < originalCount; ++i)
			{
				if (shouldRemove(doList[i]))
				{
					removedAny = true;
					break;
				}
			}

			if (!removedAny)
				return;

			// Reused across frames so the copy does not reallocate on every rebuild.
			std::vector<EventClass>& preservedEvents = ReplayState.PreservedEventsScratch;
			preservedEvents.clear();
			preservedEvents.reserve(static_cast<size_t>(originalCount));

			for (int i = 0; i < originalCount; ++i)
			{
				const auto& event = doList[i];
				if (!shouldRemove(event))
					preservedEvents.push_back(event);
			}

			doList.Init();
			for (const auto& event : preservedEvents)
			{
				doList.Add(event);
			}
		}

		// Remove events the game inserted during playback.
		void RemoveReplayGameplayEventsFromDoList()
		{
			if (!ReplayState.Playback)
				return;

			const auto currentFrame = static_cast<unsigned int>(Unsorted::CurrentFrame);

			// Treat local GameSpeed events as replay playback-speed changes.
			for (int i = 0; i < EventClass::DoList.Count; ++i)
			{
				const auto& event = EventClass::DoList[i];
				if (event.Frame == currentFrame && event.Type == EventType::GameSpeed)
				{
					const int requestedFPS = GetReplayFPSFromGameSpeed(
						std::clamp(event.GameSpeed.GameSpeed, 0, MaxGameSpeedIndex));

					ReplayState.PlaybackFPS = requestedFPS;
					ReplayState.PlaybackNextFrameDue = 0.0;
				}
			}

			RemoveDoListEvents([](const EventClass& event)
			{
				return event.Frame == static_cast<unsigned int>(Unsorted::CurrentFrame)
					&& IsReplayableGameplayEvent(event)
					&& !IsLocalPlaybackControlEvent(event);
			});
		}

		// Fills rather than returns, so the caller's buffer is reused: this runs on every main loop
		// iteration of a recording game.
		void FillSelectedObjectIDs(std::vector<uint32_t>& ids)
		{
			auto& currentObjects = ObjectClass::CurrentObjects;
			ids.clear();
			ids.reserve(currentObjects.Count);

			for (int i = 0; i < currentObjects.Count; ++i)
			{
				ObjectClass* pObj = currentObjects.Items[i];
				if (pObj)
					ids.push_back(static_cast<uint32_t>(pObj->UniqueID));
			}
		}

		bool IsCurrentSelection(const std::vector<uint32_t>& ids)
		{
			auto& currentObjects = ObjectClass::CurrentObjects;
			size_t index = 0;

			for (int i = 0; i < currentObjects.Count; ++i)
			{
				ObjectClass* pObj = currentObjects.Items[i];
				if (!pObj)
					continue;

				if (index >= ids.size() || ids[index] != static_cast<uint32_t>(pObj->UniqueID))
					return false;

				++index;
			}

			return index == ids.size();
		}

		void FlushPendingRecordedFramesThrough(int frameNumber, int currentFrameEventCount)
		{
			while (!ReplayState.PendingFrameStates.empty())
			{
				const auto& capture = ReplayState.PendingFrameStates.front();
				const int pendingFrame = capture.FrameNumber;
				if (pendingFrame > frameNumber)
					break;

				// Keep playback parsing bounded and discard overflow from the same frame.
				std::vector<SideChannelRecord>& sideChannelForFrame = ReplayState.SideChannelScratch;
				sideChannelForFrame.clear();
				bool droppedSideChannelEvent = false;
				while (!ReplayState.PendingSideChannelEvents.empty()
					&& ReplayState.PendingSideChannelEvents.front().FrameNumber <= pendingFrame)
				{
					if (sideChannelForFrame.size() < static_cast<size_t>(SideChannelMaxEventsPerFrame))
						sideChannelForFrame.push_back(ReplayState.PendingSideChannelEvents.front());
					else
						droppedSideChannelEvent = true;

					ReplayState.PendingSideChannelEvents.pop_front();
				}

				if (droppedSideChannelEvent)
					Debug::Log("[Replay] Dropped excess side-channel events on frame %d.\n", pendingFrame);

				const int eventCount = pendingFrame == frameNumber ? currentFrameEventCount : 0;
				if (!ReplayState.FrameWriter.WriteFrame(ReplayState.File, capture, eventCount, sideChannelForFrame))
				{
					Debug::Log("[Replay] Failed to write frame capture.\n");
					AbortReplaySystem();
					return;
				}

				ReplayState.PendingFrameStates.pop_front();
			}
		}

		void RecordFrameState()
		{
			if (!ReplayState.Recording)
				return;

			const int frameNumber = Unsorted::CurrentFrame;

			// The main loop runs faster than the game frame, so the same frame is usually captured
			// several times over. Overwriting the pending entry in place reuses its buffer.
			if (ReplayState.PendingFrameStates.empty()
				|| ReplayState.PendingFrameStates.back().FrameNumber != frameNumber)
			{
				ReplayState.PendingFrameStates.emplace_back();
				ReplayState.PendingFrameStates.back().FrameNumber = frameNumber;
			}

			RecordedFrameCapture& capture = ReplayState.PendingFrameStates.back();

			capture.TacticalPos = TacticalClass::Instance
				? TacticalClass::Instance->TacticalCoord1
				: Point2D { 0, 0 };

			FillSelectedObjectIDs(capture.SelectedObjectIDs);

			capture.GameSpeed = static_cast<int32_t>(GameOptionsClass::Instance.GameSpeed);
			capture.HasGameSpeed = capture.GameSpeed != ReplayState.FrameWriter.LastGameSpeed();
		}

		bool SanitizeSideChannelRecord(SideChannelRecord& record)
		{
			record.SenderName[SideChannelNameLength - 1] = L'\0';
			record.Text[SideChannelTextLength - 1] = L'\0';

			switch (static_cast<SideChannelEventType>(record.Type))
			{
			case SideChannelEventType::ChatMessage:
				if (record.House < 0 || record.House >= MaxHouses)
					return false;

				// Indexes ColorScheme::Array in TextLabelClass; fall back to the first scheme.
				if (record.Aux < 0 || record.Aux >= ColorScheme::Array.Count)
					record.Aux = 0;

				return true;

			case SideChannelEventType::BeaconPlace:
				// -1 asks the engine to pick a free slot, which is how a local placement is recorded.
				if (record.House < 0 || record.House >= MaxHouses
					|| record.Aux < -1 || record.Aux >= MaxBeaconSlots)
				{
					return false;
				}

				return record.Coord.X >= 0 && record.Coord.X < MaxMapLeptonCoord
					&& record.Coord.Y >= 0 && record.Coord.Y < MaxMapLeptonCoord;

			case SideChannelEventType::BeaconDelete:
			case SideChannelEventType::BeaconText:
				return record.House >= 0 && record.House < MaxHouses
					&& record.Aux >= 0 && record.Aux < MaxBeaconSlots;

			case SideChannelEventType::Taunt:
				// PlayTaunt range-checks the command itself, but a value that came off disk should
				// not be bounded only by a function we do not own.
				return VoxClass::IsValidTauntCommand(record.Aux);

			default:
				return false;
			}
		}

		bool RepositionPlaybackStreamToFrame(int targetFrame)
		{
			if (!ReplayState.File.IsOpen())
				return false;

			if (!ReplayState.File.RestartPlaybackStream())
			{
				Debug::Log("[Replay] Failed to rewind the replay stream while seeking.\n");
				return false;
			}

			ReplayState.HasPendingPlaybackFrame = false;
			ReplayState.PlaybackStreamEnded = false;
			ReplayState.PreparedPlaybackFrame = -1;
			ReplayState.FrameReader.Reset();
			ReplayState.PendingPlaybackFrame = {};
			ReplayState.ExpectedEventsThisFrame = 0;
			ReplayState.HasExpectedGameCRC = false;

			for (;;)
			{
				PlaybackFrameRecord record {};
				if (!ReplayState.FrameReader.ReadFrame(ReplayState.File, record, SanitizeSideChannelRecord))
				{
					Debug::Log("[Replay] Failed to read the replay stream while seeking to frame %d.\n",
						targetFrame);
					return false;
				}

				if (record.EndOfStream)
				{
					ReplayState.PlaybackStreamEnded = true;
					return true;
				}

				if (record.FrameNumber >= targetFrame)
				{
					ReplayState.PendingPlaybackFrame = std::move(record);
					ReplayState.HasPendingPlaybackFrame = true;
					return true;
				}

				if ((record.Flags & FrameRecordFlag_TacticalPos) != 0u)
				{
					ReplayState.LockedViewportPos = record.TacticalPos;
					ReplayState.HasLockedViewportPos = true;
				}

				if ((record.Flags & FrameRecordFlag_Selection) != 0u)
				{
					ReplayState.LockedSelectionIDs = record.SelectedObjectIDs;
					ReplayState.HasLockedSelection = true;
				}

				if ((record.Flags & FrameRecordFlag_GameSpeed) != 0u)
				{
					const int gameSpeed = std::clamp(static_cast<int>(record.GameSpeed), 0, MaxGameSpeedIndex);
					GameOptionsClass::Instance.GameSpeed = gameSpeed;
					GameModeOptionsClass::Instance.GameSpeed = gameSpeed;
				}

				// The frame's events sit in the stream immediately after its record.
				if (!ReplayState.FrameReader.SkipEvents(ReplayState.File, record.EventCountThisFrame))
				{
					Debug::Log("[Replay] Failed to step over frame %d's events while seeking.\n",
						record.FrameNumber);
					return false;
				}
			}
		}

		void ApplyPlaybackSelection(const PlaybackFrameRecord& frameRecord)
		{
			if (!ReplayState.SelectUnits)
				return;

			const int maxSelectionCount = std::max(AbstractClass::Array.Count, 0);
			if (frameRecord.SelectedObjectCount < 0 || frameRecord.SelectedObjectCount > maxSelectionCount)
			{
				Debug::Log("[Replay] Skipping a recorded selection of %d objects; %d objects exist.\n",
					frameRecord.SelectedObjectCount, maxSelectionCount);
				return;
			}

			if (IsCurrentSelection(frameRecord.SelectedObjectIDs))
				return;

			const std::unordered_set<uint32_t> recordedIDs(
				frameRecord.SelectedObjectIDs.begin(), frameRecord.SelectedObjectIDs.end());

			// The object array can hold thousands of entries, so only the recorded ones are kept.
			std::unordered_map<uint32_t, ObjectClass*> objectByUniqueID;
			objectByUniqueID.reserve(recordedIDs.size());

			for (int i = 0; i < AbstractClass::Array.Count; ++i)
			{
				AbstractClass* pAbs = AbstractClass::Array.GetItem(i);
				if (!pAbs)
					continue;

				const auto uniqueID = static_cast<uint32_t>(pAbs->UniqueID);
				if (recordedIDs.find(uniqueID) == recordedIDs.end())
					continue;

				if (auto* pObject = abstract_cast<ObjectClass*>(pAbs))
					objectByUniqueID.emplace(uniqueID, pObject);
			}

			MapClass::UnselectAll();

			// Recorded order, so the selection ends up ordered the way it was.
			for (const auto uniqueID : frameRecord.SelectedObjectIDs)
			{
				const auto it = objectByUniqueID.find(uniqueID);
				if (it != objectByUniqueID.end())
					it->second->Select();
			}
		}

		void SpringRecordedSelectionTriggers(const PlaybackFrameRecord& frameRecord)
		{
			for (const auto uniqueID : frameRecord.SelectionTriggerObjectIDs)
			{
				// A scan of the object array per spring, which is only ever a handful on the rare
				// frame that carries any at all.
				for (int i = 0; i < AbstractClass::Array.Count; ++i)
				{
					AbstractClass* pAbs = AbstractClass::Array.GetItem(i);
					if (!pAbs || static_cast<uint32_t>(pAbs->UniqueID) != uniqueID)
						continue;

					auto* const pTechno = abstract_cast<TechnoClass*>(pAbs);
					if (pTechno && pTechno->AttachedTag)
					{
						// The arguments TechnoClass::Select uses, CELL_NONE included: an empty cell
						// is what tells TagClass::Spring not to detach a cell tag as well.
						pTechno->AttachedTag->RaiseEvent(TriggerEvent::SelectedByPlayer, pTechno,
							CellStruct { 0, 0 }, false, nullptr);
					}
					else
					{
						Debug::Log("[Replay] Frame %d: the recording raised SelectedByPlayer on "
							"unique ID %u, which playback has no tagged techno for.\n",
							static_cast<int>(Unsorted::CurrentFrame), uniqueID);
					}

					break;
				}
			}
		}

		void RecordSelectionTriggerSpring(TechnoClass* pTechno)
		{
			if (!ReplayState.Recording || !pTechno)
				return;

			const int frameNumber = Unsorted::CurrentFrame;

			// The main loop's capture for this frame has already been made by the time input runs,
			// so this appends to it. It creates the entry if input somehow got in first.
			if (ReplayState.PendingFrameStates.empty()
				|| ReplayState.PendingFrameStates.back().FrameNumber != frameNumber)
			{
				ReplayState.PendingFrameStates.emplace_back();
				ReplayState.PendingFrameStates.back().FrameNumber = frameNumber;
			}

			auto& ids = ReplayState.PendingFrameStates.back().SelectionTriggerObjectIDs;
			if (ids.size() >= static_cast<size_t>(MaxSelectionTriggersPerFrame))
				return;
			ids.push_back(static_cast<uint32_t>(pTechno->UniqueID));
		}

		void ApplyCurrentPlaybackSelection()
		{
			if (!ReplayState.HasLockedSelection)
				return;

			PlaybackFrameRecord record {};
			record.SelectedObjectIDs = ReplayState.LockedSelectionIDs;
			record.SelectedObjectCount = static_cast<int32_t>(record.SelectedObjectIDs.size());
			ApplyPlaybackSelection(record);
		}

		int GetChatMessageDurationFrames()
		{
			constexpr int TicksPerMinute = 900;
			constexpr double DefaultMessageDelay = 0.6; // Rules' own default, if Rules is not loaded.

			const double messageDelay = RulesClass::Instance
				? RulesClass::Instance->MessageDelay
				: DefaultMessageDelay;

			return static_cast<int>(messageDelay * TicksPerMinute);
		}

		// Replays recorded chat, beacons and taunts without using the network.
		void ApplySideChannelEvent(const SideChannelRecord& record)
		{
			if (!ReplayState.ShowChatAndBeacons)
				return;

			const auto eventType = static_cast<SideChannelEventType>(record.Type);
			if (ReplaySystem::Seek::IsSeeking()
				&& (eventType == SideChannelEventType::ChatMessage
					|| eventType == SideChannelEventType::Taunt))
			{
				return;
			}

			switch (eventType)
			{
			case SideChannelEventType::ChatMessage:
				MessageListClass::Instance.AddMessage(
					record.SenderName, record.House, record.Text, record.Aux,
					TextPrintType::UseGradPal | TextPrintType::FullShadow | TextPrintType::Point6Grad,
					GetChatMessageDurationFrames(), false);
				if (RulesClass::Instance)
					VocClass::PlayGlobal(RulesClass::Instance->IncomingMessage, 0x2000, 1.0f);
				break;

			case SideChannelEventType::BeaconPlace:
				BeaconManagerClass::Instance.PlaceBeacon(record.House, record.Coord, record.Aux);
				break;

			case SideChannelEventType::BeaconDelete:
				BeaconManagerClass::Instance.DeleteBeacon(record.House, record.Aux);
				break;

			case SideChannelEventType::BeaconText:
				BeaconManagerClass::Instance.EditBeaconMessage(record.Text, record.House, record.Aux, true);
				break;

			case SideChannelEventType::Taunt:
				VoxClass::PlayTaunt(record.Aux);
				break;

			default:
				break;
			}
		}

		constexpr int DivergenceMessageDurationFrames = 1800;
		constexpr const wchar_t* DivergenceMessage = L"Replay playback has diverged from the recording.";
		constexpr int MaxLoggedDivergences = 10;
		void CaptureGameCRCForCurrentFrame()
		{
			if (!ReplayState.Recording && !ReplayState.Playback)
				return;

			const uint32_t gameCRC = static_cast<uint32_t>(EventClass::CurrentFrameCRC);
			const int frameNumber = Unsorted::CurrentFrame;

			if (ReplayState.Recording)
			{
				if (!ReplayState.PendingFrameStates.empty()
					&& ReplayState.PendingFrameStates.back().FrameNumber == frameNumber)
				{
					auto& capture = ReplayState.PendingFrameStates.back();
					capture.GameCRC = gameCRC;
					capture.HasGameCRC = true;
				}
				return;
			}

			if (!ReplayState.HasExpectedGameCRC)
				return;

			ReplayState.HasExpectedGameCRC = false;
			++ReplayState.CheckedFrameCount;

			if (ReplayState.ExpectedGameCRC == gameCRC)
				return;

			++ReplayState.MismatchedFrameCount;
			if (ReplayState.FirstMismatchFrame < 0)
				ReplayState.FirstMismatchFrame = frameNumber;
			ReplayState.LastMismatchFrame = frameNumber;

			if (ReplayState.LoggedMismatchCount < MaxLoggedDivergences)
			{
				++ReplayState.LoggedMismatchCount;
				Debug::Log("[Replay] Playback diverged on frame %d (recorded CRC %08X, playback CRC %08X).\n",
					frameNumber, ReplayState.ExpectedGameCRC, gameCRC);
			}

			if (!ReplayState.DivergenceReported)
			{
				ReplayState.DivergenceReported = true;
				MessageListClass::Instance.PrintMessage(
					DivergenceMessage, DivergenceMessageDurationFrames,
					ColorScheme::White, true);
			}
		}
		void ComputeAndCaptureGameCRCForCurrentFrame()
		{
			if (!ReplayState.Recording && !ReplayState.Playback)
				return;

			Game::ComputeFrameCRC();
			CaptureGameCRCForCurrentFrame();
		}


		void LogPlaybackDivergenceSummary()
		{

			if (!ReplayState.Playback || ReplayState.CheckedFrameCount == 0)
				return;

			if (ReplayState.MismatchedFrameCount == 0)
			{
				Debug::Log("[Replay] Playback matched the recording on all %d checked frames.\n",
					ReplayState.CheckedFrameCount);
				return;
			}

			Debug::Log("[Replay] Playback diverged on %d of %d checked frames (first frame %d, "
				"last frame %d).\n", ReplayState.MismatchedFrameCount, ReplayState.CheckedFrameCount,
				ReplayState.FirstMismatchFrame, ReplayState.LastMismatchFrame);
		}


		void StopReplaySystem()
		{
			LogPlaybackDivergenceSummary();

			if (ReplayState.Recording)
			{
				FlushPendingRecordedFramesThrough(std::numeric_limits<int>::max(), 0);
				if (ReplayState.File.IsOpen())
				{
					const bool wroteEnd = ReplayState.FrameWriter.WriteEndMarker(ReplayState.File);
					if (!wroteEnd)
						Debug::Log("[Replay] Failed to write replay end-of-stream marker.\n");

					const bool finishedStream = ReplayState.File.FinishRecording();
					if (!finishedStream)
						Debug::Log("[Replay] Failed to finish the compressed replay stream.\n");

					if (wroteEnd && finishedStream && !ReplayState.File.StampCleanShutdown(ReplayState.FrameWriter.LastFrameNumber()))
						Debug::Log("[Replay] Failed to mark the replay as complete.\n");
				}
			}

			AbortReplaySystem();
		}

		// Pins the camera to the last recorded viewport position.
		void ApplyLockedViewport()
		{
			if (!ReplayState.LockViewport || !ReplayState.HasLockedViewportPos || !TacticalClass::Instance)
				return;

			auto* const pTactical = TacticalClass::Instance;
			const Point2D& target = ReplayState.LockedViewportPos;

			// Avoid forcing a full repaint when the viewport is already correct.
			if (pTactical->TacticalCoord1.X == target.X && pTactical->TacticalCoord1.Y == target.Y)
				return;

			pTactical->TacticalCoord1 = target;
			pTactical->TacticalCoord2 = target;
			pTactical->RecalculateViewport();
			pTactical->Redrawing = true;
		}

		// Whether playback should show the whole map instead of the recording player's shroud.
		bool PlaybackWantsFullMapReveal()
		{
			return ReplayState.Playback
				&& (!ReplayState.ShroudEnabled || ReplayState.SpectatorView);
		}

		void RestoreFrameState()
		{
			if (!ReplayState.Playback)
				return;

			const int currentFrame = static_cast<int>(Unsorted::CurrentFrame);
			if (ReplayState.PreparedPlaybackFrame == currentFrame)
				return;

			// Set this before reading: every successful or empty sparse frame is prepared exactly
			// once. A read failure stops playback and ResetRuntimeFlagsForScenario clears it.
			ReplayState.PreparedPlaybackFrame = currentFrame;

			ReplayState.ExpectedEventsThisFrame = 0;
			ReplayState.HasExpectedGameCRC = false;
			ReplayState.HighestPlayedFrame = std::max(ReplayState.HighestPlayedFrame,
				currentFrame);

			// Keep the viewport locked on frames without replay records.
			ApplyLockedViewport();

			if (!ReplayState.HasPendingPlaybackFrame && !ReplayState.PlaybackStreamEnded)
			{
				PlaybackFrameRecord nextRecord {};
				if (!ReplayState.FrameReader.ReadFrame(ReplayState.File, nextRecord, SanitizeSideChannelRecord))
				{
					Debug::Log("[Replay] Failed to read frame state during playback.\n");
					StopReplaySystem();
					return;
				}

				if (nextRecord.EndOfStream)
				{
					ReplayState.PlaybackStreamEnded = true;
				}
				else
				{
					ReplayState.PendingPlaybackFrame = std::move(nextRecord);
					ReplayState.HasPendingPlaybackFrame = true;
				}
			}

			if (!ReplayState.HasPendingPlaybackFrame)
				return;

			const auto& frameRecord = ReplayState.PendingPlaybackFrame;
			if (frameRecord.FrameNumber < Unsorted::CurrentFrame)
			{
				Debug::Log("[Replay] Frame mismatch during playback (expected %u got %d).\n",
					Unsorted::CurrentFrame, frameRecord.FrameNumber);
				StopReplaySystem();
				return;
			}

			if (frameRecord.FrameNumber > Unsorted::CurrentFrame)
				return;

			ReplayState.ExpectedEventsThisFrame = frameRecord.EventCountThisFrame;

			if ((frameRecord.Flags & FrameRecordFlag_TacticalPos) != 0u)
			{
				// Track this even when locking is disabled.
				ReplayState.LockedViewportPos = frameRecord.TacticalPos;
				ReplayState.HasLockedViewportPos = true;

				ApplyLockedViewport();
			}

			if ((frameRecord.Flags & FrameRecordFlag_Selection) != 0u)
			{
				ReplayState.LockedSelectionIDs = frameRecord.SelectedObjectIDs;
				ReplayState.HasLockedSelection = true;
				ApplyPlaybackSelection(frameRecord);
			}

			// After the selection, whose own springs are suppressed, and before this frame's
			// LogicClass::AI - which is where the recording's click sprang them too.
			if ((frameRecord.Flags & FrameRecordFlag_SelectionTriggers) != 0u)
				SpringRecordedSelectionTriggers(frameRecord);

			if ((frameRecord.Flags & FrameRecordFlag_SideChannel) != 0u)
			{
				for (const auto& sideChannelEvent : frameRecord.SideChannelEvents)
					ApplySideChannelEvent(sideChannelEvent);
			}

			if ((frameRecord.Flags & FrameRecordFlag_GameCRC) != 0u)
			{
				ReplayState.ExpectedGameCRC = frameRecord.GameCRC;
				ReplayState.HasExpectedGameCRC = true;
			}


			// Applied from the same point in the frame the recording read it from.
			if ((frameRecord.Flags & FrameRecordFlag_GameSpeed) != 0u)
			{
				const int gameSpeed = std::clamp(static_cast<int>(frameRecord.GameSpeed), 0, MaxGameSpeedIndex);
				GameOptionsClass::Instance.GameSpeed = gameSpeed;
				GameModeOptionsClass::Instance.GameSpeed = gameSpeed;
			}

			ReplayState.HasPendingPlaybackFrame = false;
		}

		// Opens a fresh capture buffer for the frame. The events themselves are appended by
		// RecordExecutedEvent as the engine consumes them.
		void CaptureEventsForCurrentFrame()
		{
			if (!ReplayState.Recording)
				return;

			ReplayState.CapturedFrameEvents.clear();
			ReplayState.CapturedFrameEventsFrame = Unsorted::CurrentFrame;
		}

		// Record events as the game executes the DoList.
		void RecordExecutedEvent(const EventClass* pEvent)
		{
			if (!ReplayState.Recording || !pEvent)
				return;

			if (ReplayState.CapturedFrameEventsFrame != Unsorted::CurrentFrame)
			{
				ReplayState.CapturedFrameEvents.clear();
				ReplayState.CapturedFrameEventsFrame = Unsorted::CurrentFrame;
			}

			auto& recorded = ReplayState.CapturedFrameEvents.emplace_back(*pEvent);
			// Events are stored unexecuted so the same event is always the same bytes on disk.
			recorded.IsExecuted = false;
		}

		void RecordCapturedEventsForCurrentFrame()
		{
			if (!ReplayState.Recording)
				return;

			const int frameNumber = Unsorted::CurrentFrame;
			auto& capturedEvents = ReplayState.CapturedFrameEvents;
			const bool hasCapturedEventsForFrame = ReplayState.CapturedFrameEventsFrame == frameNumber;
			const int eventsThisFrame = hasCapturedEventsForFrame
				? static_cast<int>(capturedEvents.size())
				: 0;

			FlushPendingRecordedFramesThrough(frameNumber, eventsThisFrame);
			if (!ReplayState.Recording)
				return;

			if (hasCapturedEventsForFrame && !capturedEvents.empty()
				&& !ReplayState.FrameWriter.WriteEvents(ReplayState.File, capturedEvents))
			{
				Debug::Log("[Replay] Failed writing events to replay stream.\n");
				StopReplaySystem();
				return;
			}

			capturedEvents.clear();
			ReplayState.CapturedFrameEventsFrame = -1;

			if (frameNumber - ReplayState.LastSyncFlushFrame >= SyncFlushFrameInterval)
			{
				ReplayState.LastSyncFlushFrame = frameNumber;
				SyncFlushRecordingStream();
			}
		}

		void PushSideChannelEvent(SideChannelRecord&& record)
		{
			record.FrameNumber = Unsorted::CurrentFrame;
			ReplayState.PendingSideChannelEvents.push_back(std::move(record));
		}

		void PlaybackFrameEvents()
		{
			if (!ReplayState.Playback)
				return;

			const int eventsToReplay = ReplayState.ExpectedEventsThisFrame;
			ReplayState.ExpectedEventsThisFrame = 0;

			for (int i = 0; i < eventsToReplay; ++i)
			{
				// EventClass has no default constructor, so read into raw storage.
				alignas(EventClass) char eventBuffer[sizeof(EventClass)] = { 0 };
				EventClass* replayEvent = reinterpret_cast<EventClass*>(eventBuffer);

				if (!ReplayState.FrameReader.ReadEvent(ReplayState.File, *replayEvent))
				{
					Debug::Log("[Replay] Event stream ended unexpectedly during playback.\n");
					StopReplaySystem();
					return;
				}

				if (!IsReplayableGameplayEvent(*replayEvent))
					continue;

				replayEvent->IsExecuted = false;
				if (!EventClass::DoList.Add(*replayEvent))
				{
					Debug::Log("[Replay] DoList is full while injecting replay events.\n");
					StopReplaySystem();
					return;
				}
			}
		}

		void FinishRecordingAtMissionEnd()
		{
			if (!ReplayState.Recording)
				return;

			Debug::Log("[Replay] Mission over at frame %d; finishing the recording in %s. Later missions in "
				"this campaign are not recorded.\n", ReplayState.FrameWriter.LastFrameNumber(), Replay::GetRecordingOutputPath(GetConfig()));

			StopReplaySystem();
			ReplayState.RecordingFinishedForSession = true;
		}

		void StartReplayRecording()
		{
			// A campaign mission that has already been recorded owns the output file for the rest of the
			// launch; see FinishRecordingAtMissionEnd.
			if (ReplayState.RecordingFinishedForSession)
			{
				Debug::Log("[Replay] Not recording this scenario: %s already holds this campaign's first "
					"mission.\n", Replay::GetRecordingOutputPath(GetConfig()));
				StopReplaySystem();
				return;
			}

			AbortReplaySystem();
			ReplayState.Recording = true;

			const auto* pConfig = GetConfig();
			ReplayState.ShroudEnabled = pConfig ? pConfig->ReplayShroudEnabled : false;
			// The config's free-camera setting is the inverse of the viewport lock kept internally.
			ReplayState.LockViewport = pConfig ? !pConfig->ReplayFreeCamera : true;
			ReplayState.SelectUnits = pConfig ? pConfig->ReplayShowSelections : true;

			if (!ScenarioClass::Instance)
			{
				Debug::Log("[Replay] ScenarioClass instance unavailable at recording start.\n");
				StopReplaySystem();
				return;
			}

			if (!Replay::WriteInitialReplayFile(pConfig))
			{
				Debug::Log("[Replay] Failed to write replay header.\n");
				StopReplaySystem();
				return;
			}

			if (!ReplayState.File.OpenRecording(Replay::GetRecordingOutputPath(pConfig)))
			{
				Debug::Log("[Replay] Failed to open replay file for recording.\n");
				StopReplaySystem();
			}
		}

		void StartReplayPlayback(const char* replayPath)
		{
			AbortReplaySystem();
			ReplayState.Playback = true;

			const auto* pConfig = GetConfig();

			// ReplayPlaybackSpeed is a frame rate, one of the rungs of the viewer's speed ladder,
			// not a game speed index. Zero or less watches at the speed it was recorded at.
			const int requestedFPS = pConfig ? pConfig->ReplayPlaybackSpeed : 0;

			int recordedGameSpeed = std::clamp(GameOptionsClass::Instance.GameSpeed, 0, MaxGameSpeedIndex);
			if (!ReplayState.HasPlaybackHeader)
			{
				ReplayHeader header {};
				if (Replay::ReadReplayHeaderFromPath(replayPath, header))
				{
					ReplayState.PlaybackHeader = header;
					ReplayState.HasPlaybackHeader = true;
				}
			}

			if (ReplayState.HasPlaybackHeader)
				recordedGameSpeed = std::clamp(static_cast<int>(ReplayState.PlaybackHeader.RecordedGameSpeed), 0, MaxGameSpeedIndex);

			ReplayState.PlaybackFPS = requestedFPS > 0
				? requestedFPS
				: GetReplayFPSFromGameSpeed(recordedGameSpeed);

			// The speed the recording started at, applied once and never re-pinned: a change the
			// player made during the game is in the event stream and is replayed like any other.
			GameOptionsClass::Instance.GameSpeed = recordedGameSpeed;
			GameModeOptionsClass::Instance.GameSpeed = recordedGameSpeed;

			ReplayState.ShroudEnabled = pConfig ? pConfig->ReplayShroudEnabled : false;
			// The config's free-camera setting is the inverse of the viewport lock kept internally.
			ReplayState.LockViewport = pConfig ? !pConfig->ReplayFreeCamera : true;
			ReplayState.SelectUnits = pConfig ? pConfig->ReplayShowSelections : true;
			ReplayState.SpectatorView = ReplaySystem::IsSpectatorPlayback();
			ReplayState.ShowChatAndBeacons = pConfig ? pConfig->ReplayShowChatAndBeacons : true;

			// Both of these reproduce the recording player's own screen, so neither means anything
			// once the viewpoint has been handed to someone else.
			if (ResolveViewPlayerIndex() > 0)
			{
				ReplayState.LockViewport = false;
				ReplayState.SelectUnits = false;
			}

			strncpy_s(ReplayState.PlaybackPath, sizeof(ReplayState.PlaybackPath), replayPath, _TRUNCATE);

			ReplaySystem::Controls::InitControlBarVisibility();
			ReplaySystem::Seek::OnPlaybackStarted();

			ReplayOpenFailure failure = ReplayOpenFailure::None;
			if (!ReplayState.File.OpenPlayback(ReplayState.PlaybackPath, failure))
			{
				StopReplaySystem();

				// StartScenario already skipped CreateConnections, so there is no live session to
				// fall back to. This message is all the player gets, so it names the reason.
				Debug::FatalErrorAndExit("[Replay] Cannot play %s: %s.",
					ReplayState.PlaybackPath, Replay::DescribeReplayOpenFailure(failure));
			}
		}
	}
}

// The public API below is defined outside Internal, so pull its names in.
using namespace ReplaySystem::Internal;

void ReplaySystem::RecordChatMessage(int houseIndex, const wchar_t* senderName, const wchar_t* message, int colorSchemeIndex)
{
	if (!ReplayState.Recording)
		return;

	SideChannelRecord record {};
	record.Type = static_cast<uint8_t>(SideChannelEventType::ChatMessage);
	record.House = houseIndex;
	record.Aux = colorSchemeIndex;
	if (senderName)
		wcsncpy_s(record.SenderName, senderName, _TRUNCATE);
	if (message)
		wcsncpy_s(record.Text, message, _TRUNCATE);

	PushSideChannelEvent(std::move(record));
}

void ReplaySystem::RecordTaunt(int tauntCommand)
{
	if (!ReplayState.Recording)
		return;

	SideChannelRecord record {};
	record.Type = static_cast<uint8_t>(SideChannelEventType::Taunt);
	record.Aux = tauntCommand;

	PushSideChannelEvent(std::move(record));
}

void ReplaySystem::RecordBeaconPlace(int houseIndex, const CoordStruct& coord, int beaconSlot)
{
	if (!ReplayState.Recording)
		return;

	SideChannelRecord record {};
	record.Type = static_cast<uint8_t>(SideChannelEventType::BeaconPlace);
	record.House = houseIndex;
	record.Aux = beaconSlot;
	record.Coord = coord;

	PushSideChannelEvent(std::move(record));
}

void ReplaySystem::RecordBeaconDelete(int houseIndex, int beaconSlot)
{
	if (!ReplayState.Recording)
		return;

	SideChannelRecord record {};
	record.Type = static_cast<uint8_t>(SideChannelEventType::BeaconDelete);
	record.House = houseIndex;
	record.Aux = beaconSlot;

	PushSideChannelEvent(std::move(record));
}

void ReplaySystem::RecordBeaconText(int houseIndex, int beaconSlot, const wchar_t* text)
{
	if (!ReplayState.Recording)
		return;

	SideChannelRecord record {};
	record.Type = static_cast<uint8_t>(SideChannelEventType::BeaconText);
	record.House = houseIndex;
	record.Aux = beaconSlot;
	if (text)
		wcsncpy_s(record.Text, text, _TRUNCATE);

	PushSideChannelEvent(std::move(record));
}

bool ReplaySystem::IsPlaybackRequested()
{
	const auto* pConfig = GetConfig();
	return pConfig && pConfig->ReplayFile[0] != '\0';
}

bool ReplaySystem::IsPlaybackActive()
{
	return ReplayState.Playback;
}

int ReplaySystem::GetViewPlayerIndex()
{
	return ResolveViewPlayerIndex();
}

void ReplaySystem::ApplyPlaybackViewPlayer()
{
	ApplyViewPlayerToCurrentPlayer();
}

bool ReplaySystem::IsSpectatorPlayback()
{
	const auto* pConfig = GetConfig();

	return pConfig && IsPlaybackRequested() && pConfig->ReplaySpectator;
}

namespace
{
	bool TakeSpectatorSeat()
	{
		if (!ReplaySystem::IsSpectatorPlayback())
			return false;

		HouseClass* const pPlayer = HouseClass::CurrentPlayer;
		if (!pPlayer)
			return false;

		Game::ObserverMode = true;

		if (pPlayer->MakeObserver())
			TabClass::Instance.ThumbActive = false;

		return true;
	}
}

void ReplaySystem::ApplyPlaybackSpectator()
{
	if (TakeSpectatorSeat())
		Debug::Log("[Replay] Watching the replay from an observer seat.\n");
}

void ReplaySystem::ReapplyPlaybackSpectator()
{
	if (!IsSpectatorPlayback())
		return;

	if (!TakeSpectatorSeat())
		Debug::Log("[Replay] The observer seat could not be taken again after the keyframe load.\n");
}

void ReplaySystem::OnGameStartReset()
{
	StopReplaySystem();
	ReplayState.InitRandomHandled = false;
	ReplayState.RecordingFinishedForSession = false;
	ReplayState.PlaybackPath[0] = '\0';
}
