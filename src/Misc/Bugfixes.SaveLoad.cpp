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
#include <TechnoClass.h>
#include <TeleportLocomotionClass.h>
#include <TiberiumClass.h>
#include <TunnelLocomotionClass.h>

#include <algorithm>

// State that savegame loading reads out of the file and then writes over.
//
// AbstractClass::Load (0x410380) registers the saved pointer for swizzling and reads Size_Of()
// bytes straight into the object. Several derived Load overrides then run a constructor or a
// block of stores across part of what was just read. Some of that is necessary - vtables and COM
// interface pointers cannot survive serialisation and have to be repaired - but it also discards
// gameplay state that the file carried perfectly well.
//
// Everything in this file is that pattern, and every fix is the same shape: leave the loaded
// value alone. None of it changes the save format and all of it works on existing saves. See
// docs/save-load-correctness.md.

// Frame timers thrown away by savegame loading.
//
// A CDTimerClass is three words - the frame it was started on, an unused slot, and how long it has
// left to run - and every consumer in the engine reads it the same way:
//
//     if (StartTime != -1 && Frame - StartTime >= TimeLeft) fire;
//     if (TimeLeft == 0)                                    fire;
//
// Both branches are open on a timer holding { current frame, 0 }, so a timer left in that state
// fires on the next frame it is asked and keeps on firing. That is exactly what a load writes over
// seven timers it has just read out of the savegame: AbstractClass::Load (0x410380) reads Size_Of()
// bytes straight into the object, and the fixup code that runs afterwards resets them.
//
// The worst of the seven is the slave manager. SlaveManagerClass::AI (0x6AF5F0) does nothing at all
// unless its gate has expired, and re-arms it for ten frames when it fires, so every slave on the
// map scans for ore, harvests and respawns on one frame in ten. Resetting that gate both fires it
// immediately and re-phases it against the load rather than against the timeline being continued.
// The slaves then scan at different moments, pick different ore cells, take different paths and
// finish standing somewhere else - on the first frame after the load, every time, in any game with
// a slave miner in it.
//
// None of these timers needs anything added to the save format. The value is already in the file
// and already in the object by the time the reset happens, so the whole fix is to not write over
// it. See docs/save-load-correctness.md section 4.

#pragma region Timers reset through CDTimerClass::operator=

// CDTimerClass::operator=(int) at 0x46B640 is { StartTime = Frame; TimeLeft = argument; } and six
// load-time call sites invoke it with an argument of zero. They are intercepted here, in the one
// place they all pass through, rather than at six separate call sites: the argument was pushed by
// the caller and is cleaned up by this function's own retn 4, so declining to write is simply a
// matter of jumping to that instruction. Suppressing the call at the call site instead would leave
// the argument on the stack.
//
// Matching on the return address keeps it exact. This helper is also used by the drive and ship
// locomotors, by TActionClass, by SuperClass::Enable and by the veinhole monster, and none of those
// is a load and none of them is affected.
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

	// TiberiumClass::Load (0x721E80): whether ore is due to spread and grow. Tiberium_Spread_Logic
	// (0x7221B0) and Tiberium_Growth_Logic (0x722C40) read these before either queue is touched, so
	// a reset makes the ore field tick on the frame after every load whether it was due or not.
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

// Six bytes: mov edx, [esp+4] is four and mov eax, ecx is two, so the hook lands on an instruction
// boundary and a suppressed call falls through to the untouched remainder.
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

// SpawnManagerClass::Load (0x6B7F10) sets up the call above with
//
//     mov  ecx, Frame
//     push edi                  ; the argument, zero
//     mov  [ebx+50h], ecx       ; UpdateTimer.StartTime = Frame
//     lea  ecx, [ebx+5Ch]       ; the argument of the call that follows
//     mov  [ebx+58h], edi       ; UpdateTimer.TimeLeft  = 0
//     call CDTimerClass::operator=
//
// so the update gate is reset in passing, without going through the helper. The three instructions
// from 0x6B7F51 are replaced wholesale: the two stores are dropped and the lea they straddle is
// done here instead, leaving the call that follows with the ecx it expects.
static_assert(offsetof(SpawnManagerClass, UpdateTimer) == 0x50,
	"SpawnManagerClass::Load (0x6B7F10) resets the update timer inline at SpawnManagerClass+0x50");

DEFINE_HOOK(0x6B7F51, SpawnManagerClass_Load_KeepUpdateTimer, 0x9)
{
	GET(SpawnManagerClass* const, pManager, EBX);

	R->ECX(reinterpret_cast<DWORD>(&pManager->SpawnTimer));
	return 0x6B7F5A;
}

// HouseClass::Load (0x503040) reads the house whole and then runs the no-init constructor
// (0x4F5190) over it, whose only caller it is. Two of that constructor's stores are
//
//     mov ecx, Frame
//     mov [esi+280h], ecx       ; RepairTimer.StartTime = Frame
//     lea ecx, [esi+5500h]
//     mov [esi+288h], ebx       ; RepairTimer.TimeLeft  = 0
//
// which leave the timer describing something other than the Repairing gate it belongs to. The gate
// then clears on a different frame, a building repairs on a different tick, and an AI house can
// consume a different synchronised draw in BuildingClass::Repair_AI.
static_assert(offsetof(HouseClass, RepairTimer) == 0x280,
	"HouseClass::HouseClass(NoInitClass) (0x4F5190) resets the repair timer at HouseClass+0x280");

// Twelve bytes: mov ecx, Frame and mov [esi+280h], ecx, both six. The lea that follows is left
// alone because the constructor needs it.
DEFINE_HOOK(0x4F5327, HouseClass_NoInit_KeepRepairTimerStart, 0xC)
{
	return 0x4F5333;
}

// Six bytes, the store of TimeLeft on its own. ecx is already loaded for the call that follows and
// is carried through untouched.
DEFINE_HOOK(0x4F5339, HouseClass_NoInit_KeepRepairTimerLeft, 0x6)
{
	return 0x4F533F;
}

#pragma endregion Timers reset inline

#pragma region Locomotor members reconstructed after load

// LocomotionClass::Load (0x55AAC0) reads the whole locomotor, and four derived overrides then
// rebuild a member on top of it. The vtable repairs around them are needed; these are not.
//
// A locomotor is the part of a unit that owns where it is going and how far through getting
// there it is, so what these discard is a movement already in progress: a hovering unit part-way
// through a turn, a subterranean unit part-way through a dig, a chrono unit part-way through a
// warp, a rocket part-way through its climb. The unit restarts that step after a load, which
// takes a different number of frames and leaves it in a different place.

// FacingClass has no virtual functions - it is a desired facing, a start facing, a rotation timer
// and a rate - so the default constructor here repairs nothing at all. It only throws away a turn
// in progress.
static_assert(offsetof(HoverLocomotionClass, LocomotionFacing) == 0x30,
	"HoverLocomotionClass::Load (0x5170B0) reconstructs the facing at HoverLocomotionClass+0x30");

DEFINE_HOOK(0x5170DB, HoverLocomotionClass_Load_KeepFacing, 0x5)
{
	return 0x5170E0;
}

// Fifteen bytes: the frame fetch and the three stores that make up DigTimer - TimeLeft, StartTime
// and the rate. Nothing after them reads the registers they used.
static_assert(offsetof(TunnelLocomotionClass, DigTimer) == 0x28,
	"TunnelLocomotionClass::Load (0x72A150) resets the dig timer at TunnelLocomotionClass+0x28");

DEFINE_HOOK(0x72A177, TunnelLocomotionClass_Load_KeepDigTimer, 0xF)
{
	return 0x72A186;
}

// Fifteen bytes again, and deliberately fifteen rather than the twenty-two that reach the next
// vtable store. The store immediately after this range clears Piggybackee, which is a live
// ILocomotion pointer rather than gameplay state and genuinely cannot survive serialisation, so
// it is left to run.
static_assert(offsetof(TeleportLocomotionClass, Timer) == 0x3C,
	"TeleportLocomotionClass::Load (0x719CA0) resets the timer at TeleportLocomotionClass+0x3C");
static_assert(offsetof(TeleportLocomotionClass, Piggybackee) == 0x48,
	"TeleportLocomotionClass::Load (0x719CA0) clears the piggyback pointer just past the timer");

DEFINE_HOOK(0x719CC9, TeleportLocomotionClass_Load_KeepTimer, 0xF)
{
	return 0x719CD8;
}

// Sixteen bytes: the frame fetch and the two stores that make up TrailerTimer.
static_assert(offsetof(RocketLocomotionClass, TrailerTimer) == 0x34,
	"RocketLocomotionClass::Load (0x663410) resets the trailer timer at RocketLocomotionClass+0x34");

DEFINE_HOOK(0x663435, RocketLocomotionClass_Load_KeepTrailerTimer, 0x10)
{
	return 0x663445;
}

#pragma endregion Locomotor members reconstructed after load

// Defined with the rest of the passability work further down; the load-path call site is here.
void RecomputeAllCellPassability();

#pragma region Scenario randomiser and unique-ID counter

// ScenarioClass::Load (0x689470) reads 0x3740 bytes of scenario at 0x6894B7 and then runs
// ScenarioClass::ScenarioClass (0x683560) over the result at 0x6894C5. That constructor's first
// act is Random2Class::Random2Class(&RandomNumber, 0), which re-seeds the synchronised randomiser
// to a fixed seed and throws away the state the file just supplied.
//
// Restoring the two visible indices is not enough. The randomiser owns a shuffle table as well as
// its cursor, and all of it decides the future sequence, so the whole object is carried across.
//
// The unique-ID counter is taken here as well, because this is the only moment it holds the value
// the file recorded. It is put back at the end of the load; see below.
bool HaveScenarioLoadState = false;
Randomizer LoadedScenarioRandom {};
int LoadedScenarioUniqueID = 0;

DEFINE_HOOK(0x6894C5, ScenarioClass_Load_TakeRandomiserAndCounter, 0x5)
{
	GET(ScenarioClass* const, pScen, ECX);

	if (pScen)
	{
		LoadedScenarioRandom = pScen->Random;
		LoadedScenarioUniqueID = pScen->UniqueID;
		HaveScenarioLoadState = true;
	}

	return 0;
}

// The instruction the constructor returns to. It is also the target of the branch that skips the
// constructor entirely when the scenario pointer is null, which is why the flag above exists
// rather than a bare copy back.
DEFINE_HOOK(0x6894CA, ScenarioClass_Load_PutRandomiserBack, 0x6)
{
	GET(ScenarioClass* const, pScen, EBP);

	if (HaveScenarioLoadState && pScen)
		pScen->Random = LoadedScenarioRandom;

	return 0;
}

// AbstractClass::Create_ID (0x410230) consumes ScenarioClass::UniqueID through
// Increment_UniqueID (0x68BCB0), and the class factory that rebuilds the saved object graph goes
// through the ordinary constructors, so every object the load creates takes an ID. Each one's own
// Load then replaces that temporary ID with the saved one, but nothing gives the counter back:
// tens of thousands of IDs are consumed by a load, cells included, and the drift is a function of
// how much was in the save rather than any fixed amount.
//
// The counter is put back once the whole load has finished - after Init_Scenario_stuff and the
// tiberium initialisation, which construct objects of their own - so nothing that runs during the
// load can push it forward again.
//
// It is validated rather than trusted. Increment_UniqueID hands out the incremented value, so the
// counter is the last ID issued and must not be below any ID now in the world; a post-load routine
// that legitimately created something new has to keep its ID. Taking the higher of the two is
// correct in both directions and can never hand out an ID twice.
DEFINE_HOOK(0x67E6BD, LoadGame_PutUniqueIDCounterBack, 0x5)
{
	// Everything the load builds is in place by here - the object graph, the map, the tiberium
	// initialisation - so this is the last chance to correct the cells and the first point at which
	// their occupants are all known.
	RecomputeAllCellPassability();

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

// The flag is serialised with the techno, but MapClass::_clip_map (0x567230) - reached from
// Init_Scenario_stuff while loading - assigns it from whether the techno is inside the playable
// map right now:
//
//     bl = techno->IsInPlayfield;
//     al = Map.In_Radar(techno->Get_Cell());
//     techno->IsInPlayfield = al;
//     if (!bl && al) { ... it has just entered ... }
//
// Ordinary play never clears it. Per-cell processing sets it once a techno enters the map and
// leaves it set when the object later leaves again, so it means "has been in play", not "is
// inside the map". AircraftClass::Should_Delete_Off_Map (0x41B890) relies on that: an off-map
// aircraft whose flag is false is one that has not arrived yet and must not be removed. A spy
// plane that had flown in and was on its way out when the game was saved comes back from the load
// looking like it had never arrived, so it is never removed and keeps flying.
//
// Making the assignment monotonic restores the historical meaning without needing to know whether
// a load is in progress. It only changes the case the engine has no business changing - was
// inside, is now outside - and leaves the just-entered branch below reading the same bl and al it
// always did. On a fresh scenario every techno starts with the flag clear, so monotonic and
// assignment agree and map setup is untouched.
static_assert(offsetof(TechnoClass, IsInPlayfield) == 0x3D5,
	"MapClass::_clip_map (0x567230) assigns the playfield flag at TechnoClass+0x3D5");

DEFINE_HOOK(0x56730E, MapClass_ClipMap_KeepIsInPlayfield, 0x6)
{
	GET(TechnoClass* const, pTechno, ESI);

	if ((R->EAX() & 0xFF) != 0)
		pTechno->IsInPlayfield = true;

	// The comparison of the old flag against the new one is already in the flags register and the
	// branch that reads it is the next instruction, so nothing here may disturb either.
	return 0x567314;
}

#pragma endregion TechnoClass::IsInPlayfield

#pragma region Cell passability

// The vanilla cell stream does not carry passability - the matching Tiberian Sun source calls it
// derived state - and CellClass::Load (0x4839F0) leaves every cell reading PASSABLE_OK.
//
// The map's own copy is rebuilt correctly: MapClass::Update_PassabilityType (0x56C510) runs
// during MapClass_LevelAndPassability_567110 and fills MapClass::LevelAndPassability from the
// world. What nothing puts back is the field on the cell itself, and that is the one the
// cell-level pathfinder reads. It is wrong everywhere, and immediately and visibly wrong on the
// isometric border, where CellClass::Check_Passability (0x483C80) opens with
//
//     if (!Map.In_Radar(Position, true)) { Passability = PASSABLE_OUTSIDE; return; }
//
// so thousands of cells outside the playable area come back walkable. Routes can be planned
// through them.
//
// Check_Passability is the engine's own answer for one cell, derived from overlay, land type and
// whatever is standing there, so the fix is to ask it for every cell once the world is whole.
//
// It is asked twice, in two places, because neither alone covers both cases:
//
//  - At the entry to MapClass_LevelAndPassability_567110 (0x567110), which rebuilds the map's own
//    passability copy and all three levels of the subzone graph from these cells. Recomputing there
//    means that rebuild is fed correct input rather than repaired afterwards. That function is
//    reached only from MapClass::Set_Map_Dimensions (0x565C10) and never per frame.
//
//  - At the end of Load_Game (0x67E440). The first attempt at this fix used only the hook above, on
//    the strength of the call chain in the notes - and it never fired once, because
//    Init_Scenario_stuff (0x685120) does not call Set_Map_Dimensions at all. It initialises the
//    type heaps, resets A*, re-clips the radar and returns. A load therefore never reaches
//    0x567110, the cells stayed as CellClass::Load left them, and the divergence came straight
//    back. This second call site is on the load path by construction.
//
// Both run for ordinary map setup as well as for a load. That is deliberate and harmless: the
// answer for a cell is a function of the cell, so asking more often than the engine does either
// agrees with what is already there or corrects it.
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

// Five bytes: three pushes and the two-byte move that follows them.
DEFINE_HOOK(0x567110, MapClass_LevelAndPassability_RecomputeCellPassability, 0x5)
{
	RecomputeAllCellPassability();
	return 0;
}

#pragma endregion Cell passability
