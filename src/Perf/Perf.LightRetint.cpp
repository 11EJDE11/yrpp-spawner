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

// A map's "Retint Red/Green/Blue" trigger actions call IonStormClass::Adjust_Lighting(red, green,
// blue, false), which regenerates the palette tables of every tile drawer (LightConvertClass::Array)
// to the new ambient tint and overwrites each drawer's identifying tint (Color1) with it. Cells pick a
// drawer by that tint, so from then on no cell finds a drawer that matches: every cell that is relit
// (each step of a radiation light's fade, every object drawn on a cell without one) allocates a new
// drawer, which is never freed, and the next retint step regenerates all of them again. On a map that
// retints every frame, drawers accumulate for as long as the game lasts and each retint step costs
// more than the last: the "radiation lag that never goes away".
//
// Instead, a retint leaves the drawers alone - a drawer's table depends only on its own tint, and the
// ambient tint goes into each cell's own lighting - and drops every cell's drawer so the cell picks
// the right one again (found by tint, or created once) when it is next drawn, as Set_Detail_Level
// does. The ambient-only drawers (colour schemes, palettes) and the rest of the function are kept.
// Ion storm, nuke and dominator tints (tint true) and the restore after them (red -1) are unchanged.
//
// Draw-only: the drawers and a cell's pointer to one are not part of the game state.
//
// Measured. The test map from CnCNet/cncnet-yr-client-package issue 647 (a retint every second and
// desolator radiation; 24,093 frames, every frame drawn at the fastest playback speed, two runs each):
// 65.4 s -> 22.2 s for the whole replay. Without this the tile drawers grew to 4,986 by the end, about
// 400 more every 1,000 frames and still climbing; with it they stop at 364 from frame 11,000 on. Every
// frame's CRC as recorded in all four runs.

#include <CellClass.h>
#include <ConvertClass.h>
#include <MapClass.h>
#include <Unsorted.h>

#include <Utilities/Macro.h>

namespace
{
	void DropCellDrawers()
	{
		// The cell array directly, not MapClass's shared cell iterator, which a caller may be walking.
		auto const& cells = MapClass::Instance.Cells;
		for (int i = 0; i < cells.Capacity; ++i)
		{
			CellClass* const pCell = cells.Items[i];
			if (!pCell)
				continue;

			if (auto const pDrawer = pCell->LightConvert)
			{
				if (Game::IsActive)
					--pDrawer->RefCount;
				pCell->LightConvert = nullptr;
			}
		}

		// The scratch cell drawing uses for cells outside the map.
		MapClass::InvalidCell.LightConvert = nullptr;
	}
}

// IonStormClass::Adjust_Lighting(ecx red, edx green, blue, tint): the load of the tile drawer count
// that its first loop runs on.
DEFINE_HOOK(0x53AD00, IonStormClass_Adjust_Lighting_KeepTileDrawers, 0x5)
{
	enum { SkipTileDrawers = 0x53AD05 };

	GET(int, red, ECX);
	GET_STACK(bool, tint, 0x8);

	if (tint || red == -1)
		return 0;

	DropCellDrawers();
	R->EAX(0);
	return SkipTileDrawers;
}
