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
#include "ReplayFormat.h"
#include "ReplaySystem.Internal.h"
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

			struct PlaybackControlState
			{
				int FrameRate = 0;
				// Performance-counter deadline; reset after pause, speed changes, and seeking.
				double NextFrameDue = 0.0;
				bool Paused = false;
				// Hold the pause state steady until the next frame-start hook.
				int SingleStepTargetFrame = -1;
				// Input runs after frame preparation, so resuming must wait for frame start.
				bool ResumePending = false;
				bool ControlBarVisible = false;
			};

			PlaybackControlState State;
			// Use a high-resolution clock for playback rates above the engine timer resolution.
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

			void SetPausedState(bool paused)
			{
				State.Paused = paused;
				// Never carry a pre-pause or single-step deadline into resumed playback. Starting the
				// pacing clock afresh prevents a burst of unpaced frames on resume.
				State.NextFrameDue = 0.0;
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
				const int fps = State.FrameRate;
				const int recordedFPS = GetRecordedFPS();
				const double multiplier = recordedFPS > 0 ? static_cast<double>(fps) / recordedFPS : 1.0;

				wchar_t message[64];
				swprintf_s(message, L"Replay speed: %.2fx (%d FPS)", multiplier, fps);
				PrintControlMessage(message);
			}

		}

		void OnPlaybackStarted(int playbackFPS, bool controlBarVisible)
		{
			State = PlaybackControlState {};
			State.FrameRate = playbackFPS;
			State.ControlBarVisible = controlBarVisible;
		}

		void OnPlaybackStopped()
		{
			// A new session must not inherit a pending resume or half-finished single step.
			State = PlaybackControlState {};
		}

		void ResetFramePacing()
		{
			State.NextFrameDue = 0.0;
		}

		bool HasPlaybackSpeed()
		{
			return State.FrameRate > 0;
		}

		void ApplyFramePacing()
		{
			if (!ReplayState.Playback)
				return;

			if (State.FrameRate <= 0)
				return;

			const double frameMilliseconds = 1000.0 / State.FrameRate;
			const double now = PlaybackClockMilliseconds();

			if (now > 0.0)
			{
				if (State.NextFrameDue == 0.0
					|| now > State.NextFrameDue + frameMilliseconds)
				{
					State.NextFrameDue = now;
				}

				for (double remaining = State.NextFrameDue - PlaybackClockMilliseconds();
					remaining > 0.0;
					remaining = State.NextFrameDue - PlaybackClockMilliseconds())
				{
					// Sleep(1) can overshoot by a whole scheduler tick, so it covers the bulk and the
					// last couple of milliseconds are given up with Sleep(0).
					Sleep(remaining > 2.0 ? 1 : 0);
				}

				State.NextFrameDue += frameMilliseconds;
			}

			Unsorted::GameFrameTimer.TimeLeft = 0;
			Unsorted::NetworkFrameTimer.TimeLeft = 0;
		}

		bool IsPlaybackPaused()
		{
			return ReplayState.Playback && State.Paused;
		}

		void TogglePlaybackPause()
		{
			if (!ReplayState.Playback)
				return;

			State.SingleStepTargetFrame = -1;
			if (State.Paused)
			{
				State.ResumePending = true;
				State.NextFrameDue = 0.0;
				PrintControlMessage(L"Replay resumed.");
			}
			else
			{
				State.ResumePending = false;
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

			State.SingleStepTargetFrame = -1;
			if (paused)
			{
				State.ResumePending = false;
				SetPausedState(true);
			}
			else if (State.Paused)
			{
				State.ResumePending = true;
				State.NextFrameDue = 0.0;
			}
		}

		void RequestSingleStep()
		{
			if (!ReplayState.Playback)
				return;

			// Stepping out of a running replay freezes it on the next frame rather than the one
			// after, which is what a viewer reaching for the button is asking for.
			State.SingleStepTargetFrame = static_cast<int>(Unsorted::CurrentFrame) + 1;
			State.ResumePending = false;
			SetPausedState(true);
		}

		void ServiceFrameStart()
		{
			if (!ReplayState.Playback)
			{
				State.SingleStepTargetFrame = -1;
				State.ResumePending = false;
				return;
			}

			if (State.ResumePending)
			{
				State.ResumePending = false;
				SetPausedState(false);
			}

			if (State.SingleStepTargetFrame < 0)
				return;

			if (static_cast<int>(Unsorted::CurrentFrame) >= State.SingleStepTargetFrame)
			{
				State.SingleStepTargetFrame = -1;
				SetPausedState(true);
				return;
			}

			SetPausedState(false);
		}

		int PlaybackFPS()
		{
			return State.FrameRate > 0 ? State.FrameRate : GetRecordedFPS();
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
			return ReplayState.Playback && State.ControlBarVisible;
		}

		void ToggleControlBar()
		{
			if (!ReplayState.Playback)
				return;

			State.ControlBarVisible = !State.ControlBarVisible;
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

			const int currentIndex = FindNearestLadderIndex(State.FrameRate);
			const int nextIndex = std::clamp(currentIndex + (direction > 0 ? 1 : -1), 0, SpeedLadderCount - 1);

			State.FrameRate = SpeedLadder[nextIndex];
			State.NextFrameDue = 0.0;
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
				if (GetReplayFPSFromGameSpeed(gameSpeedIndex) <= State.FrameRate)
					return gameSpeedIndex;
			}

			return MaxGameSpeedIndex;
		}

		void SetPlaybackGameSpeedIndex(int gameSpeedIndex)
		{
			State.FrameRate = GetReplayFPSFromGameSpeed(std::clamp(gameSpeedIndex, 0, MaxGameSpeedIndex));
			State.NextFrameDue = 0.0;
		}
	}
}
