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

// Tactical::Cell_Shadow (0x6D8700, TacticalClass::GetOcclusion): the shroud edge shape of a cell,
// from the shroud bits of the cell and its eight neighbours. Units call it for a ring of cells
// around themselves whenever they move or change height, so hovering jumpjets call it hundreds of
// times a frame each. The original makes nine out-of-line MapClass::operator[] calls and rebuilds
// each neighbour coordinate through the stack; this does the same lookups inline.
//
// Exactness: the same cells are looked up in the same order through MapClass::GetCellAt, which, like
// operator[] (0x5657A0), stamps the coordinates of a cell outside the map into InvalidCell. The
// caller's CellStruct is read again for every neighbour, as the original does, since it can be
// InvalidCell's own MapCoords. The result is the same sign-extended int.

#include <CellClass.h>
#include <MapClass.h>
#include <TacticalClass.h>

#include <Utilities/Macro.h>

#include <type_traits>

namespace
{
	// Shroud shape frame for each combination of uncovered neighbours (0x7F4194).
	const signed char* const ShadowFrames = reinterpret_cast<const signed char*>(0x7F4194);

	struct Neighbour
	{
		short X, Y;
		int Bit;
	};

	// In the original's lookup order.
	constexpr Neighbour Neighbours[] = {
		{ -1, -1, 0x40 }, { 0, -1, 0x80 }, { 1, -1, 0x01 },
		{ -1, 0, 0x20 }, { 1, 0, 0x02 },
		{ -1, 1, 0x10 }, { 0, 1, 0x08 }, { 1, 1, 0x04 },
	};

	template <typename Flags>
	__forceinline int ShadowOf(const volatile CellStruct& cell, Flags CellClass::* field,
		std::type_identity_t<Flags> covered, std::type_identity_t<Flags> seen)
	{
		const Flags centre = MapClass::Instance.GetCellAt(CellStruct { cell.X, cell.Y })->*field;
		if (!(centre & seen))
			return (centre & covered) ? -1 : -2;

		int index = 0;
		for (auto const& neighbour : Neighbours)
		{
			const CellStruct at { static_cast<short>(cell.X + neighbour.X), static_cast<short>(cell.Y + neighbour.Y) };
			if (!(MapClass::Instance.GetCellAt(at)->*field & seen))
				index |= neighbour.Bit;
		}

		return ShadowFrames[index];
	}
}

class TacticalClassPerf final : public TacticalClass
{
public:
	int _GetOcclusion(const CellStruct& cell, bool useAlt) const
	{
		const volatile CellStruct& current = cell;
		if (!useAlt)
			return ShadowOf(current, &CellClass::AltFlags, AltCellFlags::Clear, AltCellFlags::Mapped);

		return ShadowOf(current, &CellClass::Flags, CellFlags::CenterRevealed | CellFlags::EdgeRevealed, CellFlags::EdgeRevealed);
	}
};

DEFINE_FUNCTION_JUMP(LJMP, 0x6D8700, TacticalClassPerf::_GetOcclusion);
