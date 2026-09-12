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
#include "ReplayCheckpointCodec.h"
#include "ReplayRecordedCheckpoint.h"
#include "ReplayKeyframeState.h"
#include "ReplayControls.h"
#include "ReplayOverlay.h"
#include "ReplaySystem.h"
#include "ReplaySystem.Internal.h"

#include <Spawner/Spawner.h>
#include <Utilities/Debug.h>
#include <Vendor/miniz/miniz.h>

#include <EventClass.h>
#include <GameModeOptionsClass.h>
#include <GameOptionsClass.h>
#include <LoadOptionsClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <Surface.h>
#include <TacticalClass.h>
#include <Unsorted.h>
#include <VocClass.h>

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

using namespace ReplaySystem::Internal;

namespace ReplaySystem
{
	namespace Seek
	{
		namespace
		{
			constexpr const char* KeyframeSubdirectory = "Replay Keyframes";

			constexpr int SeekRenderInterval = 60;

			// A backwards seek always has somewhere to land, because playback drops this one as it
			// starts. Frame 0 is the state before the first frame ran.
			constexpr int FirstKeyframeFrame = 0;

			// A rewind point pairs a temporary savegame with its simulation sidecar.
			// It is either captured during playback or extracted from the recording.
			struct Keyframe
			{
				int Frame = 0;
				uint64_t FileBytes = 0;
				KeyframeState::Snapshot Snapshot;
			};

			struct SeekState
			{
				bool StoreReady = false;
				int Interval = 0;
				uint64_t StorageLimitBytes = 0;
				uint64_t KeyframeBytes = 0;
				std::vector<Keyframe> Keyframes;

				bool Seeking = false;
				int TargetFrame = -1;
				// Loaded at the start of the next frame.
				bool LoadPending = false;
				int PendingLoadFrame = -1;
				bool LoadInProgress = false;

				int FramesSinceRender = 0;
				// What playback was doing before the seek, restored when it lands.
				bool ResumePaused = false;
				bool VocAllowedBeforeSeek = true;
			};

			SeekState State;

			using Replay::RecordedCheckpoint;
			using Replay::MaxRecordedCheckpoints;
			using Replay::MaxRecordedCheckpointBytes;
			std::vector<RecordedCheckpoint> RecordedCheckpoints;
			bool RecordingSimulationInProgress = false;

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

			std::filesystem::path KeyframePath(int frame)
			{
				char name[32] = { 0 };
				sprintf_s(name, "rk%08d.sav", frame);
				return KeyframeDirectory() / name;
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

			const Keyframe* FindKeyframe(int frame)
			{
				const auto found = std::find_if(State.Keyframes.begin(), State.Keyframes.end(),
					[frame](const Keyframe& keyframe) { return keyframe.Frame == frame; });
				return found == State.Keyframes.end() ? nullptr : &*found;
			}

			void EvictOldKeyframes()
			{
				while (State.StorageLimitBytes > 0
					&& State.KeyframeBytes > State.StorageLimitBytes
					&& State.Keyframes.size() > 1)
				{
					const auto oldest = std::min_element(State.Keyframes.begin(), State.Keyframes.end(),
						[](const Keyframe& lhs, const Keyframe& rhs) { return lhs.Frame < rhs.Frame; });

					std::error_code error {};
					std::filesystem::remove(KeyframePath(oldest->Frame), error);
					if (error)
					{
						Debug::Log("[Replay] Could not remove keyframe %d while enforcing the storage limit.\n",
							oldest->Frame);
						break;
					}

					State.KeyframeBytes -= std::min(State.KeyframeBytes, oldest->FileBytes);
					State.Keyframes.erase(oldest);
				}
			}

			bool WriteKeyframe(int frame)
			{
				if (!State.StoreReady || HaveKeyframe(frame))
					return false;

				char fileName[MAX_PATH] = { 0 };
				FormatKeyframeName(fileName, sizeof(fileName), frame);

				wchar_t description[64] = { 0 };
				swprintf_s(description, L"Replay keyframe %d", frame);

				if (!ScenarioClass::SaveGame(fileName, description))
				{
					Debug::Log("[Replay] Failed to write the keyframe for frame %d.\n", frame);
					return false;
				}

				// Capture the state playback continues from once SaveGame has returned.
				Keyframe keyframe;
				keyframe.Frame = frame;
				if (!keyframe.Snapshot.CaptureAfterSave())
				{
					Debug::Log("[Replay] Could not capture post-save simulation state for keyframe %d.\n",
						frame);
					std::error_code error {};
					std::filesystem::remove(KeyframePath(frame), error);
					return false;
				}

				std::error_code sizeError {};
				keyframe.FileBytes = std::filesystem::file_size(KeyframePath(frame), sizeError);
				if (sizeError)
				{
					Debug::Log("[Replay] Could not measure keyframe %d for storage accounting.\n", frame);
					std::error_code removeError {};
					std::filesystem::remove(KeyframePath(frame), removeError);
					return false;
				}

				State.KeyframeBytes += keyframe.FileBytes;
				State.Keyframes.push_back(std::move(keyframe));
				EvictOldKeyframes();
				return true;
			}

			void ImportRecordedCheckpoints()
			{
				std::vector<unsigned char> archive;
				if (!ReplayState.File.ReadCheckpointArchive(archive))
				{
					Debug::Log("[Replay] Ignoring an unreadable checkpoint archive.\n");
					return;
				}
				if (archive.empty()) return;
				Replay::CheckpointCodec::Reader reader { archive };
				uint32_t count = 0;
				reader.Scalar(count);
				if (!reader.Good || count > MaxRecordedCheckpoints) return;
				// Validate the complete envelope before publishing any seek points.
				std::vector<RecordedCheckpoint> records(count);
				int previousFrame = -1;
				for (auto& record : records)
				{
					Replay::CheckpointCodec::Fields(reader, record.Frame, record.RawSize,
						record.CRC, record.Compressed);
					if (!reader.Good || record.Frame <= previousFrame || record.Frame > TotalFrames()
						|| record.RawSize == 0 || record.RawSize > Replay::CheckpointCodec::MaxBytes
						|| record.Compressed.empty()) return;
					previousFrame = record.Frame;
				}
				if (reader.Position != archive.size()) return;
				for (const auto& record : records)
				{
					std::vector<unsigned char> raw(record.RawSize);
					if (tinfl_decompress_mem_to_mem(raw.data(), raw.size(), record.Compressed.data(),
						record.Compressed.size(), 0) != raw.size()
						|| mz_crc32(0, raw.data(), raw.size()) != record.CRC) continue;
					Replay::CheckpointCodec::Reader payload { raw };
					std::vector<unsigned char> save, sidecar;
					Replay::CheckpointCodec::Fields(payload, save, sidecar);
					Keyframe keyframe;
					keyframe.Frame = record.Frame;
					if (!payload.Good || payload.Position != raw.size() || save.empty()
						|| !keyframe.Snapshot.Deserialize(sidecar)) continue;
					const auto path = KeyframePath(record.Frame);
					std::ofstream output(path, std::ios::binary | std::ios::trunc);
					output.write(reinterpret_cast<const char*>(save.data()), save.size());
					output.close();
					if (!output)
					{
						std::error_code error; std::filesystem::remove(path, error);
						continue;
					}
					keyframe.FileBytes = save.size();
					State.KeyframeBytes += keyframe.FileBytes;
					State.Keyframes.push_back(std::move(keyframe));
				}
				EvictOldKeyframes();
				Debug::Log("[Replay] Loaded %u of %u recorded seek checkpoints.\n",
					static_cast<unsigned int>(State.Keyframes.size()), count);
			}

			// LoadMission tears down the in-game session. A seek bypasses the load screen
			// that would normally restore its input, audio, and in-game flag.
			void ResumeInGameSessionAfterLoad()
			{
				SessionClass::Instance.Resume();
			}


			// Loading replaces the surfaces but leaves Temp pointing at the old hidden surface.
			void RepointTempSurfaceAfterLoad()
			{
				DSurface::Temp = DSurface::Hidden;
			}

			// Drop input left over from the previous timeline or generated by loading.
			// Playback will read the keyframe's recorded events from the replay stream again.
			void ClearTransientEventQueuesForLoad(const char* phase, int keyframeFrame)
			{
				const int outCount = EventClass::OutList.Count;
				const int doCount = EventClass::DoList.Count;
				const int megaMissionCount = EventClass::MegaMissionList.Count;

				if (outCount > 0 || doCount > 0 || megaMissionCount > 0)
				{
					Debug::Log("[Replay] Keyframe %d discarded transient event queues %s loading "
						"(out %d, do %d, deferred mega-missions %d).\n",
						keyframeFrame, phase, outCount, doCount, megaMissionCount);

					const int reportCount = std::min(doCount, 4);
					for (int i = 0; i < reportCount; ++i)
					{
						const auto& event = EventClass::DoList[i];
						Debug::Log("[Replay]   discarded DoList event type %d from frame %u "
							"(executed %s).\n", static_cast<int>(event.Type), event.Frame,
							(event.IsExecuted & 1) != 0 ? "yes" : "no");
					}
				}

				EventClass::OutList.Init();
				EventClass::DoList.Init();
				EventClass::MegaMissionList.Init();
				std::memset(EventClass::MegaMissionTargetNum, 0,
					sizeof(EventClass::MegaMissionTargetNum));
				std::memset(EventClass::MegaMissionTargets, 0,
					sizeof(EventClass::MegaMissionTargets));
			}

			bool RestorePlaybackAfterLoad(const Keyframe& keyframe)
			{
				const int keyframeFrame = keyframe.Frame;
				if (!keyframe.Snapshot.RestoreBeforeResume(keyframeFrame))
					return false;


				// Finish session and observer setup before the final object repairs, so that
				// setup cannot overwrite the state those repairs put back.
				RepointTempSurfaceAfterLoad();
				ResumeInGameSessionAfterLoad();
				ReplaySystem::ReapplyPlaybackSpectator();
				keyframe.Snapshot.RestoreAfterResume(keyframeFrame, ReplayState.ShowChatAndBeacons);

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

				const bool restoreViewerViewport = !ReplayState.LockViewport && TacticalClass::Instance;
				const Point2D viewerViewport = restoreViewerViewport
					? TacticalClass::Instance->TacticalCoord1
					: Point2D { 0, 0 };

				ClearTransientEventQueuesForLoad("before", keyframe.Frame);
				State.LoadInProgress = true;
				const bool loaded = LoadOptionsClass::LoadMission(fileName);
				State.LoadInProgress = false;

				if (!loaded)
				{
					Debug::Log("[Replay] Failed to load the keyframe for frame %d.\n", keyframe.Frame);
					return false;
				}

				ClearTransientEventQueuesForLoad("after", keyframe.Frame);
				if (!RestorePlaybackAfterLoad(keyframe))
					return false;

				Point2D viewportAfterLoad {};
				bool restoreViewport = false;
				if (restoreViewerViewport)
				{
					viewportAfterLoad = viewerViewport;
					restoreViewport = true;
				}
				else if (ReplayState.LockViewport)
				{
					const auto& pending = ReplayState.PendingPlaybackFrame;
					if (ReplayState.HasPendingPlaybackFrame
						&& pending.FrameNumber == keyframe.Frame
						&& (pending.Flags & FrameRecordFlag_TacticalPos) != 0u)
					{
						ReplayState.LockedViewportPos = pending.TacticalPos;
						ReplayState.HasLockedViewportPos = true;
					}

					if (ReplayState.HasLockedViewportPos)
					{
						viewportAfterLoad = ReplayState.LockedViewportPos;
						restoreViewport = true;
					}
				}

				if (restoreViewport && TacticalClass::Instance)
				{
					auto* const pTactical = TacticalClass::Instance;
					pTactical->TacticalCoord1 = viewportAfterLoad;
					pTactical->TacticalCoord2 = viewportAfterLoad;
					pTactical->RecalculateViewport();
					pTactical->Redrawing = true;
				}

				return true;
			}

			void EndSeek()
			{
				if (!State.Seeking)
					return;

				State.Seeking = false;
				State.TargetFrame = -1;
				State.FramesSinceRender = 0;

				VocClass::VoicesEnabled = State.VocAllowedBeforeSeek;

				// A seek out of a paused replay leaves it paused on the frame asked for, which is
				// what stepping and scrubbing both want.
				if (State.ResumePaused)
					Controls::SetPlaybackPaused(true);

				// The pacing deadline is stale by however long the seek took.
				Controls::ResetFramePacing();
			}

			void BeginSeek(int targetFrame, bool pauseOnArrival)
			{
				if (!State.Seeking)
				{
					State.ResumePaused = Controls::IsPlaybackPaused();
					State.VocAllowedBeforeSeek = VocClass::VoicesEnabled;
				}

				State.ResumePaused = State.ResumePaused || pauseOnArrival;

				State.Seeking = true;
				State.TargetFrame = targetFrame;
				State.FramesSinceRender = 0;

				// Frames run back to back with no pacing while this holds, so the sound effects of
				// every one of them would land at once.
				VocClass::VoicesEnabled = false;
				Controls::SetPlaybackPaused(false);
			}
		}

		void SetRecordingSimulationInProgress(bool inProgress)
		{
			RecordingSimulationInProgress = inProgress;
		}

		void OnGameSaved(const wchar_t* path)
		{
			if (!ReplayState.Recording || RecordingSimulationInProgress || !path) return;
			const int frame = static_cast<int>(Unsorted::CurrentFrame);
			if (frame < 0 || (!RecordedCheckpoints.empty() && frame <= RecordedCheckpoints.back().Frame)) return;
			std::ifstream input(std::filesystem::path(path), std::ios::binary | std::ios::ate);
			const auto size = input.tellg();
			if (!input || size <= 0 || size > Replay::CheckpointCodec::MaxBytes) return;
			std::vector<unsigned char> save(static_cast<size_t>(size));
			input.seekg(0);
			if (!input.read(reinterpret_cast<char*>(save.data()), save.size())) return;
			KeyframeState::Snapshot snapshot;
			std::vector<unsigned char> sidecar;
			if (!snapshot.CaptureAfterSave() || !snapshot.Serialize(sidecar))
			{
				Debug::Log("[Replay] Save at frame %d could not capture a checkpoint sidecar.\n", frame);
				return;
			}
			Replay::CheckpointCodec::Writer payload;
			Replay::CheckpointCodec::Fields(payload, save, sidecar);
			if (!payload.Good) return;
			RecordedCheckpoint record;
			record.Frame = frame;
			record.RawSize = static_cast<uint32_t>(payload.Bytes.size());
			record.CRC = static_cast<uint32_t>(mz_crc32(0, payload.Bytes.data(), payload.Bytes.size()));
			size_t compressedSize = 0;
			void* compressed = tdefl_compress_mem_to_heap(payload.Bytes.data(), payload.Bytes.size(),
				&compressedSize, 128);
			if (!compressed) return;
			if (compressedSize <= MaxRecordedCheckpointBytes)
			{
				const auto* bytes = static_cast<unsigned char*>(compressed);
				record.Compressed.assign(bytes, bytes + compressedSize);
			}
			mz_free(compressed);
			if (record.Compressed.empty()) return;
			RecordedCheckpoints.push_back(std::move(record));
			Replay::TrimRecordedCheckpoints(RecordedCheckpoints);
			Debug::Log("[Replay] Captured existing save at frame %d (%u compressed bytes; %u retained).\n",
				frame, static_cast<unsigned int>(compressedSize), static_cast<unsigned int>(RecordedCheckpoints.size()));
		}

		void FinishRecordingCheckpoints()
		{
			if (RecordedCheckpoints.empty()) return;
			Replay::CheckpointCodec::Writer writer;
			// A save at shutdown may precede a frame that was never recorded.
			const int lastFrame = ReplayState.FrameWriter.LastFrameNumber();
			std::erase_if(RecordedCheckpoints, [lastFrame](const auto& item) { return item.Frame > lastFrame; });
			uint32_t count = static_cast<uint32_t>(RecordedCheckpoints.size());
			if (count == 0) return;
			writer.Scalar(count);
			for (auto& record : RecordedCheckpoints)
				Replay::CheckpointCodec::Fields(writer, record.Frame, record.RawSize, record.CRC, record.Compressed);
			if (!writer.Good || !ReplayState.File.WriteCheckpointArchive(writer.Bytes))
				Debug::Log("[Replay] Could not append recorded checkpoints; event playback remains available.\n");
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
			const int storageLimitMB = pConfig
				? std::max(0, pConfig->ReplayKeyframeStorageLimitMB)
				: 512;
			State.StorageLimitBytes = static_cast<uint64_t>(storageLimitMB) * 1024u * 1024u;

			// A previous playback that died without cleaning up would otherwise leave its keyframes
			// to be mistaken for this one's.
			RemoveKeyframeFiles();
			State.StoreReady = EnsureKeyframeDirectory();
			if (State.StoreReady) ImportRecordedCheckpoints();
		}

		void OnPlaybackStopped()
		{
			RecordedCheckpoints.clear();
			RecordingSimulationInProgress = false;
			if (State.StoreReady)
				RemoveKeyframeFiles();

			if (State.Seeking)
				VocClass::VoicesEnabled = State.VocAllowedBeforeSeek;

			State = SeekState {};
		}


		void ServiceFrameStart()
		{
			if (!ReplayState.Playback)
				return;

			if (State.LoadPending)
			{
				const int keyframeFrame = State.PendingLoadFrame;
				State.LoadPending = false;
				State.PendingLoadFrame = -1;

				const Keyframe* const keyframe = FindKeyframe(keyframeFrame);
				if (!keyframe || !LoadKeyframe(*keyframe))
				{
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

			const Keyframe* const keyframe = NewestKeyframeAtOrBefore(targetFrame);
			const bool mustGoBack = targetFrame < currentFrame;
			const bool keyframeSkipsAhead = keyframe && keyframe->Frame > currentFrame;

			if (mustGoBack && !keyframe)
			{
				Debug::Log("[Replay] No keyframe at or before frame %d; cannot seek back there.\n",
					targetFrame);
				return false;
			}

			BeginSeek(targetFrame, pauseOnArrival);

			if (mustGoBack || keyframeSkipsAhead)
			{
				State.LoadPending = true;
				State.PendingLoadFrame = keyframe->Frame;
			}

			return true;
		}

		bool IsSeeking()
		{
			return State.Seeking;
		}

		bool ShouldSkipRenderThisFrame()
		{
			if (!State.Seeking)
				return false;


			return State.FramesSinceRender < SeekRenderInterval;
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

			return std::min(CurrentFrame(), std::min_element(State.Keyframes.begin(), State.Keyframes.end(),
				[](const Keyframe& lhs, const Keyframe& rhs) { return lhs.Frame < rhs.Frame; })->Frame);
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
