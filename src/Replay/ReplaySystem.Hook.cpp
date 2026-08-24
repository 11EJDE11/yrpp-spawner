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

// Every hook the replay system installs. The work itself lives in ReplaySystem.cpp; this file is
// the list of engine sites it is wired into, and the reasoning for each one.

#include "ReplaySystem.h"
#include "ReplaySystem.Internal.h"

#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <BeaconManagerClass.h>
#include <HouseClass.h>
#include <ProgressScreenClass.h>
#include <ScenarioClass.h>

#include <limits>

using namespace ReplaySystem::Internal;

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

// Hooks Clear_Scenario's ten byte `mov [edx+214h], 1000000`; a 0x5 hook would split the immediate.
// Return 0, not 0x685663 - Phobos hooks this address too.
//
// Syringe re-executes that reset after every hook here has run, so this body still sees the
// previous scenario's counter and applies the reset by hand for BuildReplayHeader's benefit.
// Nothing written to UniqueID here survives; the playback restore is in the 0x686B6A hook below.
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

// Read_Scenario_INI, just after its `call Clear_Scenario`. Order is
// Select_Game -> Init_Random -> ... -> Read_Scenario_INI -> Clear_Scenario, so this is the first
// point past the UniqueID reset and before any object exists - every earlier restore is undone.
// It has to be pinned because object UniqueIDs come from ++ScenarioClass::UniqueID and the
// recorded selection is stored by them. The hooked `cmp Session, ebx` is also the `jnz` target
// that skips Clear_Scenario, so both paths are covered.
DEFINE_HOOK(0x686B6A, ReadScenarioINI_ReplayApplyState, 0x6)
{
	if (ReplayState.Playback)
		ApplyPlaybackInitialState();

	return 0;
}

// MapClass::Reset_Shroud (thiscall; EAX holds `house` by this point, from the `mov eax,
// [esp+house]` at the function's true entry, 0x577AB0). Gameplay reshrouds the local player
// constantly during a normal match - gap generators, spy satellite loss, shroud crates, map
// triggers - and whenever it targets PlayerPtr (or a null house, which it treats the same way
// after logging a debug warning) its body is a full map-cell walk plus a forced full-screen redraw
// and radar rebuild. During full-map-reveal playback (ReplayShroudEnabled=false or spectator view)
// MaintainFullMapReveal notices the resulting Visionary==false on the very next frame and
// immediately re-runs the equally expensive Reveal (0x577D90), so every reshroud event in the
// recording pays for two full-map passes - the intermittent CPU spikes during playback. Skipping
// the reshroud for the local player in that mode removes both: the map simply never leaves
// "revealed", which is what the setting asks for anyway. Other houses' cheap MapIsClear
// bookkeeping (the only work this function does for house != PlayerPtr) is left untouched.
//
// Size is 6, not 2: Syringe's injected jump needs a full 5 bytes regardless of what's declared
// here, and a too-small size just means it silently overwrites bytes beyond what it was told about
// (this exact mistake, on a different hook, produced a same-session access-violation crash 5 bytes
// past its declared 3-byte hook). 2 only covers `cmp eax, ebx`; 6 is the next instruction-aligned
// boundary at or past 5, covering `cmp eax, ebx; mov esi, ecx; jz loc_577AD0` as a whole (ending
// exactly at 0x577ABE, the start of `mov ecx, [eax+30h]`) - confirmed none of those three
// instructions are themselves a jump target from elsewhere in the function, so claiming them is
// safe.
DEFINE_HOOK(0x577AB8, MapClass_ResetShroud_SkipDuringFullRevealPlayback, 0x6)
{
	HouseClass* const house = R->EAX<HouseClass*>();
	if (ReplayState.Playback && PlaybackWantsFullMapReveal()
		&& (house == HouseClass::CurrentPlayer || house == nullptr))
	{
		return 0x577B9F;
	}

	return 0;
}

// Capture visible replay state before dialog handling can skip the normal replay path.
DEFINE_HOOK(0x55D878, MainLoop_RecordPlaybackFrameState, 0x6)
{
	if (ReplayState.Recording)
	{
		RecordFrameState();
	}

	if (ReplayState.Playback)
	{
		ApplyReplayTimingFromCurrentGameSpeed();
		RestoreFrameState();
	}

	return 0;
}

// The hooked `test AttractBitfield, 1` gates the engine's own Queue_Record call. Returning past
// it is how playback keeps that from running, so only do it when the replay system is actually
// active - otherwise a plain multiplayer game silently loses vanilla's recording path.
DEFINE_HOOK(0x64820E, Queue_AI_Multiplayer_RecordPlaybackEvents, 0x7)
{
	enum { SkipQueueRecord = 0x64821C };

	if (!ReplayState.Recording && !ReplayState.Playback)
		return 0;

	if (ReplayState.Recording)
	{
		RecordEventsForCurrentFrame();
	}

	if (ReplayState.Playback)
	{
		RemoveReplayGameplayEventsFromDoList();
		PlaybackFrameEvents();
	}

	return SkipQueueRecord;
}

// Spawner/Statistics.cpp jumps to this hook's address (0x64820E) to skip the statistics packet
// during playback, so playback still reaches the event pump below. Moving this hook means moving
// that jump target with it.
//
// Replay playback does not pump live network traffic.
DEFINE_HOOK(0x55D8E3, MainLoop_SkipIPXPumpDuringReplayPlayback, 0x5)
{
	if (ReplayState.Playback)
	{
		return 0x55D8E8;
	}

	return 0;
}

DEFINE_HOOK(0x55D25C, GameExit_FlushReplayBuffers, 0x6)
{
	StopReplaySystem();
	return 0;
}

DEFINE_HOOK(0x55CF13, GameExit_Sell_FlushReplayBuffers, 0x5)
{
	StopReplaySystem();
	return 0;
}

DEFINE_HOOK(0x6BEC60, Game_Exit_FlushReplayBuffers, 0x5)
{
	StopReplaySystem();
	return 0;
}

// TechnoClass::Create_Gap (thiscall, no stack args - hooked at the true entry, before any
// prologue runs, so 0x6FB460 - this function's own bare `retn` - is reachable directly).
// Whenever an enemy gap generator/jammer comes online, this walks every cell in its jam radius and,
// for each cell not allied with PlayerPtr, bumps that cell's ShroudCounter and GapsCoveringCell -
// directly re-shrouding patches of the map around itself from PlayerPtr's point of view. That is
// per-cell state, not the house-wide Visionary flag MaintainFullMapReveal watches, so an enemy gap
// generator re-fogs the ground around itself during full-map-reveal playback regardless of the
// Reset_Shroud fix above, and keeps doing it every time the generator's power flips back on (its
// own GeneratingGap latch only blocks re-entry while power stays up) - matching what was reported:
// fine until the first gap generator goes up, and recurring from then on as power fluctuates.
// Skip the function outright for the local player during full-map-reveal playback, so nothing can
// re-shroud PlayerPtr's view in the first place.
//
// This also skips the bookkeeping Create_Gap does for the player's own or an allied gap generator
// (the field_13C_gapgen coverage counter used by radar's jam-radius check, plus the unconditional
// RadarClass_Update_Map_/Flag_To_Redraw at the end) - not just the enemy-shroud effect this hook
// exists to suppress. Accepted deliberately: in full-map-reveal playback that bookkeeping only
// feeds a jam-radius indicator nobody is looking at with the whole map already shown, redraw state
// still catches up on the next unrelated frame, and a per-cell hook that preserves it would need
// to duplicate this function's own ally checks from a raw, pre-prologue entry point - meaningfully
// more risk for a cosmetic gap. Revisit with a hook inside the loop (skip only the write block
// gated on "not allied with PlayerPtr", landing on its own existing skip target loc_6FB3C1) if that
// indicator turns out to matter.
DEFINE_HOOK(0x6FB170, TechnoClass_CreateGap_SkipDuringFullRevealPlayback, 0x5)
{
	if (ReplayState.Playback && PlaybackWantsFullMapReveal())
		return 0x6FB460;

	return 0;
}

// Playback is not waiting on anyone, so the network delay budget never has to expire.
DEFINE_HOOK(0x647866, Queue_AI_Multiplayer_OverrideDelayTime, 0x5)
{
	if (ReplayState.Playback)
	{
		*reinterpret_cast<int*>(NETWORK_DELAY_TIME_ADDRESS) = std::numeric_limits<int>::max();
	}

	return 0;
}

// Force-complete the load-progress bar for playback, skirmish and campaign, instead of letting it
// animate. This is SessionClass::Callback's true entry point, but the function is also the generic
// progress-tick/pump callback threaded through the entire scenario-load pipeline (Read_Scenario_INI,
// Init_Theaters, MapGeneratorClass_*, Wait_For_Players, and more - confirmed via its xrefs), not
// just the multiplayer wait screen. A single call here just nudges progress toward a target and
// gets re-invoked repeatedly as loading proceeds, which - for any session without a real network
// peer pacing it - means the bar genuinely animates 0->100 over many real loading-screen frames.
//
// REVERTED to playback-only (2026-08-22): this was briefly widened to SessionClass::IsSingleplayer()
// (skirmish/campaign) on the theory that Sync_Delay (0x55E160) - called every loading-screen frame -
// routes GAME_SKIRMISH sessions through a real Sleep(FrameTimer.DelayTime) frame-rate cap, and that
// the animation was costing wall-clock time paying for it. That measured a real speedup (~3.3s ->
// ~1.1s on a live skirmish load), but the mechanism was wrong: Scenario_Load_Wait (0x684370), the
// actual driver of that Sync_Delay-paced loop, returns immediately for GAME_CAMPAIGN/GAME_SKIRMISH
// and never runs it. What this hook actually did instead: forcing PlayerProgresses[] directly means
// ProgressScreenClass_callback_643C50 never again sees current < target on a later call, so it never
// reaches its own redraw trigger (SendMessageA(hWnd, WM_PAINT, ...) or the fullscreen draw path
// ProgressScreenClass_LoadTextColor2) - the loading screen stops repainting entirely (reported as a
// black screen), and the speedup is a side effect of skipping those draws rather than an animation.
// The general loading slowdown is fixed in Misc/LoadingScreen.cpp by replacing the legacy fixed
// blit delay with an actual completion check. This direct completion remains playback-only because
// that path already accepts skipping the live progress animation.
DEFINE_HOOK(0x69AE90, WaitForPlayers_SkipProgressAnimation, 0x5)
{
	if (ReplayState.Playback && !ReplayState.ProgressBarForcedComplete)
	{
		ReplayState.ProgressBarForcedComplete = true;
		for (int i = 0; i < 8; ++i)
		{
			ProgressScreenClass::Instance.PlayerProgresses[i] = 100;
		}
	}

	return 0;
}

// SessionClass::Callback, at `cmp dword ptr [this+284Ch], 1` (loc_69AF0F) - the check the function
// itself uses to decide whether to run its per-player network-sync dance at all. offset 0x284C is
// SessionClass::Instance.StartSpots.Count (DynamicVectorClass<NodeNameType*>: vtable +0, Items +4,
// Capacity +8, IsAllocated +0xC, Count +0x10 -> 0x283C + 0x10 = 0x284C; StartSpots sits at 0x283C
// per YRpp/SessionClass.h, confirmed by the next field, unknown_2854, landing exactly one vector
// later). When Count == 1 it jumps straight to 0x69B14E and skips the whole dance; otherwise, once
// player 0's progress crosses 99.95%, it broadcasts a "guaranteed progress" message to every other
// player over IPX/NullModem and busy-waits (Sleep(20) in a loop) up to 240 SystemTimerClass ticks -
// about 4 seconds - for the send queue to drain below 5. Playback has no real peer to drain that
// queue, and the broadcast loop only sends anything when Players.ActiveCount > 1, so a multi-player
// replay's session takes the "do the dance" path and burns the full ~4 seconds - the load-in stall
// right after the progress bar reaches 100%. Forcing the skip-to-0x69B14E branch the game already
// takes for StartSpots.Count == 1 sessions removes exactly that stall.
DEFINE_HOOK(0x69AF0F, WaitForPlayers_ReplaySkipNetworkSyncDance, 0x7)
{
	if (ReplayState.Playback)
		return 0x69B14E;

	return 0;
}

// --- Side-channel recording taps --------------------------------------------------------------
// Chat, beacons and taunts bypass EventClass::DoList and are recorded here.
// Some local beacon calls use -1/-1; resolve them before writing the replay.
namespace
{

bool ResolveFlaggedBeaconSlot(int& house, int& slot)
{
	for (int h = 0; h < 8; ++h)
	{
		for (int s = 0; s < 3; ++s)
		{
			BeaconClass* pBeacon = BeaconManagerClass::Instance.Beacons[h][s];
			if (pBeacon && (pBeacon->Bitfield & BEACON_FLAG_LOCAL) != 0)
			{
				house = h;
				slot = s;
				return true;
			}
		}
	}

	return false;
}

} // namespace

// BeaconManagerClass::Place. __thiscall(ECX=this); stack (relative to ESP at entry, before the
// prologue's `sub esp` runs): +0 retaddr, +4 house, +8 coord.X, +0xC coord.Y, +0x10 coord.Z,
// +0x14 houseBeaconId (-1 = auto-assign a free slot).
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

// Beacon delete. Stack: +4 house, +8 slot; -1/-1 means the active local beacon.
DEFINE_HOOK(0x4311C0, BeaconPlacement_Delete_RecordDeletion, 0x6)
{
	if (ReplayState.Recording)
	{
		int house = R->Stack<int>(0x4);
		int slot = R->Stack<int>(0x8);

		bool shouldRecord = true;
		if (house == -1 && slot == -1)
			shouldRecord = ResolveFlaggedBeaconSlot(house, slot);

		if (shouldRecord)
			ReplaySystem::RecordBeaconDelete(house, slot);
	}

	return 0;
}

// Beacon text. Skip local compose previews; record final and remote-applied text.
// Stack: +4 text, +8 house, +0xC slot, +0x10 broadcast.
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
			shouldRecord = ResolveFlaggedBeaconSlot(house, slot);

		if (shouldRecord)
			ReplaySystem::RecordBeaconText(house, slot, text);
	}

	return 0;
}

// Taunts_752B70. __fastcall(ECX=command).
DEFINE_HOOK(0x752B70, Taunts_RecordPlayback, 0x5)
{
	if (ReplayState.Recording)
		ReplaySystem::RecordTaunt(R->ECX<int>());

	return 0;
}
