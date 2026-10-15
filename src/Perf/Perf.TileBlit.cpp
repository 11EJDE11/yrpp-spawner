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

// IsometricTileTypeClass::Draw_Tile (0x547CF0): the row loop of its common path, lit tiles with a
// depth test (0x548D03-0x548E99). Every full redraw of the tactical view draws every tile on screen
// through it, and the original keeps each pointer of the loop in a global and loads and stores it
// for every pixel. This runs the same rows in registers.
//
// Exactness: per pixel the same depth test ((uint16)(depth byte + depth base), skipped when the depth
// buffer holds less), the same depth write, the same shade-table and palette lookups and the same
// destination write, in the same order. Every global the loop leaves behind is left with the same
// value, and the registers the original leaves at 0x54911E are set the same way, including EBX,
// which the original reloads from 0xAA10F8 after each drawn pixel.

#include <Utilities/Macro.h>

#include <cstdint>
#include <xmmintrin.h>

namespace
{
	__forceinline void PrefetchRow(uintptr_t start, uint32_t pixels)
	{
		constexpr uintptr_t CacheLine = 64;
		const uintptr_t end = start + pixels * 2;
		for (uintptr_t line = start & ~(CacheLine - 1); line < end; line += CacheLine)
			_mm_prefetch(reinterpret_cast<const char*>(line), _MM_HINT_T0);
	}

	template <typename T>
	T& Global(uintptr_t address)
	{
		return *reinterpret_cast<T*>(address);
	}

	auto& Source = Global<uint8_t*>(0xAA10B8);
	auto& DepthSource = Global<uint8_t*>(0xAA10BC);
	auto& Palette = Global<uint16_t*>(0xAA10C0);
	auto& Depth = Global<uint16_t*>(0xAA10C4);
	auto& Intensity = Global<uint16_t*>(0xAA10C8);
	auto& Dest = Global<uint16_t*>(0xAA10CC);
	auto& Shades = Global<uint16_t*>(0xAA10D0);
	auto& RowOffsets = Global<uint8_t*>(0xAA10D4);
	auto& RowStarts = Global<uint8_t*>(0xAA10D8);
	auto& RowLengths = Global<uint8_t*>(0xAA10DC);
	auto& SourceBase = Global<uint8_t*>(0xAA10E0);
	auto& DepthSourceBase = Global<uintptr_t>(0xAA10E4);
	auto& DestRow = Global<uintptr_t>(0xAA10E8);
	auto& DepthRow = Global<uintptr_t>(0xAA10EC);
	auto& IntensityRow = Global<uintptr_t>(0xAA10F0);
	auto& RowCount = Global<int>(0xAA10F8);
	auto& DepthBase = Global<uint32_t>(0xAA1104);
	auto& DepthPitch = Global<int>(0xAA1108);
	auto& IntensityPitch = Global<int>(0xAA110C);
	auto& DestPitch = Global<int>(0xAA1110);
}

DEFINE_HOOK(0x548D03, IsometricTileTypeClass_DrawTile_LitDepthRows, 0xA)
{
	enum { Done = 0x54911E };

	int rows = static_cast<int>(R->EBX());
	if (rows <= 0)
	{
		R->EDI(0);
		return Done;
	}

	uintptr_t destRow = reinterpret_cast<uintptr_t>(Dest);
	uintptr_t depthRow = R->ECX();
	uintptr_t intensityRow = R->EAX();
	uintptr_t lengths = 0;
	int row = 0;

	do
	{
		const uint32_t offset = *reinterpret_cast<const uint16_t*>(RowOffsets);
		const uint8_t* source = SourceBase + offset;
		const uint8_t* depthSource = reinterpret_cast<const uint8_t*>(offset + DepthSourceBase);
		const uint32_t start = static_cast<uint32_t>(*RowStarts) * 2;
		DestRow = destRow;
		DepthRow = depthRow;
		IntensityRow = intensityRow;

		uint16_t* dest = reinterpret_cast<uint16_t*>(destRow + start);
		uint16_t* depth = reinterpret_cast<uint16_t*>(depthRow + start);
		const uint16_t* intensity = reinterpret_cast<const uint16_t*>(intensityRow + start);
		const uint32_t count = *RowLengths;

		if (count)
		{
			// The next row of the buffers, a pitch away, is a cache miss otherwise.
			PrefetchRow(depthRow + DepthPitch * 2 + start, count);
			PrefetchRow(intensityRow + IntensityPitch * 2 + start, count);
			PrefetchRow(destRow + DestPitch + start, count);

			const uint16_t* const palette = Palette;
			const uint16_t* const shades = Shades;
			const uint32_t depthBase = DepthBase;
			// The shade table is not written while drawing, so a repeated intensity has the same shade.
			uint32_t lastIntensity = 0x10000;
			uint32_t shade = 0;
			bool drew = false;

			for (uint32_t i = 0; i < count; ++i)
			{
				const uint16_t z = static_cast<uint16_t>(depthSource[i] + depthBase);
				if (depth[i] >= z)
				{
					depth[i] = z;
					const uint32_t level = intensity[i];
					if (level != lastIntensity)
					{
						lastIntensity = level;
						shade = shades[level];
					}

					dest[i] = palette[shade | source[i]];
					drew = true;
				}
			}

			if (drew)
				rows = RowCount;

			// What the loop leaves in its globals; the row end overwrites all but these two.
			Source = const_cast<uint8_t*>(source) + count;
			DepthSource = const_cast<uint8_t*>(depthSource) + count;
		}
		else
		{
			Source = const_cast<uint8_t*>(source);
			DepthSource = const_cast<uint8_t*>(depthSource);
		}

		destRow = DestRow + DestPitch;
		depthRow = DepthRow + DepthPitch * 2;
		intensityRow = IntensityRow + IntensityPitch * 2;
		RowOffsets += 0x78;
		RowStarts += 0x3C;
		RowLengths += 0x3C;
		lengths = reinterpret_cast<uintptr_t>(RowLengths);
		++row;
	}
	while (row < rows);

	Dest = reinterpret_cast<uint16_t*>(destRow);
	Depth = reinterpret_cast<uint16_t*>(depthRow);
	Intensity = reinterpret_cast<uint16_t*>(intensityRow);

	R->EAX(intensityRow);
	R->ECX(depthRow);
	R->EDX(0x3C);
	R->EBX(rows);
	R->ESI(destRow);
	R->EDI(row);
	R->EBP(lengths);
	return Done;
}
