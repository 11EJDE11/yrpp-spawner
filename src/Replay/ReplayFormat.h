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
		FrameRecordFlag_SelectionTriggers = 1u << 8
	};

	constexpr uint32_t KnownFrameRecordFlags = FrameRecordFlag_TacticalPos
		| FrameRecordFlag_Selection
		| FrameRecordFlag_SideChannel
		| FrameRecordFlag_GameCRC
		| FrameRecordFlag_Extensions
		| FrameRecordFlag_ObjectCensus
		| FrameRecordFlag_GameSpeed
		| FrameRecordFlag_RandomState
		| FrameRecordFlag_SelectionTriggers;

	constexpr uint32_t MaxFrameExtensionBytes = 1u << 20;
	constexpr uint32_t MaxEmbeddedFileBytes = 32u * 1024u * 1024u;
	constexpr int32_t MaxEventsPerFrame = 128 * 128;

	constexpr int32_t MaxSelectionTriggersPerFrame = 4096;

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
		uint32_t Reserved[16];
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

	static_assert(sizeof(ReplayHeader) == 1124, "ReplayHeader layout changed; update ReplayGame.cs and docs/replay-format.md");
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
	static_assert(offsetof(ReplayHeader, Reserved) == 1060, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");

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

	inline bool IsReplayHeaderValid(const ReplayHeader& header)
	{
		return header.Magic == ReplayMagic
			&& header.HeaderSize >= sizeof(ReplayHeader)
			&& header.UniqueIDCounter >= 0
			&& header.RandomNext1 >= 0 && header.RandomNext1 < 250
			&& header.RandomNext2 >= 0 && header.RandomNext2 < 250
			&& header.SpawnIniSize <= MaxEmbeddedFileBytes
			&& header.SpawnMapSize <= MaxEmbeddedFileBytes
			&& IsReplayGameSpeedIndexValid(header.RecordedGameSpeed);
	}
}
