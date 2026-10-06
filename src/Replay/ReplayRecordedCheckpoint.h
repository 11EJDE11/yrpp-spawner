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

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace Replay
{
	constexpr size_t MaxRecordedCheckpoints = 4;
	constexpr size_t MaxRecordedCheckpointBytes = 16u * 1024u * 1024u;

	// How many saves the recording keeps on disk while the game runs, the newest included.
	constexpr size_t MaxStagedCheckpoints = 8;

	// Where the embedded checkpoints go, in thousandths of the recording. Later frames cost more
	// to simulate, so they are placed to minimise the expected seek cost.
	constexpr std::array<int, MaxRecordedCheckpoints> RecordedCheckpointTargetsPerMille { 375, 575, 725, 875 };

	// The dictionary probes they are compressed with. They are compressed as the recording closes,
	// after the game; 128 would make the file about 4% smaller and take two and a half times as long.
	constexpr int RecordedCheckpointProbes = 6;

	struct RecordedCheckpoint
	{
		int32_t Frame = 0;
		uint32_t RawSize = 0;
		uint32_t CRC = 0;
		std::vector<unsigned char> Compressed;
	};

	// Which staged save to drop when there are too many: the one leaving the smallest gap. The
	// newest is never dropped.
	inline size_t ChooseStagedCheckpointToEvict(const std::vector<int32_t>& frames)
	{
		size_t victim = 0;
		int32_t smallestGap = INT32_MAX;
		for (size_t i = 0; i + 1 < frames.size(); ++i)
		{
			const int32_t gap = frames[i + 1] - (i > 0 ? frames[i - 1] : 0);
			if (gap < smallestGap)
			{
				smallestGap = gap;
				victim = i;
			}
		}
		return victim;
	}

	// The staged saves nearest each target, as indices into frames. Saves past lastFrame are never chosen.
	inline std::vector<size_t> ChooseRecordedCheckpoints(const std::vector<int32_t>& frames, int32_t lastFrame)
	{
		std::vector<size_t> chosen;
		std::vector<bool> used(frames.size(), false);

		for (const int perMille : RecordedCheckpointTargetsPerMille)
		{
			const int64_t target = static_cast<int64_t>(lastFrame) * perMille / 1000;
			size_t best = frames.size();
			for (size_t i = 0; i < frames.size() && frames[i] <= lastFrame; ++i)
			{
				if (!used[i] && (best == frames.size()
					|| std::llabs(frames[i] - target) < std::llabs(frames[best] - target)))
				{
					best = i;
				}
			}

			if (best == frames.size())
				break;
			used[best] = true;
		}

		for (size_t i = 0; i < frames.size(); ++i)
		{
			if (used[i])
				chosen.push_back(i);
		}
		return chosen;
	}
}
