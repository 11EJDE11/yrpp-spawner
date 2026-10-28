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

// The blitters that draw most of a busy battlefield, one scanline per call:
//   0x497FD0 RLEBlitTransXlatAlphaZRead::Blit - building animations and other RLE shapes
//   0x4990E0 the same blitter writing Z      - terrain objects and other shapes drawn into the tile layer
//   0x494B60 BlitTransXlatAlphaZRead::Blit    - every vehicle's cached voxel image
//   0x493DF0 BlitTransXlatAlpha::Blit         - the bar behind every selected unit's health pips
//   0x491820 RLEBlitTransXlat<BYTE>::Blit     - decoding that voxel image into the 8-bit scratch surface
// The 16-bit ones light each pixel through the alpha buffer, and all but 0x493DF0 test it against the
// Z buffer first.
//
// The originals re-load the buffer globals and spill their counters to the stack every pixel; 0x493DF0
// also stores each source byte to the stack and reads it back as a dword, which stalls. These
// keep everything in registers, run through stretches of pixels where neither ring buffer can wrap
// before testing for the wrap, and fetch the next row of both buffers ahead of the next call, which
// is where the time actually went. A voxel image is mostly transparent, so the voxel blitter skips
// transparent source pixels 16 at a time. Every pixel written, every pointer step and the returned
// count are the original's.

#include <Drawing.h>

#include <Utilities/Macro.h>

#include <algorithm>
#include <emmintrin.h>
#include <intrin.h>

namespace
{
	// Scanlines are drawn top to bottom, and the next call reads the Z and alpha buffers a row further
	// on; each row start was a cache miss. A prefetch cannot fault, so a span past the end is harmless.
	template <typename Buffer>
	__forceinline void PrefetchNextRow(Buffer* pBuffer, WORD* pRow, int pixels)
	{
		WORD* pNext = pRow + pBuffer->Width;
		pBuffer->AdjustPointer(pNext);

		constexpr uintptr_t CacheLine = 64;
		const uintptr_t end = reinterpret_cast<uintptr_t>(pNext + pixels);
		for (uintptr_t line = reinterpret_cast<uintptr_t>(pNext) & ~(CacheLine - 1); line < end; line += CacheLine)
			_mm_prefetch(reinterpret_cast<const char*>(line), _MM_HINT_T0);
	}

	// How many pixels a ring-buffer pointer can step before the original's wrap test could fire.
	template <typename Buffer>
	__forceinline ptrdiff_t PixelsBeforeWrap(Buffer* pBuffer, const WORD* pRow)
	{
		const auto pTail = static_cast<const WORD*>(pBuffer->BufferTail);
		return pRow < pTail ? pTail - pRow : 1;
	}
}

// Both 16-bit blitters: the converter's palette at +4, then rows of 256 alpha levels at +8.
class AlphaBlitterPerf
{
public:
	void* VTable;
	const WORD* Palette;
	const WORD* AlphaTable;

	// (261 * max(level, 0)) >> 11 in 32-bit wrapping arithmetic, capped at 254 only from above, as
	// the originals compute it.
	const WORD* AlphaRow(int alphaLevel) const
	{
		const unsigned int level = alphaLevel <= 0 ? 0u : static_cast<unsigned int>(alphaLevel);
		const int row = std::min(static_cast<int>(level * 261u) >> 11, 254);
		return AlphaTable + row * 256;
	}

	WORD Lit(BYTE pixel, const WORD* pAlphaRow, const WORD* pA) const
	{
		return Palette[pAlphaRow[*pA] | pixel];
	}

	// 0x497FD0
	int BlitRLE(WORD* pDst, const BYTE* pSrc, int count, int skip, int zBase, WORD* pZ, WORD* pA,
		int alphaLevel, int unused, const signed char* pZShape) const
	{
		const WORD* const pAlphaRow = AlphaRow(alphaLevel);
		auto const pZBuffer = ZBuffer::Instance;
		auto const pABuffer = ABuffer::Instance;

		if (skip > 0)
		{
			int remaining = skip;
			do
			{
				if (*pSrc++)
					--remaining;
				else
					remaining -= *pSrc++;
			}
			while (remaining > 0);

			// A transparent run can carry past the clipped edge; its overshoot is skipped too,
			// without moving the Z shape pointer.
			const int overshoot = -remaining;
			pDst += overshoot;
			pZ += overshoot;
			pZBuffer->AdjustPointer(pZ);
			count -= overshoot;
			pA += overshoot;
			pABuffer->AdjustPointer(pA);
		}

		if (count <= 0)
			return count;

		PrefetchNextRow(pZBuffer, pZ, count);
		PrefetchNextRow(pABuffer, pA, count);

		for (;;)
		{
			if (const BYTE first = *pSrc++)
			{
				// Literal pixels, up to a transparent run, the end of the line or a ring buffer's end.
				const ptrdiff_t block = std::min<ptrdiff_t>(
					std::min(PixelsBeforeWrap(pZBuffer, pZ), PixelsBeforeWrap(pABuffer, pA)), count);

				BYTE pixel = first;
				ptrdiff_t done = 0;
				for (;;)
				{
					if (zBase - *pZShape++ < *pZ)
						*pDst = Lit(pixel, pAlphaRow, pA);

					++pDst;
					++pZ;
					++pA;

					if (++done == block || !*pSrc)
						break;

					pixel = *pSrc++;
				}

				count -= static_cast<int>(done);
			}
			else
			{
				const BYTE run = *pSrc++;
				count -= run;
				pZShape += run;
				pDst += run;
				pZ += run;
				pA += run;
			}

			pZBuffer->AdjustPointer(pZ);
			pABuffer->AdjustPointer(pA);

			if (count <= 0)
				return count;
		}
	}

	// 0x4990E0, the same with a Z write: a pixel in front of the Z buffer is drawn and its depth
	// stored. Returns the Z pointer where the line ended, as the original does.
	WORD* BlitRLEWriteZ(WORD* pDst, const BYTE* pSrc, int count, int skip, int zBase, WORD* pZ, WORD* pA,
		int alphaLevel, int unused, const signed char* pZShape) const
	{
		const WORD* const pAlphaRow = AlphaRow(alphaLevel);
		auto const pZBuffer = ZBuffer::Instance;
		auto const pABuffer = ABuffer::Instance;

		if (skip > 0)
		{
			int remaining = skip;
			do
			{
				if (*pSrc++)
					--remaining;
				else
					remaining -= *pSrc++;
			}
			while (remaining > 0);

			const int overshoot = -remaining;
			pDst += overshoot;
			count -= overshoot;
			pZ += overshoot;
			pZBuffer->AdjustPointer(pZ);
			pA += overshoot;
			pABuffer->AdjustPointer(pA);
		}

		if (count <= 0)
			return pZ;

		PrefetchNextRow(pZBuffer, pZ, count);
		PrefetchNextRow(pABuffer, pA, count);

		for (;;)
		{
			if (const BYTE first = *pSrc++)
			{
				const ptrdiff_t block = std::min<ptrdiff_t>(
					std::min(PixelsBeforeWrap(pZBuffer, pZ), PixelsBeforeWrap(pABuffer, pA)), count);

				BYTE pixel = first;
				ptrdiff_t done = 0;
				for (;;)
				{
					const int z = zBase - *pZShape++;
					if (z < *pZ)
					{
						*pDst = Lit(pixel, pAlphaRow, pA);
						*pZ = static_cast<WORD>(z);
					}

					++pDst;
					++pZ;
					++pA;

					if (++done == block || !*pSrc)
						break;

					pixel = *pSrc++;
				}

				count -= static_cast<int>(done);
			}
			else
			{
				const BYTE run = *pSrc++;
				count -= run;
				pZShape += run;
				pDst += run;
				pZ += run;
				pA += run;
			}

			pZBuffer->AdjustPointer(pZ);
			pABuffer->AdjustPointer(pA);

			if (count <= 0)
				return pZ;
		}
	}

	// 0x494B60
	void Blit(WORD* pDst, const BYTE* pSrc, int size, int zValue, WORD* pZ, WORD* pA, int alphaLevel, int warp) const
	{
		const WORD* const pAlphaRow = AlphaRow(alphaLevel);
		if (size <= 0)
			return;

		auto const pZBuffer = ZBuffer::Instance;
		auto const pABuffer = ABuffer::Instance;
		PrefetchNextRow(pZBuffer, pZ, size);
		PrefetchNextRow(pABuffer, pA, size);

		while (size > 0)
		{
			const ptrdiff_t block = std::min<ptrdiff_t>(
				std::min(PixelsBeforeWrap(pZBuffer, pZ), PixelsBeforeWrap(pABuffer, pA)), size);

			ptrdiff_t i = 0;
			for (; i + 16 <= block; i += 16)
			{
				const __m128i src = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pSrc + i));
				unsigned int opaque = ~_mm_movemask_epi8(_mm_cmpeq_epi8(src, _mm_setzero_si128())) & 0xFFFFu;
				while (opaque)
				{
					unsigned long bit;
					_BitScanForward(&bit, opaque);
					const ptrdiff_t at = i + static_cast<ptrdiff_t>(bit);
					opaque &= opaque - 1;
					if (zValue < pZ[at])
						pDst[at] = Lit(pSrc[at], pAlphaRow, pA + at);
				}
			}

			for (; i < block; ++i)
			{
				if (zValue < pZ[i] && pSrc[i])
					pDst[i] = Lit(pSrc[i], pAlphaRow, pA + i);
			}

			pSrc += block;
			pDst += block;
			pZ += block;
			pA += block;
			size -= static_cast<int>(block);
			pZBuffer->AdjustPointer(pZ);
			pABuffer->AdjustPointer(pA);
		}
	}

	// 0x493DF0, the same without a Z test
	void BlitNoZ(WORD* pDst, const BYTE* pSrc, int size, int zValue, WORD* pZ, WORD* pA, int alphaLevel, int warp) const
	{
		const WORD* const pAlphaRow = AlphaRow(alphaLevel);
		if (size <= 0)
			return;

		auto const pABuffer = ABuffer::Instance;
		while (size > 0)
		{
			const ptrdiff_t block = std::min<ptrdiff_t>(PixelsBeforeWrap(pABuffer, pA), size);
			for (ptrdiff_t i = 0; i < block; ++i)
			{
				if (pSrc[i])
					pDst[i] = Lit(pSrc[i], pAlphaRow, pA + i);
			}

			pSrc += block;
			pDst += block;
			pA += block;
			size -= static_cast<int>(block);
			pABuffer->AdjustPointer(pA);
		}
	}
};

// The 8-bit blitter: a byte map at +4.
class ByteBlitterPerf
{
public:
	void* VTable;
	const BYTE* Table;

	// 0x491820
	void BlitRLE(BYTE* pDst, const BYTE* pSrc, int count, int skip, int, int, int, int, int, int) const
	{
		if (skip > 0)
		{
			int remaining = skip;
			do
			{
				if (*pSrc++)
					--remaining;
				else
					remaining -= *pSrc++;
			}
			while (remaining > 0);

			pDst -= remaining;
			count += remaining;
		}

		while (count > 0)
		{
			if (const BYTE first = *pSrc++)
			{
				*pDst++ = Table[first];
				--count;

				// The rest of a literal run without going back through the run test.
				for (; count > 0 && *pSrc; --count)
					*pDst++ = Table[*pSrc++];
			}
			else
			{
				const BYTE run = *pSrc++;
				count -= run;
				pDst += run;
			}
		}
	}
};

DEFINE_FUNCTION_JUMP(LJMP, 0x497FD0, AlphaBlitterPerf::BlitRLE);
DEFINE_FUNCTION_JUMP(LJMP, 0x4990E0, AlphaBlitterPerf::BlitRLEWriteZ);
DEFINE_FUNCTION_JUMP(LJMP, 0x494B60, AlphaBlitterPerf::Blit);
DEFINE_FUNCTION_JUMP(LJMP, 0x493DF0, AlphaBlitterPerf::BlitNoZ);
DEFINE_FUNCTION_JUMP(LJMP, 0x491820, ByteBlitterPerf::BlitRLE);
