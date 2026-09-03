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
#include <BulletClass.h>
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

			// What a techno was doing when the keyframe was taken. Most of this remains diagnostic:
			// it is checked after the load to catch per-object state that the savegame did not carry
			// across. IsInPlayfield is the one confirmed exception and is restored below.
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
				// The mission an object has been told to take up next, and the one it will go back to.
				// MissionClass::NextMission pops the queued one and calls Assign_Mission, which resets
				// the status to zero and stamps the current frame into MissionStartTime - so a queued
				// mission that arrives one frame late puts the object a whole step behind for the rest
				// of its life. That is exactly what a V3 rocket did at keyframe 36000, and neither
				// field was being looked at: the load-time comparison was clean and the frame after it
				// was not.
				int32_t QueuedMission;
				int32_t SuspendedMission;
				// The queued movement destinations behind the first one - what a player builds up by
				// shift-clicking a route. FootClass::Mission_Guard_Area reads the count and the head of
				// this queue and can hand the object a new destination off the back of it, on a path that
				// draws no randomness at all. So a queue that does not survive the load shifts what the
				// object does without moving the randomiser, which is why nothing noticed for hundreds of
				// frames.
				int32_t NavQueueCount;
				uint32_t NavQueueHeadId;
				bool IsALoaner;
				bool IsInPlayfield;
				uint32_t TeamId;
				bool TeamLeavingMap;
			};

			// HouseClass::Repairing is HouseClass::DidRepair in the Tiberian Sun source: a
			// one-repair-per-house gate which BuildingClass::Repair_AI tests before its random
			// repair-delay draw. The frame-8251 trace found that exact draw appearing only after
			// a keyframe load, so keep the gate and its timer beside the keyframe to determine
			// whether the load changed the house or one of the building-side conditions.
			struct HouseRepairSnapshot
			{
				uint32_t Id;
				bool DidRepair;
				int32_t RepairTimerStart;
				int32_t RepairTimerLeft;
			};

			// The team state both production rewrites read before they decide anything. Their first
			// loop walks TeamClass::Array and, for every team that is neither a satisfied reinforcement
			// nor already active-and-been, counts its missing task force members and remembers the
			// earliest CreationFrame per unit type. Those two arrays are what the FillEarliestTeamProbability
			// roll then chooses between - and whether there is anything to choose between at all, which
			// decides whether a second draw happens.
			//
			// The array's order is already restored with the other collections. Its contents are not
			// checked at all, and these four flags plus the creation frame are the whole input.
			struct TeamSnapshot
			{
				uint32_t Id;
				uint32_t OwnerId;
				uint32_t TypeIndex;
				int32_t CreationFrame;
				int32_t TotalObjects;
				bool IsForcedActive;
				bool IsHasBeen;
				bool IsFullStrength;
				bool IsUnderStrength;
			};

			// What the AI has already decided to build, and the factories that decision hangs off.
			//
			// Both Ares and Phobos replace the engine's production picker with the same rewrite -
			// Ares in Ext/House/MacroHacks.h, Phobos in HouseExt::UpdateVehicleProduction, which
			// says outright that it is "based on Ares' rewrite of 0x4FEA60". Each begins by asking
			// whether the house already has a choice outstanding:
			//
			//     const bool skipGround = pThis->ProducingUnitTypeIndex != -1;
			//
			// and only rolls FillEarliestTeamProbability against the randomiser when it does not.
			// So these four indices do not merely record a decision - they decide whether a draw
			// happens at all. A keyframe that brings one of them back as -1 where it was set, or
			// the other way round, moves the randomiser out of step on the first frame the house
			// thinks about production, with nothing else visibly wrong.
			//
			// That is the shape the Boot Camp seek trace found: on the first pass Phobos drew at
			// this point, and after the load Ares drew instead, at the same cursor.
			struct HouseProductionSnapshot
			{
				uint32_t Id;
				int32_t ProducingBuildingTypeIndex;
				int32_t ProducingUnitTypeIndex;
				int32_t ProducingInfantryTypeIndex;
				int32_t ProducingAircraftTypeIndex;
				// The primaries the indices are consumed by. A factory that comes back as a
				// different object, or holding a different item, releases the index on a different
				// frame even when the index itself survived.
				uint32_t PrimaryForBuildings;
				uint32_t PrimaryForVehicles;
				uint32_t PrimaryForShips;
				uint32_t PrimaryForInfantry;
				uint32_t PrimaryForAircraft;
				int32_t VehicleFactoryProgress;
				uint32_t VehicleFactoryObject;
				int32_t VehicleFactoryQueued;
				// The whole gate on HouseExt::UpdateHarvesterProduction, which returns before any of
				// the production rolls happen and quietly assigns ProducingUnitTypeIndex on its way
				// out:
				//
				//     maxHarvesters = FindBuildable(BuildRefinery)
				//         ? HarvestersPerRefinery[AIDifficulty] * pThis->CountResourceDestinations
				//         : AISlaveMinerNumber[AIDifficulty];
				//     if (IQLevel2 >= Rules->Harvester && !IsTiberiumShort && !IsControlledByHuman()
				//         && CountResourceGatherers < maxHarvesters && TechLevel >= ...)
				//
				// Both counters are incrementally maintained tallies rather than anything derived on
				// demand, which is the shape of state a load either carries or silently rebuilds.
				int32_t CountResourceGatherers;
				int32_t CountResourceDestinations;
				int32_t TechLevel;
				int32_t IQLevel2;
				bool IsTiberiumShort;
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

			// Kamikaze::Save (0x54E750) writes the node count and then the eight bytes of each node,
			// and stops. UpdateTimer is not in the save format at all - there is no version of a YR
			// savegame that carries it - so a load leaves the tracker holding whatever phase the
			// world it was loaded over happened to be in.
			//
			// Kamikaze::Update (0x54E4D0) is gated on that timer, re-arms it for thirty frames when
			// it fires, and gives every tracked aircraft MISSION_ATTACK. A V3 rocket is a missile
			// spawn and is tracked, so the whole batch takes its attack mission on one frame in
			// thirty - and after a seek that frame is a different one. Five rockets were assigned a
			// frame late at keyframe 36000, and one of them then ran a mission step behind for the
			// rest of its life.
			//
			// The nodes themselves are in the save and come back swizzled. They are kept here as
			// unique IDs anyway, to check rather than to restore: Kamikaze::Load appends to the
			// vector without clearing it first, which is only safe if something else empties it, and
			// a silent duplicate would look exactly like this bug.
			struct KamikazeSnapshot
			{
				int32_t TimerStart = -1;
				int32_t TimerLeft = 0;
				std::vector<uint32_t> Aircraft;
				bool operator==(const KamikazeSnapshot&) const = default;
			};

			void CaptureKamikazeState(KamikazeSnapshot& snapshot);
			void RestoreKamikazeState(const KamikazeSnapshot& snapshot, int keyframeFrame);

			struct KeyframeObjectName
			{
				uint32_t Id = 0;
				int32_t Type = 0;
				std::array<char, 24> TypeId {};
			};

			// The watches' share of the diagnostic budget. Defined below the stores it reclaims
			// from; declared here because the stores charge against it before that point. See the
			// budget itself for why the watches no longer share one pool with the traces.
			bool ChargeWatchMemory(size_t bytes);
			bool ChargeCellBaselineMemory(size_t bytes);
			void ResetWatchMemory();

			// Ares 3.0p1 replaces several particle-system behaviours and keeps their live
			// particles in extension-owned std::vectors. Those vectors are gameplay state:
			// the smoke handler draws once per entry on odd frames before it does anything
			// else. A load which returns a different vector therefore changes the random
			// stream immediately, even though the vanilla ParticleSystemClass is identical.
			//
			// Keep the records as opaque 44-byte values, exactly as Ares streams them. The
			// one pointer in a draw record is replaced by its ParticleTypeClass array index,
			// so the sidecar never preserves an address from the pre-load object graph.
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

			// A CDTimerClass is three words - the frame it was started on, an unused slot, and how long
			// it has to run - and every one of them is read the same way:
			//
			//     if (StartTime != -1 && Frame - StartTime >= TimeLeft) fire;
			//     if (TimeLeft == 0)                                    fire;
			//
			// so a timer holding { Frame, 0 } fires on the next frame it is asked, and keeps firing. Four
			// of the engine's own Load overrides write exactly that over the value they just read out of
			// the savegame, through CDTimerClass::operator= (0x46B640) which is { Frame, argument }:
			//
			//     TiberiumClass::Load     (0x721E80) - both ore timers; handled with the ore queues
			//     SlaveManagerClass::Load (0x6B1170) - the ten-frame gate on all slave logic
			//     SpawnManagerClass::Load (0x6B7F10) - the update gate and the spawn timer
			//     BulletClass::Load       (0x46AE70) - a projectile's two flight timers
			//
			// In a normal game none of this shows: a load is followed by whatever the player does next,
			// and one early tick of the ore or the slaves is invisible. In a replay it is a guaranteed
			// divergence on the first frame after any seek. SlaveManagerClass::AI (0x6AF5F0) runs its
			// slaves once every ten frames and does nothing at all in between, so a manager whose gate has
			// been reset is running on a different frame from the recording for the rest of the replay -
			// its slaves scan for ore at different moments, pick different cells, take different paths and
			// finish standing somewhere else. That is why seeking worked in the opening minutes and
			// stopped working once there was a slave miner on the map.
			//
			// Two timers to an entry because that is the most any of them carries; an entry using one
			// leaves the second stopped and is never asked about it.
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

			// Slave miners get more than their gate timer put back. Restoring the gate alone fixed which
			// frames SlaveManagerClass::AI ran on and the runs still parted company on the first replayed
			// frame - the same ore cell being handed to a different slave, the same code assigning the same
			// mission to a different object. That is not a question of when the manager runs but of what it
			// finds when it does, so the whole of it is carried: the manager's own status and last scan, and
			// every slave control in vector order with the slave it holds, the state machine position that
			// decides which branch of Slave_AI (0x6AF6C0) it takes, and its respawn timer.
			//
			// Order matters as much as content. Slave_AI walks the controls from the front and the first one
			// that wants an ore cell gets it, so two controls swapping places is a different pair of slaves
			// doing each other's work - which is exactly what the divergence looked like.
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

			// Two pieces of derived map state the keyframe checks rather than carries.
			//
			// Both were reported already correct after every load ever examined, before and after the
			// engine-side passability work, so copying them into each keyframe was three quarters of a
			// megabyte of writing and retention to put back values that already matched. A hash costs
			// eight bytes and one read-only pass and still catches it if that ever stops being true.
			//
			// The cells themselves are a different matter and are still carried; see CaptureCellPassability.
			struct DerivedMapHashes
			{
				bool Present = false;
				uint32_t ZonePassability = 0;
				uint32_t MovementZones = 0;
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
				std::vector<HouseRepairSnapshot> HouseRepairs;
				std::vector<HouseProductionSnapshot> HouseProduction;
				std::vector<TeamSnapshot> Teams;
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
				// What each object in AbstractClass::Array was, beside the order itself. When the load
				// brings back fewer objects than the keyframe held - which is a whole class of bug on
				// its own, not an ordering problem - the ones that are gone cannot be asked what they
				// were, because they are gone. So they are written down while they are still here.
				std::vector<KeyframeObjectName> AbstractObjects;
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

			#pragma region Ares particle-system state

			// Reversed from the supplied Ares 3.0p1 build. Do not use std::vector here:
			// Ares and the spawner both link the CRT statically, so storage allocated by
			// one DLL must be released by that same DLL.
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
						// mov ebx, [esp+10h] - the ParticleSystemClass argument, which sits there after the
						// sub esp, 8 and the push ebx. This byte read 0x0C for a while, which is the return
						// address rather than the argument, and no build ever matched: the capture below then
						// returned early leaving Captured false, and every keyframe restored nothing at all.
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

			// Rebuild a collection's pointer order, as far as the two sides have objects in common.
			// Matching by UniqueID is stable across a save/load because each object's raw Load restores
			// that ID.
			//
			// It used to give up entirely the moment the counts disagreed, which turned a small loss into
			// a large one. OverlayClass is not in the savegame object graph at all - there is no class
			// factory for it - so every crate on the map loses its wrapper object across any YR load, and
			// two crates were enough to make the whole of AbstractClass::Array come back in the load's own
			// order rather than the recording's. The crates themselves are fine: the cell keeps the overlay
			// bytes that Goodie_Check actually reads. The ordering was not.
			//
			// So the order is put back for everything that survived, in the sequence the keyframe held it,
			// and anything the load produced that the keyframe never had is left after them in the order it
			// arrived. A difference either way is still reported and still returns false, because it is
			// still worth knowing about - it just no longer costs the ordering as well.
			template <typename TCollection>
			bool RestoreObjectOrder(TCollection& collection, const std::vector<uint32_t>& savedOrder,
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
					return true;

				// Printed signed. An object whose identity a load threw away reads -1, and -1 through %u
				// is 4294967295 - a number that looks like a plausible id and is not one, which is exactly
				// how the SuperClass report read until it was chased down.
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

				return false;
			}

			// The three watchers below walk every techno, every layer object and every cell on the map,
			// every frame, and keep what they find for the whole replay. That is the right trade while
			// hunting a divergence and completely the wrong one while watching a replay, so they are off
			// unless ReplayDiagnostics asks for them.
			bool DiagnosticsWanted()
			{
				return ReplayState.DiagnosticsEnabled;
			}

			// The watches used to fall silent after a single report, which cost a round of this: an inert
			// difference ten frames after one load silenced them for a second load two thousand frames
			// later, where the real divergence was. They now report a few times, and every load starts
			// them over so each seek is diagnosed on its own.
			constexpr int MaxDriftReports = 8;

			// A frame of object samples is a sample of every techno, so the watches can only afford a
			// few hundred frames at a time. Spending that on the last few hundred frames played was
			// the obvious thing and the wrong one: a seek lands on a keyframe, keyframes are hundreds
			// or thousands of frames back, and the window had always forgotten the one it landed on.
			// One log had the watches holding frames 27617 onwards while the seek loaded keyframe
			// 27000, so every watch was blind to the load and the first frame it could see was
			// reported as the first frame anything went wrong.
			//
			// A load's damage shows immediately - the census caught keyframe 32250 on the load frame
			// itself - so what is worth keeping is a short block after each keyframe rather than a
			// long run of whatever came last. Recording only inside those blocks makes each one cheap
			// enough that the budget holds several keyframes' worth, and any backwards seek lands on
			// one of them.
			constexpr int WatchFramesAfterKeyframe = 100;

			bool FrameIsWorthWatching(int frame)
			{
				if (State.Interval <= 0)
					return true;

				if (frame - FirstKeyframeFrame >= 0 && frame - FirstKeyframeFrame < WatchFramesAfterKeyframe)
					return true;

				return frame % State.Interval < WatchFramesAfterKeyframe;
			}

			// An object is worth reporting once per load, not once per frame for as long as it stays
			// different.
			std::unordered_set<uint32_t> ReportedDriftObjects;

			// The keyframe the last load came from, so a watch can say whether it was there for it.
			int LastLoadedKeyframeFrame = -1;

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

				if (!ScenarioClass::Instance)
					return;

				const int frame = static_cast<int>(Unsorted::CurrentFrame);
				const auto it = WatchedLayersByFrame.find(frame);

				if (it != WatchedLayersByFrame.end())
				{
					std::vector<LayerSample> now;
					SampleLayerObjects(now);

					if (now != it->second && WatchedLayerDriftReports < MaxDriftReports)
						ReportLayerDrift(frame, it->second, now);

					return;
				}

				if (WatchedLayerSampleCount >= MaxWatchedLayerSamples || !FrameIsWorthWatching(frame))
					return;

				std::vector<LayerSample> sample;
				SampleLayerObjects(sample);
				if (!ChargeWatchMemory(sample.size() * sizeof(LayerSample)
					+ sizeof(std::vector<LayerSample>)))
				{
					return;
				}

				WatchedLayerSampleCount += sample.size();
				WatchedLayersByFrame[frame] = std::move(sample);
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
			// Set by a keyframe load: the hashes below it are from the world the seek left.
			bool CellDeltaBaselineStale = false;
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

				if (!ScenarioClass::Instance || !MapClass::Instance.Cells.Items)
					return;

				const int frame = static_cast<int>(Unsorted::CurrentFrame);

				std::vector<uint32_t> now;
				HashAllCells(now);

				// The frame a load lands on has nothing to measure a per-frame change against: the
				// hashes it would compare with are the ones the seek left behind. Take the baseline and
				// leave the frame alone; the whole-table comparison below still runs, and that is the
				// one that can say what the load itself did to a cell.
				if (CellDeltaBaselineStale)
				{
					CellDeltaBaselineStale = false;

					if (const auto baselineOnly = CellBaselines.find(frame);
						baselineOnly != CellBaselines.end())
					{
						SampleAllCells();
						if (baselineOnly->second.size() == CellScratch.size())
							ReportLoadCellDrift(frame, baselineOnly->second);
					}

					LiveCellHashes = std::move(now);
					return;
				}

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
					if (changes->second != delta && WatchedCellDriftReports < MaxDriftReports)
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
				else if (WatchedCellChangeCount < MaxWatchedCellChanges && FrameIsWorthWatching(frame))
				{
					// A whole cell table is 0x40000 snapshots of a hundred bytes - 25MB - kept for the
					// life of the replay, once per keyframe. Twenty-seven keyframes of that is 675MB in a
					// 32-bit process, and it was charged against nothing: it is what ran the game out of
					// address space. It is the only thing that can say what a cell held before a load as
					// well as after, so it is paid for rather than dropped - out of its own small budget,
					// which keeps the newest baselines and forgets the oldest. Sharing the watches' pool
					// kept the first few keyframes instead, and the keyframe a seek lands on is always
					// one of the last.
					if (State.Interval > 0 && frame % State.Interval == 0
						&& ChargeCellBaselineMemory(
							static_cast<size_t>(std::max(WatchedCellCount(), 0)) * sizeof(CellSnapshot)))
					{
						SampleAllCells();
						CellBaselines[frame] = CellScratch;
					}

					if (ChargeWatchMemory(delta.size() * sizeof(CellChange)
						+ sizeof(std::vector<CellChange>)))
					{
						WatchedCellChangeCount += delta.size();
						CellChangesByFrame[frame] = std::move(delta);
					}
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
			// Each sample now carries the locomotor bytes, so the cap comes down to keep the total
			// bounded in a 32-bit process.
			constexpr size_t MaxWatchedObjectSamples = 200000;

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
				bool IsALoaner;
				bool IsInPlayfield;
				uint32_t TeamId;
				bool TeamLeavingMap;
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
				// The locomotor is a COM sub-object with its own state, and none of the fields above
				// reach into it beyond the four value members whose Load methods are known to reset
				// themselves. The spy plane at frame 7508 drew six random numbers in its update the
				// first time round and none the second with every watched field identical, which is
				// what state living somewhere unwatched looks like. LocomotionClass::Load reads
				// Size_Of() bytes, so that is what is hashed - the whole thing, whatever it is.
				// Kept as bytes rather than a hash: every locomotor in the game came back different
				// after a load, and a hash can only say that, not which field. Capped at the largest
				// locomotor seen so far (112 bytes for the drive one).
				// RadioClass::Has_Contact_Index decides whether an aircraft docks with the building it
				// is over or goes looking for somewhere else to be. At frame 7508 both runs asked it
				// the same question about the same building and only one went on to the landing zone
				// scan, so the answer differed - and nothing here has ever looked at the links it
				// answers from.
				// Phobos decides an object should vanish from TechnoExt::UpdateAutoDeath, and its first
				// condition is ammunition:
				//
				//     if (pTypeExt->OwnerObject()->Ammo > 0 && pThis->Ammo <= 0
				//         && pTypeExt->AutoDeath_OnAmmoDepletion)
				//         TechnoExt::KillSelf(pThis, howToDie, ...);
				//
				// which is Stun, Limbo, RegisterKill, UnInit - the exact chain the spy plane goes
				// through on the pass where it dies. Ammo has never been sampled here.
				int32_t Ammo = 0;
				int32_t RadioLinkCount = 0;
				std::array<uint32_t, 8> RadioLinks {};
				int32_t LocomotorSize = 0;
				std::array<unsigned char, 0x70> LocomotorBytes {};
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
						static_cast<int32_t>(pTechno->Health),
						pTechno->IsALoaner,
						pTechno->IsInPlayfield,
						pFoot ? UniqueIDOf(pFoot->Team) : 0u,
						pFoot && pFoot->Team ? pFoot->Team->IsLeavingMap : false
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

					sample.Ammo = pTechno->Ammo;
					sample.RadioLinkCount = pTechno->RadioLinks.Capacity;
					for (int link = 0; link < pTechno->RadioLinks.Capacity
						&& link < static_cast<int>(sample.RadioLinks.size()); ++link)
					{
						sample.RadioLinks[static_cast<size_t>(link)] =
							UniqueIDOf(pTechno->RadioLinks[link]);
					}

						if (pFoot->Locomotor)
						{
							ILocomotion* const pInterface = pFoot->Locomotor.GetInterfacePtr();

							// LocomotionClass::Load (0x55AAC0) reads Size_Of() bytes over the object, so that
							// is exactly the state a load is meant to bring back. Hashing all of it means no
							// locomotor can differ without the watch noticing, whatever kind it is.
							// static_cast, never reinterpret_cast: LocomotionClass inherits IPersistStream and
							// ILocomotion both, so an ILocomotion* points at the second subobject and needs the
							// offset applied. Reinterpreting it and calling a virtual went through the wrong
							// vtable and took the game down at 0x4AFFC0 with a null this.
							if (auto* const pLocomotion = static_cast<LocomotionClass*>(pInterface))
							{
								const int size = pLocomotion->Size();
								if (size > 0 && size <= 0x400)
								{
									sample.LocomotorSize = size;

									const auto* const bytes =
										reinterpret_cast<const unsigned char*>(pLocomotion);
									// LocomotionClass is two vtables at 0x00 and 0x04, Owner and LinkedTo at
									// 0x08 and 0x0C, Powered and Dirty at 0x10 and 0x11, and RefCount at
									// 0x14. The vtables, the two owner pointers and the reference count all
									// move with the allocation rather than with the state - Load even saves
									// and restores RefCount around its read - so hashing them would report
									// every locomotor in the game as different after any load. The derived
									// state starts at 0x18; the two flags are folded in by hand.
									const int kept = std::min(size,
										static_cast<int>(sample.LocomotorBytes.size()));
									std::copy(bytes, bytes + kept, sample.LocomotorBytes.begin());

								}
							}

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

			// Only the part of a locomotor that a load is meant to bring back. The two vtables at 0x00
			// and 0x04, Owner and LinkedTo at 0x08 and 0x0C and RefCount at 0x14 all move with the
			// allocation - LocomotionClass::Load even saves and restores RefCount around its read - so
			// comparing them would report every locomotor in the game as different after any load.
			// Powered and Dirty at 0x10 and 0x11 are real state and are kept.
			constexpr int LocomotorStateStart = 0x18;

			// A value that is almost certainly an address rather than anything the simulation reads.
			// Nothing in a locomotor or a bullet holds a number this large otherwise: coordinates are
			// leptons, timers are frames, facings are sixteen bits. The bound starts at the module
			// base, below which nothing is mapped.
			bool LooksLikeAnAddress(uint32_t value)
			{
				return value >= 0x00400000u && value < 0x80000000u;
			}

			// Which locomotors hold what varies by type and YRpp does not describe every one of them,
			// so the pointer members cannot be listed the way a bullet's can. They can be recognised
			// instead: a load moves every allocation, so a field that reads as an address in both
			// passes and differs is a pointer, not state.
			//
			// This is not fastidiousness. Every drive locomotor carries a Piggybackee at the end and
			// the padding before it picks up whatever the allocator last left there, so every unit on
			// the map came back "drifted" after every load - and the watch reports eight times per
			// load and then stops. The eight went on chrono miners with a different Piggybackee every
			// time, and the object whose state had actually changed was never reached.
			bool LocomotorDifferenceIsAnAddress(const WatchSample& a, const WatchSample& c, int at)
			{
				const int dword = at & ~3;
				if (dword < LocomotorStateStart || dword + 4 > static_cast<int>(a.LocomotorBytes.size()))
					return false;

				uint32_t before = 0;
				uint32_t now = 0;
				std::memcpy(&before, a.LocomotorBytes.data() + dword, sizeof(before));
				std::memcpy(&now, c.LocomotorBytes.data() + dword, sizeof(now));

				return LooksLikeAnAddress(before) && LooksLikeAnAddress(now);
			}

			bool SameLocomotorState(const WatchSample& a, const WatchSample& c)
			{
				if (a.LocomotorSize <= 0)
					return true;

				// Powered is real state. Dirty at 0x11 is not: it is the IPersistStream flag that says
				// the object has changed since it was last written, so a locomotor that has been
				// through a save has it set and one that has only been played does not. It differed on
				// almost every object of every load and never meant anything.
				if (a.LocomotorBytes[0x10] != c.LocomotorBytes[0x10])
					return false;

				const int kept = std::min(a.LocomotorSize,
					static_cast<int>(a.LocomotorBytes.size()));

				for (int at = LocomotorStateStart; at < kept; ++at)
				{
					if (a.LocomotorBytes[at] == c.LocomotorBytes[at])
						continue;

					if (!LocomotorDifferenceIsAnAddress(a, c, at))
						return false;
				}

				return true;
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
					&& a.Health == c.Health
					&& a.IsALoaner == c.IsALoaner
					&& a.IsInPlayfield == c.IsInPlayfield
					&& a.TeamId == c.TeamId && a.TeamLeavingMap == c.TeamLeavingMap
					&& a.OwnerIndex == c.OwnerIndex
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
					&& a.LocomotorTimerRate == c.LocomotorTimerRate
					&& a.Ammo == c.Ammo
					&& a.RadioLinkCount == c.RadioLinkCount && a.RadioLinks == c.RadioLinks
					&& a.LocomotorSize == c.LocomotorSize
					&& SameLocomotorState(a, c);
			}

			void ReportObjectDrift(int frame, const std::vector<WatchSample>& before,
				const std::vector<WatchSample>& now)
			{
				std::unordered_map<uint32_t, const WatchSample*> beforeById;
				std::unordered_map<uint32_t, const WatchSample*> nowById;
				beforeById.reserve(before.size());
				nowById.reserve(now.size());
				for (const auto& sample : before)
					beforeById.emplace(sample.Id, &sample);
				for (const auto& sample : now)
					nowById.emplace(sample.Id, &sample);

				if (before.size() != now.size())
				{
					++WatchedObjectDriftReports;
					Debug::Log("[Replay] Frame %d holds %d objects on the way back through, %d the first time "
						"round.\n", frame, static_cast<int>(now.size()), static_cast<int>(before.size()));

					int reported = 0;
					for (const auto& sample : now)
					{
						if (beforeById.find(sample.Id) == beforeById.end() && reported++ < 4)
						{
							Debug::Log("[Replay]   only after the keyframe load: object %u (%s), house %d, "
								"mission %d at %d,%d,%d.\n", sample.Id, sample.TypeId.data(),
								sample.OwnerIndex, sample.Mission, sample.X, sample.Y, sample.Z);
						}
					}
					reported = 0;
					for (const auto& sample : before)
					{
						if (nowById.find(sample.Id) == nowById.end() && reported++ < 4)
						{
							Debug::Log("[Replay]   only on the first pass: object %u (%s), house %d, "
								"mission %d at %d,%d,%d.\n", sample.Id, sample.TypeId.data(),
								sample.OwnerIndex, sample.Mission, sample.X, sample.Y, sample.Z);
						}
					}
				}

				for (const auto& is : now)
				{
					const auto found = beforeById.find(is.Id);
					if (found == beforeById.end())
						continue;
					const WatchSample& was = *found->second;
					if (SameWatchSample(was, is) || !ReportedDriftObjects.insert(is.Id).second)
						continue;

					++WatchedObjectDriftReports;
					// Only the first frame the watch could look at, which is the first frame anything
					// drifted on only if the watch was there for the load. It says which so a report
					// three hundred frames after a keyframe is not read as the load's own doing.
					Debug::Log("[Replay] Frame %d: object %u drifted - the first frame anything did out of "
						"the frames the watch holds%s.\n", frame, is.Id,
						WatchedObjectsByFrame.count(LastLoadedKeyframeFrame) != 0
							? ", the keyframe's own frame among them, so this is where the load went wrong "
							  "rather than where it showed"
							: " (the keyframe's own frame is not among them, so the load itself was not "
							  "watched)");
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
					if (was.Ammo != is.Ammo)
						Debug::Log("[Replay]   %d ammo, was %d.\n", is.Ammo, was.Ammo);
					if (was.RadioLinkCount != is.RadioLinkCount || was.RadioLinks != is.RadioLinks)
					{
						Debug::Log("[Replay]   %d radio links (%u %u %u %u %u %u %u %u), was %d "
							"(%u %u %u %u %u %u %u %u).\n", is.RadioLinkCount,
							is.RadioLinks[0], is.RadioLinks[1], is.RadioLinks[2], is.RadioLinks[3],
							is.RadioLinks[4], is.RadioLinks[5], is.RadioLinks[6], is.RadioLinks[7],
							was.RadioLinkCount,
							was.RadioLinks[0], was.RadioLinks[1], was.RadioLinks[2], was.RadioLinks[3],
							was.RadioLinks[4], was.RadioLinks[5], was.RadioLinks[6], was.RadioLinks[7]);
					}
					if (!SameLocomotorState(was, is) || was.LocomotorSize != is.LocomotorSize)
					{
						Debug::Log("[Replay]   its locomotor is %d bytes, was %d.\n",
							is.LocomotorSize, was.LocomotorSize);

						const int kept = std::min(is.LocomotorSize,
							static_cast<int>(is.LocomotorBytes.size()));
						int named = 0;
						for (int at = 0x10; at < kept && named < 8; ++at)
						{
							if (at >= 0x12 && at < LocomotorStateStart)
								continue;
							if (was.LocomotorBytes[at] == is.LocomotorBytes[at])
								continue;

							++named;
							// Still printed when something else has reported the object, marked for
							// what they are, the way the unreachable path entries are.
							Debug::Log("[Replay]     locomotor byte 0x%02X is %02X, was %02X%s.\n", at,
								is.LocomotorBytes[at], was.LocomotorBytes[at],
								LocomotorDifferenceIsAnAddress(was, is, at)
									? " (an address, not state)" : "");
						}
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
					if (was.IsALoaner != is.IsALoaner
						|| was.IsInPlayfield != is.IsInPlayfield
						|| was.TeamId != is.TeamId
						|| was.TeamLeavingMap != is.TeamLeavingMap)
					{
						Debug::Log("[Replay]   off-map state loaner %d in-playfield %d team %u/leaving %d, "
							"was %d %d %u/%d.\n", is.IsALoaner, is.IsInPlayfield, is.TeamId,
							is.TeamLeavingMap, was.IsALoaner, was.IsInPlayfield, was.TeamId,
							was.TeamLeavingMap);
					}

					return;
				}
			}

			// Top of the frame, before anything in it has run, so both passes are sampled at the same
			// point.
			void ServiceObjectWatch()
			{
				if (!DiagnosticsWanted())
					return;

				if (!ScenarioClass::Instance)
					return;

				const int frame = static_cast<int>(Unsorted::CurrentFrame);
				const auto it = WatchedObjectsByFrame.find(frame);

				if (it != WatchedObjectsByFrame.end())
				{
					std::vector<WatchSample> now;
					SampleObjectWatch(now);

					const bool same = now.size() == it->second.size()
						&& std::equal(now.begin(), now.end(), it->second.begin(), SameWatchSample);

					if (!same && WatchedObjectDriftReports < MaxDriftReports)
						ReportObjectDrift(frame, it->second, now);

					return;
				}

				if (WatchedObjectSampleCount >= MaxWatchedObjectSamples || !FrameIsWorthWatching(frame))
					return;

				std::vector<WatchSample> sample;
				SampleObjectWatch(sample);
				if (!ChargeWatchMemory(sample.size() * sizeof(WatchSample)
					+ sizeof(std::vector<WatchSample>)))
				{
					return;
				}

				WatchedObjectSampleCount += sample.size();
				WatchedObjectsByFrame[frame] = std::move(sample);
			}

			#pragma endregion Per-frame object watch

			#pragma region Per-frame bullet watch

			// The object watch walks TechnoClass::Array, so a bullet has never been in it, and the
			// layer watch only ever sees a bullet's id, type and coordinates. That was the blind spot
			// this was written for: loading keyframe 32250 made a SparkSys, a SmallGreySSys and a
			// PIFFPIFF on its first frame that the recording never made - a bullet going off - and
			// nothing in the diagnostics could say which bullet, or what about it had changed. The
			// keyframes that restored cleanly had no bullets in the air; the ones that did not had
			// eight and ten.
			//
			// A bullet is a small object and there are rarely more than a handful in flight, so the
			// whole thing is kept rather than a chosen set of fields - including everything YRpp still
			// calls unknown_, which is exactly where an undiagnosed difference is likely to be. The
			// pointers are the one part that cannot be compared: they move with the allocation and
			// would report every bullet as different after any load. They are skipped as bytes and
			// carried as the unique ID of whatever they point at, which is the part that has to
			// survive a load.
			constexpr size_t MaxWatchedBulletBytes = 0x200;
			constexpr size_t MaxWatchedBulletSamples = 200000;

			struct BulletSample
			{
				uint32_t Id;
				uint32_t OwnerId;
				uint32_t TargetId;
				uint32_t NextAnimId;
				uint32_t NextObjectId;
				std::array<char, 32> TypeId {};
				int32_t Size;
				std::array<unsigned char, MaxWatchedBulletBytes> Bytes {};
			};

			// Every field of a bullet that holds an address. Named by offsetof rather than by number
			// so a YRpp layout change moves them rather than silently pointing them somewhere else.
			struct ByteRange
			{
				size_t Offset;
				size_t Length;
			};

			const ByteRange BulletPointerBytes[] =
			{
				// The four vtables, and the COM reference count LocomotionClass::Load also has to save
				// and restore around its own read.
				{ 0, offsetof(AbstractClass, UniqueID) },
				{ offsetof(AbstractClass, RefCount), sizeof(LONG) },
				// The notice list head. AbstractClass::AbstractClass nulls it, so an object the game
				// built has none, but AbstractClass::Load reads Size_Of() bytes and BulletClass::Load
				// runs only the no-init constructor over the result - so a bullet out of a savegame
				// carries the address the saving process had. It differed on every bullet of every
				// load and said nothing about any of them.
				{ offsetof(AbstractClass, unknown_18), sizeof(DWORD) },

				{ offsetof(ObjectClass, NextObject), sizeof(void*) },
				{ offsetof(ObjectClass, AttachedTag), sizeof(void*) },
				{ offsetof(ObjectClass, AttachedBomb), sizeof(void*) },
				// Both audio controllers own sound handles and pointers into the audio engine, none of
				// which is simulation state and none of which survives a load.
				{ offsetof(ObjectClass, AmbientSoundController), sizeof(AudioController) },
				{ offsetof(ObjectClass, CustomSoundController), sizeof(AudioController) },
				{ offsetof(ObjectClass, Parachute), sizeof(void*) },
				{ offsetof(ObjectClass, LineTrailer), sizeof(void*) },

				{ offsetof(BulletClass, Type), sizeof(void*) },
				{ offsetof(BulletClass, Owner), sizeof(void*) },
				{ offsetof(BulletClass, Target), sizeof(void*) },
				{ offsetof(BulletClass, WH), sizeof(void*) },
				{ offsetof(BulletClass, WeaponType), sizeof(void*) },
				{ offsetof(BulletClass, NextAnim), sizeof(void*) },
			};

			// Only bytes that belong to a member the table below names. Keeping the whole object and
			// skipping the pointers was not enough: a BulletClass is a third alignment padding - three
			// bytes after every bool, four before the double, the tail of every CDTimerClass - and
			// none of it is ever written, so each bullet carries whatever the allocator last left
			// there. A bullet out of a savegame carries the padding the saving process had and a
			// bullet that was played carries its own, and every one of them read as drifted.
			//
			// The table names every real member of AbstractClass, ObjectClass and BulletClass, the
			// ones YRpp still calls unknown_ included, so nothing that is written is dropped.
			const char* NameBulletByte(size_t offset);

			bool BulletByteIsComparable(size_t offset)
			{
				for (const ByteRange& range : BulletPointerBytes)
				{
					if (offset >= range.Offset && offset < range.Offset + range.Length)
						return false;
				}

				return NameBulletByte(offset) != nullptr;
			}

			// So a report says which field moved rather than only where. Anything not named here is
			// still reported by offset, which YRpp's BulletClass.h reads straight off.
			struct BulletField
			{
				size_t Offset;
				size_t Length;
				const char* Name;
			};

			const BulletField BulletFields[] =
			{
				{ offsetof(AbstractClass, UniqueID), 4, "UniqueID" },
				// One byte, not four. AbstractClass::AbstractClass reads the flags, masks the low
				// three bits and writes back only cl - the three bytes above it are never touched by
				// anything that treats them as flags, and they carry the top of a nearby address.
				{ offsetof(AbstractClass, AbstractFlags), 1, "AbstractFlags" },
				{ offsetof(AbstractClass, Dirty), 1, "Dirty" },
				{ offsetof(ObjectClass, unknown_24), 4, "unknown_24" },
				{ offsetof(ObjectClass, unknown_28), 4, "unknown_28" },
				{ offsetof(ObjectClass, FallRate), 4, "FallRate" },
				{ offsetof(ObjectClass, CustomSound), 4, "CustomSound" },
				{ offsetof(ObjectClass, BombVisible), 1, "BombVisible" },
				{ offsetof(ObjectClass, Health), 4, "Health" },
				{ offsetof(ObjectClass, EstimatedHealth), 4, "EstimatedHealth" },
				{ offsetof(ObjectClass, IsOnMap), 1, "IsOnMap" },
				{ offsetof(ObjectClass, unknown_78), 4, "unknown_78" },
				{ offsetof(ObjectClass, unknown_7C), 4, "unknown_7C" },
				// NeedsRedraw, IsSelected and IsVisible are deliberately absent: they are set by the
				// draw, and a frame that was played was drawn where a frame reached by loading a
				// keyframe was not. IsVisible alone reported four bullets a frame for eight frames
				// and spent the whole report budget on the viewport.
				{ offsetof(ObjectClass, InLimbo), 1, "InLimbo" },
				{ offsetof(ObjectClass, InOpenToppedTransport), 1, "InOpenToppedTransport" },
				{ offsetof(ObjectClass, HasParachute), 1, "HasParachute" },
				{ offsetof(ObjectClass, OnBridge), 1, "OnBridge" },
				{ offsetof(ObjectClass, IsFallingDown), 1, "IsFallingDown" },
				{ offsetof(ObjectClass, WasFallingDown), 1, "WasFallingDown" },
				{ offsetof(ObjectClass, IsABomb), 1, "IsABomb" },
				{ offsetof(ObjectClass, IsAlive), 1, "IsAlive" },
				{ offsetof(ObjectClass, LastLayer), 4, "LastLayer" },
				{ offsetof(ObjectClass, IsInLogic), 1, "IsInLogic" },
				{ offsetof(ObjectClass, Location), sizeof(CoordStruct), "Location" },

				// The fuse. Fuse_Checkup fires the bullet when the arming timer has run out and the
				// distance to HeadTo stops falling, so all four of these decide when it goes off.
				{ offsetof(BulletClass, Data) + offsetof(BulletData, UnknownTimer)
					+ offsetof(CDTimerClass, StartTime), 4, "Data.UnknownTimer.StartTime" },
				{ offsetof(BulletClass, Data) + offsetof(BulletData, UnknownTimer)
					+ offsetof(CDTimerClass, TimeLeft), 4, "Data.UnknownTimer.TimeLeft" },
				{ offsetof(BulletClass, Data) + offsetof(BulletData, ArmTimer)
					+ offsetof(CDTimerClass, StartTime), 4, "Data.ArmTimer.StartTime" },
				{ offsetof(BulletClass, Data) + offsetof(BulletData, ArmTimer)
					+ offsetof(CDTimerClass, TimeLeft), 4, "Data.ArmTimer.TimeLeft" },
				{ offsetof(BulletClass, Data) + offsetof(BulletData, Location),
					sizeof(CoordStruct), "Data.Location (fuse HeadTo)" },
				{ offsetof(BulletClass, Data) + offsetof(BulletData, Distance), 4,
					"Data.Distance (fuse closest approach)" },

				{ offsetof(BulletClass, unknown_B4), 1, "unknown_B4" },
				{ offsetof(BulletClass, Bright), 1, "Bright" },
				{ offsetof(BulletClass, unknown_E4), 4, "unknown_E4" },
				{ offsetof(BulletClass, Velocity), sizeof(BulletVelocity), "Velocity" },
				{ offsetof(BulletClass, unknown_100), 4, "unknown_100" },
				{ offsetof(BulletClass, unknown_104), 1, "unknown_104" },
				{ offsetof(BulletClass, CourseLock), 1, "CourseLock" },
				{ offsetof(BulletClass, CourseLockCounter), 4, "CourseLockCounter" },
				{ offsetof(BulletClass, Speed), 4, "Speed" },
				{ offsetof(BulletClass, InheritedColor), 4, "InheritedColor" },
				{ offsetof(BulletClass, unknown_118), 4, "unknown_118" },
				{ offsetof(BulletClass, unknown_11C), 4, "unknown_11C" },
				{ offsetof(BulletClass, unknown_120), 8, "unknown_120" },
				{ offsetof(BulletClass, AnimFrame), 1, "AnimFrame" },
				{ offsetof(BulletClass, AnimRateCounter), 1, "AnimRateCounter" },
				{ offsetof(BulletClass, SourceCoords), sizeof(CoordStruct), "SourceCoords" },
				{ offsetof(BulletClass, TargetCoords), sizeof(CoordStruct), "TargetCoords" },
				{ offsetof(BulletClass, LastMapCoords), sizeof(CellStruct), "LastMapCoords" },
				{ offsetof(BulletClass, DamageMultiplier), 4, "DamageMultiplier" },
				{ offsetof(BulletClass, SpawnNextAnim), 1, "SpawnNextAnim" },
				{ offsetof(BulletClass, Range), 4, "Range" },
			};

			// Null for a byte no member covers, which is alignment padding and is never compared.
			const char* NameBulletByte(size_t offset)
			{
				for (const BulletField& field : BulletFields)
				{
					if (offset >= field.Offset && offset < field.Offset + field.Length)
						return field.Name;
				}

				return nullptr;
			}

			std::unordered_map<int, std::vector<BulletSample>> WatchedBulletsByFrame;
			size_t WatchedBulletSampleCount = 0;
			int WatchedBulletDriftReports = 0;

			void ResetBulletWatch()
			{
				WatchedBulletsByFrame.clear();
				WatchedBulletSampleCount = 0;
				WatchedBulletDriftReports = 0;
			}

			void SampleBulletWatch(std::vector<BulletSample>& out)
			{
				out.clear();
				out.reserve(static_cast<size_t>(std::max(BulletClass::Array.Count, 0)));

				for (int i = 0; i < BulletClass::Array.Count; ++i)
				{
					const auto* const pBullet = BulletClass::Array.Items[i];
					if (!pBullet)
						continue;

					BulletSample sample {};
					sample.Id = UniqueIDOf(pBullet);
					sample.OwnerId = UniqueIDOf(pBullet->Owner);
					sample.TargetId = UniqueIDOf(pBullet->Target);
					sample.NextAnimId = UniqueIDOf(pBullet->NextAnim);
					sample.NextObjectId = UniqueIDOf(pBullet->NextObject);

					if (pBullet->Type)
					{
						strncpy_s(sample.TypeId.data(), sample.TypeId.size(), pBullet->Type->ID,
							sample.TypeId.size() - 1);
					}

					const int size = pBullet->Size();
					sample.Size = std::clamp(size, 0, static_cast<int>(MaxWatchedBulletBytes));
					std::memcpy(sample.Bytes.data(), pBullet, static_cast<size_t>(sample.Size));
					out.push_back(sample);
				}
			}

			void ReportBulletDrift(int frame, const std::vector<BulletSample>& before,
				const std::vector<BulletSample>& now)
			{
				++WatchedBulletDriftReports;

				if (before.size() != now.size())
				{
					Debug::Log("[Replay] Frame %d: %u bullets are in the air, %u the first time round.\n",
						frame, static_cast<unsigned int>(now.size()),
						static_cast<unsigned int>(before.size()));
				}

				constexpr int MaxReportedBullets = 4;
				constexpr int MaxReportedBytesPerBullet = 24;
				int reportedBullets = 0;

				for (size_t i = 0; i < now.size() && reportedBullets < MaxReportedBullets; ++i)
				{
					// Matched by position rather than by id: the arrays are the same length and in the
					// same order whenever the order restore succeeded, and when they are not the count
					// line above has already said so.
					if (i >= before.size())
						break;

					const BulletSample& was = before[i];
					const BulletSample& is = now[i];

					bool differs = was.Id != is.Id || was.OwnerId != is.OwnerId
						|| was.TargetId != is.TargetId || was.NextAnimId != is.NextAnimId
						|| was.NextObjectId != is.NextObjectId || was.Size != is.Size
						|| was.TypeId != is.TypeId;

					const int compared = std::min(was.Size, is.Size);
					for (int offset = 0; offset < compared && !differs; ++offset)
					{
						if (BulletByteIsComparable(static_cast<size_t>(offset))
							&& was.Bytes[offset] != is.Bytes[offset])
						{
							differs = true;
						}
					}

					if (!differs)
						continue;

					++reportedBullets;
					Debug::Log("[Replay] Frame %d: bullet %u [%s] drifted - owner %u, target %u "
						"(first pass: %u [%s], owner %u, target %u).\n",
						frame, is.Id, is.TypeId.data(), is.OwnerId, is.TargetId,
						was.Id, was.TypeId.data(), was.OwnerId, was.TargetId);

					int reportedBytes = 0;
					for (int offset = 0; offset < compared
						&& reportedBytes < MaxReportedBytesPerBullet; ++offset)
					{
						if (!BulletByteIsComparable(static_cast<size_t>(offset))
							|| was.Bytes[offset] == is.Bytes[offset])
						{
							continue;
						}

						++reportedBytes;
						Debug::Log("[Replay]   byte 0x%02X (%s) is %02X, was %02X.\n",
							offset, NameBulletByte(static_cast<size_t>(offset)),
							is.Bytes[offset], was.Bytes[offset]);
					}
				}
			}

			void ServiceBulletWatch()
			{
				if (!DiagnosticsWanted())
					return;

				if (!ScenarioClass::Instance)
					return;

				const int frame = static_cast<int>(Unsorted::CurrentFrame);
				const auto it = WatchedBulletsByFrame.find(frame);

				if (it != WatchedBulletsByFrame.end())
				{
					std::vector<BulletSample> now;
					SampleBulletWatch(now);

					bool same = now.size() == it->second.size();
					for (size_t i = 0; i < now.size() && same; ++i)
					{
						const BulletSample& was = it->second[i];
						const BulletSample& is = now[i];
						same = was.Id == is.Id && was.OwnerId == is.OwnerId
							&& was.TargetId == is.TargetId && was.NextAnimId == is.NextAnimId
							&& was.NextObjectId == is.NextObjectId && was.Size == is.Size
							&& was.TypeId == is.TypeId;

						const int compared = std::min(was.Size, is.Size);
						for (int offset = 0; offset < compared && same; ++offset)
						{
							if (BulletByteIsComparable(static_cast<size_t>(offset)))
								same = was.Bytes[offset] == is.Bytes[offset];
						}
					}

					if (!same && WatchedBulletDriftReports < MaxDriftReports)
						ReportBulletDrift(frame, it->second, now);

					return;
				}

				if (WatchedBulletSampleCount >= MaxWatchedBulletSamples || !FrameIsWorthWatching(frame))
					return;

				std::vector<BulletSample> sample;
				SampleBulletWatch(sample);
				if (!ChargeWatchMemory(sample.size() * sizeof(BulletSample)
					+ sizeof(std::vector<BulletSample>)))
				{
					return;
				}

				WatchedBulletSampleCount += sample.size();
				WatchedBulletsByFrame[frame] = std::move(sample);
			}

			#pragma endregion Per-frame bullet watch

			#pragma region Object names for the keyframe

			void CaptureAbstractObjectNames(std::vector<KeyframeObjectName>& out)
			{
				out.clear();
				out.reserve(static_cast<size_t>(std::max(AbstractClass::Array.Count, 0)));

				for (int i = 0; i < AbstractClass::Array.Count; ++i)
				{
					const auto* const pAbstract = AbstractClass::Array.Items[i];
					if (!pAbstract)
						continue;

					KeyframeObjectName name {};
					name.Id = UniqueIDOf(pAbstract);
					name.Type = static_cast<int32_t>(pAbstract->WhatAmI());

					if (const auto* const pObject = abstract_cast<const ObjectClass*>(pAbstract))
					{
						if (const auto* const pType = pObject->GetType())
							strncpy_s(name.TypeId.data(), name.TypeId.size(), pType->ID,
								name.TypeId.size() - 1);
					}

					out.push_back(name);
				}
			}

			// The set difference, both ways, with the missing ones named from what the keyframe wrote
			// down while they were still here. "2437 objects after loading, expected 2439" says two
			// objects did not survive their own savegame and nothing else; which two, and what they
			// were, is the whole of the question.
			void ReportLostKeyframeObjects(const Keyframe& keyframe)
			{
				constexpr int MaxReported = 12;

				std::unordered_set<uint32_t> live;
				live.reserve(static_cast<size_t>(std::max(AbstractClass::Array.Count, 0)));
				for (int i = 0; i < AbstractClass::Array.Count; ++i)
				{
					if (const auto* const pAbstract = AbstractClass::Array.Items[i])
						live.insert(UniqueIDOf(pAbstract));
				}

				int reported = 0;
				for (const auto& saved : keyframe.AbstractObjects)
				{
					if (live.count(saved.Id) != 0)
						continue;

					if (++reported > MaxReported)
						break;

					Debug::Log("[Replay]   unique ID %d did not come back from the load: abstract type "
						"%d%s%s.\n", static_cast<int>(saved.Id), saved.Type,
						saved.TypeId[0] != '\0' ? ", " : "",
						saved.TypeId[0] != '\0' ? saved.TypeId.data() : "");
				}

				std::unordered_set<uint32_t> expected;
				expected.reserve(keyframe.AbstractObjects.size());
				for (const auto& saved : keyframe.AbstractObjects)
					expected.insert(saved.Id);

				reported = 0;
				for (int i = 0; i < AbstractClass::Array.Count && reported < MaxReported; ++i)
				{
					const auto* const pAbstract = AbstractClass::Array.Items[i];
					if (!pAbstract || expected.count(UniqueIDOf(pAbstract)) != 0)
						continue;

					++reported;
					Debug::Log("[Replay]   unique ID %d came out of the load and the keyframe never "
						"held it: a %s.\n", static_cast<int>(UniqueIDOf(pAbstract)),
						DescribeAbstract(pAbstract));
				}
			}

			// OverlayClass is the one thing in AbstractClass::Array that no savegame has ever carried:
			// there is no TClassFactory<OverlayClass>, so the load never reconstructs one. Every crate
			// on the map therefore loses its wrapper object across any load, in vanilla YR as much as
			// here. The crate itself is unaffected - it lives in the cell's overlay bytes, which are
			// saved with the cell and are what CellClass::Goodie_Check reads.
			//
			// Keeping them in the keyframe's order made two crates look like the load had lost two
			// objects, on every load of every game with a crate on the map, and cost the ordering
			// restore for the array they were in. Leaving them out makes the two sides agree and the
			// order exact, and keeps a report of a real loss worth reading.
			void DropObjectsTheSavegameDoesNotCarry(Keyframe& keyframe)
			{
				std::unordered_set<uint32_t> transient;
				for (const auto& object : keyframe.AbstractObjects)
				{
					if (object.Type == static_cast<int32_t>(AbstractType::Overlay))
						transient.insert(object.Id);
				}

				if (transient.empty())
					return;

				auto& order = keyframe.Orders[OrderIndex_AbstractClass];
				order.erase(std::remove_if(order.begin(), order.end(),
					[&transient](uint32_t id) { return transient.count(id) != 0; }), order.end());

				auto& names = keyframe.AbstractObjects;
				names.erase(std::remove_if(names.begin(), names.end(),
					[](const KeyframeObjectName& name)
					{
						return name.Type == static_cast<int32_t>(AbstractType::Overlay);
					}), names.end());
			}

			#pragma endregion Object names for the keyframe

			#pragma region Kamikaze tracker

			void CaptureKamikazeState(KamikazeSnapshot& snapshot)
			{
				auto& tracker = Kamikaze::Instance;

				snapshot.TimerStart = tracker.UpdateTimer.StartTime;
				snapshot.TimerLeft = tracker.UpdateTimer.TimeLeft;

				snapshot.Aircraft.clear();
				snapshot.Aircraft.reserve(static_cast<size_t>(std::max(tracker.Nodes.Count, 0)));
				for (int i = 0; i < tracker.Nodes.Count; ++i)
				{
					const auto* const pNode = tracker.Nodes.Items[i];
					snapshot.Aircraft.push_back(pNode ? UniqueIDOf(pNode->Item) : 0u);
				}
			}

			void RestoreKamikazeState(const KamikazeSnapshot& snapshot, int keyframeFrame)
			{
				auto& tracker = Kamikaze::Instance;

				const bool timerChanged = tracker.UpdateTimer.StartTime != snapshot.TimerStart
					|| tracker.UpdateTimer.TimeLeft != snapshot.TimerLeft;

				// The gate is in the savegame now - see the kamikaze region in Bugfixes.SaveLoad.cpp -
				// so this should have nothing to do. It is kept as the check on that: if it ever puts
				// the gate back again, the save format fix is not taking, and the line says so rather
				// than quietly covering for it.
				if (timerChanged)
				{
					Debug::Log("[Replay] Keyframe %d had to put the kamikaze tracker's gate back to "
						"%d/%d; the load left it at %d/%d. The savegame is supposed to carry it now, so "
						"this means Kamikaze::Save or ::Load is not writing what it reads.\n",
						keyframeFrame, snapshot.TimerStart, snapshot.TimerLeft,
						tracker.UpdateTimer.StartTime, tracker.UpdateTimer.TimeLeft);
				}

				tracker.UpdateTimer.StartTime = snapshot.TimerStart;
				tracker.UpdateTimer.TimeLeft = snapshot.TimerLeft;

				KamikazeSnapshot afterLoad;
				CaptureKamikazeState(afterLoad);
				if (afterLoad.Aircraft != snapshot.Aircraft)
				{
					Debug::Log("[Replay] Keyframe %d: the kamikaze tracker holds %u aircraft after "
						"loading; it held %u. Kamikaze::Load appends without clearing, so this is worth "
						"reading as duplicates before it is read as losses.\n", keyframeFrame,
						static_cast<unsigned int>(afterLoad.Aircraft.size()),
						static_cast<unsigned int>(snapshot.Aircraft.size()));
				}
			}

			#pragma endregion Kamikaze tracker

			#pragma region Watch memory budget

			// The watches' share of the diagnostic budget; ReplaySystem.cpp holds the traces' share
			// and explains why the two are no longer one pool. The policy is the same: a frame that
			// does not fit is paid for by giving back the oldest frame recorded, so what is kept is
			// the window immediately behind playback rather than the opening of the replay.
			//
			// A frame of watch samples is a sample of every techno - some hundreds of bytes each -
			// plus every object in the six layers, so 128MB buys a few hundred frames rather than the
			// tens of thousands the traces get. That is enough: a backwards seek lands on a keyframe
			// and the frames worth comparing are the ones straight after it.
			constexpr size_t MaxWatchBytes = 128u * 1024u * 1024u;

			// Nothing reads a watch frame outside the call that records it, so only the frame being
			// recorded has to be safe from reclamation. The frame before it is held as well, to match
			// the traces and to leave room for a watch that grows a pointer into one later.
			constexpr int WatchFramesAlwaysHeld = 2;

			size_t WatchBytesUsed = 0;
			std::map<int, size_t> WatchBytesByFrame;
			bool WatchBudgetReported = false;

			// The cell baselines are kept apart from the frame window. They are taken once per
			// keyframe rather than once per frame, and a keyframe is old by the time it is worth
			// comparing against, so the window would always have forgotten the one that matters.
			constexpr size_t MaxCellBaselineBytes = 16u * 1024u * 1024u;
			size_t CellBaselineBytesUsed = 0;

			template <typename TStore>
			void ForgetWatchedFrame(TStore& store, int frame, size_t& entryCount)
			{
				const auto it = store.find(frame);
				if (it == store.end())
					return;

				entryCount -= std::min(entryCount, it->second.size());
				store.erase(it);
			}

			void ForgetWatchedFrameAt(std::map<int, size_t>::iterator it)
			{
				const int frame = it->first;

				ForgetWatchedFrame(WatchedObjectsByFrame, frame, WatchedObjectSampleCount);
				ForgetWatchedFrame(WatchedLayersByFrame, frame, WatchedLayerSampleCount);
				ForgetWatchedFrame(WatchedBulletsByFrame, frame, WatchedBulletSampleCount);
				ForgetWatchedFrame(CellChangesByFrame, frame, WatchedCellChangeCount);

				WatchBytesUsed -= std::min(WatchBytesUsed, it->second);
				WatchBytesByFrame.erase(it);
			}

			// The frame furthest from where playback is now, which is not always the oldest. A seek
			// backwards leaves the store full of frames ahead of the current one, and a rule that only
			// ever dropped the oldest could not touch any of them: the first backwards seek filled the
			// budget with frames it was forbidden to reclaim and every watch stopped recording, which
			// is what "a single frame does not fit in the 128 MB" meant in the log.
			//
			// Frames behind playback go first, because playback will not reach them again without
			// another seek. Only when there are none left does a frame ahead go, the furthest ahead
			// first, since that is the one playback will need last if it gets there at all.
			bool ForgetOneWatchedFrame(int frame)
			{
				if (WatchBytesByFrame.empty())
					return false;

				const auto oldest = WatchBytesByFrame.begin();
				if (oldest->first < frame - (WatchFramesAlwaysHeld - 1))
				{
					ForgetWatchedFrameAt(oldest);
					return true;
				}

				const auto newest = std::prev(WatchBytesByFrame.end());
				if (newest->first > frame)
				{
					ForgetWatchedFrameAt(newest);
					return true;
				}

				return false;
			}

			bool ChargeWatchMemory(size_t bytes)
			{
				const int frame = static_cast<int>(Unsorted::CurrentFrame);

				while (WatchBytesUsed + bytes > MaxWatchBytes && ForgetOneWatchedFrame(frame))
					;

				if (WatchBytesUsed + bytes > MaxWatchBytes)
				{
					// Only reachable if one frame of samples does not fit on its own.
					if (!WatchBudgetReported)
					{
						WatchBudgetReported = true;
						Debug::Log("[Replay] A single frame does not fit in the %u MB the watches are "
							"allowed, on frame %d. They will record nothing more; frames already recorded "
							"are still compared, and playback itself is unaffected.\n",
							static_cast<unsigned int>(MaxWatchBytes / (1024u * 1024u)), frame);
					}

					return false;
				}

				WatchBytesUsed += bytes;
				WatchBytesByFrame[frame] += bytes;
				return true;
			}

			bool ChargeCellBaselineMemory(size_t bytes)
			{
				while (CellBaselineBytesUsed + bytes > MaxCellBaselineBytes && !CellBaselines.empty())
				{
					// std::unordered_map, so the oldest has to be looked for rather than taken.
					auto oldest = CellBaselines.begin();
					for (auto it = CellBaselines.begin(); it != CellBaselines.end(); ++it)
					{
						if (it->first < oldest->first)
							oldest = it;
					}

					const size_t held = oldest->second.size() * sizeof(CellSnapshot);
					CellBaselineBytesUsed -= std::min(CellBaselineBytesUsed, held);
					CellBaselines.erase(oldest);
				}

				if (CellBaselineBytesUsed + bytes > MaxCellBaselineBytes)
					return false;

				CellBaselineBytesUsed += bytes;
				return true;
			}

			void ResetWatchMemory()
			{
				WatchBytesUsed = 0;
				WatchBytesByFrame.clear();
				WatchBudgetReported = false;
				CellBaselineBytesUsed = 0;
			}

			// How far back the watches reach right now, so a watch that reports nothing can be told
			// from one that had nothing to look at.
			int OldestWatchedFrame()
			{
				return WatchBytesByFrame.empty() ? -1 : WatchBytesByFrame.begin()->first;
			}

			#pragma endregion Watch memory budget

			void RestartDriftReporting()
			{
				ReportedDriftObjects.clear();
				WatchedLayerDriftReports = 0;
				WatchedCellDriftReports = 0;
				WatchedObjectDriftReports = 0;
				WatchedBulletDriftReports = 0;

				// The running cell hashes describe the world the seek left, not the one it landed in,
				// so the first frame after a load measured every cell the load had touched against the
				// wrong baseline: 2175 cells "changed" where the first pass changed 51, every time,
				// on every load. The frame after a load takes a fresh baseline and says nothing.
				CellDeltaBaselineStale = true;
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
						static_cast<int32_t>(pTechno->QueuedMission),
						static_cast<int32_t>(pTechno->SuspendedMission),
						pFoot ? pFoot->NavQueue.Count : 0,
						pFoot && pFoot->NavQueue.Count > 0 ? UniqueIDOf(pFoot->NavQueue.Items[0]) : 0u,
						pTechno->IsALoaner,
						pTechno->IsInPlayfield,
						pFoot ? UniqueIDOf(pFoot->Team) : 0u,
						pFoot && pFoot->Team ? pFoot->Team->IsLeavingMap : false
					});
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

			void CaptureTeamSnapshots(std::vector<TeamSnapshot>& out)
			{
				out.clear();
				out.reserve(static_cast<size_t>(std::max(TeamClass::Array.Count, 0)));
				for (int i = 0; i < TeamClass::Array.Count; ++i)
				{
					const auto* const pTeam = TeamClass::Array.Items[i];
					if (!pTeam)
						continue;

					out.push_back(TeamSnapshot {
						UniqueIDOf(pTeam),
						UniqueIDOf(pTeam->Owner),
						pTeam->Type ? static_cast<uint32_t>(pTeam->Type->ArrayIndex) : UINT32_MAX,
						pTeam->CreationFrame,
						pTeam->TotalObjects,
						pTeam->IsForcedActive,
						pTeam->IsHasBeen,
						pTeam->IsFullStrength,
						pTeam->IsUnderStrength
					});
				}
			}

			void ReportTeamSnapshotDifferences(const Keyframe& keyframe)
			{
				std::vector<TeamSnapshot> current;
				CaptureTeamSnapshots(current);

				if (current.size() != keyframe.Teams.size())
				{
					Debug::Log("[Replay] Keyframe %d: %u teams came back from the load; it held %u.\n",
						keyframe.Frame, static_cast<unsigned int>(current.size()),
						static_cast<unsigned int>(keyframe.Teams.size()));
				}

				std::unordered_map<uint32_t, const TeamSnapshot*> byId;
				byId.reserve(current.size());
				for (const auto& now : current)
					byId.emplace(now.Id, &now);

				for (const auto& saved : keyframe.Teams)
				{
					const auto found = byId.find(saved.Id);
					if (found == byId.end())
					{
						Debug::Log("[Replay] Keyframe %d: team %u (house %u) did not come back from "
							"the load.\n", keyframe.Frame, saved.Id, saved.OwnerId);
						continue;
					}

					const auto& now = *found->second;
					if (now.OwnerId == saved.OwnerId
						&& now.TypeIndex == saved.TypeIndex
						&& now.CreationFrame == saved.CreationFrame
						&& now.TotalObjects == saved.TotalObjects
						&& now.IsForcedActive == saved.IsForcedActive
						&& now.IsHasBeen == saved.IsHasBeen
						&& now.IsFullStrength == saved.IsFullStrength
						&& now.IsUnderStrength == saved.IsUnderStrength)
					{
						continue;
					}

					Debug::Log("[Replay] Keyframe %d: team %u (house %u, type %u) came back created "
						"on frame %d with %d objects, forced %d has-been %d full %d under %d; it was "
						"created on frame %d with %d objects, forced %d has-been %d full %d under %d.\n",
						keyframe.Frame, saved.Id, now.OwnerId, now.TypeIndex,
						now.CreationFrame, now.TotalObjects, now.IsForcedActive, now.IsHasBeen,
						now.IsFullStrength, now.IsUnderStrength,
						saved.CreationFrame, saved.TotalObjects, saved.IsForcedActive,
						saved.IsHasBeen, saved.IsFullStrength, saved.IsUnderStrength);
				}
			}

			void CaptureHouseProductionSnapshots(std::vector<HouseProductionSnapshot>& out)
			{
				out.clear();
				out.reserve(static_cast<size_t>(std::max(HouseClass::Array.Count, 0)));
				for (int i = 0; i < HouseClass::Array.Count; ++i)
				{
					const auto* const pHouse = HouseClass::Array.Items[i];
					if (!pHouse)
						continue;

					const auto* const pVehicleFactory = pHouse->Primary_ForVehicles;

					out.push_back(HouseProductionSnapshot {
						UniqueIDOf(pHouse),
						pHouse->ProducingBuildingTypeIndex,
						pHouse->ProducingUnitTypeIndex,
						pHouse->ProducingInfantryTypeIndex,
						pHouse->ProducingAircraftTypeIndex,
						UniqueIDOf(pHouse->Primary_ForBuildings),
						UniqueIDOf(pHouse->Primary_ForVehicles),
						UniqueIDOf(pHouse->Primary_ForShips),
						UniqueIDOf(pHouse->Primary_ForInfantry),
						UniqueIDOf(pHouse->Primary_ForAircraft),
						pVehicleFactory ? pVehicleFactory->Production.Value : -1,
						pVehicleFactory ? UniqueIDOf(pVehicleFactory->Object) : 0u,
						pVehicleFactory ? pVehicleFactory->QueuedObjects.Count : -1,
						pHouse->CountResourceGatherers,
						pHouse->CountResourceDestinations,
						pHouse->TechLevel,
						pHouse->IQLevel2,
						pHouse->IsTiberiumShort
					});
				}
			}

			void ReportHouseProductionSnapshotDifferences(const Keyframe& keyframe)
			{
				std::vector<HouseProductionSnapshot> current;
				CaptureHouseProductionSnapshots(current);

				std::unordered_map<uint32_t, const HouseProductionSnapshot*> byId;
				byId.reserve(current.size());
				for (const auto& now : current)
					byId.emplace(now.Id, &now);

				// Said outright, every time. A silent watch has been read as a passing one more than
				// once in this file, and "no house differed" and "no house was looked at" are very
				// different answers to the same question.
				int differences = 0;
				Debug::Log("[Replay] Keyframe %d compared the production state of %u houses against %u held.\n",
					keyframe.Frame, static_cast<unsigned int>(current.size()),
					static_cast<unsigned int>(keyframe.HouseProduction.size()));

				for (const auto& saved : keyframe.HouseProduction)
				{
					const auto found = byId.find(saved.Id);
					if (found == byId.end())
						continue;

					const auto& now = *found->second;
					if (now.ProducingBuildingTypeIndex == saved.ProducingBuildingTypeIndex
						&& now.ProducingUnitTypeIndex == saved.ProducingUnitTypeIndex
						&& now.ProducingInfantryTypeIndex == saved.ProducingInfantryTypeIndex
						&& now.ProducingAircraftTypeIndex == saved.ProducingAircraftTypeIndex
						&& now.PrimaryForBuildings == saved.PrimaryForBuildings
						&& now.PrimaryForVehicles == saved.PrimaryForVehicles
						&& now.PrimaryForShips == saved.PrimaryForShips
						&& now.PrimaryForInfantry == saved.PrimaryForInfantry
						&& now.PrimaryForAircraft == saved.PrimaryForAircraft
						&& now.VehicleFactoryProgress == saved.VehicleFactoryProgress
						&& now.VehicleFactoryObject == saved.VehicleFactoryObject
						&& now.VehicleFactoryQueued == saved.VehicleFactoryQueued
						&& now.CountResourceGatherers == saved.CountResourceGatherers
						&& now.CountResourceDestinations == saved.CountResourceDestinations
						&& now.TechLevel == saved.TechLevel
						&& now.IQLevel2 == saved.IQLevel2
						&& now.IsTiberiumShort == saved.IsTiberiumShort)
					{
						continue;
					}

					Debug::Log("[Replay] Keyframe %d: house %u production choices are "
						"building %d unit %d infantry %d aircraft %d after loading; they were "
						"%d, %d, %d, %d.\n", keyframe.Frame, saved.Id,
						now.ProducingBuildingTypeIndex, now.ProducingUnitTypeIndex,
						now.ProducingInfantryTypeIndex, now.ProducingAircraftTypeIndex,
						saved.ProducingBuildingTypeIndex, saved.ProducingUnitTypeIndex,
						saved.ProducingInfantryTypeIndex, saved.ProducingAircraftTypeIndex);

					Debug::Log("[Replay]   its primaries are %u/%u/%u/%u/%u "
						"(buildings/vehicles/ships/infantry/aircraft), were %u/%u/%u/%u/%u; the "
						"vehicle factory is at %d holding %u with %d queued, was %d holding %u "
						"with %d queued.\n",
						now.PrimaryForBuildings, now.PrimaryForVehicles, now.PrimaryForShips,
						now.PrimaryForInfantry, now.PrimaryForAircraft,
						saved.PrimaryForBuildings, saved.PrimaryForVehicles, saved.PrimaryForShips,
						saved.PrimaryForInfantry, saved.PrimaryForAircraft,
						now.VehicleFactoryProgress, now.VehicleFactoryObject, now.VehicleFactoryQueued,
						saved.VehicleFactoryProgress, saved.VehicleFactoryObject,
						saved.VehicleFactoryQueued);

					Debug::Log("[Replay]   it has %d gatherers and %d resource destinations, tech %d, "
						"IQ %d, tiberium short %d; it had %d, %d, tech %d, IQ %d, short %d.\n",
						now.CountResourceGatherers, now.CountResourceDestinations, now.TechLevel,
						now.IQLevel2, now.IsTiberiumShort,
						saved.CountResourceGatherers, saved.CountResourceDestinations,
						saved.TechLevel, saved.IQLevel2, saved.IsTiberiumShort);

					++differences;
				}

				if (differences == 0)
					Debug::Log("[Replay]   every house came back with the same production state.\n");
			}

			void ReportHouseRepairSnapshotDifferences(const Keyframe& keyframe)
			{
				std::unordered_map<uint32_t, const HouseClass*> byId;
				byId.reserve(static_cast<size_t>(std::max(HouseClass::Array.Count, 0)));
				for (int i = 0; i < HouseClass::Array.Count; ++i)
				{
					if (const auto* const pHouse = HouseClass::Array.Items[i])
						byId.emplace(UniqueIDOf(pHouse), pHouse);
				}

				for (const auto& saved : keyframe.HouseRepairs)
				{
					const auto found = byId.find(saved.Id);
					if (found == byId.end())
						continue;

					const auto* const pHouse = found->second;
					if (pHouse->Repairing == saved.DidRepair
						&& pHouse->RepairTimer.StartTime == saved.RepairTimerStart
						&& pHouse->RepairTimer.TimeLeft == saved.RepairTimerLeft)
					{
						continue;
					}

					Debug::Log("[Replay] Keyframe %d: house %u repair gate is %d with timer %d/%d "
						"after loading; it was %d with timer %d/%d.\n", keyframe.Frame, saved.Id,
						pHouse->Repairing, pHouse->RepairTimer.StartTime,
						pHouse->RepairTimer.TimeLeft, saved.DidRepair,
						saved.RepairTimerStart, saved.RepairTimerLeft);
				}
			}

			// HouseClass::AI uses Repairing as a one-repair-per-house gate. RepairTimer controls when
			// that gate is released. The save/load path preserves the gate byte but reconstructs the
			// timer as an already-expired timer at the current frame. On the frame after a load this
			// clears Repairing early, allowing a building to perform an extra repair allocation and,
			// for an AI house, consume an extra random number in BuildingClass::Repair_AI.
			//
			// Keep both values together: restoring only Repairing would still release it at the wrong
			// frame, while restoring only the timer could leave a stale gate after other save/load cases.
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

			// AbstractClass::Load reads the complete saved object, including TechnoClass::IsInPlayfield,
			// but the load then reconstructs and places every object. An overflight aircraft already
			// beyond the playable rectangle cannot be placed back into the map and that process clears
			// IsInPlayfield. The live timeline deliberately still has it set until AircraftClass::AI
			// observes that it is outside the radar rectangle and removes it:
			//
			//     if (!Map.In_Radar(cell) && Should_Delete_Off_Map())
			//         Remove_This();
			//
			// Should_Delete_Off_Map returns false when IsInPlayfield is clear. That left SPYP 1068707
			// alive after a frame-7500 load when the recording removed it on frame 7508, with the unique
			// ID counter still perfectly matched. The keyframe therefore puts this one history-dependent
			// placement bit back after the engine has finished reconstructing and resuming the session.
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
						&& before.QueuedMission == after.QueuedMission
						&& before.SuspendedMission == after.SuspendedMission
						&& before.NavQueueCount == after.NavQueueCount
						&& before.NavQueueHeadId == after.NavQueueHeadId
						&& before.IsALoaner == after.IsALoaner
						&& before.IsInPlayfield == after.IsInPlayfield
						&& before.TeamId == after.TeamId
						&& before.TeamLeavingMap == after.TeamLeavingMap)
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
							"accumulated %d (was %d), queued mission %d (was %d), suspended %d "
							"(was %d), %d queued destinations heading for %u "
							"(was %d heading for %u), loaner %d (was %d), in-playfield %d (was %d), "
							"team %u/leaving %d (was %u/%d).\n",
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
							after.QueuedMission, before.QueuedMission,
							after.SuspendedMission, before.SuspendedMission,
							after.NavQueueCount, after.NavQueueHeadId,
							before.NavQueueCount, before.NavQueueHeadId,
							after.IsALoaner, before.IsALoaner,
							after.IsInPlayfield, before.IsInPlayfield,
							after.TeamId, after.TeamLeavingMap,
							before.TeamId, before.TeamLeavingMap);
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
			// Still carried, and it earns its keep. Bugfixes.SaveLoad.cpp recomputes every cell's
			// passability with the engine's own CellClass::Check_Passability (0x483C80) at the end of
			// Load_Game, which should make this redundant - but the first attempt at that hook was placed
			// somewhere a load never reaches, and the only reason that was noticed at all is that this
			// restore had been dropped and the divergence came straight back. So the copy stays until the
			// line below reports zero cells restored across a long session, and then it can go.
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

			// Reported rather than repaired. If one of these stops matching, the engine-side save/load fix
			// that should be deriving it has regressed, and putting a copy back here would hide that from
			// the next person while leaving ordinary save/load broken.
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

						// Whether the ore grows or spreads at all on a given frame is this timer, read by
						// Tiberium_Growth_Logic (0x722C40) and Tiberium_Spread_Logic (0x7221B0) before either
						// queue is touched:
						//
						//     if (Started != -1 && Frame - Started >= DelayTime) goto grow;
						//     if (DelayTime == 0) goto grow;
						//
						// Both branches of that are open on a stopped timer - Started of -1 with nothing left
						// to run - which is how a TiberiumClass comes back from a load, because the four of
						// them are rebuilt from the rules rather than read out of the savegame. So the frame
						// after every seek grew and spread its ore whether or not it was due, and the first
						// draw of that frame came from TiberiumClass::Grow where the recording's had come from
						// TerrainClass::AI further down the same frame. Everything after that was off by one
						// draw, and the ore field itself had thickened a tick early.
						//
						// Put back ahead of the queue and separately from it: a queue that could not be
						// captured is ore evolving in a different order, while a timer left as the load found
						// it is a spurious growth tick on the very next frame.
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
			// AbstractClass::AbstractClass (0x410170) sets ID to -1 and leaves it there. Only the classes
			// whose constructors go on to call AbstractClass::Create_ID (0x410230) ever get a real one -
			// the objects in the world, bullets included. SlaveManagerClass and SpawnManagerClass do not,
			// so every one of them answers -1.
			//
			// That is not a detail: keying a snapshot by unique ID collapsed all four slave managers onto
			// one map entry, and the restore wrote all four recorded timers, states and slave controls onto
			// whichever manager happened to be first. Two rounds of slave-manager findings came out of that
			// and none of them meant anything. The manager's owner is a techno and does have a real ID, and
			// a techno has at most one manager of each kind, so the owner is the key.
			uint32_t KeyOfManagerOwner(const AbstractClass* pOwner)
			{
				return UniqueIDOf(pOwner);
			}

			// A key that repeats, or is zero because there is nothing to key on, cannot be matched across a
			// load - and silently aliasing is worse than not trying. Anything that cannot be keyed is left
			// out of the snapshot and counted, so the log says so instead of quietly corrupting the world.
			bool AddKeyed(std::vector<uint32_t>& seen, uint32_t key)
			{
				if (key == 0)
					return false;

				if (std::find(seen.begin(), seen.end(), key) != seen.end())
					return false;

				seen.push_back(key);
				return true;
			}

			// SlaveManagerClass is saved whole - Size_Of (0x6B1370) is 0x64, which reaches past the last
			// field - and its controls are written and read back twenty bytes at a time, so on paper none
			// of this needs carrying. It is captured anyway because the divergence says otherwise, and a
			// report naming the field that differs is worth more than another round of reading Load.
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

				// Copied whole rather than field by field. ScenarioClass::Load reads the saved struct
				// back and then runs ScenarioClass::ScenarioClass (0x683560) over the top of it, whose
				// first act is Random2Class::Random2Class(&RandomNumber, 0) - so every savegame load
				// re-seeds the synchronised randomiser to a fixed seed and throws away what the file
				// held. Putting it back is not optional, and it has to be all of it: Compute_Game_CRC
				// (0x64DAB0) draws from this randomiser as its last act, so the frame hash is part of
				// the same stream the simulation draws from.
				memcpy(keyframe.Random.data(), &ScenarioClass::Instance->Random, sizeof(Randomizer));

				CaptureTechnoSnapshots(keyframe.Technos);
				CaptureHouseRepairSnapshots(keyframe.HouseRepairs);
				CaptureHouseProductionSnapshots(keyframe.HouseProduction);
				CaptureTeamSnapshots(keyframe.Teams);
				if (!CapturePlanningState(keyframe.Planning))
					return false;
				if (!CaptureAresParticleState(keyframe.AresParticles))
					return false;
				CaptureLocomotorResetStates(keyframe.LocomotorResetStates);
				CaptureKamikazeState(keyframe.Kamikaze);
				CaptureAbstractObjectNames(keyframe.AbstractObjects);
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

				DropObjectsTheSavegameDoesNotCarry(keyframe);
				return true;
			}

			bool RestoreKeyframeState(const Keyframe& keyframe)
			{
				auto& random = ScenarioClass::Instance->Random;
				const bool randomChanged = memcmp(&random, keyframe.Random.data(), sizeof(Randomizer)) != 0;
				memcpy(&random, keyframe.Random.data(), sizeof(Randomizer));

				int reorderedCollectionCount = 0;
				auto restoreCollection = [&reorderedCollectionCount, &keyframe](auto& collection,
					const std::vector<uint32_t>& order, const char* name)
				{
					bool changed = false;
					const bool complete = RestoreObjectOrder(collection, order, name, changed);

					// Reported whether or not the sets matched: a partial restore still put the survivors
					// back where the recording had them, which is the part that decides who gets asked
					// first.
					if (changed)
					{
						++reorderedCollectionCount;
						Debug::Log("[Replay] %s came back from the load out of order; put back.\n", name);
					}

					// AbstractClass::Array is every object in the world, so a difference there is the
					// load losing or gaining objects rather than shuffling them, and the names are
					// worth spending the lookup on.
					if (!complete && static_cast<const void*>(&collection)
						== static_cast<const void*>(&AbstractClass::Array))
					{
						ReportLostKeyframeObjects(keyframe);
					}

					return complete;
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
				if (!RestoreAresParticleState(keyframe.AresParticles, keyframe.Frame))
					return false;
				if (!RestoreLocomotorResetStates(keyframe.LocomotorResetStates, keyframe.Frame))
					return false;
				RestoreKamikazeState(keyframe.Kamikaze, keyframe.Frame);
				RestoreTiberiumState(keyframe.Tiberium, keyframe.Frame);
				RestoreCellPassability(keyframe.CellPassability, keyframe.Frame);
				VerifyDerivedMapHashes(keyframe);
				if (!RestoreCellSubzones(keyframe.CellSubzones, keyframe.Frame))
					return false;
				if (!RestoreSubzoneTracking(keyframe.SubzoneGraph, keyframe.Frame))
					return false;

				{
					// A silent watch has been mistaken for a passing one three times in this file now.
					// This says outright how much locomotor state is actually being compared, so
					// "no locomotor differences" can be told from "no locomotors sampled".
					int sampled = 0;
					int bytesKept = 0;
					for (int i = 0; i < FootClass::Array.Count; ++i)
					{
						const auto* const pFoot = FootClass::Array.Items[i];
						if (!pFoot || !pFoot->Locomotor)
							continue;

						if (auto* const pLoco = static_cast<LocomotionClass*>(
							const_cast<FootClass*>(pFoot)->Locomotor.GetInterfacePtr()))
						{
							const int size = pLoco->Size();
							if (size > 0 && size <= 0x400)
							{
								++sampled;
								bytesKept += std::min(size, 0x70) - 0x18;
							}
						}
					}

					Debug::Log("[Replay] Keyframe %d is watching %d locomotors, %d bytes of state.\n",
						keyframe.Frame, sampled, bytesKept);
				}

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

				// Only for the comparison below, which is a diagnostic. A capture walks every techno, every
				// ordered collection and the whole subzone graph, and a seek across a long replay writes one
				// keyframe every few hundred frames - so taking it twice was a large part of what made the
				// first pass through a replay slow, to answer a question nobody is asking unless they have
				// turned the diagnostics on.
				Keyframe beforeSave;
				const bool wantSaveComparison = DiagnosticsWanted();
				if (wantSaveComparison && !CaptureKeyframeState(frame, beforeSave))
				{
					Debug::Log("[Replay] Could not capture simulation state for keyframe %d.\n", frame);
					return false;
				}

				if (!ScenarioClass::SaveGame(fileName, description))
				{
					Debug::Log("[Replay] Failed to write the keyframe for frame %d.\n", frame);
					return false;
				}

				// SaveGame is not a const operation on the running world. It codes and decodes the
				// object graph and lets every loaded extension write its own stream. Playback continues
				// from the state after all of that has returned, so the sidecar state paired with the
				// save must describe that post-save world too. Capturing it before SaveGame made a
				// keyframe internally consistent with the file but subtly different from the timeline
				// being recorded whenever a save changed an iteration order or extension-owned cache.
				Keyframe keyframe;
				if (!CaptureKeyframeState(frame, keyframe))
				{
					Debug::Log("[Replay] Could not capture post-save simulation state for keyframe %d.\n",
						frame);
					return false;
				}

				if (!wantSaveComparison)
				{
					State.Keyframes.push_back(std::move(keyframe));
					return true;
				}

				int changedOrders = 0;
				for (size_t i = 0; i < keyframe.Orders.size(); ++i)
					changedOrders += beforeSave.Orders[i] != keyframe.Orders[i] ? 1 : 0;
				for (size_t i = 0; i < keyframe.LayerOrders.size(); ++i)
					changedOrders += beforeSave.LayerOrders[i] != keyframe.LayerOrders[i] ? 1 : 0;

				const bool randomChanged = beforeSave.Random != keyframe.Random;
				const bool planningChanged = beforeSave.Planning != keyframe.Planning;
				const bool aresParticlesChanged = beforeSave.AresParticles != keyframe.AresParticles;
				if (changedOrders || randomChanged || planningChanged || aresParticlesChanged)
				{
					Debug::Log("[Replay] Writing keyframe %d changed live state; retaining the post-save "
						"timeline (ordered collections %d, RNG %s, planning %s, Ares particles %s).\n",
						frame, changedOrders, randomChanged ? "changed" : "same",
						planningChanged ? "changed" : "same",
						aresParticlesChanged ? "changed" : "same");
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

			// Savegames do not carry the networking queues, and Load_Game does not clear them either.
			// They are normally initialised only when Select_Game starts a new session. That is fatal
			// for a replay seek: an event injected just after one restored frame can survive a second
			// load, where Execute_DoList treats it as overdue and executes it in the later world. A
			// stale ARCHIVE event, for example, changes a factory rally point on the first frame after
			// the second load and the unit leaving that factory subsequently takes a different route.
			//
			// The replay stream is the complete source of gameplay events during playback, so none of
			// these transient queues belongs to the restored state. Clear on both sides of LoadMission:
			// before it, to discard events from the old timeline, and after it, to discard anything the
			// teardown/resume path happened to enqueue.
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
				LastLoadedKeyframeFrame = keyframe.Frame;
				RestartDriftReporting();
				RestartTraceReporting();

				// A watch that says nothing about the frame it was loaded on is either a watch that
				// found nothing or a watch that never saw the frame, and telling those apart by hand
				// has cost a round of this more than once.
				if (DiagnosticsWanted())
				{
					Debug::Log("[Replay] Keyframe %d is being loaded with the watches holding frames "
						"%d onwards (%u object, %u bullet, %u layer, %u cell-change frames, %u cell "
						"baselines); this frame itself is %s.\n",
						keyframe.Frame, OldestWatchedFrame(),
						static_cast<unsigned int>(WatchedObjectsByFrame.size()),
						static_cast<unsigned int>(WatchedBulletsByFrame.size()),
						static_cast<unsigned int>(WatchedLayersByFrame.size()),
						static_cast<unsigned int>(CellChangesByFrame.size()),
						static_cast<unsigned int>(CellBaselines.size()),
						WatchedObjectsByFrame.count(keyframe.Frame) != 0 ? "held" : "not held");
				}

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
				ReportHouseRepairSnapshotDifferences(keyframe);
				ReportHouseProductionSnapshotDifferences(keyframe);
				ReportTeamSnapshotDifferences(keyframe);

				RepointTempSurfaceAfterLoad();
				ResumeInGameSessionAfterLoad();
				RestoreHouseRepairState(keyframe);
				RestoreLoadResetTimerState(keyframe);
				RestoreSlaveManagerState(keyframe);
				RestoreTechnoInPlayfieldState(keyframe);

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

				// A locked replay deliberately replaces the camera with the recorded viewport as the
				// repositioned frame stream is read. With locking off, however, the camera belongs to
				// the viewer and LoadMission must not replace it with the keyframe save's viewport.
				// Keep the committed position rather than the desired one so a half-finished scroll
				// cannot resume from a different world state after the load.
				const bool restoreViewerViewport = !ReplayState.LockViewport && TacticalClass::Instance;
				const Point2D viewerViewport = restoreViewerViewport
					? TacticalClass::Instance->TacticalCoord1
					: Point2D { 0, 0 };

				ClearTransientEventQueuesForLoad("before", keyframe.Frame);
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
					// RepositionPlaybackStreamToFrame leaves the target frame's record pending. A seek
					// which lands exactly on a keyframe can re-pause before RestoreFrameState consumes
					// it, so include its sticky viewport here instead of displaying the savegame's view.
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

		// HouseExt::UpdateHarvesterProduction, reproduced input by input. Phobos' copy is the one that
		// runs; this one only reads, and exists so the answer and every term behind it are written
		// down. The seek traces show its answer flipping across a keyframe load - the house's whole
		// Phobos production block disappears from the draw sequence - while every field the keyframe
		// snapshot compares comes back identical, so the term that moved has to be named rather than
		// guessed at.
		//
		// Logged for a short window after each keyframe boundary, which both the first pass and the
		// replayed pass run through, so the two can be diffed line for line.
		void TraceHouseProductionGate(HouseClass* pHouse)
		{
			constexpr int GateTraceFrames = 12;

			if (!DiagnosticsWanted() || !pHouse || State.Interval <= 0 || IsLoadInProgress())
				return;

			const int frame = static_cast<int>(Unsorted::CurrentFrame);
			if (frame % State.Interval >= GateTraceFrames)
				return;

			auto* const pRules = RulesClass::Instance;
			if (!pRules || !pHouse->Type)
				return;

			const int difficulty = static_cast<int>(pHouse->GetAIDifficultyIndex());
			const int idxParentCountry = pHouse->Type->FindParentCountryIndex();
			const auto bitOwner = 1u << idxParentCountry;

			// FindOwned: the first harvester type this country is allowed to own. Pure type data, so
			// it cannot move across a load - it is logged to prove that rather than assume it.
			const UnitTypeClass* pHarvester = nullptr;
			for (int i = 0; i < pRules->HarvesterUnit.Count; ++i)
			{
				const auto* const pItem = pRules->HarvesterUnit.Items[i];
				if (pItem && pItem->InOwners(bitOwner))
				{
					pHarvester = pItem;
					break;
				}
			}

			// FindBuildable: the first refinery this house can expect to build. CanExpectToBuild walks
			// prerequisites, which read the house's building tallies - state a load rebuilds.
			const BuildingTypeClass* pRefinery = nullptr;
			for (int i = 0; i < pRules->BuildRefinery.Count; ++i)
			{
				const auto* const pItem = pRules->BuildRefinery.Items[i];
				if (pItem && pHouse->CanExpectToBuild(pItem, idxParentCountry))
				{
					pRefinery = pItem;
					break;
				}
			}

			// Both of these are TypeLists read from the rules, and a ruleset that never mentions one
			// leaves it empty with a null Items - which is not a hypothetical: AISlaveMinerNumber is
			// empty in this one, and reading it at the difficulty index put a null dereference in the
			// middle of the trace. Phobos indexes both the same way without a bounds check; it only
			// gets away with it because a house has to reach UpdateHarvesterProduction to do so, and
			// this trace runs for every house at the top of HouseClass::AI instead.
			auto const ruleAt = [](const TypeList<int>& list, int index, int fallback)
			{
				return (list.Items && index >= 0 && index < list.Count) ? list.Items[index] : fallback;
			};

			const int perRefinery = ruleAt(pRules->HarvestersPerRefinery, difficulty, -1);
			const int slaveMiners = ruleAt(pRules->AISlaveMinerNumber, difficulty, -1);

			const int maxHarvesters = (pHarvester && pRefinery)
				? perRefinery * pHouse->CountResourceDestinations
				: slaveMiners;

			const bool wouldReturnTrue = pHarvester
				&& pHouse->IQLevel2 >= pRules->Harvester
				&& !pHouse->IsTiberiumShort
				&& !pHouse->IsControlledByHuman()
				&& pHouse->CountResourceGatherers < maxHarvesters
				&& pHouse->TechLevel >= pHarvester->TechLevel;

			// The two rule values are printed raw as well as folded into the maximum, because -1 there
			// means the ruleset does not define that list at all rather than defining it as zero.
			Debug::Log("[Replay] Frame %d house %u harvester gate: producing %d, harvester [%s], "
				"refinery buildable [%s], gatherers %d < max %d (per refinery %d of %d, slave miners "
				"%d of %d, destinations %d, difficulty %d), IQ %d vs %d, short %d, human %d, "
				"tech %d vs %d -> %s.\n",
				frame, UniqueIDOf(pHouse), pHouse->ProducingUnitTypeIndex,
				pHarvester ? pHarvester->ID : "-", pRefinery ? pRefinery->ID : "-",
				pHouse->CountResourceGatherers, maxHarvesters,
				perRefinery, pRules->HarvestersPerRefinery.Count,
				slaveMiners, pRules->AISlaveMinerNumber.Count,
				pHouse->CountResourceDestinations,
				difficulty, pHouse->IQLevel2, pRules->Harvester, pHouse->IsTiberiumShort ? 1 : 0,
				pHouse->IsControlledByHuman() ? 1 : 0, pHouse->TechLevel,
				pHarvester ? pHarvester->TechLevel : -1,
				wouldReturnTrue ? "builds a harvester and skips the rolls" : "falls through to the rolls");
		}

		bool IsLoadInProgress()
		{
			return State.LoadInProgress;
		}

		void OnPlaybackStarted()
		{
			ResetDiagnostics();
			ResetDiagnosticMemory();
			State = SeekState {};

			const auto* pConfig = GetConfig();
			State.Interval = pConfig ? std::max(0, pConfig->ReplayRewindCheckpointInterval) : 0;

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

			ResetDiagnostics();
			ResetDiagnosticMemory();
			State = SeekState {};
		}

		void ResetDiagnostics()
		{
			ResetObjectWatch();
			ResetCellWatch();
			ResetLayerWatch();
			ResetBulletWatch();
			ResetWatchMemory();
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

			ServiceTraces();
			ServiceObjectWatch();
			ServiceBulletWatch();
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

		bool ShouldSkipRenderThisFrame()
		{
			if (!State.Seeking)
				return false;

			const auto* const pConfig = GetConfig();
			if (pConfig && pConfig->ReplaySeekRenderEveryFrame)
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
