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

// LightSourceClass::Draw (0x554AF0) relights the cells around a light whenever the light changes and
// then asks for the whole tactical view to be redrawn (GScreenClass::Flag_To_Redraw(true)). A
// radiation site's light fades a step every RadLightDelay frames, so a few desolators make the game
// redraw every terrain tile on screen every few frames, at several milliseconds each.
//
// When the cells are relit on the spot (update_drawers false, as radiation sites do), this redraws
// only the screen area of the cells the light covers instead: the same Tactical::Redraw_Area the game
// uses for a cell whose ore or wall changed, with the alpha flag set so the shroud and alpha layers
// are redrawn there too, as a full redraw does. The rest of Flag_To_Redraw is kept: the tactical
// redraw flag and the map's redraw counter, which overlay drawing uses to tell a stale cached draw.
// Lights relit later through the pending list (update_drawers true) keep the original full redraw.
//
// Not pixel-exact: like every partial redraw in the game, a long building shadow cast across the
// area's edge from a cell well outside it is not redrawn inside it until the next full redraw. It
// only changes the picture, never the game.

#include <CellClass.h>
#include <MapClass.h>
#include <Surface.h>
#include <TacticalClass.h>
#include <LightSourceClass.h>

#include <Utilities/Macro.h>

#include <algorithm>
#include <cstddef>

namespace
{
	bool __fastcall IsValidCell(const CellStruct& cell)
	{
		using Function = bool(__thiscall*)(MapClass*, const CellStruct*);
		return reinterpret_cast<Function>(0x5657E0)(&MapClass::Instance, &cell);
	}

	CellClass* CellAt(const CellStruct& cell)
	{
		using Function = CellClass*(__thiscall*)(MapClass*, const CellStruct*);
		return reinterpret_cast<Function>(0x5657A0)(&MapClass::Instance, &cell);
	}

	RectangleStruct TileRect(CellClass* pCell)
	{
		using Function = RectangleStruct*(__thiscall*)(CellClass*, RectangleStruct*);
		RectangleStruct rect {};
		reinterpret_cast<Function>(0x47FF80)(pCell, &rect);
		return rect;
	}

	void Grow(RectangleStruct& total, bool& any, const RectangleStruct& rect)
	{
		if (rect.Width <= 0 || rect.Height <= 0)
			return;

		if (!any)
		{
			total = rect;
			any = true;
			return;
		}

		const int left = std::min(total.X, rect.X);
		const int top = std::min(total.Y, rect.Y);
		const int right = std::max(total.X + total.Width, rect.X + rect.Width);
		const int bottom = std::max(total.Y + total.Height, rect.Y + rect.Height);
		total = { left, top, right - left, bottom - top };
	}

	// Every cell the light's loop visits, valid or not near the edge, so the area covers each cell it
	// can have relit; a slightly larger area only redraws unchanged pixels again.
	void RedrawLitArea(const LightSourceClass* pLight)
	{
		const short centreX = static_cast<short>(pLight->Location.X / 256);
		const short centreY = static_cast<short>(pLight->Location.Y / 256);
		const int radius = pLight->LightVisibility / 256 + 1;

		RectangleStruct total {};
		bool any = false;
		for (int dy = -radius; dy <= radius; ++dy)
		{
			for (int dx = -radius; dx <= radius; ++dx)
			{
				const CellStruct cell { static_cast<short>(centreX + dx), static_cast<short>(centreY + dy) };
				if (static_cast<unsigned short>(cell.X) >= 512 || static_cast<unsigned short>(cell.Y) >= 512 || !IsValidCell(cell))
					continue;

				CellClass* const pCell = CellAt(cell);
				RectangleStruct rect {};
				Grow(total, any, TileRect(pCell));
				Grow(total, any, *pCell->GetContainingRect(&rect));
				Grow(total, any, *pCell->ShapeRect(&rect));
			}
		}

		// GScreenClass::Flag_To_Redraw without the complete redraw.
		if (TacticalClass::Instance)
			TacticalClass::Instance->Redrawing = true;
		using IncRedraws = void(__thiscall*)(MapClass*);
		reinterpret_cast<IncRedraws>(0x578AC0)(&MapClass::Instance);

		if (!any || !TacticalClass::Instance)
			return;

		total.X -= DSurface::ViewBounds.X;
		total.Y -= DSurface::ViewBounds.Y;
		TacticalClass::Instance->RegisterDirtyArea(total, true);
	}
}

// LightSourceClass::Draw, the "push 1; mov ecx, offset Map; call Flag_To_Redraw" at its end.
DEFINE_HOOK(0x554D0A, LightSourceClass_Draw_RedrawLitArea, 0x7)
{
	enum { SkipFullRedraw = 0x554D16 };

	const bool updateDrawers = *reinterpret_cast<const bool*>(R->EBP() + 8);
	if (updateDrawers)
		return 0;

	GET(LightSourceClass*, pThis, EDI);
	RedrawLitArea(pThis);
	return SkipFullRedraw;
}

static_assert(offsetof(LightSourceClass, Location) == 0x38);
static_assert(offsetof(LightSourceClass, LightVisibility) == 0x44);
static_assert(offsetof(TacticalClass, Redrawing) == 0xD7D);
