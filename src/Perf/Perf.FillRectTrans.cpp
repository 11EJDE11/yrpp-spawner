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

// DSurface::Fill_Rect_Trans (0x4BB830) blends a colour over a rectangle, the opacity in percent. The
// MPDebug panel and the replay control bar fill their backgrounds with it every frame, and the
// original blends one pixel at a time. This blends eight at a time for the 565 pixel format, and
// keeps the original's own arithmetic for any other format.
//
// Exactness: per channel the original computes ((scale * c + delta * p) >> 8) & mask with c and p
// still in place in the pixel. With scale + delta = 255, that is the same sum on the channel moved to
// bit 0, shifted back into place; it fits a 16-bit lane, and gives every pixel the original's value.

#include <Drawing.h>
#include <Surface.h>

#include <Utilities/Macro.h>

#include <algorithm>
#include <emmintrin.h>

class DSurfacePerf : public DSurface
{
public:
	bool _FillRectTrans(RectangleStruct* pRect, ColorStruct* pColor, int opacity)
	{
		// Through the vtable: YRpp's Surface virtuals are stubs a direct call could be bound to.
		Surface* const pSurface = this;

		if (pSurface->GetBytesPerPixel() < 2 || pRect->Width <= 0 || pRect->Height <= 0)
			return false;

		RectangleStruct surfaceRect;
		const RectangleStruct rect = Drawing::Intersect(*pSurface->GetRect(&surfaceRect), *pRect);
		if (rect.Width <= 0 || rect.Height <= 0)
			return false;

		auto const pBits = static_cast<WORD*>(pSurface->Lock(rect.X, rect.Y));
		if (!pBits)
			return false;

		// The original's opacity is unsigned, so a negative one is clamped to 100 too.
		const unsigned int clamped = std::min(static_cast<unsigned int>(opacity), 100u);
		const unsigned int scale = 255u * clamped / 100u;
		const WORD delta = static_cast<WORD>(255 - scale);
		const WORD color = static_cast<WORD>(
			((pColor->R >> RGBClass::RedShiftRight) << RGBClass::RedShiftLeft)
			| ((pColor->G >> RGBClass::GreenShiftRight) << RGBClass::GreenShiftLeft)
			| ((pColor->B >> RGBClass::BlueShiftRight) << RGBClass::BlueShiftLeft));
		const int stride = pSurface->GetPitch() / 2;

		const bool is565 = RGBClass::RedShiftLeft == 11 && RGBClass::GreenShiftLeft == 5 && RGBClass::BlueShiftLeft == 0
			&& RGBClass::RedShiftRight == 3 && RGBClass::GreenShiftRight == 2 && RGBClass::BlueShiftRight == 3;

		if (is565)
			Blend565(pBits, stride, rect.Width, rect.Height, color, scale, delta);
		else
			BlendAnyFormat(pBits, stride, rect.Width, rect.Height, color, scale, delta);

		pSurface->Unlock();
		return true;
	}

private:
	static void Blend565(WORD* pBits, int stride, int width, int height, WORD color, unsigned int scale, WORD delta)
	{
		const WORD red = static_cast<WORD>(scale * (color >> 11));
		const WORD green = static_cast<WORD>(scale * ((color >> 5) & 0x3F));
		const WORD blue = static_cast<WORD>(scale * (color & 0x1F));

		const __m128i vDelta = _mm_set1_epi16(static_cast<short>(delta));
		const __m128i vRed = _mm_set1_epi16(static_cast<short>(red));
		const __m128i vGreen = _mm_set1_epi16(static_cast<short>(green));
		const __m128i vBlue = _mm_set1_epi16(static_cast<short>(blue));
		const __m128i greenMask = _mm_set1_epi16(0x3F);
		const __m128i blueMask = _mm_set1_epi16(0x1F);

		for (int y = 0; y < height; ++y)
		{
			WORD* const pRow = pBits + y * stride;
			int x = 0;
			for (; x + 8 <= width; x += 8)
			{
				const __m128i pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pRow + x));
				__m128i r = _mm_srli_epi16(pixels, 11);
				__m128i g = _mm_and_si128(_mm_srli_epi16(pixels, 5), greenMask);
				__m128i b = _mm_and_si128(pixels, blueMask);
				r = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(r, vDelta), vRed), 8);
				g = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(g, vDelta), vGreen), 8);
				b = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(b, vDelta), vBlue), 8);
				const __m128i blended = _mm_or_si128(_mm_or_si128(_mm_slli_epi16(r, 11), _mm_slli_epi16(g, 5)), b);
				_mm_storeu_si128(reinterpret_cast<__m128i*>(pRow + x), blended);
			}

			for (; x < width; ++x)
			{
				const unsigned int p = pRow[x];
				pRow[x] = static_cast<WORD>((((red + delta * (p >> 11)) >> 8) << 11)
					| (((green + delta * ((p >> 5) & 0x3F)) >> 8) << 5)
					| ((blue + delta * (p & 0x1F)) >> 8));
			}
		}
	}

	// The original's loop.
	static void BlendAnyFormat(WORD* pBits, int stride, int width, int height, WORD color, unsigned int scale, WORD delta)
	{
		const unsigned int redMask = 0xFFu >> RGBClass::RedShiftRight << RGBClass::RedShiftLeft;
		const unsigned int greenMask = 0xFFu >> RGBClass::GreenShiftRight << RGBClass::GreenShiftLeft;
		const unsigned int blueMask = 0xFFu >> RGBClass::BlueShiftRight << RGBClass::BlueShiftLeft;
		const unsigned int red = scale * (color & redMask);
		const unsigned int green = scale * (color & greenMask);
		const unsigned int blue = scale * (color & blueMask);

		for (int y = 0; y < height; ++y)
		{
			WORD* const pRow = pBits + y * stride;
			for (int x = 0; x < width; ++x)
			{
				const unsigned int p = pRow[x];
				pRow[x] = static_cast<WORD>((greenMask & ((green + delta * (p & greenMask)) >> 8))
					| (redMask & ((red + delta * (p & redMask)) >> 8))
					| ((blue + delta * (p & blueMask)) >> 8));
			}
		}
	}
};

DEFINE_FUNCTION_JUMP(LJMP, 0x4BB830, DSurfacePerf::_FillRectTrans);
