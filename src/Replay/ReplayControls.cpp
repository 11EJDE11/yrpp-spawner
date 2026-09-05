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
#include <MapClass.h>
#include <MessageListClass.h>
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
	}
}
