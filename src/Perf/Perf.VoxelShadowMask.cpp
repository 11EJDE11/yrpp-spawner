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

// VoxelLibraryClass::Draw_Shadow (0x756860) draws a masked voxel shadow - every frame for a Kirov, whose
// shadow is never cached, and for ships such as the Dreadnought - by reading the mask surface twice for
// every voxel through the virtual Get_Pixel, which locks the surface, asks it for its pitch and pixel
// size through more virtual calls, reads one pixel and unlocks it. Twenty Kirovs and twenty
// Dreadnoughts spent a third of the frame there.
//
// This runs that loop - the same 16-bit stepping over the layer, the same mask bytes - and reads the
// pixels straight from the surface's buffer when the surface is a plain BSurface and the point is
// inside it, the one case every Get_Pixel - the game's and Phobos' bounds-checked replacement alike -
// answers with the pixel at Lock(x, y). Any other surface or point goes through Get_Pixel as before.

#include <Surface.h>

#include <Utilities/Macro.h>

namespace
{
	// BSurface's vtable and the entries the shortcut stands in for.
	constexpr uintptr_t BSurfaceVTable = 0x7E2070;
	constexpr uintptr_t BSurfaceLock = 0x4115F0;
	constexpr uintptr_t XSurfaceUnlock = 0x411570;
	constexpr uintptr_t BSurfaceGetBytesPerPixel = 0x411630;
	constexpr uintptr_t BSurfaceGetPitch = 0x411640;
	constexpr uintptr_t SurfaceGetWidth = 0x411540;

	BYTE* const VoxelPixelBuffer = reinterpret_cast<BYTE*>(0xB2FF78);

	// Whether BSurface's Lock, Unlock and size queries are still the game's, read once: a patched
	// entry would change what Lock(x, y) points at.
	bool PlainBSurface()
	{
		static const bool plain = []
		{
			auto const pVTable = reinterpret_cast<const uintptr_t*>(BSurfaceVTable);
			return pVTable[0x5C / 4] == BSurfaceLock && pVTable[0x60 / 4] == XSurfaceUnlock
				&& pVTable[0x70 / 4] == BSurfaceGetBytesPerPixel && pVTable[0x74 / 4] == BSurfaceGetPitch
				&& pVTable[0x7C / 4] == SurfaceGetWidth;
		}();

		return plain;
	}

	// Reads mask pixels the way Get_Pixel does, straight from the buffer when the surface is a plain
	// BSurface and the point is inside it.
	struct MaskReader
	{
		Surface* const pSurface;
		const BYTE* pBuffer = nullptr;
		unsigned int Width = 0;
		unsigned int Height = 0;
		int BytesPerPixel = 0;

		explicit MaskReader(Surface* pSurface) : pSurface { pSurface }
		{
			if (*reinterpret_cast<const uintptr_t*>(pSurface) == BSurfaceVTable && PlainBSurface())
			{
				pBuffer = *reinterpret_cast<BYTE* const*>(reinterpret_cast<const BYTE*>(pSurface) + 0x14);
				Width = static_cast<unsigned int>(pSurface->Width);
				Height = static_cast<unsigned int>(pSurface->Height);
				BytesPerPixel = static_cast<XSurface*>(pSurface)->BytesPerPixel;
			}
		}

		unsigned int Get(int x, int y) const
		{
			if (pBuffer && static_cast<unsigned int>(x) < Width && static_cast<unsigned int>(y) < Height)
			{
				auto const pPixel = pBuffer + x * BytesPerPixel + y * (static_cast<int>(Width) * BytesPerPixel);
				return BytesPerPixel == 2 ? *reinterpret_cast<const WORD*>(pPixel) : *pPixel;
			}

			Point2D point { x, y };
			return pSurface->GetPixel(&point);
		}
	};
}

// The masked loop, from the row count to the function's return.
DEFINE_HOOK(0x7569BA, VoxelLibraryClass_DrawShadow_MaskedLoop, 0x7)
{
	enum { Return = 0x756AE9 };

	const unsigned int rows = R->Stack<unsigned int>(0x80);
	if (!rows)
		return 0;

	GET(Surface* const, pSurface, ECX);
	GET(unsigned int, x, EBX);
	GET(unsigned int, y, EAX);
	GET(unsigned int, index, EDI);

	const unsigned int columns = R->Stack<unsigned int>(0x10);
	auto const pLayer = R->Stack<const int*>(0x48);
	const unsigned int shadowX = R->Stack<unsigned int>(0x28);
	const unsigned int shadowY = R->Stack<unsigned int>(0x30);
	const unsigned int columnStepX = R->Stack<unsigned int>(0x62);
	const unsigned int columnStepY = R->Stack<unsigned int>(0x64);
	const unsigned int rowStepX = R->Stack<unsigned int>(0x68);
	const unsigned int rowStepY = R->Stack<unsigned int>(0x6A);

	const MaskReader mask { pSurface };
	for (unsigned int row = 0; row < rows; ++row)
	{
		const unsigned int rowX = x, rowY = y, rowIndex = index;
		for (unsigned int column = 0; column < columns; ++column, ++index, x += columnStepX, y += columnStepY)
		{
			if (pLayer[index] == -1)
				continue;

			const unsigned int pixel = (y & 0xFF00) | ((x & 0xFFFF) >> 8);
			const int pointX = static_cast<int>((shadowX + ((x >> 8) & 0xFF)) & 0xFFFF);
			const int pointY = static_cast<int>((shadowY + ((y >> 8) & 0xFF)) & 0xFFFF);
			VoxelPixelBuffer[pixel] = mask.Get(pointX, pointY) == 0;
			VoxelPixelBuffer[pixel + 1] = mask.Get(pointX + 1, pointY) == 0;
		}

		x = rowX + rowStepX;
		y = rowY + rowStepY;
		index = rowIndex + columns;
	}

	return Return;
}
