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

#include "ReplaySeek.h"
#include "ReplayControls.h"
#include "ReplayOverlay.h"
#include "ReplaySystem.h"
#include "ReplaySystem.Internal.h"

#include <Spawner/Spawner.h>
#include <Utilities/Debug.h>

#include <BuildingClass.h>
#include <GameModeOptionsClass.h>
#include <GameOptionsClass.h>
#include <HouseClass.h>
#include <InfantryClass.h>
#include <LoadOptionsClass.h>
#include <MapClass.h>
#include <ScenarioClass.h>
#include <Unsorted.h>
#include <UnitClass.h>

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <type_traits>
#include <vector>

using namespace ReplaySystem::Internal;

namespace ReplaySystem
{
	namespace Seek
	{
		namespace
		{
			// Keyframes are savegames, and the spawner rewrites every savegame path to sit under
			// SavedGameDir (see SavedGamesInSubdir.cpp), so these names are relative to that and the
			// folder below it has to exist before the first save.
			constexpr const char* KeyframeSubdirectory = "Replay Keyframes";

			// A seek still draws every so many frames, so a long one reads as progress rather than
			// as a hang. Everything between is simulated without being drawn, which is most of the
			// cost of a frame.
			constexpr int SeekRenderInterval = 60;

			// A backwards seek always has somewhere to land, because playback drops this one as it
			// starts. Frame 0 is the state before the first frame ran.
			constexpr int FirstKeyframeFrame = 0;
			constexpr size_t ScenarioRandomTableSize = 0xFA;

			struct ScenarioRandomState
			{
				bool Unknown00 = false;
				int Next1 = 0;
				int Next2 = 0;
				std::array<DWORD, ScenarioRandomTableSize> Table {};
			};

			struct Keyframe
			{
				int Frame = 0;
				// Load constructs the saved object graph after ScenarioClass::Load has restored
				// this counter. Those constructors consume IDs even though each object's Load then
				// replaces its temporary ID with the saved one, so the counter has to be restored
				// explicitly once the whole load has finished.
				int ScenarioUniqueID = 0;
				// Compute_Game_CRC consumes the scenario RNG and walks these vectors in their current
				// order. Savegame loading rebuilds the object graph and is free to perturb both even
				// when it restores the same objects, so a seek keyframe retains them out of band.
				ScenarioRandomState Random;
				std::vector<uint32_t> InfantryOrder;
				std::vector<uint32_t> UnitOrder;
				std::vector<uint32_t> BuildingOrder;
				std::vector<uint32_t> HouseOrder;
				std::array<std::vector<uint32_t>, 5> LayerOrders;
				std::vector<uint32_t> LogicOrder;
			};

			struct SeekState
			{
				bool StoreReady = false;
				int Interval = 0;
				std::vector<Keyframe> Keyframes;

				bool Seeking = false;
				int TargetFrame = -1;
				// Set when a seek needs a keyframe loaded, and consumed at the top of the next
				// frame - a load tears the world down and rebuilds it, which is not something to do
				// from the middle of one.
				bool LoadPending = false;
				Keyframe PendingLoadKeyframe;
				bool LoadInProgress = false;

				int FramesSinceRender = 0;
				// What playback was doing before the seek, restored when it lands.
				bool ResumePaused = false;
				bool VocAllowedBeforeSeek = true;
			};

			SeekState State;

			// VocClass::Play (0x750920) returns immediately when this is clear, which silences the
			// sound effects a seek would otherwise fire off in one burst as it runs through frames.
			bool& VocAllowed()
			{
				return *reinterpret_cast<bool*>(0x8464AC);
			}

			std::filesystem::path KeyframeDirectory()
			{
				const auto* pConfig = GetConfig();
				const char* const savedGameDir = pConfig ? pConfig->SavedGameDir : "Saved Games";

				return std::filesystem::path(savedGameDir) / KeyframeSubdirectory;
			}

			// Relative to SavedGameDir, which is what the savegame path hooks expect.
			void FormatKeyframeName(char* buffer, size_t bufferSize, int frame)
			{
				sprintf_s(buffer, bufferSize, "%s\\rk%08d.sav", KeyframeSubdirectory, frame);
			}

			void RemoveKeyframeFiles()
			{
				std::error_code error {};
				std::filesystem::remove_all(KeyframeDirectory(), error);
			}

			bool EnsureKeyframeDirectory()
			{
				std::error_code error {};
				const auto directory = KeyframeDirectory();

				if (std::filesystem::exists(directory, error))
					return true;

				if (!std::filesystem::create_directories(directory, error))
				{
					Debug::Log("[Replay] Could not create the keyframe folder %s; seeking backwards "
						"will not be available.\n", directory.string().c_str());
					return false;
				}

				return true;
			}

			const Keyframe* NewestKeyframeAtOrBefore(int frame)
			{
				const Keyframe* best = nullptr;
				for (const auto& keyframe : State.Keyframes)
				{
					if (keyframe.Frame <= frame && (!best || keyframe.Frame > best->Frame))
						best = &keyframe;
				}

				return best;
			}

			bool HaveKeyframe(int frame)
			{
				return std::find_if(State.Keyframes.begin(), State.Keyframes.end(),
					[frame](const Keyframe& keyframe) { return keyframe.Frame == frame; })
					!= State.Keyframes.end();
			}

			uint32_t UniqueIDOf(const AbstractClass* pObject)
			{
				return pObject ? static_cast<uint32_t>(pObject->UniqueID) : 0u;
			}

			template <typename TCollection>
			void CaptureObjectOrder(const TCollection& collection, std::vector<uint32_t>& outOrder)
			{
				outOrder.clear();
				outOrder.reserve(static_cast<size_t>(std::max(collection.Count, 0)));
				for (int i = 0; i < collection.Count; ++i)
					outOrder.push_back(UniqueIDOf(collection.Items[i]));
			}

			// Rebuild a collection's pointer order without adding or removing anything. Matching by
			// UniqueID is stable across a save/load because each object's raw Load restores that ID.
			template <typename TCollection>
			bool RestoreObjectOrder(TCollection& collection, const std::vector<uint32_t>& savedOrder,
				const char* collectionName, bool& changed)
			{
				changed = false;
				if (collection.Count != static_cast<int>(savedOrder.size()))
				{
					Debug::Log("[Replay] Keyframe collection %s has %d objects after loading; expected %d.\n",
						collectionName, collection.Count, static_cast<int>(savedOrder.size()));
					return false;
				}

				using Pointer = std::remove_reference_t<decltype(collection.Items[0])>;
				std::vector<Pointer> reordered(savedOrder.size(), nullptr);
				std::vector<bool> used(savedOrder.size(), false);

				for (size_t savedIndex = 0; savedIndex < savedOrder.size(); ++savedIndex)
				{
					bool found = false;
					for (int currentIndex = 0; currentIndex < collection.Count; ++currentIndex)
					{
						const size_t index = static_cast<size_t>(currentIndex);
						if (!used[index] && UniqueIDOf(collection.Items[currentIndex]) == savedOrder[savedIndex])
						{
							reordered[savedIndex] = collection.Items[currentIndex];
							used[index] = true;
							found = true;
							break;
						}
					}

					if (!found)
					{
						Debug::Log("[Replay] Keyframe collection %s is missing object unique ID %u.\n",
							collectionName, savedOrder[savedIndex]);
						return false;
					}
				}

				for (int i = 0; i < collection.Count; ++i)
				{
					if (collection.Items[i] != reordered[static_cast<size_t>(i)])
						changed = true;

					collection.Items[i] = reordered[static_cast<size_t>(i)];
				}

				return true;
			}

			bool CaptureKeyframeState(int frame, Keyframe& keyframe)
			{
				if (!ScenarioClass::Instance)
					return false;

				keyframe.Frame = frame;
				keyframe.ScenarioUniqueID = ScenarioClass::Instance->UniqueID;

				const auto& random = ScenarioClass::Instance->Random;
				keyframe.Random.Unknown00 = random.unknown_00;
				keyframe.Random.Next1 = random.Next1;
				keyframe.Random.Next2 = random.Next2;
				std::copy_n(random.Table, ScenarioRandomTableSize, keyframe.Random.Table.begin());

				CaptureObjectOrder(InfantryClass::Array, keyframe.InfantryOrder);
				CaptureObjectOrder(UnitClass::Array, keyframe.UnitOrder);
				CaptureObjectOrder(BuildingClass::Array, keyframe.BuildingOrder);
				CaptureObjectOrder(HouseClass::Array, keyframe.HouseOrder);
				for (size_t layer = 0; layer < keyframe.LayerOrders.size(); ++layer)
					CaptureObjectOrder(MapClass::ObjectsInLayers[layer], keyframe.LayerOrders[layer]);
				CaptureObjectOrder(LogicClass::Instance, keyframe.LogicOrder);
				return true;
			}

			bool RestoreKeyframeState(const Keyframe& keyframe)
			{
				auto& random = ScenarioClass::Instance->Random;
				const bool randomChanged = random.unknown_00 != keyframe.Random.Unknown00
					|| random.Next1 != keyframe.Random.Next1
					|| random.Next2 != keyframe.Random.Next2
					|| !std::equal(keyframe.Random.Table.begin(), keyframe.Random.Table.end(), random.Table);

				random.unknown_00 = keyframe.Random.Unknown00;
				random.Next1 = keyframe.Random.Next1;
				random.Next2 = keyframe.Random.Next2;
				std::copy(keyframe.Random.Table.begin(), keyframe.Random.Table.end(), random.Table);

				int reorderedCollectionCount = 0;
				auto restoreCollection = [&reorderedCollectionCount](auto& collection,
					const std::vector<uint32_t>& order, const char* name)
				{
					bool changed = false;
					if (!RestoreObjectOrder(collection, order, name, changed))
						return false;

					if (changed)
						++reorderedCollectionCount;
					return true;
				};

				if (!restoreCollection(InfantryClass::Array, keyframe.InfantryOrder, "InfantryClass::Array")
					|| !restoreCollection(UnitClass::Array, keyframe.UnitOrder, "UnitClass::Array")
					|| !restoreCollection(BuildingClass::Array, keyframe.BuildingOrder, "BuildingClass::Array")
					|| !restoreCollection(HouseClass::Array, keyframe.HouseOrder, "HouseClass::Array"))
				{
					return false;
				}

				for (size_t layer = 0; layer < keyframe.LayerOrders.size(); ++layer)
				{
					char name[32] = { 0 };
					sprintf_s(name, "MapClass::Layer[%u]", static_cast<unsigned int>(layer));
					if (!restoreCollection(MapClass::ObjectsInLayers[layer], keyframe.LayerOrders[layer], name))
						return false;
				}

				if (!restoreCollection(LogicClass::Instance, keyframe.LogicOrder, "LogicClass::Instance"))
					return false;

				if (randomChanged || reorderedCollectionCount > 0)
				{
					Debug::Log("[Replay] Keyframe %d restored CRC state after loading "
						"(RNG changed: %s; reordered collections: %d).\n",
						keyframe.Frame, randomChanged ? "yes" : "no", reorderedCollectionCount);
				}

				return true;
			}

			bool WriteKeyframe(int frame)
			{
				if (!State.StoreReady || HaveKeyframe(frame))
					return false;

				char fileName[MAX_PATH] = { 0 };
				FormatKeyframeName(fileName, sizeof(fileName), frame);

				wchar_t description[64] = { 0 };
				swprintf_s(description, L"Replay keyframe %d", frame);

				Keyframe keyframe;
				if (!CaptureKeyframeState(frame, keyframe))
				{
					Debug::Log("[Replay] Could not capture simulation state for keyframe %d.\n", frame);
					return false;
				}

				if (!ScenarioClass::SaveGame(fileName, description))
				{
					Debug::Log("[Replay] Failed to write the keyframe for frame %d.\n", frame);
					return false;
				}

				State.Keyframes.push_back(std::move(keyframe));
				return true;
			}

			// Everything a load leaves in a state playback cannot use: the frame counter it may or
			// may not have restored, the simulation speed the in-game teardown resets on the way
			// through, and the frame stream, which is still sitting wherever the seek left it.
			bool RestorePlaybackAfterLoad(const Keyframe& keyframe)
			{
				const int keyframeFrame = keyframe.Frame;
				if (static_cast<int>(Unsorted::CurrentFrame) != keyframeFrame)
				{
					Debug::Log("[Replay] Keyframe %d restored the frame counter as %d; forcing it.\n",
						keyframeFrame, static_cast<int>(Unsorted::CurrentFrame));
					Unsorted::CurrentFrame = keyframeFrame;
				}

				if (!ScenarioClass::Instance)
				{
					Debug::Log("[Replay] Keyframe %d loaded without a scenario.\n", keyframeFrame);
					return false;
				}

				if (ScenarioClass::Instance->UniqueID != keyframe.ScenarioUniqueID)
				{
					Debug::Log("[Replay] Keyframe %d advanced the scenario unique ID from %d to %d "
						"while loading; restoring it.\n", keyframeFrame, keyframe.ScenarioUniqueID,
						ScenarioClass::Instance->UniqueID);
					ScenarioClass::Instance->UniqueID = keyframe.ScenarioUniqueID;
				}

				if (!RestoreKeyframeState(keyframe))
					return false;

				// SessionClass's in-game teardown, which the load runs through, puts the game speed
				// back to whatever a live game last used. Playback pins it to the recorded one.
				if (ReplayState.HasPlaybackHeader)
				{
					const int recordedGameSpeed = std::clamp(
						static_cast<int>(ReplayState.PlaybackHeader.RecordedGameSpeed), 0, MaxGameSpeedIndex);

					GameOptionsClass::Instance.GameSpeed = recordedGameSpeed;
					GameModeOptionsClass::Instance.GameSpeed = recordedGameSpeed;
				}

				ReplayState.DivergenceReported = false;

				// The panel is still on screen but the world under it has been swapped out.
				Overlay::CancelInteraction();

				return RepositionPlaybackStreamToFrame(keyframeFrame);
			}

			bool LoadKeyframe(const Keyframe& keyframe)
			{
				char fileName[MAX_PATH] = { 0 };
				FormatKeyframeName(fileName, sizeof(fileName), keyframe.Frame);

				State.LoadInProgress = true;
				// LoadOptionsClass::LoadMission is the engine's own in-game load: it stops the
				// sounds and the movies, tears the world down and rebuilds it from the file. Going
				// through it rather than Load_Game keeps the mouse and dialog bookkeeping that the
				// engine does around a load.
				const bool loaded = LoadOptionsClass::LoadMission(fileName);
				State.LoadInProgress = false;

				if (!loaded)
				{
					Debug::Log("[Replay] Failed to load the keyframe for frame %d.\n", keyframe.Frame);
					return false;
				}

				return RestorePlaybackAfterLoad(keyframe);
			}

			void EndSeek()
			{
				if (!State.Seeking)
					return;

				State.Seeking = false;
				State.TargetFrame = -1;
				State.FramesSinceRender = 0;

				VocAllowed() = State.VocAllowedBeforeSeek;

				// A seek out of a paused replay leaves it paused on the frame asked for, which is
				// what stepping and scrubbing both want.
				if (State.ResumePaused)
					Controls::SetPlaybackPaused(true);

				// The pacing deadline is stale by however long the seek took.
				ReplayState.PlaybackNextFrameDue = 0.0;
			}

			void BeginSeek(int targetFrame, bool pauseOnArrival)
			{
				if (!State.Seeking)
				{
					State.ResumePaused = Controls::IsPlaybackPaused();
					State.VocAllowedBeforeSeek = VocAllowed();
				}

				State.ResumePaused = State.ResumePaused || pauseOnArrival;

				State.Seeking = true;
				State.TargetFrame = targetFrame;
				State.FramesSinceRender = 0;

				// Frames run back to back with no pacing while this holds, so the sound effects of
				// every one of them would land at once.
				VocAllowed() = false;
				Controls::SetPlaybackPaused(false);
			}
		}

		int KeyframeInterval()
		{
			return State.Interval;
		}

		bool IsLoadInProgress()
		{
			return State.LoadInProgress;
		}

		void OnPlaybackStarted()
		{
			State = SeekState {};

			const auto* pConfig = GetConfig();
			State.Interval = pConfig ? std::max(0, pConfig->ReplayKeyframeInterval) : 0;

			if (State.Interval <= 0)
			{
				Debug::Log("[Replay] Keyframes are off; seeking backwards is not available.\n");
				return;
			}

			// A previous playback that died without cleaning up would otherwise leave its keyframes
			// to be mistaken for this one's.
			RemoveKeyframeFiles();
			State.StoreReady = EnsureKeyframeDirectory();
		}

		void OnPlaybackStopped()
		{
			if (State.StoreReady)
				RemoveKeyframeFiles();

			if (State.Seeking)
				VocAllowed() = State.VocAllowedBeforeSeek;

			State = SeekState {};
		}

		void ServiceFrameStart()
		{
			if (!ReplayState.Playback)
				return;

			if (State.LoadPending)
			{
				Keyframe keyframe = std::move(State.PendingLoadKeyframe);
				State.LoadPending = false;
				State.PendingLoadKeyframe = {};

				if (!LoadKeyframe(keyframe))
				{
					// Playback is now sitting on a state that does not match the stream, so there
					// is nothing sensible to carry on from.
					Debug::Log("[Replay] Seek failed; stopping playback.\n");
					EndSeek();
					StopReplaySystem();
					return;
				}
			}

			if (State.Seeking && static_cast<int>(Unsorted::CurrentFrame) >= State.TargetFrame)
				EndSeek();

			if (ReplayState.PlaybackStreamEnded && State.Seeking)
				EndSeek();

			if (State.Interval <= 0 || !State.StoreReady)
				return;

			const int frame = static_cast<int>(Unsorted::CurrentFrame);
			if (frame == FirstKeyframeFrame || (frame > 0 && frame % State.Interval == 0))
				WriteKeyframe(frame);
		}

		bool RequestSeek(int targetFrame, bool pauseOnArrival)
		{
			if (!ReplayState.Playback)
				return false;

			const int currentFrame = static_cast<int>(Unsorted::CurrentFrame);
			targetFrame = std::max(0, targetFrame);

			if (targetFrame == currentFrame)
			{
				if (pauseOnArrival)
					Controls::SetPlaybackPaused(true);

				return true;
			}

			if (targetFrame > currentFrame)
			{
				// Forwards is just the simulation run without being drawn; no keyframe needed.
				BeginSeek(targetFrame, pauseOnArrival);
				return true;
			}

			const Keyframe* const keyframe = NewestKeyframeAtOrBefore(targetFrame);
			if (!keyframe)
			{
				Debug::Log("[Replay] No keyframe at or before frame %d; cannot seek back there.\n",
					targetFrame);
				return false;
			}

			BeginSeek(targetFrame, pauseOnArrival);
			State.LoadPending = true;
			State.PendingLoadKeyframe = *keyframe;
			return true;
		}

		bool IsSeeking()
		{
			return State.Seeking;
		}

		int SeekTargetFrame()
		{
			return State.Seeking ? State.TargetFrame : -1;
		}

		bool ShouldSkipRenderThisFrame()
		{
			return State.Seeking && State.FramesSinceRender < SeekRenderInterval;
		}

		void CountRenderedFrame()
		{
			if (!State.Seeking)
				return;

			if (State.FramesSinceRender < SeekRenderInterval)
				++State.FramesSinceRender;
			else
				State.FramesSinceRender = 0;
		}

		int EarliestSeekableFrame()
		{
			if (State.Keyframes.empty())
				return static_cast<int>(Unsorted::CurrentFrame);

			return std::min_element(State.Keyframes.begin(), State.Keyframes.end(),
				[](const Keyframe& lhs, const Keyframe& rhs) { return lhs.Frame < rhs.Frame; })->Frame;
		}

		int CollectKeyframeFrames(int* outFrames, int maxFrames)
		{
			if (!outFrames || maxFrames <= 0)
				return 0;

			const int count = std::min(maxFrames, static_cast<int>(State.Keyframes.size()));
			for (int i = 0; i < count; ++i)
				outFrames[i] = State.Keyframes[static_cast<size_t>(i)].Frame;

			std::sort(outFrames, outFrames + count);
			return count;
		}

		int CurrentFrame()
		{
			return static_cast<int>(Unsorted::CurrentFrame);
		}

		int TotalFrames()
		{
			const int played = std::max(ReplayState.HighestPlayedFrame, CurrentFrame());

			// A recording that died with the process never got its length stamped in, so the only
			// honest answer is how far playback has got.
			const int recorded = ReplayState.HasPlaybackHeader
				? static_cast<int>(ReplayState.PlaybackHeader.TotalFrames)
				: 0;

			return std::max(recorded, played);
		}

		int RecordedFPS()
		{
			if (!ReplayState.HasPlaybackHeader)
				return GetReplayFPSFromGameSpeed(0);

			return GetReplayFPSFromGameSpeed(
				static_cast<int>(ReplayState.PlaybackHeader.RecordedGameSpeed));
		}
	}
}
