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

#include "Spawner.h"
#include "NetHack.h"
#include <Replay/ReplaySeek.h>
#include <Replay/ReplaySystem.h>
#include <UI/DesyncDialog.h>

#include <HouseClass.h>
#include <SessionClass.h>
#include <BeaconManagerClass.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>
#include <Unsorted.h>
#include <CCINIClass.h>

DEFINE_HOOK(0x6BD7C5, WinMain_SpawnerInit, 0x6)
{
	if (Spawner::Enabled)
	{
		Spawner::Init();


		Patch::Apply_CALL(0x48CDD3, Spawner::StartGame); // Main_Game
		Patch::Apply_CALL(0x48CFAA, Spawner::StartGame); // Main_Game

		{ // HousesStuff
			Patch::Apply_CALL(0x68745E, Spawner::AssignHouses); // Read_Scenario_INI
			Patch::Apply_CALL(0x68ACFF, Spawner::AssignHouses); // ScenarioClass::Read_INI

			Patch::Apply_LJMP(0x5D74A0, 0x5D7570);   // MPGameModeClass_AllyTeams
			Patch::Apply_LJMP(0x501721, 0x501736);   // HouseClass_ComputerParanoid
			//Patch::Apply_LJMP(0x686A9E, 0x686AC6); // ReadScenario_InitSomeThings - Moved to a hook to allow conditional toggling of Special house's alliances.
		}

		{ // NetHack
			Patch::Apply_CALL(0x7B3D75, NetHack::SendTo);   // UDPInterfaceClass::Message_Handler
			Patch::Apply_CALL(0x7B3EEC, NetHack::RecvFrom); // UDPInterfaceClass::Message_Handler
		}

		{ // Skip Intro, EA_WWLOGO and LoadScreen
			Patch::Apply_LJMP(0x52CB50, 0x52CB6E); // InitIntro_Skip
			Patch::Apply_LJMP(0x52C5E0, 0x52C5F8); // InitGame_SkipLogoAndLoadScreen
		}

		{ // Cooperative
			Patch::Apply_LJMP(0x553321, 0x5533C5); // LoadProgressMgr_Draw_CooperativeDescription
			Patch::Apply_LJMP(0x55D0DF, 0x55D0E8); // AuxLoop_Cooperative_EndgameCrashFix
		}

		// Set ConnTimeout, for the wait at the start and the one restarted whenever anyone's loading
		// progress changes (otherwise a fixed 7200 ticks, so a player who never loads held everyone for two minutes)
		Patch::Apply_TYPED<int>(0x6843C7, { Spawner::GetConfig()->ConnTimeout }); // Scenario_Load_Wait
		Patch::Apply_TYPED<int>(0x684484, { Spawner::GetConfig()->ConnTimeout }); // Scenario_Load_Wait, on progress

		// While the game waits for a player, the FRAMESYNC that tells them where it is goes out only
		// every 180 ticks (3 s). Nothing else is sent while stalled, so once a loss burst ends a
		// stalled pair could sit for up to 3 s more. Resend every 10 ticks instead; it is a few bytes.
		Patch::Apply_TYPED<int>(0x648061, { 10 }); // Queue_AI_Multiplayer, Wait_For_Players resend_delta

		// Show GameMode in DiplomacyDialog in Skirmish
		Patch::Apply_LJMP(0x658117, 0x658126); // RadarClass_DiplomacyDialog

		// Allow surrendering to observe even when only one human is still playing
		Patch::Apply_LJMP(0x4F1993, 0x4F19C4); // AbortMissionDialog_Init_SkipHumanCount

		// Skip load *.PKT, *.YRO and *.YRM map files
		Patch::Apply_LJMP(0x699AD9, 0x69A1B2); // SessionClass::Read_Scenario_Descriptions
	}

	return 0;
}

// Display UIGameMode if is set
// Otherwise use mode name from MPModesMD.ini
DEFINE_HOOK(0x65812E, RadarClass__DiplomacyDialog_UIGameMode, 0x6)
{
	enum { Show = 0x65813E, DontShow = 0x65814D };

	if (Spawner::Enabled && Spawner::GetConfig()->UIGameMode[0])
	{
		R->EBX(R->EAX());
		R->EAX(Spawner::GetConfig()->UIGameMode);
		return Show;
	}

	if (!SessionClass::Instance.MPGameMode)
		return DontShow;

	return 0;
}

// Clear UIGameMode on game load
DEFINE_HOOK(0x689669, ScenarioClass_Load_Suffix, 0x6)
{
	if (Spawner::Enabled)
		Spawner::GetConfig()->UIGameMode[0] = 0;

	return 0;
}

#pragma region MPlayerDefeated
namespace MPlayerDefeated
{
	HouseClass* pThis = nullptr;
}

DEFINE_HOOK(0x4FC0B6, HouseClass__MPlayerDefeated_SaveArgument, 0x5)
{
	MPlayerDefeated::pThis = (Spawner::Enabled && !SessionClass::IsCampaign())
		? R->ECX<HouseClass*>()
		: nullptr;

	return 0;
}

// Skip match-end logic if MPlayerDefeated called for observer
DEFINE_HOOK_AGAIN(0x4FC332, HouseClass__MPlayerDefeated_SkipObserver, 0x5)
DEFINE_HOOK(0x4FC262, HouseClass__MPlayerDefeated_SkipObserver, 0x6)
{
	enum { ProcEpilogue = 0x4FC6BC };

	if (!MPlayerDefeated::pThis)
		return 0;

	return MPlayerDefeated::pThis->IsInitiallyObserver()
		? ProcEpilogue
		: 0;
}

DEFINE_HOOK(0x4FC551, HouseClass__MPlayerDefeated_NoEnemies, 0x5)
{
	enum { ProcEpilogue = 0x4FC6BC };

	if (!MPlayerDefeated::pThis)
		return 0;

	for (const auto pHouse : HouseClass::Array)
	{
		if (pHouse->Defeated || pHouse == MPlayerDefeated::pThis || pHouse->Type->MultiplayPassive)
			continue;

		if ((pHouse->IsHumanPlayer || Spawner::GetConfig()->ContinueWithoutHumans) && pHouse->IsMutualAlly(MPlayerDefeated::pThis))
		{
			Debug::Log("MPlayer_Defeated() - Defeated player has a living ally");
			if (Spawner::GetConfig()->DefeatedBecomesObserver)
				MPlayerDefeated::pThis->MakeObserver();

			return ProcEpilogue;
		}
	}

	return 0;
}

DEFINE_HOOK(0x4FC57C, HouseClass__MPlayerDefeated_CheckAliveAndHumans, 0x7)
{
	enum { ProcEpilogue = 0x4FC6BC, FinishMatch = 0x4FC591 };

	if (!MPlayerDefeated::pThis)
		return 0;

	GET_STACK(int, numHumans, STACK_OFFSET(0xC0, -0xA8));
	GET_STACK(int, numAlive, STACK_OFFSET(0xC0, -0xAC));

	bool continueWithoutHumans = Spawner::GetConfig()->ContinueWithoutHumans
		|| MPlayerDefeated::pThis->IsInitiallyObserver();

	if (!continueWithoutHumans && !MPlayerDefeated::pThis->IsHumanPlayer)
	{
		bool isHasAliveHumanPlayers = false;
		for (const auto pHouse : HouseClass::Array)
		{
			if (pHouse->IsHumanPlayer && !pHouse->Defeated)
			{
				isHasAliveHumanPlayers = true;
				break;
			}
		}

		if (!isHasAliveHumanPlayers)
			continueWithoutHumans = true;
	}

	if (numAlive > 1 && (numHumans != 0 || continueWithoutHumans))
	{
		if (Spawner::GetConfig()->DefeatedBecomesObserver)
			MPlayerDefeated::pThis->MakeObserver();

		return ProcEpilogue;
	}

	return FinishMatch;
}

// Leaves the bottom bar closed for a defeated local player. When the local player loses, the game
// closes the bar (Toggle_Thumb) and sets a flag (0x824410) that nothing clears, so every later defeat
// that ends the match reopens it (Init_Thumb). Each reopen links the command bar buttons into the
// gadget list again, so the list loops back on itself and the game hangs - e.g. when the local player
// and another house are both defeated in the last frames. Skipping it is the engine's own path when
// the flag is clear. The hook above returns here too when the match ends.
DEFINE_HOOK(0x4FC591, HouseClass__MPlayerDefeated_SkipThumbRestore, 0x5)
{
	enum { SkipThumbRestore = 0x4FC5AA };

	return (Spawner::Enabled || ReplaySystem::IsSpectatorPlayback())
		? SkipThumbRestore
		: 0;
}

#pragma endregion MPlayerDefeated

#pragma region Save&Load

DEFINE_HOOK_AGAIN(0x624271, SomeFunc_InterceptMainLoop, 0x5);
DEFINE_HOOK_AGAIN(0x623D72, SomeFunc_InterceptMainLoop, 0x5);
DEFINE_HOOK_AGAIN(0x62314E, SomeFunc_InterceptMainLoop, 0x5);
DEFINE_HOOK_AGAIN(0x60D407, SomeFunc_InterceptMainLoop, 0x5);
DEFINE_HOOK_AGAIN(0x608206, SomeFunc_InterceptMainLoop, 0x5);
DEFINE_HOOK(0x48CE8A, SomeFunc_InterceptMainLoop, 0x5)
{
	// A save the game master chose after a desync is loaded between frames.
	if (DesyncDialog.Has_Pending_Reload())
		DesyncDialog.Perform_Pending_Reload();

	//  Main loop.
	Game::MainLoop();
	ReplaySystem::Seek::SetRecordingSimulationInProgress(false);

	// After loop.
	if (Spawner::Enabled)
		Spawner::After_Main_Loop();

	return R->Origin() + 0x5;
}

DEFINE_HOOK(0x52DAEF, Game_Start_ResetGlobal, 0x5)
{
	Spawner::DoSave = false;
	Spawner::NextAutoSaveFrame = -1;
	Spawner::NextAutoSaveNumber = 0;
	ReplaySystem::OnGameStartReset();
	return 0;
}

DEFINE_HOOK(0x686B20, INIClass_ReadScenario_AutoSave, 0x6)
{
	if (!Spawner::Enabled)
		return 0;

	/**
	 *  Schedule the next autosave.
	 */
	Spawner::NextAutoSaveFrame = Unsorted::CurrentFrame;
	Spawner::NextAutoSaveFrame += Spawner::GetConfig()->AutoSaveInterval;
	return 0;
}

// Do not change the address without adjusting Phobos handling
// and reading the comments in Spawner::After_Main_Loop
DEFINE_HOOK(0x4C7A14, EventClass_RespondToEvent_SaveGame, 0x5)
{
	if (!Spawner::Enabled)
		return 0;

	Spawner::RespondToSaveGame();
	return 0x4C7B42;
}

// for some reason beacons are only inited on scenario init, which doesn't happen on load
DEFINE_HOOK(0x67E6DA, LoadGame_AfterInit, 0x6)
{
	BeaconManagerClass::Instance.LoadArt();
	return 0;
}

// Loading a multiplayer save starts at full progress, so SessionClass's progress callback sends
// every update as a guaranteed message, then waits up to four seconds each time for the global send
// queue to drain to five. Whoever starts loading first sends before the others are listening, and
// the others stop acknowledging global messages once they reach the frame sync, so the first player
// stalls on every callback - 10 to 20 seconds - while everyone else waits. The messages only drive
// the loading bars and stay queued; the frame sync is what holds the start, so skip the wait.
DEFINE_HOOK(0x69B01A, SessionClass_Callback_NoProgressWaitOnLoad, 0x5)
{
	enum { SkipWait = 0x69B148 };

	return Spawner::Enabled && Spawner::GetConfig()->LoadSaveGame
		? SkipWait
		: 0;
}

#pragma endregion

DEFINE_HOOK(0x686A9E, ReadScenario_InitSomeThings_SpecialHouseIsAlly, 0x6)
{
	if (!Spawner::Enabled)
		return 0;

	if (Spawner::GetConfig()->SpecialHouseIsAlly)
		return 0;

	return 0x686AC6;
}

DEFINE_HOOK(0x686D46, ReadScenarioINI_MissionININame, 0x5)
{
	if (!Spawner::Enabled)
		return 0;

	LEA_STACK(CCFileClass*, pFile, STACK_OFFSET(0x174, -0xF0));

	if (Spawner::GetConfig()->ReadMissionSection)
	{
		pFile->SetFileName("SPAWN.INI");
		return 0x686D57;
	}

	return 0;
}

DEFINE_HOOK(0x65F57F, BriefingDialog_MissionININame, 0x6)
{
	if (!Spawner::Enabled)
		return 0;

	LEA_STACK(CCFileClass*, pFile, STACK_OFFSET(0x1D4, -0x16C));

	if (Spawner::GetConfig()->ReadMissionSection)
	{
		pFile->SetFileName("SPAWN.INI");
		return 0x65F58F;
	}

	return 0;
}
