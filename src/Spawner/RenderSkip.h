/**
*  yrpp-spawner
*
*  Copyright(C) 2023-present CnCNet
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

/**
*  RenderSkip - drops one render frame in three while this client cannot keep
*  up: its average frame reaches the budget (MinProcessMs, or the frame time at
*  the requested rate if longer) and rendering is at least RenderSharePercent
*  of that frame. Multiplayer only.
*
*  A client that can't keep up stalls every peer through the MaxAhead gate.
*  Skipping renders is sync-safe: the engine already skips rendering whenever
*  the window loses focus.
*/

#pragma once

class RenderSkip
{
public:
	static bool Enabled;

	// Minimum per-frame budget in milliseconds; the budget is otherwise the
	// frame time at the requested rate.
	static int MinProcessMs;

	// Percent of the frame rendering must take before throttling. 0 disables the check.
	static int RenderSharePercent;

	static void Reset();

	// One measured render, in microseconds.
	static void NoteRenderCost(int us);

	// Called in place of Main_Loop's render. Advances the throttle state.
	static bool ShouldRenderThisFrame();

	static bool ThrottleActive();
};
