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

#include <Utilities/Debug.h>

#include <AircraftClass.h>
#include <AnimClass.h>
#include <BuildingClass.h>
#include <BulletClass.h>
#include <FactoryClass.h>
#include <FootClass.h>
#include <HouseClass.h>
#include <InfantryClass.h>
#include <ParticleClass.h>
#include <ParticleSystemClass.h>
#include <RadSiteClass.h>
#include <ScenarioClass.h>
#include <SmudgeClass.h>
#include <SuperClass.h>
#include <TagClass.h>
#include <TeamClass.h>
#include <TechnoClass.h>
#include <TerrainClass.h>
#include <TriggerClass.h>
#include <UnitClass.h>
#include <Unsorted.h>
#include <WaveClass.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace ReplaySystem::KeyframeState
{
	using namespace Detail;

	#define REPLAY_FOR_EACH_ORDERED_COLLECTION(ENTRY) \
		ENTRY(AbstractClass::Array, "AbstractClass") \
		ENTRY(TechnoClass::Array, "TechnoClass") \
		ENTRY(FootClass::Array, "FootClass") \
		ENTRY(AircraftClass::Array, "AircraftClass") \
		ENTRY(InfantryClass::Array, "InfantryClass") \
		ENTRY(UnitClass::Array, "UnitClass") \
		ENTRY(BuildingClass::Array, "BuildingClass") \
		ENTRY(HouseClass::Array, "HouseClass") \
		ENTRY(TeamClass::Array, "TeamClass") \
		ENTRY(AnimClass::Array, "AnimClass") \
		ENTRY(BulletClass::Array, "BulletClass") \
		ENTRY(FactoryClass::Array, "FactoryClass") \
		ENTRY(TerrainClass::Array, "TerrainClass") \
		ENTRY(SmudgeClass::Array, "SmudgeClass") \
		ENTRY(ParticleClass::Array, "ParticleClass") \
		ENTRY(ParticleSystemClass::Array, "ParticleSystemClass") \
		ENTRY(RadSiteClass::Array, "RadSiteClass") \
		ENTRY(SuperClass::Array, "SuperClass") \
		ENTRY(TagClass::Array, "TagClass") \
		ENTRY(TriggerClass::Array, "TriggerClass") \
		ENTRY(WaveClass::Array, "WaveClass") \
		ENTRY(LogicClass::Instance, "LogicClass")

	// Loading refills object arrays in a different order. Restore their recorded order
	// because update order and target-selection ties can affect later simulation.
	template <typename TCollection>
	void CaptureObjectOrder(const TCollection& collection, std::vector<uint32_t>& outOrder)
	{
		outOrder.clear();
		outOrder.reserve(static_cast<size_t>(std::max(collection.Count, 0)));
		for (int i = 0; i < collection.Count; ++i)
			outOrder.push_back(UniqueIDOf(collection.Items[i]));
	}

	template <typename TCollection>
	void RestoreObjectOrder(TCollection& collection, const std::vector<uint32_t>& savedOrder,
		const char* collectionName, bool& changed)
	{
		changed = false;

		using Pointer = std::remove_reference_t<decltype(collection.Items[0])>;
		std::vector<Pointer> reordered;
		reordered.reserve(static_cast<size_t>(std::max(collection.Count, 0)));
		std::vector<bool> used(static_cast<size_t>(std::max(collection.Count, 0)), false);

		int missing = 0;
		for (const uint32_t wanted : savedOrder)
		{
			bool found = false;
			for (int currentIndex = 0; currentIndex < collection.Count; ++currentIndex)
			{
				const size_t index = static_cast<size_t>(currentIndex);
				if (!used[index] && UniqueIDOf(collection.Items[currentIndex]) == wanted)
				{
					reordered.push_back(collection.Items[currentIndex]);
					used[index] = true;
					found = true;
					break;
				}
			}

			if (!found)
				++missing;
		}

		int extra = 0;
		for (int i = 0; i < collection.Count; ++i)
		{
			if (used[static_cast<size_t>(i)])
				continue;

			++extra;
			reordered.push_back(collection.Items[i]);
		}

		for (int i = 0; i < collection.Count; ++i)
		{
			if (collection.Items[i] != reordered[static_cast<size_t>(i)])
				changed = true;

			collection.Items[i] = reordered[static_cast<size_t>(i)];
		}

		if (missing == 0 && extra == 0)
			return;

		Debug::Log("[Replay] Keyframe collection %s came back from the load holding %d objects where "
			"the keyframe held %d: %d of the keyframe's are gone and %d are new. The order has been put "
			"back for the %d they have in common.\n",
			collectionName, collection.Count, static_cast<int>(savedOrder.size()), missing, extra,
			static_cast<int>(savedOrder.size()) - missing);

		constexpr int MaxReportedIDs = 8;
		int reported = 0;
		for (size_t i = 0; i < savedOrder.size() && reported < MaxReportedIDs; ++i)
		{
			bool present = false;
			for (int j = 0; j < collection.Count && !present; ++j)
				present = UniqueIDOf(collection.Items[j]) == savedOrder[i];

			if (!present)
			{
				Debug::Log("[Replay]   the keyframe expected unique ID %d at position %u, and nothing "
					"after the load has it.\n",
					static_cast<int>(savedOrder[i]), static_cast<unsigned int>(i));
				++reported;
			}
		}

		reported = 0;
		for (int i = 0; i < collection.Count && reported < MaxReportedIDs; ++i)
		{
			const uint32_t liveID = UniqueIDOf(collection.Items[i]);
			if (std::find(savedOrder.begin(), savedOrder.end(), liveID) == savedOrder.end())
			{
				Debug::Log("[Replay]   the load left unique ID %d at position %d, which the keyframe "
					"never held.\n", static_cast<int>(liveID), i);
				++reported;
			}
		}

	}

	Snapshot::Snapshot() : Data(std::make_unique<SnapshotData>()) { }
	Snapshot::~Snapshot() = default;
	Snapshot::Snapshot(Snapshot&&) noexcept = default;
	Snapshot& Snapshot::operator=(Snapshot&&) noexcept = default;

	bool Snapshot::CaptureAfterSave()
	{
		if (!ScenarioClass::Instance)
			return false;

		auto& snapshot = *this->Data;
		snapshot.ScenarioUniqueID = ScenarioClass::Instance->UniqueID;

		memcpy(snapshot.Random.data(), &ScenarioClass::Instance->Random, sizeof(Randomizer));

		CaptureTechnoSnapshots(snapshot.Technos);
		CaptureHouseRepairSnapshots(snapshot.HouseRepairs);
		if (!CapturePlanningState(snapshot.Planning))
			return false;
		if (!CaptureAresParticleState(snapshot.AresParticles))
			return false;
		CaptureLocomotorResetStates(snapshot.LocomotorResetStates);
		CaptureKamikazeState(snapshot.Kamikaze);
		CaptureTiberiumState(snapshot.Tiberium);
		CaptureLoadResetTimerState(snapshot.LoadResetTimers);
		CaptureSlaveManagerState(snapshot.SlaveManagers);
		CaptureCellPassability(snapshot.CellPassability);
		CaptureDerivedMapHashes(snapshot.DerivedMap);
		if (!CaptureCellSubzones(snapshot.CellSubzones))
			return false;
		CaptureSubzoneTracking(snapshot.SubzoneGraph);

		int orderIndex = 0;
		#define REPLAY_CAPTURE_ORDER(collection, name) \
			CaptureObjectOrder(collection, snapshot.Orders[orderIndex++]);
		REPLAY_FOR_EACH_ORDERED_COLLECTION(REPLAY_CAPTURE_ORDER)
		#undef REPLAY_CAPTURE_ORDER

		for (size_t layer = 0; layer < snapshot.LayerOrders.size(); ++layer)
			CaptureObjectOrder(MapClass::ObjectsInLayers[layer], snapshot.LayerOrders[layer]);

		return true;
	}

	bool Snapshot::RestoreBeforeResume(int keyframeFrame) const
	{
		const auto& snapshot = *this->Data;
		// Saved timers and replay events must use the frame this keyframe was captured on.
		if (static_cast<int>(Unsorted::CurrentFrame) != keyframeFrame)
		{
			Debug::Log("[Replay] Keyframe %d restored the frame counter as %d; forcing it.\n",
				keyframeFrame, static_cast<int>(Unsorted::CurrentFrame));
			Unsorted::CurrentFrame = keyframeFrame;
		}

		if (!ScenarioClass::Instance)
		{
			Debug::Log("[Replay] Keyframe %d loaded without a scenario.\n", keyframeFrame);
			return false;
		}

		// Load constructors consume IDs even though the objects regain their saved IDs.
		// Put the counter back so newly created objects get the same IDs as before.
		if (ScenarioClass::Instance->UniqueID != snapshot.ScenarioUniqueID)
		{
			Debug::Log("[Replay] Keyframe %d advanced the scenario unique ID from %d to %d "
				"while loading; restoring it.\n", keyframeFrame, snapshot.ScenarioUniqueID,
				ScenarioClass::Instance->UniqueID);
			ScenarioClass::Instance->UniqueID = snapshot.ScenarioUniqueID;
		}

		// Scenario reconstruction can reset the saved RNG; keep the next random draw identical.
		auto& random = ScenarioClass::Instance->Random;
		const bool randomChanged = memcmp(&random, snapshot.Random.data(), sizeof(Randomizer)) != 0;
		memcpy(&random, snapshot.Random.data(), sizeof(Randomizer));

		int reorderedCollectionCount = 0;
		auto restoreCollection = [&reorderedCollectionCount](auto& collection,
			const std::vector<uint32_t>& order, const char* name)
		{
			bool changed = false;
			RestoreObjectOrder(collection, order, name, changed);

			if (changed)
			{
				++reorderedCollectionCount;
				Debug::Log("[Replay] %s came back from the load out of order; put back.\n", name);
			}

		};

		int orderIndex = 0;
		#define REPLAY_RESTORE_ORDER(collection, name) \
			restoreCollection(collection, snapshot.Orders[orderIndex++], name);
		REPLAY_FOR_EACH_ORDERED_COLLECTION(REPLAY_RESTORE_ORDER)
		#undef REPLAY_RESTORE_ORDER

		for (size_t layer = 0; layer < snapshot.LayerOrders.size(); ++layer)
		{
			char name[32] = { 0 };
			sprintf_s(name, "MapClass::Layer[%u]", static_cast<unsigned int>(layer));
			restoreCollection(MapClass::ObjectsInLayers[layer], snapshot.LayerOrders[layer], name);
		}

		if (!RestorePlanningState(snapshot.Planning, keyframeFrame))
			return false;
		if (!RestoreAresParticleState(snapshot.AresParticles, keyframeFrame))
			return false;
		if (!RestoreLocomotorResetStates(snapshot.LocomotorResetStates, keyframeFrame))
			return false;
		RestoreKamikazeState(snapshot.Kamikaze);
		RestoreTiberiumState(snapshot.Tiberium, keyframeFrame);
		RestoreCellPassability(snapshot.CellPassability, keyframeFrame);
		VerifyDerivedMapHashes(snapshot, keyframeFrame);
		if (!RestoreCellSubzones(snapshot.CellSubzones, keyframeFrame))
			return false;
		if (!RestoreSubzoneTracking(snapshot.SubzoneGraph, keyframeFrame))
			return false;

		if (randomChanged || reorderedCollectionCount > 0)
		{
			Debug::Log("[Replay] Keyframe %d restored CRC state after loading "
				"(RNG changed: %s; reordered collections: %d).\n",
				keyframeFrame, randomChanged ? "yes" : "no", reorderedCollectionCount);
		}

		return true;
	}

	void Snapshot::RestoreAfterResume(int keyframeFrame) const
	{
		const auto& snapshot = *this->Data;
		RestoreHouseRepairState(snapshot, keyframeFrame);
		RestoreLoadResetTimerState(snapshot, keyframeFrame);
		RestoreSlaveManagerState(snapshot, keyframeFrame);
		RestoreTechnoInPlayfieldState(snapshot, keyframeFrame);
	}

	#undef REPLAY_FOR_EACH_ORDERED_COLLECTION
}
