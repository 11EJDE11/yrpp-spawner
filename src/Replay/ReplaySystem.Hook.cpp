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
#include "ReplayOverlay.h"
#include "ReplaySeek.h"
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
#include <WWMouseClass.h>
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

	// A seek loads a keyframe through the engine's own savegame path, which comes back through
	// here. Reopening the replay would throw away the playback already in flight and start it
	// again from frame zero; the seek repositions the stream itself. See ReplaySeek.h.
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

// Read_Scenario_INI, just past Clear_Scenario: the first point where a restored RNG and object ID
// counter survive, and before any object exists. Object IDs come from that counter and the recorded
// selection is stored by them, so playback re-applies its initial state here.
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

	// Both of these want a whole, quiescent frame: the single-step machine has to settle the
	// pause flag before anything in this iteration reads it, and a keyframe save or load is a
	// snapshot of the entire world. This is the only point in the loop that is either.
	ReplaySystem::Controls::ServiceFrameStart();
	ReplaySystem::Seek::ServiceFrameStart();

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

// The two ways to draw from a Random2Class. They do not share an implementation: the ranged one
// walks the table itself rather than calling the other, and it draws repeatedly until the value
// fits the range - so both have to be tapped, and one ranged call can move the table more than
// once. Almost all gameplay randomness comes through the ranged form; Compute_Game_CRC uses the
// plain one. See TraceRandomDraw in ReplaySystem.cpp.
// TechnoClass::Target_Something_Nearby, which draws as its first act. The divergence trace
// keeps catching this one, and knowing which object asked is the difference between a
// techno whose schedule slipped and a different techno entirely running in its place.
// MissionClass::Assign_Mission, the single point every mission change goes through. Recorded
// per frame so a seek can be checked against what the same frame did the first time round.
// FootClass::Basic_Path, the one entry point every route request goes through. Recorded per
// frame so a seek can be checked against the questions the same frame asked the first time.
// AircraftClass::New_LZ. Recorded on the way in, and the aircraft is named as the asker so the
// randomiser report attributes the draw inside it rather than saying "object 0".
// The object update loop at the end of LogicClass::AI, one call per object per frame:
//
//     55B608  mov eax, [edi+4]           ; Logic.Objects.Vector
//     55B60B  mov ecx, [eax+esi*4]       ; the object, five bytes with the next instruction
//     55B60E  mov edx, [ecx]
//     55B610  call dword ptr [edx+5Ch]   ; three bytes
//     55B613  mov eax, [edi+10h]         ; three bytes
//
// Hooked at 55B60B, where the two register loads come to exactly five bytes. Hooking the call
// itself does not: five bytes from 55B610 takes the three-byte call and the first two bytes of
// the instruction after it, and the game came apart with EIP at zero and a stack pointer one
// byte out of alignment. The object is read the way the split instruction would have.
DEFINE_HOOK(0x55B60B, LogicClass_AI_TraceUpdateOrder, 0x5)
{
	GET(void** const, objects, EAX);
	GET(int const, index, ESI);

	if (objects && index >= 0)
		TraceObjectUpdate(objects[index]);

	return 0;
}

// FlyLocomotionClass::Nearing_Target, the step above New_LZ. The landing zone trace showed the
// scan entered on one pass and not the other; this says whether the locomotor step that asks
// for it ran at all.
//
//     4CEFB0  sub esp, 38h        ; three bytes
//     4CEFB3  push ebx            ; one
//     4CEFB4  push ebp            ; one, so five lands on a boundary
//     4CEFB6  lea esi, [ecx+0Ch]  ; ECX is the locomotor, +0x0C is LinkedTo
DEFINE_HOOK(0x4CEFB0, FlyLocomotion_NearingTarget_TraceForDivergence, 0x5)
{
	GET(void** const, locomotor, ECX);
	GET_STACK(int const, coordX, 0x8);
	GET_STACK(int const, coordY, 0xC);

	if (locomotor)
	{
		// The coord the locomotor was told to head for, as a cell, so a difference in where it
		// was going is separable from a difference in what it found there.
		TraceLandingZoneCell(1, static_cast<TechnoClass*>(locomotor[3]),
			coordX / 256, coordY / 256, nullptr);
	}

	return 0;
}

// ObjectClass::Remove_This, one link above Limbo. The Limbo trace came back with its caller in
// Ares.dll+0x5EF44, which is ObjectClass_UnInit_SkipInvalidation - an Ares hook on the engine's
// own removal path that calls the object's Limbo virtual. So the plane is being removed, and
// what remains is what decided to remove it.
//
// Not AircraftClass::Mission_Spyplane_Overfly, which is the obvious candidate and is ruled out:
// it reveals what it is over and steers to a map edge cell, and removes nothing.
//
//     5F65F0  push esi           ; one byte
//     5F65F1  mov esi, ecx       ; two
//     5F65F3  mov ecx, [esi+38h] ; three, so six is the first clean boundary past five
// The first return address on the stack that still lies inside the executable. The immediate
// caller here is a hook in a mod DLL, and that DLL's shipped build does not match any source
// tree to hand - but the frame above it is the engine function the hook was placed on, and that
// is resolvable in a database that does match.
static const void* Engine_Frame_Above(REGISTERS* R)
{
	HMODULE hSelf = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, nullptr, &hSelf))
		return nullptr;

	const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hSelf);
	const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
		reinterpret_cast<const unsigned char*>(hSelf) + dos->e_lfanew);

	// Code only. Accepting the whole image let a data pointer through: one report named
	// gamemd-spawn.exe+0x47F778, which is the Logic global rather than a return address.
	const auto base = reinterpret_cast<uintptr_t>(hSelf) + nt->OptionalHeader.BaseOfCode;
	const auto end = base + nt->OptionalHeader.SizeOfCode;

	// Past the immediate caller, which is the one already recorded.
	for (int at = 4; at < 0x100; at += 4)
	{
		const auto value = reinterpret_cast<uintptr_t>(R->Stack<const void*>(at));
		if (value > base && value < end)
			return reinterpret_cast<const void*>(value);
	}

	return nullptr;
}

// FootClass::Remove_This, which is where the removal actually starts. The frame above the
// Phobos caller came back as gamemd-spawn.exe+0xDE610 - the return address just past 0x4DE60B,
// and the Phobos v0.4.0.2 source names that address outright:
//
//     DEFINE_FUNCTION_JUMP(CALL, 0x4DE60B, TechnoClass_UnInit_Wrapper);   // FootClass
//
// So Phobos decides nothing there; it has wrapped the UnInit call inside Remove_This to keep
// its limbo tracking straight. Everything from Remove_This down is teardown, and the decision
// belongs to whoever called it.
//
//     4DE5D0  push esi              ; one byte
//     4DE5D1  mov esi, ecx          ; two
//     4DE5D3  mov ecx, [esi+2BCh]   ; six, so nine is the first clean boundary
DEFINE_HOOK(0x4DE5D0, FootClass_RemoveThis_TraceForDivergence, 0x9)
{
	GET(TechnoClass* const, pFoot, ECX);

	TraceLandingZoneCell(8, pFoot, -1, -1, nullptr, R->Stack<const void*>(0x0),
		Engine_Frame_Above(R));
	return 0;
}

// The exact first predicate guarding AircraftClass::AI's off-map removal:
//
//     GetMapCoords(&cell);
//     if (!Map.In_Radar(cell) && Should_Delete_Off_Map())
//         Remove_This();
//
// The site-8 caller named that virtual Remove_This call (0x414FD1). Instrument the
// In_Radar entry rather than guessing from its consequence. At this call ESI is still the
// AircraftClass, the cell pointer is the first argument, and the return address is 0x414FBB.
// MapClass::In_Radar itself is pure rectangle arithmetic, repeated here so the trace records
// the value the function is about to return without changing any game state.
//
//     568300  mov eax, [esp+4]      ; four bytes
//     568304  mov edx, [ecx+0F4h]  ; six bytes, so ten lands on a boundary
DEFINE_HOOK(0x568300, AircraftClass_AI_TraceOffMapPredicate, 0xA)
{
	const auto* const caller = R->Stack<const void*>(0x0);
	if (caller != reinterpret_cast<const void*>(0x414FBB))
		return 0;

	GET(MapClass* const, pMap, ECX);
	GET_STACK(const CellStruct* const, pCell, 0x4);
	GET(TechnoClass* const, pAircraft, ESI);

	// Keep this focused on the overflight aircraft which exposed the divergence. Recording
	// every aircraft on every frame would exhaust the bounded trace before a long seek.
	if (!pMap || !pCell || !pAircraft
		|| static_cast<int>(pAircraft->CurrentMission) != 31)
	{
		return 0;
	}

	const int x = pCell->X;
	const int y = pCell->Y;
	const int width = pMap->MapRect.Width;
	const int height = pMap->MapRect.Height;
	const int sum = x + y;
	const int inRadar = sum > width && x - y < width && y - x < width
		&& sum <= width + 2 * height;

	TraceLandingZoneCell(9, pAircraft, x, y, nullptr, caller, nullptr,
		inRadar ? 1 : 0, pAircraft->IsInPlayfield ? 1 : 0,
		pAircraft->IsALoaner ? 1 : 0, static_cast<int>(pAircraft->CurrentMission),
		pAircraft->Target, width, height);
	return 0;
}


DEFINE_HOOK(0x5F65F0, ObjectClass_RemoveThis_TraceForDivergence, 0x6)
{
	GET(TechnoClass* const, pObject, ECX);

	TraceLandingZoneCell(7, pObject, -1, -1, nullptr, R->Stack<const void*>(0x0),
		Engine_Frame_Above(R));
	return 0;
}

// FootClass::Limbo, which is what the spy plane goes through on the pass where it dies: the
// Stop_Driver trace came back with its caller at gamemd-spawn.exe+0xDB301, the return address
// just past the Stop_Driver call inside this function. So one pass limbos the plane at frame
// 7508 and the other never does, and what decided that is this function's own caller.
//
// Not FlyLocomotionClass::Edge_Of_World_AI, which is the obvious candidate and is ruled out:
// it removes an aircraft only when Get_Mission() == MISSION_RETREAT, and this one is on
// mission 31, SpyplaneOverfly.
//
//     4DB260  sub esp, 8      ; three bytes
//     4DB263  push edi        ; one
//     4DB264  mov edi, ecx    ; two, so six is the first clean boundary past five
DEFINE_HOOK(0x4DB260, FootClass_Limbo_TraceForDivergence, 0x6)
{
	GET(TechnoClass* const, pFoot, ECX);

	TraceLandingZoneCell(6, pFoot, -1, -1, nullptr, R->Stack<const void*>(0x0));
	return 0;
}

// FootClass::Stop_Driver, which is the only thing that calls a locomotor's Stop_Moving:
//
//     this->Locomotion->Stop_Moving(this->Locomotion);
//
// It is a FootClass virtual, so hooking it here catches every dispatch to it. The caller is
// what matters: FootClass::Limbo is among them, which is what runs when an object is being
// taken off the map - and the spy plane reaching Stop_Moving in one pass and not the other is
// the whole divergence.
//
//     4D55C0  push esi              ; one byte
//     4D55C1  mov esi, ecx          ; two
//     4D55C3  mov eax, [esi+674h]   ; six
//
// Three bytes is short of the five a jump needs and the next instruction is six, so nine is
// the first clean boundary. All three are register-relative and safe to relocate.
DEFINE_HOOK(0x4D55C0, FootClass_StopDriver_TraceForDivergence, 0x9)
{
	GET(TechnoClass* const, pFoot, ECX);

	TraceLandingZoneCell(5, pFoot, -1, -1, nullptr, R->Stack<const void*>(0x0));
	return 0;
}

// FlyLocomotionClass::Stop_Moving, the other caller of New_LZ.
//
//     4CCFD0  sub esp, 18h            ; three bytes
//     4CCFD3  push esi                ; one
//     4CCFD4  mov esi, [esp+1Ch]      ; four, so five from 4CCFD0 would split it
//
// Hooked one instruction in, where push esi and the mov come to exactly five. The locomotor
// is the stack argument, and LinkedTo sits at +0x0C inside it.
DEFINE_HOOK(0x4CCFD3, FlyLocomotion_StopMoving_TraceForDivergence, 0x5)
{
	GET_STACK(void** const, locomotor, 0x1C);

	if (locomotor)
	{
		// Everything this function does is behind one gate:
		//
		//     X = this->IsDirty(this);   // 0x4CCFDC
		//     if (X) { ... New_LZ(Owner, Map[cell]) ... }
		//
		// and IsDirty is "mov dl, [ecx+11h]; setz al" - it returns the inverse, so the body
		// runs when Dirty is clear. Past that the aircraft can go three ways: a null cell
		// takes damage and dies, MISSION_ATTACK goes to Good_LZ, anything else to New_LZ.
		// So the gate and the mission are what decide which, and both travel here.
		const int dirty = *(reinterpret_cast<const unsigned char*>(locomotor) + 0x11) ? 1 : 0;

		auto* const pOwner = static_cast<TechnoClass*>(locomotor[2]);
		const int mission = pOwner ? static_cast<int>(pOwner->CurrentMission) : -1;

		TraceLandingZoneCell(4, pOwner, dirty, mission);
	}

	return 0;
}

// The CellClass::Cell_Building call inside Nearing_Target - exactly five bytes, and ECX is the
// destination cell. Reaching this says the two runs agree on where the aircraft is heading.
DEFINE_HOOK(0x4CF0B3, FlyLocomotion_NearingTarget_TraceDestinationCell, 0x5)
{
	GET(CellClass* const, pCell, ECX);

	if (pCell)
		TraceLandingZoneCell(2, nullptr, pCell->MapCoords.X, pCell->MapCoords.Y, nullptr);

	return 0;
}

// RadioClass::Has_Contact_Index. Reaching this from Nearing_Target says a building was found on
// that cell and the aircraft is not a hunter-seeker, so only the answer is left to differ - and
// the answer decides between docking, which draws nothing, and New_LZ, which draws six times.
//
//     65AD50  push esi                 ; one byte
//     65AD51  mov esi, [esp+8]         ; four, so five lands on a boundary
DEFINE_HOOK(0x65AD50, RadioClass_HasContactIndex_TraceForDivergence, 0x5)
{
	GET(TechnoClass* const, pAsker, ECX);
	GET_STACK(AbstractClass* const, pOther, 0x4);

	// Both runs reach here with the same aircraft and the same building and only one goes on
	// to New_LZ, so the answer is what differs - and recording the links it answers from was
	// not enough, because they matched too. So the answer itself is worked out here, exactly
	// the way the function does it:
	//
	//     for (i = 0; i < this->Radio.VectorMax; ++i)
	//         if (this->Radio.Vector[i] == radio) return true;
	//
	// It is a raw pointer scan, so it is free of side effects and safe to repeat here.
	int links = -1;
	int contact = -1;
	if (pAsker)
	{
		links = pAsker->RadioLinks.Capacity;
		contact = 0;
		for (int at = 0; at < links; ++at)
		{
			if (static_cast<AbstractClass*>(pAsker->RadioLinks[at]) == pOther)
			{
				contact = 1;
				break;
			}
		}
	}

	TraceLandingZoneCell(3, pAsker, links, contact, pOther);
	return 0;
}

DEFINE_HOOK(0x418E20, AircraftClass_NewLZ_TraceForDivergence, 0x6)
{
	GET(TechnoClass* const, pAircraft, ECX);
	GET_STACK(CellClass* const, pZone, 0x4);

	// Which of New_LZ's two call sites this is. Nearing_Target reaching it and Stop_Moving
	// reaching it are different events, and the trace could not tell them apart.
	TraceLandingZoneCell(0, pAircraft, pZone ? pZone->MapCoords.X : -1,
		pZone ? pZone->MapCoords.Y : -1, nullptr, R->Stack<const void*>(0x0));
	SetRandomDrawContext(pAircraft);
	return 0;
}

DEFINE_HOOK(0x4D3920, FootClass_BasicPath_TraceForDivergence, 0x5)
{
	TracePathRequest(R->ECX<void*>(), R->Stack<int>(0x4), R->Stack<int>(0x8), R->Stack<int>(0xC));
	return 0;
}

DEFINE_HOOK(0x5B35E0, MissionClass_AssignMission_TraceForDivergence, 0x5)
{
	TraceMissionAssignment(R->ECX<void*>(), R->Stack<int>(0x4), R->Stack<const void*>(0x0));
	return 0;
}

DEFINE_HOOK(0x709820, TechnoClass_TargetSomethingNearby_NameTheAsker, 0x5)
{
	if (ReplayState.Playback)
		SetRandomDrawContext(R->ECX<TechnoClass*>());

	return 0;
}

DEFINE_HOOK(0x65C780, Random2Class_Draw_TraceForDivergence, 0x5)
{
	// The caller is still on the stack: nothing has been pushed at the function's first byte.
	TraceRandomDraw(R->ECX<const void*>(), R->Stack<const void*>(0x0));
	return 0;
}

DEFINE_HOOK(0x65C7E0, Random2Class_DrawRanged_TraceForDivergence, 0x6)
{
	TraceRandomDraw(R->ECX<const void*>(), R->Stack<const void*>(0x0));
	return 0;
}

#pragma region Playback controls overlay

// GScreenClass::Render, after the sidebar, the message list and the tooltips have gone onto the
// composite surface and before the mouse cursor is blitted over it. Nothing in the game can cover
// the panel from here, and the cursor still sits on top of it.
DEFINE_HOOK(0x4F4583, GScreenClass_Render_DrawReplayOverlay, 0x6)
{
	ReplaySystem::Overlay::Draw();
	return 0;
}

// MouseClass::AI, where it hands the frame's input to the rest of the chain - scrolling, then the
// sidebar, then the map. The two stolen instructions load that call's arguments off the stack;
// taking the input here means a click on the panel is not also a click on the map behind it, and
// the bottom screen edge underneath it does not scroll the view. Keyboard input is untouched:
// Main_Loop runs Keyboard_Process on the same key after GScreenClass::Input returns.
DEFINE_HOOK(0x5BDF13, MouseClass_AI_ReplayOverlayInput, 0x8)
{
	// Past the two argument loads, the two pushes and the call itself, landing on the epilogue.
	// Because the pushes are skipped as well, the stack is left exactly as it is here, which
	// is what the callee-cleaned call would otherwise have tidied up.
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
