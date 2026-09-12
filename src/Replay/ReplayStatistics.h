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

#include "ReplayFormat.h"

#include <vector>

namespace Replay
{
	class File;
}

// Game statistics carried by a recording: a per-house economy/army sample every
// HouseStatsIntervalFrames in the frame stream, and a statistics section after it holding the type
// table, each house's end-of-game counts, and the game's own statistics packet when it built one.
// Everything is read from engine state; nothing here calls into the simulation.
namespace ReplaySystem::Statistics
{
	void Reset();

	// Called at the frame's hash site, every HouseStatsIntervalFrames, while recording.
	void FillHouseStats(std::vector<Replay::HouseStatsSample>& samples);

	// Called at the frame's hash site on every frame while recording.
	void OnFrame(int frame);

	// Appends the statistics section. Call once the frame stream is finished.
	void WriteSection(Replay::File& file);
}
