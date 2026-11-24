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

#include <Utilities/Macro.h>

#include <EventClass.h>
#include <FootClass.h>
#include <PlanningTokenClass.h>
#include <TechnoClass.h>

// PlanningTokenClass::Remove_Node (0x636B50) removes a unit's last planned order without
// removing it from ActiveRouteOwners or decrementing HouseRouteCounts, as other route clears do.
// The unit is left in the list and becomes a dangling pointer when it dies. Nothing in the
// simulation reads either, so this cannot desync.
DEFINE_HOOK(0x636CCC, PlanningTokenClass_RemoveNode_ClearActiveOwner, 0x6)
{
	GET(PlanningTokenClass* const, pToken, EDI);

	auto* const pOwner = pToken->OwnerUnit;
	if (pToken->PlanningNodes.Count == 0 && pOwner
		&& PlanningTokenClass::ActiveRouteOwners.FindItemIndex(pOwner) != -1)
	{
		// The game's own clean-up for an emptied route, otherwise unused: decrements the count for
		// the same house the other clear paths use and removes the owner from the list.
		reinterpret_cast<void(__thiscall*)(TechnoClass*)>(0x6377B0u)(pOwner);
	}

	return 0;
}

// A unit starting its next planned order is put in Wait before the order is sent
// (Planning_6385C0), and the order's arrival is what takes it out again. EventClass::Execute
// refuses a MegaMission whose target or destination is dead or in limbo (0x4C723C, 0x4C7278) -
// a dog mid-leap, say - so the unit is left in Wait for good: it never auto-targets and never
// goes idle again until the player gives it a new order. When that is about to happen, do what
// going idle would have done instead: start the next planned order, or return to guard.
namespace
{
	bool RefusesObject(TargetClass& target, unsigned char rtti)
	{
		if (!rtti)
			return false;

		auto* const pObject = target.As_Object();
		return pObject && (!pObject->IsAlive || pObject->Health == 0 || pObject->InLimbo);
	}
}

// After the house check (0x4C71D2, hardened builds), vanilla's checks on the unit itself, and
// ClearPlanningTokens, which has cleared the token's "order sent" flag; just before the checks
// on the target and destination. MEGAMISSION_F shares this code with a different layout.
DEFINE_HOOK(0x4C7225, EventClass_Execute_RefusedPlannedOrderLeavesWait, 0x5)
{
	enum { DoNothing = 0x4C8109 };

	GET(EventClass* const, pEvent, ESI);
	GET(TechnoClass* const, pTechno, EDI);

	if (pEvent->Type != EventType::MegaMission)
		return 0;

	auto& data = pEvent->MegaMission;
	if (data.PlanningState != PlanningOrderState::Executing || pTechno->CurrentMission != Mission::Wait)
		return 0;

	if (!RefusesObject(data.Target, data.Target.m_RTTI) && !RefusesObject(data.Destination, data.Destination.m_RTTI))
		return 0;

	// What TechnoClass::Enter_Idle_Mode does with a planned route: start the next planned order.
	if (pTechno->IsNotWarpingIn() && pTechno->TryNextPlanningTokenNode())
		return DoNothing;

	// Otherwise go idle. Assign_Mission and Set_Mission both refuse Wait -> Guard (0x5B35E0,
	// 0x5B2FD0), so leave Wait first and let the unit's own idle logic choose its mission.
	pTechno->ForceMission(Mission::None);
	pTechno->EnterIdleMode(false, true);

	return DoNothing;
}
