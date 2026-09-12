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


#include <GeneralStructures.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Replay
{
	constexpr uint32_t ReplayMagic = 0x50525259u; // 'YRRP'
	constexpr uint32_t ReplayVersion = 1;
	constexpr uint32_t MaxCheckpointArchiveBytes = 32u * 1024u * 1024u;
	constexpr int MaxGameSpeedIndex = 6;

	enum ReplayHeaderFlags : uint32_t
	{
		ReplayHeaderFlag_None = 0u,
		// Recording reached StopReplaySystem instead of dying with the process. Without it a
		// reader cannot tell a crashed recording from one that was quit on frame 0.
		ReplayHeaderFlag_CleanShutdown = 1u << 0
	};

	// Which optional blocks follow a frame record's header, written and read in this order.
	enum FrameRecordFlags : uint32_t
	{
		FrameRecordFlag_None = 0u,
		FrameRecordFlag_TacticalPos = 1u << 0,
		FrameRecordFlag_Selection = 1u << 1,
		FrameRecordFlag_SideChannel = 1u << 2,
		FrameRecordFlag_GameCRC = 1u << 3,
		FrameRecordFlag_Extensions = 1u << 4,
		FrameRecordFlag_ObjectCensus = 1u << 5,
		FrameRecordFlag_GameSpeed = 1u << 6,
		FrameRecordFlag_RandomState = 1u << 7,
		FrameRecordFlag_SelectionTriggers = 1u << 8,
		FrameRecordFlag_HouseStats = 1u << 9,
		FrameRecordFlag_MoneyIn = 1u << 10
	};

	constexpr uint32_t KnownFrameRecordFlags = FrameRecordFlag_TacticalPos
		| FrameRecordFlag_Selection
		| FrameRecordFlag_SideChannel
		| FrameRecordFlag_GameCRC
		| FrameRecordFlag_Extensions
		| FrameRecordFlag_ObjectCensus
		| FrameRecordFlag_GameSpeed
		| FrameRecordFlag_RandomState
		| FrameRecordFlag_SelectionTriggers
		| FrameRecordFlag_HouseStats
		| FrameRecordFlag_MoneyIn;

	constexpr uint32_t MaxFrameExtensionBytes = 1u << 20;
	constexpr uint32_t MaxEmbeddedFileBytes = 32u * 1024u * 1024u;
	constexpr int32_t MaxEventsPerFrame = 128 * 128;

	constexpr int32_t MaxSelectionTriggersPerFrame = 4096;

	// One economy/army sample per house every this many frames: a second of game time at the
	// fastest speed, which is finer than any end-of-game chart needs and costs 84 bytes a house.
	constexpr int HouseStatsIntervalFrames = 60;
	constexpr int32_t MaxHouseStatsPerFrame = 32;

	// The statistics section after the frame stream. See docs/replay-format.md.
	constexpr uint32_t MaxStatisticsSectionBytes = 8u * 1024u * 1024u;
	constexpr uint32_t MakeChunkTag(char a, char b, char c, char d)
	{
		return static_cast<uint32_t>(static_cast<uint8_t>(a))
			| (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8)
			| (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16)
			| (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
	}
	// Each chunk is uint32 tag, uint32 length, then that many bytes; readers skip tags they do not know.
	constexpr uint32_t StatisticsChunk_Types = MakeChunkTag('T', 'Y', 'P', 'E');
	constexpr uint32_t StatisticsChunk_Houses = MakeChunkTag('H', 'O', 'U', 'S');
	constexpr uint32_t StatisticsChunk_StatsPacket = MakeChunkTag('S', 'T', 'A', 'T');
	constexpr uint32_t StatisticsChunk_Game = MakeChunkTag('G', 'A', 'M', 'E');
	constexpr uint32_t StatisticsChunk_Modules = MakeChunkTag('M', 'O', 'D', 'S');

	constexpr int32_t MaxMoneyInPerFrame = 1024;

	// HouseStatsSample::Flags and StatisticsHouseRecord::Flags.
	enum HouseStatsFlags : uint32_t
	{
		HouseStatsFlag_Defeated = 1u << 0,
		HouseStatsFlag_Winner = 1u << 1,
		HouseStatsFlag_Loser = 1u << 2,
		HouseStatsFlag_Observer = 1u << 3,
		HouseStatsFlag_Human = 1u << 4,
		HouseStatsFlag_LostConnection = 1u << 5,
		HouseStatsFlag_Resigned = 1u << 6,
		HouseStatsFlag_RecordingPlayer = 1u << 7,
	};

	// StatisticsChunk_Types entry flags.
	enum StatisticsTypeFlags : uint32_t
	{
		StatisticsTypeFlag_Naval = 1u << 0,
		StatisticsTypeFlag_DontScore = 1u << 1,
		StatisticsTypeFlag_Insignificant = 1u << 2,
	};

	// The per-type count arrays that follow each StatisticsHouseRecord, in this order. Indices are
	// positions in the matching type array, except CollectedCrates, which is indexed by crate type.
	enum StatisticsHouseArray : uint32_t
	{
		StatisticsHouseArray_BuiltAircraft,
		StatisticsHouseArray_BuiltInfantry,
		StatisticsHouseArray_BuiltUnits,
		StatisticsHouseArray_BuiltBuildings,
		StatisticsHouseArray_KilledAircraft,
		StatisticsHouseArray_KilledInfantry,
		StatisticsHouseArray_KilledUnits,
		StatisticsHouseArray_KilledBuildings,
		StatisticsHouseArray_CapturedBuildings,
		StatisticsHouseArray_CollectedCrates,
		StatisticsHouseArray_LeftAircraft,
		StatisticsHouseArray_LeftInfantry,
		StatisticsHouseArray_LeftUnits,
		StatisticsHouseArray_LeftBuildings,
		// The engine keeps only totals of what a house lost; these are counted by the recorder,
		// on the same condition the engine counts the totals on.
		StatisticsHouseArray_LostAircraft,
		StatisticsHouseArray_LostInfantry,
		StatisticsHouseArray_LostUnits,
		StatisticsHouseArray_LostBuildings,
		StatisticsHouseArray_Count
	};

	// Non-deterministic network and UI events, recorded separately from EventClass::DoList.
	enum class SideChannelEventType : uint8_t
	{
		ChatMessage = 1,
		BeaconPlace = 2,
		BeaconDelete = 3,
		BeaconText = 4,
		Taunt = 5,
	};

	constexpr size_t SideChannelTextLength = 128; // matches BeaconClass::Text, and fits a chat line
	constexpr size_t SideChannelNameLength = 24;
	constexpr int32_t SideChannelMaxEventsPerFrame = 64; // bound for playback parsing
	// BeaconManagerClass::Beacons is [8][3].
	constexpr int MaxHouses = 8;
	constexpr int MaxBeaconSlots = 3;
	// The cell grid is a fixed 512x512, at 256 leptons per cell.
	constexpr int32_t MaxMapLeptonCoord = 512 * 256;

#pragma pack(push, 1)

	struct SideChannelRecord
	{
		int32_t FrameNumber = 0;
		uint8_t Type = 0; // SideChannelEventType
		int32_t House = -1;
		// Per-type payload: chat color, beacon slot or taunt command.
		int32_t Aux = 0;
		CoordStruct Coord = {}; // BeaconPlace only
		wchar_t SenderName[SideChannelNameLength] = {}; // ChatMessage only
		wchar_t Text[SideChannelTextLength] = {}; // ChatMessage and BeaconText only
	};

	struct ReplayHeader
	{
		uint32_t Magic;
		uint32_t Version;
		uint32_t HeaderSize;
		uint32_t GameMode;

		int UniqueIDCounter;
		int Seed;
		int RandomNext1;
		int RandomNext2;
		uint32_t RandomizerTable[250];

		uint32_t SpawnIniSize;
		uint32_t SpawnMapSize;
		uint32_t RecordedGameSpeed;

		uint64_t RecordedUnixTime;
		uint32_t TotalFrames;
		uint32_t Flags;
		// Absolute file offset and size of the checkpoint archive after the frame stream. Both stay
		// zero when the recording captured no saves or never finalized.
		uint64_t CheckpointArchiveOffset;
		uint32_t CheckpointArchiveSize;
		// Absolute file offset and size of the statistics section, likewise stamped only once the
		// whole section is on disk.
		uint64_t StatisticsOffset;
		uint32_t StatisticsSize;
	};

	// One house's economy and army at a sampled frame. Every field is a plain read of engine state;
	// nothing here is computed by calling into the simulation.
	struct HouseStatsSample
	{
		int32_t HouseIndex;
		int32_t Credits;          // HouseClass::Balance
		int32_t StoredOreValue;   // OwnedTiberium.GetTotalValue(): ore sitting in refineries and silos
		int32_t CreditsSpent;
		int32_t HarvestedCredits;
		int32_t PowerOutput;
		int32_t PowerDrain;
		int32_t Units;            // OwnedUnits, which includes ships
		int32_t Infantry;
		int32_t Aircraft;
		int32_t Buildings;
		int32_t ArmyValue;        // summed type Cost of live units, infantry and aircraft
		int32_t BuildingValue;
		int32_t UnitsKilled;      // units of other houses this house destroyed
		int32_t BuildingsKilled;
		int32_t UnitsLost;        // the field YRpp calls TotalKilledUnits; see ReplayStatistics.cpp
		int32_t BuildingsLost;
		int32_t UnitsBuilt;       // Built{Aircraft,Infantry,Unit}Types totals
		int32_t BuildingsBuilt;
		int32_t Score;            // HouseClass::PointTotal
		uint32_t Flags;           // HouseStatsFlags
	};

	// One payment into a house's balance through HouseClass::Refund_Money (0x4F9950), recorded raw:
	// who received it, how much, and the address the call returns to. Which kind of income a caller
	// is - harvest, oil derrick, sale - is decided by readers, so the recorder never has to follow
	// Ares or Phobos moving it. Payments to the same house from the same caller in one frame are summed.
	struct MoneyInRecord
	{
		uint8_t House;
		uint8_t Reserved[3];
		uint32_t Caller;          // absolute return address; see StatisticsChunk_Modules to resolve it
		int32_t Amount;
	};

	// The fixed part of one house's end-of-game record in StatisticsChunk_Houses.
	struct StatisticsHouseRecord
	{
		int32_t HouseIndex;
		wchar_t Name[21];         // HouseClass::UIName: the player's name in multiplayer
		char Country[0x18];       // HouseTypeClass::ID
		int32_t ColorSchemeIndex;
		int32_t SpawnPosition;
		uint32_t Allies;          // bit per house index
		uint32_t Flags;           // HouseStatsFlags
		int32_t Credits;
		int32_t StoredOreValue;
		int32_t CreditsSpent;
		int32_t HarvestedCredits;
		int32_t PowerOutput;
		int32_t PowerDrain;
		int32_t UnitsLost;
		int32_t BuildingsLost;
		int32_t Score;
		// Of each other house, how many units and buildings this house destroyed.
		int32_t UnitsKilledOfHouse[20];
		int32_t BuildingsKilledOfHouse[20];
	};

	// StatisticsChunk_Game: facts about the game as a whole at the moment the recording closed.
	struct StatisticsGameRecord
	{
		uint64_t EndUnixTime;     // time() at close; with the header's RecordedUnixTime, the wall-clock length
		int32_t EndFrame;
		int32_t OutOfSyncFrame;   // first frame the game's OutOfSync flag was seen set, or -1
		uint8_t OutOfSync;        // OutOfSync (0xA8B8C2) at close
		uint8_t SawCompletion;    // SawCompletion (0xA8B8C1): the game reached its own end
		uint8_t Reserved[2];
	};

	// The randomiser's two table cursors. Enough to tell a drifted randomiser from a drifted
	// simulation, without putting a kilobyte of table in every frame.
	struct FrameRandomState
	{
		int32_t Next1;
		int32_t Next2;
	};

	struct FrameObjectCensus
	{
		int32_t AbstractCount;
		int32_t ScenarioUniqueID;
	};

	struct FrameRecordHeader
	{
		int32_t FrameNumber;         // -1 marks the end of the stream
		int32_t EventCountThisFrame;
		uint32_t Flags;              // FrameRecordFlags
	};

#pragma pack(pop)

	static_assert(sizeof(ReplayHeader) == 1084, "ReplayHeader layout changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(sizeof(HouseStatsSample) == 84, "HouseStatsSample layout changed; update docs/replay-format.md");
	static_assert(sizeof(StatisticsHouseRecord) == 282, "StatisticsHouseRecord layout changed; update docs/replay-format.md");
	static_assert(sizeof(MoneyInRecord) == 12, "MoneyInRecord layout changed; update docs/replay-format.md");
	static_assert(sizeof(StatisticsGameRecord) == 20, "StatisticsGameRecord layout changed; update docs/replay-format.md");
	static_assert(sizeof(FrameRecordHeader) == 12, "FrameRecordHeader layout changed; update docs/replay-format.md");
	static_assert(sizeof(FrameObjectCensus) == 8, "FrameObjectCensus layout changed; update docs/replay-format.md");
	static_assert(sizeof(FrameRandomState) == 8, "FrameRandomState layout changed; update docs/replay-format.md");
	static_assert(sizeof(SideChannelRecord) == 329, "SideChannelRecord layout changed; update docs/replay-format.md");

	static_assert(offsetof(ReplayHeader, Magic) == 0, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, Version) == 4, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, HeaderSize) == 8, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, SpawnIniSize) == 1032, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, SpawnMapSize) == 1036, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, RecordedGameSpeed) == 1040, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, RecordedUnixTime) == 1044, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, TotalFrames) == 1052, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, Flags) == 1056, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, CheckpointArchiveOffset) == 1060, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, CheckpointArchiveSize) == 1068, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, StatisticsOffset) == 1072, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, StatisticsSize) == 1080, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");

	static_assert(offsetof(FrameRecordHeader, FrameNumber) == 0, "FrameRecordHeader layout changed; update docs/replay-format.md");
	static_assert(offsetof(FrameRecordHeader, EventCountThisFrame) == 4, "FrameRecordHeader layout changed; update docs/replay-format.md");
	static_assert(offsetof(FrameRecordHeader, Flags) == 8, "FrameRecordHeader layout changed; update docs/replay-format.md");

	inline bool IsReplayGameSpeedIndexValid(uint32_t gameSpeedIndex)
	{
		return gameSpeedIndex <= static_cast<uint32_t>(MaxGameSpeedIndex);
	}

	// The mapping Queue_AI_Multiplayer uses: 0 -> 60, 1 -> 45, 2 and up -> 60 / gameSpeed.
	inline int GetReplayFPSFromGameSpeed(int gameSpeed)
	{
		gameSpeed = std::clamp(gameSpeed, 0, MaxGameSpeedIndex);

		if (gameSpeed <= 0)
			return 60;

		if (gameSpeed == 1)
			return 45;

		return std::max(1, 60 / gameSpeed);
	}

}
