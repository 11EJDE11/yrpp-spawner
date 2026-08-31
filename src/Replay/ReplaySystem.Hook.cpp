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

// Every engine site the replay system is wired into. The work itself lives in ReplaySystem.cpp,
// and docs/replay-format.md has the detail behind the trickier hooks.

#include "ReplayControls.h"
#include "ReplaySystem.h"
#include "ReplaySystem.Internal.h"

#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <BeaconManagerClass.h>
#include <EventClass.h>
#include <HouseClass.h>
#include <ObjectClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <TActionClass.h>
#include <Timer.h>
#include <TechnoClass.h>
#include <TriggerClass.h>
#include <TriggerTypeClass.h>
#include <Unsorted.h>

#include <algorithm>
#include <limits>

using namespace ReplaySystem::Internal;

static const EventClass* EventFromScaledDoListSlot(unsigned int scaledSlot)
{
	static_assert(sizeof(EventClass) % 3 == 0,
		"Execute_DoList scales its slot index by sizeof(EventClass) / 3 and leaves the rest to the "
		"x86 [reg + reg*2] addressing mode; a size not divisible by 3 breaks that.");
	constexpr unsigned int ScaleFactor = sizeof(EventClass) / 3;

	return &EventClass::DoList.GetArray()[scaledSlot / ScaleFactor];
}

// FootClass::Active_Click_With makes a move flash animation where the player clicked. There is no
// click on playback, so skip it while recording too, or the two diverge. Multiplayer does the same.
DEFINE_HOOK(0x4D7EB5, FootClass_ActiveClickWith_SkipMoveFlashDuringReplay, 0x5)
{
	enum { SkipMoveFlashBlock = 0x4D8065 };

	if (ReplayState.Recording || ReplayState.Playback)
		return SkipMoveFlashBlock;

	R->AL(static_cast<BYTE>(Unsorted::MoveFeedback));
	return 0x4D7EBA;
}

// Init_Random. Seed playback from the replay header rather than from the clock, so the simulation
// starts from the RNG state the recording started from.
DEFINE_HOOK(0x52FC42, InitRandom_CheckReplayMode, 0x7)
{
	if (ReplayState.InitRandomHandled)
	{
		if (ReplaySystem::IsPlaybackRequested() && ReplayState.HasPlaybackHeader)
		{
			ApplyPlaybackInitialState();
			R->EAX(Game::Seed);
			return 0x52FDF9;
		}

		return 0;
	}

	ReplayState.InitRandomHandled = true;
	if (!ReplaySystem::IsPlaybackRequested())
		return 0;

	const auto* pConfig = GetConfig();
	if (!pConfig)
		return 0;

	ReplayHeader header {};
	if (!ReadReplayHeaderFromPath(pConfig->ReplayFile, header))
	{
		Debug::Log("[Replay] Failed to read replay header from %s.\n", pConfig->ReplayFile);
		return 0;
	}

	ReplayState.PlaybackHeader = header;
	ReplayState.HasPlaybackHeader = true;
	ApplyPlaybackInitialState();

	R->EAX(Game::Seed);
	return 0x52FDF9;
}

// Clear_Scenario, at the start of every scenario: open the replay for playback or recording, or
// make sure the system is off. The hook covers the whole scenario UniqueID reset, which Syringe
// re-executes afterwards, so the counter is applied here as well for the header's benefit. Phobos
// hooks this address too, so return 0 rather than an address.
DEFINE_HOOK(0x685659, ScenarioClass_Start_ReplayInit, 0xA)
{
	auto* const pScenario = R->EDX<ScenarioClass*>();
	if (pScenario)
		pScenario->UniqueID = 1000000;

	const auto* pConfig = GetConfig();
	if (!pConfig)
		return 0;

	if (ReplaySystem::IsPlaybackRequested())
	{
		ApplyPlaybackInitialState();
		StartReplayPlayback(pConfig->ReplayFile);
	}
	else if (pConfig->EnableReplayRecording)
	{
		StartReplayRecording();
	}
	else
	{
		StopReplaySystem();
	}

	return 0;
}

// Read_Scenario_INI, just past Clear_Scenario: the first point where a restored RNG and object ID
// counter survive, and before any object exists. Object IDs come from that counter and the recorded
// selection is stored by them, so playback re-applies its initial state here.
DEFINE_HOOK(0x686B6A, ReadScenarioINI_ReplayApplyState, 0x6)
{
	if (ReplayState.Playback)
		ApplyPlaybackInitialState();

	return 0;
}

// Main_Loop, ahead of the dialog handling that can skip the rest of the frame. Captures the frame's
// visible state while recording, and restores it during playback.
DEFINE_HOOK(0x55D878, MainLoop_RecordPlaybackFrameState, 0x6)
{
	if (ReplayState.Recording)
		RecordFrameState();

	// A paused iteration must leave the stream where it was, or the frame's event count and its
	// events part company and every later frame reads the wrong bytes. The pause hooks below hold
	// the event pump and the frame counter to match.
	if (ReplayState.Playback && !ReplaySystem::Controls::IsPlaybackPaused())
		RestoreFrameState();

	return 0;
}

// Queue_AI_Multiplayer, once the engine has computed its own per-frame hash: take a copy for the
// divergence check.
DEFINE_HOOK(0x647689, Queue_AI_Multiplayer_ReplayGameCRC, 0x6)
{
	CaptureGameCRCForCurrentFrame();
	return 0;
}

// Queue_AI's campaign and skirmish branch, which never computes that hash - so compute it here, and
// write out the frame's events while the recorded copies are still to hand.
DEFINE_HOOK(0x6475B3, Queue_AI_Singleplayer_ReplayGameCRC, 0x8)
{
	const int executeResult = R->EAX<int>();

	if (SessionClass::IsSingleplayer() && (ReplayState.Recording || ReplayState.Playback))
	{
		ComputeAndCaptureGameCRCForCurrentFrame();
		if (ReplayState.Recording)
			RecordCapturedEventsForCurrentFrame();
	}

	R->EAX(executeResult);
	return executeResult != 0 ? 0x6474B8 : 0x6475BB;
}

// Execute_DoList, where an accepted MegaMission is marked consumed.
DEFINE_HOOK(0x64C75D, ExecuteDoList_RecordAcceptedMegaMission, 0x5)
{
	if (ReplayState.Recording)
		RecordExecutedEvent(EventFromScaledDoListSlot(R->EAX<unsigned int>()));

	return 0;
}

// Execute_DoList, where every other event is marked consumed.
DEFINE_HOOK(0x64CAC0, ExecuteDoList_RecordConsumedEvent, 0x7)
{
	if (ReplayState.Recording)
	{
		const auto* const pEvent = EventFromScaledDoListSlot(R->EAX<unsigned int>());
		if (pEvent && pEvent->Type != EventType::MegaMission)
			RecordExecutedEvent(pEvent);
	}

	return 0;
}

// Queue_AI_Multiplayer, at the gate for vanilla's own recording: capture this frame's events, or
// inject the recorded ones, and take vanilla's recorder out of the picture. Only while the replay
// system is active, so a normal multiplayer game keeps it.
DEFINE_HOOK(0x64820E, Queue_AI_Multiplayer_RecordPlaybackEvents, 0x7)
{
	enum { SkipQueueRecord = 0x64821C };

	if (!ReplayState.Recording && !ReplayState.Playback)
		return 0;

	if (ReplayState.Recording)
		CaptureEventsForCurrentFrame();

	if (ReplayState.Playback)
	{
		RemoveReplayGameplayEventsFromDoList();
		PlaybackFrameEvents();
	}

	return SkipQueueRecord;
}

// Queue_AI_Multiplayer, just after Execute_DoList has returned and before the queue's spent entries
// are dropped: write out the frame's events. The events themselves were captured as the engine
// consumed them; this is only the point where the frame is known to be complete. The singleplayer
// branch writes from the matching point in Queue_AI.
DEFINE_HOOK(0x648234, Queue_AI_Multiplayer_WriteFrameEvents, 0x8)
{
	enum { ExecuteSucceeded = 0x6482BD, ExecuteFailed = 0x64823C };

	const int executeResult = R->EAX<int>();

	if (ReplayState.Recording)
		RecordCapturedEventsForCurrentFrame();

	// This hook stands on vanilla's own test of the return value, so it does that test itself.
	R->EAX(executeResult);
	return executeResult != 0 ? ExecuteSucceeded : ExecuteFailed;
}

// The same gate in Queue_AI's own branch, where campaign and skirmish sessions run their events -
// neither of them ever reaches Queue_AI_Multiplayer.
DEFINE_HOOK(0x647586, Queue_AI_Singleplayer_RecordPlaybackEvents, 0x7)
{
	enum { SkipQueueRecord = 0x647594 };

	if (!SessionClass::IsSingleplayer())
		return 0;

	if (!ReplayState.Recording && !ReplayState.Playback)
		return 0;

	if (ReplayState.Recording)
		CaptureEventsForCurrentFrame();

	if (ReplayState.Playback)
	{
		RemoveReplayGameplayEventsFromDoList();
		PlaybackFrameEvents();
	}

	return SkipQueueRecord;
}

// Sync_Delay, where the game waits out the rest of a frame. Playback waits against the performance
// counter instead: the engine's frame timers are rewritten later in Main_Loop, and they are too
// coarse for the rates the viewer's speed ladder asks for.
DEFINE_HOOK(0x55E160, SyncDelay_PaceReplayPlayback, 0x6)
{
	ApplyPlaybackFramePacing();
	return 0;
}

// Main_Loop's network pump. Playback has no live session to service.
DEFINE_HOOK(0x55D8E3, MainLoop_SkipIPXPumpDuringReplayPlayback, 0x5)
{
	return ReplayState.Playback ? 0x55D8E8 : 0;
}

// Queue_AI_Multiplayer, at the test of what Wait_For_Players returned. There are no peers during
// playback, so it always reports a timeout and the frame runs into the reconnect prompts and
// "system not responding" boxes below. Take the branch a successful wait takes.
DEFINE_HOOK(0x64806E, Queue_AI_Multiplayer_ReplaySkipWaitForPlayers, 0x8)
{
	enum { WaitSucceeded = 0x64820E, WaitFailed = 0x648076 };

	if (ReplayState.Playback)
		return WaitSucceeded;

	return R->ESI<int>() == R->EBX<int>() ? WaitSucceeded : WaitFailed;
}

#pragma region Closing the replay file

// The ways a game can end, all of which close the replay file out. StopReplaySystem does nothing
// once the system is already stopped, so overlapping paths are harmless.

// Aux_Loop's abort branch: the player quit the mission from the menu.
DEFINE_HOOK(0x55D25C, AuxLoop_PlayerAborted_FlushReplayBuffers, 0x6)
{
	StopReplaySystem();
	return 0;
}

// Disconnect_Gracefully, the teardown a networked session goes through.
DEFINE_HOOK(0x55CF13, DisconnectGracefully_FlushReplayBuffers, 0x5)
{
	StopReplaySystem();
	return 0;
}

// Game_Exit, the last point before the process goes away.
DEFINE_HOOK(0x6BEC60, Game_Exit_FlushReplayBuffers, 0x5)
{
	StopReplaySystem();
	return 0;
}

// Main_Game, where the frame loop exits - the one point every ending converges on, and the only one
// a skirmish reaches at all. Hooked next to the loop's own call rather than on it, since a relative
// call cannot be relocated into a hook's saved bytes.
DEFINE_HOOK(0x48CEAF, MainGame_GameLoopFinished_FlushReplayBuffers, 0x5)
{
	StopReplaySystem();
	return 0;
}

// Do_Win, as a mission ends. A campaign starts its next mission from inside this function without
// the game loop ever exiting, so the recording is closed out here and switched off for the rest of
// the launch: a replay embeds the spawn.ini the client wrote for the mission it launched, and
// nothing describes the ones after it. Campaign only - a skirmish or multiplayer game ends at the
// loop-exit hook above, and a loss or restart re-enters the same scenario and may record again.
DEFINE_HOOK(0x685670, DoWin_FinishReplayRecording, 0x5)
{
	if (SessionClass::IsCampaign())
		FinishRecordingAtMissionEnd();

	return 0;
}

#pragma endregion Closing the replay file

#pragma region Ending playback when the mission ends

// A replay must not follow a campaign into its next mission: the score screen's only way out is
// Continue, which starts a scenario the replay has no events for. The score screen is the mission's
// result and worth keeping, so playback shows it and treats Continue as the exit; the victory and
// defeat movies are skipped. Each hook jumps to its own function's existing "game over, do not
// chain" tail, which clears GameActive and so ends the game loop and the process. They are gated on
// the spawn.ini key, so a replay stopped early by a read error still cannot chain.

// Skips the victory movie.
DEFINE_HOOK(0x685915, DoWin_SkipWinMovieDuringPlayback, 0x5)
{
	enum { AfterWinMovie = 0x68593A };

	return ReplaySystem::IsPlaybackRequested() ? AfterWinMovie : 0;
}

// Where the branches that showed and skipped the score screen meet again, so playback leaves either
// way. Everything past it is movies, map selection and the next mission.
DEFINE_HOOK(0x685965, DoWin_EndGameAfterScoreScreen, 0x6)
{
	enum { EndGameWithoutContinuing = 0x685B2D };

	return ReplaySystem::IsPlaybackRequested() ? EndGameWithoutContinuing : 0;
}

// Do_Lose. A campaign defeat has no score screen - what follows the movie is the "replay this
// mission?" prompt and a restart - so playback leaves at the movie.
DEFINE_HOOK(0x686060, DoLose_EndGameAfterPlayback, 0x5)
{
	enum { EndGameWithoutRestarting = 0x6863C3 };

	return ReplaySystem::IsPlaybackRequested() ? EndGameWithoutRestarting : 0;
}

#pragma endregion Ending playback when the mission ends

// Triggers can run off the player discovering an area. If enableShroud=false then
// they are pre-discovered which leads to divergence.
// Show enemy units on the minimap when the playback reveals the map.
DEFINE_HOOK(0x70DA48, TechnoClass_RadarTrackingAI_ShowAllOnViewerRadar, 0x8)
{
	if (!PlaybackWantsFullMapReveal())
		return 0;

	GET(TechnoClass*, pTechno, ESI);
	if (!pTechno)
		return 0;

	const auto* const pType = pTechno->GetTechnoType();
	if (!pType || pType->Invisible || pType->RadarInvisible)
		return 0;

	R->AL(1);
	return 0;
}

#pragma region Viewer map reveal

// No shroud
DEFINE_HOOK(0x48022B, CellShadowUpdate_ReplaySkipShroudBlit, 0xA)
{
	enum { SkipShroudBlit = 0x480235 };

	return PlaybackWantsFullMapReveal() ? SkipShroudBlit : 0;
}

// The same function's fog half, at the fog-of-war test. Foggedness has already been written at this
// point, so jumping to the epilogue only skips the fog shape. The function's return value is unused
// by both callers.
DEFINE_HOOK(0x480249, CellShadowUpdate_ReplaySkipFogBlit, 0x6)
{
	enum { SkipFogBlit = 0x480295 };

	return PlaybackWantsFullMapReveal() ? SkipFogBlit : 0;
}

// No fog
DEFINE_HOOK(0x487950, CellClass_IsShrouded_ReplayNeverShroudedForDrawing, 0x6)
{
	enum { ReturnNotShrouded = 0x4879A5 };

	if (!PlaybackWantsFullMapReveal())
		return 0;

	R->EAX(0);
	return ReturnNotShrouded;
}

// Fix radar drawing
DEFINE_HOOK(0x655D87, RadarClass_DrawPixel_ReplayRevealCell, 0x7)
{
	if (PlaybackWantsFullMapReveal())
	{
		R->Stack8(0x44, 0); // not shrouded
		R->Stack8(0x13, 0); // not covered by a gap generator
	}

	return 0;
}

// Allow clicking through the shroud (we don't blit it so it's still "there")
DEFINE_HOOK(0x6924FC, ScrollClass_ClickInfo_ReplayClickThroughShroud, 0x12)
{
	enum { HiddenCell = 0x6925F0, PickObject = 0x69250E };

	GET(bool*, pShrouded, ESI);
	GET(bool*, pFogged, EDI);

	if (PlaybackWantsFullMapReveal())
	{
		*pShrouded = false;
		*pFogged = false;
	}

	return (*pShrouded || *pFogged) ? HiddenCell : PickObject;
}

#pragma endregion Viewer map reveal

// Queue_AI_Multiplayer, just past where it starts its own skip-CRC timer. Playback is not being
// compared against anyone, so let that timer never run out.
DEFINE_HOOK(0x647866, Queue_AI_Multiplayer_OverrideDelayTime, 0x5)
{
	if (ReplayState.Playback)
		Unsorted::QueueAIMultiplayerSkipCRC.TimeLeft = std::numeric_limits<int>::max();

	return 0;
}


// The same function's end-of-load handshake, which broadcasts to the other players and then waits
// about four seconds for a send queue playback will never drain. Take the branch the engine already
// uses for single-player sessions.
DEFINE_HOOK(0x69AF0F, WaitForPlayers_ReplaySkipNetworkSyncDance, 0x7)
{
	if (ReplayState.Playback)
		return 0x69B14E;

	return 0;
}

#pragma region In-game options dialog game speed

// The simulation scales off the game speed option, so playback pins it to the speed the game was
// recorded at. The ESC options dialog binds its slider to that same option, which left the slider
// showing the recorded speed and doing nothing. These three give the dialog the viewer's playback
// speed instead, without ever letting it write to the pinned value.

// Opens the slider at the speed playback is running.
DEFINE_HOOK(0x4E209E, GameControlsDialog_ShowPlaybackSpeed, 0x6)
{
	if (ReplayState.Playback && ReplayState.PlaybackFPS > 0)
	{
		R->EDX(ReplaySystem::Controls::GetPlaybackGameSpeedIndex());
		return 0x4E20A4;
	}

	return 0;
}

// The apply handler does nothing when the slider matches the current speed, so compare against the
// playback speed - picking the recorded speed back would otherwise be a no-op.
DEFINE_HOOK(0x4E1E1B, GameControlsApply_ComparePlaybackSpeed, 0x5)
{
	if (ReplayState.Playback && ReplayState.PlaybackFPS > 0)
	{
		R->EAX(ReplaySystem::Controls::GetPlaybackGameSpeedIndex());
		return 0x4E1E20;
	}

	return 0;
}

// The branch campaign and skirmish take, which applies the new speed directly. Take the value for
// playback and skip the write, so the pinned value survives.
DEFINE_HOOK(0x4E1EBA, GameControlsApply_ApplyPlaybackSpeedDirectly, 0x6)
{
	if (ReplayState.Playback)
	{
		ReplaySystem::Controls::SetPlaybackGameSpeedIndex(R->ECX<int>());
		return 0x4E1EC0;
	}

	return 0;
}

#pragma endregion In-game options dialog game speed

// Init_Commands, at the address Phobos registers its own commands from. Adds the viewer's commands
// so they appear in the in-game keyboard options and can be bound there or in KEYBOARDMD.INI. They
// ship unbound: every plausible default is already taken, and this runs once per process, so a
// stolen key would reach normal games too.
DEFINE_HOOK(0x533066, Init_Commands_RegisterReplayCommands, 0x6)
{
	ReplaySystem::Controls::RegisterReplayCommands();
	return 0;
}
#pragma region Playback pause

// Pause replay simulation without blocking input or menus. The related hooks stop event processing,
// logic and the frame counter together so the current frame remains unchanged.

// Holds the logic tick.
DEFINE_HOOK(0x55DC99, MainLoop_ReplayPause_SkipLogicAI, 0x5)
{
	enum { SkipLogicAI = 0x55DCA3 };

	return ReplaySystem::Controls::IsPlaybackPaused() ? SkipLogicAI : 0;
}

// Holds the event pump, and with it the recorded events for the frame.
DEFINE_HOOK(0x55DE3A, MainLoop_ReplayPause_SkipQueueAI, 0x6)
{
	enum { SkipQueueAI = 0x55DE45 };

	return ReplaySystem::Controls::IsPlaybackPaused() ? SkipQueueAI : 0;
}

// Holds the frame counter, and is where a paused iteration gets its viewport committed and drawn.
DEFINE_HOOK(0x55DE73, MainLoop_ReplayPause_SkipFrameAdvance, 0x6)
{
	enum { SyncDelay = 0x55DE9A };

	if (!ReplaySystem::Controls::IsPlaybackPaused())
		return 0;

	ReplaySystem::Controls::RenderPausedFrame();
	return SyncDelay;
}

#pragma endregion Playback pause

#pragma region Side-channel recording taps

// Chat, beacons and taunts bypass EventClass::DoList and are recorded here.

namespace
{
	// Some local beacon calls pass -1 for both house and slot, meaning the beacon the local player
	// has selected. Resolve it before writing the replay.
	bool ResolveSelectedBeaconSlot(int& house, int& slot)
	{
		for (int h = 0; h < MaxHouses; ++h)
		{
			for (int s = 0; s < MaxBeaconSlots; ++s)
			{
				BeaconClass* pBeacon = BeaconManagerClass::Instance.Beacons[h][s];
				if (pBeacon && pBeacon->IsSelected())
				{
					house = h;
					slot = s;
					return true;
				}
			}
		}

		return false;
	}
}

// BeaconManagerClass::Place. Arguments come off the stack: house, coordinate, and the beacon slot,
// where -1 asks the engine to pick a free one.
DEFINE_HOOK(0x430BA0, BeaconManagerClass_Place_RecordPlacement, 0x6)
{
	if (ReplayState.Recording)
	{
		const int house = R->Stack<int>(0x4);
		CoordStruct coord;
		coord.X = R->Stack<int>(0x8);
		coord.Y = R->Stack<int>(0xC);
		coord.Z = R->Stack<int>(0x10);
		const int slot = R->Stack<int>(0x14);

		ReplaySystem::RecordBeaconPlace(house, coord, slot);
	}

	return 0;
}

// BeaconManagerClass::DeleteBeacon, with house and slot on the stack.
DEFINE_HOOK(0x4311C0, BeaconPlacement_Delete_RecordDeletion, 0x6)
{
	if (ReplayState.Recording)
	{
		int house = R->Stack<int>(0x4);
		int slot = R->Stack<int>(0x8);

		bool shouldRecord = true;
		if (house == -1 && slot == -1)
			shouldRecord = ResolveSelectedBeaconSlot(house, slot);

		if (shouldRecord)
			ReplaySystem::RecordBeaconDelete(house, slot);
	}

	return 0;
}

// BeaconManagerClass::EditBeaconMessage. Skips the local compose preview and records the final and
// remote-applied text.
DEFINE_HOOK(0x431450, BeaconPlacement_Message_RecordText, 0x6)
{
	if (ReplayState.Recording)
	{
		const wchar_t* text = R->Stack<const wchar_t*>(0x4);
		int house = R->Stack<int>(0x8);
		int slot = R->Stack<int>(0xC);
		const int broadcast = R->Stack<int>(0x10);

		bool shouldRecord = broadcast != 0 || house != -1 || slot != -1;
		// Committed local text still needs its concrete beacon slot.
		if (shouldRecord && house == -1 && slot == -1)
			shouldRecord = ResolveSelectedBeaconSlot(house, slot);

		if (shouldRecord)
			ReplaySystem::RecordBeaconText(house, slot, text);
	}

	return 0;
}

// Taunt playback, which happens outside the event system.
DEFINE_HOOK(0x752B70, Taunts_RecordPlayback, 0x5)
{
	if (ReplayState.Recording)
		ReplaySystem::RecordTaunt(R->ECX<int>());

	return 0;
}

#pragma endregion Side-channel recording taps

// Speak, the one entry point every EVA announcement goes through. Those lines belong to the player
// who recorded the game, and someone watching from an observer's seat is not that player. Zeroing
// the EVA stream handle sends the function down its own "nothing to speak on" route.
DEFINE_HOOK(0x752480, Speak_SilenceDuringSpectatorPlayback, 0x5)
{
	if (ReplaySystem::IsSpectatorPlayback())
	{
		R->EAX(0);
		return 0x752485;
	}

	return 0;
}
