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

#include "ReplayObjects.h"
#include "ReplaySystem.h"

#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <HouseClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace ReplaySystem::Objects
{
	using namespace Replay;

	namespace
	{
		// A lost-and-found list rather than a bound on anything real: a death is normally consumed by
		// the next snapshot, and one for an object that never leaves would otherwise sit here forever.
		constexpr size_t MaxPendingDeaths = 4096;

		struct KnownObject
		{
			ObjectAppearRecord Appear {};
			ObjectUpdateRecord Update {};
			uint32_t SeenInSnapshot = 0;
		};

		struct Death
		{
			uint16_t X = 0;
			uint16_t Y = 0;
			uint8_t KillerHouse = ObjectNoHouse;
		};

		// Enough for a large 8-player game without the table rehashing as armies are built.
		constexpr size_t ExpectedObjects = 4096;

		struct ObjectsState
		{
			// What the last snapshot told a reader, by unique ID.
			std::unordered_map<uint32_t, KnownObject> Present;
			// Kills recorded since the last snapshot, by the victim's unique ID.
			std::unordered_map<uint32_t, Death> Deaths;
			uint32_t Snapshot = 0;
			// Buffers returned by written captures, handed to the next snapshot with their capacity.
			std::vector<ObjectAppearRecord> SpareAppeared;
			std::vector<ObjectUpdateRecord> SpareUpdated;
			std::vector<ObjectGoneRecord> SpareGone;
		};

		ObjectsState State;

		uint16_t Position(int leptons)
		{
			return static_cast<uint16_t>(std::clamp(leptons >> ObjectPositionShift, 0, 0xFFFF));
		}

		bool KindOf(TechnoClass* pTechno, uint8_t& kind)
		{
			switch (pTechno->WhatAmI())
			{
			case AbstractType::Unit: kind = ObjectRecordKind_Unit; return true;
			case AbstractType::Infantry: kind = ObjectRecordKind_Infantry; return true;
			case AbstractType::Aircraft: kind = ObjectRecordKind_Aircraft; return true;
			case AbstractType::Building: kind = ObjectRecordKind_Building; return true;
			default: return false;
			}
		}

		uint8_t HouseByte(const HouseClass* pHouse)
		{
			return pHouse && pHouse->ArrayIndex >= 0 && pHouse->ArrayIndex < ObjectNoHouse
				? static_cast<uint8_t>(pHouse->ArrayIndex)
				: ObjectNoHouse;
		}

		bool Describe(TechnoClass* pTechno, ObjectAppearRecord& appear, ObjectUpdateRecord& update)
		{
			uint8_t kind = 0;
			const auto* pType = pTechno->GetTechnoType();
			if (!pType || !KindOf(pTechno, kind))
				return false;

			const auto id = static_cast<uint32_t>(pTechno->UniqueID);

			appear = {};
			appear.UniqueID = id;
			appear.TypeIndex = static_cast<uint16_t>(std::clamp(pType->GetArrayIndex(), 0, 0xFFFF));
			appear.Kind = kind;
			appear.Owner = HouseByte(pTechno->Owner);
			appear.FoundationWidth = 1;
			appear.FoundationHeight = 1;
			if (kind == ObjectRecordKind_Building)
			{
				const auto* pBuildingType = static_cast<BuildingClass*>(pTechno)->Type;
				if (!pBuildingType)
					return false;
				appear.FoundationWidth = static_cast<uint8_t>(std::clamp<int>(pBuildingType->GetFoundationWidth(), 1, 0xFF));
				appear.FoundationHeight = static_cast<uint8_t>(std::clamp<int>(pBuildingType->GetFoundationHeight(false), 1, 0xFF));
			}

			update = {};
			update.UniqueID = id;
			update.X = Position(pTechno->Location.X);
			update.Y = Position(pTechno->Location.Y);
			const int strength = pType->Strength;
			update.Health = static_cast<uint8_t>(strength > 0
				? std::clamp(pTechno->Health * 255 / strength, 0, 255)
				: 255);
			const int mission = static_cast<int>(pTechno->CurrentMission);
			update.Mission = mission >= 0 && mission < 0xFF ? static_cast<uint8_t>(mission) : 0xFF;
			uint8_t flags = 0;
			if (pTechno->Veterancy.IsElite())
				flags |= ObjectRecordFlag_Elite;
			else if (pTechno->Veterancy.IsVeteran())
				flags |= ObjectRecordFlag_Veteran;
			if (pTechno->CloakState != CloakState::Uncloaked)
				flags |= ObjectRecordFlag_Cloaked;
			update.Flags = flags;
			update.Height = static_cast<uint8_t>(std::clamp(pTechno->Location.Z >> ObjectPositionShift, 0, 0xFF));
			return true;
		}

		bool SameAppearance(const ObjectAppearRecord& a, const ObjectAppearRecord& b)
		{
			return a.TypeIndex == b.TypeIndex && a.Kind == b.Kind && a.Owner == b.Owner;
		}
	}

	void Reset()
	{
		State = {};
		State.Present.reserve(ExpectedObjects);
	}

	void RecycleBuffers(std::vector<ObjectAppearRecord>& appeared,
		std::vector<ObjectUpdateRecord>& updated,
		std::vector<ObjectGoneRecord>& gone)
	{
		// Keep whichever buffer has grown larger.
		if (appeared.capacity() > State.SpareAppeared.capacity())
			appeared.swap(State.SpareAppeared);
		if (updated.capacity() > State.SpareUpdated.capacity())
			updated.swap(State.SpareUpdated);
		if (gone.capacity() > State.SpareGone.capacity())
			gone.swap(State.SpareGone);
	}

	void FillSnapshot(std::vector<ObjectAppearRecord>& appeared,
		std::vector<ObjectUpdateRecord>& updated,
		std::vector<ObjectGoneRecord>& gone)
	{
		// A fresh capture's vectors are empty; take the spares, which already have room.
		if (appeared.capacity() < State.SpareAppeared.capacity())
			appeared.swap(State.SpareAppeared);
		if (updated.capacity() < State.SpareUpdated.capacity())
			updated.swap(State.SpareUpdated);
		if (gone.capacity() < State.SpareGone.capacity())
			gone.swap(State.SpareGone);
		appeared.clear();
		updated.clear();
		gone.clear();

		const uint32_t snapshot = ++State.Snapshot;
		const auto limit = static_cast<size_t>(MaxObjectRecordsPerFrame);

		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			TechnoClass* pTechno = TechnoClass::Array.Items[i];
			// Limbo covers a factory's unfinished product and anything inside a transport or building:
			// not on the map, so not drawn. It comes back as a new appearance when it leaves.
			if (!pTechno || !pTechno->IsAlive || pTechno->InLimbo)
				continue;

			ObjectAppearRecord appear;
			ObjectUpdateRecord update;
			if (!Describe(pTechno, appear, update))
				continue;

			auto [it, isNew] = State.Present.try_emplace(appear.UniqueID);
			KnownObject& known = it->second;

			if ((isNew || !SameAppearance(known.Appear, appear)) && appeared.size() < limit)
			{
				// A capture or mind control calls Record_The_Kill_Object without a killer, leaving a death
				// for an object that lives on. An owner change tells them apart.
				if (!isNew && known.Appear.Owner != appear.Owner)
					State.Deaths.erase(appear.UniqueID);
				appeared.push_back(appear);
				known.Appear = appear;
			}

			// Only where an object first appears is written. Movement and state changes were most of
			// a long game's replay size; the position is still kept so a removal reports where it was.
			if (isNew && updated.size() < limit)
				updated.push_back(update);
			known.Update = update;

			known.SeenInSnapshot = snapshot;
		}

		for (auto it = State.Present.begin(); it != State.Present.end();)
		{
			if (it->second.SeenInSnapshot == snapshot)
			{
				++it;
				continue;
			}

			if (gone.size() < limit)
			{
				ObjectGoneRecord record {};
				record.UniqueID = it->first;
				record.X = it->second.Update.X;
				record.Y = it->second.Update.Y;
				record.Reason = ObjectGoneReason_Removed;
				record.KillerHouse = ObjectNoHouse;

				const auto death = State.Deaths.find(it->first);
				if (death != State.Deaths.end())
				{
					record.X = death->second.X;
					record.Y = death->second.Y;
					record.Reason = ObjectGoneReason_Destroyed;
					record.KillerHouse = death->second.KillerHouse;
					State.Deaths.erase(death);
				}
				gone.push_back(record);
			}

			it = State.Present.erase(it);
		}

		if (State.Deaths.size() > MaxPendingDeaths)
			State.Deaths.clear();
	}
}

// Runs before the object leaves TechnoClass::Array, so the next snapshot reports where it died
// and who killed it.
void ReplaySystem::RecordObjectDestroyed(TechnoClass* pTechno, HouseClass* pKiller)
{
	if (!ReplaySystem::IsRecordingActive() || !pTechno)
		return;

	auto& death = Objects::State.Deaths[static_cast<uint32_t>(pTechno->UniqueID)];
	death.X = Objects::Position(pTechno->Location.X);
	death.Y = Objects::Position(pTechno->Location.Y);
	death.KillerHouse = Objects::HouseByte(pKiller);
}
