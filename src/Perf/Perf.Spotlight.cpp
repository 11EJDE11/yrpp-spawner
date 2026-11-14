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

// A spotlight brightens the screen under its cone through MMX_Brighten_565 (0x7DF280): for every
// lit pixel it looks the screen pixel up in SpotlightClass' table of 8-bit channels, and per channel
// takes min(255, c + (c * light >> 8)) >> 2 back into 5:6:5. The original stores the light value to
// the stack a byte at a time and reads it back as a qword for the multiply, which stalls on every
// pixel. This does the same arithmetic on the same table in ordinary registers.

#include <Utilities/Macro.h>

#include <Windows.h>

#include <algorithm>
#include <emmintrin.h>

namespace
{
	__forceinline unsigned int Brighten(unsigned int channel, unsigned int light)
	{
		return std::min(255u, ((channel * light) >> 8) + channel) >> 2;
	}

	// 0x7DF280
	void __cdecl Brighten565(const BYTE* pSrc, WORD* pDst, int srcPitch, int dstPitch, int width, int height, const DWORD* pTable)
	{
		const __m128i zero = _mm_setzero_si128();
		do
		{
			const BYTE* pLight = pSrc;
			WORD* pPixel = pDst;
			int remaining = width;
			do
			{
				// Most of the square around the cone is dark; step over it sixteen at a time.
				if (remaining >= 16
					&& _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(pLight)), zero)) == 0xFFFF)
				{
					pLight += 16;
					pPixel += 16;
					remaining -= 16;
					continue;
				}

				if (const unsigned int light = *pLight)
				{
					const DWORD channels = pTable[*pPixel];
					const unsigned int blue = Brighten(channels & 0xFF, light);
					const unsigned int green = Brighten((channels >> 8) & 0xFF, light);
					const unsigned int red = Brighten((channels >> 16) & 0xFF, light);
					*pPixel = static_cast<WORD>((blue >> 1) | (green << 5) | ((red << 10) & 0xF800));
				}

				++pLight;
				++pPixel;
				--remaining;
			}
			while (remaining > 0);

			pSrc += srcPitch;
			pDst = reinterpret_cast<WORD*>(reinterpret_cast<BYTE*>(pDst) + dstPitch);
		}
		while (--height);
	}
}

DEFINE_FUNCTION_JUMP(LJMP, 0x7DF280, Brighten565);
