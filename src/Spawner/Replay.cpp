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

#include <Ext/Event/Body.h>

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

// Native replay header helpers. They write/read BuildLevel/Seed/ScenarioName/Whom/Special/Options
// to/from the open RecordFile.
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
static auto Load_Recording_Values = reinterpret_cast<RecordingValuesFunc>(0x531960);


namespace
{
bool PlaybackActive = false;
// Playback frame hooks run continuously; these latches keep one-shot fixes and diagnostics from repeating.
bool PlaybackSpectatorEnabled = false;
bool PlaybackQueueAILogged = false;
bool PlaybackFrameOptionsLogged = false;
bool PlaybackOutListLogged = false;

void ResetPlaybackState()
{
	PlaybackActive = false;
	PlaybackSpectatorEnabled = false;
	PlaybackQueueAILogged = false;
	PlaybackFrameOptionsLogged = false;
	PlaybackOutListLogged = false;
}

bool IsNativePlaybackReading()
{
	return (static_cast<unsigned int>(Game::RecordingFlag) & static_cast<unsigned int>(RecordFlag::Read)) != 0u;
}

// Network/timing bookkeeping events that only existed to synchronise live peers. Offline playback has
// no peers, so none of these should run; the recorded gameplay events already carry the frame they
// execute on. (ResponseTime2 is ProtocolZero's extension event, whose type byte sits past the native range.)
bool IsNonGameplayPlaybackEvent(EventType type)
{
	switch (type)
	{
	case EventType::Empty:
	case EventType::FrameSync:
	case EventType::ResponseTime:
	case EventType::ProcessTime:
	case EventType::MegaFrameInfo:
	case EventType::PacketTiming:
		return true;
	default:
		return static_cast<unsigned char>(type) == static_cast<unsigned char>(EventTypeExt::ResponseTime2);
	}
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
	// HouseClass::Observer = CurrentPlayer; it deliberately does NOT reassign CurrentPlayer,
	// so the rest of the game still treats us as the same house. Reveal the shroud too so the
	// whole map is visible.
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

void InitPlaybackSpeed()
{
	const auto pConfig = GetReplayConfig();
	if (!PlaybackActive || !pConfig)
		return;

	// Load_Recording_Values restored the recorded OptionsClass (including GameSpeed) over spawn.ini,
	// so set the speed the replay should start at here. From now on the live GameSpeed is authoritative,
	// so the viewer can change it through the in-game options menu (see ProcessLocalPlaybackEvents).
	GameOptionsClass::Instance.GameSpeed = std::clamp(pConfig->GameSpeed, 0, 6);
	Game::Network::RequestedFPS = GetReplayFPSFromGameSpeed(GameOptionsClass::Instance.GameSpeed);
}

void ApplyPlaybackTiming()
{
	if (!PlaybackActive)
		return;

	// Native playback runs uncapped and ignores RequestedFPS; the 0x55D46A patch instead routes playback
	// through the RequestedFPS pacing path. Track the live GameSpeed every frame so a speed the viewer
	// picks in the options menu takes effect immediately, instead of pinning it to one fixed value.
	Game::Network::RequestedFPS = GetReplayFPSFromGameSpeed(std::clamp(GameOptionsClass::Instance.GameSpeed, 0, 6));
}

void ProcessLocalPlaybackEvents()
{
	if (!PlaybackActive)
		return;

	const int outListCount = EventClass::OutList.Count;
	if (outListCount <= 0)
		return;

	// Queue_AI's playback path drives the simulation entirely from the recorded event stream and never
	// executes OutList, so local input queued during an unlocked-viewport session must be discarded or it
	// would pile up (and, if it ever reached the simulation, desync playback). The in-game options menu is
	// the one exception: during a network session it queues the viewer's speed change as a GameSpeed event
	// instead of applying it directly, so honor that event locally before discarding the rest.
	for (int i = 0; i < outListCount; ++i)
	{
		const auto& event = EventClass::OutList[i];
		if (event.Type == EventType::GameSpeed)
			GameOptionsClass::Instance.GameSpeed = std::clamp(event.GameSpeed.GameSpeed, 0, 6);
	}

	if (!PlaybackOutListLogged)
	{
		Debug::Log("[Spawner] Replay: discarding local playback OutList events (first count=%d)\n", outListCount);
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
	ResetPlaybackState();
	SessionClass::Instance.Record = 1;
	Debug::Log("[Spawner] Replay: recording prepared\n");
}

void Replay::StartRecording()
{
	if (const auto pConfig = Spawner::GetConfig(); pConfig && pConfig->IsReplayPlayback)
	{
		Debug::Log("[Spawner] Replay: recording skipped because playback is active\n");
		SessionClass::Instance.Record = 0;
		return;
	}

	// This path writes a recording, not a playback session; clear playback latches before using the game's native recorder.
	ResetPlaybackState();
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

bool Replay::SetupPlayback()
{
	auto pConfig = Spawner::GetConfig();
	ResetPlaybackState();

	if (!pConfig || !pConfig->ReplayDataDir[0])
	{
		Debug::Log("[Spawner] Replay: failed to set up playback because ReplayDataDir is empty\n");
		return false;
	}

	char eventsPath[MAX_PATH];
	size_t dirLen = strlen(pConfig->ReplayDataDir);
	bool hasTrailingSlash = dirLen > 0 && (pConfig->ReplayDataDir[dirLen - 1] == '\\' || pConfig->ReplayDataDir[dirLen - 1] == '/');
	const int written = snprintf(eventsPath, sizeof(eventsPath), hasTrailingSlash ? "%sevents.dat" : "%s\\events.dat", pConfig->ReplayDataDir);
	if (written < 0 || written >= static_cast<int>(sizeof(eventsPath)))
	{
		Debug::Log("[Spawner] Replay: failed to set up playback because ReplayDataDir is too long: %s\n", pConfig->ReplayDataDir);
		return false;
	}

	auto& recordFile = SessionClass::Instance.RecordFile;
	recordFile.SetFileName(eventsPath);

	if (!recordFile.Open(FileAccessMode::Read))
	{
		Debug::Log("[Spawner] Replay: failed to open events.dat for playback: %s\n", eventsPath);
		return false;
	}

	const int recordFileSize = recordFile.GetFileSize();
	if (recordFileSize < 464)
	{
		Debug::Log("[Spawner] Replay: failed to set up playback because events.dat is too small (%d bytes): %s\n",
			recordFileSize,
			eventsPath);
		recordFile.Close();
		return false;
	}

	// Restore the same header the native replay menu path restores: RNG seed, scenario metadata,
	// owning house/special flags, and recorded game options. This leaves RecordFile at the
	// event stream so Queue_AI can consume the recorded event batches.
	Load_Recording_Values(&recordFile);

	// Tell Queue_AI to read events from RecordFile instead of accepting player input.
	SessionClass::Instance.Play = 1;
	PlaybackActive = true;

	// TrapPrintCRC defaults to 0, which makes the playback path's check
	// "if (frame >= TrapPrintCRC)" fire immediately at frame 0, dumping CRCs
	// and calling Emergency_Exit(0). Disable it by setting it to INT_MAX.
	auto& TrapPrintCRC = *reinterpret_cast<int*>(0xa8e30cu);
	TrapPrintCRC = 0x7FFFFFFF;

	Debug::Log("[Spawner] Replay: playback set up from %s (Seed=%08x)\n", eventsPath, Game::Seed);
	return true;
}

void Replay::ApplyPlaybackOptions()
{
	const auto pConfig = GetReplayConfig();
	if (!PlaybackActive || !pConfig)
		return;

	// Pick the speed the replay starts at; from here the viewer can change it via the options menu.
	InitPlaybackSpeed();

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

	// Playback restores or bypasses some local options each frame. Handle queued local events first
	// (this may pick up a viewer speed change), then re-sync timing to the resulting live GameSpeed.
	ProcessLocalPlaybackEvents();
	ApplyPlaybackTiming();
	ApplyLocalPlaybackControls();

	if (!PlaybackFrameOptionsLogged)
	{
		const auto pConfig = GetReplayConfig();
		Debug::Log("[Spawner] Replay: per-frame playback options reached at frame %d (Speed=%d, FPS=%d, Spectator=%d)\n",
			Unsorted::CurrentFrame,
			std::clamp(GameOptionsClass::Instance.GameSpeed, 0, 6),
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

// Keep modal dialogs (the in-game options menu) visible during playback by taking OwnerDraw_Loop's
// Call_Back branch (0x62315A) instead of letting it run Main_Loop underneath, which would advance the
// replay behind the dialog. We must NOT hook the `call InMainLoop_55CBF0` at 0x623145: returning 0 there
// makes Syringe re-execute that stolen *relative* call from its trampoline without fixing up the
// displacement, so it calls a garbage address and crashes (e.g. when this fires for a recording, which
// keeps the native Main_Loop path). Hook the position-independent `mov eax, SystemResponseMessages`
// just before it instead, so the non-playback path can safely fall through to the native code.
DEFINE_HOOK(0x62313C, OwnerDrawLoop_ReplayPlayback_NoMainLoop, 0x5)
{
	return Replay::IsPlaybackActive()
		? 0x62315A
		: 0;
}

// Scenario_Load_Wait blocks a network game until every player reports it has finished loading.
// During playback there are no connected peers, so the recorded players never report in: the wait
// would stall for its full timeout and then "drop" them. The native code already skips this wait for
// campaign/skirmish; treat playback the same and report "everyone loaded" immediately.
// EAX holds Session.Type here (loaded just before the patched jz); 0 == campaign, the native early-out.
DEFINE_HOOK(0x684384, ScenarioLoadWait_SkipDuringPlayback, 0x6)
{
	if (R->EAX() == 0 || Replay::IsPlaybackActive())
		return 0x68460A; // success epilogue: mov al,1; restore frame; retn

	return 0x68438A; // not campaign/playback: fall through to the skirmish check
}

// Drop network/timing events from the DoList during playback, before Execute_DoList walks it.
// Replaying offline, these events have no peers to sync with and must not run: executed late they fall
// into the network "Packet received too late!" path (which calls through the null ConnManClass pointer
// and crashes), and ProtocolZero's ResponseTime2 isn't even a native event type so dispatching it jumps
// through the event jumptable into garbage. Marking them executed makes Execute_DoList skip them and
// Clean_DoList drop them. Gameplay events (and GameSpeed) are left to run on their recorded frames.
//
// The one timing event we must not ignore is Timing: it carries the FrameSendRate the recording switched
// to (ProtocolZero raises it as latency worsens). The native read gates batches on Frame % FrameSendRate,
// so playback has to track that same cadence or the file read drifts and starts decoding gameplay events
// out of garbage. So apply its FrameSendRate, but still skip the event itself - letting it run would also
// overwrite RequestedFPS and fight the viewer's chosen playback speed.
//
// Hooked at the instruction that loads the loop count, so the marking happens before the first iteration.
DEFINE_HOOK(0x64C38D, ExecuteDoList_DropTimingEventsDuringPlayback, 0x6)
{
	if (Replay::IsPlaybackActive())
	{
		auto& doList = EventClass::DoList;
		for (int i = 0; i < doList.Count; ++i)
		{
			auto& event = doList[i];

			if (event.Type == EventType::Timing)
			{
				Game::Network::FrameSendRate = std::max(1, static_cast<int>(event.Timing.FrameSendRate));
				event.IsExecuted = true;
			}
			else if (IsNonGameplayPlaybackEvent(event.Type))
			{
				event.IsExecuted = true;
			}
		}
	}

	return 0;
}

