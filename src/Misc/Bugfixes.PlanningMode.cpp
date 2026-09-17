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

#include <FootClass.h>
#include <PlanningTokenClass.h>
#include <TechnoClass.h>

// A unit joins PlanningTokenClass::ActiveRouteOwners, and its house's HouseRouteCounts goes up,
// when its first planned order is queued. Every path that clears a route with orders still in it
// undoes both. PlanningTokenClass::Remove_Node (0x636B50) does not: it is what the main loop
// (0x637270) uses to drop planned orders whose target has died, and what deleting a waypoint
// (0x639BC0) uses. When it removes the last order the unit stays in the list with an empty route,
// is added again by its next plan, and becomes a dangling pointer when it dies. The inflated count
// eventually refuses planning mode with "MSG:PlannerMaximum" (0x639130).
// Nothing in the simulation reads either, so finishing the bookkeeping cannot desync.
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
