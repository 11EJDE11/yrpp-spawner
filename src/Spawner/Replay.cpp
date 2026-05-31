/**
*  yrpp-spawner
*
*  Copyright(C) 2022-present CnCNet
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

#include "Replay.h"
#include "Spawner.h"

#include <CCFileClass.h>
#include <EventClass.h>
#include <GameModeOptionsClass.h>
#include <GameOptionsClass.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <SessionClass.h>
#include <Unsorted.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>
#include <Utilities/Patch.h>

#include <Windows.h>
#include <algorithm>
#include <cstdio>

// The game's native function that writes BuildLevel/Seed/ScenarioName/Whom/Special/Options
// to the open RecordFile. Same format as Save_Recording_Values (0x5318C0).
// RecordFile header layout (464 bytes total):
//   offset 0:   BuildLevel  (4)
//   offset 4:   Seed        (4)
//   offset 8:   Scen.Scenario (4)
//   offset 12:  ScenarioName (260)
//   offset 272: Whom        (4)
//   offset 276: Special     (4)
//   offset 280: Options     (184)
//   offset 464: event stream begins
using RecordingValuesFunc = char(__thiscall*)(void*);
static auto Save_Recording_Values = reinterpret_cast<RecordingValuesFunc>(0x5318C0);


namespace
{
bool PlaybackActive = false;
// Playback frame hooks run continuously; these latches keep one-shot fixes and diagnostics from repeating.
bool PlaybackSpectatorEnabled = false;
bool PlaybackQueueAILogged = false;
bool PlaybackFrameOptionsLogged = false;
bool PlaybackOutListLogged = false;

bool IsNativePlaybackReading()
{
	return (static_cast<unsigned int>(Game::RecordingFlag) & static_cast<unsigned int>(RecordFlag::Read)) != 0u;
}

int GetReplayFPSFromGameSpeed(int gameSpeed)
{
	// Native playback bypasses RequestedFPS, which made the client's speed option ineffective; map it to the FPS value Main_Loop will use.
	gameSpeed = std::clamp(gameSpeed, 0, 6);

	if (gameSpeed <= 0)
		return 60;

	if (gameSpeed == 1)
		return 45;

	return std::max(1, 60 / gameSpeed);
}

const SpawnerConfig* GetReplayConfig()
{
	const auto pConfig = Spawner::GetConfig();
	return pConfig && pConfig->IsReplayPlayback
		? pConfig
		: nullptr;
}

void EnableSpectatorView()
{
	const auto pConfig = GetReplayConfig();
	if (!Replay::IsPlaybackActive() || !pConfig || !pConfig->ReplaySpectator || PlaybackSpectatorEnabled)
		return;

	// Playback loads us as a normal player. Making that player the Observer flips
	// HouseClass::IsCurrentPlayerObserver(), which the Observers.Visibility hooks key off to
	// reveal cloaked/disguised units, radar blips and pips. MakeObserver only sets
	// HouseClass::Observer = CurrentPlayer; it does NOT change CurrentPlayer, so the private
	// anti-cheat's PlayerPtr-mirror check is not tripped (only the frame-100 observer check,
	// which is gated on Session.Play). Reveal the shroud too so the whole map is visible.
	Debug::Log("[Spawner] Replay: enabling spectator view at frame %d\n", Unsorted::CurrentFrame);

	if (HouseClass::CurrentPlayer)
	{
		Game::ObserverMode = true;
		HouseClass::CurrentPlayer->MakeObserver();
	}

	for (const auto pHouse : HouseClass::Array)
	{
		if (pHouse)
			MapClass::Instance.Reveal(pHouse);
	}

	MapClass::Instance.MarkNeedsRedraw(0);
	PlaybackSpectatorEnabled = true;
}

void ApplyPlaybackTiming()
{
	const auto pConfig = GetReplayConfig();
	if (!PlaybackActive || !pConfig)
		return;

	// The replay header restores recorded game options over spawn.ini, so reapply the client's playback speed after that restore.
	const int playbackSpeed = std::clamp(pConfig->GameSpeed, 0, 6);
	GameOptionsClass::Instance.GameSpeed = playbackSpeed;
	GameModeOptionsClass::Instance.GameSpeed = playbackSpeed;
	Game::Network::RequestedFPS = GetReplayFPSFromGameSpeed(playbackSpeed);
}

void ClearLocalPlaybackEvents()
{
	if (!PlaybackActive)
		return;

	const int outListCount = EventClass::OutList.Count;
	if (outListCount <= 0)
		return;

	// Local input is needed for an unlocked viewport, but gameplay events from that input would corrupt playback; discard them before Queue_AI.
	if (!PlaybackOutListLogged)
	{
		Debug::Log("[Spawner] Replay: clearing local playback OutList events (first count=%d)\n", outListCount);
		PlaybackOutListLogged = true;
	}

	EventClass::OutList.Init();
}

void ApplyLocalPlaybackControls()
{
	const auto pConfig = GetReplayConfig();
	if (!Replay::IsPlaybackActive() || !pConfig || pConfig->ReplayLockedViewport)
		return;

	// Native playback treats Escape as replay control, so handle it locally once input is re-enabled for viewport movement.
	if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0)
		Game::SpecialDialog = 1;
}
}

bool Replay::IsPlaybackActive()
{
	return PlaybackActive && IsNativePlaybackReading();
}

void Replay::PrepareRecording()
{
	// Recording uses the native menu path correctly; clear playback-only state so prior playback runs cannot leak into recording.
	PlaybackActive = false;
	PlaybackSpectatorEnabled = false;
	PlaybackQueueAILogged = false;
	PlaybackFrameOptionsLogged = false;
	PlaybackOutListLogged = false;
	SessionClass::Instance.Record = 1;
	Debug::Log("[Spawner] Replay: recording prepared\n");
}

void Replay::StartRecording()
{
	// This path writes a recording, not a playback session; clear playback latches before using the game's native recorder.
	PlaybackActive = false;
	PlaybackSpectatorEnabled = false;
	PlaybackQueueAILogged = false;
	PlaybackFrameOptionsLogged = false;
	PlaybackOutListLogged = false;
	auto& recordFile = SessionClass::Instance.RecordFile;

	if (!recordFile.Open(FileAccessMode::Write))
	{
		Debug::Log("[Spawner] Replay: failed to open RECORD.BIN for writing\n");
		SessionClass::Instance.Record = 0;
		return;
	}

	// Write the standard recording header (BuildLevel, Seed, ScenarioName, Whom, Special, Options)
	Save_Recording_Values(&recordFile);

	// Create a flag file so the client knows a recording was started and RECORD.BIN is valid.
	// The client deletes this file after packaging the replay.
	HANDLE hFlag = CreateFileA(
		"spawn_recording_active.flag",
		GENERIC_WRITE, 0, NULL,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFlag != INVALID_HANDLE_VALUE)
		CloseHandle(hFlag);

	Debug::Log("[Spawner] Replay: recording started (RECORD.BIN open)\n");
}

void Replay::SetupPlayback()
{
	auto pConfig = Spawner::GetConfig();

	char eventsPath[MAX_PATH];
	size_t dirLen = strlen(pConfig->ReplayDataDir);
	bool hasTrailingSlash = dirLen > 0 && (pConfig->ReplayDataDir[dirLen - 1] == '\\' || pConfig->ReplayDataDir[dirLen - 1] == '/');
	snprintf(eventsPath, sizeof(eventsPath), hasTrailingSlash ? "%sevents.dat" : "%s\\events.dat", pConfig->ReplayDataDir);

	auto& recordFile = SessionClass::Instance.RecordFile;
	recordFile.SetFileName(eventsPath);

	if (!recordFile.Open(FileAccessMode::Read))
	{
		Debug::Log("[Spawner] Replay: failed to open events.dat for playback: %s\n", eventsPath);
		// Without events.dat there is no native playback stream to drive Queue_AI, so disable playback-only frame work.
		PlaybackActive = false;
		PlaybackSpectatorEnabled = false;
			PlaybackQueueAILogged = false;
		PlaybackFrameOptionsLogged = false;
		PlaybackOutListLogged = false;
		return;
	}

	// Read BuildLevel (discard) and Seed from the header, then seek to the event stream.
	int buildLevel = 0;
	int recordedSeed = 0;
	recordFile.ReadBytes(&buildLevel, sizeof(buildLevel));
	recordFile.ReadBytes(&recordedSeed, sizeof(recordedSeed));

	// Override the seed set from spawn.ini so Init_Random restores the recorded RNG state.
	Game::Seed = recordedSeed;

	// Seek past the rest of the header (464 bytes total; already read 8).
	recordFile.Seek(456, FileSeekMode::Current);

	// Tell Queue_AI to read events from RecordFile instead of accepting player input.
	SessionClass::Instance.Play = 1;
	PlaybackActive = true;
	PlaybackSpectatorEnabled = false;
	PlaybackQueueAILogged = false;
	PlaybackFrameOptionsLogged = false;
	PlaybackOutListLogged = false;

	// TrapPrintCRC defaults to 0, which makes the playback path's check
	// "if (frame >= TrapPrintCRC)" fire immediately at frame 0, dumping CRCs
	// and calling Emergency_Exit(0). Disable it by setting it to INT_MAX.
	auto& TrapPrintCRC = *reinterpret_cast<int*>(0xa8e30cu);
	TrapPrintCRC = 0x7FFFFFFF;

	Debug::Log("[Spawner] Replay: playback set up from %s (Seed=%08x)\n", eventsPath, recordedSeed);
}

void Replay::ApplyPlaybackOptions()
{
	const auto pConfig = GetReplayConfig();
	if (!PlaybackActive || !pConfig)
		return;

	// Private SelfCRC checks .text after startup; apply replay code patches before its baseline so playback options do not trip the guard.
	ApplyPlaybackTiming();

	// Native playback bypasses DesiredFrameRate/RequestedFPS; force it through normal pacing.
	Debug::Log("[Spawner] Replay: patching native playback timing to use RequestedFPS\n");
	Patch::Apply_RAW(0x55D46A, { 0xEB });

	if (!pConfig->ReplayLockedViewport)
	{
		// Playback skips GScreen/keyboard input, which prevents panning; allow that input only when the client unlocks the viewport.
		Debug::Log("[Spawner] Replay: allowing local input for unlocked viewport\n");
		Patch::Apply_RAW(0x55D881, { 0x90, 0x90 });

		// The viewport record must still be read, but applying Set_View_Point would override the user's unlocked viewport position.
		Debug::Log("[Spawner] Replay: patching out native recorded viewport restore\n");
		Patch::Apply_RAW(0x55DA7E, { 0x83, 0xC4, 0x04, 0x90, 0x90 });

		// Tactical::AI early-returns during playback, so Scroll_Map updates never reach the displayed tactical coord; let AI commit them.
		Debug::Log("[Spawner] Replay: patching out native tactical view freeze for unlocked viewport\n");
		Patch::Apply_RAW(0x6D2550, { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 });
	}

	if (!pConfig->ReplaySelectUnits)
	{
		// Selection records must still be read to keep the stream aligned, but the client can choose not to mirror recorded selections.
		Debug::Log("[Spawner] Replay: patching out native recorded selection restore\n");
		Patch::Apply_RAW(0x55DB0E, { 0x90, 0x90, 0x90, 0x90, 0x90 });
		Patch::Apply_RAW(0x55DB6F, { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 });
	}

	// SidebarClass::Activate is a no-op during network-game playback (guard jz at 0x6A7D82), which
	// blocks Fill_In_Data's initial Activate(1) at scenario start so the sidebar is never set up and
	// shows as a black area. Make the jump unconditional so the sidebar activates and draws normally
	// during playback (player sidebar in player mode, spectator stats sidebar in spectator mode).
	Debug::Log("[Spawner] Replay: enabling sidebar activation during playback\n");
	Patch::Apply_RAW(0x6A7D82, { 0xEB });

	Debug::Log("[Spawner] Replay: playback options applied (Speed=%d, FPS=%d, Spectator=%d, LockViewport=%d, SelectUnits=%d)\n",
		std::clamp(pConfig->GameSpeed, 0, 6),
		Game::Network::RequestedFPS,
		pConfig->ReplaySpectator,
		pConfig->ReplayLockedViewport,
		pConfig->ReplaySelectUnits);
}

void Replay::ApplyPlaybackFrameOptions()
{
	if (!Replay::IsPlaybackActive())
		return;

	// Playback keeps restoring or bypassing local options, so enforce the client's playback-only choices each frame.
	ApplyPlaybackTiming();
	ApplyLocalPlaybackControls();
	ClearLocalPlaybackEvents();

	if (!PlaybackFrameOptionsLogged)
	{
		const auto pConfig = GetReplayConfig();
		Debug::Log("[Spawner] Replay: per-frame playback options reached at frame %d (Speed=%d, FPS=%d, Spectator=%d)\n",
			Unsorted::CurrentFrame,
			pConfig ? std::clamp(pConfig->GameSpeed, 0, 6) : -1,
			Game::Network::RequestedFPS,
			pConfig ? pConfig->ReplaySpectator : -1);
		PlaybackFrameOptionsLogged = true;
	}

	EnableSpectatorView();
}

// Native playback treats Escape as ending playback; use it as the local options hotkey instead.
DEFINE_HOOK(0x647299, QueueAI_Playback_EscapeShowsOptions, 0x6)
{
	if (Replay::IsPlaybackActive() && !PlaybackQueueAILogged)
	{
		// This confirms Queue_AI reached the native playback reader that consumes the recorded event stream.
		Debug::Log("[Spawner] Replay: Queue_AI playback path reached at frame %d\n", Unsorted::CurrentFrame);
		PlaybackQueueAILogged = true;
	}

	if (Replay::IsPlaybackActive() && (R->EAX() & 0xFFFF) == VK_ESCAPE)
	{
		Game::SpecialDialog = 1;
		return 0x6472FC;
	}

	return 0;
}

// Native playback ignores queued exit events, so close playback immediately from the menu.
DEFINE_HOOK(0x6471A0, QueueExit_ReplayPlayback_ExitLocally, 0x8)
{
	if (Replay::IsPlaybackActive())
	{
		Game::IsActive = false;
		R->EAX(1);
		return 0x647253;
	}

	return 0;
}

// Keep modal dialogs visible by preventing playback's main loop render from running under them.
DEFINE_HOOK(0x623145, OwnerDrawLoop_ReplayPlayback_NoMainLoop, 0x5)
{
	return Replay::IsPlaybackActive()
		? 0x62315A
		: 0;
}
