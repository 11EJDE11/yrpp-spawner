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

#include "ReplayCommands.h"
#include "Commands.h"

#include <Replay/ReplayControls.h>
#include <Replay/ReplaySeek.h>
#include <Replay/ReplaySystem.h>

#include <StringTable.h>

namespace Commands
{
	namespace
	{
		// How far the jump hotkeys move, in seconds of recorded game time. The seek bar is for
		// anything longer than a nudge.
		constexpr int HotkeySeekSeconds = 10;

		void SeekBySeconds(int seconds)
		{
			if (!ReplaySystem::IsPlaybackActive())
				return;

			const int target = ReplaySystem::Seek::CurrentFrame() + seconds * ReplaySystem::Seek::RecordedFPS();
			if (!ReplaySystem::Seek::RequestSeek(target))
				ReplaySystem::Controls::PrintControlMessage(L"Nothing to rewind to yet.");
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

			virtual void Execute(WWKey) const override { ReplaySystem::Controls::TogglePlaybackPause(); }
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

			virtual void Execute(WWKey) const override { ReplaySystem::Controls::StepPlaybackSpeed(1); }
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

			virtual void Execute(WWKey) const override { ReplaySystem::Controls::StepPlaybackSpeed(-1); }
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

			virtual void Execute(WWKey) const override { ReplaySystem::Controls::RequestSingleStep(); }
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

			virtual void Execute(WWKey) const override { ReplaySystem::Controls::ToggleControlBar(); }
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

			virtual void Execute(WWKey) const override { ReplaySystem::Controls::ToggleLockedViewport(); }
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

			virtual void Execute(WWKey) const override { ReplaySystem::Controls::ToggleRecordedSelection(); }
		};
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
