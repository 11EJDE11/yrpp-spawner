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

// TechnoClass::Draw_Health_Bar_Selected draws a selected vehicle's or infantryman's health as up to 17
// pips, each a 2x2 frame of pips.shp drawn through CC_Draw_Shape (0x4AED70). Every call clips the frame,
// wraps it in a surface, locks both surfaces and picks a blitter for four pixels, so a hundred selected
// units cost a tenth of a millisecond a frame in pips alone.
//
// This draws the pips with the surface locked once and the frame's rows copied the way the blitter
// CC_Draw_Shape picks (BlitTransXlat, 0x4912B0) copies them, through the game's own clipping
// (0x7BBE20). Anything the shortcut does not cover - another blitter, a compressed frame, a shape not
// yet loaded - runs the original loop instead.

#include <ConvertClass.h>
#include <Drawing.h>
#include <FileSystem.h>
#include <Surface.h>

#include <Utilities/Macro.h>

#include <algorithm>

namespace
{
	constexpr uintptr_t BlitTransXlatCopy = 0x4912B0;

	// What CC_Draw_Shape does for each pip: (x + 2 * i, y), frame, bounds, flags BF_CENTER | BF_400,
	// everything else zero. Returns false, having drawn nothing, if it cannot reproduce it.
	bool DrawPips(int x, int y, int frame, int count, const RectangleStruct& bounds)
	{
		SHPStruct* pShape = FileSystem::PIPS_SHP;
		ConvertClass* const pDrawer = FileSystem::PALETTE_PAL;
		Surface* const pSurface = DSurface::Temp;
		if (!pShape || !pDrawer || !pSurface || pSurface->GetBytesPerPixel() != 2)
			return false;

		if (pShape->IsReference())
		{
			auto const pReference = pShape->AsReference();
			if (!pReference->Loaded)
				return false;

			pShape = pReference->Data;
			if (!pShape)
				return false;
		}

		if (frame < 0 || frame >= pShape->Frames || pShape->HasCompression(frame))
			return false;

		RectangleStruct surfaceRect;
		const RectangleStruct area = Drawing::Intersect(bounds, *pSurface->GetRect(&surfaceRect));
		if (area.Width <= 0 || area.Height <= 0)
			return true;

		// CC_Draw_Shape stores the drawer value before picking the blitter.
		*reinterpret_cast<int*>(reinterpret_cast<char*>(pDrawer) + 0x17C) = 0;

		auto const pBlitter = pDrawer->SelectPlainBlitter(BlitterFlags(0x600));
		if (!pBlitter || (*reinterpret_cast<uintptr_t* const*>(pBlitter))[1] != BlitTransXlatCopy)
			return false;

		auto const pPalette = *reinterpret_cast<const WORD* const*>(reinterpret_cast<const char*>(pBlitter) + 4);
		const RectangleStruct frameRect = pShape->GetFrameBounds(frame);
		auto const pPixels = pShape->GetPixels(frame);
		const int pitch = pSurface->GetPitch();

		// Buffer_To_Surface copies a whole block in one call when a row spans the pitch.
		if (frameRect.Width <= 0 || frameRect.Height <= 0 || !pPixels || frameRect.Width == pitch)
			return false;

		auto const pBits = static_cast<BYTE*>(pSurface->Lock(0, 0));
		if (!pBits)
			return false;

		const RectangleStruct frameArea { 0, 0, frameRect.Width, frameRect.Height };
		const int left = x - pShape->Width / 2 + frameRect.X;
		const int top = y - pShape->Height / 2 + frameRect.Y;
		for (int i = 0; i < count; ++i)
		{
			RectangleStruct to { left + 2 * i, top, frameRect.Width, frameRect.Height };
			RectangleStruct from = frameArea;
			if (!Drawing::BlitClip(to, area, from, frameArea))
				continue;

			auto pDst = reinterpret_cast<WORD*>(pBits + (to.Y + area.Y) * pitch) + to.X + area.X;
			auto pSrc = pPixels + from.Y * frameRect.Width + from.X;
			for (int row = std::min(from.Height, to.Height); row > 0; --row)
			{
				for (int column = 0; column < from.Width; ++column)
				{
					if (const BYTE pixel = pSrc[column])
						pDst[column] = pPalette[pixel];
				}

				pDst = reinterpret_cast<WORD*>(reinterpret_cast<BYTE*>(pDst) + pitch);
				pSrc += frameRect.Width;
			}
		}

		pSurface->Unlock();
		return true;
	}
}

DEFINE_HOOK(0x6F69F3, TechnoClass_DrawHealthBar_Pips, 0x6)
{
	enum { AfterPips = 0x6F6A58 };

	GET(Point2D* const, pLocation, EDI);
	GET(const int, yOffset, EBP);
	GET_STACK(const int, frame, 0x14);
	GET_STACK(const int, xOffset, 0x1C);
	GET_STACK(const int, count, 0x50);
	GET_STACK(RectangleStruct* const, pBounds, 0x54);

	const int x = pLocation->X + xOffset;
	const int y = pLocation->Y + yOffset;
	if (count > 0 && !DrawPips(x, y, frame, count, *pBounds))
		return 0;

	// Where the original loop leaves its registers and the last pip's position.
	if (count > 0)
	{
		R->Stack(0x34, x + 2 * (count - 1));
		R->Stack(0x38, y);
	}

	R->EAX(count);
	R->EBX(count > 0 ? count : 0);
	return AfterPips;
}
