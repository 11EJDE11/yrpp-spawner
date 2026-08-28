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

// On-disk layout of a .yrrp replay file, and nothing else - no engine state, no hooks.
//
// The client mirrors this by hand in DXMainClient/Domain/ReplayGame.cs, and docs/replay-format.md
// writes it out; there is no compile-time link between the three, so a change here has to be made
// in all of them together. The static_asserts below turn a layout change into a build error rather
// than a silent misparse on the reading side. Read "Changing the format" in that doc first:
// additive changes keep the version where it is, and only a moved or repurposed field bumps it.

#include <GeneralStructures.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Replay
{

constexpr uint32_t REPLAY_MAGIC = 0x4A455259u;
constexpr uint32_t REPLAY_VERSION = 1;
constexpr uint32_t MIN_SUPPORTED_REPLAY_VERSION = 1;
// The engine's GameSpeed is an index into a fixed table; anything above this is out of range.
constexpr int MAX_GAME_SPEED_INDEX = 6;

enum ReplayHeaderFlags : uint32_t
{
	ReplayHeaderFlag_None = 0u,
	// Recording reached StopReplaySystem instead of dying with the process. Without it a reader
	// cannot tell a crashed recording from one that was quit on frame 0.
	ReplayHeaderFlag_CleanShutdown = 1u << 0
};

enum FrameRecordFlags : uint32_t
{
	FrameRecordFlag_None = 0u,
	FrameRecordFlag_TacticalPos = 1u << 0,
	FrameRecordFlag_Selection = 1u << 1,
	FrameRecordFlag_SideChannel = 1u << 2,
	FrameRecordFlag_GameCRC = 1u << 3,
	FrameRecordFlag_Extensions = 1u << 4,
	FrameRecordFlag_ObjectCensus = 1u << 5,
	// The game speed changed on this frame; an int32 index follows. Written only when it
	// changes, because it almost never does - and a single player game changes it without any
	// event, so the stream is the only place playback can learn about it.
	FrameRecordFlag_GameSpeed = 1u << 6
};

constexpr uint32_t KNOWN_FRAME_RECORD_FLAGS = FrameRecordFlag_TacticalPos
	| FrameRecordFlag_Selection
	| FrameRecordFlag_SideChannel
	| FrameRecordFlag_GameCRC
	| FrameRecordFlag_Extensions
	| FrameRecordFlag_ObjectCensus
	| FrameRecordFlag_GameSpeed;

constexpr uint32_t MAX_FRAME_EXTENSION_BYTES = 1u << 20;

// Non-deterministic network/UI events are recorded separately from EventClass::DoList.
enum class SideChannelEventType : uint8_t
{
	ChatMessage = 1,
	BeaconPlace = 2,
	BeaconDelete = 3,
	BeaconText = 4,
	Taunt = 5,
};

constexpr size_t SIDECHANNEL_TEXT_LENGTH = 128; // matches BeaconClass::Text and comfortably fits chat
constexpr size_t SIDECHANNEL_NAME_LENGTH = 24;
constexpr int32_t SIDECHANNEL_MAX_EVENTS_PER_FRAME = 64; // sanity bound for playback parsing
// BeaconManagerClass::Beacons is [8][3].
constexpr int MAX_HOUSES = 8;
constexpr int MAX_BEACON_SLOTS = 3;
// MapClass_Array is a fixed 512x512 cell grid (0x40000 entries); 256 leptons per cell.
constexpr int32_t MAX_MAP_LEPTON_COORD = 512 * 256;

#pragma pack(push, 1)
struct SideChannelRecord
{
	int32_t FrameNumber = 0;
	uint8_t Type = 0;        // SideChannelEventType
	int32_t House = -1;
	// Per-type payload: chat color, beacon slot or taunt command.
	int32_t Aux = 0;
	CoordStruct Coord = {}; // BeaconPlace only
	wchar_t SenderName[SIDECHANNEL_NAME_LENGTH] = {}; // ChatMessage only
	wchar_t Text[SIDECHANNEL_TEXT_LENGTH] = {}; // ChatMessage / BeaconText only
};
#pragma pack(pop)

#pragma pack(push, 1)
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

struct FrameObjectCensus
{
	int32_t AbstractCount;
	int32_t ScenarioUniqueID;
};

struct FrameRecordHeader
{
	int32_t FrameNumber;         // -1 indicates end-of-stream marker
	int32_t EventCountThisFrame;
	uint32_t Flags;              // FrameRecordFlags
};

#pragma pack(pop)

// Mirrored by hand in the client's ReplayGame.cs and in docs/replay-format.md. A size change is
// silent on the reading side, so pin it here and make the mismatch a build error instead.
static_assert(sizeof(ReplayHeader) == 1452, "ReplayHeader layout changed; update ReplayGame.cs and docs/replay-format.md");
static_assert(sizeof(FrameRecordHeader) == 12, "FrameRecordHeader layout changed; update docs/replay-format.md");
static_assert(sizeof(FrameObjectCensus) == 8, "FrameObjectCensus layout changed; update docs/replay-format.md");
static_assert(sizeof(SideChannelRecord) == 329, "SideChannelRecord layout changed; update docs/replay-format.md");

// Size alone does not pin a layout - swapping two fields of the same width leaves sizeof untouched
// and misparses everything after them - so pin each offset the client hardcodes individually.
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

// The four version bytes are read as one group by anything reporting which spawner recorded a file.
static_assert(offsetof(ReplayHeader, SpawnerVersionPatch) == offsetof(ReplayHeader, SpawnerVersionMajor) + 3,
	"Spawner version bytes have to stay adjacent and in order");

static_assert(offsetof(FrameRecordHeader, FrameNumber) == 0, "FrameRecordHeader layout changed; update docs/replay-format.md");
static_assert(offsetof(FrameRecordHeader, EventCountThisFrame) == 4, "FrameRecordHeader layout changed; update docs/replay-format.md");
static_assert(offsetof(FrameRecordHeader, Flags) == 8, "FrameRecordHeader layout changed; update docs/replay-format.md");

inline bool IsReplayGameSpeedIndexValid(uint32_t gameSpeedIndex)
{
	return gameSpeedIndex <= static_cast<uint32_t>(MAX_GAME_SPEED_INDEX);
}

inline int GetReplayFPSFromGameSpeed(int gameSpeed)
{
	gameSpeed = std::clamp(gameSpeed, 0, MAX_GAME_SPEED_INDEX);

	// Vanilla mapping used by Queue_AI_Multiplayer:
	// 0 -> 60, 1 -> 45, 2+ -> 60 / gameSpeed.
	if (gameSpeed <= 0)
		return 60;

	if (gameSpeed == 1)
		return 45;

	return std::max(1, 60 / gameSpeed);
}

// Whether this build understands a file's layout generation at all. Nothing to do with whether the
// recorded game will reproduce: that is what the client's per-file hash check answers, and a replay
// can pass this and still diverge because the rules or the engine moved underneath it.
inline bool IsReplayVersionSupported(uint32_t version)
{
	return version >= MIN_SUPPORTED_REPLAY_VERSION && version <= REPLAY_VERSION;
}

inline bool IsReplayHeaderValid(const ReplayHeader& header)
{
	return header.Magic == REPLAY_MAGIC
		&& IsReplayVersionSupported(header.Version)
		&& header.HeaderSize >= sizeof(ReplayHeader)
		&& IsReplayGameSpeedIndexValid(header.RecordedGameSpeed);
}

} // namespace Replay
