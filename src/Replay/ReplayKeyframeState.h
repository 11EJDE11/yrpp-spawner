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

#include <memory>
#include <vector>

namespace ReplaySystem::KeyframeState
{

	namespace Detail
	{
		struct SnapshotData;
	}

	// Sidecar for state the engine savegame omits or changes while loading. It stays
	// in memory during playback and can be serialized alongside a recorded save.
	// Object references are captured as IDs and resolved against the loaded world.
	// This supplements the general save/load fixes with the exact keyframe values.
	class Snapshot
	{
	public:
		Snapshot();
		~Snapshot();
		Snapshot(Snapshot&&) noexcept;
		Snapshot& operator=(Snapshot&&) noexcept;
		Snapshot(const Snapshot&) = delete;
		Snapshot& operator=(const Snapshot&) = delete;

		bool Serialize(std::vector<unsigned char>& bytes) const;
		bool Deserialize(const std::vector<unsigned char>& bytes);

		// SaveGame must have completed before this is called.
		bool CaptureAfterSave();

		// LoadMission and transient event-queue cleanup must have completed.
		// Restores the frame counter, scenario IDs/RNG, collection order, and world state.
		bool RestoreBeforeResume(int keyframeFrame) const;

		// Run after Session::Resume and replay spectator setup.
		void RestoreAfterResume(int keyframeFrame, bool showBeacons) const;

	private:
		std::unique_ptr<Detail::SnapshotData> Data;
	};
}
