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

namespace ReplaySystem
{
	namespace SideChannels
	{
		// Called by the replay lifecycle; pending captures never carry into another session.
		void Reset();

		// Consumes records up to this frame, retaining the existing per-frame limit and order.
		// The returned buffer is reused by the next drain or reset.
		const std::vector<Replay::SideChannelRecord>& DrainThroughFrame(int frameNumber);

		// The codec calls this after reading a complete record. Checks that depend on engine
		// arrays and terminates fixed text buffers before playback can use them.
		bool ValidateRecord(Replay::SideChannelRecord& record);

		// Seeking suppresses transient chat/taunt effects but still rebuilds beacon state.
		void ApplyRecord(const Replay::SideChannelRecord& record, bool visible, bool seeking);
	}
}
