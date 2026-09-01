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

// On-disk layout of a .yrrp replay file. No engine state and no hooks.
//
// The CnCNet client mirrors this by hand in DXMainClient/Domain/ReplayGame.cs, and
// docs/replay-format.md writes it out. Nothing links the three at build time, so a change here has
// to be made in all of them together; the static_asserts below turn a layout change into a build
// error instead of a silent misparse. See "Changing the format" in that doc: additive changes keep
// the version where it is, only a moved or repurposed field bumps it.

#include <GeneralStructures.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Replay
{
	constexpr uint32_t ReplayMagic = 0x50525259u; // 'YRRP'
	constexpr uint32_t ReplayVersion = 1;
	constexpr uint32_t MinSupportedReplayVersion = 1;
	// GameSpeed is an index into a fixed table; anything above this is out of range.
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
		// The game speed changed on this frame; an int32 index follows. A single player game
		// changes it without queueing an event, so the stream is the only place playback can
		// learn about it.
		FrameRecordFlag_GameSpeed = 1u << 6,
		// Where the scenario randomiser stood after this frame's hash was taken. Compute_Game_CRC
		// draws from that randomiser, so a hash that differs while the objects match means the
		// randomiser drifted rather than the simulation - two very different bugs. A FrameRandomState
		// follows.
		FrameRecordFlag_RandomState = 1u << 7
	};

	constexpr uint32_t KnownFrameRecordFlags = FrameRecordFlag_TacticalPos
		| FrameRecordFlag_Selection
		| FrameRecordFlag_SideChannel
		| FrameRecordFlag_GameCRC
		| FrameRecordFlag_Extensions
		| FrameRecordFlag_ObjectCensus
		| FrameRecordFlag_GameSpeed
		| FrameRecordFlag_RandomState;

	constexpr uint32_t MaxFrameExtensionBytes = 1u << 20;

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
		char MapName[260];
		uint8_t SpawnerVersionMajor;
		uint8_t SpawnerVersionMinor;
		uint8_t SpawnerVersionRevision;
		uint8_t SpawnerVersionPatch;
		char GameClientVersion[64];
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

	// Size alone does not pin a layout - swapping two fields of the same width leaves sizeof
	// untouched and misparses everything after them - so the offsets the client hardcodes are
	// pinned one by one as well.
	static_assert(sizeof(ReplayHeader) == 1452, "ReplayHeader layout changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(sizeof(FrameRecordHeader) == 12, "FrameRecordHeader layout changed; update docs/replay-format.md");
	static_assert(sizeof(FrameObjectCensus) == 8, "FrameObjectCensus layout changed; update docs/replay-format.md");
	static_assert(sizeof(FrameRandomState) == 8, "FrameRandomState layout changed; update docs/replay-format.md");
	static_assert(sizeof(SideChannelRecord) == 329, "SideChannelRecord layout changed; update docs/replay-format.md");

	static_assert(offsetof(ReplayHeader, Magic) == 0, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, Version) == 4, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, HeaderSize) == 8, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, MapName) == 12, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, SpawnerVersionMajor) == 272, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, GameClientVersion) == 276, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, SpawnIniSize) == 1360, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, SpawnMapSize) == 1364, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, RecordedGameSpeed) == 1368, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, RecordedUnixTime) == 1372, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, TotalFrames) == 1380, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, Flags) == 1384, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");
	static_assert(offsetof(ReplayHeader, Reserved) == 1388, "Replay header offsets changed; update ReplayGame.cs and docs/replay-format.md");

	// Anything reporting which spawner recorded a file reads the four version bytes as one group.
	static_assert(offsetof(ReplayHeader, SpawnerVersionPatch) == offsetof(ReplayHeader, SpawnerVersionMajor) + 3,
		"Spawner version bytes have to stay adjacent and in order");

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

	// Whether this build understands a file's layout generation. Nothing to do with whether the
	// recorded game will reproduce - that is what the client's per-file hash check answers, and a
	// replay can pass this and still diverge because the rules or the engine moved underneath it.
	inline bool IsReplayVersionSupported(uint32_t version)
	{
		return version >= MinSupportedReplayVersion && version <= ReplayVersion;
	}

	inline bool IsReplayHeaderValid(const ReplayHeader& header)
	{
		return header.Magic == ReplayMagic
			&& IsReplayVersionSupported(header.Version)
			&& header.HeaderSize >= sizeof(ReplayHeader)
			&& IsReplayGameSpeedIndexValid(header.RecordedGameSpeed);
	}
}
