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
#include "ReplaySystem.h"
#include "ReplaySystem.Internal.h"

#include <ColorScheme.h>
#include <CommandClass.h>
#include <MapClass.h>
#include <MessageListClass.h>
#include <StringTable.h>
#include <TacticalClass.h>

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

			void PrintControlMessage(const wchar_t* pMessage)
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

			// The rung the current speed sits on. A speed that is not exactly on the ladder - a
			// hand written ReplayPlaybackSpeed, or a GameSpeed event out of the recording - snaps
			// to the nearest rung so the next step still moves somewhere sensible.
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

			ReplayState.PlaybackPaused = !ReplayState.PlaybackPaused;
			PrintControlMessage(ReplayState.PlaybackPaused ? L"Replay paused." : L"Replay resumed.");
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
			// The slider counts the other way round from the ladder and stops at 60 FPS. Anything
			// faster reports as its fastest position, so opening the dialog and leaving it alone
			// cannot slow playback back down.
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
		}

		void RegisterReplayCommands()
		{
			MakeCommand<ReplayTogglePauseCommandClass>();
			MakeCommand<ReplaySpeedUpCommandClass>();
			MakeCommand<ReplaySpeedDownCommandClass>();
		}
	}
}
