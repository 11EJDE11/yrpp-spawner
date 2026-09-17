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

// Where every unit, infantryman, aircraft and building is, for readers outside the game - the
// analyser's map. Written as changes every ObjectSnapshotIntervalFrames: objects that appeared or
// changed owner, objects that moved or changed state, and objects that left, with who destroyed them.
// Everything is a plain read of object state; nothing here calls into the simulation.
namespace ReplaySystem::Objects
{
	void Reset();

	// Called at the frame's hash site every ObjectSnapshotIntervalFrames while recording.
	void FillSnapshot(std::vector<Replay::ObjectAppearRecord>& appeared,
		std::vector<Replay::ObjectUpdateRecord>& updated,
		std::vector<Replay::ObjectGoneRecord>& gone);

	// Hands a written capture's buffers back, so the next snapshot fills them instead of growing new ones.
	void RecycleBuffers(std::vector<Replay::ObjectAppearRecord>& appeared,
		std::vector<Replay::ObjectUpdateRecord>& updated,
		std::vector<Replay::ObjectGoneRecord>& gone);
}
