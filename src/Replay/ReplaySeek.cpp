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
#include <GameModeOptionsClass.h>
#include <GameOptionsClass.h>
#include <FootClass.h>
#include <HoverLocomotionClass.h>
#include <HouseClass.h>
#include <InfantryClass.h>
#include <RocketLocomotionClass.h>
#include <LoadOptionsClass.h>
#include <MapClass.h>
#include <Memory.h>
#include <PlanningTokenClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <PriorityQueueClass.h>
#include <TechnoClass.h>
#include <TiberiumClass.h>
#include <Surface.h>
#include <TeleportLocomotionClass.h>
#include <TunnelLocomotionClass.h>
#include <Randomizer.h>
#include <Unsorted.h>
#include <UnitClass.h>

#include <windows.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
			// Keyframes are savegames, and the spawner rewrites every savegame path to sit under
			// SavedGameDir (see SavedGamesInSubdir.cpp), so these names are relative to that and the
			// folder below it has to exist before the first save.
			constexpr const char* KeyframeSubdirectory = "Replay Keyframes";

			// A seek still draws every so many frames, so a long one reads as progress rather than
			// as a hang. Everything between is simulated without being drawn, which is most of the
			// cost of a frame.
			constexpr int SeekRenderInterval = 60;

			// A backwards seek always has somewhere to land, because playback drops this one as it
			// starts. Frame 0 is the state before the first frame ran.
			constexpr int FirstKeyframeFrame = 0;

			// What a techno was doing when the keyframe was taken. Nothing here is restored - it is
			// checked after the load, to catch per-object state that the savegame did not carry across.
			// Both branches the divergence trace pointed at turn on the target: a guarding building only
			// draws its idle jitter while it has none, and Can_Opportunity_Fire only lets a techno go
			// looking for one while it has none. Target is a pointer, so it is saved as an ID and
			// re-resolved on load, which is exactly the kind of thing that can come back different.
			struct TechnoSnapshot
			{
				uint32_t Id;
				uint32_t TargetId;
				uint32_t ArchiveTargetId;
				uint32_t DestinationId;
				int32_t Mission;
				int32_t MissionStartTime;
				// The gate on the call the divergence trace caught. Started is an absolute frame and the
				// timer fires when Frame - Started >= TimeLeft, so both halves have to come back.
				int32_t TargetingStart;
				int32_t TargetingLeft;
				// MissionClass::AI runs an object's mission function only when this timer expires, and then
				// restarts it from the value the mission function returned. It is the schedule the whole of
				// an object's behaviour hangs off: shift it and the object still does all the same things,
				// just on different frames, which is exactly the shape of what the divergence trace found.
				int32_t MissionTimerStart;
				int32_t MissionTimerLeft;
				int32_t MissionStatus;
				int32_t MissionAccumulate;
				// The queued movement destinations behind the first one - what a player builds up by
				// shift-clicking a route. FootClass::Mission_Guard_Area reads the count and the head of
				// this queue and can hand the object a new destination off the back of it, on a path that
				// draws no randomness at all. So a queue that does not survive the load shifts what the
				// object does without moving the randomiser, which is why nothing noticed for hundreds of
				// frames.
				int32_t NavQueueCount;
				uint32_t NavQueueHeadId;
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
				// Commands already taken from a token but not yet handed to the network event
				// queue. While one of these exists the token's field_1C is set and it cannot
				// advance to the next route node. The game keeps this pointer vector outside
				// the savegame object graph, so it has to travel with the keyframe too.
				std::vector<std::array<unsigned char, sizeof(EventClass)>> PendingEvents;
				std::array<std::vector<uint32_t>, 3> ManagerNodeLists;
				std::vector<uint32_t> ActiveRouteOwners;
				std::array<int, 24> HouseRouteCounts {};
				bool operator==(const PlanningSnapshot&) const = default;
			};

			bool CapturePlanningState(PlanningSnapshot& snapshot);
			bool RestorePlanningState(const PlanningSnapshot& snapshot, int keyframeFrame);

			// Several vanilla locomotor Load methods read the complete saved object and then run a
			// constructor over one of their value-type members. That silently destroys state which was
			// present in the save stream: Hover resets its steering FacingClass, Tunnel resets its dig
			// timer, Teleport resets its phase timer, and Rocket resets its trailer timer. The consequence
			// can be delayed until a moving unit reaches its next path cell, which makes a sound keyframe
			// appear to diverge hundreds of frames after it was loaded.
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

			// Each TiberiumClass keeps two priority queues - one for growth, one for spread - naming the
			// cells waiting to change and when. TiberiumClass::Grow (0x722F00) pops the top of the growth
			// heap, thickens that cell, and pushes it back scored Frame + rand() % 50, so the order the ore
			// field evolves in is something the game builds up over its whole length.
			//
			// Load_Game (0x67E440) finishes with Tiberium_Init_Growth_Data (0x722D00) and
			// Tiberium_Init_Spread_Data (0x722240). Both throw the queues away and rebuild them through
			// TiberiumClass::Recalc_Growth_Data (0x7233A0), which walks the map with the cell iterator and
			// pushes every eligible cell scored 0.0. The set of cells survives that; the order does not. So
			// after a load the ore grows in map-scan order instead of the order the recording had reached,
			// different cells thicken, harvesters pick different ore, and the units driving around them take
			// different paths - none of which draws a single extra random number, which is why this stayed
			// invisible to every randomiser check while showing up hundreds of frames later as a unit
			// following a different route.
			//
			// The queues are derived state the savegame does not carry, so the keyframe carries them: the
			// live heap as node values, and the per-cell "already queued" flags. Restoring is a write-back
			// over what Tiberium_Init_* just built.
			//
			// The node pool behind the heap is a bump allocator that is only ever appended to and reset
			// wholesale by Recalc_Growth_Data, and nothing reads an entry that is not in the heap - so only
			// the entries the heap points at are worth keeping, and the pool is rewritten compacted.
			struct TiberiumQueueSnapshot
			{
				bool Present = false;
				// The live heap in its own order, held as values; the engine numbers its heap from one,
				// so entry k here is the engine's slot k + 1.
				std::vector<PriorityQueueClassNode> Heap;
				// TiberiumLogic::CellIndexesWithTiberium, one bit per cell rather than the engine's one byte.
				std::vector<unsigned char> CellFlagBits;
				int CellFlagCount = 0;
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

			struct Keyframe
			{
				int Frame = 0;
				// Load constructs the saved object graph after ScenarioClass::Load has restored
				// this counter. Those constructors consume IDs even though each object's Load then
				// replaces its temporary ID with the saved one, so the counter has to be restored
				// explicitly once the whole load has finished.
				int ScenarioUniqueID = 0;
				// The synchronised randomiser, kept as opaque bytes because all that matters is that
				// every one of them goes back. Compute_Game_CRC consumes this randomiser and walks the
				// vectors below in their current order, so both are part of what has to match.
				std::array<unsigned char, sizeof(Randomizer)> Random {};
				std::vector<TechnoSnapshot> Technos;
				PlanningSnapshot Planning;
				TiberiumSnapshot Tiberium;
				std::vector<unsigned char> CellPassability;
				std::vector<CellLevelPassabilityStruct> ZonePassability;
				std::vector<LevelAndPassabilityStruct2> CellSubzones;
				std::array<std::vector<uint16_t>, 13> MovementZones;
				SubzoneGraphSnapshot SubzoneGraph;
				std::vector<LocomotorResetSnapshot> LocomotorResetStates;
				// Every object array the engine iterates, in the order it held them. None of these is hashed
				// by Compute_Game_CRC, which is why a shuffle stays invisible: the frame still hashes the
				// same and the objects still do all the same things. What changes is who gets asked first,
				// and the engine settles a tie by taking whichever candidate it happened to reach first -
				// a decision made without drawing a single random number, so nothing notices for hundreds
				// of frames. The load rebuilds these as it reconstructs the object graph and does not put
				// them back the way they were.
				std::array<std::vector<uint32_t>, OrderedCollectionCount> Orders;
				std::array<std::vector<uint32_t>, 5> LayerOrders;
			};

			struct SeekState
			{
				bool StoreReady = false;
				int Interval = 0;
				std::vector<Keyframe> Keyframes;

				bool Seeking = false;
				int TargetFrame = -1;
				// Set when a seek needs a keyframe loaded, and consumed at the top of the next
				// frame - a load tears the world down and rebuilds it, which is not something to do
				// from the middle of one.
				bool LoadPending = false;
				Keyframe PendingLoadKeyframe;
				bool LoadInProgress = false;

				int FramesSinceRender = 0;
				// What playback was doing before the seek, restored when it lands.
				bool ResumePaused = false;
				bool VocAllowedBeforeSeek = true;
			};

			SeekState State;

			// VocClass::Play (0x750920) returns immediately when this is clear, which silences the
			// sound effects a seek would otherwise fire off in one burst as it runs through frames.
			bool& VocAllowed()
			{
				return *reinterpret_cast<bool*>(0x8464AC);
			}

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

			uint32_t UniqueIDOf(const AbstractClass* pObject)
			{
				return pObject ? static_cast<uint32_t>(pObject->UniqueID) : 0u;
			}

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

			// Rebuild a collection's pointer order without adding or removing anything. Matching by
			// UniqueID is stable across a save/load because each object's raw Load restores that ID.
			template <typename TCollection>
			bool RestoreObjectOrder(TCollection& collection, const std::vector<uint32_t>& savedOrder,
				const char* collectionName, bool& changed)
			{
				changed = false;
				if (collection.Count != static_cast<int>(savedOrder.size()))
				{
					Debug::Log("[Replay] Keyframe collection %s has %d objects after loading; expected %d.\n",
						collectionName, collection.Count, static_cast<int>(savedOrder.size()));
					return false;
				}

				using Pointer = std::remove_reference_t<decltype(collection.Items[0])>;
				std::vector<Pointer> reordered(savedOrder.size(), nullptr);
				std::vector<bool> used(savedOrder.size(), false);

				for (size_t savedIndex = 0; savedIndex < savedOrder.size(); ++savedIndex)
				{
					bool found = false;
					for (int currentIndex = 0; currentIndex < collection.Count; ++currentIndex)
					{
						const size_t index = static_cast<size_t>(currentIndex);
						if (!used[index] && UniqueIDOf(collection.Items[currentIndex]) == savedOrder[savedIndex])
						{
							reordered[savedIndex] = collection.Items[currentIndex];
							used[index] = true;
							found = true;
							break;
						}
					}

					if (!found)
					{
						Debug::Log("[Replay] Keyframe collection %s is missing object unique ID %u.\n",
							collectionName, savedOrder[savedIndex]);
						return false;
					}
				}

				for (int i = 0; i < collection.Count; ++i)
				{
					if (collection.Items[i] != reordered[static_cast<size_t>(i)])
						changed = true;

					collection.Items[i] = reordered[static_cast<size_t>(i)];
				}

				return true;
			}

			// The three watchers below walk every techno, every layer object and every cell on the map,
			// every frame, and keep what they find for the whole replay. That is the right trade while
			// hunting a divergence and completely the wrong one while watching a replay, so they are off
			// unless ReplayDiagnostics asks for them.
			bool DiagnosticsWanted()
			{
				const auto* const pConfig = GetConfig();
				return pConfig && pConfig->ReplayDiagnostics;
			}

			// The watches used to fall silent after a single report, which cost a round of this: an inert
			// difference ten frames after one load silenced them for a second load two thousand frames
			// later, where the real divergence was. They now report a few times, and every load starts
			// them over so each seek is diagnosed on its own.
			constexpr int MaxDriftReports = 8;

			// An object is worth reporting once per load, not once per frame for as long as it stays
			// different.
			std::unordered_set<uint32_t> ReportedDriftObjects;

			#pragma region Per-frame layer watch

			// Compute_Game_CRC does not only read technos. It folds in the coordinates and types of every
			// object in the five display layers and in the logic queue as well - which is how a frame can hash
			// differently while every techno still matches, as one seek did: the hash moved at 5586 and the
			// object watch had nothing to say until 5587. Anims and bullets live in those layers and in no
			// array the object watch walks.
			//
			// This watches exactly what the hash reads there, so the watch can no longer be the later of the
			// two.
			constexpr size_t MaxWatchedLayerSamples = 2000000;

			struct LayerSample
			{
				uint32_t Id;
				int32_t Layer;
				int32_t Type;
				int32_t X;
				int32_t Y;
				int32_t Z;
				bool operator==(const LayerSample&) const = default;
			};

			std::unordered_map<int, std::vector<LayerSample>> WatchedLayersByFrame;
			size_t WatchedLayerSampleCount = 0;
			int WatchedLayerDriftReports = 0;

			void ResetLayerWatch()
			{
				WatchedLayersByFrame.clear();
				WatchedLayerSampleCount = 0;
				WatchedLayerDriftReports = 0;
			}

			void SampleLayerObjects(std::vector<LayerSample>& out)
			{
				out.clear();

				const auto sampleOne = [&out](const LayerClass& collection, int layer)
				{
					for (int i = 0; i < collection.Count; ++i)
					{
						const auto* const pObject = collection.Items[i];
						if (!pObject)
							continue;

						out.push_back(LayerSample {
							UniqueIDOf(pObject),
							layer,
							static_cast<int32_t>(pObject->WhatAmI()),
							pObject->Location.X,
							pObject->Location.Y,
							pObject->Location.Z
						});
					}
				};

				for (int layer = 0; layer < 5; ++layer)
					sampleOne(MapClass::ObjectsInLayers[layer], layer);

				sampleOne(LogicClass::Instance, 5);
			}

			void ReportLayerDrift(int frame, const std::vector<LayerSample>& before,
				const std::vector<LayerSample>& now)
			{
				++WatchedLayerDriftReports;

				static const char* const LayerNames[6] =
					{ "underground", "surface", "ground", "air", "top", "logic queue" };

				if (before.size() != now.size())
				{
					Debug::Log("[Replay] Frame %d: the display layers hold %d objects, %d the first time "
						"round - something the hash reads was created or destroyed on one side only.\n",
						frame, static_cast<int>(now.size()), static_cast<int>(before.size()));
				}

				for (size_t i = 0; i < std::min(before.size(), now.size()); ++i)
				{
					if (before[i] == now[i])
						continue;

					const LayerSample& was = before[i];
					const LayerSample& is = now[i];
					const char* const where = (is.Layer >= 0 && is.Layer < 6) ? LayerNames[is.Layer] : "?";

					Debug::Log("[Replay] Frame %d: object %u in the %s layer drifted - the first thing the "
						"hash reads that did.\n", frame, is.Id, where);

					if (was.Id != is.Id)
						Debug::Log("[Replay]   at this position it was object %u.\n", was.Id);
					if (was.Type != is.Type)
						Debug::Log("[Replay]   type %d, was %d.\n", is.Type, was.Type);
					if (was.X != is.X || was.Y != is.Y || was.Z != is.Z)
					{
						Debug::Log("[Replay]   at %d/%d/%d, was %d/%d/%d.\n", is.X, is.Y, is.Z,
							was.X, was.Y, was.Z);
					}

					return;
				}
			}

			void ServiceLayerWatch()
			{
				if (!DiagnosticsWanted())
					return;

				if (WatchedLayerDriftReports >= MaxDriftReports || !ScenarioClass::Instance)
					return;

				const int frame = static_cast<int>(Unsorted::CurrentFrame);
				const auto it = WatchedLayersByFrame.find(frame);

				if (it != WatchedLayersByFrame.end())
				{
					std::vector<LayerSample> now;
					SampleLayerObjects(now);

					if (now != it->second)
						ReportLayerDrift(frame, it->second, now);

					return;
				}

				if (WatchedLayerSampleCount >= MaxWatchedLayerSamples)
					return;

				std::vector<LayerSample>& sample = WatchedLayersByFrame[frame];
				SampleLayerObjects(sample);
				WatchedLayerSampleCount += sample.size();
			}

			#pragma endregion Per-frame layer watch

			#pragma region Per-frame cell watch

			// The object watch only sees objects, and the ore queues showed how much of what decides an
			// object's behaviour lives outside them. This is the same trick for the map: every cell is
			// sampled each frame, the frames are compared the way the object samples are, and the cells that
			// come out different are named along with the fields that moved.
			//
			// Frames are stored as deltas, because a frame only ever changes a handful of cells and the full
			// table is far too big to keep one copy of per frame. The table is kept whole - as values rather
			// than hashes, so the difference can be named - at the frames a keyframe exists at, which is
			// where a load has to be checked against something.
			constexpr size_t MaxWatchedCellChanges = 4000000;

			// Everything here is state the simulation reads. Nothing that only decides what gets drawn - the
			// redraw frame, the fog and shroud frames, the lighting - because those differ harmlessly between
			// a frame that was rendered and one a seek skipped past.
			#define REPLAY_FOR_EACH_CELL_FIELD(ENTRY) \
				ENTRY(OccupationFlags) \
				ENTRY(AltOccupationFlags) \
				ENTRY(Flags) \
				ENTRY(AltFlags) \
				ENTRY(OverlayTypeIndex) \
				ENTRY(OverlayData) \
				ENTRY(SmudgeTypeIndex) \
				ENTRY(SmudgeData) \
				ENTRY(LandType) \
				ENTRY(Passability) \
				ENTRY(WallOwnerIndex) \
				ENTRY(InfantryOwnerIndex) \
				ENTRY(AltInfantryOwnerIndex) \
				ENTRY(Level) \
				ENTRY(Height) \
				ENTRY(BlockedNeighbours) \
				ENTRY(OccupyHeightsCoveringMe) \
				ENTRY(TubeIndex) \
				ENTRY(ShroudCounter) \
				ENTRY(CloakedByHouses) \
				ENTRY(GapsCoveringThisCell) \
				ENTRY(FirstObject) \
				ENTRY(SecondObject) \
				ENTRY(AltObject) \
				ENTRY(Jumpjet)

			enum CellFieldIndex
			{
				#define REPLAY_CELL_FIELD_ENUM(name) CellField_##name,
				REPLAY_FOR_EACH_CELL_FIELD(REPLAY_CELL_FIELD_ENUM)
				#undef REPLAY_CELL_FIELD_ENUM
				CellFieldCount
			};

			const char* const CellFieldNames[CellFieldCount] =
			{
				#define REPLAY_CELL_FIELD_NAME(name) #name,
				REPLAY_FOR_EACH_CELL_FIELD(REPLAY_CELL_FIELD_NAME)
				#undef REPLAY_CELL_FIELD_NAME
			};

			struct CellSnapshot
			{
				std::array<int32_t, CellFieldCount> Fields {};
				bool operator==(const CellSnapshot&) const = default;
			};

			struct CellChange
			{
				int32_t Index;
				uint32_t Hash;
				bool operator==(const CellChange&) const = default;
			};

			std::vector<CellSnapshot> CellScratch;
			std::vector<uint32_t> LiveCellHashes;
			std::unordered_map<int, std::vector<CellChange>> CellChangesByFrame;
			std::unordered_map<int, std::vector<CellSnapshot>> CellBaselines;
			size_t WatchedCellChangeCount = 0;
			int WatchedCellDriftReports = 0;

			void ResetCellWatch()
			{
				CellScratch.clear();
				LiveCellHashes.clear();
				CellChangesByFrame.clear();
				CellBaselines.clear();
				WatchedCellChangeCount = 0;
				WatchedCellDriftReports = 0;
			}

			void SampleCell(const CellClass* pCell, CellSnapshot& out)
			{
				out = CellSnapshot {};

				auto& fields = out.Fields;
				fields[CellField_OccupationFlags] = static_cast<int32_t>(pCell->OccupationFlags);
				fields[CellField_AltOccupationFlags] = static_cast<int32_t>(pCell->AltOccupationFlags);
				fields[CellField_Flags] = static_cast<int32_t>(pCell->Flags);
				fields[CellField_AltFlags] = static_cast<int32_t>(pCell->AltFlags);
				fields[CellField_OverlayTypeIndex] = pCell->OverlayTypeIndex;
				fields[CellField_OverlayData] = pCell->OverlayData;
				fields[CellField_SmudgeTypeIndex] = pCell->SmudgeTypeIndex;
				fields[CellField_SmudgeData] = pCell->SmudgeData;
				fields[CellField_LandType] = static_cast<int32_t>(pCell->LandType);
				fields[CellField_Passability] = static_cast<int32_t>(pCell->Passability);
				fields[CellField_WallOwnerIndex] = pCell->WallOwnerIndex;
				fields[CellField_InfantryOwnerIndex] = pCell->InfantryOwnerIndex;
				fields[CellField_AltInfantryOwnerIndex] = pCell->AltInfantryOwnerIndex;
				fields[CellField_Level] = pCell->Level;
				fields[CellField_Height] = pCell->Height;
				fields[CellField_BlockedNeighbours] = pCell->BlockedNeighbours;
				fields[CellField_OccupyHeightsCoveringMe] = pCell->OccupyHeightsCoveringMe;
				fields[CellField_TubeIndex] = pCell->TubeIndex;
				fields[CellField_ShroudCounter] = pCell->ShroudCounter;
				fields[CellField_CloakedByHouses] = static_cast<int32_t>(pCell->CloakedByHouses);
				fields[CellField_GapsCoveringThisCell] = static_cast<int32_t>(pCell->GapsCoveringThisCell);

				// The head of the linked list the engine walks whenever it asks a cell what is on it, and the
				// one behind it - enough to catch two objects sharing a cell in the other order.
				const ObjectClass* const pFirst = pCell->FirstObject;
				fields[CellField_FirstObject] = static_cast<int32_t>(UniqueIDOf(pFirst));
				fields[CellField_SecondObject] =
					static_cast<int32_t>(UniqueIDOf(pFirst ? pFirst->NextObject : nullptr));
				fields[CellField_AltObject] = static_cast<int32_t>(UniqueIDOf(pCell->AltObject));
				fields[CellField_Jumpjet] = static_cast<int32_t>(UniqueIDOf(pCell->Jumpjet));
			}

			uint32_t HashCell(const CellSnapshot& snapshot)
			{
				uint32_t hash = 2166136261u;
				for (const int32_t field : snapshot.Fields)
					hash = (hash ^ static_cast<uint32_t>(field)) * 16777619u;

				return hash;
			}

			int WatchedCellCount()
			{
				return std::min(MapClass::Instance.MaxNumCells, MapClass::MaxCells);
			}

			// Hashes only, which is all a frame needs unless it is one a whole table is kept for.
			// Filling a quarter of a million snapshots into a vector every frame was most of what made
			// the watch too slow to leave running.
			void HashAllCells(std::vector<uint32_t>& out)
			{
				const int count = std::max(WatchedCellCount(), 0);
				out.assign(static_cast<size_t>(count), 0u);

				CellSnapshot snapshot;
				for (int i = 0; i < count; ++i)
				{
					if (const auto* const pCell = MapClass::Instance.Cells[i])
					{
						SampleCell(pCell, snapshot);
						out[static_cast<size_t>(i)] = HashCell(snapshot);
					}
				}
			}

			void SampleAllCells()
			{
				const int count = std::max(WatchedCellCount(), 0);
				CellScratch.assign(static_cast<size_t>(count), CellSnapshot {});

				for (int i = 0; i < count; ++i)
				{
					if (const auto* const pCell = MapClass::Instance.Cells[i])
						SampleCell(pCell, CellScratch[static_cast<size_t>(i)]);
				}
			}

			void DescribeCell(int cellIndex)
			{
				const auto* const pCell = (cellIndex >= 0 && cellIndex < WatchedCellCount())
					? MapClass::Instance.Cells[cellIndex] : nullptr;
				if (!pCell)
					return;

				int depth = 0;
				for (const ObjectClass* pObject = pCell->FirstObject; pObject && depth < 4;
					pObject = pObject->NextObject, ++depth)
				{
					const auto* const pTechno = abstract_cast<const TechnoClass*>(pObject);
					Debug::Log("[Replay]     standing here: object %u (%s).\n", UniqueIDOf(pObject),
						pTechno && pTechno->get_ID() ? pTechno->get_ID() : "<not a techno>");
				}
			}

			// Which fields moved, by name, so a report says what to go and read rather than only where.
			void ReportCellFields(int cellIndex, const CellSnapshot& before, const CellSnapshot& now)
			{
				const auto* const pCell = (cellIndex >= 0 && cellIndex < WatchedCellCount())
					? MapClass::Instance.Cells[cellIndex] : nullptr;

				if (pCell)
				{
					Debug::Log("[Replay]   cell %d,%d (index %d):\n", pCell->MapCoords.X, pCell->MapCoords.Y,
						cellIndex);
				}
				else
				{
					Debug::Log("[Replay]   cell index %d:\n", cellIndex);
				}

				for (int field = 0; field < CellFieldCount; ++field)
				{
					if (before.Fields[field] == now.Fields[field])
						continue;

					Debug::Log("[Replay]     %s is %d (0x%X), was %d (0x%X).\n", CellFieldNames[field],
						now.Fields[field], static_cast<unsigned int>(now.Fields[field]),
						before.Fields[field], static_cast<unsigned int>(before.Fields[field]));
				}

				DescribeCell(cellIndex);
			}

			// The load frame. Every cell that came back different is worth having, not just the first: one
			// cell and a thousand cells are completely different diagnoses. This does not silence the watch,
			// because what happens afterwards is the more interesting half.
			void ReportLoadCellDrift(int frame, const std::vector<CellSnapshot>& baseline)
			{
				constexpr int MaxReportedCells = 12;

				int differing = 0;
				for (size_t i = 0; i < baseline.size() && i < CellScratch.size(); ++i)
				{
					if (baseline[i] == CellScratch[i])
						continue;

					++differing;
					if (differing <= MaxReportedCells)
						ReportCellFields(static_cast<int>(i), baseline[i], CellScratch[i]);
				}

				if (differing == 0)
					return;

				Debug::Log("[Replay] Frame %d: the load left %d of %d cells different from the frame the "
					"keyframe was written at%s.\n", frame, differing, static_cast<int>(baseline.size()),
					differing > MaxReportedCells ? " (only the first few are listed)" : "");
			}

			void ServiceCellWatch()
			{
				if (!DiagnosticsWanted())
					return;

				if (WatchedCellDriftReports >= MaxDriftReports || !ScenarioClass::Instance
					|| !MapClass::Instance.Cells.Items)
					return;

				const int frame = static_cast<int>(Unsorted::CurrentFrame);

				std::vector<uint32_t> now;
				HashAllCells(now);

				std::vector<CellChange> delta;
				if (LiveCellHashes.size() == now.size())
				{
					for (size_t i = 0; i < now.size(); ++i)
					{
						if (now[i] != LiveCellHashes[i])
							delta.push_back(CellChange { static_cast<int32_t>(i), now[i] });
					}
				}

				const auto baseline = CellBaselines.find(frame);
				const auto changes = CellChangesByFrame.find(frame);

				if (baseline != CellBaselines.end())
				{
					// A whole table was kept for this frame, so the load can be checked against it directly.
					SampleAllCells();

					if (baseline->second.size() != CellScratch.size())
						Debug::Log("[Replay] Frame %d: the map is a different size after the load.\n", frame);
					else
						ReportLoadCellDrift(frame, baseline->second);
				}
				else if (changes != CellChangesByFrame.end())
				{
					if (changes->second != delta)
					{
						// The first cell the two passes disagree about changing, or changed differently.
						++WatchedCellDriftReports;

						const std::vector<CellChange>& before = changes->second;
						int cellIndex = -1;
						for (size_t i = 0; i < std::max(before.size(), delta.size()); ++i)
						{
							const bool haveBefore = i < before.size();
							const bool haveNow = i < delta.size();
							if (!haveBefore || !haveNow || before[i] != delta[i])
							{
								cellIndex = haveNow ? delta[i].Index : (haveBefore ? before[i].Index : -1);
								break;
							}
						}

						Debug::Log("[Replay] Frame %d: the map drifted - a cell changed that did not change "
							"the first time round, or changed into something else. %d cells changed on this "
							"frame, %d the first time round.\n", frame, static_cast<int>(delta.size()),
							static_cast<int>(before.size()));

						if (cellIndex >= 0 && cellIndex < WatchedCellCount())
						{
							// Only hashes were kept for this frame, so what the cell held the first time round
							// cannot be printed - only what it holds now.
							CellSnapshot snapshot;
							if (const auto* const pCell = MapClass::Instance.Cells[cellIndex])
								SampleCell(pCell, snapshot);

							ReportCellFields(cellIndex, snapshot, snapshot);
						}
					}
				}
				else if (WatchedCellChangeCount < MaxWatchedCellChanges)
				{
					if (State.Interval > 0 && frame % State.Interval == 0)
					{
						SampleAllCells();
						CellBaselines[frame] = CellScratch;
					}

					WatchedCellChangeCount += delta.size();
					CellChangesByFrame[frame] = std::move(delta);
				}

				LiveCellHashes = std::move(now);
			}

			#pragma endregion Per-frame cell watch

			#pragma region Per-frame object watch

			// The draw trace only sees frames where something asked the randomiser for a number, and the
			// paths that move underneath us draw nothing at all. So this watches the state itself: every
			// frame of playback each object is written down, and a frame replayed after a seek is checked
			// against the same frame the first time round. It reports the first frame anything drifted -
			// where the load actually went wrong, rather than where the consequences first showed - and
			// names the fields that moved.
			constexpr size_t MaxWatchedObjectSamples = 600000;

			struct WatchSample
			{
				uint32_t Id;
				uint16_t PrimaryFacing;
				uint16_t PrimaryDesired;
				uint16_t SecondaryFacing;
				uint16_t SecondaryDesired;
				int32_t Mission;
				int32_t MissionStatus;
				int32_t MissionStart;
				int32_t UpdateStart;
				int32_t UpdateLeft;
				int32_t TargetingStart;
				int32_t TargetingLeft;
				uint32_t TargetId;
				uint32_t ArchiveId;
				uint32_t DestinationId;
				int32_t NavQueueCount;
				int32_t PlanningPathIndex;
				int16_t WaypointDeltaX;
				int16_t WaypointDeltaY;
				int16_t WaypointCellX;
				int16_t WaypointCellY;
				int8_t WaypointIndex;
				bool PlanningCommandInFlight;
				int32_t PlanningNodeCount;
				int32_t PlanningCurrentNode;
				int32_t PlanningClosedLoopNodeCount;
				int32_t PlanningStepsToClosedLoop;
				int32_t X;
				int32_t Y;
				int32_t Z;
				int32_t Health;
				// Copied rather than pointed at: the load destroys and rebuilds the type objects too, so
				// a pointer taken on the first pass reads freed memory once the keyframe is back.
				std::array<char, 32> TypeId {};
				int32_t OwnerIndex = -1;
				uint16_t PrimaryStart = 0;
				uint16_t PrimaryROT = 0;
				int32_t PrimaryRotationStart = 0;
				int32_t PrimaryRotationLeft = 0;
				std::array<int32_t, 24> PathDirections {};
				int32_t PathDelayStart = 0;
				int32_t PathDelayLeft = 0;
				int32_t PathWaitTimes = 0;
				int32_t UnknownPathTimerStart = 0;
				int32_t UnknownPathTimerLeft = 0;
				int32_t BlockageTimerStart = 0;
				int32_t BlockageTimerLeft = 0;
				int16_t CurrentMapX = 0;
				int16_t CurrentMapY = 0;
				int16_t LastMapX = 0;
				int16_t LastMapY = 0;
				uint32_t LastDestinationId = 0;
				unsigned char LocomotorResetKind = 0xFF;
				uint16_t LocomotorFacing = 0;
				uint16_t LocomotorDesired = 0;
				uint16_t LocomotorStart = 0;
				uint16_t LocomotorROT = 0;
				int32_t LocomotorTimerStart = 0;
				int32_t LocomotorTimerLeft = 0;
				int32_t LocomotorTimerRate = 0;
			};

			std::unordered_map<int, std::vector<WatchSample>> WatchedObjectsByFrame;
			size_t WatchedObjectSampleCount = 0;
			int WatchedObjectDriftReports = 0;

			void ResetObjectWatch()
			{
				WatchedObjectsByFrame.clear();
				WatchedObjectSampleCount = 0;
				WatchedObjectDriftReports = 0;
			}

			void SampleObjectWatch(std::vector<WatchSample>& out)
			{
				out.clear();
				out.reserve(static_cast<size_t>(std::max(TechnoClass::Array.Count, 0)));

				for (int i = 0; i < TechnoClass::Array.Count; ++i)
				{
					const auto* const pTechno = TechnoClass::Array.Items[i];
					if (!pTechno)
						continue;

					const auto* const pFoot = abstract_cast<const FootClass*>(pTechno);
					const auto* const pToken = pTechno->PlanningToken;

					WatchSample sample {
						UniqueIDOf(pTechno),
						pTechno->PrimaryFacing.Current().Raw,
						pTechno->PrimaryFacing.Desired().Raw,
						pTechno->SecondaryFacing.Current().Raw,
						pTechno->SecondaryFacing.Desired().Raw,
						static_cast<int32_t>(pTechno->CurrentMission),
						static_cast<int32_t>(pTechno->MissionStatus),
						static_cast<int32_t>(pTechno->CurrentMissionStartTime),
						static_cast<int32_t>(pTechno->UpdateTimer.StartTime),
						static_cast<int32_t>(pTechno->UpdateTimer.TimeLeft),
						static_cast<int32_t>(pTechno->TargetingTimer.StartTime),
						static_cast<int32_t>(pTechno->TargetingTimer.TimeLeft),
						UniqueIDOf(pTechno->Target),
						UniqueIDOf(pTechno->ArchiveTarget),
						pFoot ? UniqueIDOf(pFoot->Destination) : 0u,
						pFoot ? pFoot->NavQueue.Count : 0,
						pFoot ? pFoot->PlanningPathIdx : -1,
						pFoot ? pFoot->WaypointNearbyAccessibleCellDelta.X : 0,
						pFoot ? pFoot->WaypointNearbyAccessibleCellDelta.Y : 0,
						pFoot ? pFoot->WaypointCell.X : 0,
						pFoot ? pFoot->WaypointCell.Y : 0,
						pFoot ? pFoot->WaypointIndex : static_cast<signed char>(-1),
						pToken ? pToken->field_1C : false,
						pToken ? pToken->PlanningNodes.Count : 0,
						pToken ? pToken->field_8C : -1,
						pToken ? pToken->ClosedLoopNodeCount : -1,
						pToken ? pToken->StepsToClosedLoop : -1,
						pTechno->Location.X,
						pTechno->Location.Y,
						pTechno->Location.Z,
						static_cast<int32_t>(pTechno->Health)
					};

					if (const char* const pTypeId = pTechno->get_ID())
						strncpy_s(sample.TypeId.data(), sample.TypeId.size(), pTypeId, _TRUNCATE);
					sample.OwnerIndex = pTechno->Owner ? pTechno->Owner->ArrayIndex : -1;
					sample.PrimaryStart = pTechno->PrimaryFacing.StartFacing.Raw;
					sample.PrimaryROT = pTechno->PrimaryFacing.ROT.Raw;
					sample.PrimaryRotationStart = pTechno->PrimaryFacing.RotationTimer.StartTime;
					sample.PrimaryRotationLeft = pTechno->PrimaryFacing.RotationTimer.TimeLeft;

					if (pFoot)
					{
						std::copy(std::begin(pFoot->PathDirections), std::end(pFoot->PathDirections),
							sample.PathDirections.begin());
						sample.PathDelayStart = pFoot->PathDelayTimer.StartTime;
						sample.PathDelayLeft = pFoot->PathDelayTimer.TimeLeft;
						sample.PathWaitTimes = pFoot->PathWaitTimes;
						sample.UnknownPathTimerStart = pFoot->unknown_timer_650.StartTime;
						sample.UnknownPathTimerLeft = pFoot->unknown_timer_650.TimeLeft;
						sample.BlockageTimerStart = pFoot->BlockagePathTimer.StartTime;
						sample.BlockageTimerLeft = pFoot->BlockagePathTimer.TimeLeft;
						sample.CurrentMapX = pFoot->CurrentMapCoords.X;
						sample.CurrentMapY = pFoot->CurrentMapCoords.Y;
						sample.LastMapX = pFoot->LastMapCoords.X;
						sample.LastMapY = pFoot->LastMapCoords.Y;
						sample.LastDestinationId = UniqueIDOf(pFoot->LastDestination);

						if (pFoot->Locomotor)
						{
							ILocomotion* const pInterface = pFoot->Locomotor.GetInterfacePtr();
							if (const auto* const pHover =
								locomotion_cast<const HoverLocomotionClass*>(pInterface))
							{
								sample.LocomotorResetKind =
									static_cast<unsigned char>(LocomotorResetStateKind::HoverFacing);
								sample.LocomotorFacing = pHover->LocomotionFacing.Current().Raw;
								sample.LocomotorDesired =
									pHover->LocomotionFacing.DesiredFacing.Raw;
								sample.LocomotorStart = pHover->LocomotionFacing.StartFacing.Raw;
								sample.LocomotorROT = pHover->LocomotionFacing.ROT.Raw;
								sample.LocomotorTimerStart =
									pHover->LocomotionFacing.RotationTimer.StartTime;
								sample.LocomotorTimerLeft =
									pHover->LocomotionFacing.RotationTimer.TimeLeft;
							}
							else if (const auto* const pTunnel =
								locomotion_cast<const TunnelLocomotionClass*>(pInterface))
							{
								sample.LocomotorResetKind =
									static_cast<unsigned char>(
										LocomotorResetStateKind::TunnelDigTimer);
								sample.LocomotorTimerStart = pTunnel->DigTimer.StartTime;
								sample.LocomotorTimerLeft = pTunnel->DigTimer.TimeLeft;
								sample.LocomotorTimerRate = pTunnel->DigTimer.Rate;
							}
							else if (const auto* const pTeleport =
								locomotion_cast<const TeleportLocomotionClass*>(pInterface))
							{
								sample.LocomotorResetKind =
									static_cast<unsigned char>(LocomotorResetStateKind::TeleportTimer);
								sample.LocomotorTimerStart = pTeleport->Timer.StartTime;
								sample.LocomotorTimerLeft = pTeleport->Timer.TimeLeft;
							}
							else if (const auto* const pRocket =
								locomotion_cast<const RocketLocomotionClass*>(pInterface))
							{
								sample.LocomotorResetKind =
									static_cast<unsigned char>(
										LocomotorResetStateKind::RocketTrailerTimer);
								sample.LocomotorTimerStart = pRocket->TrailerTimer.StartTime;
								sample.LocomotorTimerLeft = pRocket->TrailerTimer.TimeLeft;
							}
						}
					}

					out.push_back(std::move(sample));
				}
			}

			// AStarClass::Build_Final_Path writes the facings and then a FACING_NONE terminator, and
			// FootClass::Basic_Path copies exactly that many entries into the unit, so everything past
			// the terminator is leftovers of routes the unit finished long ago.
			//
			// Those leftovers are not dead state, which is worth being clear about because this file
			// once assumed they were. AStarClass::Apply_Path_Collision_Avoidance reads a blocking
			// unit's Path[0] and Path[1] - Path[2] as well for infantry - to decide whether it is about
			// to stop and therefore worth routing around, and it does not care whether those entries
			// are still part of a live route. FootClass::Serialize writes the whole array, so they come
			// back from a load and they are as deterministic as anything else. The whole array is
			// compared, and the live length is reported alongside so a difference can be read for what
			// it is.
			int LiveRouteLength(const std::array<int32_t, 24>& path)
			{
				int length = 0;
				while (length < static_cast<int>(path.size()) && path[length] >= 0 && path[length] <= 7)
					++length;

				return length;
			}


			// Which entries of the path buffer the engine can actually reach. Apply_Path_Collision_
			// Avoidance tests Path[0] first and gives up on the blocker when it is the terminator, so it
			// only ever reads Path[1] and Path[2] - Path[2] for infantry - on a unit that has a live
			// route. A unit standing still carries leftovers in every slot but the first, and nothing
			// will look at them until it is given a route, which overwrites them. Comparing those cost
			// two rounds of this: three frames of the same standing unit used up the whole report budget
			// before the frame that mattered.
			//
			// They are still printed when something else reports the object, marked for what they are.
			bool SameReadablePath(const std::array<int32_t, 24>& a, const std::array<int32_t, 24>& c)
			{
				const int length = LiveRouteLength(a);
				if (length != LiveRouteLength(c))
					return false;

				// Everything the unit itself will walk, plus the terminator that stops the avoidance scan.
				// A route one step long puts the terminator at index 1, so the scan never reaches index 2 -
				// and index 2 is where the leftovers start, which are not deterministic: Basic_Path copies
				// path->Length entries out of an uninitialised stack buffer, and the stack differs between a
				// run that reached the frame straight and one that loaded a keyframe to get there.
				const int readable = std::min(length + 1, static_cast<int>(a.size()));
				return std::equal(a.begin(), a.begin() + readable, c.begin());
			}

			bool SameWatchSample(const WatchSample& a, const WatchSample& c)
			{
				return a.Id == c.Id && a.PrimaryFacing == c.PrimaryFacing
					&& a.PrimaryDesired == c.PrimaryDesired
					&& a.SecondaryFacing == c.SecondaryFacing
					&& a.SecondaryDesired == c.SecondaryDesired
					&& a.Mission == c.Mission && a.MissionStatus == c.MissionStatus
					&& a.MissionStart == c.MissionStart && a.UpdateStart == c.UpdateStart
					&& a.UpdateLeft == c.UpdateLeft && a.TargetingStart == c.TargetingStart
					&& a.TargetingLeft == c.TargetingLeft && a.TargetId == c.TargetId
					&& a.ArchiveId == c.ArchiveId && a.DestinationId == c.DestinationId
					&& a.NavQueueCount == c.NavQueueCount
					&& a.PlanningPathIndex == c.PlanningPathIndex
					&& a.WaypointDeltaX == c.WaypointDeltaX
					&& a.WaypointDeltaY == c.WaypointDeltaY
					&& a.WaypointCellX == c.WaypointCellX
					&& a.WaypointCellY == c.WaypointCellY
					&& a.WaypointIndex == c.WaypointIndex
					&& a.PlanningCommandInFlight == c.PlanningCommandInFlight
					&& a.PlanningNodeCount == c.PlanningNodeCount
					&& a.PlanningCurrentNode == c.PlanningCurrentNode
					&& a.PlanningClosedLoopNodeCount == c.PlanningClosedLoopNodeCount
					&& a.PlanningStepsToClosedLoop == c.PlanningStepsToClosedLoop
					&& a.X == c.X && a.Y == c.Y && a.Z == c.Z
					&& a.Health == c.Health && a.OwnerIndex == c.OwnerIndex
					&& a.PrimaryStart == c.PrimaryStart && a.PrimaryROT == c.PrimaryROT
					&& a.PrimaryRotationStart == c.PrimaryRotationStart
					&& a.PrimaryRotationLeft == c.PrimaryRotationLeft
					&& SameReadablePath(a.PathDirections, c.PathDirections)
					&& a.PathDelayStart == c.PathDelayStart
					&& a.PathDelayLeft == c.PathDelayLeft
					&& a.PathWaitTimes == c.PathWaitTimes
					&& a.UnknownPathTimerStart == c.UnknownPathTimerStart
					&& a.UnknownPathTimerLeft == c.UnknownPathTimerLeft
					&& a.BlockageTimerStart == c.BlockageTimerStart
					&& a.BlockageTimerLeft == c.BlockageTimerLeft
					&& a.CurrentMapX == c.CurrentMapX && a.CurrentMapY == c.CurrentMapY
					&& a.LastMapX == c.LastMapX && a.LastMapY == c.LastMapY
					&& a.LastDestinationId == c.LastDestinationId
					&& a.LocomotorResetKind == c.LocomotorResetKind
					&& a.LocomotorFacing == c.LocomotorFacing
					&& a.LocomotorDesired == c.LocomotorDesired
					&& a.LocomotorStart == c.LocomotorStart
					&& a.LocomotorROT == c.LocomotorROT
					&& a.LocomotorTimerStart == c.LocomotorTimerStart
					&& a.LocomotorTimerLeft == c.LocomotorTimerLeft
					&& a.LocomotorTimerRate == c.LocomotorTimerRate;
			}

			void ReportObjectDrift(int frame, const std::vector<WatchSample>& before,
				const std::vector<WatchSample>& now)
			{
				if (before.size() != now.size())
				{
					++WatchedObjectDriftReports;
					Debug::Log("[Replay] Frame %d holds %d objects on the way back through, %d the first time "
						"round.\n", frame, static_cast<int>(now.size()), static_cast<int>(before.size()));
					return;
				}

				for (size_t i = 0; i < before.size(); ++i)
				{
					const WatchSample& was = before[i];
					const WatchSample& is = now[i];
					if (SameWatchSample(was, is) || !ReportedDriftObjects.insert(is.Id).second)
						continue;

					++WatchedObjectDriftReports;
					Debug::Log("[Replay] Frame %d: object %u drifted - the first frame anything did, so this "
						"is where the load went wrong rather than where it showed.\n", frame, is.Id);
					Debug::Log("[Replay]   object is %s owned by house %d; first pass was %s owned by "
						"house %d.\n", is.TypeId.data(), is.OwnerIndex, was.TypeId.data(), was.OwnerIndex);

					if (was.Id != is.Id)
						Debug::Log("[Replay]   at this position it was object %u.\n", was.Id);
					if (was.PrimaryFacing != is.PrimaryFacing
						|| was.PrimaryDesired != is.PrimaryDesired
						|| was.SecondaryFacing != is.SecondaryFacing
						|| was.SecondaryDesired != is.SecondaryDesired)
					{
						Debug::Log("[Replay]   facing body %u->%u turret %u->%u, was body %u->%u "
							"turret %u->%u.\n", is.PrimaryFacing, is.PrimaryDesired,
							is.SecondaryFacing, is.SecondaryDesired, was.PrimaryFacing,
							was.PrimaryDesired, was.SecondaryFacing, was.SecondaryDesired);
					}
					if (was.PrimaryStart != is.PrimaryStart || was.PrimaryROT != is.PrimaryROT
						|| was.PrimaryRotationStart != is.PrimaryRotationStart
						|| was.PrimaryRotationLeft != is.PrimaryRotationLeft)
					{
						Debug::Log("[Replay]   body turn start %u ROT %u timer %d/%d, was start %u "
							"ROT %u timer %d/%d.\n", is.PrimaryStart, is.PrimaryROT,
							is.PrimaryRotationStart, is.PrimaryRotationLeft, was.PrimaryStart,
							was.PrimaryROT, was.PrimaryRotationStart, was.PrimaryRotationLeft);
					}
					if (was.Mission != is.Mission || was.MissionStatus != is.MissionStatus
						|| was.MissionStart != is.MissionStart)
					{
						Debug::Log("[Replay]   mission %d status %d started %d, was mission %d status %d "
							"started %d.\n", is.Mission, is.MissionStatus, is.MissionStart,
							was.Mission, was.MissionStatus, was.MissionStart);
					}
					if (was.UpdateStart != is.UpdateStart || was.UpdateLeft != is.UpdateLeft)
					{
						Debug::Log("[Replay]   mission timer %d/%d, was %d/%d.\n",
							is.UpdateStart, is.UpdateLeft, was.UpdateStart, was.UpdateLeft);
					}
					if (was.TargetingStart != is.TargetingStart || was.TargetingLeft != is.TargetingLeft)
					{
						Debug::Log("[Replay]   targeting timer %d/%d, was %d/%d.\n",
							is.TargetingStart, is.TargetingLeft, was.TargetingStart, was.TargetingLeft);
					}
					if (was.TargetId != is.TargetId || was.ArchiveId != is.ArchiveId
						|| was.DestinationId != is.DestinationId || was.NavQueueCount != is.NavQueueCount)
					{
						Debug::Log("[Replay]   target %u archive %u destination %u queued %d, "
							"was target %u archive %u destination %u queued %d.\n",
							is.TargetId, is.ArchiveId, is.DestinationId, is.NavQueueCount,
							was.TargetId, was.ArchiveId, was.DestinationId, was.NavQueueCount);
					}
					if (was.PlanningPathIndex != is.PlanningPathIndex
						|| was.WaypointDeltaX != is.WaypointDeltaX
						|| was.WaypointDeltaY != is.WaypointDeltaY
						|| was.WaypointCellX != is.WaypointCellX
						|| was.WaypointCellY != is.WaypointCellY
						|| was.WaypointIndex != is.WaypointIndex)
					{
						Debug::Log("[Replay]   waypoint path %d node %d cell %d/%d delta %d/%d, "
							"was path %d node %d cell %d/%d delta %d/%d.\n",
							is.PlanningPathIndex, static_cast<int>(is.WaypointIndex),
							is.WaypointCellX, is.WaypointCellY, is.WaypointDeltaX,
							is.WaypointDeltaY, was.PlanningPathIndex,
							static_cast<int>(was.WaypointIndex), was.WaypointCellX,
							was.WaypointCellY, was.WaypointDeltaX, was.WaypointDeltaY);
					}
					if (was.PlanningCommandInFlight != is.PlanningCommandInFlight
						|| was.PlanningNodeCount != is.PlanningNodeCount
						|| was.PlanningCurrentNode != is.PlanningCurrentNode
						|| was.PlanningClosedLoopNodeCount != is.PlanningClosedLoopNodeCount
						|| was.PlanningStepsToClosedLoop != is.PlanningStepsToClosedLoop)
					{
						Debug::Log("[Replay]   planning token in-flight %d nodes %d current %d loop "
							"%d/%d, was %d nodes %d current %d loop %d/%d.\n",
							is.PlanningCommandInFlight, is.PlanningNodeCount,
							is.PlanningCurrentNode, is.PlanningClosedLoopNodeCount,
							is.PlanningStepsToClosedLoop, was.PlanningCommandInFlight,
							was.PlanningNodeCount, was.PlanningCurrentNode,
							was.PlanningClosedLoopNodeCount, was.PlanningStepsToClosedLoop);
					}
					if (was.PathDirections != is.PathDirections)
					{
						const int wasLength = LiveRouteLength(was.PathDirections);
						const int isLength = LiveRouteLength(is.PathDirections);
						Debug::Log("[Replay]   route is %d steps, was %d.\n", isLength, wasLength);

						for (int step = 0; step < static_cast<int>(is.PathDirections.size()); ++step)
						{
							if (was.PathDirections[step] == is.PathDirections[step])
								continue;

							Debug::Log("[Replay]   path entry %d is %d, was %d%s.\n", step,
								is.PathDirections[step], was.PathDirections[step],
								step >= isLength && step >= wasLength
									? " - past the terminator, which the collision avoidance still reads"
									: "");
							break;
						}
					}
					if (was.PathDelayStart != is.PathDelayStart
						|| was.PathDelayLeft != is.PathDelayLeft
						|| was.PathWaitTimes != is.PathWaitTimes
						|| was.UnknownPathTimerStart != is.UnknownPathTimerStart
						|| was.UnknownPathTimerLeft != is.UnknownPathTimerLeft
						|| was.BlockageTimerStart != is.BlockageTimerStart
						|| was.BlockageTimerLeft != is.BlockageTimerLeft)
					{
						Debug::Log("[Replay]   path delay %d/%d waits %d secondary %d/%d blockage "
							"%d/%d, was %d/%d waits %d secondary %d/%d blockage %d/%d.\n",
							is.PathDelayStart, is.PathDelayLeft, is.PathWaitTimes,
							is.UnknownPathTimerStart, is.UnknownPathTimerLeft,
							is.BlockageTimerStart, is.BlockageTimerLeft, was.PathDelayStart,
							was.PathDelayLeft, was.PathWaitTimes, was.UnknownPathTimerStart,
							was.UnknownPathTimerLeft, was.BlockageTimerStart,
							was.BlockageTimerLeft);
					}
					if (was.CurrentMapX != is.CurrentMapX || was.CurrentMapY != is.CurrentMapY
						|| was.LastMapX != is.LastMapX || was.LastMapY != is.LastMapY
						|| was.LastDestinationId != is.LastDestinationId)
					{
						Debug::Log("[Replay]   map cell %d/%d last %d/%d last destination %u, was "
							"%d/%d last %d/%d last destination %u.\n", is.CurrentMapX,
							is.CurrentMapY, is.LastMapX, is.LastMapY, is.LastDestinationId,
							was.CurrentMapX, was.CurrentMapY, was.LastMapX, was.LastMapY,
							was.LastDestinationId);
					}
					if (was.LocomotorResetKind != is.LocomotorResetKind
						|| was.LocomotorFacing != is.LocomotorFacing
						|| was.LocomotorDesired != is.LocomotorDesired
						|| was.LocomotorStart != is.LocomotorStart
						|| was.LocomotorROT != is.LocomotorROT
						|| was.LocomotorTimerStart != is.LocomotorTimerStart
						|| was.LocomotorTimerLeft != is.LocomotorTimerLeft
						|| was.LocomotorTimerRate != is.LocomotorTimerRate)
					{
						Debug::Log("[Replay]   locomotor class %d facing %u->%u start %u ROT %u "
							"timer %d/%d rate %d, was class %d facing %u->%u start %u ROT %u "
							"timer %d/%d rate %d.\n", is.LocomotorResetKind,
							is.LocomotorFacing, is.LocomotorDesired, is.LocomotorStart,
							is.LocomotorROT, is.LocomotorTimerStart, is.LocomotorTimerLeft,
							is.LocomotorTimerRate, was.LocomotorResetKind,
							was.LocomotorFacing, was.LocomotorDesired, was.LocomotorStart,
							was.LocomotorROT, was.LocomotorTimerStart,
							was.LocomotorTimerLeft, was.LocomotorTimerRate);
					}
					if (was.X != is.X || was.Y != is.Y || was.Z != is.Z)
					{
						Debug::Log("[Replay]   at %d/%d/%d, was %d/%d/%d - moved %d/%d/%d differently.\n",
							is.X, is.Y, is.Z, was.X, was.Y, was.Z,
							is.X - was.X, is.Y - was.Y, is.Z - was.Z);
					}
					if (was.Health != is.Health)
						Debug::Log("[Replay]   %d health, was %d.\n", is.Health, was.Health);

					return;
				}
			}

			// Top of the frame, before anything in it has run, so both passes are sampled at the same
			// point.
			void ServiceObjectWatch()
			{
				if (!DiagnosticsWanted())
					return;

				if (WatchedObjectDriftReports >= MaxDriftReports || !ScenarioClass::Instance)
					return;

				const int frame = static_cast<int>(Unsorted::CurrentFrame);
				const auto it = WatchedObjectsByFrame.find(frame);

				if (it != WatchedObjectsByFrame.end())
				{
					std::vector<WatchSample> now;
					SampleObjectWatch(now);

					const bool same = now.size() == it->second.size()
						&& std::equal(now.begin(), now.end(), it->second.begin(), SameWatchSample);

					if (!same)
						ReportObjectDrift(frame, it->second, now);

					return;
				}

				if (WatchedObjectSampleCount >= MaxWatchedObjectSamples)
					return;

				std::vector<WatchSample>& sample = WatchedObjectsByFrame[frame];
				SampleObjectWatch(sample);
				WatchedObjectSampleCount += sample.size();
			}

			#pragma endregion Per-frame object watch

			void RestartDriftReporting()
			{
				ReportedDriftObjects.clear();
				WatchedLayerDriftReports = 0;
				WatchedCellDriftReports = 0;
				WatchedObjectDriftReports = 0;
			}


			void CaptureTechnoSnapshots(std::vector<TechnoSnapshot>& out)
			{
				out.clear();
				out.reserve(static_cast<size_t>(std::max(TechnoClass::Array.Count, 0)));

				for (int i = 0; i < TechnoClass::Array.Count; ++i)
				{
					const auto* const pTechno = TechnoClass::Array.Items[i];
					if (!pTechno)
						continue;

					const auto* const pFoot = abstract_cast<const FootClass*>(pTechno);

					out.push_back(TechnoSnapshot {
						UniqueIDOf(pTechno),
						UniqueIDOf(pTechno->Target),
						UniqueIDOf(pTechno->ArchiveTarget),
						pFoot ? UniqueIDOf(pFoot->Destination) : 0u,
						static_cast<int32_t>(pTechno->CurrentMission),
						static_cast<int32_t>(pTechno->CurrentMissionStartTime),
						static_cast<int32_t>(pTechno->TargetingTimer.StartTime),
						static_cast<int32_t>(pTechno->TargetingTimer.TimeLeft),
						static_cast<int32_t>(pTechno->UpdateTimer.StartTime),
						static_cast<int32_t>(pTechno->UpdateTimer.TimeLeft),
						static_cast<int32_t>(pTechno->MissionStatus),
						static_cast<int32_t>(pTechno->MissionAccumulateTime),
						pFoot ? pFoot->NavQueue.Count : 0,
						pFoot && pFoot->NavQueue.Count > 0 ? UniqueIDOf(pFoot->NavQueue.Items[0]) : 0u
					});
				}
			}

			// Reports what the load changed rather than putting it back: until it is known which of these
			// the savegame is meant to carry and which the engine rebuilds, writing over them would be
			// guesswork. A handful of lines is enough to name the field and the object.
			void ReportTechnoSnapshotDifferences(const Keyframe& keyframe)
			{
				constexpr int MaxReportedTechnoDifferences = 8;

				std::vector<TechnoSnapshot> now;
				CaptureTechnoSnapshots(now);

				std::unordered_map<uint32_t, const TechnoSnapshot*> byId;
				byId.reserve(now.size());
				for (const auto& snapshot : now)
					byId.emplace(snapshot.Id, &snapshot);

				int reported = 0;
				int missing = 0;
				for (const auto& before : keyframe.Technos)
				{
					const auto it = byId.find(before.Id);
					if (it == byId.end())
					{
						++missing;
						continue;
					}

					const TechnoSnapshot& after = *it->second;
					if (before.TargetId == after.TargetId
						&& before.ArchiveTargetId == after.ArchiveTargetId
						&& before.DestinationId == after.DestinationId
						&& before.Mission == after.Mission
						&& before.MissionStartTime == after.MissionStartTime
						&& before.TargetingStart == after.TargetingStart
						&& before.TargetingLeft == after.TargetingLeft
						&& before.MissionTimerStart == after.MissionTimerStart
						&& before.MissionTimerLeft == after.MissionTimerLeft
						&& before.MissionStatus == after.MissionStatus
						&& before.MissionAccumulate == after.MissionAccumulate
						&& before.NavQueueCount == after.NavQueueCount
						&& before.NavQueueHeadId == after.NavQueueHeadId)
					{
						continue;
					}

					if (reported < MaxReportedTechnoDifferences)
					{
						++reported;
						Debug::Log("[Replay] Keyframe %d: techno %u came back from the load with "
							"target %u (was %u), archive target %u (was %u), destination %u (was %u), "
							"mission %d (was %d) started frame %d (was %d), targeting timer %d/%d "
							"(was %d/%d), mission timer %d/%d (was %d/%d), status %d (was %d), "
							"accumulated %d (was %d), %d queued destinations heading for %u "
							"(was %d heading for %u).\n",
							keyframe.Frame, before.Id,
							after.TargetId, before.TargetId,
							after.ArchiveTargetId, before.ArchiveTargetId,
							after.DestinationId, before.DestinationId,
							after.Mission, before.Mission,
							after.MissionStartTime, before.MissionStartTime,
							after.TargetingStart, after.TargetingLeft,
							before.TargetingStart, before.TargetingLeft,
							after.MissionTimerStart, after.MissionTimerLeft,
							before.MissionTimerStart, before.MissionTimerLeft,
							after.MissionStatus, before.MissionStatus,
							after.MissionAccumulate, before.MissionAccumulate,
							after.NavQueueCount, after.NavQueueHeadId,
							before.NavQueueCount, before.NavQueueHeadId);
					}
					else
					{
						++reported;
					}
				}

				if (reported > 0 || missing > 0)
				{
					Debug::Log("[Replay] Keyframe %d: %d of %d technos came back differently, %d went "
						"missing.\n", keyframe.Frame, reported,
						static_cast<int>(keyframe.Technos.size()), missing);
				}
			}

			#pragma region Cell passability

			// CellClass::Passability is the single value the zone builder and the pathfinder work from - the
			// cell's terrain, overlay and occupiers boiled down to one of eight cases, indexed straight into
			// MapClass::MovementAdjustArray. And the savegame does not carry it. The Tiberian Sun source says
			// so in as many words, in the middle of CellClass::Serialize:
			//
			//     // Passability -- no save has ever carried it either; derived from the terrain and whatever
			//     // is standing here.
			//
			// It is meant to be recomputed, by CellClass::Recalc_Passability, whenever something that could
			// block or unblock the cell changes. Nothing recomputes it after a load, so every cell comes back
			// holding whatever its constructor left - Passable, zero - until something happens on that cell to
			// make the engine work it out again.
			//
			// For most of the map that is invisible, because the terrain says the same thing anyway. The
			// border does not: Recalc_Passability's very first act is
			//
			//     if (!Map.In_Local_Radar(CellID)) { Passability = PASSABLE_OUTSIDE; return; }
			//
			// so every cell outside the playable rectangle carries OutsideMap while the game runs, and comes
			// back from a load as Passable. Nearly four thousand of them on the map this was found on. The
			// pathfinder then believes routes may run off the edge of the world, and units near the border
			// take different ones - which is how a keyframe load ends up changing step twenty of a
			// twenty-four step route more than a thousand frames later, without touching the randomiser.
			//
			// The keyframe carries the value rather than recomputing it: what has to be matched is the
			// recording, not what a fresh calculation would arrive at.
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

			// CellClass::Passability is not the only copy. MapClass keeps a second one per playfield cell in
			// LevelAndPassability, alongside that cell's level and the index of the zone it belongs to, and
			// that array is what the zone lookups actually read. MapClass::IsCellInPassableZone (0x56D230)
			// ends with
			//
			//     return MoveZones[mzone][2 * LevelAndPassabilityArray[index].ZoneArrayIndex];
			//
			// indexed by X + Y * (MapRect.Height + MapRect.Width + 1), and FootClass::Basic_Path consults
			// Is_In_Same_Zone before it does anything else. The Tiberian Sun source keeps the two in step by
			// hand - CellClass::Recalc_Attributes ends with
			//
			//     Recalc_Passability();
			//     zone->Height = Height;
			//     zone->Passability = Passability;
			//
			// - so whatever the load does to one it does to the other, and putting the cells back on their own
			// left this array still describing a map whose border was walkable.
			//
			// It travels with the keyframe for the same reason the cell copy does: what has to be matched is
			// the recording, not what a recalculation would arrive at.
			void CaptureZonePassability(std::vector<CellLevelPassabilityStruct>& out)
			{
				out.clear();

				const auto& map = MapClass::Instance;
				const int count = map.ValidMapCellCount;
				if (!map.LevelAndPassability || count <= 0 || count > MapClass::MaxCells)
					return;

				out.assign(map.LevelAndPassability, map.LevelAndPassability + count);
			}

			void RestoreZonePassability(const std::vector<CellLevelPassabilityStruct>& zones, int keyframeFrame)
			{
				if (zones.empty())
					return;

				auto& map = MapClass::Instance;
				const int count = map.ValidMapCellCount;
				if (!map.LevelAndPassability || count <= 0 || static_cast<size_t>(count) != zones.size())
				{
					Debug::Log("[Replay] Keyframe %d holds zone passability for %d cells but the map now has "
						"%d; leaving it as the load built it.\n", keyframeFrame,
						static_cast<int>(zones.size()), count);
					return;
				}

				int passability = 0;
				int level = 0;
				int zoneIndex = 0;

				for (int i = 0; i < count; ++i)
				{
					const CellLevelPassabilityStruct& wanted = zones[static_cast<size_t>(i)];
					CellLevelPassabilityStruct& live = map.LevelAndPassability[i];

					if (live.CellPassability != wanted.CellPassability)
						++passability;
					if (live.CellLevel != wanted.CellLevel)
						++level;
					if (live.ZoneArrayIndex != wanted.ZoneArrayIndex)
						++zoneIndex;

					live = wanted;
				}

				if (passability > 0 || level > 0 || zoneIndex > 0)
				{
					Debug::Log("[Replay] Keyframe %d put back the map's own copy of the zone data: %d cells "
						"differed in passability, %d in level, %d in which zone they belong to.\n",
						keyframeFrame, passability, level, zoneIndex);
				}
				else
				{
					Debug::Log("[Replay] Keyframe %d found the map's copy of the zone data already correct.\n",
						keyframeFrame);
				}
			}

			// The graph's other half is the per-cell array at MapClass+0x70. A hierarchical search
			// starts by reading word_0[level] from the start and destination cells (0x42C34A and
			// 0x42C36E), then uses those values directly as indices into SubzoneTracking[level].
			// Load rebuilds this array and the graph together. Restoring the old graph without restoring
			// these IDs therefore makes every shifted cell point at an unrelated recorded node.
			static_assert(sizeof(LevelAndPassabilityStruct2) == 0x0A);
			static_assert(offsetof(LevelAndPassabilityStruct2, word_0) == 0x00);
			static_assert(offsetof(LevelAndPassabilityStruct2, CellLevel) == 0x08);
			static_assert(offsetof(LevelAndPassabilityStruct2, field_9) == 0x09);

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

			// The last thing a zone lookup reads. MapClass::IsCellInPassableZone finishes with
			//
			//     mov ecx, [edi+68h]         ; LevelAndPassability
			//     mov dx,  [ecx+eax*4+2]     ; that cell's ZoneArrayIndex, a WORD at +2, stride 4
			//     mov ecx, [edi+eax*4+18h]   ; MovementZones[mzone]
			//     mov ax,  [ecx+edx*2]       ; a WORD array indexed by the zone index
			//
			// so there are thirteen WORD tables, one per movement zone, mapping a cell's zone to the movement
			// zone it belongs to for that kind of mover. Nothing states how long they are, but it does not
			// matter: the only entries the game can reach are the ones some cell's ZoneArrayIndex names, and
			// LevelAndPassability holds every one of those.
			//
			// Cell passability and the map's own copy of it both come back correct now, so if two runs still
			// disagree about a route, this is where the disagreement would live - Basic_Path asks
			// Is_In_Same_Zone before it does anything else, and this is what that question resolves to.
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

			void CaptureMovementZones(std::array<std::vector<uint16_t>, 13>& out)
			{
				for (auto& table : out)
					table.clear();

				const int highest = HighestZoneArrayIndex();
				if (highest < 0)
					return;

				const auto entries = static_cast<size_t>(highest) + 1u;
				for (size_t zone = 0; zone < out.size(); ++zone)
				{
					if (const auto* const pTable = static_cast<const uint16_t*>(MapClass::Instance.MovementZones[zone]))
						out[zone].assign(pTable, pTable + entries);
				}
			}

			void RestoreMovementZones(const std::array<std::vector<uint16_t>, 13>& zones, int keyframeFrame)
			{
				const int highest = HighestZoneArrayIndex();
				if (highest < 0)
					return;

				const auto entries = static_cast<size_t>(highest) + 1u;
				int differed = 0;
				int compared = 0;

				for (size_t zone = 0; zone < zones.size(); ++zone)
				{
					const std::vector<uint16_t>& wanted = zones[zone];
					auto* const pTable = static_cast<uint16_t*>(MapClass::Instance.MovementZones[zone]);
					if (wanted.size() != entries || !pTable)
						continue;

					for (size_t i = 0; i < entries; ++i)
					{
						++compared;
						if (pTable[i] == wanted[i])
							continue;

						++differed;
						pTable[i] = wanted[i];
					}
				}

				if (compared == 0)
					return;

				if (differed > 0)
				{
					Debug::Log("[Replay] Keyframe %d put back the movement zone tables: %d of %d entries "
						"differed across %d zones of %d.\n", keyframeFrame, differed, compared,
						static_cast<int>(entries), static_cast<int>(zones.size()));
				}
				else
				{
					Debug::Log("[Replay] Keyframe %d found the movement zone tables already correct (%d "
						"entries).\n", keyframeFrame, compared);
				}
			}

			// A third copy of passability, and the one that decides the shape of a long route rather than its
			// first step. Above the cell grid the map keeps a subzone graph, and the pathfinder searches that
			// first to get a corridor which the cell-level search is then confined to. Each subzone carries
			// its own passability, and the corridor search both scores and gates on it:
			//
			//     PassabilityType passability = Map.SubzoneTracking[level][to_subzone].Passability;
			//     static const float _passability_scores[] = {1.0, 0.0, 0.0, 1.0, 0.0, 1.0, 1.0};
			//     float score = _passability_scores[passability] + best_node->Score + threat + extra;
			//     ... && pass_table[passability] == TRAVERSAL_PASSABLE
			//
			// So a subzone whose passability came back wrong does not send a unit off in the wrong direction -
			// it bends the corridor somewhere further along, and the route differs in the middle or near its
			// end while its first steps stay identical. A twenty-four step route differing at step twenty is
			// what that looks like.
			//
			// RA2's builder (0x581F90) pins these layouts: it advances outer entries by 36 bytes and
			// connections by 8, writes the parent at +0x18, passability at +0x1C and threat-region
			// index at +0x20. OpenTS gives the fields the names used here; the RA2 binary remains the
			// authority for their offsets and ownership.
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

				// AStarClass::Reset_Subzone_Tables (0x42C1C0) sizes three visit/cost arrays from
				// MapClass's three entry counters. Load already called it for the smaller graph it built;
				// call it again only after both the counters and graph have their recorded sizes.
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

					// The engine keeps the span of pool entries its heap is holding; it is written the same
					// way here so it still describes the heap that is now in place. Its own two names for
					// these are the wrong way round.
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

				for (int i = 0; i < typeCount; ++i)
				{
					auto* const pTiberium = TiberiumClass::Array.Items[i];
					if (!pTiberium)
						continue;

					for (int kind = 0; kind < TiberiumQueueCount; ++kind)
					{
						const TiberiumQueueSnapshot& queue =
							snapshot.Queues[static_cast<size_t>(i)][static_cast<size_t>(kind)];
						if (!queue.Present)
							continue;

						if (RestoreTiberiumQueue(TiberiumLogicOf(pTiberium, kind), surfaceCount, queue))
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

				return true;
			}

			#pragma endregion Ore growth and spread queues

			bool CaptureKeyframeState(int frame, Keyframe& keyframe)
			{
				if (!ScenarioClass::Instance)
					return false;

				keyframe.Frame = frame;
				keyframe.ScenarioUniqueID = ScenarioClass::Instance->UniqueID;

				// Copied whole rather than field by field. ScenarioClass::Load reads the saved struct
				// back and then runs ScenarioClass::ScenarioClass (0x683560) over the top of it, whose
				// first act is Random2Class::Random2Class(&RandomNumber, 0) - so every savegame load
				// re-seeds the synchronised randomiser to a fixed seed and throws away what the file
				// held. Putting it back is not optional, and it has to be all of it: Compute_Game_CRC
				// (0x64DAB0) draws from this randomiser as its last act, so the frame hash is part of
				// the same stream the simulation draws from.
				memcpy(keyframe.Random.data(), &ScenarioClass::Instance->Random, sizeof(Randomizer));

				CaptureTechnoSnapshots(keyframe.Technos);
				if (!CapturePlanningState(keyframe.Planning))
					return false;
				CaptureLocomotorResetStates(keyframe.LocomotorResetStates);
				CaptureTiberiumState(keyframe.Tiberium);
				CaptureCellPassability(keyframe.CellPassability);
				CaptureZonePassability(keyframe.ZonePassability);
				if (!CaptureCellSubzones(keyframe.CellSubzones))
					return false;
				CaptureMovementZones(keyframe.MovementZones);
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
					if (!RestoreObjectOrder(collection, order, name, changed))
						return false;

					if (changed)
					{
						++reorderedCollectionCount;
						Debug::Log("[Replay] %s came back from the load out of order; put back.\n", name);
					}
					return true;
				};

				// A collection that cannot be put back in order is worth saying so about, but it is not
				// worth abandoning the seek over: the rest of the state is still sound and a shuffled
				// array is a smaller problem than dumping the viewer back to no playback at all.
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
				if (!RestoreLocomotorResetStates(keyframe.LocomotorResetStates, keyframe.Frame))
					return false;
				RestoreTiberiumState(keyframe.Tiberium, keyframe.Frame);
				RestoreCellPassability(keyframe.CellPassability, keyframe.Frame);
				RestoreZonePassability(keyframe.ZonePassability, keyframe.Frame);
				if (!RestoreCellSubzones(keyframe.CellSubzones, keyframe.Frame))
					return false;
				RestoreMovementZones(keyframe.MovementZones, keyframe.Frame);
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

				Keyframe keyframe;
				if (!CaptureKeyframeState(frame, keyframe))
				{
					Debug::Log("[Replay] Could not capture simulation state for keyframe %d.\n", frame);
					return false;
				}

				if (!ScenarioClass::SaveGame(fileName, description))
				{
					Debug::Log("[Replay] Failed to write the keyframe for frame %d.\n", frame);
					return false;
				}

				State.Keyframes.push_back(std::move(keyframe));
				return true;
			}

			// Everything a load leaves in a state playback cannot use: the frame counter it may or
			// may not have restored, the simulation speed the in-game teardown resets on the way
			// through, and the frame stream, which is still sitting wherever the seek left it.
			// Decode_All_Pointers runs Allocate_Surfaces (0x533FD0), which deletes and reallocates
			// CompositeSurface, TileSurface, SidebarSurface, HiddenSurface and AlternateSurface and
			// updates each of those globals - but not DSurface::Temp (0x887314), which is an alias
			// that WWMouseClass::PrepareScreen normally points at the hidden surface. After a load it
			// is left pointing at freed memory.
			//
			// Nothing in the frame notices, because both places that use it (GScreenClass::Render and
			// GScreenClass::Input) overwrite it before reading it. Ares does notice: the routine it
			// runs before opening an in-game dialog walks every surface global and calls a virtual on
			// each non-null one, so pressing Escape after a load faulted in Ares.dll at +0x6258A -
			// call [eax+18h] with ecx loaded from 0x887314.
			// LoadOptionsClass::Load_File runs SessionClass's in-game teardown (0x69BB40) on the way
			// into the load, which clears the flag at SessionClass+0x30D8 that says the game is in
			// play, releases and hides the mouse and drops the audio level. The engine normally sets
			// all of that back up on the way out of the load screen; a seek never goes through one.
			//
			// Leaving the flag clear is what made Escape crash: the options dialog asks for it
			// (0x69BBE0) and lays itself out for the shell when it reads false, which dereferences a
			// menu-only shape pointer that is null in game - the fault at 0x60B3B5, movsx from
			// [0xB0FAC4]. 0x69BAB0 is the engine's own counterpart to the teardown.
			void ResumeInGameSessionAfterLoad()
			{
				reinterpret_cast<void(__thiscall*)(SessionClass*)>(0x69BAB0)(&SessionClass::Instance);
			}

			#pragma region Planning tokens

			// Planning tokens are not AbstractClass objects, so the game's save stream never sees
			// them. A node can be shared by several tokens, though, which means a per-unit copy is
			// subtly wrong. The keyframe stores the graph once and tokens refer to nodes by index.
			// The planning EventClass packets contain TargetClass IDs and scalar command data, so
			// copying their 111 bytes preserves commands without carrying world addresses across a load.
			DynamicVectorClass<TechnoClass*>& ActivePlanningRouteOwners()
			{
				return *reinterpret_cast<DynamicVectorClass<TechnoClass*>*>(0xAC4C40);
			}

			int* PlanningHouseRouteCounts()
			{
				return reinterpret_cast<int*>(0xAC4B84);
			}

			DynamicVectorClass<EventClass*>& PendingPlanningEvents()
			{
				return *reinterpret_cast<DynamicVectorClass<EventClass*>*>(0xAC4B48);
			}

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

				const auto& pendingEvents = PendingPlanningEvents();
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

				const auto& activeOwners = ActivePlanningRouteOwners();
				snapshot.ActiveRouteOwners.reserve(static_cast<size_t>(std::max(activeOwners.Count, 0)));
				for (int i = 0; i < activeOwners.Count; ++i)
					snapshot.ActiveRouteOwners.push_back(UniqueIDOf(activeOwners.Items[i]));

				memcpy(snapshot.HouseRouteCounts.data(), PlanningHouseRouteCounts(),
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

				// PlannedEvents (0xAC4B48) is not cleared by either Clear_Scenario's planning
				// reset or the savegame loader. Without replacing it here a backward seek retains
				// commands from the future, while a command pending at the keyframe is absent.
				// Both cases leave field_1C set on the wrong tokens and eventually skip or stall a
				// waypoint.
				auto& pendingEvents = PendingPlanningEvents();
				for (int i = 0; i < pendingEvents.Count; ++i)
				{
					if (pendingEvents.Items[i])
						YRMemory::Deallocate(pendingEvents.Items[i]);
				}
				pendingEvents.Count = 0;

				// Clear_Scenario normally did this during the load. Repeating the manager's own reset
				// here gives reconstruction a defined empty base and also discards any unexpected
				// planning objects an extension may have created while loading.
				reinterpret_cast<void(__cdecl*)()>(0x6370B0)();
				for (auto& pair : technoById)
					pair.second->PlanningToken = nullptr;

				using NodeCtor = PlanningNodeClass* (__thiscall*)(PlanningNodeClass*, int);
				using TokenCtor = PlanningTokenClass* (__thiscall*)(PlanningTokenClass*, TechnoClass*);
				const auto constructNode = reinterpret_cast<NodeCtor>(0x633CC0);
				const auto constructToken = reinterpret_cast<TokenCtor>(0x635F20);

				std::vector<PlanningNodeClass*> nodes;
				nodes.reserve(snapshot.Nodes.size());
				size_t memberCount = 0;

				for (const auto& savedNode : snapshot.Nodes)
				{
					auto* const memory = static_cast<PlanningNodeClass*>(
						YRMemory::AllocateChecked(sizeof(PlanningNodeClass)));
					auto* const pNode = constructNode(memory, savedNode.Field18);
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
					auto* const pToken = constructToken(memory, pOwner);

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

				auto& activeOwners = ActivePlanningRouteOwners();
				for (uint32_t ownerId : snapshot.ActiveRouteOwners)
				{
					if (!activeOwners.AddItem(findTechno(ownerId)))
						return false;
				}
				memcpy(PlanningHouseRouteCounts(), snapshot.HouseRouteCounts.data(),
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

			bool RestorePlaybackAfterLoad(const Keyframe& keyframe)
			{
				RestartDriftReporting();

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

				ReportTechnoSnapshotDifferences(keyframe);

				RepointTempSurfaceAfterLoad();
				ResumeInGameSessionAfterLoad();

				// Both the teardown and the resume above have their own opinion about the game speed -
				// the teardown puts back whatever a live game last used, and the resume forces 2 for a
				// campaign - so the pin to the recorded speed has to come after both of them.
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

				State.LoadInProgress = true;
				// LoadOptionsClass::LoadMission is the engine's own in-game load: it stops the
				// sounds and the movies, tears the world down and rebuilds it from the file. Going
				// through it rather than Load_Game keeps the mouse and dialog bookkeeping that the
				// engine does around a load.
				const bool loaded = LoadOptionsClass::LoadMission(fileName);
				State.LoadInProgress = false;

				if (!loaded)
				{
					Debug::Log("[Replay] Failed to load the keyframe for frame %d.\n", keyframe.Frame);
					return false;
				}

				return RestorePlaybackAfterLoad(keyframe);
			}

			void EndSeek()
			{
				if (!State.Seeking)
					return;

				State.Seeking = false;
				State.TargetFrame = -1;
				State.FramesSinceRender = 0;

				VocAllowed() = State.VocAllowedBeforeSeek;

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
					State.VocAllowedBeforeSeek = VocAllowed();
				}

				State.ResumePaused = State.ResumePaused || pauseOnArrival;

				State.Seeking = true;
				State.TargetFrame = targetFrame;
				State.FramesSinceRender = 0;

				// Frames run back to back with no pacing while this holds, so the sound effects of
				// every one of them would land at once.
				VocAllowed() = false;
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
			ResetObjectWatch();
			ResetCellWatch();
			ResetLayerWatch();
			State = SeekState {};

			const auto* pConfig = GetConfig();
			State.Interval = pConfig ? std::max(0, pConfig->ReplayKeyframeInterval) : 0;

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
				VocAllowed() = State.VocAllowedBeforeSeek;

			ResetObjectWatch();
			ResetCellWatch();
			ResetLayerWatch();
			State = SeekState {};
		}

		void ServiceFrameStart()
		{
			if (!ReplayState.Playback)
				return;

			if (State.LoadPending)
			{
				Keyframe keyframe = std::move(State.PendingLoadKeyframe);
				State.LoadPending = false;
				State.PendingLoadKeyframe = {};

				if (!LoadKeyframe(keyframe))
				{
					// Playback is now sitting on a state that does not match the stream, so there
					// is nothing sensible to carry on from.
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

			ServiceObjectWatch();
			ServiceCellWatch();
			ServiceLayerWatch();

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

			// The keyframe to start from is the newest one at or before the target, whichever way
			// the seek is going. Backwards it is the only option, because a frame cannot be run in
			// reverse. Forwards it is still worth loading whenever it lands further along than
			// playback already is: loading one state beats simulating the thousands of frames that
			// state stands for, leaving only the remainder after it to run.
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
				State.PendingLoadKeyframe = *keyframe;
			}

			return true;
		}

		bool IsSeeking()
		{
			return State.Seeking;
		}

		int SeekTargetFrame()
		{
			return State.Seeking ? State.TargetFrame : -1;
		}

		bool ShouldSkipRenderThisFrame()
		{
			return State.Seeking && State.FramesSinceRender < SeekRenderInterval;
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
