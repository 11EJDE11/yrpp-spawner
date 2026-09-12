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

#include "ReplayStatistics.h"
#include "ReplayFile.h"
#include "ReplaySystem.h"

#include <Utilities/Debug.h>

#include <AircraftClass.h>
#include <AircraftTypeClass.h>
#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <HouseClass.h>
#include <HouseTypeClass.h>
#include <InfantryClass.h>
#include <InfantryTypeClass.h>
#include <TechnoClass.h>
#include <UnitClass.h>
#include <UnitTypeClass.h>
#include <Unsorted.h>

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <string>

namespace ReplaySystem::Statistics
{
	using namespace Replay;

	namespace
	{
		// UnitTrackerClass::UnitTotals.
		constexpr int TrackerCapacity = 0x200;
		constexpr size_t MaxTypeNameLength = 128;
		// The statistics packet is built in a 0x8000-byte stack buffer (Send_Statistics_Packet).
		constexpr int MaxStatsPacketBytes = 0x8000;
		// Globals Send_Statistics_Packet reads for OOSY and FINI.
		constexpr uintptr_t OutOfSyncAddress = 0xA8B8C2;
		constexpr uintptr_t SawCompletionAddress = 0xA8B8C1;

		// Lost counts by kind, in StatisticsHouseArray_Lost* order.
		enum LostKind { Lost_Aircraft, Lost_Infantry, Lost_Units, Lost_Buildings, Lost_Count };

		struct TypeEntry
		{
			char ID[0x18] = {};
			char Cameo[0x19] = {};
			std::wstring Name;
			int32_t Cost = 0;
			uint32_t Flags = 0;
		};

		struct TypeList
		{
			AbstractType Kind = AbstractType::None;
			std::vector<TypeEntry> Entries;
		};

		struct HouseSnapshot
		{
			StatisticsHouseRecord Record {};
			std::array<std::vector<int32_t>, StatisticsHouseArray_Count> Arrays;
			int32_t UnitsBuilt = 0;
			int32_t BuildingsBuilt = 0;
		};

		// What the recorder counts itself, because the engine does not: indexed by house.
		struct HouseCounters
		{
			std::array<std::vector<int32_t>, Lost_Count> Lost;
		};

		struct StatisticsState
		{
			std::vector<TypeList> Types;
			std::vector<HouseSnapshot> Houses;
			std::vector<HouseCounters> Counters;
			// Send_Statistics_Packet clears the built counts and refills them with what each house
			// still owns (0x6C7D98-0x6C7EA8) - that is where the packet's UNL/INL/PLL/BLL "left"
			// arrays come from - and hands the arrays back byte-swapped. After that they describe the
			// packet rather than the game, so the snapshot is taken just before and then held.
			bool Frozen = false;
			std::vector<unsigned char> StatsPacket;
			std::vector<int32_t> ArmyValue;
			std::vector<int32_t> BuildingValue;
			int OutOfSyncFrame = -1;
		};

		StatisticsState State;

		bool IsTrackedHouse(HouseClass* pHouse)
		{
			// Special, Neutral and the civilian houses are MultiplayPassive; they own nothing worth charting.
			return pHouse && pHouse->Type && !pHouse->Type->MultiplayPassive;
		}

		HouseCounters& CountersFor(int houseIndex)
		{
			if (static_cast<size_t>(houseIndex) >= State.Counters.size())
				State.Counters.resize(static_cast<size_t>(houseIndex) + 1);
			return State.Counters[static_cast<size_t>(houseIndex)];
		}

		// To_Network_Format (0x749100) byte-swaps the array in place for the packet; nothing promises
		// the swap is undone, so read through the flag the tracker keeps.
		int32_t TrackerValue(const UnitTrackerClass& tracker, int index)
		{
			const int32_t value = tracker.UnitTotals[index];
			return tracker.InNetworkFormat
				? static_cast<int32_t>(_byteswap_ulong(static_cast<unsigned long>(value)))
				: value;
		}

		void TrimTrailingZeros(std::vector<int32_t>& values)
		{
			while (!values.empty() && values.back() == 0)
				values.pop_back();
		}

		std::vector<int32_t> TrackerArray(const UnitTrackerClass& tracker, int length)
		{
			std::vector<int32_t> values;
			length = std::clamp(length, 0, TrackerCapacity);
			values.reserve(static_cast<size_t>(length));
			for (int i = 0; i < length; ++i)
				values.push_back(TrackerValue(tracker, i));

			TrimTrailingZeros(values);
			return values;
		}

		int32_t Sum(const std::vector<int32_t>& values)
		{
			int64_t total = 0;
			for (int32_t value : values)
				total += value;
			return static_cast<int32_t>(std::clamp<int64_t>(total, 0, INT32_MAX));
		}

		// How many of each type the house owns right now, the way the packet's "left" arrays count.
		template <typename TObjectArray>
		std::vector<int32_t> CountOwned(const TObjectArray& objects, const HouseClass* pHouse, int typeCount)
		{
			std::vector<int32_t> counts(static_cast<size_t>(std::max(typeCount, 0)), 0);
			for (int i = 0; i < objects.Count; ++i)
			{
				auto* pObject = objects.Items[i];
				if (!pObject || pObject->Owner != pHouse || !pObject->IsAlive)
					continue;

				const auto* pType = pObject->GetTechnoType();
				const int index = pType ? pType->GetArrayIndex() : -1;
				if (index >= 0 && index < typeCount)
					++counts[static_cast<size_t>(index)];
			}

			TrimTrailingZeros(counts);
			return counts;
		}

		std::vector<int32_t> CountOwnedBuildings(HouseClass* pHouse, int typeCount)
		{
			std::vector<int32_t> counts(static_cast<size_t>(std::max(typeCount, 0)), 0);
			for (auto* pBuilding : pHouse->Buildings)
			{
				const auto* pType = pBuilding ? pBuilding->Type : nullptr;
				const int index = pType ? pType->GetArrayIndex() : -1;
				if (index >= 0 && index < typeCount)
					++counts[static_cast<size_t>(index)];
			}

			TrimTrailingZeros(counts);
			return counts;
		}

		uint32_t HouseFlags(HouseClass* pHouse)
		{
			uint32_t flags = 0;
			if (pHouse->Defeated)
				flags |= HouseStatsFlag_Defeated;
			if (pHouse->IsWinner)
				flags |= HouseStatsFlag_Winner;
			if (pHouse->IsLoser)
				flags |= HouseStatsFlag_Loser;
			if (pHouse->IsObserver() || pHouse->IsInitiallyObserver())
				flags |= HouseStatsFlag_Observer;
			if (pHouse->IsHumanPlayer)
				flags |= HouseStatsFlag_Human;
			if (pHouse->LostConnection)
				flags |= HouseStatsFlag_LostConnection;
			if (pHouse->IsResigner)
				flags |= HouseStatsFlag_Resigned;
			if (pHouse == HouseClass::CurrentPlayer)
				flags |= HouseStatsFlag_RecordingPlayer;
			return flags;
		}

		// Summed Cost of everything each house owns that is really on the field. A factory holds
		// what it is building as a limbo object, so limbo only counts inside a transport.
		void ComputeValues()
		{
			const int houseCount = HouseClass::Array.Count;
			State.ArmyValue.assign(static_cast<size_t>(std::max(houseCount, 0)), 0);
			State.BuildingValue.assign(static_cast<size_t>(std::max(houseCount, 0)), 0);

			for (int i = 0; i < TechnoClass::Array.Count; ++i)
			{
				TechnoClass* pTechno = TechnoClass::Array.Items[i];
				if (!pTechno || !pTechno->IsAlive || !pTechno->Owner)
					continue;

				if (pTechno->InLimbo && !pTechno->Transporter)
					continue;

				const auto* pType = pTechno->GetTechnoType();
				const int house = pTechno->Owner->ArrayIndex;
				if (!pType || house < 0 || house >= houseCount)
					continue;

				auto& bucket = pTechno->WhatAmI() == AbstractType::Building ? State.BuildingValue : State.ArmyValue;
				bucket[static_cast<size_t>(house)] += pType->Cost;
			}
		}

		template <typename TTypeArray>
		void CaptureTypeList(AbstractType kind, const TTypeArray& types)
		{
			TypeList list;
			list.Kind = kind;
			list.Entries.reserve(static_cast<size_t>(std::max(types.Count, 0)));

			for (int i = 0; i < types.Count; ++i)
			{
				TypeEntry entry;
				if (const auto* pType = types.Items[i])
				{
					memcpy(entry.ID, pType->ID, sizeof(entry.ID) - 1);
					memcpy(entry.Cameo, pType->CameoFile, sizeof(entry.Cameo) - 1);
					// UIName is the string table entry, so the name reads as the game showed it.
					if (pType->UIName)
						entry.Name.assign(pType->UIName, wcsnlen(pType->UIName, MaxTypeNameLength));
					entry.Cost = pType->Cost;
					if (pType->Naval)
						entry.Flags |= StatisticsTypeFlag_Naval;
					if (pType->DontScore)
						entry.Flags |= StatisticsTypeFlag_DontScore;
					if (pType->Insignificant)
						entry.Flags |= StatisticsTypeFlag_Insignificant;
				}
				list.Entries.push_back(std::move(entry));
			}

			State.Types.push_back(std::move(list));
		}

		// The type arrays are complete once the scenario has loaded - the map appends its own types
		// while it is read - and do not change afterwards.
		void CaptureTypes()
		{
			State.Types.clear();
			CaptureTypeList(AbstractType::BuildingType, BuildingTypeClass::Array);
			CaptureTypeList(AbstractType::InfantryType, InfantryTypeClass::Array);
			CaptureTypeList(AbstractType::UnitType, UnitTypeClass::Array);
			CaptureTypeList(AbstractType::AircraftType, AircraftTypeClass::Array);
		}

		void FillRecord(HouseClass* pHouse, StatisticsHouseRecord& record)
		{
			record = {};
			record.HouseIndex = pHouse->ArrayIndex;
			// Fixed arrays that need not be terminated: copy a bounded prefix, keep the terminator.
			wmemcpy(record.Name, pHouse->UIName, std::size(record.Name) - 1);
			memcpy(record.Country, pHouse->Type->ID, sizeof(record.Country) - 1);
			record.ColorSchemeIndex = pHouse->ColorSchemeIndex;
			record.SpawnPosition = pHouse->GetSpawnPosition();
			record.Allies = pHouse->Allies.data;
			record.Flags = HouseFlags(pHouse);
			record.Credits = pHouse->Balance;
			record.StoredOreValue = pHouse->OwnedTiberium.GetTotalValue();
			record.CreditsSpent = pHouse->CreditsSpent;
			record.HarvestedCredits = pHouse->HarvestedCredits;
			record.PowerOutput = pHouse->PowerOutput;
			record.PowerDrain = pHouse->PowerDrain;
			// YRpp names these TotalKilledUnits/TotalKilledBuildings, but TechnoClass::Record_The_Kill_Object
			// (0x702D40) increments them on the house that *lost* the object. The per-house arrays next
			// to them are the kills, credited to the killer and indexed by the victim's house.
			record.UnitsLost = pHouse->TotalKilledUnits;
			record.BuildingsLost = pHouse->TotalKilledBuildings;
			record.Score = pHouse->PointTotal;
			for (size_t i = 0; i < std::size(record.UnitsKilledOfHouse); ++i)
			{
				record.UnitsKilledOfHouse[i] = pHouse->KilledUnitsOfHouses[i];
				record.BuildingsKilledOfHouse[i] = pHouse->KilledBuildingsOfHouses[i];
			}
		}

		// The recorder's own counters, which keep counting after the snapshot freezes.
		void ApplyCounters(HouseSnapshot& snapshot)
		{
			const auto& counters = CountersFor(snapshot.Record.HouseIndex);
			auto& a = snapshot.Arrays;
			a[StatisticsHouseArray_LostAircraft] = counters.Lost[Lost_Aircraft];
			a[StatisticsHouseArray_LostInfantry] = counters.Lost[Lost_Infantry];
			a[StatisticsHouseArray_LostUnits] = counters.Lost[Lost_Units];
			a[StatisticsHouseArray_LostBuildings] = counters.Lost[Lost_Buildings];
		}

		void RefreshSnapshot()
		{
			if (State.Frozen)
				return;

			const int aircraftTypes = AircraftTypeClass::Array.Count;
			const int infantryTypes = InfantryTypeClass::Array.Count;
			const int unitTypes = UnitTypeClass::Array.Count;
			const int buildingTypes = BuildingTypeClass::Array.Count;

			State.Houses.clear();
			for (int i = 0; i < HouseClass::Array.Count; ++i)
			{
				HouseClass* pHouse = HouseClass::Array.Items[i];
				if (!IsTrackedHouse(pHouse))
					continue;

				HouseSnapshot snapshot;
				FillRecord(pHouse, snapshot.Record);

				auto& a = snapshot.Arrays;
				a[StatisticsHouseArray_BuiltAircraft] = TrackerArray(pHouse->BuiltAircraftTypes, aircraftTypes);
				a[StatisticsHouseArray_BuiltInfantry] = TrackerArray(pHouse->BuiltInfantryTypes, infantryTypes);
				a[StatisticsHouseArray_BuiltUnits] = TrackerArray(pHouse->BuiltUnitTypes, unitTypes);
				a[StatisticsHouseArray_BuiltBuildings] = TrackerArray(pHouse->BuiltBuildingTypes, buildingTypes);
				a[StatisticsHouseArray_KilledAircraft] = TrackerArray(pHouse->KilledAircraftTypes, aircraftTypes);
				a[StatisticsHouseArray_KilledInfantry] = TrackerArray(pHouse->KilledInfantryTypes, infantryTypes);
				a[StatisticsHouseArray_KilledUnits] = TrackerArray(pHouse->KilledUnitTypes, unitTypes);
				a[StatisticsHouseArray_KilledBuildings] = TrackerArray(pHouse->KilledBuildingTypes, buildingTypes);
				a[StatisticsHouseArray_CapturedBuildings] = TrackerArray(pHouse->CapturedBuildings, buildingTypes);
				a[StatisticsHouseArray_CollectedCrates] = TrackerArray(pHouse->CollectedCrates, TrackerCapacity);
				a[StatisticsHouseArray_LeftAircraft] = CountOwned(AircraftClass::Array, pHouse, aircraftTypes);
				a[StatisticsHouseArray_LeftInfantry] = CountOwned(InfantryClass::Array, pHouse, infantryTypes);
				a[StatisticsHouseArray_LeftUnits] = CountOwned(UnitClass::Array, pHouse, unitTypes);
				a[StatisticsHouseArray_LeftBuildings] = CountOwnedBuildings(pHouse, buildingTypes);

				snapshot.UnitsBuilt = Sum(a[StatisticsHouseArray_BuiltAircraft])
					+ Sum(a[StatisticsHouseArray_BuiltInfantry])
					+ Sum(a[StatisticsHouseArray_BuiltUnits]);
				snapshot.BuildingsBuilt = Sum(a[StatisticsHouseArray_BuiltBuildings]);

				State.Houses.push_back(std::move(snapshot));
			}
		}

		const HouseSnapshot* FindSnapshot(int houseIndex)
		{
			for (const auto& snapshot : State.Houses)
			{
				if (snapshot.Record.HouseIndex == houseIndex)
					return &snapshot;
			}
			return nullptr;
		}

		class ChunkWriter
		{
		public:
			std::vector<unsigned char> Bytes;

			void Raw(const void* data, size_t size)
			{
				const auto* bytes = static_cast<const unsigned char*>(data);
				Bytes.insert(Bytes.end(), bytes, bytes + size);
			}

			template <typename T>
			void Value(const T& value)
			{
				Raw(&value, sizeof(value));
			}

			size_t Begin(uint32_t tag)
			{
				Value(tag);
				const size_t lengthAt = Bytes.size();
				Value(uint32_t { 0 });
				return lengthAt;
			}

			void End(size_t lengthAt)
			{
				const auto length = static_cast<uint32_t>(Bytes.size() - lengthAt - sizeof(uint32_t));
				memcpy(Bytes.data() + lengthAt, &length, sizeof(length));
			}
		};

		void WriteTypes(ChunkWriter& out)
		{
			const size_t chunk = out.Begin(StatisticsChunk_Types);
			out.Value(static_cast<uint32_t>(State.Types.size()));
			for (const auto& list : State.Types)
			{
				out.Value(static_cast<uint32_t>(list.Kind));
				out.Value(static_cast<uint32_t>(list.Entries.size()));
				for (const auto& entry : list.Entries)
				{
					out.Raw(entry.ID, sizeof(entry.ID));
					out.Raw(entry.Cameo, sizeof(entry.Cameo));
					out.Value(static_cast<uint16_t>(entry.Name.size()));
					out.Raw(entry.Name.data(), entry.Name.size() * sizeof(wchar_t));
					out.Value(entry.Cost);
					out.Value(entry.Flags);
				}
			}
			out.End(chunk);
		}

		void WriteHouses(ChunkWriter& out)
		{
			const size_t chunk = out.Begin(StatisticsChunk_Houses);
			out.Value(static_cast<uint32_t>(State.Houses.size()));
			for (auto& snapshot : State.Houses)
			{
				ApplyCounters(snapshot);
				out.Value(snapshot.Record);
				out.Value(static_cast<uint32_t>(snapshot.Arrays.size()));
				for (const auto& values : snapshot.Arrays)
				{
					out.Value(static_cast<uint32_t>(values.size()));
					out.Raw(values.data(), values.size() * sizeof(int32_t));
				}
			}
			out.End(chunk);
		}

		// Every module loaded in the process, so a reader can turn a MoneyInRecord's absolute caller
		// into a module and an offset - and, by the build timestamp, into the right table for that build.
		void WriteModules(ChunkWriter& out)
		{
			const size_t chunk = out.Begin(StatisticsChunk_Modules);
			const size_t countAt = out.Bytes.size();
			out.Value(uint32_t { 0 });

			uint32_t count = 0;
			HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
			if (snapshot != INVALID_HANDLE_VALUE)
			{
				MODULEENTRY32W entry {};
				entry.dwSize = sizeof(entry);
				for (BOOL ok = Module32FirstW(snapshot, &entry); ok; ok = Module32NextW(snapshot, &entry))
				{
					uint32_t timeDateStamp = 0;
					const auto* pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(entry.modBaseAddr);
					if (pDos && pDos->e_magic == IMAGE_DOS_SIGNATURE)
					{
						const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(entry.modBaseAddr + pDos->e_lfanew);
						if (pNt->Signature == IMAGE_NT_SIGNATURE)
							timeDateStamp = pNt->FileHeader.TimeDateStamp;
					}

					const size_t nameLength = wcsnlen(entry.szModule, std::size(entry.szModule));
					out.Value(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(entry.modBaseAddr)));
					out.Value(static_cast<uint32_t>(entry.modBaseSize));
					out.Value(timeDateStamp);
					out.Value(static_cast<uint16_t>(nameLength));
					out.Raw(entry.szModule, nameLength * sizeof(wchar_t));
					++count;
				}
				CloseHandle(snapshot);
			}

			memcpy(out.Bytes.data() + countAt, &count, sizeof(count));
			out.End(chunk);
		}

		void WriteGame(ChunkWriter& out)
		{
			StatisticsGameRecord game {};
			game.EndUnixTime = static_cast<uint64_t>(time(nullptr));
			game.EndFrame = static_cast<int32_t>(Unsorted::CurrentFrame);
			game.OutOfSyncFrame = State.OutOfSyncFrame;
			game.OutOfSync = *reinterpret_cast<const bool*>(OutOfSyncAddress) ? 1 : 0;
			game.SawCompletion = *reinterpret_cast<const bool*>(SawCompletionAddress) ? 1 : 0;

			const size_t chunk = out.Begin(StatisticsChunk_Game);
			out.Value(game);
			out.End(chunk);
		}
	}

	void Reset()
	{
		State = {};
	}

	void OnFrame(int frame)
	{
		if (State.OutOfSyncFrame < 0 && *reinterpret_cast<const bool*>(OutOfSyncAddress))
			State.OutOfSyncFrame = frame;
	}

	void FillHouseStats(std::vector<HouseStatsSample>& samples)
	{
		samples.clear();

		if (State.Types.empty())
			CaptureTypes();

		ComputeValues();
		RefreshSnapshot();

		for (int i = 0; i < HouseClass::Array.Count; ++i)
		{
			HouseClass* pHouse = HouseClass::Array.Items[i];
			if (!IsTrackedHouse(pHouse))
				continue;

			if (samples.size() >= static_cast<size_t>(MaxHouseStatsPerFrame))
				break;

			const int house = pHouse->ArrayIndex;
			HouseStatsSample sample {};
			sample.HouseIndex = house;
			sample.Credits = pHouse->Balance;
			sample.StoredOreValue = pHouse->OwnedTiberium.GetTotalValue();
			sample.CreditsSpent = pHouse->CreditsSpent;
			sample.HarvestedCredits = pHouse->HarvestedCredits;
			sample.PowerOutput = pHouse->PowerOutput;
			sample.PowerDrain = pHouse->PowerDrain;
			sample.Units = pHouse->OwnedUnits;
			sample.Infantry = pHouse->OwnedInfantry;
			sample.Aircraft = pHouse->OwnedAircraft;
			sample.Buildings = pHouse->OwnedBuildings;
			if (house >= 0 && static_cast<size_t>(house) < State.ArmyValue.size())
			{
				sample.ArmyValue = State.ArmyValue[static_cast<size_t>(house)];
				sample.BuildingValue = State.BuildingValue[static_cast<size_t>(house)];
			}
			for (size_t victim = 0; victim < std::size(pHouse->KilledUnitsOfHouses); ++victim)
			{
				sample.UnitsKilled += pHouse->KilledUnitsOfHouses[victim];
				sample.BuildingsKilled += pHouse->KilledBuildingsOfHouses[victim];
			}
			sample.UnitsLost = pHouse->TotalKilledUnits;
			sample.BuildingsLost = pHouse->TotalKilledBuildings;
			if (const auto* pSnapshot = FindSnapshot(house))
			{
				sample.UnitsBuilt = pSnapshot->UnitsBuilt;
				sample.BuildingsBuilt = pSnapshot->BuildingsBuilt;
			}
			sample.Score = pHouse->PointTotal;
			sample.Flags = HouseFlags(pHouse);

			samples.push_back(sample);
		}
	}

	void WriteSection(File& file)
	{
		// The houses are still alive on every path that closes a recording; the check guards a
		// scenario that has already been torn down, which leaves the last sample's snapshot.
		if (HouseClass::Array.Count > 0)
			RefreshSnapshot();

		if (State.Types.empty())
			CaptureTypes();

		ChunkWriter out;
		WriteTypes(out);
		WriteHouses(out);
		WriteGame(out);
		WriteModules(out);

		if (!State.StatsPacket.empty())
		{
			const size_t chunk = out.Begin(StatisticsChunk_StatsPacket);
			out.Raw(State.StatsPacket.data(), State.StatsPacket.size());
			out.End(chunk);
		}

		if (!file.WriteStatisticsSection(out.Bytes))
			Debug::Log("[Replay] Could not append the statistics section.\n");
	}
}

void ReplaySystem::OnStatisticsPacketStarting()
{
	if (!ReplaySystem::IsRecordingActive())
		return;

	Statistics::RefreshSnapshot();
	Statistics::State.Frozen = true;
}

void ReplaySystem::RecordStatisticsPacket(const void* data, int length)
{
	if (!ReplaySystem::IsRecordingActive() || !data || length <= 0 || length > Statistics::MaxStatsPacketBytes)
		return;

	const auto* bytes = static_cast<const unsigned char*>(data);
	Statistics::State.StatsPacket.assign(bytes, bytes + length);
}

// Mirrors the condition both Record_The_Kill functions count UnitsLost/BuildingsLost on: a building
// only when it is not Insignificant and its OwnerCountryIndex (+0x53C) is set, and - in
// Record_The_Kill_Object, whose whole tally sits inside it - only for a type that scores.
void ReplaySystem::RecordObjectLost(TechnoClass* pTechno, bool scoringTypesOnly)
{
	if (!ReplaySystem::IsRecordingActive() || !pTechno || !pTechno->Owner || pTechno->Owner->ArrayIndex < 0)
		return;

	const auto* pType = pTechno->GetTechnoType();
	if (!pType || (scoringTypesOnly && pType->DontScore))
		return;

	int kind;
	switch (pTechno->WhatAmI())
	{
	case AbstractType::Aircraft:
		kind = Statistics::Lost_Aircraft;
		break;
	case AbstractType::Infantry:
		kind = Statistics::Lost_Infantry;
		break;
	case AbstractType::Unit:
		kind = Statistics::Lost_Units;
		break;
	case AbstractType::Building:
		if (pType->Insignificant || static_cast<BuildingClass*>(pTechno)->OwnerCountryIndex == static_cast<DWORD>(-1))
			return;
		kind = Statistics::Lost_Buildings;
		break;
	default:
		return;
	}

	const int index = pType->GetArrayIndex();
	if (index < 0 || index >= 0x1000)
		return;

	auto& lost = Statistics::CountersFor(pTechno->Owner->ArrayIndex).Lost[static_cast<size_t>(kind)];
	if (static_cast<size_t>(index) >= lost.size())
		lost.resize(static_cast<size_t>(index) + 1, 0);
	++lost[static_cast<size_t>(index)];
}
