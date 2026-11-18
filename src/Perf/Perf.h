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

// Engine speed-ups that change nothing the game computes, only how fast it computes it. Each one is a
// drop-in replacement with the original's exact results and side effects, so a game stays in sync
// with players who do not have them. Cell_Shadow, the blitters and Fill_Rect_Trans are static jumps,
// the tile drawer's row loop is a hook; the MapRevealer takeover has to wait for Syringe's hooks and
// check which Ares it is replacing. The one exception is LightRedraw, which redraws less of the screen
// when a light changes: it changes nothing the game computes, but its picture can differ slightly.
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
}
