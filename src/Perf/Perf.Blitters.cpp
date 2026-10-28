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
//   0x4984D0/0x4986D0/0x4988C0 RLEBlitTransLucent75/50/25AlphaZRead::Blit - smoke and missile trails
//   0x4950C0/0x495250 BlitTransLucent50/25AlphaZRead::Blit - the same for shapes that are not RLE
//   0x496820 RLEBlitTransDarkenZRead::Blit    - the shadows of buildings, infantry and other shapes
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
#include <cstring>
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

// The translucent RLE blitters - smoke, missile trails, explosions: the lit pixel blended with the one
// under it. The game blends through the converter's shift mask at +0xC in 16-bit arithmetic. Phobos'
// translucency fix (BlittersFix.cpp, applied when its INI asks for it) rewrites that blend in place
// with a rounding average in 32-bit arithmetic, whose mask register keeps the Z value's upper half.
// Which one is installed is read from the code itself, and anything else runs the original.
namespace Lucent
{
	enum class Level { Lucent75, Lucent50, Lucent25 };
	enum class Blend { Unread, Game, Phobos, Other };

	struct Site
	{
		uintptr_t Address;
		size_t Size;
		const BYTE* Game;
		const BYTE* Phobos;
	};

	constexpr BYTE Game75[] = {
		0x83, 0xC7, 0x02, 0x66, 0x8B, 0x70, 0x0C, 0x66, 0x8B, 0x0A, 0x33, 0xC0, 0x66, 0x8B, 0x44, 0x4D, 0x00,
		0x8B, 0x4C, 0x24, 0x18, 0x81, 0xE1, 0xFF, 0x00, 0x00, 0x00, 0x8B, 0x6C, 0x24, 0x2C, 0x0B, 0xC1, 0x8B,
		0x4C, 0x24, 0x10, 0x8B, 0x49, 0x04, 0x66, 0x8B, 0x0C, 0x41, 0x66, 0x8B, 0x47, 0xFE, 0x66, 0xC1, 0xE8,
		0x02, 0x23, 0xC6, 0x66, 0xC1, 0xE9, 0x02, 0x23, 0xCE, 0x8D, 0x04, 0x40, 0x03, 0xC8, 0x8B, 0x44, 0x24,
		0x20, 0x66, 0x89, 0x4F, 0xFE };
	constexpr BYTE Phobos75[] = {
		0x66, 0xBE, 0xDE, 0xF7, 0x66, 0x8B, 0x0A, 0x31, 0xC0, 0x66, 0x8B, 0x44, 0x4D, 0x00, 0x8B, 0x4C, 0x24,
		0x18, 0x81, 0xE1, 0xFF, 0x00, 0x00, 0x00, 0x8B, 0x6C, 0x24, 0x2C, 0x09, 0xC8, 0x8B, 0x4C, 0x24, 0x10,
		0x8B, 0x49, 0x04, 0x8B, 0x04, 0x41, 0x89, 0xC1, 0x33, 0x0F, 0x21, 0xF1, 0xD1, 0xE9, 0x0B, 0x07, 0x29,
		0xC8, 0x89, 0xC1, 0x33, 0x07, 0x09, 0xC1, 0x21, 0xF0, 0xD1, 0xE8, 0x29, 0xC1, 0x8B, 0x44, 0x24, 0x20,
		0x66, 0x89, 0x0F, 0x47, 0x47 };
	constexpr BYTE Game25[] = {
		0x83, 0xC7, 0x02, 0x66, 0x8B, 0x70, 0x0C, 0x66, 0x8B, 0x0A, 0x33, 0xC0, 0x66, 0x8B, 0x44, 0x4D, 0x00,
		0x8B, 0x4C, 0x24, 0x18, 0x81, 0xE1, 0xFF, 0x00, 0x00, 0x00, 0x8B, 0x6C, 0x24, 0x2C, 0x0B, 0xC1, 0x8B,
		0x4C, 0x24, 0x10, 0x8B, 0x49, 0x04, 0x66, 0x8B, 0x04, 0x41, 0x66, 0x8B, 0x4F, 0xFE, 0x66, 0xC1, 0xE8,
		0x02, 0x23, 0xC6, 0x66, 0xC1, 0xE9, 0x02, 0x8D, 0x04, 0x40, 0x23, 0xCE, 0x03, 0xC1, 0x66, 0x89, 0x47,
		0xFE };
	constexpr BYTE Phobos25[] = {
		0x66, 0xBE, 0xDE, 0xF7, 0x66, 0x8B, 0x0A, 0x31, 0xC0, 0x66, 0x8B, 0x44, 0x4D, 0x00, 0x8B, 0x4C, 0x24,
		0x18, 0x81, 0xE1, 0xFF, 0x00, 0x00, 0x00, 0x8B, 0x6C, 0x24, 0x2C, 0x09, 0xC8, 0x8B, 0x4C, 0x24, 0x10,
		0x8B, 0x49, 0x04, 0x8B, 0x04, 0x41, 0x89, 0xC1, 0x33, 0x0F, 0x21, 0xF1, 0xD1, 0xE9, 0x50, 0x0B, 0x07,
		0x29, 0xC8, 0x59, 0x31, 0xC1, 0x09, 0xC8, 0x21, 0xF1, 0xD1, 0xE9, 0x29, 0xC8, 0x66, 0x89, 0x07, 0x47,
		0x47 };
	constexpr BYTE Game50Mask[] = { 0x66, 0x8B, 0x50, 0x0C };
	constexpr BYTE Phobos50Mask[] = { 0x66, 0xBA, 0xDE, 0xF7 };
	constexpr BYTE Game50Blend[] = {
		0x66, 0x8B, 0x04, 0x41, 0x66, 0x8B, 0x0B, 0x66, 0xD1, 0xE8, 0x66, 0xD1, 0xE9, 0x23, 0xC2, 0x23, 0xCA,
		0x03, 0xC1 };
	constexpr BYTE Phobos50Blend[] = {
		0x8B, 0x04, 0x41, 0x89, 0xC1, 0x33, 0x0B, 0x21, 0xD1, 0xD1, 0xE9, 0x0B, 0x03, 0x29, 0xC8, 0x90, 0x90,
		0x90, 0x90 };
	static_assert(sizeof(Game75) == sizeof(Phobos75) && sizeof(Game25) == sizeof(Phobos25)
		&& sizeof(Game50Blend) == sizeof(Phobos50Blend));

	struct Variant
	{
		uintptr_t Function;
		Site Sites[2];
		int SiteCount;
		DWORD Key[2];
		Blend Installed;
	};

	Variant Variants[3] = {
		{ 0x4984D0, { { 0x4985FE, sizeof(Game75), Game75, Phobos75 } }, 1 },
		{ 0x4986D0, { { 0x4987F7, sizeof(Game50Mask), Game50Mask, Phobos50Mask }, { 0x498817, sizeof(Game50Blend), Game50Blend, Phobos50Blend } }, 2 },
		{ 0x4988C0, { { 0x4989EE, sizeof(Game25), Game25, Phobos25 } }, 1 },
	};

	// Which blend the code holds now. A patch site's first four bytes are checked every call, and the
	// whole site again whenever they change.
	Blend Installed(Variant& variant)
	{
		bool same = variant.Installed != Blend::Unread;
		for (int i = 0; i < variant.SiteCount; ++i)
			same = same && *reinterpret_cast<const DWORD*>(variant.Sites[i].Address) == variant.Key[i];

		if (same)
			return variant.Installed;

		bool game = true, phobos = true;
		for (int i = 0; i < variant.SiteCount; ++i)
		{
			auto const& site = variant.Sites[i];
			auto const pCode = reinterpret_cast<const BYTE*>(site.Address);
			game = game && !memcmp(pCode, site.Game, site.Size);
			phobos = phobos && !memcmp(pCode, site.Phobos, site.Size);
			variant.Key[i] = *reinterpret_cast<const DWORD*>(site.Address);
		}

		variant.Installed = game ? Blend::Game : phobos ? Blend::Phobos : Blend::Other;
		return variant.Installed;
	}

	template <Level level, Blend blend>
	__forceinline WORD Mix(const WORD* pLit, const WORD* pUnder, int z, WORD mask)
	{
		if constexpr (blend == Blend::Game)
		{
			const WORD lit = *pLit, under = *pUnder;
			if constexpr (level == Level::Lucent75)
				return static_cast<WORD>(((lit >> 2) & mask) + 3 * ((under >> 2) & mask));
			else if constexpr (level == Level::Lucent50)
				return static_cast<WORD>(((lit >> 1) & mask) + ((under >> 1) & mask));
			else
				return static_cast<WORD>(3 * ((lit >> 2) & mask) + ((under >> 2) & mask));
		}
		else if constexpr (level == Level::Lucent50)
		{
			// The mask register's upper half is clear here.
			const unsigned int lit = *pLit, under = *pUnder;
			return static_cast<WORD>((lit | under) - (((lit ^ under) & 0xF7DEu) >> 1));
		}
		else
		{
			// Both pixels are read as dwords, so the next pixel's low bit reaches bit 15 through the
			// shift when the Z value is negative.
			const unsigned int m = (static_cast<unsigned int>(z) & 0xFFFF0000u) | 0xF7DEu;
			const unsigned int lit = *reinterpret_cast<const unsigned int*>(pLit);
			const unsigned int under = *reinterpret_cast<const unsigned int*>(pUnder);
			const unsigned int half = (lit | under) - (((lit ^ under) & m) >> 1);
			if constexpr (level == Level::Lucent75)
				return static_cast<WORD>((half | under) - (((half ^ under) & m) >> 1));
			else
				return static_cast<WORD>((half | lit) - (((lit ^ half) & m) >> 1));
		}
	}
}

class LucentBlitterPerf
{
public:
	void* VTable;
	const WORD* Palette;
	const WORD* AlphaTable;
	WORD Mask;

	using Function = int(__thiscall*)(const LucentBlitterPerf*, WORD*, const BYTE*, int, int, int, WORD*, WORD*, int, int, const signed char*);

	template <Lucent::Level level, Lucent::Blend blend>
	int BlitRLE(WORD* pDst, const BYTE* pSrc, int count, int skip, int zBase, WORD* pZ, WORD* pA,
		int alphaLevel, int unused, const signed char* pZShape) const
	{
		const WORD* const pAlphaRow = reinterpret_cast<const AlphaBlitterPerf*>(this)->AlphaRow(alphaLevel);
		auto const pZBuffer = ZBuffer::Instance;
		auto const pABuffer = ABuffer::Instance;
		const WORD mask = Mask;

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
			return count;

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
						*pDst = Lucent::Mix<level, blend>(&Palette[pAlphaRow[*pA] | pixel], pDst, z, mask);

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

	// The original, for a blend this does not know: its first five bytes (push ecx; mov eax,
	// [esp+2Ch]), which the jump here replaced, then on into the rest of it.
	static Function Original(int index, uintptr_t function)
	{
		static Function trampolines[3] {};
		if (!trampolines[index])
		{
			auto const pCode = static_cast<BYTE*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
			const BYTE entry[] = { 0x51, 0x8B, 0x44, 0x24, 0x2C, 0xE9 };
			memcpy(pCode, entry, sizeof(entry));
			*reinterpret_cast<int32_t*>(pCode + 6) = static_cast<int32_t>(function + 5 - reinterpret_cast<uintptr_t>(pCode + 10));
			trampolines[index] = reinterpret_cast<Function>(pCode);
		}

		return trampolines[index];
	}

	template <Lucent::Level level>
	__forceinline int Dispatch(WORD* pDst, const BYTE* pSrc, int count, int skip, int zBase, WORD* pZ, WORD* pA,
		int alphaLevel, int unused, const signed char* pZShape) const
	{
		constexpr int index = static_cast<int>(level);
		auto& variant = Lucent::Variants[index];
		switch (Lucent::Installed(variant))
		{
		case Lucent::Blend::Game:
			return BlitRLE<level, Lucent::Blend::Game>(pDst, pSrc, count, skip, zBase, pZ, pA, alphaLevel, unused, pZShape);
		case Lucent::Blend::Phobos:
			return BlitRLE<level, Lucent::Blend::Phobos>(pDst, pSrc, count, skip, zBase, pZ, pA, alphaLevel, unused, pZShape);
		default:
			return Original(index, variant.Function)(this, pDst, pSrc, count, skip, zBase, pZ, pA, alphaLevel, unused, pZShape);
		}
	}

	// 0x4984D0, 0x4986D0, 0x4988C0: RLEBlitTransLucent75/50/25AlphaZRead::Blit
	int BlitRLE75(WORD* pDst, const BYTE* pSrc, int count, int skip, int zBase, WORD* pZ, WORD* pA,
		int alphaLevel, int unused, const signed char* pZShape) const
	{
		return Dispatch<Lucent::Level::Lucent75>(pDst, pSrc, count, skip, zBase, pZ, pA, alphaLevel, unused, pZShape);
	}

	int BlitRLE50(WORD* pDst, const BYTE* pSrc, int count, int skip, int zBase, WORD* pZ, WORD* pA,
		int alphaLevel, int unused, const signed char* pZShape) const
	{
		return Dispatch<Lucent::Level::Lucent50>(pDst, pSrc, count, skip, zBase, pZ, pA, alphaLevel, unused, pZShape);
	}

	int BlitRLE25(WORD* pDst, const BYTE* pSrc, int count, int skip, int zBase, WORD* pZ, WORD* pA,
		int alphaLevel, int unused, const signed char* pZShape) const
	{
		return Dispatch<Lucent::Level::Lucent25>(pDst, pSrc, count, skip, zBase, pZ, pA, alphaLevel, unused, pZShape);
	}

	// The same blends for shapes that are not RLE (0x4950C0, 0x495250: BlitTransLucent50/25AlphaZRead::
	// Blit), whose originals store each source byte to the stack and read it back as a dword, and keep
	// the source pointer and the count on the stack. The 75% one (0x494F20) is left alone: no replay
	// used it to check a replacement against. Nothing patches these that this
	// knows of, so the routine is checked against the game's own bytes, and anything else runs it.
	struct PlainVariant
	{
		uintptr_t Function;
		size_t Size;
		DWORD Hash;
		uintptr_t BlendAt;
		DWORD BlendKey;
		Lucent::Blend Installed;
	};

	static inline PlainVariant PlainVariants[3] = {
		{ 0x494F20, 0x10B, 0x1F6AE85A, 0x494FCC },
		{ 0x4950C0, 0x100, 0x5D7C34C1, 0x495169 },
		{ 0x495250, 0x10B, 0x8BF6835B, 0x4952FC },
	};

	// Unchanged from the game's, past the first eight bytes (the jump here and the rest of the
	// instruction it cuts into). Hashed again whenever the blend's first bytes change.
	static Lucent::Blend PlainInstalled(PlainVariant& variant)
	{
		const DWORD key = *reinterpret_cast<const DWORD*>(variant.BlendAt);
		if (variant.Installed != Lucent::Blend::Unread && key == variant.BlendKey)
			return variant.Installed;

		DWORD hash = 0x811C9DC5u;
		for (auto p = reinterpret_cast<const BYTE*>(variant.Function) + 8; p < reinterpret_cast<const BYTE*>(variant.Function + variant.Size); ++p)
			hash = (hash ^ *p) * 0x01000193u;

		variant.BlendKey = key;
		variant.Installed = hash == variant.Hash ? Lucent::Blend::Game : Lucent::Blend::Other;
		return variant.Installed;
	}

	using PlainFunction = void(__thiscall*)(const LucentBlitterPerf*, WORD*, const BYTE*, int, int, WORD*, WORD*, int, int);

	// Its first eight bytes (four pushes and a mov), then on into the rest of it.
	static PlainFunction PlainOriginal(int index)
	{
		static PlainFunction trampolines[3] {};
		if (!trampolines[index])
		{
			const uintptr_t function = PlainVariants[index].Function;
			auto const pCode = static_cast<BYTE*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
			const BYTE entry[3][8] = {
				{ 0x51, 0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x2C },
				{ 0x51, 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x2C },
				{ 0x51, 0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x2C },
			};
			memcpy(pCode, entry[index], 8);
			pCode[8] = 0xE9;
			*reinterpret_cast<int32_t*>(pCode + 9) = static_cast<int32_t>(function + 8 - reinterpret_cast<uintptr_t>(pCode + 13));
			trampolines[index] = reinterpret_cast<PlainFunction>(pCode);
		}

		return trampolines[index];
	}

	template <Lucent::Level level>
	void BlitPlain(WORD* pDst, const BYTE* pSrc, int size, int zValue, WORD* pZ, WORD* pA, int alphaLevel, int warp) const
	{
		constexpr int index = static_cast<int>(level);
		if (PlainInstalled(PlainVariants[index]) != Lucent::Blend::Game)
			return PlainOriginal(index)(this, pDst, pSrc, size, zValue, pZ, pA, alphaLevel, warp);

		const WORD* const pAlphaRow = reinterpret_cast<const AlphaBlitterPerf*>(this)->AlphaRow(alphaLevel);
		if (size <= 0)
			return;

		const WORD mask = Mask;
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
						pDst[at] = Lucent::Mix<level, Lucent::Blend::Game>(&Palette[pAlphaRow[pA[at]] | pSrc[at]], pDst + at, 0, mask);
				}
			}

			for (; i < block; ++i)
			{
				if (zValue < pZ[i] && pSrc[i])
					pDst[i] = Lucent::Mix<level, Lucent::Blend::Game>(&Palette[pAlphaRow[pA[i]] | pSrc[i]], pDst + i, 0, mask);
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

	void Blit50(WORD* pDst, const BYTE* pSrc, int size, int zValue, WORD* pZ, WORD* pA, int alphaLevel, int warp) const
	{
		BlitPlain<Lucent::Level::Lucent50>(pDst, pSrc, size, zValue, pZ, pA, alphaLevel, warp);
	}

	void Blit25(WORD* pDst, const BYTE* pSrc, int size, int zValue, WORD* pZ, WORD* pA, int alphaLevel, int warp) const
	{
		BlitPlain<Lucent::Level::Lucent25>(pDst, pSrc, size, zValue, pZ, pA, alphaLevel, warp);
	}
};

// RLEBlitTransDarkenZRead::Blit (0x496820) - the shadows of buildings, infantry and other shapes: a
// shadow pixel in front of the Z buffer halves the pixel under it through the mask at +4. The original
// re-reads the Z buffer global, its own pointer and the Z base from memory for every pixel.
class DarkenBlitterPerf
{
public:
	void* VTable;
	WORD Mask;

	// Returns what the original leaves in eax: the Z buffer after drawing anything, the Z pointer when
	// only skipping, or the skip count when there was nothing to do.
	int BlitRLE(WORD* pDst, const BYTE* pSrc, int count, int skip, int zBase, WORD* pZ, WORD* pA,
		int alphaLevel, int unused, const signed char* pZShape) const
	{
		auto const pZBuffer = ZBuffer::Instance;
		int result = skip;

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
			result = reinterpret_cast<int>(pZ);
		}

		if (count <= 0)
			return result;

		const WORD mask = Mask;
		for (;;)
		{
			if (const BYTE first = *pSrc++)
			{
				const ptrdiff_t block = std::min<ptrdiff_t>(PixelsBeforeWrap(pZBuffer, pZ), count);
				ptrdiff_t done = 0;
				for (;;)
				{
					if (zBase - *pZShape++ < *pZ)
						*pDst = static_cast<WORD>((*pDst >> 1) & mask);

					++pDst;
					++pZ;

					if (++done == block || !*pSrc)
						break;

					++pSrc;
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
			}

			pZBuffer->AdjustPointer(pZ);

			if (count <= 0)
				return reinterpret_cast<int>(pZBuffer);
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

		// Decoding a cached voxel image goes through a table that maps every byte to itself, so a
		// literal run is copied whole; any other table is looked up a byte at a time.
		if (IsIdentity(Table))
		{
			while (count > 0)
			{
				if (*pSrc)
				{
					const int length = LiteralLength(pSrc, count);
					memcpy(pDst, pSrc, static_cast<size_t>(length));
					pSrc += length;
					pDst += length;
					count -= length;
				}
				else
				{
					const BYTE run = pSrc[1];
					pSrc += 2;
					count -= run;
					pDst += run;
				}
			}

			return;
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

private:
	// Whether the table maps every byte to itself, checked again every 1024 calls.
	static bool IsIdentity(const BYTE* pTable)
	{
		static const BYTE* pChecked = nullptr;
		static bool identity = false;
		static unsigned int calls = 0;
		if (pTable != pChecked || (++calls & 1023) == 0)
		{
			const __m128i step = _mm_set1_epi8(16);
			__m128i expected = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
			int same = 0xFFFF;
			for (int i = 0; i < 256; i += 16)
			{
				same &= _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(pTable + i)), expected));
				expected = _mm_add_epi8(expected, step);
			}

			pChecked = pTable;
			identity = same == 0xFFFF;
		}

		return identity;
	}

	// How many bytes from pSrc on, up to limit, are not zero. Reads 16 at a time where that cannot
	// cross into the next page.
	static int LiteralLength(const BYTE* pSrc, int limit)
	{
		int length = 0;
		while (length < limit)
		{
			const BYTE* const p = pSrc + length;
			if ((reinterpret_cast<uintptr_t>(p) & 0xFFF) <= 0xFF0)
			{
				const unsigned int zero = _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), _mm_setzero_si128()));
				if (zero)
				{
					unsigned long bit;
					_BitScanForward(&bit, zero);
					return std::min(length + static_cast<int>(bit), limit);
				}

				length += 16;
			}
			else
			{
				if (!*p)
					return length;
				++length;
			}
		}

		return limit;
	}
};

DEFINE_FUNCTION_JUMP(LJMP, 0x497FD0, AlphaBlitterPerf::BlitRLE);
DEFINE_FUNCTION_JUMP(LJMP, 0x4990E0, AlphaBlitterPerf::BlitRLEWriteZ);
DEFINE_FUNCTION_JUMP(LJMP, 0x494B60, AlphaBlitterPerf::Blit);
DEFINE_FUNCTION_JUMP(LJMP, 0x493DF0, AlphaBlitterPerf::BlitNoZ);
DEFINE_FUNCTION_JUMP(LJMP, 0x491820, ByteBlitterPerf::BlitRLE);
DEFINE_FUNCTION_JUMP(LJMP, 0x4984D0, LucentBlitterPerf::BlitRLE75);
DEFINE_FUNCTION_JUMP(LJMP, 0x4986D0, LucentBlitterPerf::BlitRLE50);
DEFINE_FUNCTION_JUMP(LJMP, 0x4988C0, LucentBlitterPerf::BlitRLE25);
DEFINE_FUNCTION_JUMP(LJMP, 0x4950C0, LucentBlitterPerf::Blit50);
DEFINE_FUNCTION_JUMP(LJMP, 0x495250, LucentBlitterPerf::Blit25);
DEFINE_FUNCTION_JUMP(LJMP, 0x496820, DarkenBlitterPerf::BlitRLE);
