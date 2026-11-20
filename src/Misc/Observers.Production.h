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

#pragma once

// What a house is producing, read for observers: shared by the cameo over a selected factory
// (Observers.SelectionInfo.cpp) and the observer sidebar pages (Observers.Sidebar.cpp).

#include <vector>

class BuildingClass;
class BuildingTypeClass;
class FactoryClass;
class HouseClass;
class TechnoTypeClass;

namespace ObserverProduction
{
	// The house-wide queues a human player's sidebar feeds.
	enum class Slot : int
	{
		Building,
		Defense,
		Infantry,
		Vehicle,
		Ship,
		Aircraft,
		Count
	};

	constexpr int FullStage = 54; // FactoryClass production always runs 54 steps

	enum class RowState
	{
		Building,
		OnHold,
		NoFunds,
		Ready,
		Cancelled
	};

	struct TypeCount
	{
		const TechnoTypeClass* Type;
		int Count;
	};

	struct ProductionRow
	{
		const TechnoTypeClass* Type;
		RowState State;
		int Stage;
		std::vector<TypeCount> Queue;

		int QueuedCount() const
		{
			int queued = 0;
			for (const auto& group : Queue)
				queued += group.Count;

			return queued;
		}
	};

	void AddToGroups(std::vector<TypeCount>& groups, const TechnoTypeClass* pType);

	bool SlotOf(const TechnoTypeClass* pType, Slot& slot);

	// The queues a factory building of this type serves: a construction yard both of its own.
	int SlotsOf(const BuildingTypeClass* pType, Slot (&slots)[2]);

	// The factory filling this queue for the building: the house's, for a human; the building's own,
	// for the AI.
	FactoryClass* FactoryFor(const BuildingClass* pBuilding, Slot slot);

	// The factory filling this queue for the house: for the AI, the first of its buildings busy on it.
	FactoryClass* FactoryFor(const HouseClass* pHouse, Slot slot);

	// The state of the factory, or a cancel of the queue in the last few seconds. False if neither.
	bool GetRow(const FactoryClass* pFactory, const HouseClass* pHouse, Slot slot, ProductionRow& row);

	void RememberCancel(const FactoryClass* pFactory);
}
