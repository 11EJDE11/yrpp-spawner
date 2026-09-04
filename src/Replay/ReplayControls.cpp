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
#include "ReplaySeek.h"
#include "ReplaySystem.h"
#include "ReplaySystem.Internal.h"

#include <Spawner/Spawner.h>

#include <ColorScheme.h>
#include <CommandClass.h>
#include <MapClass.h>
#include <MessageListClass.h>
#include <StringTable.h>
#include <TacticalClass.h>
#include <Unsorted.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace ReplaySystem::Internal;

namespace ReplaySystem
{
	namespace Controls
	{
		namespace
		{
			constexpr int ControlMessageDurationFrames = 90;

			// The frame at which a single step takes the pause back. The pause flag is read at four
			// points in a main-loop iteration, so it is held steady from one frame start to the next.
			int SingleStepTargetFrame = -1;
			// Input is handled after RestoreFrameState. A resume requested there must wait for the
			// next frame-start hook or the simulation advances without preparing this frame's record.
			bool ResumePending = false;
			bool ControlBarVisible = false;

			void SetPausedState(bool paused)
			{
				ReplayState.PlaybackPaused = paused;
				// Never carry a pre-pause or single-step deadline into resumed playback. Starting the
				// pacing clock afresh prevents a burst of unpaced frames on resume.
				ReplayState.PlaybackNextFrameDue = 0.0;
			}

			void PrintControlMessageInternal(const wchar_t* pMessage)
			{
				MessageListClass::Instance.PrintMessage(
					pMessage,
					ControlMessageDurationFrames,
					ColorScheme::White,
					/* bSilent: */ true);
			}

			int GetRecordedFPS()
			{
				if (!ReplayState.HasPlaybackHeader)
					return GetReplayFPSFromGameSpeed(0);

				return GetReplayFPSFromGameSpeed(static_cast<int>(ReplayState.PlaybackHeader.RecordedGameSpeed));
			}

			int FindNearestLadderIndex(int fps)
			{
				int bestIndex = 0;
				int bestDistance = std::abs(SpeedLadder[0] - fps);

				for (int i = 1; i < SpeedLadderCount; ++i)
				{
					const int distance = std::abs(SpeedLadder[i] - fps);
					if (distance < bestDistance)
					{
						bestDistance = distance;
						bestIndex = i;
					}
				}

				return bestIndex;
			}

			void ReportPlaybackSpeed()
			{
				const int fps = ReplayState.PlaybackFPS;
				const int recordedFPS = GetRecordedFPS();
				const double multiplier = recordedFPS > 0 ? static_cast<double>(fps) / recordedFPS : 1.0;

				wchar_t message[64];
				swprintf_s(message, L"Replay speed: %.2fx (%d FPS)", multiplier, fps);
				PrintControlMessage(message);
			}

			// How far the jump hotkeys move, in seconds of recorded game time. The seek bar is for
			// anything longer than a nudge.
			constexpr int HotkeySeekSeconds = 10;

			void SeekBySeconds(int seconds)
			{
				if (!ReplayState.Playback)
					return;

				const int target = Seek::CurrentFrame() + seconds * Seek::RecordedFPS();
				if (!Seek::RequestSeek(target))
					PrintControlMessage(L"Nothing to rewind to yet.");
			}

			// None of these labels exist in the stock CSF, so every one carries an English
			// fallback for clients that have not shipped a translation.
			const wchar_t* ReplayCategory()
			{
				return StringTable::TryFetchString("TXT_REPLAY_CATEGORY", L"Replay");
			}

			class ReplayTogglePauseCommandClass : public CommandClass
			{
			public:
				virtual const char* GetName() const override { return "ReplayTogglePause"; }

				virtual const wchar_t* GetUIName() const override
					{ return StringTable::TryFetchString("TXT_REPLAY_PAUSE", L"Replay: Pause/Resume"); }

				virtual const wchar_t* GetUICategory() const override { return ReplayCategory(); }

				virtual const wchar_t* GetUIDescription() const override
				{
					return StringTable::TryFetchString("TXT_REPLAY_PAUSE_DESC",
						L"Freezes replay playback. The map can still be scrolled while paused.");
				}

				virtual void Execute(WWKey) const override { TogglePlaybackPause(); }
			};

			class ReplaySpeedUpCommandClass : public CommandClass
			{
			public:
				virtual const char* GetName() const override { return "ReplaySpeedUp"; }

				virtual const wchar_t* GetUIName() const override
					{ return StringTable::TryFetchString("TXT_REPLAY_SPEED_UP", L"Replay: Speed Up"); }

				virtual const wchar_t* GetUICategory() const override { return ReplayCategory(); }

				virtual const wchar_t* GetUIDescription() const override
				{
					return StringTable::TryFetchString("TXT_REPLAY_SPEED_UP_DESC",
						L"Plays the replay back faster, past the speed a live game allows.");
				}

				virtual void Execute(WWKey) const override { StepPlaybackSpeed(1); }
			};

			class ReplaySpeedDownCommandClass : public CommandClass
			{
			public:
				virtual const char* GetName() const override { return "ReplaySpeedDown"; }

				virtual const wchar_t* GetUIName() const override
					{ return StringTable::TryFetchString("TXT_REPLAY_SLOW_DOWN", L"Replay: Slow Down"); }

				virtual const wchar_t* GetUICategory() const override { return ReplayCategory(); }

				virtual const wchar_t* GetUIDescription() const override
				{
					return StringTable::TryFetchString("TXT_REPLAY_SLOW_DOWN_DESC",
						L"Plays the replay back slower.");
				}

				virtual void Execute(WWKey) const override { StepPlaybackSpeed(-1); }
			};

			class ReplayStepFrameCommandClass : public CommandClass
			{
			public:
				virtual const char* GetName() const override { return "ReplayStepFrame"; }

				virtual const wchar_t* GetUIName() const override
					{ return StringTable::TryFetchString("TXT_REPLAY_STEP_FRAME", L"Replay: Advance One Frame"); }

				virtual const wchar_t* GetUICategory() const override { return ReplayCategory(); }

				virtual const wchar_t* GetUIDescription() const override
				{
					return StringTable::TryFetchString("TXT_REPLAY_STEP_FRAME_DESC",
						L"Runs a single frame and freezes again. Pauses playback first if it is running.");
				}

				virtual void Execute(WWKey) const override { RequestSingleStep(); }
			};

			class ReplaySeekBackCommandClass : public CommandClass
			{
			public:
				virtual const char* GetName() const override { return "ReplaySeekBack"; }

				virtual const wchar_t* GetUIName() const override
					{ return StringTable::TryFetchString("TXT_REPLAY_SEEK_BACK", L"Replay: Jump Back"); }

				virtual const wchar_t* GetUICategory() const override { return ReplayCategory(); }

				virtual const wchar_t* GetUIDescription() const override
				{
					return StringTable::TryFetchString("TXT_REPLAY_SEEK_BACK_DESC",
						L"Jumps ten seconds back, restarting from the nearest keyframe.");
				}

				virtual void Execute(WWKey) const override { SeekBySeconds(-HotkeySeekSeconds); }
			};

			class ReplaySeekForwardCommandClass : public CommandClass
			{
			public:
				virtual const char* GetName() const override { return "ReplaySeekForward"; }

				virtual const wchar_t* GetUIName() const override
					{ return StringTable::TryFetchString("TXT_REPLAY_SEEK_FORWARD", L"Replay: Jump Forward"); }

				virtual const wchar_t* GetUICategory() const override { return ReplayCategory(); }

				virtual const wchar_t* GetUIDescription() const override
				{
					return StringTable::TryFetchString("TXT_REPLAY_SEEK_FORWARD_DESC",
						L"Jumps ten seconds forward, running the frames in between without drawing them.");
				}

				virtual void Execute(WWKey) const override { SeekBySeconds(HotkeySeekSeconds); }
			};

			class ReplayToggleControlBarCommandClass : public CommandClass
			{
			public:
				virtual const char* GetName() const override { return "ReplayToggleControlBar"; }

				virtual const wchar_t* GetUIName() const override
					{ return StringTable::TryFetchString("TXT_REPLAY_CONTROL_BAR", L"Replay: Show/Hide Controls"); }

				virtual const wchar_t* GetUICategory() const override { return ReplayCategory(); }

				virtual const wchar_t* GetUIDescription() const override
				{
					return StringTable::TryFetchString("TXT_REPLAY_CONTROL_BAR_DESC",
						L"Shows or hides the on-screen playback controls.");
				}

				virtual void Execute(WWKey) const override { ToggleControlBar(); }
			};

			class ReplayToggleViewportLockCommandClass : public CommandClass
			{
			public:
				virtual const char* GetName() const override { return "ReplayToggleViewportLock"; }

				virtual const wchar_t* GetUIName() const override
					{ return StringTable::TryFetchString("TXT_REPLAY_VIEWPORT_LOCK", L"Replay: Lock/Unlock Viewport"); }

				virtual const wchar_t* GetUICategory() const override { return ReplayCategory(); }

				virtual const wchar_t* GetUIDescription() const override
				{
					return StringTable::TryFetchString("TXT_REPLAY_VIEWPORT_LOCK_DESC",
						L"Toggles following the viewport recorded in the replay.");
				}

				virtual void Execute(WWKey) const override { ToggleLockedViewport(); }
			};

			class ReplayToggleSelectionCommandClass : public CommandClass
			{
			public:
				virtual const char* GetName() const override { return "ReplayToggleSelection"; }

				virtual const wchar_t* GetUIName() const override
					{ return StringTable::TryFetchString("TXT_REPLAY_SELECTION", L"Replay: Follow/Free Unit Selection"); }

				virtual const wchar_t* GetUICategory() const override { return ReplayCategory(); }

				virtual const wchar_t* GetUIDescription() const override
				{
					return StringTable::TryFetchString("TXT_REPLAY_SELECTION_DESC",
						L"Toggles reproducing the unit selection recorded in the replay.");
				}

				virtual void Execute(WWKey) const override { ToggleRecordedSelection(); }
			};

			template <typename T>
			T* MakeCommand()
			{
				T* pCommand = GameCreate<T>();
				CommandClass::Array.AddItem(pCommand);
				return pCommand;
			}
		}

		bool IsPlaybackPaused()
		{
			return ReplayState.Playback && ReplayState.PlaybackPaused;
		}

		void TogglePlaybackPause()
		{
			if (!ReplayState.Playback)
				return;

			SingleStepTargetFrame = -1;
			if (ReplayState.PlaybackPaused)
			{
				ResumePending = true;
				ReplayState.PlaybackNextFrameDue = 0.0;
				PrintControlMessage(L"Replay resumed.");
			}
			else
			{
				ResumePending = false;
				SetPausedState(true);
				PrintControlMessage(L"Replay paused.");
			}
		}

		void PrintControlMessage(const wchar_t* pMessage)
		{
			PrintControlMessageInternal(pMessage);
		}

		void SetPlaybackPaused(bool paused)
		{
			if (!ReplayState.Playback)
				return;

			SingleStepTargetFrame = -1;
			if (paused)
			{
				ResumePending = false;
				SetPausedState(true);
			}
			else if (ReplayState.PlaybackPaused)
			{
				ResumePending = true;
				ReplayState.PlaybackNextFrameDue = 0.0;
			}
		}

		void RequestSingleStep()
		{
			if (!ReplayState.Playback || Seek::IsSeeking())
				return;

			// Stepping out of a running replay freezes it on the next frame rather than the one
			// after, which is what a viewer reaching for the button is asking for.
			SingleStepTargetFrame = static_cast<int>(Unsorted::CurrentFrame) + 1;
			ResumePending = false;
			SetPausedState(true);
		}

		void ServiceFrameStart()
		{
			if (!ReplayState.Playback)
			{
				SingleStepTargetFrame = -1;
				ResumePending = false;
				return;
			}

			if (ResumePending)
			{
				ResumePending = false;
				SetPausedState(false);
			}

			if (SingleStepTargetFrame < 0)
				return;

			if (static_cast<int>(Unsorted::CurrentFrame) >= SingleStepTargetFrame)
			{
				SingleStepTargetFrame = -1;
				SetPausedState(true);
				return;
			}

			SetPausedState(false);
		}

		int PlaybackFPS()
		{
			return ReplayState.PlaybackFPS > 0 ? ReplayState.PlaybackFPS : GetRecordedFPS();
		}

		double PlaybackSpeedMultiplier()
		{
			const int recordedFPS = GetRecordedFPS();
			if (recordedFPS <= 0)
				return 1.0;

			return static_cast<double>(PlaybackFPS()) / recordedFPS;
		}

		bool IsControlBarVisible()
		{
			return ReplayState.Playback && ControlBarVisible;
		}

		void ToggleControlBar()
		{
			if (!ReplayState.Playback)
				return;

			ControlBarVisible = !ControlBarVisible;
		}

		void InitControlBarVisibility()
		{
			const auto* const pConfig = Spawner::GetConfig();
			ControlBarVisible = pConfig && pConfig->ReplayControlBar;
		}

		void ToggleLockedViewport()
		{
			if (!ReplayState.Playback)
				return;

			ReplayState.LockViewport = !ReplayState.LockViewport;
			if (ReplayState.LockViewport)
				ApplyLockedViewport();

			PrintControlMessage(ReplayState.LockViewport
				? L"Replay viewport locked."
				: L"Replay viewport unlocked.");
		}

		void ToggleRecordedSelection()
		{
			if (!ReplayState.Playback)
				return;

			ReplayState.SelectUnits = !ReplayState.SelectUnits;
			if (ReplayState.SelectUnits)
				ApplyCurrentPlaybackSelection();

			PrintControlMessage(ReplayState.SelectUnits
				? L"Following recorded unit selection."
				: L"Recorded unit selection disabled.");
		}

		void StepPlaybackSpeed(int direction)
		{
			if (!ReplayState.Playback || direction == 0)
				return;

			const int currentIndex = FindNearestLadderIndex(ReplayState.PlaybackFPS);
			const int nextIndex = std::clamp(currentIndex + (direction > 0 ? 1 : -1), 0, SpeedLadderCount - 1);

			// Reported either way: the ladder may have run out, or an off-ladder speed may have
			// snapped onto the rung it was nearest to without moving.
			ReplayState.PlaybackFPS = SpeedLadder[nextIndex];
			ReplayState.PlaybackNextFrameDue = 0.0;
			ReportPlaybackSpeed();
		}

		void RenderPausedFrame()
		{
			if (!TacticalClass::Instance)
				return;

			TacticalClass::Instance->AI();
			MapClass::Instance.Render();
		}

		int GetPlaybackGameSpeedIndex()
		{
			for (int gameSpeedIndex = 0; gameSpeedIndex <= MaxGameSpeedIndex; ++gameSpeedIndex)
			{
				if (GetReplayFPSFromGameSpeed(gameSpeedIndex) <= ReplayState.PlaybackFPS)
					return gameSpeedIndex;
			}

			return MaxGameSpeedIndex;
		}

		void SetPlaybackGameSpeedIndex(int gameSpeedIndex)
		{
			ReplayState.PlaybackFPS = GetReplayFPSFromGameSpeed(std::clamp(gameSpeedIndex, 0, MaxGameSpeedIndex));
			ReplayState.PlaybackNextFrameDue = 0.0;
		}

		void RegisterReplayCommands()
		{
			MakeCommand<ReplayTogglePauseCommandClass>();
			MakeCommand<ReplaySpeedUpCommandClass>();
			MakeCommand<ReplaySpeedDownCommandClass>();
			MakeCommand<ReplayStepFrameCommandClass>();
			MakeCommand<ReplaySeekBackCommandClass>();
			MakeCommand<ReplaySeekForwardCommandClass>();
			MakeCommand<ReplayToggleControlBarCommandClass>();
			MakeCommand<ReplayToggleViewportLockCommandClass>();
			MakeCommand<ReplayToggleSelectionCommandClass>();
		}
	}
}
