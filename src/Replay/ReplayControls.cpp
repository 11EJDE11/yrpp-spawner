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

constexpr int CONTROL_MESSAGE_DURATION_FRAMES = 90;

bool CommandsRegistered = false;

void PrintControlMessage(const wchar_t* pMessage)
{
	MessageListClass::Instance.PrintMessage(
		pMessage,
		CONTROL_MESSAGE_DURATION_FRAMES,
		ColorScheme::White,
		/* bSilent: */ true);
}

int GetRecordedFPS()
{
	if (!ReplayState.HasPlaybackHeader)
		return GetReplayFPSFromGameSpeed(0);

	return GetReplayFPSFromGameSpeed(static_cast<int>(ReplayState.PlaybackHeader.RecordedGameSpeed));
}

// The rung the current speed sits on. A speed that is not exactly on the ladder - a hand written
// ReplayPlaybackSpeed, or a GameSpeed event replayed out of the recording - snaps to the nearest
// rung so that the next step still moves somewhere sensible.
int FindNearestLadderIndex(int fps)
{
	int bestIndex = 0;
	int bestDistance = std::abs(SPEED_LADDER[0] - fps);

	for (int i = 1; i < SPEED_LADDER_COUNT; ++i)
	{
		const int distance = std::abs(SPEED_LADDER[i] - fps);
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

class ReplayTogglePauseCommandClass : public CommandClass
{
public:
	const char* GetName() const override { return "ReplayTogglePause"; }
	const wchar_t* GetUIName() const override { return L"Replay: Pause/Resume"; }
	const wchar_t* GetUICategory() const override { return L"Replay"; }
	const wchar_t* GetUIDescription() const override
		{ return L"Freezes replay playback. The map can still be scrolled while paused."; }

	void Execute(WWKey) const override { TogglePlaybackPause(); }
};

class ReplaySpeedUpCommandClass : public CommandClass
{
public:
	const char* GetName() const override { return "ReplaySpeedUp"; }
	const wchar_t* GetUIName() const override { return L"Replay: Speed Up"; }
	const wchar_t* GetUICategory() const override { return L"Replay"; }
	const wchar_t* GetUIDescription() const override
		{ return L"Plays the replay back faster, past the speed a live game allows."; }

	void Execute(WWKey) const override { StepPlaybackSpeed(1); }
};

class ReplaySpeedDownCommandClass : public CommandClass
{
public:
	const char* GetName() const override { return "ReplaySpeedDown"; }
	const wchar_t* GetUIName() const override { return L"Replay: Slow Down"; }
	const wchar_t* GetUICategory() const override { return L"Replay"; }
	const wchar_t* GetUIDescription() const override
		{ return L"Plays the replay back slower."; }

	void Execute(WWKey) const override { StepPlaybackSpeed(-1); }
};

template <typename T>
void MakeCommand()
{
	CommandClass::Array.AddItem(GameCreate<T>());
}

} // namespace

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
	const int nextIndex = std::clamp(currentIndex + (direction > 0 ? 1 : -1), 0, SPEED_LADDER_COUNT - 1);
	const int nextFPS = SPEED_LADDER[nextIndex];

	// Either the ladder ran out, or the speed was off-ladder and snapped onto the rung it was
	// nearest to. Either way there is nothing to apply, but still say where playback ended up.
	if (nextFPS != ReplayState.PlaybackFPS)
	{
		ReplayState.PlaybackFPS = nextFPS;
	}

	ReportPlaybackSpeed();
}

void RenderPausedFrame()
{
	if (!TacticalClass::Instance)
		return;

	const auto TacticalAI = reinterpret_cast<void(__thiscall*)(TacticalClass*)>(TACTICAL_AI_ADDRESS);
	TacticalAI(TacticalClass::Instance);

	MapClass::Instance.Render();
}

int GetPlaybackGameSpeedIndex()
{
	// The slider counts the other way round from the ladder and stops at 60 FPS. Anything faster
	// reports as its fastest position, so that opening the dialog and leaving it alone cannot slow
	// playback back down.
	for (int gameSpeedIndex = 0; gameSpeedIndex <= MAX_GAME_SPEED_INDEX; ++gameSpeedIndex)
	{
		if (GetReplayFPSFromGameSpeed(gameSpeedIndex) <= ReplayState.PlaybackFPS)
			return gameSpeedIndex;
	}

	return MAX_GAME_SPEED_INDEX;
}

void SetPlaybackGameSpeedIndex(int gameSpeedIndex)
{
	ReplayState.PlaybackFPS = GetReplayFPSFromGameSpeed(std::clamp(gameSpeedIndex, 0, MAX_GAME_SPEED_INDEX));
}

void RegisterReplayCommands()
{
	// Init_Commands runs once per process, but the hook this is called from sits at an address the
	// engine reaches once per command it creates - guard rather than rely on that staying true.
	if (CommandsRegistered)
		return;

	CommandsRegistered = true;
	MakeCommand<ReplayTogglePauseCommandClass>();
	MakeCommand<ReplaySpeedUpCommandClass>();
	MakeCommand<ReplaySpeedDownCommandClass>();
}

} // namespace Controls

} // namespace ReplaySystem
