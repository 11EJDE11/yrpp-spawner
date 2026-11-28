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

// Production orders under lockstep.
//
// In the original game a finished unit, infantry or aircraft leaves its factory only after a
// networked round trip: the owner's sidebar (StripClass::AI, 0x6A8EE8) sends a PLACE event when
// the factory reaches 100%, and the unit exits when that event executes MaxAhead frames later
// (HouseClass::Place_Object, 0x4FB0E0). Until then the factory sits completed and suspended, and
// three things go wrong in that window:
//
//  - a PRODUCE for the same type that was sent in the MaxAhead frames before completion lands on
//    the completed factory, Begin_Production treats it as a resume, FactoryClass::Start refuses
//    (stage already 54) and the order is dropped without feedback;
//  - a left-click on the cameo sends a second PLACE instead of a PRODUCE;
//  - a right-click decides between hold and cancel from the local factory state, so two quick
//    right-clicks both send SUSPEND and the second one executes as a no-op.
//
// Each produced unit therefore costs about two MaxAhead windows of lost orders plus MaxAhead frames
// of factory idle time. This file removes the round trip and makes the remaining cases do what the
// player meant.

#include "ProductionQueue.h"
#include "Spawner.h"

#include <Helpers/Macro.h>
#include <Fundamentals.h>
#include <GeneralDefinitions.h>
#include <GeneralStructures.h>
#include <SessionClass.h>
#include <HouseClass.h>
#include <FactoryClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <EventClass.h>
#include <Unsorted.h>

bool ProductionQueue::IsActive()
{
	return Spawner::Enabled && SessionClass::IsMultiplayer();
}

namespace
{
	// HouseClass::Place_Object. For units, infantry and aircraft the sidebar's PLACE event passes
	// heap id -1 and the game's CELL_NONE, which makes Place_Object exit the completed object from
	// whichever factory can build it.
	bool PlaceCompletedObject(HouseClass* pHouse, AbstractType rtti, bool naval)
	{
		return pHouse->PlaceObject(rtti, -1, naval ? 1 : 0, &Unsorted::CellNone);
	}

	// Exit every completed non-building product of a house's sidebar factories. Runs on every
	// client on the frame the product completes, in place of the owner's PLACE event.
	void ExitCompletedProducts(HouseClass* pHouse)
	{
		FactoryClass* const factories[] =
		{
			pHouse->Primary_ForInfantry,
			pHouse->Primary_ForVehicles,
			pHouse->Primary_ForShips,
			pHouse->Primary_ForAircraft,
		};

		for (auto* const pFactory : factories)
		{
			if (!pFactory || !pFactory->IsDone())
				continue;

			auto* const pObject = pFactory->Object; // null for a completed special item
			if (!pObject || pObject->WhatAmI() == AbstractType::Building)
				continue;

			auto* const pType = pObject->GetTechnoType();
			PlaceCompletedObject(pHouse, pObject->WhatAmI(), pType && pType->Naval);
			// Place_Object may have deleted pFactory; nothing below touches it.
		}
	}

	// Whether this player has a SUSPEND for the given cameo that has not executed yet, in the
	// outgoing queue or already stamped into the DoList.
	bool HasPendingSuspend(AbstractType rtti, int heapId)
	{
		const auto me = HouseClass::CurrentPlayer ? HouseClass::CurrentPlayer->ArrayIndex : -1;

		const auto matches = [&](const EventClass& event)
		{
			return event.Type == EventType::Suspend
				&& (event.IsExecuted & 1) == 0
				&& event.HouseIndex == me
				&& event.Suspend.RTTIType == rtti
				&& event.Suspend.HeapID == heapId;
		};

		for (int i = 0; i < EventClass::OutList.Count; ++i)
			if (matches(EventClass::OutList[i]))
				return true;

		for (int i = 0; i < EventClass::DoList.Count; ++i)
			if (matches(EventClass::DoList[i]))
				return true;

		return false;
	}
}

// LogicClass::AI, between the factory AI loop and the house AI loop: every factory has ticked, so
// a product that completed this frame leaves now, on every client alike.
DEFINE_HOOK(0x55B68D, LogicClass_AI_ExitCompletedProducts, 0x5)
{
	if (ProductionQueue::IsActive())
	{
		for (auto* const pHouse : HouseClass::Array)
		{
			if (pHouse && pHouse->IsControlledByHuman())
				ExitCompletedProducts(pHouse);
		}
	}

	return 0;
}

// HouseClass::Place_Object, the branch taken when a non-building product could not exit (factory
// door busy, barracks radio slots full, exit cell blocked). The original abandons and refunds the
// finished product. Keep it instead; the hook above tries again next frame, and the player can
// still cancel it from the sidebar.
DEFINE_HOOK(0x4FB595, HouseClass_PlaceObject_KeepUnexitedProduct, 0x5)
{
	enum { ReturnFalse = 0x4FB5BA };

	return ProductionQueue::IsActive() ? ReturnFalse : 0;
}

// HouseClass::Begin_Production, the "factory is suspended" check. A completed factory is suspended
// too, so a PRODUCE of the same type as the finished product was treated as a resume and dropped.
// Queue it instead, as a PRODUCE of any other type already is. Buildings keep the original path:
// FactoryClass::Set abandons a building factory's current product.
DEFINE_HOOK(0x4FA5A8, HouseClass_BeginProduction_QueueOnCompletedFactory, 0x5)
{
	enum { Queue = 0x4FA5C8 };

	GET(FactoryClass* const, pFactory, ESI);

	if (ProductionQueue::IsActive() && pFactory->IsDone() && pFactory->Object
		&& pFactory->Object->WhatAmI() != AbstractType::Building)
	{
		R->BL(0); // is_building_the_same = false
		return Queue;
	}

	return 0;
}

// SidebarClass::StripClass::SelectClass::Action, left-click on a cameo whose non-building product
// has completed but not left yet. The original sends another PLACE, which only repeats what the
// exit hook does every frame. Queue one more instead, which is what the click means.
DEFINE_HOOK(0x6AB3D9, SelectClass_Action_QueueInsteadOfPlace, 0x6)
{
	enum { Done = 0x6AB95A };

	if (!ProductionQueue::IsActive())
		return 0;

	GET(const AbstractType, rtti, EBP);
	GET_STACK(const int, heapId, 0x14);
	GET_STACK(TechnoTypeClass* const, pType, 0x28);

	EventClass::OutList.Add(EventClass(HouseClass::CurrentPlayer->ArrayIndex, EventType::Produce,
		static_cast<int>(rtti), heapId, pType && pType->Naval));

	return Done;
}

// SidebarClass::StripClass::SelectClass::Action, right-click on the cameo in production. The
// original holds a running factory and cancels a held one, judged by local state. With a SUSPEND
// still in flight the factory is as good as held, so the second right-click cancels.
DEFINE_HOOK(0x6AAE08, SelectClass_Action_AbandonWhenSuspendPending, 0x5)
{
	enum { Abandon = 0x6AAE13 };

	if (!ProductionQueue::IsActive())
		return 0;

	GET(const AbstractType, rtti, EBP);
	GET_STACK(const int, heapId, 0x14);

	return HasPendingSuspend(rtti, heapId) ? Abandon : 0;
}
