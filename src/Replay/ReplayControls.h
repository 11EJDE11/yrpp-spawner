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

// Viewer-side controls for replay playback: pause, and a playback speed allowed to run past the
// 60 FPS ceiling the in-game speed slider tops out at. Both are plain CommandClass commands, bound
// from the in-game keyboard options like any other. Neither is part of the replay file format.

#include <iterator>

namespace ReplaySystem
{
	namespace Controls
	{
		// Playback speed ladder, in frames per second. The first seven rungs are the rates the
		// game-speed slider produces, so the options dialog and these hotkeys agree where they
		// overlap; the rest exist only for playback, which is not pacing itself against other
		// players. The engine paces a frame in whole milliseconds, so above 240 FPS only rungs
		// that land on a different millisecond mean anything, and 2000 divides to no wait at all.
		constexpr int SpeedLadder[] = { 10, 12, 15, 20, 30, 45, 60, 90, 120, 180, 240, 300, 500, 1000, 2000 };
		constexpr int SpeedLadderCount = static_cast<int>(std::size(SpeedLadder));

		// True while playback is frozen. The main loop still renders, scrolls and takes input;
		// only the simulation is held.
		bool IsPlaybackPaused();

		// Toggles the freeze. Does nothing outside playback.
		void TogglePlaybackPause();

		// Freezes or resumes playback without the on-screen notice the hotkey prints. Clears any
		// half-finished single step.
		void SetPlaybackPaused(bool paused);

		// Runs exactly one more frame and freezes again. Pauses first if playback is running.
		void RequestSingleStep();

		// Top of the frame, before the frame state is restored: commits deferred resumes and releases
		// playback until an explicit single-step target arrives, then takes the pause back. The target
		// is cancelled by a manual pause/resume action.
		void ServiceFrameStart();

		// Playback speed as a multiple of the speed the game was recorded at.
		double PlaybackSpeedMultiplier();

		// The frame rate playback is running at.
		int PlaybackFPS();

		// Whether the on-screen playback controls are being drawn, and its toggle. Starts at the
		// ReplayControlBar setting.
		bool IsControlBarVisible();
		void ToggleControlBar();
		void InitControlBarVisibility();

		// Prints a short notice in the message list, in the style the playback hotkeys use.
		void PrintControlMessage(const wchar_t* pMessage);

		// Walks the ladder by one rung: +1 faster, -1 slower. Does nothing outside playback.
		void StepPlaybackSpeed(int direction);

		// Commits and draws whatever the viewer scrolled to during a paused frame, in place of the
		// per-frame work that would normally do it.
		void RenderPausedFrame();

		// The game-speed index (0 fastest .. 6 slowest) the options dialog should show and compare
		// against for the current playback speed. Speeds past 60 FPS have no slider position of
		// their own and report as 0, the fastest one the slider has.
		int GetPlaybackGameSpeedIndex();

		// Applies a game-speed index chosen from the options dialog or replayed from a GameSpeed
		// event.
		void SetPlaybackGameSpeedIndex(int gameSpeedIndex);

		// Adds the replay commands to CommandClass::Array so the keyboard options dialog lists
		// them.
		void RegisterReplayCommands();
	}
}
