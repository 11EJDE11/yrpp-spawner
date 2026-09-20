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

#include <BulletClass.h>
#include <FootClass.h>
#include <HouseClass.h>
#include <HoverLocomotionClass.h>
#include <InfantryClass.h>
#include <Kamikaze.h>
#include <RocketLocomotionClass.h>
#include <SlaveManagerClass.h>
#include <SpawnManagerClass.h>
#include <TechnoClass.h>
#include <TeleportLocomotionClass.h>
#include <TunnelLocomotionClass.h>
#include <Unsorted.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <type_traits>
#include <utility>

namespace ReplaySystem::KeyframeState::Detail
{
	template <typename T>
	void AddLocomotorResetSnapshot(std::vector<LocomotorResetSnapshot>& out, uint32_t ownerId,
		LocomotorResetStateKind kind, const T& value)
	{
		static_assert(sizeof(T) <= MaxLocomotorResetStateSize);

		LocomotorResetSnapshot snapshot {};
		snapshot.OwnerId = ownerId;
		snapshot.Kind = kind;
		memcpy(snapshot.Bytes.data(), &value, sizeof(T));
		out.push_back(std::move(snapshot));
	}

	// Derived Load methods overwrite hover steering, tunnel/teleport timers, and the
	// rocket trailer timer. Carry those values by owner ID to preserve movement state.
	void CaptureLocomotorResetStates(std::vector<LocomotorResetSnapshot>& out)
	{
		out.clear();
		out.reserve(static_cast<size_t>(std::max(FootClass::Array.Count, 0)));

		for (int i = 0; i < FootClass::Array.Count; ++i)
		{
			auto* const pFoot = FootClass::Array.Items[i];
			if (!pFoot || !pFoot->Locomotor)
				continue;

			ILocomotion* const pInterface = pFoot->Locomotor.GetInterfacePtr();
			const uint32_t ownerId = UniqueIDOf(pFoot);

			if (const auto* const pHover =
				locomotion_cast<const HoverLocomotionClass*>(pInterface))
			{
				AddLocomotorResetSnapshot(out, ownerId,
					LocomotorResetStateKind::HoverFacing, pHover->LocomotionFacing);
			}
			else if (const auto* const pTunnel =
				locomotion_cast<const TunnelLocomotionClass*>(pInterface))
			{
				AddLocomotorResetSnapshot(out, ownerId,
					LocomotorResetStateKind::TunnelDigTimer, pTunnel->DigTimer);
			}
			else if (const auto* const pTeleport =
				locomotion_cast<const TeleportLocomotionClass*>(pInterface))
			{
				AddLocomotorResetSnapshot(out, ownerId,
					LocomotorResetStateKind::TeleportTimer, pTeleport->Timer);
			}
			else if (const auto* const pRocket =
				locomotion_cast<const RocketLocomotionClass*>(pInterface))
			{
				AddLocomotorResetSnapshot(out, ownerId,
					LocomotorResetStateKind::RocketTrailerTimer, pRocket->TrailerTimer);
			}
		}
	}

	bool RestoreLocomotorResetStates(const std::vector<LocomotorResetSnapshot>& snapshots,
		int keyframeFrame)
	{
		std::unordered_map<uint32_t, FootClass*> footById;
		footById.reserve(static_cast<size_t>(std::max(FootClass::Array.Count, 0)));
		for (int i = 0; i < FootClass::Array.Count; ++i)
		{
			if (auto* const pFoot = FootClass::Array.Items[i])
				footById.emplace(UniqueIDOf(pFoot), pFoot);
		}

		int restored = 0;
		std::array<int, 4> restoredByKind {};
		for (const auto& snapshot : snapshots)
		{
			const auto owner = footById.find(snapshot.OwnerId);
			if (owner == footById.end() || !owner->second->Locomotor)
			{
				Debug::Log("[Replay] Keyframe %d locomotor-state owner %u is missing after "
					"load.\n", keyframeFrame, snapshot.OwnerId);
				return false;
			}

			ILocomotion* const pInterface = owner->second->Locomotor.GetInterfacePtr();
			void* destination = nullptr;
			size_t size = 0;

			switch (snapshot.Kind)
			{
			case LocomotorResetStateKind::HoverFacing:
				if (auto* const pLoco = locomotion_cast<HoverLocomotionClass*>(pInterface))
				{
					destination = &pLoco->LocomotionFacing;
					size = sizeof(pLoco->LocomotionFacing);
				}
				break;

			case LocomotorResetStateKind::TunnelDigTimer:
				if (auto* const pLoco = locomotion_cast<TunnelLocomotionClass*>(pInterface))
				{
					destination = &pLoco->DigTimer;
					size = sizeof(pLoco->DigTimer);
				}
				break;

			case LocomotorResetStateKind::TeleportTimer:
				if (auto* const pLoco = locomotion_cast<TeleportLocomotionClass*>(pInterface))
				{
					destination = &pLoco->Timer;
					size = sizeof(pLoco->Timer);
				}
				break;

			case LocomotorResetStateKind::RocketTrailerTimer:
				if (auto* const pLoco = locomotion_cast<RocketLocomotionClass*>(pInterface))
				{
					destination = &pLoco->TrailerTimer;
					size = sizeof(pLoco->TrailerTimer);
				}
				break;
			}

			if (!destination)
			{
				Debug::Log("[Replay] Keyframe %d locomotor-state owner %u came back with a "
					"different locomotor type.\n", keyframeFrame, snapshot.OwnerId);
				return false;
			}

			if (memcmp(destination, snapshot.Bytes.data(), size) != 0)
			{
				memcpy(destination, snapshot.Bytes.data(), size);
				++restored;
				++restoredByKind[static_cast<size_t>(snapshot.Kind)];
			}
		}

		if (restored > 0)
		{
			Debug::Log("[Replay] Keyframe %d restored %d locomotor values discarded by engine "
				"Load (hover facing %d, tunnel timer %d, teleport timer %d, rocket timer "
				"%d).\n", keyframeFrame, restored, restoredByKind[0], restoredByKind[1],
				restoredByKind[2], restoredByKind[3]);
		}
		return true;
	}
	#pragma region Kamikaze tracker

	// Preserve the Kamikaze tracker's update timer so loading does not shift its next tick.
	void CaptureKamikazeState(KamikazeSnapshot& snapshot)
	{
		auto& tracker = Kamikaze::Instance;

		snapshot.TimerStart = tracker.UpdateTimer.StartTime;
		snapshot.TimerLeft = tracker.UpdateTimer.TimeLeft;
	}

	void RestoreKamikazeState(const KamikazeSnapshot& snapshot)
	{
		auto& tracker = Kamikaze::Instance;

		tracker.UpdateTimer.StartTime = snapshot.TimerStart;
		tracker.UpdateTimer.TimeLeft = snapshot.TimerLeft;
	}

	#pragma endregion Kamikaze tracker

	// Map clipping during load can clear the historical in-playfield flag on off-map units.
	// Restore the saved flag after session setup instead of deriving it from position.
	void CaptureTechnoSnapshots(std::vector<TechnoSnapshot>& out)
	{
		out.clear();
		out.reserve(static_cast<size_t>(std::max(TechnoClass::Array.Count, 0)));

		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			const auto* const pTechno = TechnoClass::Array.Items[i];
			if (pTechno)
				out.push_back({ UniqueIDOf(pTechno), pTechno->IsInPlayfield });
		}
	}
	// Keep each house's repair flag and timer together so repairs continue on the same tick.
	void CaptureHouseRepairSnapshots(std::vector<HouseRepairSnapshot>& out)
	{
		out.clear();
		out.reserve(static_cast<size_t>(std::max(HouseClass::Array.Count, 0)));
		for (int i = 0; i < HouseClass::Array.Count; ++i)
		{
			const auto* const pHouse = HouseClass::Array.Items[i];
			if (!pHouse)
				continue;

			out.push_back(HouseRepairSnapshot {
				UniqueIDOf(pHouse),
				pHouse->Repairing,
				pHouse->RepairTimer.StartTime,
				pHouse->RepairTimer.TimeLeft
			});
		}
	}

	void RestoreHouseRepairState(const SnapshotData& snapshot, int keyframeFrame)
	{
		std::unordered_map<uint32_t, HouseClass*> byId;
		byId.reserve(static_cast<size_t>(std::max(HouseClass::Array.Count, 0)));
		for (int i = 0; i < HouseClass::Array.Count; ++i)
		{
			if (auto* const pHouse = HouseClass::Array.Items[i])
				byId.emplace(UniqueIDOf(pHouse), pHouse);
		}

		int restored = 0;
		uint32_t first = 0;
		for (const auto& saved : snapshot.HouseRepairs)
		{
			const auto found = byId.find(saved.Id);
			if (found == byId.end())
				continue;

			auto* const pHouse = found->second;
			if (pHouse->Repairing == saved.DidRepair
				&& pHouse->RepairTimer.StartTime == saved.RepairTimerStart
				&& pHouse->RepairTimer.TimeLeft == saved.RepairTimerLeft)
			{
				continue;
			}

			if (!first)
				first = saved.Id;
			pHouse->Repairing = saved.DidRepair;
			pHouse->RepairTimer.StartTime = saved.RepairTimerStart;
			pHouse->RepairTimer.TimeLeft = saved.RepairTimerLeft;
			++restored;
		}

		if (restored > 0)
		{
			Debug::Log("[Replay] Keyframe %d restored the repair gate and timer on %d houses "
				"after the load (first house %u).\n", keyframeFrame, restored, first);
		}
	}

	void RestoreTechnoInPlayfieldState(const SnapshotData& snapshot, int keyframeFrame)
	{
		std::unordered_map<uint32_t, TechnoClass*> byId;
		byId.reserve(static_cast<size_t>(std::max(TechnoClass::Array.Count, 0)));
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			if (auto* const pTechno = TechnoClass::Array.Items[i])
				byId.emplace(UniqueIDOf(pTechno), pTechno);
		}

		int restored = 0;
		uint32_t first = 0;
		for (const auto& saved : snapshot.Technos)
		{
			const auto found = byId.find(saved.Id);
			if (found == byId.end() || found->second->IsInPlayfield == saved.IsInPlayfield)
				continue;

			if (!first)
				first = saved.Id;
			found->second->IsInPlayfield = saved.IsInPlayfield;
			++restored;
		}

		if (restored > 0)
		{
			Debug::Log("[Replay] Keyframe %d restored IsInPlayfield on %d technos after the load "
				"(first object %u).\n", keyframeFrame, restored, first);
		}
	}
	#pragma region Slave manager state
	uint32_t KeyOfManagerOwner(const AbstractClass* pOwner)
	{
		return UniqueIDOf(pOwner);
	}

	bool AddKeyed(std::vector<uint32_t>& seen, uint32_t key)
	{
		if (key == 0)
			return false;

		if (std::find(seen.begin(), seen.end(), key) != seen.end())
			return false;

		seen.push_back(key);
		return true;
	}

	static_assert(sizeof(SlaveManagerClass::SlaveControl) == 0x14,
		"SlaveManagerClass::Load (0x6B1170) reads each control as twenty bytes");
	static_assert(offsetof(SlaveManagerClass, State) == 0x5C,
		"SlaveManagerClass::Size_Of (0x6B1370) covers 0x64 bytes ending with LastScanFrame");
	static_assert(offsetof(SlaveManagerClass, LastScanFrame) == 0x60,
		"SlaveManagerClass::Size_Of (0x6B1370) covers 0x64 bytes ending with LastScanFrame");

	// Preserve the manager's scan/state timers and each slave's assignment, state, and
	// respawn timer. Resolve saved IDs to loaded objects instead of keeping old pointers.
	void CaptureSlaveManagerState(std::vector<SlaveManagerSnapshot>& out)
	{
		out.clear();
		out.reserve(static_cast<size_t>(std::max(SlaveManagerClass::Array.Count, 0)));

		std::vector<uint32_t> seen;
		seen.reserve(static_cast<size_t>(std::max(SlaveManagerClass::Array.Count, 0)));
		int unkeyed = 0;

		for (int i = 0; i < SlaveManagerClass::Array.Count; ++i)
		{
			const auto* const pManager = SlaveManagerClass::Array.Items[i];
			if (!pManager)
				continue;

			// A manager whose owner has been killed does nothing at all - SlaveManagerClass::AI
			// (0x6AF5F0) returns before Slave_AI when Owner is null - so there is nothing to carry.
			const uint32_t key = KeyOfManagerOwner(pManager->Owner);
			if (!AddKeyed(seen, key))
			{
				++unkeyed;
				continue;
			}

			SlaveManagerSnapshot entry {};
			entry.Id = key;
			entry.Owner = key;
			entry.State = static_cast<int32_t>(pManager->State);
			entry.LastScanFrame = pManager->LastScanFrame;
			entry.TimerStart = pManager->RespawnTimer.StartTime;
			entry.TimerLeft = pManager->RespawnTimer.TimeLeft;

			entry.Controls.reserve(static_cast<size_t>(std::max(pManager->SlaveNodes.Count, 0)));
			for (int at = 0; at < pManager->SlaveNodes.Count; ++at)
			{
				const auto* const pControl = pManager->SlaveNodes.Items[at];
				if (!pControl)
				{
					entry.Controls.push_back(SlaveControlSnapshot {});
					continue;
				}

				entry.Controls.push_back(SlaveControlSnapshot {
					UniqueIDOf(pControl->Slave),
					static_cast<int32_t>(pControl->State),
					pControl->RespawnTimer.StartTime,
					pControl->RespawnTimer.TimeLeft
				});
			}

			out.push_back(std::move(entry));
		}

		if (unkeyed > 0)
		{
			Debug::Log("[Replay] Keyframe %d is carrying %d slave managers and leaving %d that have "
				"no owner to name them by.\n", static_cast<int>(Unsorted::CurrentFrame),
				static_cast<int>(out.size()), unkeyed);
		}
	}

	void RestoreSlaveManagerState(const SnapshotData& snapshot, int keyframeFrame)
	{
		if (snapshot.SlaveManagers.empty())
			return;

		std::unordered_map<uint32_t, SlaveManagerClass*> byId;
		byId.reserve(static_cast<size_t>(std::max(SlaveManagerClass::Array.Count, 0)));
		for (int i = 0; i < SlaveManagerClass::Array.Count; ++i)
		{
			auto* const pManager = SlaveManagerClass::Array.Items[i];
			if (!pManager)
				continue;

			const uint32_t key = KeyOfManagerOwner(pManager->Owner);
			if (key != 0)
				byId.emplace(key, pManager);
		}

		// The slave a control holds is a pointer, so putting one back means finding the infantry
		// the recording had there. One that cannot be found is reported rather than guessed at.
		std::unordered_map<uint32_t, InfantryClass*> infantryById;
		infantryById.reserve(static_cast<size_t>(std::max(InfantryClass::Array.Count, 0)));
		for (int i = 0; i < InfantryClass::Array.Count; ++i)
		{
			if (auto* const pInfantry = InfantryClass::Array.Items[i])
				infantryById.emplace(UniqueIDOf(pInfantry), pInfantry);
		}

		int managersChanged = 0;
		int controlsChanged = 0;
		int missing = 0;
		int reshaped = 0;
		int reported = 0;

		for (const auto& saved : snapshot.SlaveManagers)
		{
			const auto found = byId.find(saved.Id);
			if (found == byId.end())
			{
				++missing;
				continue;
			}

			auto* const pManager = found->second;
			bool managerDiffered = false;

			if (static_cast<int32_t>(pManager->State) != saved.State
				|| pManager->LastScanFrame != saved.LastScanFrame
				|| pManager->RespawnTimer.StartTime != saved.TimerStart
				|| pManager->RespawnTimer.TimeLeft != saved.TimerLeft)
			{
				if (reported < 4)
				{
					++reported;
					Debug::Log("[Replay] Keyframe %d: the slave manager owned by %u came back in state "
						"%d, last scan %d, gate %d/%d; it was state %d, last scan %d, gate %d/%d.\n",
						keyframeFrame, saved.Owner,
						static_cast<int>(pManager->State), pManager->LastScanFrame,
						pManager->RespawnTimer.StartTime, pManager->RespawnTimer.TimeLeft,
						saved.State, saved.LastScanFrame, saved.TimerStart, saved.TimerLeft);
				}

				pManager->State = static_cast<SlaveManagerStatus>(saved.State);
				pManager->LastScanFrame = saved.LastScanFrame;
				pManager->RespawnTimer.StartTime = saved.TimerStart;
				pManager->RespawnTimer.TimeLeft = saved.TimerLeft;
				managerDiffered = true;
			}

			if (pManager->SlaveNodes.Count != static_cast<int>(saved.Controls.size()))
			{
				++reshaped;
				Debug::Log("[Replay] Keyframe %d: the slave manager owned by %u came back with %d "
					"controls; it had %d. Leaving them as the load built them.\n", keyframeFrame,
					saved.Owner,
					pManager->SlaveNodes.Count, static_cast<int>(saved.Controls.size()));
			}
			else
			{
				for (int at = 0; at < pManager->SlaveNodes.Count; ++at)
				{
					auto* const pControl = pManager->SlaveNodes.Items[at];
					if (!pControl)
						continue;

					const SlaveControlSnapshot& want = saved.Controls[static_cast<size_t>(at)];
					const uint32_t slave = UniqueIDOf(pControl->Slave);
					if (slave == want.Slave && static_cast<int32_t>(pControl->State) == want.State
						&& pControl->RespawnTimer.StartTime == want.TimerStart
						&& pControl->RespawnTimer.TimeLeft == want.TimerLeft)
					{
						continue;
					}

					if (reported < 4)
					{
						++reported;
						Debug::Log("[Replay] Keyframe %d: the slave manager owned by %u had control %d "
							"come back holding slave %u in state %d with timer %d/%d; it held slave %u in "
							"state %d with timer %d/%d.\n", keyframeFrame, saved.Owner, at,
							slave, static_cast<int>(pControl->State),
							pControl->RespawnTimer.StartTime, pControl->RespawnTimer.TimeLeft,
							want.Slave, want.State, want.TimerStart, want.TimerLeft);
					}

					if (slave != want.Slave)
					{
						const auto wanted = infantryById.find(want.Slave);
						if (want.Slave == 0)
							pControl->Slave = nullptr;
						else if (wanted != infantryById.end())
							pControl->Slave = wanted->second;
					}

					pControl->State = static_cast<SlaveControlStatus>(want.State);
					pControl->RespawnTimer.StartTime = want.TimerStart;
					pControl->RespawnTimer.TimeLeft = want.TimerLeft;
					++controlsChanged;
					managerDiffered = true;
				}
			}

			if (managerDiffered)
				++managersChanged;
		}

		if (managersChanged == 0 && missing == 0 && reshaped == 0)
		{
			Debug::Log("[Replay] Keyframe %d found all %d slave managers already as it recorded "
				"them.\n", keyframeFrame, static_cast<int>(snapshot.SlaveManagers.size()));
			return;
		}

		Debug::Log("[Replay] Keyframe %d put back %d of %d slave managers (%d controls); %d were "
			"not in the world and %d had a different number of controls.\n", keyframeFrame,
			managersChanged, static_cast<int>(snapshot.SlaveManagers.size()), controlsChanged,
			missing, reshaped);
	}

	#pragma endregion Slave manager state

	#pragma region Frame timers the load resets

	// The offsets the four Load overrides write through, so a YRpp layout change is a build error
	// here rather than three silently wrong timers at runtime.
	static_assert(offsetof(SlaveManagerClass, RespawnTimer) == 0x50,
		"SlaveManagerClass::AI (0x6AF5F0) reads its gate at SlaveManagerClass+0x50");
	static_assert(offsetof(SpawnManagerClass, UpdateTimer) == 0x50,
		"SpawnManagerClass::Load (0x6B7F10) resets the update timer at SpawnManagerClass+0x50");
	static_assert(offsetof(SpawnManagerClass, SpawnTimer) == 0x5C,
		"SpawnManagerClass::Load (0x6B7F10) resets the spawn timer at SpawnManagerClass+0x5C");
	static_assert(offsetof(BulletClass, Data) == 0xB8,
		"BulletClass::Load (0x46AE70) resets both flight timers at BulletClass+0xB8");

	std::array<CDTimerClass*, 2> TimersOfSpawnManager(SpawnManagerClass* pManager)
	{
		return { &pManager->UpdateTimer, &pManager->SpawnTimer };
	}

	std::array<CDTimerClass*, 2> TimersOfBullet(BulletClass* pBullet)
	{
		return { &pBullet->Data.UnknownTimer, &pBullet->Data.ArmTimer };
	}

	template <typename TArray, typename TKeyOf, typename TTimersOf>
	void CaptureLoadResetTimers(TArray& array, std::vector<LoadResetTimerSnapshot>& out,
		TKeyOf keyOf, TTimersOf timersOf)
	{
		out.clear();
		out.reserve(static_cast<size_t>(std::max(array.Count, 0)));

		std::vector<uint32_t> seen;
		seen.reserve(static_cast<size_t>(std::max(array.Count, 0)));

		for (int i = 0; i < array.Count; ++i)
		{
			auto* const pItem = array.Items[i];
			if (!pItem)
				continue;

			const uint32_t key = keyOf(pItem);
			if (!AddKeyed(seen, key))
				continue;

			const std::array<CDTimerClass*, 2> timers = timersOf(pItem);

			LoadResetTimerSnapshot entry {};
			entry.Id = key;
			if (timers[0])
			{
				entry.FirstStart = timers[0]->StartTime;
				entry.FirstLeft = timers[0]->TimeLeft;
			}

			if (timers[1])
			{
				entry.SecondStart = timers[1]->StartTime;
				entry.SecondLeft = timers[1]->TimeLeft;
			}

			out.push_back(entry);
		}
	}

	// Matched by unique ID rather than by position: the load rebuilds these arrays as it
	// reconstructs the object graph, and an object that is no longer there is simply skipped.
	template <typename TArray, typename TKeyOf, typename TTimersOf>
	int RestoreLoadResetTimers(TArray& array, const std::vector<LoadResetTimerSnapshot>& saved,
		TKeyOf keyOf, TTimersOf timersOf)
	{
		if (saved.empty())
			return 0;

		using Pointer = std::remove_reference_t<decltype(array.Items[0])>;
		std::unordered_map<uint32_t, Pointer> byId;
		byId.reserve(static_cast<size_t>(std::max(array.Count, 0)));
		for (int i = 0; i < array.Count; ++i)
		{
			auto* const pItem = array.Items[i];
			if (!pItem)
				continue;

			const uint32_t key = keyOf(pItem);
			if (key != 0)
				byId.emplace(key, pItem);
		}

		int restored = 0;
		for (const auto& entry : saved)
		{
			const auto found = byId.find(entry.Id);
			if (found == byId.end())
				continue;

			const std::array<CDTimerClass*, 2> timers = timersOf(found->second);
			bool changed = false;

			if (timers[0] && (timers[0]->StartTime != entry.FirstStart
				|| timers[0]->TimeLeft != entry.FirstLeft))
			{
				timers[0]->StartTime = entry.FirstStart;
				timers[0]->TimeLeft = entry.FirstLeft;
				changed = true;
			}

			if (timers[1] && (timers[1]->StartTime != entry.SecondStart
				|| timers[1]->TimeLeft != entry.SecondLeft))
			{
				timers[1]->StartTime = entry.SecondStart;
				timers[1]->TimeLeft = entry.SecondLeft;
				changed = true;
			}

			if (changed)
				++restored;
		}

		return restored;
	}

	// Load initialization resets spawn-manager and bullet timers. Restore their saved
	// start/remaining values after session setup to retain spawn and projectile timing.
	void CaptureLoadResetTimerState(LoadResetTimerSnapshots& out)
	{
		CaptureLoadResetTimers(SpawnManagerClass::Array, out.SpawnManagers,
			[](const SpawnManagerClass* p) { return KeyOfManagerOwner(p->Owner); },
			TimersOfSpawnManager);
		CaptureLoadResetTimers(BulletClass::Array, out.Bullets,
			[](const BulletClass* p) { return UniqueIDOf(p); }, TimersOfBullet);
	}

	void RestoreLoadResetTimerState(const SnapshotData& snapshot, int keyframeFrame)
	{
		const int spawns = RestoreLoadResetTimers(SpawnManagerClass::Array,
			snapshot.LoadResetTimers.SpawnManagers,
			[](const SpawnManagerClass* p) { return KeyOfManagerOwner(p->Owner); },
			TimersOfSpawnManager);
		const int bullets = RestoreLoadResetTimers(BulletClass::Array,
			snapshot.LoadResetTimers.Bullets,
			[](const BulletClass* p) { return UniqueIDOf(p); }, TimersOfBullet);

		if (spawns == 0 && bullets == 0)
		{
			Debug::Log("[Replay] Keyframe %d found the %d spawn managers and %d bullets already "
				"holding the timers it recorded.\n", keyframeFrame,
				static_cast<int>(snapshot.LoadResetTimers.SpawnManagers.size()),
				static_cast<int>(snapshot.LoadResetTimers.Bullets.size()));
			return;
		}

		Debug::Log("[Replay] Keyframe %d put back the frame timers the load had reset: %d of %d "
			"spawn managers, %d of %d bullets.\n", keyframeFrame,
			spawns, static_cast<int>(snapshot.LoadResetTimers.SpawnManagers.size()),
			bullets, static_cast<int>(snapshot.LoadResetTimers.Bullets.size()));
	}

	#pragma endregion Frame timers the load resets
}
