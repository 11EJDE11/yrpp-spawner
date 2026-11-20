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

#include "Observers.Production.h"

#include <Utilities/Macro.h>

#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <FactoryClass.h>
#include <HouseClass.h>
#include <TechnoTypeClass.h>

#include <windows.h>

#include <algorithm>

namespace ObserverProduction
{
	namespace
	{
		// Production that was cancelled leaves nothing behind in the factory, so the abandon is
		// remembered for a few seconds to show it.
		struct CancelRecord
		{
			const HouseClass* House;
			Slot QueueSlot;
			const TechnoTypeClass* Type;
			int Stage;
			DWORD Tick;
		};

		std::vector<CancelRecord> Cancels;
		constexpr DWORD CancelShownForMs = 3000;
		constexpr size_t CancelsKept = 64;

		const CancelRecord* RecentCancel(const HouseClass* pHouse, Slot slot)
		{
			const DWORD now = GetTickCount();
			for (auto it = Cancels.rbegin(); it != Cancels.rend(); ++it)
			{
				if (now - it->Tick > CancelShownForMs)
					break;

				if (it->House == pHouse && it->QueueSlot == slot)
					return &*it;
			}

			return nullptr;
		}

		bool ProducesFor(const FactoryClass* pFactory, Slot slot)
		{
			Slot productSlot;
			return pFactory && pFactory->Object && SlotOf(pFactory->Object->GetTechnoType(), productSlot)
				&& productSlot == slot;
		}

		FactoryClass* HumanFactory(const HouseClass* pHouse, Slot slot)
		{
			switch (slot)
			{
			case Slot::Building: return pHouse->Primary_ForBuildings;
			case Slot::Defense:  return pHouse->Primary_ForDefenses;
			case Slot::Infantry: return pHouse->Primary_ForInfantry;
			case Slot::Vehicle:  return pHouse->Primary_ForVehicles;
			case Slot::Ship:     return pHouse->Primary_ForShips;
			case Slot::Aircraft: return pHouse->Primary_ForAircraft;
			default:             return nullptr;
			}
		}
	}

	void AddToGroups(std::vector<TypeCount>& groups, const TechnoTypeClass* pType)
	{
		if (!pType)
			return;

		for (auto& group : groups)
		{
			if (group.Type == pType)
			{
				++group.Count;
				return;
			}
		}

		groups.push_back({ pType, 1 });
	}

	bool SlotOf(const TechnoTypeClass* pType, Slot& slot)
	{
		if (!pType)
			return false;

		switch (pType->WhatAmI())
		{
		case AbstractType::BuildingType:
			slot = static_cast<const BuildingTypeClass*>(pType)->BuildCat == BuildCat::Combat
				? Slot::Defense : Slot::Building;
			return true;
		case AbstractType::InfantryType:
			slot = Slot::Infantry;
			return true;
		case AbstractType::UnitType:
			slot = pType->Naval ? Slot::Ship : Slot::Vehicle;
			return true;
		case AbstractType::AircraftType:
			slot = Slot::Aircraft;
			return true;
		default:
			return false;
		}
	}

	int SlotsOf(const BuildingTypeClass* pType, Slot (&slots)[2])
	{
		switch (pType->Factory)
		{
		case AbstractType::BuildingType:
			slots[0] = Slot::Building;
			slots[1] = Slot::Defense;
			return 2;
		case AbstractType::InfantryType:
			slots[0] = Slot::Infantry;
			return 1;
		case AbstractType::UnitType:
			slots[0] = pType->Naval ? Slot::Ship : Slot::Vehicle;
			return 1;
		case AbstractType::AircraftType:
			slots[0] = Slot::Aircraft;
			return 1;
		default:
			return 0;
		}
	}

	// A human house builds through one factory per queue, shared by every building that serves it
	// (what Ares shows too). The AI builds one object at a time in the building's own factory
	// (BuildingClass::Factory_AI, 0x4500F0).
	FactoryClass* FactoryFor(const BuildingClass* pBuilding, Slot slot)
	{
		const auto pOwner = pBuilding->Owner;
		if (!pOwner->IsControlledByHuman())
			return ProducesFor(pBuilding->Factory, slot) ? pBuilding->Factory : nullptr;

		return HumanFactory(pOwner, slot);
	}

	FactoryClass* FactoryFor(const HouseClass* pHouse, Slot slot)
	{
		if (pHouse->IsControlledByHuman())
			return HumanFactory(pHouse, slot);

		for (const auto pBuilding : pHouse->Buildings)
		{
			if (pBuilding && ProducesFor(pBuilding->Factory, slot))
				return pBuilding->Factory;
		}

		return nullptr;
	}

	bool GetRow(const FactoryClass* pFactory, const HouseClass* pHouse, Slot slot, ProductionRow& row)
	{
		if (pFactory && pFactory->Object)
		{
			row.Type = pFactory->Object->GetTechnoType();
			if (!row.Type)
				return false;

			row.Stage = std::clamp(pFactory->Production.Value, 0, FullStage);

			// FactoryClass::Has_Completed (0x4CA130) / Suspend (0x4C9E60) / AI (0x4C9B20): a finished
			// product suspends the factory; a hold suspends it before; OnHold is set by AI when a
			// production step could not be paid for.
			if (row.Stage >= FullStage)
				row.State = RowState::Ready;
			else if (pFactory->IsSuspended)
				row.State = RowState::OnHold;
			else if (pFactory->OnHold)
				row.State = RowState::NoFunds;
			else
				row.State = RowState::Building;

			row.Queue.clear();
			for (const auto pQueued : pFactory->QueuedObjects)
				AddToGroups(row.Queue, pQueued);

			return true;
		}

		if (const auto pCancel = RecentCancel(pHouse, slot))
		{
			row.Type = pCancel->Type;
			row.Stage = pCancel->Stage;
			row.State = RowState::Cancelled;
			row.Queue.clear();
			return true;
		}

		return false;
	}

	void RememberCancel(const FactoryClass* pFactory)
	{
		if (!pFactory->Object || !pFactory->Owner)
			return;

		const auto pType = pFactory->Object->GetTechnoType();
		Slot slot;
		if (!SlotOf(pType, slot))
			return;

		if (Cancels.size() >= CancelsKept)
			Cancels.erase(Cancels.begin());

		Cancels.push_back({ pFactory->Owner, slot, pType,
			std::clamp(pFactory->Production.Value, 0, FullStage), GetTickCount() });
	}
}

// FactoryClass::Abandon, called by HouseClass::Abandon_Production for a player's cancel and by
// HouseClass::AI_Raise_Money when the AI cancels to free money. Its other callers replace or tear
// down the factory and are not cancels.
DEFINE_HOOK(0x4C9FF0, FactoryClass_Abandon_RememberCancel, 0x6)
{
	GET(FactoryClass*, pFactory, ECX);
	GET_STACK(DWORD, returnAddress, 0x0);

	if (returnAddress == 0x4FABAB || returnAddress == 0x4FE07F)
		ObserverProduction::RememberCancel(pFactory);

	return 0;
}
