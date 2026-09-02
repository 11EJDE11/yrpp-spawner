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
#include <BulletClass.h>
#include <HouseClass.h>
#include <SlaveManagerClass.h>
#include <SpawnManagerClass.h>
#include <TiberiumClass.h>

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
