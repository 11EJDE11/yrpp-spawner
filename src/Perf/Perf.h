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
// with players who do not have them. Cell_Shadow and the blitters are static jumps, the tile drawer's
// row loop is a hook; the MapRevealer takeover has to wait for Syringe's hooks and check which Ares it
// is replacing.

namespace Perf
{
	namespace MapRevealer { void Apply(); }
}
