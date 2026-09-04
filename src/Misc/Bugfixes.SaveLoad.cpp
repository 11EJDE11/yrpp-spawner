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

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>
#include <AbstractClass.h>
#include <BulletClass.h>
#include <HouseClass.h>
#include <HoverLocomotionClass.h>
#include <RocketLocomotionClass.h>
#include <ScenarioClass.h>
#include <SlaveManagerClass.h>
#include <SpawnManagerClass.h>
#include <CellClass.h>
#include <MapClass.h>
#include <ParasiteClass.h>
#include <TechnoClass.h>
#include <TeleportLocomotionClass.h>
#include <TiberiumClass.h>
#include <TunnelLocomotionClass.h>

#include <algorithm>

// Preserve serialized gameplay state that vanilla load initialization overwrites.

#pragma region Timers reset through CDTimerClass::operator=

constexpr DWORD TimerResetsToSuppress[] =
{
	// BulletClass::Load (0x46AE70): the two flight timers of a projectile in the air.
	0x46AEAB, // Data.UnknownTimer
	0x46AEB5, // Data.ArmTimer

	// SlaveManagerClass::Load (0x6B1170): the ten-frame gate on all slave logic.
	0x6B11C5,

	// SpawnManagerClass::Load (0x6B7F10): the respawn timer for a carrier's aircraft. The update
	// gate beside it is written inline rather than through this helper; see below.
	0x6B7F5F,

	0x721FBB, // SpreadLogic.Timer
	0x721FC7, // GrowthLogic.Timer
};

// The offsets the six call sites write through, so a YRpp layout change is a build error here
// rather than six silently misplaced timers at runtime.
static_assert(offsetof(BulletClass, Data) == 0xB8,
	"BulletClass::Load (0x46AE70) resets both flight timers at BulletClass+0xB8");
static_assert(offsetof(SlaveManagerClass, RespawnTimer) == 0x50,
	"SlaveManagerClass::AI (0x6AF5F0) reads its gate at SlaveManagerClass+0x50");
static_assert(offsetof(SpawnManagerClass, SpawnTimer) == 0x5C,
	"SpawnManagerClass::Load (0x6B7F10) resets the spawn timer at SpawnManagerClass+0x5C");
static_assert(offsetof(TiberiumClass, SpreadLogic) + offsetof(TiberiumLogic, Timer) == 0x100,
	"TiberiumClass::Load (0x721E80) resets the spread timer at TiberiumClass+0x100");
static_assert(offsetof(TiberiumClass, GrowthLogic) + offsetof(TiberiumLogic, Timer) == 0x11C,
	"TiberiumClass::Load (0x721E80) resets the growth timer at TiberiumClass+0x11C");

// The tail of CDTimerClass::operator=, which is its retn 4. Reaching it without having written
// anything leaves the timer as the load found it and still balances the caller's argument.
constexpr DWORD CDTimerClass_operator_assign_Return = 0x46B651;

DEFINE_HOOK(0x46B640, CDTimerClass_Assign_KeepValueAcrossLoad, 0x6)
{
	const DWORD caller = R->Stack<DWORD>(0x0);

	for (const DWORD suppressed : TimerResetsToSuppress)
	{
		if (caller == suppressed)
			return CDTimerClass_operator_assign_Return;
	}

	return 0;
}

#pragma endregion Timers reset through CDTimerClass::operator=

#pragma region Timers reset inline

static_assert(offsetof(SpawnManagerClass, UpdateTimer) == 0x50,
	"SpawnManagerClass::Load (0x6B7F10) resets the update timer inline at SpawnManagerClass+0x50");

DEFINE_HOOK(0x6B7F51, SpawnManagerClass_Load_KeepUpdateTimer, 0x9)
{
	GET(SpawnManagerClass* const, pManager, EBX);

	R->ECX(reinterpret_cast<DWORD>(&pManager->SpawnTimer));
	return 0x6B7F5A;
}

static_assert(offsetof(HouseClass, RepairTimer) == 0x280,
	"HouseClass::HouseClass(NoInitClass) (0x4F5190) resets the repair timer at HouseClass+0x280");

DEFINE_HOOK(0x4F5327, HouseClass_NoInit_KeepRepairTimerStart, 0xC)
{
	return 0x4F5333;
}

// Preserve the loaded repair timer's remaining time.
DEFINE_HOOK(0x4F5339, HouseClass_NoInit_KeepRepairTimerLeft, 0x6)
{
	return 0x4F533F;
}

#pragma endregion Timers reset inline

#pragma region Locomotor members reconstructed after load

// Keep serialized movement state instead of reconstructing it after load.
static_assert(offsetof(HoverLocomotionClass, LocomotionFacing) == 0x30,
	"HoverLocomotionClass::Load (0x5170B0) reconstructs the facing at HoverLocomotionClass+0x30");

DEFINE_HOOK(0x5170DB, HoverLocomotionClass_Load_KeepFacing, 0x5)
{
	return 0x5170E0;
}

static_assert(offsetof(TunnelLocomotionClass, DigTimer) == 0x28,
	"TunnelLocomotionClass::Load (0x72A150) resets the dig timer at TunnelLocomotionClass+0x28");

DEFINE_HOOK(0x72A177, TunnelLocomotionClass_Load_KeepDigTimer, 0xF)
{
	return 0x72A186;
}

static_assert(offsetof(TeleportLocomotionClass, Timer) == 0x3C,
	"TeleportLocomotionClass::Load (0x719CA0) resets the timer at TeleportLocomotionClass+0x3C");
static_assert(offsetof(TeleportLocomotionClass, Piggybackee) == 0x48,
	"TeleportLocomotionClass::Load (0x719CA0) clears the piggyback pointer just past the timer");

DEFINE_HOOK(0x719CC9, TeleportLocomotionClass_Load_KeepTimer, 0xF)
{
	return 0x719CD8;
}

static_assert(offsetof(RocketLocomotionClass, TrailerTimer) == 0x34,
	"RocketLocomotionClass::Load (0x663410) resets the trailer timer at RocketLocomotionClass+0x34");

DEFINE_HOOK(0x663435, RocketLocomotionClass_Load_KeepTrailerTimer, 0x10)
{
	return 0x663445;
}

static_assert(offsetof(ParasiteClass, SuppressionTimer) == 0x2C,
	"ParasiteClass::Load (0x6295B0) resets the suppression timer at ParasiteClass+0x2C");
static_assert(offsetof(ParasiteClass, DamageDeliveryTimer) == 0x38,
	"ParasiteClass::Load (0x6295B0) resets the damage delivery timer at ParasiteClass+0x38");

DEFINE_HOOK(0x6295DB, ParasiteClass_Load_KeepTimers, 0x6)
{
	return 0x6295FA;
}

#pragma endregion Locomotor members reconstructed after load

// Defined with the rest of the passability work further down; the load-path call site is here.
void RecomputeAllCellPassability();

#pragma region Unique IDs thrown away by the AbstractClass constructor

// Restore saved object IDs after load constructors clear them.
AbstractClass* ConstructAbstractBase(AbstractClass* pAbstract)
{
	using Constructor = AbstractClass* (__thiscall*)(AbstractClass*);
	return reinterpret_cast<Constructor>(0x410170)(pAbstract);
}

void ConstructAbstractBaseKeepingUniqueID(AbstractClass* pAbstract)
{
	if (!pAbstract)
		return;

	const DWORD uniqueID = pAbstract->UniqueID;
	ConstructAbstractBase(pAbstract);
	pAbstract->UniqueID = uniqueID;
}

static_assert(offsetof(AbstractClass, UniqueID) == 0x10,
	"AbstractClass::AbstractClass (0x410170) writes the unique ID at AbstractClass+0x10");

DEFINE_HOOK(0x6CDF0E, SuperClass_Load_KeepUniqueID, 0x5)
{
	GET(AbstractClass* const, pSuper, ESI);

	ConstructAbstractBaseKeepingUniqueID(pSuper);
	return 0x6CDF13;
}

DEFINE_HOOK(0x7281BA, TubeClass_Load_KeepUniqueID, 0x5)
{
	GET(AbstractClass* const, pTube, ESI);

	ConstructAbstractBaseKeepingUniqueID(pTube);
	return 0x7281BF;
}

#pragma endregion Unique IDs thrown away by the AbstractClass constructor

#pragma region Scenario randomiser and unique-ID counter

// Preserve the saved randomizer and ID counter across scenario reconstruction.
bool HaveScenarioLoadState = false;
Randomizer LoadedScenarioRandom {};
int LoadedScenarioUniqueID = 0;

bool SaveGameLoadInProgress = false;

DEFINE_HOOK(0x6894C5, ScenarioClass_Load_TakeRandomiserAndCounter, 0x5)
{
	GET(ScenarioClass* const, pScen, ECX);

	SaveGameLoadInProgress = true;

	if (pScen)
	{
		LoadedScenarioRandom = pScen->Random;
		LoadedScenarioUniqueID = pScen->UniqueID;
		HaveScenarioLoadState = true;
	}

	return 0;
}

DEFINE_HOOK(0x6894CA, ScenarioClass_Load_PutRandomiserBack, 0x6)
{
	GET(ScenarioClass* const, pScen, EBP);

	if (HaveScenarioLoadState && pScen)
		pScen->Random = LoadedScenarioRandom;

	return 0;
}

DEFINE_HOOK(0x67E6BD, LoadGame_PutUniqueIDCounterBack, 0x5)
{
	RecomputeAllCellPassability();

	// Cleared before the early returns below, so the window closes whatever else this load did.
	SaveGameLoadInProgress = false;

	if (!HaveScenarioLoadState || !ScenarioClass::Instance)
		return 0;

	HaveScenarioLoadState = false;

	int highest = 0;
	for (int i = 0; i < AbstractClass::Array.Count; ++i)
	{
		if (const auto* const pAbstract = AbstractClass::Array.Items[i])
			highest = std::max(highest, static_cast<int>(pAbstract->UniqueID));
	}

	const int wanted = std::max(LoadedScenarioUniqueID, highest);
	const int loaded = ScenarioClass::Instance->UniqueID;
	if (loaded == wanted)
		return 0;

	Debug::Log("[SaveLoad] The load advanced the scenario unique ID counter from %d to %d; putting "
		"it back to %d.\n", LoadedScenarioUniqueID, loaded, wanted);

	ScenarioClass::Instance->UniqueID = wanted;
	return 0;
}

#pragma endregion Scenario randomiser and unique-ID counter

#pragma region TechnoClass::IsInPlayfield

// Keep the historical in-playfield flag when loading an object outside the map.
static_assert(offsetof(TechnoClass, IsInPlayfield) == 0x3D5,
	"MapClass::_clip_map (0x567230) assigns the playfield flag at TechnoClass+0x3D5");

DEFINE_HOOK(0x56730E, MapClass_ClipMap_KeepIsInPlayfield, 0x6)
{
	GET(TechnoClass* const, pTechno, ESI);

	const bool isInPlayfield = (R->EAX() & 0xFF) != 0;

	if (isInPlayfield || !SaveGameLoadInProgress)
		pTechno->IsInPlayfield = isInPlayfield;

	return 0x567314;
}

#pragma endregion TechnoClass::IsInPlayfield

#pragma region Cell passability

// Recompute cell passability after the map has been reconstructed.
void CheckCellPassability(CellClass* pCell)
{
	using CheckPassability = void(__thiscall*)(CellClass*);
	reinterpret_cast<CheckPassability>(0x483C80)(pCell);
}

void RecomputeAllCellPassability()
{
	// Called during the very first map setup as well, when there may be no cell array yet.
	if (!MapClass::Instance.Cells.Items)
		return;

	const int count = std::min(MapClass::Instance.MaxNumCells, MapClass::MaxCells);
	for (int i = 0; i < count; ++i)
	{
		if (auto* const pCell = MapClass::Instance.Cells[i])
			CheckCellPassability(pCell);
	}
}

DEFINE_HOOK(0x567110, MapClass_LevelAndPassability_RecomputeCellPassability, 0x5)
{
	RecomputeAllCellPassability();
	return 0;
}

#pragma endregion Cell passability
