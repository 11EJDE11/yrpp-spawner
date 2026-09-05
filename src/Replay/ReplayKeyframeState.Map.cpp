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

#include "ReplayKeyframeState.Internal.h"

#include <Utilities/Debug.h>

#include <AStarClass.h>
#include <FootClass.h>
#include <Memory.h>
#include <TiberiumClass.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <utility>

namespace ReplaySystem::KeyframeState::Detail
{
	#pragma region Cell passability

	// Passability is not serialized. Preserve the live values, including the map border,
	// so the pathfinder sees the same blocked cells after loading.
	void CaptureCellPassability(std::vector<unsigned char>& out)
	{
		const int count = std::max(std::min(MapClass::Instance.MaxNumCells, MapClass::MaxCells), 0);
		out.assign(static_cast<size_t>(count), 0u);

		for (int i = 0; i < count; ++i)
		{
			if (const auto* const pCell = MapClass::Instance.Cells[i])
				out[static_cast<size_t>(i)] = static_cast<unsigned char>(pCell->Passability);
		}
	}

	// Not fatal to a seek: the routes near the map edge going wrong is a slow divergence, and dropping
	// the viewer back to no playback at all over it would be the worse trade.
	void RestoreCellPassability(const std::vector<unsigned char>& passability, int keyframeFrame)
	{
		if (passability.empty())
			return;

		const int count = std::max(std::min(MapClass::Instance.MaxNumCells, MapClass::MaxCells), 0);
		if (static_cast<size_t>(count) != passability.size())
		{
			Debug::Log("[Replay] Keyframe %d holds passability for %d cells but the map now has %d; "
				"leaving it as the load built it.\n", keyframeFrame,
				static_cast<int>(passability.size()), count);
			return;
		}

		int restored = 0;
		for (int i = 0; i < count; ++i)
		{
			auto* const pCell = MapClass::Instance.Cells[i];
			if (!pCell)
				continue;

			const auto wanted = static_cast<PassabilityType>(passability[static_cast<size_t>(i)]);
			if (pCell->Passability == wanted)
				continue;

			pCell->Passability = wanted;
			++restored;
		}

		if (restored > 0)
		{
			Debug::Log("[Replay] Keyframe %d put back the passability of %d cells the load had reset "
				"to Passable, most of them the map border.\n", keyframeFrame, restored);
		}
	}

	uint32_t HashBytes(uint32_t hash, const void* data, size_t size)
	{
		const auto* const pBytes = static_cast<const unsigned char*>(data);
		for (size_t i = 0; i < size; ++i)
			hash = (hash ^ pBytes[i]) * 16777619u;

		return hash;
	}
	uint32_t HashZonePassability()
	{
		const auto& map = MapClass::Instance;
		const int count = map.ValidMapCellCount;
		if (!map.LevelAndPassability || count <= 0 || count > MapClass::MaxCells)
			return 0u;

		return HashBytes(2166136261u, map.LevelAndPassability,
			sizeof(CellLevelPassabilityStruct) * static_cast<size_t>(count));
	}


	// These cell IDs index the subzone graph below; both must come from the same snapshot.
	bool CaptureCellSubzones(std::vector<LevelAndPassabilityStruct2>& out)
	{
		out.clear();

		const auto& map = MapClass::Instance;
		const int count = map.ValidMapCellCount;
		if (!map.LevelAndPassabilityStruct2pointer_70 || count <= 0 || count > MapClass::MaxCells)
			return false;

		out.assign(map.LevelAndPassabilityStruct2pointer_70,
			map.LevelAndPassabilityStruct2pointer_70 + count);
		return true;
	}

	bool RestoreCellSubzones(const std::vector<LevelAndPassabilityStruct2>& cells, int keyframeFrame)
	{
		auto& map = MapClass::Instance;
		const int count = map.ValidMapCellCount;
		if (!map.LevelAndPassabilityStruct2pointer_70 || count <= 0
			|| static_cast<size_t>(count) != cells.size())
		{
			Debug::Log("[Replay] Keyframe %d holds subzone IDs for %d cells but the map now has "
				"%d; the recorded graph cannot be restored safely.\n", keyframeFrame,
				static_cast<int>(cells.size()), count);
			return false;
		}

		std::array<int, 4> zoneDifferences {};
		int levelDifferences = 0;
		int field9Differences = 0;

		for (int i = 0; i < count; ++i)
		{
			const LevelAndPassabilityStruct2& wanted = cells[static_cast<size_t>(i)];
			LevelAndPassabilityStruct2& live = map.LevelAndPassabilityStruct2pointer_70[i];

			for (size_t level = 0; level < zoneDifferences.size(); ++level)
			{
				if (live.word_0[level] != wanted.word_0[level])
					++zoneDifferences[level];
			}

			if (live.CellLevel != wanted.CellLevel)
				++levelDifferences;
			if (live.field_9 != wanted.field_9)
				++field9Differences;
			live = wanted;
		}

		Debug::Log("[Replay] Keyframe %d restored the per-cell subzone IDs: %d/%d/%d cells "
			"differed at levels 0/1/2, %d differed in base zone; level/field9 differed in "
			"%d/%d cells.\n", keyframeFrame, zoneDifferences[0], zoneDifferences[1],
			zoneDifferences[2], zoneDifferences[3], levelDifferences, field9Differences);
		return true;
	}

	int HighestZoneArrayIndex()
	{
		const auto& map = MapClass::Instance;
		const int count = map.ValidMapCellCount;
		if (!map.LevelAndPassability || count <= 0 || count > MapClass::MaxCells)
			return -1;

		int highest = -1;
		for (int i = 0; i < count; ++i)
			highest = std::max(highest, static_cast<int>(map.LevelAndPassability[i].ZoneArrayIndex));

		return highest;
	}
	uint32_t HashMovementZones()
	{
		const int highest = HighestZoneArrayIndex();
		if (highest < 0)
			return 0u;

		const auto entries = static_cast<size_t>(highest) + 1u;
		uint32_t hash = 2166136261u;
		for (size_t zone = 0; zone < 13; ++zone)
		{
			if (const auto* const pTable = MapClass::Instance.MovementZones[zone])
				hash = HashBytes(hash, pTable, sizeof(uint16_t) * entries);
		}

		return hash;
	}

	// These tables are rebuilt by the engine. Keep hashes to detect a different rebuild;
	// the sidecar restores cell/subzone state, not these derived tables.
	void CaptureDerivedMapHashes(DerivedMapHashes& out)
	{
		out.ZonePassability = HashZonePassability();
		out.MovementZones = HashMovementZones();
		out.Present = true;
	}

	void VerifyDerivedMapHashes(const SnapshotData& snapshot, int keyframeFrame)
	{
		if (!snapshot.DerivedMap.Present)
			return;

		const uint32_t zones = HashZonePassability();
		const uint32_t movement = HashMovementZones();

		if (zones == snapshot.DerivedMap.ZonePassability
			&& movement == snapshot.DerivedMap.MovementZones)
		{
			return;
		}

		Debug::Log("[Replay] Keyframe %d came back from the load deriving the map differently: "
			"zone passability %s, movement zones %s. Both of these are rebuilt from the cells, so "
			"look at what the cells came back holding first.\n", keyframeFrame,
			zones == snapshot.DerivedMap.ZonePassability ? "matches" : "DIFFERS",
			movement == snapshot.DerivedMap.MovementZones ? "matches" : "DIFFERS");
	}


	static_assert(sizeof(SubzoneConnectionStruct) == 0x08);
	static_assert(offsetof(SubzoneConnectionStruct, unknown_dword_0) == 0x00);
	static_assert(offsetof(SubzoneConnectionStruct, unknown_byte_4) == 0x04);
	static_assert(sizeof(SubzoneTrackingStruct) == 0x24);
	static_assert(offsetof(SubzoneTrackingStruct, SubzoneConnections) == 0x00);
	static_assert(offsetof(SubzoneTrackingStruct, unknown_word_18) == 0x18);
	static_assert(offsetof(SubzoneTrackingStruct, unknown_dword_1C) == 0x1C);
	static_assert(offsetof(SubzoneTrackingStruct, unknown_dword_20) == 0x20);

	DWORD& SubzoneEntryCount(size_t level)
	{
		switch (level)
		{
		case 0:
			return MapClass::Instance.unknown_74;
		case 1:
			return MapClass::Instance.unknown_78;
		default:
			return MapClass::Instance.unknown_7C;
		}
	}

	// Incremental map changes leave history in subzone IDs and connection order that a
	// fresh rebuild loses. Preserve that graph and resize A* scratch storage on restore.
	void CaptureSubzoneTracking(SubzoneGraphSnapshot& out)
	{
		for (size_t level = 0; level < out.Levels.size(); ++level)
		{
			auto& snapshot = out.Levels[level];
			snapshot.clear();
			out.EntryCounts[level] = static_cast<int32_t>(SubzoneEntryCount(level));

			const auto& tracking = MapClass::Instance.SubzoneTracking[level];
			snapshot.reserve(static_cast<size_t>(std::max(tracking.Count, 0)));

			for (int i = 0; i < tracking.Count; ++i)
			{
				const SubzoneTrackingStruct& entry = tracking.Items[i];
				SubzoneEntrySnapshot saved;
				saved.ScalarA = entry.unknown_word_18;
				saved.ScalarB = entry.unknown_dword_1C;
				saved.ScalarC = entry.unknown_dword_20;
				saved.Connections.reserve(
					static_cast<size_t>(std::max(entry.SubzoneConnections.Count, 0)));

				for (int j = 0; j < entry.SubzoneConnections.Count; ++j)
				{
					const SubzoneConnectionStruct& connection = entry.SubzoneConnections.Items[j];
					saved.Connections.push_back(SubzoneConnectionSnapshot {
						connection.unknown_dword_0,
						connection.unknown_byte_4
					});
				}

				snapshot.push_back(std::move(saved));
			}
		}
	}

	bool RestoreSubzoneTracking(const SubzoneGraphSnapshot& snapshot,
		int keyframeFrame)
	{
		int differed = 0;
		int totalEntries = 0;
		int connectionCount = 0;

		// Grow every engine-owned buffer first. If an allocation fails, no logical counts or
		// entries have been changed yet, so the freshly loaded graph is still usable.
		for (size_t level = 0; level < snapshot.Levels.size(); ++level)
		{
			const std::vector<SubzoneEntrySnapshot>& wanted = snapshot.Levels[level];
			auto& tracking = MapClass::Instance.SubzoneTracking[level];

			if (snapshot.EntryCounts[level] < 0
				|| static_cast<size_t>(snapshot.EntryCounts[level]) != wanted.size())
			{
				Debug::Log("[Replay] Keyframe %d subzone level %u has inconsistent entry counts "
					"(%d counter, %u records).\n", keyframeFrame,
					static_cast<unsigned int>(level), snapshot.EntryCounts[level],
					static_cast<unsigned int>(wanted.size()));
				return false;
			}

			if (wanted.size() > static_cast<size_t>(INT_MAX))
			{
				Debug::Log("[Replay] Keyframe %d subzone level %u is too large to restore.\n",
					keyframeFrame, static_cast<unsigned int>(level));
				return false;
			}

			const int wantedCount = static_cast<int>(wanted.size());
			if (tracking.Capacity < wantedCount && !tracking.SetCapacity(wantedCount, nullptr))
			{
				Debug::Log("[Replay] Keyframe %d could not grow subzone level %u to %d entries.\n",
					keyframeFrame, static_cast<unsigned int>(level), wantedCount);
				return false;
			}

			for (int i = 0; i < wantedCount; ++i)
			{
				const size_t wantedConnections = wanted[static_cast<size_t>(i)].Connections.size();
				if (wantedConnections > static_cast<size_t>(INT_MAX))
				{
					Debug::Log("[Replay] Keyframe %d subzone %d at level %u has too many connections.\n",
						keyframeFrame, i, static_cast<unsigned int>(level));
					return false;
				}

				auto& connections = tracking.Items[i].SubzoneConnections;
				const int wantedConnectionCount = static_cast<int>(wantedConnections);
				if (connections.Capacity < wantedConnectionCount
					&& !connections.SetCapacity(wantedConnectionCount, nullptr))
				{
					Debug::Log("[Replay] Keyframe %d could not grow subzone %d at level %u to %d "
						"connections.\n", keyframeFrame, i, static_cast<unsigned int>(level),
						wantedConnectionCount);
					return false;
				}
			}
		}

		for (size_t level = 0; level < snapshot.Levels.size(); ++level)
		{
			const std::vector<SubzoneEntrySnapshot>& wanted = snapshot.Levels[level];
			auto& tracking = MapClass::Instance.SubzoneTracking[level];
			const int oldCount = tracking.Count;
			const int common = std::min(oldCount, static_cast<int>(wanted.size()));
			totalEntries += std::max(oldCount, static_cast<int>(wanted.size()));

			for (int i = 0; i < common; ++i)
			{
				const SubzoneTrackingStruct& live = tracking.Items[i];
				const SubzoneEntrySnapshot& saved = wanted[static_cast<size_t>(i)];
				bool same = live.unknown_word_18 == saved.ScalarA
					&& live.unknown_dword_1C == saved.ScalarB
					&& live.unknown_dword_20 == saved.ScalarC
					&& live.SubzoneConnections.Count == static_cast<int>(saved.Connections.size());

				for (int j = 0; same && j < live.SubzoneConnections.Count; ++j)
				{
					const auto& have = live.SubzoneConnections.Items[j];
					const auto& want = saved.Connections[static_cast<size_t>(j)];
					same = have.unknown_dword_0 == want.SubzoneID
						&& have.unknown_byte_4 == want.IsCrossBlock;
				}

				if (!same)
					++differed;
			}

			if (oldCount != static_cast<int>(wanted.size()))
				differed += std::max(oldCount, static_cast<int>(wanted.size())) - common;

			for (size_t i = 0; i < wanted.size(); ++i)
			{
				SubzoneTrackingStruct& live = tracking.Items[i];
				const SubzoneEntrySnapshot& saved = wanted[i];
				auto& connections = live.SubzoneConnections;
				connections.CapacityIncrement = 16;

				for (size_t j = 0; j < saved.Connections.size(); ++j)
				{
					connections.Items[j].unknown_dword_0 = saved.Connections[j].SubzoneID;
					connections.Items[j].unknown_byte_4 = saved.Connections[j].IsCrossBlock;
				}

				connections.Count = static_cast<int>(saved.Connections.size());
				connectionCount += connections.Count;
				live.unknown_word_18 = saved.ScalarA;
				live.unknown_dword_1C = saved.ScalarB;
				live.unknown_dword_20 = saved.ScalarC;
			}

			// Entries beyond Count are never read, but clearing their nested vectors releases the
			// graph produced by the load instead of retaining thousands of unreachable allocations.
			for (int i = static_cast<int>(wanted.size()); i < oldCount; ++i)
				tracking.Items[i].SubzoneConnections.Clear();

			tracking.Count = static_cast<int>(wanted.size());
			SubzoneEntryCount(level) = static_cast<DWORD>(snapshot.EntryCounts[level]);
		}

		using ResetSubzoneTables = void(__thiscall*)(AStarClass*);
		reinterpret_cast<ResetSubzoneTables>(0x42C1C0)(&AStarClass::Instance);

		Debug::Log("[Replay] Keyframe %d restored the complete subzone graph: %d entries differed "
			"among %d; %d directed connections restored.\n",
			keyframeFrame, differed, totalEntries, connectionCount);
		return true;
	}

	#pragma endregion Cell passability

	#pragma region Ore growth and spread queues

	// FootClass::Basic_Path (0x4D3920) clears the head of the path list with
	// mov dword ptr [ebp+5E0h], 0FFFFFFFFh, which pins where the watch has to read it from.
	static_assert(offsetof(FootClass, PathDirections) == 0x5E0,
		"FootClass::Basic_Path (0x4D3920) writes the path list at FootClass+0x5E0");

	static_assert(offsetof(TiberiumClass, SpreadLogic) == 0xF0,
		"Tiberium_Init_Spread_Data (0x722240) writes the spread queue at TiberiumClass+0xF0");
	static_assert(offsetof(TiberiumClass, GrowthLogic) == 0x10C,
		"Tiberium_Init_Growth_Data (0x722D00) writes the growth queue at TiberiumClass+0x10C");
	static_assert(sizeof(PriorityQueueClassNode) == 8,
		"Recalc_Growth_Data (0x7233A0) walks the node pool eight bytes at a time");

	TiberiumLogic& TiberiumLogicOf(TiberiumClass* pTiberium, int kind)
	{
		return kind == TiberiumQueue_Growth ? pTiberium->GrowthLogic : pTiberium->SpreadLogic;
	}

	bool CaptureTiberiumQueue(const TiberiumLogic& logic, int surfaceCount, TiberiumQueueSnapshot& out)
	{
		out = TiberiumQueueSnapshot {};

		// Taken first, so that a queue this cannot capture still carries its timer across.
		out.TimerPresent = true;
		out.TimerStart = logic.Timer.StartTime;
		out.TimerLeft = logic.Timer.TimeLeft;

		auto* const pQueue = logic.Queue;
		if (!pQueue || !pQueue->Nodes || !logic.Nodes || !logic.CellIndexesWithTiberium)
			return false;

		const int heapCount = pQueue->Count;
		if (heapCount < 0 || heapCount > pQueue->Capacity)
			return false;

		out.Heap.reserve(static_cast<size_t>(heapCount));
		for (int i = 1; i <= heapCount; ++i)
		{
			const auto* const pNode = pQueue->Nodes[i];
			if (!pNode)
				return false;

			out.Heap.push_back(*pNode);
		}

		out.CellFlagCount = surfaceCount;
		out.CellFlagBits.assign((static_cast<size_t>(surfaceCount) + 7u) / 8u, 0u);
		for (int i = 0; i < surfaceCount; ++i)
		{
			if (logic.CellIndexesWithTiberium[i])
				out.CellFlagBits[static_cast<size_t>(i) / 8u] |= static_cast<unsigned char>(1u << (i % 8));
		}

		out.Present = true;
		return true;
	}

	bool RestoreTiberiumQueue(TiberiumLogic& logic, int surfaceCount, const TiberiumQueueSnapshot& snapshot)
	{
		if (!snapshot.Present || snapshot.CellFlagCount != surfaceCount)
			return false;

		auto* const pQueue = logic.Queue;
		if (!pQueue || !pQueue->Nodes || !logic.Nodes || !logic.CellIndexesWithTiberium)
			return false;

		const int heapCount = static_cast<int>(snapshot.Heap.size());
		if (heapCount > pQueue->Capacity || heapCount > surfaceCount)
			return false;

		// Everything the load's own rebuild left behind goes, so that no slot past the restored heap
		// is still pointing into the pool underneath it.
		std::memset(pQueue->Nodes, 0,
			sizeof(PriorityQueueClassNode*) * (static_cast<size_t>(pQueue->Capacity) + 1u));

		pQueue->Count = heapCount;
		pQueue->LMost = nullptr;
		pQueue->RMost = reinterpret_cast<PriorityQueueClassNode*>(~static_cast<uintptr_t>(0));

		for (int i = 0; i < heapCount; ++i)
		{
			auto* const pNode = logic.Nodes + i;
			*pNode = snapshot.Heap[static_cast<size_t>(i)];
			pQueue->Nodes[i + 1] = pNode;

			if (pNode > pQueue->LMost)
				pQueue->LMost = pNode;
			if (pNode < pQueue->RMost)
				pQueue->RMost = pNode;
		}

		// The pool is a bump allocator the engine only appends to, so putting the live nodes at the
		// front of it and pointing the mark just past them leaves it behaving exactly as it did.
		logic.Count = heapCount;

		std::memset(logic.CellIndexesWithTiberium, 0, static_cast<size_t>(surfaceCount));
		for (int i = 0; i < surfaceCount; ++i)
		{
			if ((snapshot.CellFlagBits[static_cast<size_t>(i) / 8u] >> (i % 8)) & 1u)
				logic.CellIndexesWithTiberium[i] = true;
		}

		return true;
	}

	// Loading rebuilds ore queues in map order. Keep both heaps, queued-cell flags, and
	// timers so the same cells grow or spread next, with the same priorities.
	bool CaptureTiberiumState(TiberiumSnapshot& snapshot)
	{
		snapshot = TiberiumSnapshot {};

		const int surfaceCount = PriorityQueueClassNode::SurfaceDataCount();
		if (surfaceCount <= 0)
			return false;

		snapshot.Queues.resize(static_cast<size_t>(std::max(TiberiumClass::Array.Count, 0)));
		for (int i = 0; i < TiberiumClass::Array.Count; ++i)
		{
			auto* const pTiberium = TiberiumClass::Array.Items[i];
			if (!pTiberium)
				continue;

			for (int kind = 0; kind < TiberiumQueueCount; ++kind)
			{
				CaptureTiberiumQueue(TiberiumLogicOf(pTiberium, kind), surfaceCount,
					snapshot.Queues[static_cast<size_t>(i)][static_cast<size_t>(kind)]);
			}
		}

		snapshot.Captured = true;
		return true;
	}

	// Not fatal to a seek: ore that thickens in the wrong order is a slow divergence, and dropping the
	// viewer back to no playback at all over it would be the worse trade.
	bool RestoreTiberiumState(const TiberiumSnapshot& snapshot, int keyframeFrame)
	{
		if (!snapshot.Captured)
			return true;

		const int surfaceCount = PriorityQueueClassNode::SurfaceDataCount();
		const int typeCount = std::min(TiberiumClass::Array.Count, static_cast<int>(snapshot.Queues.size()));

		int restored = 0;
		int failed = 0;
		int cells = 0;
		int timers = 0;

		for (int i = 0; i < typeCount; ++i)
		{
			auto* const pTiberium = TiberiumClass::Array.Items[i];
			if (!pTiberium)
				continue;

			for (int kind = 0; kind < TiberiumQueueCount; ++kind)
			{
				const TiberiumQueueSnapshot& queue =
					snapshot.Queues[static_cast<size_t>(i)][static_cast<size_t>(kind)];
				TiberiumLogic& logic = TiberiumLogicOf(pTiberium, kind);

				if (queue.TimerPresent && (logic.Timer.StartTime != queue.TimerStart
					|| logic.Timer.TimeLeft != queue.TimerLeft))
				{
					if (timers == 0)
					{
						Debug::Log("[Replay] Keyframe %d: ore type %d %s timer is %d/%d after loading; "
							"it was %d/%d.\n", keyframeFrame, i,
							kind == TiberiumQueue_Growth ? "growth" : "spread",
							logic.Timer.StartTime, logic.Timer.TimeLeft,
							queue.TimerStart, queue.TimerLeft);
					}

					logic.Timer.StartTime = queue.TimerStart;
					logic.Timer.TimeLeft = queue.TimerLeft;
					++timers;
				}

				if (!queue.Present)
					continue;

				if (RestoreTiberiumQueue(logic, surfaceCount, queue))
				{
					++restored;
					cells += static_cast<int>(queue.Heap.size());
				}
				else
				{
					++failed;
				}
			}
		}

		if (failed > 0)
		{
			Debug::Log("[Replay] Keyframe %d put back %d of the ore growth and spread queues but not "
				"%d of them; the ore will thicken in a different order from here.\n",
				keyframeFrame, restored, failed);
		}
		else if (restored > 0)
		{
			Debug::Log("[Replay] Keyframe %d put back %d ore growth and spread queues holding %d "
				"cells, which the load had rebuilt in map order.\n", keyframeFrame, restored, cells);
		}

		if (timers > 0)
		{
			Debug::Log("[Replay] Keyframe %d put back %d ore growth and spread timers the load had "
				"left ready to fire.\n", keyframeFrame, timers);
		}

		return true;
	}

	#pragma endregion Ore growth and spread queues
}
