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
#include <vector>

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

	// Which blocks a decoded frame record carried. In memory only: on disk each block is a tagged,
	// length-prefixed FrameBlockTag, and the hash is a bit of the record's first varint.
	enum FrameRecordFlags : uint32_t
	{
		FrameRecordFlag_None = 0u,
		FrameRecordFlag_TacticalPos = 1u << 0,
		FrameRecordFlag_Selection = 1u << 1,
		FrameRecordFlag_SideChannel = 1u << 2,
		FrameRecordFlag_GameCRC = 1u << 3,
		FrameRecordFlag_GameSpeed = 1u << 4,
		FrameRecordFlag_SelectionTriggers = 1u << 5,
	};

	// A frame record's blocks. Each is written as varint tag, varint length, then that many bytes, and
	// tag 0 ends the list. A reader skips a tag it does not know by its length, so a block can be added
	// without invalidating anything already recorded.
	enum FrameBlockTag : uint32_t
	{
		FrameBlock_End = 0,
		FrameBlock_TacticalPos = 1,       // zigzag dx, zigzag dy from the last recorded position
		FrameBlock_Selection = 2,         // varint count, then that many varint unique IDs
		FrameBlock_SideChannel = 3,       // varint count, then that many side-channel records
		FrameBlock_GameSpeed = 4,         // uint8 game speed index
		FrameBlock_SelectionTriggers = 5, // varint count, then that many varint unique IDs
		FrameBlock_HouseStats = 6,        // varint count, then that many HouseStatsSample
		FrameBlock_MoneyIn = 7,           // varint count, then that many MoneyInRecord
		FrameBlock_Seal = 8,              // SealRecord: the canonical hash chain so far
	};

	constexpr uint32_t MaxFrameBlockBytes = 1u << 20;
	constexpr uint32_t MaxEmbeddedFileBytes = 32u * 1024u * 1024u;
	constexpr int32_t MaxEventsPerFrame = 128 * 128;

	constexpr int32_t MaxSelectionTriggersPerFrame = 4096;
	constexpr int32_t MaxSelectedObjects = 4096;

	// EventClass::EventLength (0x8208EC): the payload bytes the network carries for each vanilla event
	// type, and so the bytes a received event holds. The rest of the 104-byte union is zero on a
	// receiver and leftover stack on the sender, so only these bytes are recorded.
	constexpr int VanillaEventTypeCount = 47;
	constexpr uint8_t VanillaEventLength[VanillaEventTypeCount] = {
		0, 5, 5, 4, 23, 24, 5, 5, 0, 5, 5, 16, 0, 4, 12, 12,
		12, 5, 8, 0, 16, 5, 5, 4, 4, 0, 0, 1, 7, 0, 10, 4,
		5, 2, 0, 4, 4, 104, 64, 0, 4, 5, 10, 0, 5, 4, 12,
	};
	constexpr size_t EventPayloadOffset = 7;
	constexpr size_t MaxEventPayload = 104;

	// FRAMEINFO's CRC is replaced on disk by the outcome of the check the engine makes with it: the
	// value is the same as the recorder's own hash unless that peer was out of sync.
	enum FrameInfoCheck : uint8_t
	{
		FrameInfoCheck_Mismatch = 0,
		FrameInfoCheck_Match = 1,
		FrameInfoCheck_NotChecked = 2,
	};

	// The canonical hash chain: a SHA-256 over the part of the recording every player's copy shares,
	// sealed every SealIntervalFrames. See docs/replay-format.md.
	constexpr int SealIntervalFrames = 900;
	constexpr size_t SealHashBytes = 32;
	// The salted fingerprint a chat message's ChatFingerprint event carries.
	constexpr size_t ChatSaltBytes = 16;
	constexpr size_t ChatFingerprintBytes = 16;

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
	// uint32 count, then that many SealRecord: every seal the frame stream carries, and a last one
	// over whatever followed the final full interval.
	constexpr uint32_t StatisticsChunk_Seals = MakeChunkTag('S', 'E', 'A', 'L');

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

	// In memory. On disk: uint8 type, zigzag house, zigzag aux, zigzag X/Y/Z, varint length + UTF-8
	// sender name, varint length + UTF-8 text, then for chat a uint8 of ChatRecordFlags, the salt when
	// it has one, and the label and recipients when it has those.
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
		// ChatMessage only: the salt the sender's ChatFingerprint event was computed with.
		bool HasSalt = false;
		uint8_t Salt[ChatSaltBytes] = {};
		// ChatMessage only: who the sender sent it to, as a ChatLabel and a bit per house index.
		// Both 0 when the sender did not say.
		uint8_t Label = 0;
		uint8_t Recipients = 0;
	};

	enum ChatRecordFlags : uint8_t
	{
		ChatRecordFlag_Salt = 1u << 0,
		ChatRecordFlag_Recipients = 1u << 1,
	};

#pragma pack(push, 1)

	struct SealRecord
	{
		int32_t ThroughFrame;          // the last frame the chain covers
		uint8_t Hash[SealHashBytes];
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

		// Bytes on disk. Both files are raw deflate; their inflated sizes are SpawnIniRawSize and
		// SpawnMapRawSize.
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
		// whole section is on disk. The section is raw deflate; StatisticsRawSize is its inflated size.
		uint64_t StatisticsOffset;
		uint32_t StatisticsSize;
		uint32_t SpawnIniRawSize;
		uint32_t SpawnMapRawSize;
		uint32_t StatisticsRawSize;
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

	// One payment through HouseClass::Refund_Money (0x4F9950): who received it, how much, and the
	// caller's return address, which readers map to the kind of income. Summed per house and caller per frame.
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

#pragma pack(pop)

	static_assert(sizeof(ReplayHeader) == 1096, "ReplayHeader layout changed; update YRReplayGame.cs and docs/replay-format.md");
	static_assert(sizeof(HouseStatsSample) == 84, "HouseStatsSample layout changed; update docs/replay-format.md");
	static_assert(sizeof(StatisticsHouseRecord) == 282, "StatisticsHouseRecord layout changed; update docs/replay-format.md");
	static_assert(sizeof(MoneyInRecord) == 12, "MoneyInRecord layout changed; update docs/replay-format.md");
	static_assert(sizeof(StatisticsGameRecord) == 20, "StatisticsGameRecord layout changed; update docs/replay-format.md");
	static_assert(sizeof(SealRecord) == 36, "SealRecord layout changed; update docs/replay-format.md");

	static_assert(offsetof(ReplayHeader, Magic) == 0, "Replay header offsets changed; update YRReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, Version) == 4, "Replay header offsets changed; update YRReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, HeaderSize) == 8, "Replay header offsets changed; update YRReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, SpawnIniSize) == 1032, "Replay header offsets changed; update YRReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, SpawnMapSize) == 1036, "Replay header offsets changed; update YRReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, RecordedGameSpeed) == 1040, "Replay header offsets changed; update YRReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, RecordedUnixTime) == 1044, "Replay header offsets changed; update YRReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, TotalFrames) == 1052, "Replay header offsets changed; update YRReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, Flags) == 1056, "Replay header offsets changed; update YRReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, CheckpointArchiveOffset) == 1060, "Replay header offsets changed; update YRReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, CheckpointArchiveSize) == 1068, "Replay header offsets changed; update YRReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, StatisticsOffset) == 1072, "Replay header offsets changed; update YRReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, StatisticsSize) == 1080, "Replay header offsets changed; update YRReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, SpawnIniRawSize) == 1084, "Replay header offsets changed; update YRReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, SpawnMapRawSize) == 1088, "Replay header offsets changed; update YRReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, StatisticsRawSize) == 1092, "Replay header offsets changed; update YRReplayGame.cs and docs/replay-format.md");


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

	// LEB128: seven bits a byte, low first, the top bit set on every byte but the last.
	inline void AppendVarint(std::vector<uint8_t>& out, uint32_t value)
	{
		while (value >= 0x80u)
		{
			out.push_back(static_cast<uint8_t>(value | 0x80u));
			value >>= 7;
		}
		out.push_back(static_cast<uint8_t>(value));
	}

	// Small signed values as small varints: 0, -1, 1, -2, 2 ... become 0, 1, 2, 3, 4 ...
	inline uint32_t ZigZagEncode(int32_t value)
	{
		return (static_cast<uint32_t>(value) << 1) ^ static_cast<uint32_t>(value >> 31);
	}

	inline int32_t ZigZagDecode(uint32_t value)
	{
		return static_cast<int32_t>(value >> 1) ^ -static_cast<int32_t>(value & 1u);
	}

}
