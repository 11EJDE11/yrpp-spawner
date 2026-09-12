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

#include <cstdint>
#include <vector>

namespace Replay
{
	constexpr size_t MaxRecordedCheckpoints = 4;
	constexpr size_t MaxRecordedCheckpointBytes = 16u * 1024u * 1024u;
	struct RecordedCheckpoint
	{
		int32_t Frame = 0;
		uint32_t RawSize = 0;
		uint32_t CRC = 0;
		std::vector<unsigned char> Compressed;
	};

	inline void TrimRecordedCheckpoints(std::vector<RecordedCheckpoint>& records)
	{
		for (;;)
		{
			size_t total = 0;
			for (const auto& item : records) total += item.Compressed.size();
			if (records.size() <= MaxRecordedCheckpoints && total <= MaxRecordedCheckpointBytes)
				return;

			// Keep the first and latest saves while thinning densely sampled intervals.
			// If even those two exceed the byte budget, prefer the latest save.
			size_t victim = 0;
			if (records.size() > 2)
			{
				victim = 1;
				for (size_t i = 2; i + 1 < records.size(); ++i)
					if (records[i + 1].Frame - records[i - 1].Frame
						< records[victim + 1].Frame - records[victim - 1].Frame)
						victim = i;
			}
			records.erase(records.begin() + victim);
		}
	}
}
