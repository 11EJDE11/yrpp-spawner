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
#include <CellClass.h>
#include <FootClass.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <ObjectClass.h>
#include <TechnoTypeClass.h>

#include <cmath>

// Where a move click on ground the unit cannot reach sends it.
//
// A move, attack move or patrol click on a cell, in FootClass::Active_Click_With (0x4D7D50), takes
// its destination from FootClass_4DE1D0. A visible cell the unit can reach is used as it is.
// Otherwise it asks MapClass::Nearby_Location (0x56DC20) for a cell near the click in the unit's zone,
// with two arguments that make this go wrong:
// - a11, which only accepts cells within one height level of the clicked cell. A click on raised
//   ground the unit cannot reach - a mesa, a plateau beyond a cliff - finds nothing within the 32
//   rings Nearby_Location searches, and the click is dropped: Active_Click_With returns without
//   sending an order (0x4D8081), so the unit does not move. Or it finds the nearest reachable cell
//   at that height instead, which can be across the map from the click, often the way the unit came.
// - closeTo, passed as (0,0). That is CELL_NONE, which makes Nearby_Location pick at random among the
//   cells it found (Frame % count) instead of the one closest to closeTo.
// The click on an object has the same search for a target the unit cannot reach (0x4D7B5E, its
// result standing in for the object at 0x4D7C08).
//
// Fix: search with the height filter and without it, both closest to the selection's centre, and
// use the one without it when the other found nothing or is more than ExtraCellsForSameHeight
// further from the click. That keeps vanilla's choice of the clicked height wherever it is about as
// near as anything else - a click on the rim of a plateau the unit can climb still goes to the top -
// and replaces it where it is not. The centre of the selection rather than each unit keeps a group
// going to one place, on the side facing it.
//
// Only the clicking machine runs this, before the order is sent, and the order carries the chosen
// cell, so it cannot desync, whatever build the other players have.
namespace ClickDestinationFix
{
	// How much further from the click a cell at the clicked height may be, and still be preferred.
	constexpr double ExtraCellsForSameHeight = 2.0;

	double Distance(const CellStruct& a, const CellStruct& b)
	{
		const double dx = a.X - b.X;
		const double dy = a.Y - b.Y;
		return std::sqrt(dx * dx + dy * dy);
	}

	bool IsNone(const CellStruct& cell)
	{
		return cell.X == 0 && cell.Y == 0; // CELL_NONE
	}

	// The centre of the units selected with this one, or this unit's own cell. Never (0,0), which
	// would make Nearby_Location choose at random again.
	CellStruct SelectionCentre(FootClass* pUnit)
	{
		int sumX = 0, sumY = 0, count = 0;
		for (auto pObject : ObjectClass::CurrentObjects)
		{
			auto pFoot = abstract_cast<FootClass*>(pObject);
			if (!pFoot || pFoot->Owner != pUnit->Owner)
				continue;

			const CellStruct cell = pFoot->GetMapCoords();
			sumX += cell.X;
			sumY += cell.Y;
			++count;
		}

		CellStruct centre = count > 0
			? CellStruct { static_cast<short>(sumX / count), static_cast<short>(sumY / count) }
			: pUnit->GetMapCoords();

		if (IsNone(centre))
			centre = pUnit->GetMapCoords();
		if (IsNone(centre))
			centre = CellStruct { 1, 1 };

		return centre;
	}

	// zone and movementZone are what the caller computed for its own search.
	CellStruct Find(FootClass* pUnit, const CellStruct& clicked, int zone, MovementZone movementZone)
	{
		auto& map = MapClass::Instance;
		auto const pType = pUnit->GetTechnoType();
		auto const pClicked = map.TryGetCellAt(clicked);
		const bool clickedBridge = pClicked && (static_cast<unsigned int>(pClicked->Flags) & 0x100u) != 0;
		const CellStruct closeTo = SelectionCentre(pUnit);

		// The arguments FootClass_4DE1D0 passes at 0x4DE528, apart from the height filter and closeTo.
		const auto search = [&](bool sameHeight)
		{
			return map.NearByLocation(clicked, pType->SpeedType, zone, movementZone, clickedBridge,
				1, 1, false, sameHeight, false, true, closeTo, false, false);
		};

		const CellStruct sameHeight = search(true);
		const CellStruct anyHeight = search(false);

		if (IsNone(sameHeight))
			return anyHeight;

		if (!IsNone(anyHeight)
			&& Distance(clicked, anyHeight) + ExtraCellsForSameHeight < Distance(clicked, sameHeight))
		{
			return anyHeight;
		}

		return sameHeight;
	}
}

// FootClass_4DE1D0, the search for a move click (loc_4DE4BD, reached by every path that needs it).
// esi the unit, edi the clicked cell, ebp the movement zone it adjusted; [esp+0x1C] the cell of the
// unit's destination, whose zone it searches, [esp+0x18] whether the unit is on a bridge, and
// [esp+0x10] the result, which 0x4DE535 goes on with.
DEFINE_HOOK(0x4DE4BD, FootClass_ClickDestination_SearchNearClick, 0x5)
{
	enum { UseResult = 0x4DE535 };

	GET(FootClass* const, pUnit, ESI);
	GET(const CellStruct* const, pClicked, EDI);
	GET(const int, movementZone, EBP);
	GET_STACK(CellClass* const, pDestinationCell, 0x1C);
	GET_STACK(const bool, onBridge, 0x18);

	const int zone = MapClass::Instance.GetMovementZoneType(pDestinationCell->MapCoords,
		static_cast<MovementZone>(movementZone), onBridge);

	const CellStruct result = ClickDestinationFix::Find(pUnit, *pClicked, zone, static_cast<MovementZone>(movementZone));
	R->Stack(0x10, result);
	return UseResult;
}

// FootClass::Active_Click_With for an object, the same search for a target the unit cannot reach
// (loc_4D7B5E). esi the unit, edi the movement zone, ebp whether the unit is on a bridge; [esp+0x14]
// the target's cell, [esp+0x18] the cell of the unit's destination, and [esp+0x24] the cell the order
// goes to, which stays the target's when nothing is found, as vanilla's does (0x4D7C08).
DEFINE_HOOK(0x4D7B5E, FootClass_ActiveClickWithObject_SearchNearTarget, 0x6)
{
	enum { UseResult = 0x4D7C08 };

	GET(FootClass* const, pUnit, ESI);
	GET(const int, movementZone, EDI);
	GET(const int, onBridge, EBP);
	GET_STACK(const CellStruct, targetCell, 0x14);
	GET_STACK(CellClass* const, pDestinationCell, 0x18);

	const int zone = MapClass::Instance.GetMovementZoneType(pDestinationCell->MapCoords,
		static_cast<MovementZone>(movementZone), (onBridge & 0xFF) != 0);

	const CellStruct result = ClickDestinationFix::Find(pUnit, targetCell, zone, static_cast<MovementZone>(movementZone));
	if (!ClickDestinationFix::IsNone(result))
		R->Stack(0x24, MapClass::Instance.GetCellAt(result));

	return UseResult;
}
