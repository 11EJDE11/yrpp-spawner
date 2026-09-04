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

#include "ReplaySeek.h"
#include "ReplayControls.h"
#include "ReplayOverlay.h"
#include "ReplaySystem.h"
#include "ReplaySystem.Internal.h"

#include <Spawner/Spawner.h>
#include <Utilities/Debug.h>

#include <AnimClass.h>
#include <BulletClass.h>
#include <FactoryClass.h>
#include <ParticleClass.h>
#include <ParticleSystemClass.h>
#include <ParticleTypeClass.h>
#include <RadSiteClass.h>
#include <SmudgeClass.h>
#include <SuperClass.h>
#include <TagClass.h>
#include <TeamClass.h>
#include <TerrainClass.h>
#include <TriggerClass.h>
#include <WaveClass.h>
#include <AircraftClass.h>
#include <AStarClass.h>
#include <BuildingClass.h>
#include <EventClass.h>
#include <GameModeOptionsClass.h>
#include <GameOptionsClass.h>
#include <FootClass.h>
#include <HoverLocomotionClass.h>
#include <HouseClass.h>
#include <InfantryClass.h>
#include <Kamikaze.h>
#include <RocketLocomotionClass.h>
#include <LoadOptionsClass.h>
#include <MapClass.h>
#include <Memory.h>
#include <PlanningTokenClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <SlaveManagerClass.h>
#include <SpawnManagerClass.h>
#include <PriorityQueueClass.h>
#include <TechnoClass.h>
#include <TiberiumClass.h>
#include <Surface.h>
#include <TacticalClass.h>
#include <TeleportLocomotionClass.h>
#include <TunnelLocomotionClass.h>
#include <Randomizer.h>
#include <Unsorted.h>
#include <UnitClass.h>
#include <VocClass.h>

#include <windows.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace ReplaySystem::Internal;

namespace ReplaySystem
{
	namespace Seek
	{
		namespace
		{
			constexpr const char* KeyframeSubdirectory = "Replay Keyframes";

			constexpr int SeekRenderInterval = 60;

			// A backwards seek always has somewhere to land, because playback drops this one as it
			// starts. Frame 0 is the state before the first frame ran.
			constexpr int FirstKeyframeFrame = 0;

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

			bool CapturePlanningState(PlanningSnapshot& snapshot);
			bool RestorePlanningState(const PlanningSnapshot& snapshot, int keyframeFrame);

			// Preserve the Kamikaze update timer across keyframe loads.
			struct KamikazeSnapshot
			{
				int32_t TimerStart = -1;
				int32_t TimerLeft = 0;
			};

			void CaptureKamikazeState(KamikazeSnapshot& snapshot);
			void RestoreKamikazeState(const KamikazeSnapshot& snapshot);

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

			bool CaptureAresParticleState(AresParticleSnapshot& snapshot);
			bool RestoreAresParticleState(const AresParticleSnapshot& snapshot, int keyframeFrame);

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

			void CaptureLocomotorResetStates(std::vector<LocomotorResetSnapshot>& out);
			bool RestoreLocomotorResetStates(const std::vector<LocomotorResetSnapshot>& snapshots,
				int keyframeFrame);

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

			bool CaptureTiberiumState(TiberiumSnapshot& snapshot);
			bool RestoreTiberiumState(const TiberiumSnapshot& snapshot, int keyframeFrame);

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

			// One place the ordered collections are listed, so capturing and restoring them cannot drift
			// apart and adding another is a single line.
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

			struct Keyframe
			{
				int Frame = 0;
				uint64_t FileBytes = 0;
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

			struct SeekState
			{
				bool StoreReady = false;
				int Interval = 0;
				uint64_t StorageLimitBytes = 0;
				uint64_t KeyframeBytes = 0;
				std::vector<Keyframe> Keyframes;

				bool Seeking = false;
				int TargetFrame = -1;
				// Loaded at the start of the next frame.
				bool LoadPending = false;
				int PendingLoadFrame = -1;
				bool LoadInProgress = false;

				int FramesSinceRender = 0;
				// What playback was doing before the seek, restored when it lands.
				bool ResumePaused = false;
				bool VocAllowedBeforeSeek = true;
			};

			SeekState State;


			std::filesystem::path KeyframeDirectory()
			{
				const auto* pConfig = GetConfig();
				const char* const savedGameDir = pConfig ? pConfig->SavedGameDir : "Saved Games";

				return std::filesystem::path(savedGameDir) / KeyframeSubdirectory;
			}

			// Relative to SavedGameDir, which is what the savegame path hooks expect.
			void FormatKeyframeName(char* buffer, size_t bufferSize, int frame)
			{
				sprintf_s(buffer, bufferSize, "%s\\rk%08d.sav", KeyframeSubdirectory, frame);
			}

			std::filesystem::path KeyframePath(int frame)
			{
				char name[32] = { 0 };
				sprintf_s(name, "rk%08d.sav", frame);
				return KeyframeDirectory() / name;
			}

			void RemoveKeyframeFiles()
			{
				std::error_code error {};
				std::filesystem::remove_all(KeyframeDirectory(), error);
			}

			bool EnsureKeyframeDirectory()
			{
				std::error_code error {};
				const auto directory = KeyframeDirectory();

				if (std::filesystem::exists(directory, error))
					return true;

				if (!std::filesystem::create_directories(directory, error))
				{
					Debug::Log("[Replay] Could not create the keyframe folder %s; seeking backwards "
						"will not be available.\n", directory.string().c_str());
					return false;
				}

				return true;
			}

			const Keyframe* NewestKeyframeAtOrBefore(int frame)
			{
				const Keyframe* best = nullptr;
				for (const auto& keyframe : State.Keyframes)
				{
					if (keyframe.Frame <= frame && (!best || keyframe.Frame > best->Frame))
						best = &keyframe;
				}

				return best;
			}

			bool HaveKeyframe(int frame)
			{
				return std::find_if(State.Keyframes.begin(), State.Keyframes.end(),
					[frame](const Keyframe& keyframe) { return keyframe.Frame == frame; })
					!= State.Keyframes.end();
			}

			const Keyframe* FindKeyframe(int frame)
			{
				const auto found = std::find_if(State.Keyframes.begin(), State.Keyframes.end(),
					[frame](const Keyframe& keyframe) { return keyframe.Frame == frame; });
				return found == State.Keyframes.end() ? nullptr : &*found;
			}

			void EvictOldKeyframes()
			{
				while (State.StorageLimitBytes > 0
					&& State.KeyframeBytes > State.StorageLimitBytes
					&& State.Keyframes.size() > 1)
				{
					const auto oldest = std::min_element(State.Keyframes.begin(), State.Keyframes.end(),
						[](const Keyframe& lhs, const Keyframe& rhs) { return lhs.Frame < rhs.Frame; });

					std::error_code error {};
					std::filesystem::remove(KeyframePath(oldest->Frame), error);
					if (error)
					{
						Debug::Log("[Replay] Could not remove keyframe %d while enforcing the storage limit.\n",
							oldest->Frame);
						break;
					}

					State.KeyframeBytes -= std::min(State.KeyframeBytes, oldest->FileBytes);
					State.Keyframes.erase(oldest);
				}
			}
			uint32_t UniqueIDOf(const AbstractClass* pObject)
			{
				return pObject ? static_cast<uint32_t>(pObject->UniqueID) : 0u;
			}

			#pragma region Ares particle-system state

			struct AresVectorView
			{
				unsigned char* Begin;
				unsigned char* End;
				unsigned char* Capacity;
			};

			struct AresParticleExtView
			{
				ParticleSystemClass* Owner;
				int Initialized;
				int Behave;
				ParticleTypeClass* HeldParticleType;
				AresVectorView MovementData;
				AresVectorView DrawData;
			};

			static_assert(sizeof(AresVectorView) == 0x0C);
			static_assert(offsetof(AresParticleExtView, Behave) == 0x08);
			static_assert(offsetof(AresParticleExtView, HeldParticleType) == 0x0C);
			static_assert(offsetof(AresParticleExtView, MovementData) == 0x10);
			static_assert(offsetof(AresParticleExtView, DrawData) == 0x1C);

			constexpr size_t AresDrawLinkedParticleTypeOffset = 0x24;
			constexpr size_t MaximumAresParticleRecords = 1u << 20;

			struct AresParticleApi
			{
				using FindExtension = AresParticleExtView* (__thiscall*)(void*, ParticleSystemClass*);
				using AllocateRecords = unsigned char* (__stdcall*)(unsigned int);
				using AdoptRecords = void (__thiscall*)(AresVectorView*, unsigned char*,
					unsigned int, unsigned int);

				unsigned char* Module = nullptr;
				void* ExtensionMap = nullptr;
				FindExtension Find = nullptr;
				AllocateRecords Allocate = nullptr;
				AdoptRecords Adopt = nullptr;
				bool Compatible = false;
			};

			bool BytesMatch(const unsigned char* address, const unsigned char* expected, size_t count)
			{
				return address && memcmp(address, expected, count) == 0;
			}

			const AresParticleApi& GetAresParticleApi()
			{
				static const AresParticleApi api = []()
				{
					AresParticleApi result {};
					result.Module = reinterpret_cast<unsigned char*>(GetModuleHandleA("Ares.dll"));
					if (!result.Module)
					{
						Debug::Log("[Replay] Ares is not loaded; its particle state will not travel with a "
							"keyframe.\n");
						return result;
					}

					const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(result.Module);
					if (dos->e_magic != IMAGE_DOS_SIGNATURE)
						return result;
					const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
						result.Module + dos->e_lfanew);
					if (nt->Signature != IMAGE_NT_SIGNATURE
						|| nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386
						|| nt->OptionalHeader.SizeOfImage <= 0xC2B84)
						return result;

					// ExtContainer::Find, vector<44-byte-record>::allocate and its
					// _Change_array/adopt helper in the exact reversed release build.
					const unsigned char findSignature[] = {
						0x83, 0xEC, 0x08, 0x53, 0x8B, 0x5C, 0x24, 0x10,
						0x0F, 0xB6, 0xC3, 0x35, 0xC5, 0x9D, 0x1C, 0x81
					};
					const unsigned char allocateSignature[] = {
						0x8B, 0x44, 0x24, 0x04, 0x3D, 0x5D, 0x74, 0xD1,
						0x05, 0x77, 0x41, 0x6B, 0xC0, 0x2C
					};
					const unsigned char adoptSignature[] = {
						0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x37, 0x85, 0xF6
					};

					auto* const find = result.Module + 0x58900;
					auto* const allocate = result.Module + 0x29730;
					auto* const adopt = result.Module + 0x296B0;
					const bool findOk = BytesMatch(find, findSignature, sizeof(findSignature));
					const bool allocateOk = BytesMatch(allocate, allocateSignature, sizeof(allocateSignature));
					const bool adoptOk = BytesMatch(adopt, adoptSignature, sizeof(adoptSignature));
					if (!findOk || !allocateOk || !adoptOk)
					{
						Debug::Log("[Replay] This Ares build does not match the one the particle state was "
							"reversed from (Find %s, allocate %s, adopt %s); its particle state will not "
							"travel with a keyframe.\n", findOk ? "matches" : "does not match",
							allocateOk ? "matches" : "does not match", adoptOk ? "matches" : "does not match");
						return result;
					}

					result.ExtensionMap = result.Module + 0xC2B84;
					result.Find = reinterpret_cast<AresParticleApi::FindExtension>(find);
					result.Allocate = reinterpret_cast<AresParticleApi::AllocateRecords>(allocate);
					result.Adopt = reinterpret_cast<AresParticleApi::AdoptRecords>(adopt);
					result.Compatible = true;
					return result;
				}();
				return api;
			}

			bool AresVectorCount(const AresVectorView& vector, size_t& count, size_t& capacity)
			{
				count = 0;
				capacity = 0;
				if (!vector.Begin && !vector.End && !vector.Capacity)
					return true;
				if (!vector.Begin || !vector.End || !vector.Capacity)
					return false;

				const uintptr_t begin = reinterpret_cast<uintptr_t>(vector.Begin);
				const uintptr_t end = reinterpret_cast<uintptr_t>(vector.End);
				const uintptr_t cap = reinterpret_cast<uintptr_t>(vector.Capacity);
				if (end < begin || cap < end
					|| (end - begin) % AresParticleRecordSize
					|| (cap - begin) % AresParticleRecordSize)
					return false;

				count = (end - begin) / AresParticleRecordSize;
				capacity = (cap - begin) / AresParticleRecordSize;
				return count <= MaximumAresParticleRecords
					&& capacity <= MaximumAresParticleRecords;
			}

			int ParticleTypeIndex(const ParticleTypeClass* pType)
			{
				if (!pType)
					return -1;
				for (int i = 0; i < ParticleTypeClass::Array.Count; ++i)
				{
					if (ParticleTypeClass::Array.Items[i] == pType)
						return i;
				}
				return -2;
			}

			ParticleTypeClass* ParticleTypeAt(int index)
			{
				if (index < 0)
					return nullptr;
				return index < ParticleTypeClass::Array.Count
					? ParticleTypeClass::Array.Items[index]
					: nullptr;
			}

			bool CopyAresVector(const AresVectorView& source,
				std::vector<std::array<unsigned char, AresParticleRecordSize>>& destination)
			{
				size_t count = 0;
				size_t capacity = 0;
				if (!AresVectorCount(source, count, capacity))
					return false;

				destination.resize(count);
				if (count)
					memcpy(destination.data(), source.Begin, count * AresParticleRecordSize);
				return true;
			}

			bool RestoreAresVector(const AresParticleApi& api, AresVectorView& destination,
				const unsigned char* source, size_t count)
			{
				if (count > MaximumAresParticleRecords)
					return false;

				size_t oldCount = 0;
				size_t capacity = 0;
				if (!AresVectorCount(destination, oldCount, capacity))
					return false;

				if (count <= capacity)
				{
					if (count)
						memcpy(destination.Begin, source, count * AresParticleRecordSize);
					destination.End = count
						? destination.Begin + count * AresParticleRecordSize
						: destination.Begin;
					return true;
				}

				auto* const memory = api.Allocate(static_cast<unsigned int>(count));
				if (!memory)
					return count == 0;
				memcpy(memory, source, count * AresParticleRecordSize);
				api.Adopt(&destination, memory, static_cast<unsigned int>(count),
					static_cast<unsigned int>(count));
				return true;
			}

			bool CaptureAresParticleState(AresParticleSnapshot& snapshot)
			{
				snapshot = {};
				const auto& api = GetAresParticleApi();
				if (!api.Compatible)
					return true;

				snapshot.Captured = true;
				snapshot.Systems.reserve(static_cast<size_t>(
					std::max(ParticleSystemClass::Array.Count, 0)));

				for (int i = 0; i < ParticleSystemClass::Array.Count; ++i)
				{
					auto* const pSystem = ParticleSystemClass::Array.Items[i];
					if (!pSystem)
						continue;
					auto* const pExt = api.Find(api.ExtensionMap, pSystem);
					if (!pExt || pExt->Owner != pSystem)
						return false;

					AresParticleSystemSnapshot saved {};
					saved.OwnerId = UniqueIDOf(pSystem);
					saved.Behave = pExt->Behave;
					saved.HeldParticleTypeIndex = ParticleTypeIndex(pExt->HeldParticleType);
					if (saved.HeldParticleTypeIndex == -2
						|| !CopyAresVector(pExt->MovementData, saved.MovementData))
						return false;

					std::vector<std::array<unsigned char, AresParticleRecordSize>> draw;
					if (!CopyAresVector(pExt->DrawData, draw))
						return false;
					saved.DrawData.reserve(draw.size());
					for (auto& bytes : draw)
					{
						AresParticleRecordSnapshot item {};
						item.Bytes = bytes;
						ParticleTypeClass* pType = nullptr;
						memcpy(&pType, item.Bytes.data() + AresDrawLinkedParticleTypeOffset,
							sizeof(pType));
						item.LinkedParticleTypeIndex = ParticleTypeIndex(pType);
						if (item.LinkedParticleTypeIndex == -2)
							return false;
						memset(item.Bytes.data() + AresDrawLinkedParticleTypeOffset, 0,
							sizeof(pType));
						saved.DrawData.push_back(std::move(item));
					}
					snapshot.Systems.push_back(std::move(saved));
				}
				return true;
			}

			bool RestoreAresParticleState(const AresParticleSnapshot& snapshot, int keyframeFrame)
			{
			// This used to fail silently in a dozen places, so a run where it restored nothing looked
			// exactly like a run where it had nothing to do. Every way out now says which it was.
			auto Give_Up = [keyframeFrame](const char* why)
			{
				Debug::Log("[Replay] Keyframe %d could not restore the Ares particle state: %s.\n",
					keyframeFrame, why);
				return false;
			};

				if (!snapshot.Captured)
				{
					Debug::Log("[Replay] Keyframe %d holds no Ares particle state to restore.\n",
						keyframeFrame);
					return true;
				}
				const auto& api = GetAresParticleApi();
				if (!api.Compatible)
					return Give_Up("the Ares build does not match the one this was reversed from");

				std::unordered_map<uint32_t, ParticleSystemClass*> systems;
				systems.reserve(static_cast<size_t>(std::max(ParticleSystemClass::Array.Count, 0)));
				for (int i = 0; i < ParticleSystemClass::Array.Count; ++i)
				{
					auto* const pSystem = ParticleSystemClass::Array.Items[i];
					if (pSystem)
						systems.emplace(UniqueIDOf(pSystem), pSystem);
				}
				if (systems.size() != snapshot.Systems.size())
					{
						Debug::Log("[Replay] Keyframe %d holds %u particle systems but the load left %u.\n",
							keyframeFrame, static_cast<unsigned int>(snapshot.Systems.size()),
							static_cast<unsigned int>(systems.size()));
						return Give_Up("the number of particle systems changed");
					}

				int changedSystems = 0;
				size_t movementRecords = 0;
				size_t drawRecords = 0;
				for (const auto& saved : snapshot.Systems)
				{
					const auto found = systems.find(saved.OwnerId);
					if (found == systems.end())
						return Give_Up("a particle system is missing after the load");
					auto* const pExt = api.Find(api.ExtensionMap, found->second);
					if (!pExt || pExt->Owner != found->second)
						return Give_Up("a particle system has no Ares extension, or one belonging to something else");

					size_t liveMovement = 0;
					size_t movementCapacity = 0;
					size_t liveDraw = 0;
					size_t drawCapacity = 0;
					if (!AresVectorCount(pExt->MovementData, liveMovement, movementCapacity)
						|| !AresVectorCount(pExt->DrawData, liveDraw, drawCapacity))
						return Give_Up("an Ares particle vector does not look like a vector");

					const bool changed = pExt->Behave != saved.Behave
						|| ParticleTypeIndex(pExt->HeldParticleType) != saved.HeldParticleTypeIndex
						|| liveMovement != saved.MovementData.size()
						|| liveDraw != saved.DrawData.size();

					pExt->Behave = saved.Behave;
					pExt->HeldParticleType = ParticleTypeAt(saved.HeldParticleTypeIndex);
					if (saved.HeldParticleTypeIndex >= 0 && !pExt->HeldParticleType)
						return Give_Up("the held particle type is no longer in the array");

					if (!RestoreAresVector(api, pExt->MovementData,
						saved.MovementData.empty() ? nullptr : saved.MovementData.front().data(),
						saved.MovementData.size()))
						return Give_Up("the movement record vector could not be rebuilt");

					std::vector<std::array<unsigned char, AresParticleRecordSize>> draw;
					draw.reserve(saved.DrawData.size());
					for (const auto& item : saved.DrawData)
					{
						auto bytes = item.Bytes;
						auto* const pType = ParticleTypeAt(item.LinkedParticleTypeIndex);
						if (item.LinkedParticleTypeIndex >= 0 && !pType)
							return Give_Up("a draw record names a particle type no longer in the array");
						memcpy(bytes.data() + AresDrawLinkedParticleTypeOffset, &pType,
							sizeof(pType));
						draw.push_back(bytes);
					}
					if (!RestoreAresVector(api, pExt->DrawData,
						draw.empty() ? nullptr : draw.front().data(), draw.size()))
						return Give_Up("the draw record vector could not be rebuilt");

					changedSystems += changed ? 1 : 0;
					movementRecords += saved.MovementData.size();
					drawRecords += saved.DrawData.size();
				}

				AresParticleSnapshot rebuilt {};
				if (!CaptureAresParticleState(rebuilt) || rebuilt != snapshot)
				{
					Debug::Log("[Replay] Keyframe %d Ares particle state did not reproduce exactly "
						"after restoring it.\n", keyframeFrame);
					return Give_Up("the state read back differently from what was written");
				}

				if (changedSystems)
				{
					Debug::Log("[Replay] Keyframe %d restored Ares particle state for %d systems "
						"(%u movement records, %u draw records; %d systems differed after load).\n",
						keyframeFrame, static_cast<int>(snapshot.Systems.size()),
						static_cast<unsigned int>(movementRecords),
						static_cast<unsigned int>(drawRecords), changedSystems);
				}
				return true;
			}

			#pragma endregion Ares particle-system state

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

			#pragma region Kamikaze tracker

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

			void RestoreHouseRepairState(const Keyframe& keyframe)
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
				for (const auto& saved : keyframe.HouseRepairs)
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
						"after the load (first house %u).\n", keyframe.Frame, restored, first);
				}
			}

			void RestoreTechnoInPlayfieldState(const Keyframe& keyframe)
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
				for (const auto& saved : keyframe.Technos)
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
						"(first object %u).\n", keyframe.Frame, restored, first);
				}
			}

			#pragma region Cell passability

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

			void CaptureDerivedMapHashes(DerivedMapHashes& out)
			{
				out.ZonePassability = HashZonePassability();
				out.MovementZones = HashMovementZones();
				out.Present = true;
			}

			void VerifyDerivedMapHashes(const Keyframe& keyframe)
			{
				if (!keyframe.DerivedMap.Present)
					return;

				const uint32_t zones = HashZonePassability();
				const uint32_t movement = HashMovementZones();

				if (zones == keyframe.DerivedMap.ZonePassability
					&& movement == keyframe.DerivedMap.MovementZones)
				{
					return;
				}

				Debug::Log("[Replay] Keyframe %d came back from the load deriving the map differently: "
					"zone passability %s, movement zones %s. Both of these are rebuilt from the cells, so "
					"look at what the cells came back holding first.\n", keyframe.Frame,
					zones == keyframe.DerivedMap.ZonePassability ? "matches" : "DIFFERS",
					movement == keyframe.DerivedMap.MovementZones ? "matches" : "DIFFERS");
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

			void RestoreSlaveManagerState(const Keyframe& keyframe)
			{
				if (keyframe.SlaveManagers.empty())
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

				for (const auto& saved : keyframe.SlaveManagers)
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
								keyframe.Frame, saved.Owner,
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
							"controls; it had %d. Leaving them as the load built them.\n", keyframe.Frame,
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
									"state %d with timer %d/%d.\n", keyframe.Frame, saved.Owner, at,
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
						"them.\n", keyframe.Frame, static_cast<int>(keyframe.SlaveManagers.size()));
					return;
				}

				Debug::Log("[Replay] Keyframe %d put back %d of %d slave managers (%d controls); %d were "
					"not in the world and %d had a different number of controls.\n", keyframe.Frame,
					managersChanged, static_cast<int>(keyframe.SlaveManagers.size()), controlsChanged,
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

			void CaptureLoadResetTimerState(LoadResetTimerSnapshots& out)
			{
				CaptureLoadResetTimers(SpawnManagerClass::Array, out.SpawnManagers,
					[](const SpawnManagerClass* p) { return KeyOfManagerOwner(p->Owner); },
					TimersOfSpawnManager);
				CaptureLoadResetTimers(BulletClass::Array, out.Bullets,
					[](const BulletClass* p) { return UniqueIDOf(p); }, TimersOfBullet);
			}

			void RestoreLoadResetTimerState(const Keyframe& keyframe)
			{
				const int spawns = RestoreLoadResetTimers(SpawnManagerClass::Array,
					keyframe.LoadResetTimers.SpawnManagers,
					[](const SpawnManagerClass* p) { return KeyOfManagerOwner(p->Owner); },
					TimersOfSpawnManager);
				const int bullets = RestoreLoadResetTimers(BulletClass::Array,
					keyframe.LoadResetTimers.Bullets,
					[](const BulletClass* p) { return UniqueIDOf(p); }, TimersOfBullet);

				if (spawns == 0 && bullets == 0)
				{
					Debug::Log("[Replay] Keyframe %d found the %d spawn managers and %d bullets already "
						"holding the timers it recorded.\n", keyframe.Frame,
						static_cast<int>(keyframe.LoadResetTimers.SpawnManagers.size()),
						static_cast<int>(keyframe.LoadResetTimers.Bullets.size()));
					return;
				}

				Debug::Log("[Replay] Keyframe %d put back the frame timers the load had reset: %d of %d "
					"spawn managers, %d of %d bullets.\n", keyframe.Frame,
					spawns, static_cast<int>(keyframe.LoadResetTimers.SpawnManagers.size()),
					bullets, static_cast<int>(keyframe.LoadResetTimers.Bullets.size()));
			}

			#pragma endregion Frame timers the load resets

			bool CaptureKeyframeState(int frame, Keyframe& keyframe)
			{
				if (!ScenarioClass::Instance)
					return false;

				keyframe.Frame = frame;
				keyframe.ScenarioUniqueID = ScenarioClass::Instance->UniqueID;

				memcpy(keyframe.Random.data(), &ScenarioClass::Instance->Random, sizeof(Randomizer));

				CaptureTechnoSnapshots(keyframe.Technos);
				CaptureHouseRepairSnapshots(keyframe.HouseRepairs);
				if (!CapturePlanningState(keyframe.Planning))
					return false;
				if (!CaptureAresParticleState(keyframe.AresParticles))
					return false;
				CaptureLocomotorResetStates(keyframe.LocomotorResetStates);
				CaptureKamikazeState(keyframe.Kamikaze);
				CaptureTiberiumState(keyframe.Tiberium);
				CaptureLoadResetTimerState(keyframe.LoadResetTimers);
				CaptureSlaveManagerState(keyframe.SlaveManagers);
				CaptureCellPassability(keyframe.CellPassability);
				CaptureDerivedMapHashes(keyframe.DerivedMap);
				if (!CaptureCellSubzones(keyframe.CellSubzones))
					return false;
				CaptureSubzoneTracking(keyframe.SubzoneGraph);

				int orderIndex = 0;
				#define REPLAY_CAPTURE_ORDER(collection, name) \
					CaptureObjectOrder(collection, keyframe.Orders[orderIndex++]);
				REPLAY_FOR_EACH_ORDERED_COLLECTION(REPLAY_CAPTURE_ORDER)
				#undef REPLAY_CAPTURE_ORDER

				for (size_t layer = 0; layer < keyframe.LayerOrders.size(); ++layer)
					CaptureObjectOrder(MapClass::ObjectsInLayers[layer], keyframe.LayerOrders[layer]);

				return true;
			}

			bool RestoreKeyframeState(const Keyframe& keyframe)
			{
				auto& random = ScenarioClass::Instance->Random;
				const bool randomChanged = memcmp(&random, keyframe.Random.data(), sizeof(Randomizer)) != 0;
				memcpy(&random, keyframe.Random.data(), sizeof(Randomizer));

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
					restoreCollection(collection, keyframe.Orders[orderIndex++], name);
				REPLAY_FOR_EACH_ORDERED_COLLECTION(REPLAY_RESTORE_ORDER)
				#undef REPLAY_RESTORE_ORDER

				for (size_t layer = 0; layer < keyframe.LayerOrders.size(); ++layer)
				{
					char name[32] = { 0 };
					sprintf_s(name, "MapClass::Layer[%u]", static_cast<unsigned int>(layer));
					restoreCollection(MapClass::ObjectsInLayers[layer], keyframe.LayerOrders[layer], name);
				}

				if (!RestorePlanningState(keyframe.Planning, keyframe.Frame))
					return false;
				if (!RestoreAresParticleState(keyframe.AresParticles, keyframe.Frame))
					return false;
				if (!RestoreLocomotorResetStates(keyframe.LocomotorResetStates, keyframe.Frame))
					return false;
				RestoreKamikazeState(keyframe.Kamikaze);
				RestoreTiberiumState(keyframe.Tiberium, keyframe.Frame);
				RestoreCellPassability(keyframe.CellPassability, keyframe.Frame);
				VerifyDerivedMapHashes(keyframe);
				if (!RestoreCellSubzones(keyframe.CellSubzones, keyframe.Frame))
					return false;
				if (!RestoreSubzoneTracking(keyframe.SubzoneGraph, keyframe.Frame))
					return false;

				if (randomChanged || reorderedCollectionCount > 0)
				{
					Debug::Log("[Replay] Keyframe %d restored CRC state after loading "
						"(RNG changed: %s; reordered collections: %d).\n",
						keyframe.Frame, randomChanged ? "yes" : "no", reorderedCollectionCount);
				}

				return true;
			}

			bool WriteKeyframe(int frame)
			{
				if (!State.StoreReady || HaveKeyframe(frame))
					return false;

				char fileName[MAX_PATH] = { 0 };
				FormatKeyframeName(fileName, sizeof(fileName), frame);

				wchar_t description[64] = { 0 };
				swprintf_s(description, L"Replay keyframe %d", frame);

				if (!ScenarioClass::SaveGame(fileName, description))
				{
					Debug::Log("[Replay] Failed to write the keyframe for frame %d.\n", frame);
					return false;
				}

				Keyframe keyframe;
				if (!CaptureKeyframeState(frame, keyframe))
				{
					Debug::Log("[Replay] Could not capture post-save simulation state for keyframe %d.\n",
						frame);
					std::error_code error {};
					std::filesystem::remove(KeyframePath(frame), error);
					return false;
				}

				std::error_code sizeError {};
				keyframe.FileBytes = std::filesystem::file_size(KeyframePath(frame), sizeError);
				if (sizeError)
				{
					Debug::Log("[Replay] Could not measure keyframe %d for storage accounting.\n", frame);
					std::error_code removeError {};
					std::filesystem::remove(KeyframePath(frame), removeError);
					return false;
				}

				State.KeyframeBytes += keyframe.FileBytes;
				State.Keyframes.push_back(std::move(keyframe));
				EvictOldKeyframes();
				return true;
			}

			void ResumeInGameSessionAfterLoad()
			{
				SessionClass::Instance.Resume();
			}

			#pragma region Planning tokens

			std::array<DynamicVectorClass<PlanningNodeClass*>*, 3> PlanningManagerNodeLists()
			{
				return {
					&PlanningNodeClass::Unknown1,
					&PlanningNodeClass::Unknown2,
					&PlanningNodeClass::Unknown3
				};
			}

			bool CapturePlanningState(PlanningSnapshot& snapshot)
			{
				snapshot = {};

				const auto& pendingEvents = PlanningTokenClass::PendingEvents;
				snapshot.PendingEvents.reserve(static_cast<size_t>(std::max(pendingEvents.Count, 0)));
				for (int i = 0; i < pendingEvents.Count; ++i)
				{
					std::array<unsigned char, sizeof(EventClass)> packet {};
					if (pendingEvents.Items[i])
						memcpy(packet.data(), pendingEvents.Items[i], sizeof(EventClass));
					snapshot.PendingEvents.push_back(std::move(packet));
				}

				std::unordered_map<const PlanningNodeClass*, uint32_t> nodeIndices;
				std::vector<const PlanningNodeClass*> nodes;
				const auto indexNode = [&nodeIndices, &nodes](const PlanningNodeClass* pNode)
				{
					if (!pNode)
						return InvalidPlanningNode;

					const auto found = nodeIndices.find(pNode);
					if (found != nodeIndices.end())
						return found->second;

					const uint32_t index = static_cast<uint32_t>(nodes.size());
					nodes.push_back(pNode);
					nodeIndices.emplace(pNode, index);
					return index;
				};

				const auto managerLists = PlanningManagerNodeLists();
				for (size_t listIndex = 0; listIndex < managerLists.size(); ++listIndex)
				{
					const auto& list = *managerLists[listIndex];
					auto& savedList = snapshot.ManagerNodeLists[listIndex];
					savedList.reserve(static_cast<size_t>(std::max(list.Count, 0)));
					for (int i = 0; i < list.Count; ++i)
						savedList.push_back(indexNode(list.Items[i]));
				}

				snapshot.Tokens.reserve(static_cast<size_t>(std::max(PlanningTokenClass::Array.Count, 0)));
				for (int i = 0; i < PlanningTokenClass::Array.Count; ++i)
				{
					const auto* const pToken = PlanningTokenClass::Array.Items[i];
					if (!pToken)
						continue;

					PlanningTokenSnapshot token {};
					token.OwnerId = UniqueIDOf(pToken->OwnerUnit);
					token.Field1C = pToken->field_1C;
					memcpy(token.CurrentEvent.data(), &pToken->CurrentEvent, sizeof(EventClass));
					token.Field8C = pToken->field_8C;
					token.ClosedLoopNodeCount = pToken->ClosedLoopNodeCount;
					token.StepsToClosedLoop = pToken->StepsToClosedLoop;
					token.Field98 = pToken->field_98;
					token.Field99 = pToken->field_99;
					token.Nodes.reserve(static_cast<size_t>(std::max(pToken->PlanningNodes.Count, 0)));
					for (int n = 0; n < pToken->PlanningNodes.Count; ++n)
						token.Nodes.push_back(indexNode(pToken->PlanningNodes.Items[n]));

					snapshot.Tokens.push_back(std::move(token));
				}

				snapshot.Nodes.reserve(nodes.size());
				for (const auto* const pNode : nodes)
				{
					PlanningNodeSnapshot node {};
					node.Field18 = pNode->field_18;
					node.Field1C = pNode->field_1C;
					memcpy(node.Packet.data(), &pNode->Packet, sizeof(EventClass));
					node.FieldA8 = pNode->field_A8;
					node.FieldAC = pNode->field_AC;
					node.BranchNumber = pNode->BranchNumber;
					node.FieldB4 = pNode->field_B4;

					node.Members.reserve(static_cast<size_t>(std::max(pNode->PlanningMembers.Count, 0)));
					for (int m = 0; m < pNode->PlanningMembers.Count; ++m)
					{
						const auto* const pMember = pNode->PlanningMembers.Items[m];
						if (!pMember)
						{
							node.Members.push_back({});
							continue;
						}

						PlanningMemberSnapshot member {};
						member.Present = true;
						member.OwnerId = UniqueIDOf(pMember->Owner);
						if (pMember->Packet)
							memcpy(member.Packet.data(), pMember->Packet, sizeof(EventClass));
						member.Field8 = pMember->field_8;
						member.FieldC = pMember->field_C;
						node.Members.push_back(std::move(member));
					}

					node.Branches.reserve(static_cast<size_t>(std::max(pNode->PlanningBranches.Count, 0)));
					for (int b = 0; b < pNode->PlanningBranches.Count; ++b)
					{
						const auto* const pBranch = pNode->PlanningBranches.Items[b];
						if (!pBranch)
						{
							node.Branches.push_back({});
							continue;
						}

						PlanningBranchSnapshot branch {};
						branch.Present = true;
						memcpy(branch.Packet.data(), &pBranch->Packet, sizeof(EventClass));
						branch.MemberCount = pBranch->MemberCount;
						branch.MemberIndex = pBranch->MemberIndex;
						node.Branches.push_back(std::move(branch));
					}

					snapshot.Nodes.push_back(std::move(node));
				}

				const auto& activeOwners = PlanningTokenClass::ActiveRouteOwners;
				snapshot.ActiveRouteOwners.reserve(static_cast<size_t>(std::max(activeOwners.Count, 0)));
				for (int i = 0; i < activeOwners.Count; ++i)
					snapshot.ActiveRouteOwners.push_back(UniqueIDOf(activeOwners.Items[i]));

				memcpy(snapshot.HouseRouteCounts.data(), PlanningTokenClass::HouseRouteCounts,
					sizeof(snapshot.HouseRouteCounts));
				return true;
			}

			bool RestorePlanningState(const PlanningSnapshot& snapshot, int keyframeFrame)
			{
				std::unordered_map<uint32_t, TechnoClass*> technoById;
				technoById.reserve(static_cast<size_t>(std::max(TechnoClass::Array.Count, 0)));
				for (int i = 0; i < TechnoClass::Array.Count; ++i)
				{
					if (auto* const pTechno = TechnoClass::Array.Items[i])
						technoById.emplace(UniqueIDOf(pTechno), pTechno);
				}

				const auto findTechno = [&technoById](uint32_t id) -> TechnoClass*
				{
					const auto found = technoById.find(id);
					return found == technoById.end() ? nullptr : found->second;
				};

				const auto validNodeIndex = [&snapshot](uint32_t index)
				{
					return index == InvalidPlanningNode || index < snapshot.Nodes.size();
				};

				for (const auto& token : snapshot.Tokens)
				{
					if (!token.OwnerId || !findTechno(token.OwnerId))
					{
						Debug::Log("[Replay] Keyframe %d planning route owner %u is missing after load.\n",
							keyframeFrame, token.OwnerId);
						return false;
					}
					if (!std::all_of(token.Nodes.begin(), token.Nodes.end(), validNodeIndex))
						return false;
				}

				for (const auto& node : snapshot.Nodes)
				{
					for (const auto& member : node.Members)
					{
						if (!member.Present)
							continue;

						if (!member.OwnerId || !findTechno(member.OwnerId))
						{
							Debug::Log("[Replay] Keyframe %d planning member owner %u is missing after "
								"load.\n", keyframeFrame, member.OwnerId);
							return false;
						}
					}
				}

				for (const auto& list : snapshot.ManagerNodeLists)
				{
					if (!std::all_of(list.begin(), list.end(), validNodeIndex))
						return false;
				}
				for (uint32_t ownerId : snapshot.ActiveRouteOwners)
				{
					if (!ownerId || !findTechno(ownerId))
						return false;
				}

				auto& pendingEvents = PlanningTokenClass::PendingEvents;
				for (int i = 0; i < pendingEvents.Count; ++i)
				{
					if (pendingEvents.Items[i])
						YRMemory::Deallocate(pendingEvents.Items[i]);
				}
				pendingEvents.Count = 0;

				PlanningTokenClass::ClearAll();
				for (auto& pair : technoById)
					pair.second->PlanningToken = nullptr;


				std::vector<PlanningNodeClass*> nodes;
				nodes.reserve(snapshot.Nodes.size());
				size_t memberCount = 0;

				for (const auto& savedNode : snapshot.Nodes)
				{
					auto* const memory = static_cast<PlanningNodeClass*>(
						YRMemory::AllocateChecked(sizeof(PlanningNodeClass)));
					auto* const pNode = PlanningNodeClass::Construct(memory, savedNode.Field18);
					pNode->field_1C = savedNode.Field1C;
					memcpy(&pNode->Packet, savedNode.Packet.data(), sizeof(EventClass));
					pNode->field_A8 = savedNode.FieldA8;
					pNode->field_AC = savedNode.FieldAC;
					pNode->BranchNumber = savedNode.BranchNumber;
					pNode->field_B4 = savedNode.FieldB4;

					for (const auto& savedBranch : savedNode.Branches)
					{
						if (!savedBranch.Present)
						{
							if (!pNode->PlanningBranches.AddItem(nullptr))
								return false;
							continue;
						}

						auto* const pBranch = static_cast<PlanningBranchClass*>(
							YRMemory::AllocateChecked(sizeof(PlanningBranchClass)));
						memcpy(&pBranch->Packet, savedBranch.Packet.data(), sizeof(EventClass));
						pBranch->MemberCount = savedBranch.MemberCount;
						pBranch->MemberIndex = savedBranch.MemberIndex;
						if (!pNode->PlanningBranches.AddItem(pBranch))
							return false;
					}

					for (const auto& savedMember : savedNode.Members)
					{
						if (!savedMember.Present)
						{
							if (!pNode->PlanningMembers.AddItem(nullptr))
								return false;
							continue;
						}

						auto* const pMember = static_cast<PlanningMemberClass*>(
							YRMemory::AllocateChecked(sizeof(PlanningMemberClass)));
						pMember->Owner = findTechno(savedMember.OwnerId);
						pMember->Packet = static_cast<EventClass*>(
							YRMemory::AllocateChecked(sizeof(EventClass)));
						memcpy(pMember->Packet, savedMember.Packet.data(), sizeof(EventClass));
						pMember->field_8 = savedMember.Field8;
						pMember->field_C = savedMember.FieldC;
						if (!pNode->PlanningMembers.AddItem(pMember))
							return false;
						++memberCount;
					}

					nodes.push_back(pNode);
				}

				const auto nodeAt = [&nodes](uint32_t index) -> PlanningNodeClass*
				{
					return index == InvalidPlanningNode ? nullptr : nodes[index];
				};

				for (const auto& savedToken : snapshot.Tokens)
				{
					auto* const pOwner = findTechno(savedToken.OwnerId);
					auto* const memory = static_cast<PlanningTokenClass*>(
						YRMemory::AllocateChecked(sizeof(PlanningTokenClass)));
					auto* const pToken = PlanningTokenClass::Construct(memory, pOwner);

					pToken->field_1C = savedToken.Field1C;
					memcpy(&pToken->CurrentEvent, savedToken.CurrentEvent.data(), sizeof(EventClass));
					pToken->field_8C = savedToken.Field8C;
					pToken->ClosedLoopNodeCount = savedToken.ClosedLoopNodeCount;
					pToken->StepsToClosedLoop = savedToken.StepsToClosedLoop;
					pToken->field_98 = savedToken.Field98;
					pToken->field_99 = savedToken.Field99;
					for (uint32_t nodeIndex : savedToken.Nodes)
					{
						if (!pToken->PlanningNodes.AddItem(nodeAt(nodeIndex)))
							return false;
					}

					if (!PlanningTokenClass::Array.AddItem(pToken))
						return false;
					pOwner->PlanningToken = pToken;
				}

				const auto managerLists = PlanningManagerNodeLists();
				for (size_t listIndex = 0; listIndex < managerLists.size(); ++listIndex)
				{
					for (uint32_t nodeIndex : snapshot.ManagerNodeLists[listIndex])
					{
						if (!managerLists[listIndex]->AddItem(nodeAt(nodeIndex)))
							return false;
					}
				}

				auto& activeOwners = PlanningTokenClass::ActiveRouteOwners;
				for (uint32_t ownerId : snapshot.ActiveRouteOwners)
				{
					if (!activeOwners.AddItem(findTechno(ownerId)))
						return false;
				}
				memcpy(PlanningTokenClass::HouseRouteCounts, snapshot.HouseRouteCounts.data(),
					sizeof(snapshot.HouseRouteCounts));

				for (const auto& savedPacket : snapshot.PendingEvents)
				{
					auto* const pPacket = static_cast<EventClass*>(
						YRMemory::AllocateChecked(sizeof(EventClass)));
					memcpy(pPacket, savedPacket.data(), sizeof(EventClass));
					if (!pendingEvents.AddItem(pPacket))
					{
						YRMemory::Deallocate(pPacket);
						return false;
					}
				}

				PlanningSnapshot rebuilt {};
				if (!CapturePlanningState(rebuilt) || rebuilt != snapshot)
				{
					Debug::Log("[Replay] Keyframe %d planning graph did not reproduce exactly after "
						"rebuilding it.\n", keyframeFrame);
					return false;
				}

				if (!snapshot.Tokens.empty())
				{
					Debug::Log("[Replay] Keyframe %d rebuilt %d planning routes (%d shared nodes, %d "
						"members, %d pending events).\n", keyframeFrame,
						static_cast<int>(snapshot.Tokens.size()),
						static_cast<int>(snapshot.Nodes.size()), static_cast<int>(memberCount),
						static_cast<int>(snapshot.PendingEvents.size()));
				}
				return true;
			}

			#pragma endregion Planning tokens

			void RepointTempSurfaceAfterLoad()
			{
				DSurface::Temp = DSurface::Hidden;
			}

			void ClearTransientEventQueuesForLoad(const char* phase, int keyframeFrame)
			{
				const int outCount = EventClass::OutList.Count;
				const int doCount = EventClass::DoList.Count;
				const int megaMissionCount = EventClass::MegaMissionList.Count;

				if (outCount > 0 || doCount > 0 || megaMissionCount > 0)
				{
					Debug::Log("[Replay] Keyframe %d discarded transient event queues %s loading "
						"(out %d, do %d, deferred mega-missions %d).\n",
						keyframeFrame, phase, outCount, doCount, megaMissionCount);

					const int reportCount = std::min(doCount, 4);
					for (int i = 0; i < reportCount; ++i)
					{
						const auto& event = EventClass::DoList[i];
						Debug::Log("[Replay]   discarded DoList event type %d from frame %u "
							"(executed %s).\n", static_cast<int>(event.Type), event.Frame,
							(event.IsExecuted & 1) != 0 ? "yes" : "no");
					}
				}

				EventClass::OutList.Init();
				EventClass::DoList.Init();
				EventClass::MegaMissionList.Init();
				std::memset(EventClass::MegaMissionTargetNum, 0,
					sizeof(EventClass::MegaMissionTargetNum));
				std::memset(EventClass::MegaMissionTargets, 0,
					sizeof(EventClass::MegaMissionTargets));
			}

			bool RestorePlaybackAfterLoad(const Keyframe& keyframe)
			{
				const int keyframeFrame = keyframe.Frame;
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

				if (ScenarioClass::Instance->UniqueID != keyframe.ScenarioUniqueID)
				{
					Debug::Log("[Replay] Keyframe %d advanced the scenario unique ID from %d to %d "
						"while loading; restoring it.\n", keyframeFrame, keyframe.ScenarioUniqueID,
						ScenarioClass::Instance->UniqueID);
					ScenarioClass::Instance->UniqueID = keyframe.ScenarioUniqueID;
				}

				if (!RestoreKeyframeState(keyframe))
					return false;


				RepointTempSurfaceAfterLoad();
				ResumeInGameSessionAfterLoad();
				RestoreHouseRepairState(keyframe);
				RestoreLoadResetTimerState(keyframe);
				RestoreSlaveManagerState(keyframe);
				RestoreTechnoInPlayfieldState(keyframe);

				if (ReplayState.HasPlaybackHeader)
				{
					const int recordedGameSpeed = std::clamp(
						static_cast<int>(ReplayState.PlaybackHeader.RecordedGameSpeed), 0, MaxGameSpeedIndex);

					GameOptionsClass::Instance.GameSpeed = recordedGameSpeed;
					GameModeOptionsClass::Instance.GameSpeed = recordedGameSpeed;
				}

				ReplayState.DivergenceReported = false;

				// The panel is still on screen but the world under it has been swapped out.
				Overlay::CancelInteraction();

				return RepositionPlaybackStreamToFrame(keyframeFrame);
			}

			bool LoadKeyframe(const Keyframe& keyframe)
			{
				char fileName[MAX_PATH] = { 0 };
				FormatKeyframeName(fileName, sizeof(fileName), keyframe.Frame);

				const bool restoreViewerViewport = !ReplayState.LockViewport && TacticalClass::Instance;
				const Point2D viewerViewport = restoreViewerViewport
					? TacticalClass::Instance->TacticalCoord1
					: Point2D { 0, 0 };

				ClearTransientEventQueuesForLoad("before", keyframe.Frame);
				State.LoadInProgress = true;
				const bool loaded = LoadOptionsClass::LoadMission(fileName);
				State.LoadInProgress = false;

				if (!loaded)
				{
					Debug::Log("[Replay] Failed to load the keyframe for frame %d.\n", keyframe.Frame);
					return false;
				}

				ClearTransientEventQueuesForLoad("after", keyframe.Frame);
				if (!RestorePlaybackAfterLoad(keyframe))
					return false;

				Point2D viewportAfterLoad {};
				bool restoreViewport = false;
				if (restoreViewerViewport)
				{
					viewportAfterLoad = viewerViewport;
					restoreViewport = true;
				}
				else if (ReplayState.LockViewport)
				{
					const auto& pending = ReplayState.PendingPlaybackFrame;
					if (ReplayState.HasPendingPlaybackFrame
						&& pending.FrameNumber == keyframe.Frame
						&& (pending.Flags & FrameRecordFlag_TacticalPos) != 0u)
					{
						ReplayState.LockedViewportPos = pending.TacticalPos;
						ReplayState.HasLockedViewportPos = true;
					}

					if (ReplayState.HasLockedViewportPos)
					{
						viewportAfterLoad = ReplayState.LockedViewportPos;
						restoreViewport = true;
					}
				}

				if (restoreViewport && TacticalClass::Instance)
				{
					auto* const pTactical = TacticalClass::Instance;
					pTactical->TacticalCoord1 = viewportAfterLoad;
					pTactical->TacticalCoord2 = viewportAfterLoad;
					pTactical->RecalculateViewport();
					pTactical->Redrawing = true;
				}

				return true;
			}

			void EndSeek()
			{
				if (!State.Seeking)
					return;

				State.Seeking = false;
				State.TargetFrame = -1;
				State.FramesSinceRender = 0;

				VocClass::VoicesEnabled = State.VocAllowedBeforeSeek;

				// A seek out of a paused replay leaves it paused on the frame asked for, which is
				// what stepping and scrubbing both want.
				if (State.ResumePaused)
					Controls::SetPlaybackPaused(true);

				// The pacing deadline is stale by however long the seek took.
				ReplayState.PlaybackNextFrameDue = 0.0;
			}

			void BeginSeek(int targetFrame, bool pauseOnArrival)
			{
				if (!State.Seeking)
				{
					State.ResumePaused = Controls::IsPlaybackPaused();
					State.VocAllowedBeforeSeek = VocClass::VoicesEnabled;
				}

				State.ResumePaused = State.ResumePaused || pauseOnArrival;

				State.Seeking = true;
				State.TargetFrame = targetFrame;
				State.FramesSinceRender = 0;

				// Frames run back to back with no pacing while this holds, so the sound effects of
				// every one of them would land at once.
				VocClass::VoicesEnabled = false;
				Controls::SetPlaybackPaused(false);
			}
		}

		int KeyframeInterval()
		{
			return State.Interval;
		}

		bool IsLoadInProgress()
		{
			return State.LoadInProgress;
		}

		void OnPlaybackStarted()
		{
			State = SeekState {};

			const auto* pConfig = GetConfig();
			State.Interval = pConfig ? std::max(0, pConfig->ReplayKeyframeInterval) : 0;
			const int storageLimitMB = pConfig
				? std::max(0, pConfig->ReplayKeyframeStorageLimitMB)
				: 512;
			State.StorageLimitBytes = static_cast<uint64_t>(storageLimitMB) * 1024u * 1024u;

			if (State.Interval <= 0)
			{
				Debug::Log("[Replay] Keyframes are off; seeking backwards is not available.\n");
				return;
			}

			// A previous playback that died without cleaning up would otherwise leave its keyframes
			// to be mistaken for this one's.
			RemoveKeyframeFiles();
			State.StoreReady = EnsureKeyframeDirectory();
		}

		void OnPlaybackStopped()
		{
			if (State.StoreReady)
				RemoveKeyframeFiles();

			if (State.Seeking)
				VocClass::VoicesEnabled = State.VocAllowedBeforeSeek;

			State = SeekState {};
		}


		void ServiceFrameStart()
		{
			if (!ReplayState.Playback)
				return;

			if (State.LoadPending)
			{
				const int keyframeFrame = State.PendingLoadFrame;
				State.LoadPending = false;
				State.PendingLoadFrame = -1;

				const Keyframe* const keyframe = FindKeyframe(keyframeFrame);
				if (!keyframe || !LoadKeyframe(*keyframe))
				{
					Debug::Log("[Replay] Seek failed; stopping playback.\n");
					EndSeek();
					StopReplaySystem();
					return;
				}
			}

			if (State.Seeking && static_cast<int>(Unsorted::CurrentFrame) >= State.TargetFrame)
				EndSeek();

			if (ReplayState.PlaybackStreamEnded && State.Seeking)
				EndSeek();


			if (State.Interval <= 0 || !State.StoreReady)
				return;

			const int frame = static_cast<int>(Unsorted::CurrentFrame);
			if (frame == FirstKeyframeFrame || (frame > 0 && frame % State.Interval == 0))
				WriteKeyframe(frame);
		}

		bool RequestSeek(int targetFrame, bool pauseOnArrival)
		{
			if (!ReplayState.Playback)
				return false;

			const int currentFrame = static_cast<int>(Unsorted::CurrentFrame);
			targetFrame = std::max(0, targetFrame);

			if (targetFrame == currentFrame)
			{
				if (pauseOnArrival)
					Controls::SetPlaybackPaused(true);

				return true;
			}

			const Keyframe* const keyframe = NewestKeyframeAtOrBefore(targetFrame);
			const bool mustGoBack = targetFrame < currentFrame;
			const bool keyframeSkipsAhead = keyframe && keyframe->Frame > currentFrame;

			if (mustGoBack && !keyframe)
			{
				Debug::Log("[Replay] No keyframe at or before frame %d; cannot seek back there.\n",
					targetFrame);
				return false;
			}

			BeginSeek(targetFrame, pauseOnArrival);

			if (mustGoBack || keyframeSkipsAhead)
			{
				State.LoadPending = true;
				State.PendingLoadFrame = keyframe->Frame;
			}

			return true;
		}

		bool IsSeeking()
		{
			return State.Seeking;
		}

		bool ShouldSkipRenderThisFrame()
		{
			if (!State.Seeking)
				return false;


			return State.FramesSinceRender < SeekRenderInterval;
		}

		void CountRenderedFrame()
		{
			if (!State.Seeking)
				return;

			if (State.FramesSinceRender < SeekRenderInterval)
				++State.FramesSinceRender;
			else
				State.FramesSinceRender = 0;
		}

		int EarliestSeekableFrame()
		{
			if (State.Keyframes.empty())
				return static_cast<int>(Unsorted::CurrentFrame);

			return std::min_element(State.Keyframes.begin(), State.Keyframes.end(),
				[](const Keyframe& lhs, const Keyframe& rhs) { return lhs.Frame < rhs.Frame; })->Frame;
		}

		int CollectKeyframeFrames(int* outFrames, int maxFrames)
		{
			if (!outFrames || maxFrames <= 0)
				return 0;

			const int count = std::min(maxFrames, static_cast<int>(State.Keyframes.size()));
			for (int i = 0; i < count; ++i)
				outFrames[i] = State.Keyframes[static_cast<size_t>(i)].Frame;

			std::sort(outFrames, outFrames + count);
			return count;
		}

		int CurrentFrame()
		{
			return static_cast<int>(Unsorted::CurrentFrame);
		}

		int TotalFrames()
		{
			const int played = std::max(ReplayState.HighestPlayedFrame, CurrentFrame());

			// A recording that died with the process never got its length stamped in, so the only
			// honest answer is how far playback has got.
			const int recorded = ReplayState.HasPlaybackHeader
				? static_cast<int>(ReplayState.PlaybackHeader.TotalFrames)
				: 0;

			return std::max(recorded, played);
		}

		int RecordedFPS()
		{
			if (!ReplayState.HasPlaybackHeader)
				return GetReplayFPSFromGameSpeed(0);

			return GetReplayFPSFromGameSpeed(
				static_cast<int>(ReplayState.PlaybackHeader.RecordedGameSpeed));
		}
	}
}
