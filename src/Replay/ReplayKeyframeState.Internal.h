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

// Snapshot implementation details shared only by ReplayKeyframeState*.cpp.
#include <AbstractClass.h>
#include <EventClass.h>
#include <Facing.h>
#include <MapClass.h>
#include <PriorityQueueClass.h>
#include <Randomizer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ReplaySystem::KeyframeState::Detail
{
	// State that the savegame does not restore.
	struct TechnoSnapshot
	{
		uint32_t Id;
		bool IsInPlayfield;
	};
	struct HouseRepairSnapshot
	{
		uint32_t Id;
		bool DidRepair;
		int32_t RepairTimerStart;
		int32_t RepairTimerLeft;
	};

	constexpr uint32_t InvalidPlanningNode = UINT32_MAX;

	struct PlanningMemberSnapshot
	{
		bool Present = false;
		uint32_t OwnerId = 0;
		std::array<unsigned char, sizeof(EventClass)> Packet {};
		int Field8 = -1;
		char FieldC = 0;
		bool operator==(const PlanningMemberSnapshot&) const = default;
	};

	struct PlanningBranchSnapshot
	{
		bool Present = false;
		std::array<unsigned char, sizeof(EventClass)> Packet {};
		int MemberCount = 0;
		int MemberIndex = -1;
		bool operator==(const PlanningBranchSnapshot&) const = default;
	};

	struct PlanningNodeSnapshot
	{
		int Field18 = 0;
		bool Field1C = false;
		std::array<unsigned char, sizeof(EventClass)> Packet {};
		int FieldA8 = 0;
		int FieldAC = -1;
		int BranchNumber = -1;
		int FieldB4 = -1;
		std::vector<PlanningMemberSnapshot> Members;
		std::vector<PlanningBranchSnapshot> Branches;
		bool operator==(const PlanningNodeSnapshot&) const = default;
	};

	struct PlanningTokenSnapshot
	{
		uint32_t OwnerId = 0;
		bool Field1C = false;
		std::array<unsigned char, sizeof(EventClass)> CurrentEvent {};
		int Field8C = -1;
		int ClosedLoopNodeCount = -1;
		int StepsToClosedLoop = -1;
		bool Field98 = false;
		bool Field99 = false;
		std::vector<uint32_t> Nodes;
		bool operator==(const PlanningTokenSnapshot&) const = default;
	};

	struct PlanningSnapshot
	{
		std::vector<PlanningNodeSnapshot> Nodes;
		std::vector<PlanningTokenSnapshot> Tokens;
		std::vector<std::array<unsigned char, sizeof(EventClass)>> PendingEvents;
		std::array<std::vector<uint32_t>, 3> ManagerNodeLists;
		std::vector<uint32_t> ActiveRouteOwners;
		std::array<int, 24> HouseRouteCounts {};
		bool operator==(const PlanningSnapshot&) const = default;
	};

	// Preserve the Kamikaze update timer across keyframe loads.
	struct KamikazeSnapshot
	{
		int32_t TimerStart = -1;
		int32_t TimerLeft = 0;
	};

	constexpr size_t AresParticleRecordSize = 0x2C;

	struct AresParticleRecordSnapshot
	{
		std::array<unsigned char, AresParticleRecordSize> Bytes {};
		int32_t LinkedParticleTypeIndex = -1;
		bool operator==(const AresParticleRecordSnapshot&) const = default;
	};

	struct AresParticleSystemSnapshot
	{
		uint32_t OwnerId = 0;
		int32_t Behave = 0;
		int32_t HeldParticleTypeIndex = -1;
		std::vector<std::array<unsigned char, AresParticleRecordSize>> MovementData;
		std::vector<AresParticleRecordSnapshot> DrawData;
		bool operator==(const AresParticleSystemSnapshot&) const = default;
	};

	struct AresParticleSnapshot
	{
		bool Captured = false;
		std::vector<AresParticleSystemSnapshot> Systems;
		bool operator==(const AresParticleSnapshot&) const = default;
	};

	enum class LocomotorResetStateKind : unsigned char
	{
		HoverFacing,
		TunnelDigTimer,
		TeleportTimer,
		RocketTrailerTimer
	};

	constexpr size_t MaxLocomotorResetStateSize = sizeof(FacingClass);

	struct LocomotorResetSnapshot
	{
		uint32_t OwnerId = 0;
		LocomotorResetStateKind Kind = LocomotorResetStateKind::HoverFacing;
		std::array<unsigned char, MaxLocomotorResetStateSize> Bytes {};
	};

	struct TiberiumQueueSnapshot
	{
		bool Present = false;
		// The live heap in its own order, held as values; the engine numbers its heap from one,
		// so entry k here is the engine's slot k + 1.
		std::vector<PriorityQueueClassNode> Heap;
		// TiberiumLogic::CellIndexesWithTiberium, one bit per cell rather than the engine's one byte.
		std::vector<unsigned char> CellFlagBits;
		int CellFlagCount = 0;
		// TiberiumLogic::Timer, kept whether or not the queue beside it could be captured: it is
		// what decides whether the queue is looked at this frame at all.
		bool TimerPresent = false;
		int TimerStart = -1;
		int TimerLeft = 0;
	};

	struct TiberiumSnapshot
	{
		bool Captured = false;
		// Two per type, indexed by TiberiumQueueKind.
		std::vector<std::array<TiberiumQueueSnapshot, 2>> Queues;
	};

	enum TiberiumQueueKind
	{
		TiberiumQueue_Spread,
		TiberiumQueue_Growth,
		TiberiumQueueCount
	};

	struct SubzoneConnectionSnapshot
	{
		uint32_t SubzoneID;
		uint8_t IsCrossBlock;
		bool operator==(const SubzoneConnectionSnapshot&) const = default;
	};

	// One entry of the map's subzone graph, kept by value so a keyframe can put it back. See the
	// Cell passability region for what it is and why it travels.
	struct SubzoneEntrySnapshot
	{
		std::vector<SubzoneConnectionSnapshot> Connections;
		uint16_t ScalarA;
		uint32_t ScalarB;
		uint32_t ScalarC;
		bool operator==(const SubzoneEntrySnapshot&) const = default;
	};

	struct SubzoneGraphSnapshot
	{
		std::array<std::vector<SubzoneEntrySnapshot>, 3> Levels;
		std::array<int32_t, 3> EntryCounts {};
	};

	enum OrderedCollectionIndex
	{
		OrderIndex_AbstractClass,
		OrderIndex_TechnoClass,
		OrderIndex_FootClass,
		OrderIndex_AircraftClass,
		OrderIndex_InfantryClass,
		OrderIndex_UnitClass,
		OrderIndex_BuildingClass,
		OrderIndex_HouseClass,
		OrderIndex_TeamClass,
		OrderIndex_AnimClass,
		OrderIndex_BulletClass,
		OrderIndex_FactoryClass,
		OrderIndex_TerrainClass,
		OrderIndex_SmudgeClass,
		OrderIndex_ParticleClass,
		OrderIndex_ParticleSystemClass,
		OrderIndex_RadSiteClass,
		OrderIndex_SuperClass,
		OrderIndex_TagClass,
		OrderIndex_TriggerClass,
		OrderIndex_WaveClass,
		OrderIndex_LogicClass,
		OrderedCollectionCount
	};

	struct LoadResetTimerSnapshot
	{
		uint32_t Id = 0;
		int32_t FirstStart = -1;
		int32_t FirstLeft = 0;
		int32_t SecondStart = -1;
		int32_t SecondLeft = 0;
	};

	struct LoadResetTimerSnapshots
	{
		std::vector<LoadResetTimerSnapshot> SpawnManagers;
		std::vector<LoadResetTimerSnapshot> Bullets;
	};

	struct SlaveControlSnapshot
	{
		uint32_t Slave = 0;
		int32_t State = 0;
		int32_t TimerStart = -1;
		int32_t TimerLeft = 0;
	};

	struct SlaveManagerSnapshot
	{
		uint32_t Id = 0;
		uint32_t Owner = 0;
		int32_t State = 0;
		int32_t LastScanFrame = 0;
		int32_t TimerStart = -1;
		int32_t TimerLeft = 0;
		std::vector<SlaveControlSnapshot> Controls;
	};

	struct DerivedMapHashes
	{
		bool Present = false;
		uint32_t ZonePassability = 0;
		uint32_t MovementZones = 0;
	};

	struct SnapshotData
	{
		int ScenarioUniqueID = 0;
		std::array<unsigned char, sizeof(Randomizer)> Random {};
		std::vector<TechnoSnapshot> Technos;
		std::vector<HouseRepairSnapshot> HouseRepairs;
		PlanningSnapshot Planning;
		AresParticleSnapshot AresParticles;
		TiberiumSnapshot Tiberium;
		LoadResetTimerSnapshots LoadResetTimers;
		std::vector<SlaveManagerSnapshot> SlaveManagers;
		std::vector<unsigned char> CellPassability;
		DerivedMapHashes DerivedMap;
		std::vector<LevelAndPassabilityStruct2> CellSubzones;
		SubzoneGraphSnapshot SubzoneGraph;
		std::vector<LocomotorResetSnapshot> LocomotorResetStates;
		KamikazeSnapshot Kamikaze;
		std::array<std::vector<uint32_t>, OrderedCollectionCount> Orders;
		std::array<std::vector<uint32_t>, 5> LayerOrders;
	};

	inline uint32_t UniqueIDOf(const AbstractClass* pObject)
	{
		return pObject ? static_cast<uint32_t>(pObject->UniqueID) : 0u;
	}

	bool CapturePlanningState(PlanningSnapshot& snapshot);
	bool RestorePlanningState(const PlanningSnapshot& snapshot, int keyframeFrame);
	bool CaptureAresParticleState(AresParticleSnapshot& snapshot);
	bool RestoreAresParticleState(const AresParticleSnapshot& snapshot, int keyframeFrame);

	void CaptureLocomotorResetStates(std::vector<LocomotorResetSnapshot>& out);
	bool RestoreLocomotorResetStates(const std::vector<LocomotorResetSnapshot>& snapshots, int keyframeFrame);
	void CaptureKamikazeState(KamikazeSnapshot& snapshot);
	void RestoreKamikazeState(const KamikazeSnapshot& snapshot);
	void CaptureTechnoSnapshots(std::vector<TechnoSnapshot>& out);
	void CaptureHouseRepairSnapshots(std::vector<HouseRepairSnapshot>& out);
	void RestoreHouseRepairState(const SnapshotData& snapshot, int keyframeFrame);
	void RestoreTechnoInPlayfieldState(const SnapshotData& snapshot, int keyframeFrame);
	void CaptureSlaveManagerState(std::vector<SlaveManagerSnapshot>& out);
	void RestoreSlaveManagerState(const SnapshotData& snapshot, int keyframeFrame);
	void CaptureLoadResetTimerState(LoadResetTimerSnapshots& out);
	void RestoreLoadResetTimerState(const SnapshotData& snapshot, int keyframeFrame);

	void CaptureCellPassability(std::vector<unsigned char>& out);
	void RestoreCellPassability(const std::vector<unsigned char>& passability, int keyframeFrame);
	bool CaptureCellSubzones(std::vector<LevelAndPassabilityStruct2>& out);
	bool RestoreCellSubzones(const std::vector<LevelAndPassabilityStruct2>& cells, int keyframeFrame);
	void CaptureDerivedMapHashes(DerivedMapHashes& out);
	void VerifyDerivedMapHashes(const SnapshotData& snapshot, int keyframeFrame);
	void CaptureSubzoneTracking(SubzoneGraphSnapshot& out);
	bool RestoreSubzoneTracking(const SubzoneGraphSnapshot& snapshot, int keyframeFrame);
	bool CaptureTiberiumState(TiberiumSnapshot& snapshot);
	bool RestoreTiberiumState(const TiberiumSnapshot& snapshot, int keyframeFrame);
}
