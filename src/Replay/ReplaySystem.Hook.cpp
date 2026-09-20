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

// The lifecycle, simulation, and viewer hooks. Side-channel recording taps live in ReplaySideChannels.Hook.cpp,
// and docs/replay-format.md has the detail behind the trickier hooks.

#include "ReplayControls.h"
#include "ReplayFile.h"
#include "ReplayOverlay.h"
#include "ReplaySeek.h"
#include "ReplaySystem.h"
#include "ReplaySystem.Internal.h"

#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <EventClass.h>
#include <HouseClass.h>
#include <ProgressScreenClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <Timer.h>
#include <TechnoClass.h>
#include <WWMouseClass.h>
#include <Unsorted.h>

#include <limits>

using namespace ReplaySystem::Internal;

static const EventClass* EventFromScaledDoListSlot(unsigned int scaledSlot)
{
	static_assert(sizeof(EventClass) % 3 == 0,
		"DoList slot scaling requires EventClass to be divisible by three.");
	constexpr unsigned int ScaleFactor = sizeof(EventClass) / 3;

	return &EventClass::DoList.GetArray()[scaledSlot / ScaleFactor];
}

DEFINE_HOOK(0x6FBFD7, TechnoClass_Select_ReplaySelectionTrigger, 0x5)
{
	enum { SkipSpring = 0x6FBFE9 };

	if (ReplayState.Playback)
		return SkipSpring;

	if (ReplayState.Recording)
	{
		GET(TechnoClass* const, pThis, ESI);
		RecordSelectionTriggerSpring(pThis);
	}

	return 0;
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
	if (!Replay::ReadReplayHeaderFromPath(pConfig->ReplayFile, header))
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

DEFINE_HOOK(0x685659, ScenarioClass_Start_ReplayInit, 0xA)
{
	auto* const pScenario = R->EDX<ScenarioClass*>();
	if (pScenario)
		pScenario->UniqueID = 1000000;

	const auto* pConfig = GetConfig();
	if (!pConfig)
		return 0;

	if (ReplaySystem::Seek::IsLoadInProgress())
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

DEFINE_HOOK(0x686B6A, ReadScenarioINI_ReplayApplyState, 0x6)
{
	// A keyframe load restores the recorded state instead of re-deriving it, so re-seeding the
	// RNG and the object ID counter here would undo the load.
	if (ReplayState.Playback && !ReplaySystem::Seek::IsLoadInProgress())
		ApplyPlaybackInitialState();

	return 0;
}

// Main_Loop, ahead of the dialog handling that can skip the rest of the frame. Captures the frame's
// visible state while recording, and restores it during playback.
DEFINE_HOOK(0x55D878, MainLoop_RecordPlaybackFrameState, 0x6)
{
	if (ReplayState.Recording)
		RecordFrameState();

	ReplaySystem::Controls::ServiceFrameStart();
	ReplaySystem::Seek::ServiceFrameStart();

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

// Record accepted megamissions when the game executes the DoList.
DEFINE_HOOK(0x64C75D, ExecuteDoList_RecordAcceptedMegaMission, 0x5)
{
	if (ReplayState.Recording)
		RecordExecutedEvent(EventFromScaledDoListSlot(R->EAX<unsigned int>()));

	return 0;
}

// Record non-megamission events when the game executes the DoList.
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

DEFINE_HOOK(0x55E160, SyncDelay_PaceReplayPlayback, 0x6)
{
	ReplaySystem::Controls::ApplyFramePacing();
	return 0;
}

// Main_Loop's network pump. Playback has no live session to service.
DEFINE_HOOK(0x55D8E3, MainLoop_SkipIPXPumpDuringReplayPlayback, 0x5)
{
	return ReplayState.Playback ? 0x55D8E8 : 0;
}

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

DEFINE_HOOK(0x48CEAF, MainGame_GameLoopFinished_FlushReplayBuffers, 0x5)
{
	StopReplaySystem();
	return 0;
}

DEFINE_HOOK(0x685670, DoWin_FinishReplayRecording, 0x5)
{
	if (SessionClass::IsCampaign())
		FinishRecordingAtMissionEnd();

	return 0;
}

#pragma endregion Closing the replay file

#pragma region Ending playback when the mission ends


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

// Skips the tab thumb restore at the end of a match, which hangs the game when watching as a
// spectator. Playback has no thumb to restore.
DEFINE_HOOK(0x4FC591, MPlayerDefeated_SkipThumbRestoreDuringSpectatorPlayback, 0x5)
{
	enum { SkipThumbRestore = 0x4FC5AA };

	return ReplaySystem::IsSpectatorPlayback() ? SkipThumbRestore : 0;
}

#pragma endregion Ending playback when the mission ends

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


DEFINE_HOOK(0x69AF0F, WaitForPlayers_ReplaySkipNetworkSyncDance, 0x7)
{
	if (ReplayState.Playback)
	{
		// Scenario_Load_Wait (0x684370) holds the load until every slot reads complete, and a replay
		// has no peers to report in, so the peers are declared finished here.
		//
		// Slot 0 is the local player and is deliberately left alone. Session_Callback advances and
		// repaints the bar a few bytes above this hook, and only while Get_Player_Progress(0) is
		// still behind the percentage it was handed - so writing slot 0 here satisfies that test
		// forever and freezes the loading bar wherever it happened to be.
		auto& progresses = ProgressScreenClass::Instance.PlayerProgresses;
		for (size_t i = 1; i < std::size(progresses); ++i)
			progresses[i] = 100.0;

		return 0x69B14E;
	}

	return 0;
}

#pragma region Playback controls overlay

DEFINE_HOOK(0x4F4583, GScreenClass_Render_DrawReplayOverlay, 0x6)
{
	ReplaySystem::Overlay::Draw();
	return 0;
}

DEFINE_HOOK(0x5BDF13, MouseClass_AI_ReplayOverlayInput, 0x8)
{
	enum { SkipInputChain = 0x5BDF24 };

	if (ReplayState.Playback && WWMouseClass::Instance)
	{
		const int mouseX = WWMouseClass::Instance->GetX();
		const int mouseY = WWMouseClass::Instance->GetY();

		if (ReplaySystem::Overlay::ProcessMouseInput(mouseX, mouseY))
			return SkipInputChain;
	}

	return 0;
}

// Main_Loop's per-frame render. A seek runs frames as fast as it can and drawing is most of what
// one costs, so it draws only every so often - enough to read as progress rather than as a hang.
DEFINE_HOOK(0x55D8F2, MainLoop_SkipRenderWhileSeeking, 0x5)
{
	enum { SkipRender = 0x55D8F7 };

	if (!ReplaySystem::Seek::IsSeeking())
		return 0;

	if (ReplaySystem::Seek::ShouldSkipRenderThisFrame())
	{
		ReplaySystem::Seek::CountRenderedFrame();
		return SkipRender;
	}

	ReplaySystem::Seek::CountRenderedFrame();
	return 0;
}

#pragma endregion Playback controls overlay

#pragma region In-game options dialog game speed


// Opens the slider at the speed playback is running.
DEFINE_HOOK(0x4E209E, GameControlsDialog_ShowPlaybackSpeed, 0x6)
{
	if (ReplayState.Playback && ReplaySystem::Controls::HasPlaybackSpeed())
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
	if (ReplayState.Playback && ReplaySystem::Controls::HasPlaybackSpeed())
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

#pragma region Playback pause

// Pause replay simulation without blocking input or menus. The related hooks stop event processing,
// logic and the frame counter together so the current frame remains unchanged.

// Hide the observer from the simulation while a step runs, so a spectated house is not treated as
// an observer by gameplay code the way it is by the display. Spectator playback only.
namespace
{
	HouseClass* HiddenSpectatorObserver = nullptr;

	void HideSpectatorObserverFromSimulation()
	{
		if (ReplaySystem::IsSpectatorPlayback()
			&& HouseClass::Observer
			&& HouseClass::Observer == HouseClass::CurrentPlayer)
		{
			HiddenSpectatorObserver = HouseClass::Observer;
			HouseClass::Observer = nullptr;
		}
	}

	void RestoreSpectatorObserverAfterSimulation()
	{
		if (HiddenSpectatorObserver)
		{
			HouseClass::Observer = HiddenSpectatorObserver;
			HiddenSpectatorObserver = nullptr;
		}
	}
}

// Holds the logic tick.
DEFINE_HOOK(0x55DC99, MainLoop_ReplayPause_SkipLogicAI, 0x5)
{
	enum { SkipLogicAI = 0x55DCA3 };

	if (ReplaySystem::Controls::IsPlaybackPaused())
		return SkipLogicAI;

	HideSpectatorObserverFromSimulation();
	return 0;
}

// Restore the observer after the simulation update.
DEFINE_HOOK(0x55DCA3, MainLoop_Spectator_RestoreObserverAfterLogicAI, 0x5)
{
	RestoreSpectatorObserverAfterSimulation();
	return 0;
}

// Holds the event pump, and with it the recorded events for the frame.
DEFINE_HOOK(0x55DE3A, MainLoop_ReplayPause_SkipQueueAI, 0x6)
{
	enum { SkipQueueAI = 0x55DE45 };

	if (ReplaySystem::Controls::IsPlaybackPaused())
		return SkipQueueAI;

	HideSpectatorObserverFromSimulation();
	return 0;
}

// Restore the observer after the event pump.
DEFINE_HOOK(0x55DE45, MainLoop_Spectator_RestoreObserverAfterQueueAI, 0x5)
{
	RestoreSpectatorObserverAfterSimulation();
	return 0;
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

DEFINE_HOOK(0x752480, Speak_SilenceDuringSpectatorPlayback, 0x5)
{
	if (ReplaySystem::IsSpectatorPlayback())
	{
		R->EAX(0);
		return 0x752485;
	}

	return 0;
}
