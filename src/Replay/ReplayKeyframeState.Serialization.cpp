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

#include "ReplayKeyframeState.h"
#include "ReplayKeyframeState.Internal.h"
#include "ReplayCheckpointCodec.h"

template<typename A> void Visit(A& a, PriorityQueueClassNode& v)
{ Replay::CheckpointCodec::Fields(a, v.MapCoord.X, v.MapCoord.Y, v.Score); }
template<typename A> void Visit(A& a, LevelAndPassabilityStruct2& v)
{ Replay::CheckpointCodec::Fields(a, v.word_0, v.CellLevel, v.field_9); }

namespace ReplaySystem::KeyframeState::Detail
{
	template<typename A> void Visit(A& a, TechnoSnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.Id, v.IsInPlayfield);
	}
	template<typename A> void Visit(A& a, HouseRepairSnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.Id, v.DidRepair, v.RepairTimerStart, v.RepairTimerLeft);
	}
	template<typename A> void Visit(A& a, PlanningMemberSnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.Present, v.OwnerId, v.Packet, v.Field8, v.FieldC);
	}
	template<typename A> void Visit(A& a, PlanningBranchSnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.Present, v.Packet, v.MemberCount, v.MemberIndex);
	}
	template<typename A> void Visit(A& a, PlanningNodeSnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.Field18, v.Field1C, v.Packet, v.FieldA8, v.FieldAC, v.BranchNumber, v.FieldB4,
			v.Members, v.Branches);
	}
	template<typename A> void Visit(A& a, PlanningTokenSnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.OwnerId, v.Field1C, v.CurrentEvent, v.Field8C, v.ClosedLoopNodeCount,
			v.StepsToClosedLoop, v.Field98, v.Field99, v.Nodes);
	}
	template<typename A> void Visit(A& a, PlanningSnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.Nodes, v.Tokens, v.PendingEvents, v.ManagerNodeLists, v.ActiveRouteOwners,
			v.HouseRouteCounts);
	}
	template<typename A> void Visit(A& a, KamikazeSnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.TimerStart, v.TimerLeft);
	}
	template<typename A> void Visit(A& a, AresParticleRecordSnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.Bytes, v.LinkedParticleTypeIndex);
	}
	template<typename A> void Visit(A& a, AresParticleSystemSnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.OwnerId, v.Behave, v.HeldParticleTypeIndex, v.MovementData, v.DrawData);
	}
	template<typename A> void Visit(A& a, AresParticleSnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.Captured, v.Systems);
	}
	template<typename A> void Visit(A& a, LocomotorResetSnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.OwnerId, v.Kind, v.Bytes);
	}
	template<typename A> void Visit(A& a, TiberiumQueueSnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.Present, v.Heap, v.CellFlagBits, v.CellFlagCount, v.TimerPresent, v.TimerStart,
			v.TimerLeft);
	}
	template<typename A> void Visit(A& a, TiberiumSnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.Captured, v.Queues);
	}
	template<typename A> void Visit(A& a, SubzoneConnectionSnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.SubzoneID, v.IsCrossBlock);
	}
	template<typename A> void Visit(A& a, SubzoneEntrySnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.Connections, v.ScalarA, v.ScalarB, v.ScalarC);
	}
	template<typename A> void Visit(A& a, SubzoneGraphSnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.Levels, v.EntryCounts);
	}
	template<typename A> void Visit(A& a, LoadResetTimerSnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.Id, v.FirstStart, v.FirstLeft, v.SecondStart, v.SecondLeft);
	}
	template<typename A> void Visit(A& a, LoadResetTimerSnapshots& v)
	{
		Replay::CheckpointCodec::Fields(a, v.SpawnManagers, v.Bullets);
	}
	template<typename A> void Visit(A& a, SlaveControlSnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.Slave, v.State, v.TimerStart, v.TimerLeft);
	}
	template<typename A> void Visit(A& a, SlaveManagerSnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.Id, v.Owner, v.State, v.LastScanFrame, v.TimerStart, v.TimerLeft, v.Controls);
	}
	template<typename A> void Visit(A& a, DerivedMapHashes& v)
	{
		Replay::CheckpointCodec::Fields(a, v.Present, v.ZonePassability, v.MovementZones);
	}
	template<typename A> void Visit(A& a, BeaconSnapshot& v)
	{
		Replay::CheckpointCodec::Fields(a, v.Present, v.X, v.Y, v.Z, v.Text);
	}
	template<typename A> void Visit(A& a, SnapshotData& v)
	{
		Replay::CheckpointCodec::Fields(a, v.ScenarioUniqueID, v.Random, v.Technos, v.HouseRepairs, v.Planning,
			v.AresParticles, v.Tiberium, v.LoadResetTimers, v.SlaveManagers, v.CellPassability, v.DerivedMap, v.CellSubzones,
			v.SubzoneGraph, v.LocomotorResetStates, v.Kamikaze, v.Orders, v.LayerOrders, v.Beacons);
	}
}

namespace ReplaySystem::KeyframeState
{
	bool Snapshot::Serialize(std::vector<unsigned char>& bytes) const
	{
		Replay::CheckpointCodec::Writer writer;
		Detail::Visit(writer, *Data);
		if (!writer.Good) return false;
		bytes = std::move(writer.Bytes);
		return true;
	}
	bool Snapshot::Deserialize(const std::vector<unsigned char>& bytes)
	{
		if (bytes.size() > Replay::CheckpointCodec::MaxBytes) return false;
		Replay::CheckpointCodec::Reader reader { bytes };
		auto data = std::make_unique<Detail::SnapshotData>();
		Detail::Visit(reader, *data);
		if (!reader.Good || reader.Position != bytes.size() || data->ScenarioUniqueID < 0) return false;
		int next1 = 0, next2 = 0;
		std::memcpy(&next1, data->Random.data() + offsetof(Randomizer, Next1), sizeof(next1));
		std::memcpy(&next2, data->Random.data() + offsetof(Randomizer, Next2), sizeof(next2));
		if (next1 < 0 || next1 >= 250 || next2 < 0 || next2 >= 250 || data->Random[0] > 1) return false;
		for (const auto& pair : data->Tiberium.Queues)
			for (const auto& queue : pair)
				if (queue.CellFlagCount < 0 || queue.CellFlagCount > 512 * 512
					|| queue.CellFlagBits.size() != (static_cast<size_t>(queue.CellFlagCount) + 7) / 8) return false;
		for (const auto& state : data->LocomotorResetStates)
			if (state.Kind > Detail::LocomotorResetStateKind::RocketTrailerTimer) return false;
		for (const auto& house : data->Beacons)
			for (const auto& beacon : house)
				if (beacon.Present && (beacon.X < 0 || beacon.X >= 512 * 256
					|| beacon.Y < 0 || beacon.Y >= 512 * 256 || beacon.Text.back() != 0)) return false;
		Data = std::move(data);
		return true;
	}
}
