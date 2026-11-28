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

#pragma once

#include <Windows.h>

#include <cstddef>

// Engine speed-ups that change nothing the game computes, only how fast it computes it. Each one is a
// drop-in replacement with the original's exact results and side effects, so a game stays in sync
// with players who do not have them. Cell_Shadow, the blitters, Fill_Rect_Trans and the end-of-frame
// delete sweep are static jumps; the tile drawer's row loop and the removal announcements are hooks;
// the MapRevealer, EmptyCells and Weapons takeovers wait for Syringe's hooks and check the code they
// replace, the hooks in it included. Two exceptions: LightRedraw redraws less of the screen when a
// light changes, so its picture can differ slightly; LightRetint is a fix as well as a speed-up (a
// retint no longer leaves relit cells making new tile drawers for the rest of the game), and it only
// touches drawing.
//
// Measured together (each file's head comment has its own figures). The Invasion replay, 8 players and
// 109,527 frames, every frame drawn: played back in 634.6 s without any of the drawing and shroud
// speed-ups and 443.3 s with them, drawing 3.06 -> 1.75 ms a frame and logic 2.21 -> 1.80 ms. Ten
// ladder games with drawing on and the recorded camera, everything here switched in and out every 250
// frames: 1.45x to 2.00x.

namespace Perf
{
	namespace MapRevealer { void Apply(); }
	namespace EmptyCells { void Apply(); }
	namespace Weapons { void Apply(); }
	namespace Detach { void Check(); }

	// A Syringe hook as a DLL lists it in its hook table: where, how many bytes, whose and which.
	struct KnownHook { DWORD Address; DWORD Length; const wchar_t* Module; const char* Name; };

	// Whether every hook any loaded DLL lists in [start, end) is one of the known ones. Logs the first
	// that is not, naming what it was checked for.
	bool OnlyKnownHooks(DWORD start, DWORD end, const KnownHook* known, size_t count, const char* what);

	template <size_t N>
	bool OnlyKnownHooks(DWORD start, DWORD end, const KnownHook(&known)[N], const char* what)
	{
		return OnlyKnownHooks(start, end, known, N, what);
	}

	// Whether that DLL lists a hook of that name at that address.
	bool HookListed(const wchar_t* module, DWORD address, DWORD length, const char* name);

	// Whether one of the known hooks covers that byte of the game's code.
	template <size_t N>
	bool InKnownHook(DWORD address, const KnownHook(&known)[N])
	{
		for (auto const& hook : known)
		{
			if (address >= hook.Address && address < hook.Address + hook.Length)
				return true;
		}

		return false;
	}
}
