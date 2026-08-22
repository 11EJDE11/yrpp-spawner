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

#include "ReplaySystem.h"
#include "ReplayStream.h"

#include <Spawner/Spawner.h>
#include <Utilities/Debug.h>
#include <Ext/Event/Body.h>
#include <Ext/INIClass/Body.h>

#include <BeaconManagerClass.h>
#include <ColorScheme.h>
#include <EventClass.h>
#include <GameModeOptionsClass.h>
#include <GameOptionsClass.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <MessageListClass.h>
#include <ProgressScreenClass.h>
#include <RulesClass.h>
#include <SessionClass.h>
#include <VocClass.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <deque>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{

constexpr uint32_t REPLAY_MAGIC = 0x4A455259u;
constexpr uint32_t REPLAY_VERSION = 1;
// How often the compressed stream is given a decodable point. A recording cut short by a crash
// loses at most this many frames - one second of play - and the cadence costs about 1% of ratio.
constexpr int REPLAY_SYNC_FLUSH_FRAME_INTERVAL = 60;
// Flush rarely; forced disk commits can stall the game thread.
constexpr uint64_t REPLAY_FLUSH_INTERVAL_BYTES = 50ull * 1024 * 1024;
constexpr const char* DEFAULT_RECORDING_PATH = "replay.dat";
// The engine's GameSpeed is an index into a fixed table; anything above this is out of range.
constexpr int MAX_GAME_SPEED_INDEX = 6;
// Queue_AI_Multiplayer's network delay budget.
constexpr uintptr_t NETWORK_DELAY_TIME_ADDRESS = 0x00AFA458;
// BeaconClass::Bitfield flag marking the beacon the local player placed.
constexpr int BEACON_FLAG_LOCAL = 2;

enum FrameRecordFlags : uint32_t
{
	FrameRecordFlag_None = 0u,
	FrameRecordFlag_TacticalPos = 1u << 0,
	FrameRecordFlag_Selection = 1u << 1,
	FrameRecordFlag_SideChannel = 1u << 2
};

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
	char MapName[260];
	uint32_t StartFrame;
	uint8_t SpawnerVersionMajor;
	uint8_t SpawnerVersionMinor;
	uint8_t SpawnerVersionRevision;
	uint8_t SpawnerVersionPatch;
	char GameVersionString[32];
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
};

struct FrameStateRecord
{
	int FrameNumber;
	Point2D TacticalPos;
	int32_t SelectedObjectCount;
	uint32_t SelectedObjectCRC;
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
static_assert(sizeof(ReplayHeader) == 1416, "ReplayHeader layout changed; update ReplayGame.cs and docs/replay-format.md");
static_assert(sizeof(FrameRecordHeader) == 12, "FrameRecordHeader layout changed; update docs/replay-format.md");
static_assert(sizeof(SideChannelRecord) == 329, "SideChannelRecord layout changed; update docs/replay-format.md");

struct PlaybackFrameRecord
{
	int32_t FrameNumber = 0;
	int32_t EventCountThisFrame = 0;
	uint32_t Flags = FrameRecordFlag_None;
	Point2D TacticalPos = { 0, 0 };
	int32_t SelectedObjectCount = 0;
	uint32_t SelectedObjectCRC = 0;
	std::vector<uint32_t> SelectedObjectIDs;
	std::vector<SideChannelRecord> SideChannelEvents;
	bool EndOfStream = false;
};

struct PendingRecordedFrameCapture
{
	FrameStateRecord FrameState;
	std::vector<uint32_t> SelectedObjectIDs;
};

struct ReplayRuntimeState
{
	bool Recording = false;
	bool Playback = false;
	bool InitRandomHandled = false;
	bool PlayersMarkedLoaded = false;

	bool ShroudEnabled = false;
	bool LockViewport = true;
	bool SelectUnits = true;
	bool SpectatorView = false;
	// Playback-only; recording keeps this data unconditionally.
	bool ShowChatAndBeacons = true;
	int PlaybackSpeedIndex = -1;

	int ExpectedEventsThisFrame = 0;
	uint64_t BytesAtLastDiskFlush = 0;
	int LastSyncFlushFrame = 0;

	HANDLE ReplayFile = INVALID_HANDLE_VALUE;
	// Only one of these is ever active: recording deflates, playback inflates.
	Replay::DeflateWriter Writer;
	Replay::InflateReader Reader;

	char PlaybackPath[MAX_PATH] = { 0 };
	std::deque<PendingRecordedFrameCapture> PendingFrameStates;
	std::deque<SideChannelRecord> PendingSideChannelEvents;
	ReplayHeader PlaybackHeader = {};
	bool HasPlaybackHeader = false;

	bool HasPendingPlaybackFrame = false;
	bool PlaybackStreamEnded = false;

	// Last recorded viewport position, re-applied between sparse frame records.
	Point2D LockedViewportPos = { 0, 0 };
	bool HasLockedViewportPos = false;
	PlaybackFrameRecord PendingPlaybackFrame = {};

	bool HasLastWrittenFrameState = false;
	int32_t LastWrittenFrameNumber = 0;
	Point2D LastRecordedTacticalPos = { 0, 0 };
	int32_t LastRecordedSelectionCount = 0;
	uint32_t LastRecordedSelectionCRC = 0;
	std::vector<uint32_t> LastRecordedSelectionIDs;
};

ReplayRuntimeState gReplay;

void StopReplaySystem();
void AbortReplaySystem();
void ApplyPlaybackInitialState();

const SpawnerConfig* GetConfig()
{
	return Spawner::GetConfig();
}

const char* GetRecordingOutputPath()
{
	const auto* pConfig = GetConfig();
	if (pConfig && pConfig->ReplayFileOut[0] != '\0')
		return pConfig->ReplayFileOut;

	return DEFAULT_RECORDING_PATH;
}

void EnsureParentDirectoryExists(const char* path)
{
	char buffer[MAX_PATH];
	strncpy_s(buffer, sizeof(buffer), path, _TRUNCATE);

	for (char* cursor = buffer; *cursor != '\0'; ++cursor)
	{
		if (*cursor != '\\' && *cursor != '/')
			continue;

		// Skip a leading separator, a drive root and the second slash of a UNC prefix.
		if (cursor == buffer || *(cursor - 1) == ':' || *(cursor - 1) == '\\' || *(cursor - 1) == '/')
			continue;

		const char separator = *cursor;
		*cursor = '\0';
		CreateDirectoryA(buffer, nullptr);
		*cursor = separator;
	}
}

bool IsReplayGameSpeedIndexValid(uint32_t gameSpeedIndex)
{
	return gameSpeedIndex <= static_cast<uint32_t>(MAX_GAME_SPEED_INDEX);
}

int GetReplayFPSFromGameSpeed(int gameSpeed)
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

void ApplyReplayTimingFromCurrentGameSpeed()
{
	if (gReplay.Playback && gReplay.HasPlaybackHeader)
	{
		// Keep simulation speed locked to the replay's recorded speed.
		const int recordedGameSpeed = std::clamp(static_cast<int>(gReplay.PlaybackHeader.RecordedGameSpeed), 0, MAX_GAME_SPEED_INDEX);
		GameOptionsClass::Instance.GameSpeed = recordedGameSpeed;
		GameModeOptionsClass::Instance.GameSpeed = recordedGameSpeed;
	}

	const int speedIndex = (gReplay.Playback && gReplay.PlaybackSpeedIndex >= 0)
		? gReplay.PlaybackSpeedIndex
		: GameOptionsClass::Instance.GameSpeed;
	const int requestedFPS = GetReplayFPSFromGameSpeed(speedIndex);

	// RequestedFPS controls local pacing. Keep SessionClass::DesiredFrameRate
	// owned by the game simulation to avoid altering deterministic logic.
	Game::Network::RequestedFPS = requestedFPS;
}

bool WriteRawToHandle(HANDLE file, const void* data, size_t size)
{
	if (file == INVALID_HANDLE_VALUE)
		return false;

	DWORD bytesWritten = 0;
	return WriteFile(file, data, static_cast<DWORD>(size), &bytesWritten, nullptr)
		&& bytesWritten == static_cast<DWORD>(size);
}

bool ReadRawFromHandle(HANDLE file, void* buffer, size_t size)
{
	if (file == INVALID_HANDLE_VALUE)
		return false;

	DWORD bytesRead = 0;
	return ReadFile(file, buffer, static_cast<DWORD>(size), &bytesRead, nullptr)
		&& bytesRead == static_cast<DWORD>(size);
}

bool WriteRaw(const void* data, size_t size)
{
	if (gReplay.Writer.IsActive())
		return gReplay.Writer.Write(data, size);

	return WriteRawToHandle(gReplay.ReplayFile, data, size);
}

bool ReadRaw(void* buffer, size_t size)
{
	if (gReplay.Reader.IsActive())
		return gReplay.Reader.Read(buffer, size);

	return ReadRawFromHandle(gReplay.ReplayFile, buffer, size);
}

// Ends the current deflate block, so everything recorded up to now can be decoded without any of
// the bytes that follow it. Called on a frame cadence rather than a byte one, because what
// matters after a crash is how many frames of play survived, not how many bytes.
void SyncFlushRecordingStream()
{
	if (!gReplay.Writer.IsActive())
		return;

	if (!gReplay.Writer.SyncFlush())
	{
		Debug::Log("[Replay] Failed to flush the replay stream; stopping the recording.\n");
		AbortReplaySystem();
		return;
	}

	// Committing to the disk is far more expensive, so it stays on a rare byte-count schedule.
	const uint64_t written = gReplay.Writer.CompressedBytesWritten();
	if (written - gReplay.BytesAtLastDiskFlush >= REPLAY_FLUSH_INTERVAL_BYTES)
	{
		FlushFileBuffers(gReplay.ReplayFile);
		gReplay.BytesAtLastDiskFlush = written;
	}
}

bool ReadRequiredFile(const char* fileName, std::vector<char>& content)
{
	content.clear();

	HANDLE hFile = CreateFileA(
		fileName,
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr
	);

	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	const DWORD fileSize = GetFileSize(hFile, nullptr);
	if (fileSize == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
	{
		CloseHandle(hFile);
		return false;
	}

	if (fileSize > 0)
	{
		content.resize(fileSize);
		DWORD bytesRead = 0;
		if (!ReadFile(hFile, content.data(), fileSize, &bytesRead, nullptr) || bytesRead != fileSize)
		{
			content.clear();
			CloseHandle(hFile);
			return false;
		}
	}

	CloseHandle(hFile);
	return true;
}

void SanitizeSpawnIniForReplay(std::vector<char>& spawnIni)
{
	static constexpr const char* AddressKeys[] = { "Ip", "IPv6", "LanIP" };
	static constexpr const char* BlankedAddress = "0.0.0.0";

	const auto equalsIgnoreCase = [](const char* a, size_t aLen, const char* b)
	{
		const size_t bLen = strlen(b);
		if (aLen != bLen)
			return false;

		for (size_t i = 0; i < aLen; ++i)
		{
			if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i])))
				return false;
		}

		return true;
	};

	const size_t size = spawnIni.size();
	const char* const base = spawnIni.data();

	std::vector<char> sanitized;
	sanitized.reserve(size);

	size_t lineStart = 0;
	while (true)
	{
		size_t lineEnd = lineStart;
		while (lineEnd < size && base[lineEnd] != '\n')
			++lineEnd;

		// A trailing '\r' belongs to the line ending, so exclude it from the content we inspect
		// and copy it back verbatim afterwards.
		size_t contentEnd = lineEnd;
		if (contentEnd > lineStart && base[contentEnd - 1] == '\r')
			--contentEnd;

		// Find 'key=value' and trim whitespace around the key.
		size_t separator = lineStart;
		while (separator < contentEnd && base[separator] != '=')
			++separator;

		bool blanked = false;
		if (separator < contentEnd)
		{
			size_t keyStart = lineStart;
			size_t keyEnd = separator;
			while (keyStart < keyEnd && isspace(static_cast<unsigned char>(base[keyStart])))
				++keyStart;
			while (keyEnd > keyStart && isspace(static_cast<unsigned char>(base[keyEnd - 1])))
				--keyEnd;

			for (const char* addressKey : AddressKeys)
			{
				if (equalsIgnoreCase(base + keyStart, keyEnd - keyStart, addressKey))
				{
					sanitized.insert(sanitized.end(), base + lineStart, base + separator + 1);
					sanitized.insert(sanitized.end(), BlankedAddress, BlankedAddress + strlen(BlankedAddress));
					sanitized.insert(sanitized.end(), base + contentEnd, base + lineEnd);
					blanked = true;
					break;
				}
			}
		}

		if (!blanked)
			sanitized.insert(sanitized.end(), base + lineStart, base + lineEnd);

		if (lineEnd >= size)
			break;

		sanitized.push_back('\n');
		lineStart = lineEnd + 1;
	}

	spawnIni.swap(sanitized);
}

// CCINIClass needs an explicit unload, so keep the pairing in one place.
class ScopedINIFile
{
public:
	explicit ScopedINIFile(const char* fileName)
		: pINI { CCINIClass::LoadINIFile(fileName) }
	{ }

	~ScopedINIFile()
	{
		if (pINI)
			CCINIClass::UnloadINIFile(pINI);
	}

	ScopedINIFile(const ScopedINIFile&) = delete;
	ScopedINIFile& operator=(const ScopedINIFile&) = delete;

	CCINIClass* Get() const { return pINI; }

private:
	CCINIClass* pINI;
};

bool TryReadString(CCINIClass* pINI, const char* section, const char* key, char* outBuffer, size_t outBufferSize)
{
	if (!outBuffer || outBufferSize == 0)
		return false;

	outBuffer[0] = '\0';

	if (!pINI)
		return false;

	const int charsRead = INIClassExt::ReadString_WithoutAresHook(
		pINI,
		section,
		key,
		"",
		outBuffer,
		outBufferSize
	);

	return charsRead > 0 && outBuffer[0] != '\0';
}

bool TryParseVersionString(const char* versionText, uint8_t& outMajor, uint8_t& outMinor, uint8_t& outRevision, uint8_t& outPatch)
{
	if (!versionText || versionText[0] == '\0')
		return false;

	unsigned int parsed[4] = { 0, 0, 0, 0 };
	int count = 0;
	const char* cursor = versionText;

	while (*cursor != '\0')
	{
		if (count >= 4 || !isdigit(static_cast<unsigned char>(*cursor)))
			return false;

		char* end = nullptr;
		const unsigned long value = strtoul(cursor, &end, 10);
		if (end == cursor || value > std::numeric_limits<uint8_t>::max())
			return false;

		parsed[count++] = static_cast<unsigned int>(value);
		cursor = end;

		if (*cursor == '\0')
			break;

		if (*cursor != '.')
			return false;

		++cursor;
		if (*cursor == '\0')
			return false;
	}

	if (count == 0)
		return false;

	outMajor = static_cast<uint8_t>(parsed[0]);
	outMinor = static_cast<uint8_t>(parsed[1]);
	outRevision = static_cast<uint8_t>(parsed[2]);
	outPatch = static_cast<uint8_t>(parsed[3]);
	return true;
}

ReplayHeader BuildReplayHeader(uint32_t spawnIniSize, uint32_t spawnMapSize)
{
	ReplayHeader header {};
	header.Magic = REPLAY_MAGIC;
	header.Version = REPLAY_VERSION;

	const ScopedINIFile spawnIni { "spawn.ini" };
	const ScopedINIFile spawnMapIni { "spawnmap.ini" };

	if (!TryReadString(spawnMapIni.Get(), "Basic", "Name", header.MapName, sizeof(header.MapName))
		&& !TryReadString(spawnIni.Get(), "Settings", "UIMapName", header.MapName, sizeof(header.MapName))
		&& ScenarioClass::Instance)
		strncpy_s(header.MapName, sizeof(header.MapName), ScenarioClass::Instance->FileName, _TRUNCATE);

	header.StartFrame = 0;
	header.SpawnerVersionMajor = VERSION_MAJOR;
	header.SpawnerVersionMinor = VERSION_MINOR;
	header.SpawnerVersionRevision = VERSION_REVISION;
	header.SpawnerVersionPatch = VERSION_PATCH;

	char versionBuffer[64] = { 0 };
	if (TryReadString(spawnIni.Get(), "Settings", "SpawnerVersion", versionBuffer, sizeof(versionBuffer)))
	{
		TryParseVersionString(
			versionBuffer,
			header.SpawnerVersionMajor,
			header.SpawnerVersionMinor,
			header.SpawnerVersionRevision,
			header.SpawnerVersionPatch
		);
	}

	strcpy_s(header.GameVersionString, "1.006");
	TryReadString(spawnIni.Get(), "Settings", "GameClientVersion", header.GameClientVersion, sizeof(header.GameClientVersion));
	header.GameMode = static_cast<uint32_t>(SessionClass::Instance.GameMode);

	header.UniqueIDCounter = ScenarioClass::Instance ? ScenarioClass::Instance->UniqueID : 0;
	header.Seed = Game::Seed;
	header.RandomNext1 = ScenarioClass::Instance ? ScenarioClass::Instance->Random.Next1 : -1;
	header.RandomNext2 = ScenarioClass::Instance ? ScenarioClass::Instance->Random.Next2 : -1;
	if (ScenarioClass::Instance)
	{
		memcpy(header.RandomizerTable, ScenarioClass::Instance->Random.Table, sizeof(header.RandomizerTable));
	}

	header.SpawnIniSize = spawnIniSize;
	header.SpawnMapSize = spawnMapSize;
	header.RecordedGameSpeed = static_cast<uint32_t>(std::clamp(GameOptionsClass::Instance.GameSpeed, 0, MAX_GAME_SPEED_INDEX));
	header.RecordedUnixTime = static_cast<uint64_t>(time(nullptr));
	header.TotalFrames = 0; // stamped by StopReplaySystem once recording finishes cleanly
	return header;
}

bool WriteInitialReplayFile()
{
	std::vector<char> spawnIni;
	std::vector<char> spawnMap;

	if (!ReadRequiredFile("spawn.ini", spawnIni))
	{
		Debug::Log("[Replay] Required file spawn.ini was not found or could not be read.\n");
		return false;
	}

	// Must happen before the header is built - SpawnIniSize has to describe what actually gets written.
	SanitizeSpawnIniForReplay(spawnIni);

	if (!ReadRequiredFile("spawnmap.ini", spawnMap))
	{
		Debug::Log("[Replay] Required file spawnmap.ini was not found or could not be read.\n");
		return false;
	}

	const ReplayHeader header = BuildReplayHeader(
		static_cast<uint32_t>(spawnIni.size()),
		static_cast<uint32_t>(spawnMap.size())
	);

	const char* const outputPath = GetRecordingOutputPath();
	EnsureParentDirectoryExists(outputPath);

	HANDLE file = CreateFileA(
		outputPath,
		GENERIC_WRITE,
		FILE_SHARE_READ,
		nullptr,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr
	);

	if (file == INVALID_HANDLE_VALUE)
	{
		Debug::Log("[Replay] Failed to create replay file for recording: %s\n", outputPath);
		return false;
	}

	bool ok = WriteRawToHandle(file, &header, sizeof(header));
	if (ok && !spawnIni.empty())
		ok = WriteRawToHandle(file, spawnIni.data(), spawnIni.size());
	if (ok && !spawnMap.empty())
		ok = WriteRawToHandle(file, spawnMap.data(), spawnMap.size());

	CloseHandle(file);
	return ok;
}

bool IsReplayHeaderValid(const ReplayHeader& header)
{
	return header.Magic == REPLAY_MAGIC
		&& header.Version == REPLAY_VERSION
		&& IsReplayGameSpeedIndexValid(header.RecordedGameSpeed);
}

bool ReadReplayHeaderFromHandle(HANDLE file, ReplayHeader& outHeader)
{
	outHeader = {};
	return ReadRawFromHandle(file, &outHeader, sizeof(outHeader))
		&& IsReplayHeaderValid(outHeader);
}

bool ReadReplayHeaderFromPath(const char* replayPath, ReplayHeader& outHeader)
{
	HANDLE file = CreateFileA(
		replayPath,
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr
	);

	if (file == INVALID_HANDLE_VALUE)
		return false;

	const bool ok = ReadReplayHeaderFromHandle(file, outHeader);
	CloseHandle(file);
	return ok;
}

void CloseReplayFile()
{
	gReplay.Writer.Reset();
	gReplay.Reader.Reset();

	if (gReplay.ReplayFile != INVALID_HANDLE_VALUE)
	{
		FlushFileBuffers(gReplay.ReplayFile);
		CloseHandle(gReplay.ReplayFile);
		gReplay.ReplayFile = INVALID_HANDLE_VALUE;
	}
}

bool OpenRecordingReplayStream()
{
	CloseReplayFile();

	const char* const outputPath = GetRecordingOutputPath();

	gReplay.ReplayFile = CreateFileA(
		outputPath,
		GENERIC_WRITE,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr
	);

	if (gReplay.ReplayFile == INVALID_HANDLE_VALUE)
	{
		// WriteInitialReplayFile has just written this file. Recreating it would start the deflate
		// stream at offset 0 and produce a headerless file no reader can parse.
		Debug::Log("[Replay] Could not reopen the replay file for recording: %s\n", outputPath);
		return false;
	}

	SetFilePointer(gReplay.ReplayFile, 0, nullptr, FILE_END);

	// Everything from here on - and nothing before it - is deflated.
	if (!gReplay.Writer.Start(gReplay.ReplayFile))
	{
		Debug::Log("[Replay] Failed to start the compressed replay stream.\n");
		CloseReplayFile();
		return false;
	}

	gReplay.BytesAtLastDiskFlush = 0;
	gReplay.LastSyncFlushFrame = 0;
	return true;
}

bool OpenPlaybackReplayStream(const char* replayPath)
{
	CloseReplayFile();

	gReplay.ReplayFile = CreateFileA(
		replayPath,
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr
	);

	if (gReplay.ReplayFile == INVALID_HANDLE_VALUE)
		return false;

	ReplayHeader header {};
	if (!ReadReplayHeaderFromHandle(gReplay.ReplayFile, header))
	{
		CloseReplayFile();
		return false;
	}

	// The sizes come straight off disk, so check them against the file before seeking.
	const uint64_t payloadSize = static_cast<uint64_t>(header.SpawnIniSize) + header.SpawnMapSize;

	LARGE_INTEGER fileSize {};
	if (!GetFileSizeEx(gReplay.ReplayFile, &fileSize)
		|| sizeof(ReplayHeader) + payloadSize > static_cast<uint64_t>(fileSize.QuadPart))
	{
		Debug::Log("[Replay] Replay declares embedded file sizes that do not fit the file.\n");
		CloseReplayFile();
		return false;
	}

	LARGE_INTEGER payloadOffset {};
	payloadOffset.QuadPart = static_cast<LONGLONG>(payloadSize);

	if (payloadSize > 0 && !SetFilePointerEx(gReplay.ReplayFile, payloadOffset, nullptr, FILE_CURRENT))
	{
		CloseReplayFile();
		return false;
	}

	// The frame stream is always deflated; the header and the two INIs above it never are.
	if (!gReplay.Reader.Start(gReplay.ReplayFile))
	{
		Debug::Log("[Replay] Failed to start reading the compressed replay stream.\n");
		CloseReplayFile();
		return false;
	}

	return true;
}

void ResetRuntimeFlagsForScenario()
{
	gReplay.Recording = false;
	gReplay.Playback = false;
	gReplay.SpectatorView = false;
	gReplay.PlaybackSpeedIndex = -1;
	gReplay.ExpectedEventsThisFrame = 0;
	gReplay.BytesAtLastDiskFlush = 0;
	gReplay.LastSyncFlushFrame = 0;
	gReplay.PlayersMarkedLoaded = false;
	gReplay.PendingFrameStates.clear();
	gReplay.PendingSideChannelEvents.clear();
	gReplay.HasPlaybackHeader = false;
	memset(&gReplay.PlaybackHeader, 0, sizeof(gReplay.PlaybackHeader));
	gReplay.HasPendingPlaybackFrame = false;
	gReplay.PlaybackStreamEnded = false;
	gReplay.PendingPlaybackFrame = {};
	gReplay.LockedViewportPos = { 0, 0 };
	gReplay.HasLockedViewportPos = false;
	gReplay.HasLastWrittenFrameState = false;
	gReplay.LastWrittenFrameNumber = 0;
	gReplay.LastRecordedTacticalPos = { 0, 0 };
	gReplay.LastRecordedSelectionCount = 0;
	gReplay.LastRecordedSelectionCRC = 0;
	gReplay.LastRecordedSelectionIDs.clear();
}

void AbortReplaySystem()
{
	ResetRuntimeFlagsForScenario();
	CloseReplayFile();
}

void ApplyPlaybackInitialState()
{
	if (!gReplay.HasPlaybackHeader)
		return;

	Game::Seed = gReplay.PlaybackHeader.Seed;
	if (ScenarioClass::Instance)
	{
		ScenarioClass::Instance->Random.Next1 = gReplay.PlaybackHeader.RandomNext1;
		ScenarioClass::Instance->Random.Next2 = gReplay.PlaybackHeader.RandomNext2;
		memcpy(ScenarioClass::Instance->Random.Table, gReplay.PlaybackHeader.RandomizerTable, sizeof(gReplay.PlaybackHeader.RandomizerTable));
		ScenarioClass::Instance->UniqueID = gReplay.PlaybackHeader.UniqueIDCounter;
	}
}

// Network/timing events are recorded for diagnostics but not replayed.
bool IsTimingEvent(EventType eventType)
{
	// Keep extension timing events out of playback without guessing numeric ranges.
	if (EventExt::IsValidType(static_cast<EventTypeExt>(eventType)))
		return true;

	switch (eventType)
	{
	case EventType::Empty:
	case EventType::ResponseTime:
	case EventType::FrameInfo:
	case EventType::Timing:
	case EventType::ProcessTime:
	case EventType::PacketTiming:
	case EventType::MegaFrameInfo:
	case EventType::FrameSync:
		return true;
	default:
		return false;
	}
}

// Playback injects only deterministic gameplay events.
bool IsReplayableGameplayEvent(const EventClass& event)
{
	return event.Type != EventType::Options && !IsTimingEvent(event.Type);
}

// Allow these to remain in DoList during playback so local controls work.
bool IsLocalPlaybackControlEvent(const EventClass& event)
{
	switch (event.Type)
	{
	case EventType::Options:
	case EventType::Exit:
	case EventType::GameSpeed:
	case EventType::SaveGame:
		return true;
	default:
		return false;
	}
}

template <typename Predicate>
void RemoveDoListEvents(Predicate shouldRemove)
{
	auto& doList = EventClass::DoList;
	const int originalCount = doList.Count;
	if (originalCount <= 0)
		return;

	// DoList holds up to MAX_EVENTS * 128 events of 111 bytes, and playback runs this every frame
	// with usually nothing to remove - so scan first, and only pay for the copy when needed.
	bool removedAny = false;
	for (int i = 0; i < originalCount; ++i)
	{
		if (shouldRemove(doList[i]))
		{
			removedAny = true;
			break;
		}
	}

	if (!removedAny)
		return;

	// Reused across frames so the copy does not reallocate on every rebuild.
	static std::vector<EventClass> preservedEvents;
	preservedEvents.clear();
	preservedEvents.reserve(static_cast<size_t>(originalCount));

	for (int i = 0; i < originalCount; ++i)
	{
		const auto& event = doList[i];
		if (!shouldRemove(event))
			preservedEvents.push_back(event);
	}

	doList.Init();
	for (const auto& event : preservedEvents)
	{
		doList.Add(event);
	}
}

// Remove events the game inserted during playback.
void RemoveReplayGameplayEventsFromDoList()
{
	if (!gReplay.Playback)
		return;

	const auto currentFrame = static_cast<unsigned int>(Unsorted::CurrentFrame);
	bool playbackSpeedChanged = false;

	// Treat local GameSpeed events as replay playback-speed changes.
	for (int i = 0; i < EventClass::DoList.Count; ++i)
	{
		const auto& event = EventClass::DoList[i];
		if (event.Frame == currentFrame && event.Type == EventType::GameSpeed)
		{
			const int requestedSpeed = std::clamp(event.GameSpeed.GameSpeed, 0, MAX_GAME_SPEED_INDEX);
			if (requestedSpeed != gReplay.PlaybackSpeedIndex)
			{
				gReplay.PlaybackSpeedIndex = requestedSpeed;
				playbackSpeedChanged = true;
			}
		}
	}

	if (playbackSpeedChanged)
		ApplyReplayTimingFromCurrentGameSpeed();

	RemoveDoListEvents([](const EventClass& event)
	{
		return event.Frame == static_cast<unsigned int>(Unsorted::CurrentFrame)
			&& IsReplayableGameplayEvent(event)
			&& !IsLocalPlaybackControlEvent(event);
	});
}

std::vector<uint32_t> GetSelectedObjectIDs()
{
	auto& currentObjects = ObjectClass::CurrentObjects;
	std::vector<uint32_t> ids;
	ids.reserve(currentObjects.Count);

	for (int i = 0; i < currentObjects.Count; ++i)
	{
		ObjectClass* pObj = currentObjects.Items[i];
		if (pObj)
			ids.push_back(static_cast<uint32_t>(pObj->UniqueID));
	}

	return ids;
}

bool IsCurrentSelection(const std::vector<uint32_t>& ids)
{
	auto& currentObjects = ObjectClass::CurrentObjects;
	size_t index = 0;

	for (int i = 0; i < currentObjects.Count; ++i)
	{
		ObjectClass* pObj = currentObjects.Items[i];
		if (!pObj)
			continue;

		if (index >= ids.size() || ids[index] != static_cast<uint32_t>(pObj->UniqueID))
			return false;

		++index;
	}

	return index == ids.size();
}

uint32_t SumObjectIDs(const std::vector<uint32_t>& ids)
{
	uint32_t sum = 0;
	for (const auto id : ids)
		sum += id;

	return sum;
}

// Record this frame when deterministic events or visible replay state changed.
bool WriteFrameCapture(const PendingRecordedFrameCapture& capture, int eventsThisFrame,
	const std::vector<SideChannelRecord>& sideChannelEvents)
{
	const bool tacticalPosChanged = !gReplay.HasLastWrittenFrameState
		|| capture.FrameState.TacticalPos.X != gReplay.LastRecordedTacticalPos.X
		|| capture.FrameState.TacticalPos.Y != gReplay.LastRecordedTacticalPos.Y;

	const bool selectionChanged = !gReplay.HasLastWrittenFrameState
		|| capture.FrameState.SelectedObjectCount != gReplay.LastRecordedSelectionCount
		|| capture.FrameState.SelectedObjectCRC != gReplay.LastRecordedSelectionCRC
		|| capture.SelectedObjectIDs != gReplay.LastRecordedSelectionIDs;

	const bool hasSideChannelEvents = !sideChannelEvents.empty();

	if (eventsThisFrame == 0 && !tacticalPosChanged && !selectionChanged && !hasSideChannelEvents)
		return true;

	FrameRecordHeader header {};
	header.FrameNumber = capture.FrameState.FrameNumber;
	header.EventCountThisFrame = eventsThisFrame;
	header.Flags = FrameRecordFlag_None;
	if (tacticalPosChanged)
		header.Flags |= FrameRecordFlag_TacticalPos;
	if (selectionChanged)
		header.Flags |= FrameRecordFlag_Selection;
	if (hasSideChannelEvents)
		header.Flags |= FrameRecordFlag_SideChannel;

	if (!WriteRaw(&header, sizeof(header)))
		return false;

	if ((header.Flags & FrameRecordFlag_TacticalPos) != 0u
		&& !WriteRaw(&capture.FrameState.TacticalPos, sizeof(capture.FrameState.TacticalPos)))
	{
		return false;
	}

	if ((header.Flags & FrameRecordFlag_Selection) != 0u)
	{
		if (!WriteRaw(&capture.FrameState.SelectedObjectCount, sizeof(capture.FrameState.SelectedObjectCount))
			|| !WriteRaw(&capture.FrameState.SelectedObjectCRC, sizeof(capture.FrameState.SelectedObjectCRC)))
		{
			return false;
		}

		// One call for the whole block: every WriteRaw is a tdefl_compress call, and box-selecting
		// a hundred units would otherwise mean a hundred of them on that frame.
		if (!capture.SelectedObjectIDs.empty()
			&& !WriteRaw(capture.SelectedObjectIDs.data(),
				capture.SelectedObjectIDs.size() * sizeof(uint32_t)))
		{
			return false;
		}
	}

	if (hasSideChannelEvents)
	{
		const int32_t count = static_cast<int32_t>(sideChannelEvents.size());
		if (!WriteRaw(&count, sizeof(count)))
			return false;

		for (const auto& sideChannelEvent : sideChannelEvents)
		{
			if (!WriteRaw(&sideChannelEvent, sizeof(sideChannelEvent)))
				return false;
		}
	}

	gReplay.HasLastWrittenFrameState = true;
	gReplay.LastWrittenFrameNumber = capture.FrameState.FrameNumber;
	gReplay.LastRecordedTacticalPos = capture.FrameState.TacticalPos;
	gReplay.LastRecordedSelectionCount = capture.FrameState.SelectedObjectCount;
	gReplay.LastRecordedSelectionCRC = capture.FrameState.SelectedObjectCRC;
	gReplay.LastRecordedSelectionIDs = capture.SelectedObjectIDs;

	return true;
}

void FlushPendingRecordedFramesThrough(int frameNumber, int currentFrameEventCount)
{
	while (!gReplay.PendingFrameStates.empty())
	{
		const auto& capture = gReplay.PendingFrameStates.front();
		const int pendingFrame = capture.FrameState.FrameNumber;
		if (pendingFrame > frameNumber)
			break;

		// Keep playback parsing bounded; excess side-channel records spill into later frames.
		std::vector<SideChannelRecord> sideChannelForFrame;
		while (!gReplay.PendingSideChannelEvents.empty()
			&& gReplay.PendingSideChannelEvents.front().FrameNumber <= pendingFrame
			&& sideChannelForFrame.size() < static_cast<size_t>(SIDECHANNEL_MAX_EVENTS_PER_FRAME))
		{
			sideChannelForFrame.push_back(gReplay.PendingSideChannelEvents.front());
			gReplay.PendingSideChannelEvents.pop_front();
		}

		const int eventCount = pendingFrame == frameNumber ? currentFrameEventCount : 0;
		if (!WriteFrameCapture(capture, eventCount, sideChannelForFrame))
		{
			Debug::Log("[Replay] Failed to write frame capture.\n");
			AbortReplaySystem();
			return;
		}

		gReplay.PendingFrameStates.pop_front();
	}
}

void RecordFrameState()
{
	if (!gReplay.Recording)
		return;

	PendingRecordedFrameCapture capture;
	auto& frameState = capture.FrameState;
	frameState.FrameNumber = Unsorted::CurrentFrame;
	frameState.TacticalPos = TacticalClass::Instance
		? TacticalClass::Instance->TacticalCoord1
		: Point2D { 0, 0 };

	capture.SelectedObjectIDs = GetSelectedObjectIDs();
	frameState.SelectedObjectCount = static_cast<int32_t>(capture.SelectedObjectIDs.size());
	frameState.SelectedObjectCRC = SumObjectIDs(capture.SelectedObjectIDs);

	if (!gReplay.PendingFrameStates.empty()
		&& gReplay.PendingFrameStates.back().FrameState.FrameNumber == frameState.FrameNumber)
	{
		gReplay.PendingFrameStates.back() = std::move(capture);
	}
	else
	{
		gReplay.PendingFrameStates.push_back(std::move(capture));
	}
}

bool WriteReplayEndOfStreamMarker()
{
	FrameRecordHeader marker {};
	marker.FrameNumber = -1;
	return WriteRaw(&marker, sizeof(marker));
}

// Stamp TotalFrames only after a clean recording shutdown.
bool WriteRecordedTotalFramesToHeader()
{
	if (gReplay.ReplayFile == INVALID_HANDLE_VALUE)
		return false;

	const LONG headerOffset = static_cast<LONG>(offsetof(ReplayHeader, TotalFrames));
	if (SetFilePointer(gReplay.ReplayFile, headerOffset, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
		return false;

	const uint32_t totalFrames = static_cast<uint32_t>(std::max(0, gReplay.LastWrittenFrameNumber));

	DWORD bytesWritten = 0;
	const bool ok = WriteFile(gReplay.ReplayFile, &totalFrames, sizeof(totalFrames), &bytesWritten, nullptr) != FALSE
		&& bytesWritten == sizeof(totalFrames);

	SetFilePointer(gReplay.ReplayFile, 0, nullptr, FILE_END);
	return ok;
}

// Replays are shared, so records off disk are untrusted: the text arrays need not be terminated
// and House/Aux index straight into engine arrays. False means the record cannot be made safe.
// Taunts need no bound - Taunts_752B70 rejects out-of-range commands itself.
bool SanitizeSideChannelRecord(SideChannelRecord& record)
{
	record.SenderName[SIDECHANNEL_NAME_LENGTH - 1] = L'\0';
	record.Text[SIDECHANNEL_TEXT_LENGTH - 1] = L'\0';

	switch (static_cast<SideChannelEventType>(record.Type))
	{
	case SideChannelEventType::ChatMessage:
		if (record.House < 0 || record.House >= MAX_HOUSES)
			return false;

		// Indexes ColorScheme::Array in TextLabelClass; fall back to the first scheme.
		if (record.Aux < 0 || record.Aux >= ColorScheme::Array.Count)
			record.Aux = 0;

		return true;

	case SideChannelEventType::BeaconPlace:
		// -1 asks the engine to pick a free slot, which is how a local placement is recorded.
		return record.House >= 0 && record.House < MAX_HOUSES
			&& record.Aux >= -1 && record.Aux < MAX_BEACON_SLOTS;

	case SideChannelEventType::BeaconDelete:
	case SideChannelEventType::BeaconText:
		return record.House >= 0 && record.House < MAX_HOUSES
			&& record.Aux >= 0 && record.Aux < MAX_BEACON_SLOTS;

	case SideChannelEventType::Taunt:
		return true;

	default:
		return false;
	}
}

bool ReadNextPlaybackFrameRecord(PlaybackFrameRecord& record)
{
	FrameRecordHeader header {};
	if (!ReadRaw(&header, sizeof(header)))
		return false;

	record = PlaybackFrameRecord {};
	record.FrameNumber = header.FrameNumber;
	record.EventCountThisFrame = header.EventCountThisFrame;
	record.Flags = header.Flags;

	if (record.FrameNumber == -1)
	{
		record.EndOfStream = true;
		return true;
	}

	if (record.FrameNumber < 0 || record.EventCountThisFrame < 0)
		return false;

	constexpr uint32_t knownFlags = FrameRecordFlag_TacticalPos | FrameRecordFlag_Selection | FrameRecordFlag_SideChannel;
	if ((record.Flags & ~knownFlags) != 0u)
		return false;

	if ((record.Flags & FrameRecordFlag_TacticalPos) != 0u
		&& !ReadRaw(&record.TacticalPos, sizeof(record.TacticalPos)))
	{
		return false;
	}

	if ((record.Flags & FrameRecordFlag_Selection) != 0u)
	{
		if (!ReadRaw(&record.SelectedObjectCount, sizeof(record.SelectedObjectCount))
			|| !ReadRaw(&record.SelectedObjectCRC, sizeof(record.SelectedObjectCRC)))
		{
			return false;
		}

		if (record.SelectedObjectCount < 0 || record.SelectedObjectCount > 4096)
			return false;

		record.SelectedObjectIDs.resize(static_cast<size_t>(record.SelectedObjectCount), 0u);
		if (!record.SelectedObjectIDs.empty()
			&& !ReadRaw(record.SelectedObjectIDs.data(),
				record.SelectedObjectIDs.size() * sizeof(uint32_t)))
		{
			return false;
		}
	}

	if ((record.Flags & FrameRecordFlag_SideChannel) != 0u)
	{
		int32_t sideChannelCount = 0;
		if (!ReadRaw(&sideChannelCount, sizeof(sideChannelCount)))
			return false;

		if (sideChannelCount < 0 || sideChannelCount > SIDECHANNEL_MAX_EVENTS_PER_FRAME)
			return false;

		// Read every record to keep the stream aligned, but keep only the ones that survive
		// validation - the rest would index out of an engine array or run off a text buffer.
		record.SideChannelEvents.reserve(static_cast<size_t>(sideChannelCount));
		for (int i = 0; i < sideChannelCount; ++i)
		{
			SideChannelRecord sideChannelRecord {};
			if (!ReadRaw(&sideChannelRecord, sizeof(SideChannelRecord)))
				return false;

			if (SanitizeSideChannelRecord(sideChannelRecord))
				record.SideChannelEvents.push_back(sideChannelRecord);
			else
				Debug::Log("[Replay] Discarded an out-of-range side-channel record during playback.\n");
		}
	}

	return true;
}

void ApplyPlaybackSelection(const PlaybackFrameRecord& frameRecord)
{
	const int maxSelectionCount = std::max(AbstractClass::Array.Count, 0);
	if (frameRecord.SelectedObjectCount < 0 || frameRecord.SelectedObjectCount > maxSelectionCount)
	{
		Debug::Log("[Replay] Invalid selected object count (%d) during playback.\n", frameRecord.SelectedObjectCount);
		StopReplaySystem();
		return;
	}

	if (!gReplay.SelectUnits)
		return;

	if (IsCurrentSelection(frameRecord.SelectedObjectIDs))
		return;

	const std::unordered_set<uint32_t> recordedIDs(
		frameRecord.SelectedObjectIDs.begin(), frameRecord.SelectedObjectIDs.end());

	// The object array can hold thousands of entries, so only the recorded ones are kept.
	std::unordered_map<uint32_t, ObjectClass*> objectByUniqueID;
	objectByUniqueID.reserve(recordedIDs.size());

	for (int i = 0; i < AbstractClass::Array.Count; ++i)
	{
		AbstractClass* pAbs = AbstractClass::Array.GetItem(i);
		if (!pAbs)
			continue;

		const auto uniqueID = static_cast<uint32_t>(pAbs->UniqueID);
		if (recordedIDs.find(uniqueID) == recordedIDs.end())
			continue;

		if (auto* pObject = abstract_cast<ObjectClass*>(pAbs))
			objectByUniqueID.emplace(uniqueID, pObject);
	}

	MapClass::UnselectAll();

	// Recorded order, so the selection ends up ordered the way it was.
	for (const auto uniqueID : frameRecord.SelectedObjectIDs)
	{
		const auto it = objectByUniqueID.find(uniqueID);
		if (it != objectByUniqueID.end())
			it->second->Select();
	}
}

// Replays recorded chat, beacons and taunts without using the network.
void ApplySideChannelEvent(const SideChannelRecord& record)
{
	if (!gReplay.ShowChatAndBeacons)
		return;

	constexpr int SIDECHANNEL_MESSAGE_DURATION_FRAMES = 1800;

	switch (static_cast<SideChannelEventType>(record.Type))
	{
	case SideChannelEventType::ChatMessage:
		MessageListClass::Instance.AddMessage(
			record.SenderName, record.House, record.Text, record.Aux,
			TextPrintType::UseGradPal | TextPrintType::FullShadow | TextPrintType::Point6Grad,
			SIDECHANNEL_MESSAGE_DURATION_FRAMES, false);
		if (RulesClass::Instance)
			VocClass::PlayGlobal(RulesClass::Instance->IncomingMessage, 0x2000, 1.0f);
		break;

	case SideChannelEventType::BeaconPlace:
		BeaconManagerClass::Instance.PlaceBeacon(record.House, record.Coord, record.Aux);
		break;

	case SideChannelEventType::BeaconDelete:
		BeaconManagerClass::Instance.DeleteBeacon(record.House, record.Aux);
		break;

	case SideChannelEventType::BeaconText:
	{
		// Beacon text uses BeaconPlacement::Message_431450.
		using BeaconMessageFn = char(__thiscall*)(BeaconManagerClass*, const wchar_t*, int, int, char);
		reinterpret_cast<BeaconMessageFn>(0x431450)(&BeaconManagerClass::Instance, record.Text, record.House, record.Aux, 1);
		break;
	}

	case SideChannelEventType::Taunt:
	{
		// Taunts_752B70 resolves the command byte and plays the sound.
		using TauntsFn = int(__fastcall*)(int);
		reinterpret_cast<TauntsFn>(0x752B70)(record.Aux);
		break;
	}

	default:
		break;
	}
}

void StopReplaySystem()
{
	if (gReplay.Recording)
	{
		FlushPendingRecordedFramesThrough(std::numeric_limits<int>::max(), 0);
		if (gReplay.ReplayFile != INVALID_HANDLE_VALUE)
		{
			if (!WriteReplayEndOfStreamMarker())
				Debug::Log("[Replay] Failed to write replay end-of-stream marker.\n");

			// The header stamp seeks to the start of the file, so the stream has to be terminated
			// and fully written out first.
			if (gReplay.Writer.IsActive() && !gReplay.Writer.Finish())
				Debug::Log("[Replay] Failed to finish the compressed replay stream.\n");

			if (!WriteRecordedTotalFramesToHeader())
				Debug::Log("[Replay] Failed to stamp the total frame count into the replay header.\n");
		}
	}

	AbortReplaySystem();
}

// Pins the camera to the last recorded viewport position.
void ApplyLockedViewport()
{
	if (!gReplay.LockViewport || !gReplay.HasLockedViewportPos || !TacticalClass::Instance)
		return;

	auto* tc = TacticalClass::Instance;
	const Point2D& target = gReplay.LockedViewportPos;

	// Avoid forcing a full repaint when the viewport is already correct.
	if (tc->TacticalCoord1.X == target.X && tc->TacticalCoord1.Y == target.Y)
		return;

	tc->TacticalCoord1 = target;
	tc->TacticalCoord2 = target;

	// sub_6D8B30: recalculates TacticalPos (top-left) and visible area.
	static auto RecalcViewport
		= reinterpret_cast<void(__thiscall*)(TacticalClass*)>(0x6D8B30);
	RecalcViewport(tc);

	tc->Redrawing = true;
}

// Whether playback should show the whole map instead of the recording player's shroud.
bool PlaybackWantsFullMapReveal()
{
	return gReplay.Playback && (!gReplay.ShroudEnabled || gReplay.SpectatorView);
}

// Revealing once does not hold. HouseClass::Visionary gates MapClass::Reveal, and every path that
// reshrouds - startup, spy satellite loss, crates, gap generators, triggers - clears it first, so
// re-testing the flag each frame re-reveals exactly when something took the reveal away.
void MaintainFullMapReveal()
{
	if (!PlaybackWantsFullMapReveal())
		return;

	HouseClass* const pPlayer = HouseClass::CurrentPlayer;
	if (!pPlayer || pPlayer->Visionary)
		return;

	MapClass::Instance.Reveal(pPlayer);
}

void RestoreFrameState()
{
	if (!gReplay.Playback)
		return;

	gReplay.ExpectedEventsThisFrame = 0;

	// Keep the viewport locked on frames without replay records.
	ApplyLockedViewport();

	MaintainFullMapReveal();

	if (!gReplay.HasPendingPlaybackFrame && !gReplay.PlaybackStreamEnded)
	{
		PlaybackFrameRecord nextRecord {};
		if (!ReadNextPlaybackFrameRecord(nextRecord))
		{
			Debug::Log("[Replay] Failed to read frame state during playback.\n");
			StopReplaySystem();
			return;
		}

		if (nextRecord.EndOfStream)
		{
			gReplay.PlaybackStreamEnded = true;
		}
		else
		{
			gReplay.PendingPlaybackFrame = std::move(nextRecord);
			gReplay.HasPendingPlaybackFrame = true;
		}
	}

	if (!gReplay.HasPendingPlaybackFrame)
		return;

	const auto& frameRecord = gReplay.PendingPlaybackFrame;
	if (frameRecord.FrameNumber < Unsorted::CurrentFrame)
	{
		Debug::Log("[Replay] Frame mismatch during playback (expected %u got %d).\n",
			Unsorted::CurrentFrame, frameRecord.FrameNumber);
		StopReplaySystem();
		return;
	}

	if (frameRecord.FrameNumber > Unsorted::CurrentFrame)
		return;

	gReplay.ExpectedEventsThisFrame = frameRecord.EventCountThisFrame;

	if ((frameRecord.Flags & FrameRecordFlag_TacticalPos) != 0u)
	{
		// Track this even when locking is disabled.
		gReplay.LockedViewportPos = frameRecord.TacticalPos;
		gReplay.HasLockedViewportPos = true;

		ApplyLockedViewport();
	}

	if ((frameRecord.Flags & FrameRecordFlag_Selection) != 0u)
	{
		ApplyPlaybackSelection(frameRecord);
	}

	if ((frameRecord.Flags & FrameRecordFlag_SideChannel) != 0u)
	{
		for (const auto& sideChannelEvent : frameRecord.SideChannelEvents)
			ApplySideChannelEvent(sideChannelEvent);
	}

	gReplay.HasPendingPlaybackFrame = false;
}

// Record every event on this frame; playback filters non-gameplay events later.
void RecordEventsForCurrentFrame()
{
	const int doListCount = EventClass::DoList.Count;
	int eventsThisFrame = 0;

	for (int i = 0; i < doListCount; ++i)
	{
		const auto& event = EventClass::DoList[i];
		if (event.Frame == static_cast<unsigned int>(Unsorted::CurrentFrame))
			++eventsThisFrame;
	}

	FlushPendingRecordedFramesThrough(Unsorted::CurrentFrame, eventsThisFrame);
	if (!gReplay.Recording)
		return;

	for (int i = 0; i < doListCount; ++i)
	{
		const auto& event = EventClass::DoList[i];
		if (event.Frame == static_cast<unsigned int>(Unsorted::CurrentFrame))
		{
			if (!WriteRaw(&event, sizeof(EventClass)))
			{
				Debug::Log("[Replay] Failed writing event to replay stream.\n");
				StopReplaySystem();
				return;
			}
		}
	}

	if (Unsorted::CurrentFrame - gReplay.LastSyncFlushFrame >= REPLAY_SYNC_FLUSH_FRAME_INTERVAL)
	{
		gReplay.LastSyncFlushFrame = Unsorted::CurrentFrame;
		SyncFlushRecordingStream();
	}
}

void PushSideChannelEvent(SideChannelRecord&& record)
{
	record.FrameNumber = Unsorted::CurrentFrame;
	gReplay.PendingSideChannelEvents.push_back(std::move(record));
}

void PlaybackFrameEvents()
{
	if (!gReplay.Playback)
		return;

	const int eventsToReplay = gReplay.ExpectedEventsThisFrame;
	gReplay.ExpectedEventsThisFrame = 0;

	for (int i = 0; i < eventsToReplay; ++i)
	{
		alignas(EventClass) char eventBuffer[sizeof(EventClass)] = { 0 };
		EventClass* replayEvent = reinterpret_cast<EventClass*>(eventBuffer);

		if (!ReadRaw(replayEvent, sizeof(EventClass)))
		{
			Debug::Log("[Replay] Event stream ended unexpectedly during playback.\n");
			StopReplaySystem();
			return;
		}

		// Every event on the frame was recorded, so the whole batch has to be read to keep the
		// stream aligned - but only the ones that drive the simulation get injected.
		if (!IsReplayableGameplayEvent(*replayEvent))
			continue;

		replayEvent->IsExecuted = false;
		if (!EventClass::DoList.Add(*replayEvent))
		{
			Debug::Log("[Replay] DoList is full while injecting replay events.\n");
			StopReplaySystem();
			return;
		}
	}
}

void StartReplayRecording()
{
	AbortReplaySystem();
	gReplay.Recording = true;

	const auto* pConfig = GetConfig();
	gReplay.ShroudEnabled = pConfig ? pConfig->ReplayShroudEnabled : false;
	gReplay.LockViewport = pConfig ? pConfig->ReplayLockedViewport : true;
	gReplay.SelectUnits = pConfig ? pConfig->ReplaySelectUnits : true;

	if (!ScenarioClass::Instance)
	{
		Debug::Log("[Replay] ScenarioClass instance unavailable at recording start.\n");
		StopReplaySystem();
		return;
	}

	if (!WriteInitialReplayFile())
	{
		Debug::Log("[Replay] Failed to write replay header.\n");
		StopReplaySystem();
		return;
	}

	if (!OpenRecordingReplayStream())
	{
		Debug::Log("[Replay] Failed to open replay file for recording.\n");
		StopReplaySystem();
	}
}

void StartReplayPlayback(const char* replayPath)
{
	AbortReplaySystem();
	gReplay.Playback = true;

	gReplay.PlaybackSpeedIndex = std::clamp(GameOptionsClass::Instance.GameSpeed, 0, MAX_GAME_SPEED_INDEX);

	int recordedGameSpeed = gReplay.PlaybackSpeedIndex;
	if (!gReplay.HasPlaybackHeader)
	{
		ReplayHeader header {};
		if (ReadReplayHeaderFromPath(replayPath, header))
		{
			gReplay.PlaybackHeader = header;
			gReplay.HasPlaybackHeader = true;
		}
	}

	if (gReplay.HasPlaybackHeader)
		recordedGameSpeed = std::clamp(static_cast<int>(gReplay.PlaybackHeader.RecordedGameSpeed), 0, MAX_GAME_SPEED_INDEX);

	GameOptionsClass::Instance.GameSpeed = recordedGameSpeed;
	GameModeOptionsClass::Instance.GameSpeed = recordedGameSpeed;

	ApplyReplayTimingFromCurrentGameSpeed();

	const auto* pConfig = GetConfig();
	gReplay.ShroudEnabled = pConfig ? pConfig->ReplayShroudEnabled : false;
	gReplay.LockViewport = pConfig ? pConfig->ReplayLockedViewport : true;
	gReplay.SelectUnits = pConfig ? pConfig->ReplaySelectUnits : true;
	gReplay.SpectatorView = pConfig ? pConfig->ReplaySpectator : false;
	gReplay.ShowChatAndBeacons = pConfig ? pConfig->ReplayShowChatAndBeacons : true;

	strncpy_s(gReplay.PlaybackPath, sizeof(gReplay.PlaybackPath), replayPath, _TRUNCATE);

	if (!OpenPlaybackReplayStream(gReplay.PlaybackPath))
	{
		StopReplaySystem();

		// StartScenario already skipped CreateConnections because ReplayFile was set, so there is no
		// live session to fall back to - carrying on leaves a game that can never advance a frame.
		Debug::FatalErrorAndExit("[Replay] Failed to open replay file for playback: %s",
			gReplay.PlaybackPath);
	}
}

} // namespace

void ReplaySystem::RecordChatMessage(int houseIndex, const wchar_t* senderName, const wchar_t* message, int colorSchemeIndex)
{
	if (!gReplay.Recording)
		return;

	SideChannelRecord record {};
	record.Type = static_cast<uint8_t>(SideChannelEventType::ChatMessage);
	record.House = houseIndex;
	record.Aux = colorSchemeIndex;
	if (senderName)
		wcsncpy_s(record.SenderName, senderName, _TRUNCATE);
	if (message)
		wcsncpy_s(record.Text, message, _TRUNCATE);

	PushSideChannelEvent(std::move(record));
}

void ReplaySystem::RecordTaunt(int tauntCommand)
{
	if (!gReplay.Recording)
		return;

	SideChannelRecord record {};
	record.Type = static_cast<uint8_t>(SideChannelEventType::Taunt);
	record.Aux = tauntCommand;

	PushSideChannelEvent(std::move(record));
}

void ReplaySystem::RecordBeaconPlace(int houseIndex, const CoordStruct& coord, int beaconSlot)
{
	if (!gReplay.Recording)
		return;

	SideChannelRecord record {};
	record.Type = static_cast<uint8_t>(SideChannelEventType::BeaconPlace);
	record.House = houseIndex;
	record.Aux = beaconSlot;
	record.Coord = coord;

	PushSideChannelEvent(std::move(record));
}

void ReplaySystem::RecordBeaconDelete(int houseIndex, int beaconSlot)
{
	if (!gReplay.Recording)
		return;

	SideChannelRecord record {};
	record.Type = static_cast<uint8_t>(SideChannelEventType::BeaconDelete);
	record.House = houseIndex;
	record.Aux = beaconSlot;

	PushSideChannelEvent(std::move(record));
}

void ReplaySystem::RecordBeaconText(int houseIndex, int beaconSlot, const wchar_t* text)
{
	if (!gReplay.Recording)
		return;

	SideChannelRecord record {};
	record.Type = static_cast<uint8_t>(SideChannelEventType::BeaconText);
	record.House = houseIndex;
	record.Aux = beaconSlot;
	if (text)
		wcsncpy_s(record.Text, text, _TRUNCATE);

	PushSideChannelEvent(std::move(record));
}

bool ReplaySystem::IsPlaybackRequested()
{
	const auto* pConfig = GetConfig();
	return pConfig && pConfig->ReplayFile[0] != '\0';
}

bool ReplaySystem::IsPlaybackActive()
{
	return gReplay.Playback;
}

void ReplaySystem::OnGameStartReset()
{
	StopReplaySystem();
	gReplay.InitRandomHandled = false;
	gReplay.PlaybackPath[0] = '\0';
}

DEFINE_HOOK(0x52FC42, InitRandom_CheckReplayMode, 0x7)
{
	if (gReplay.InitRandomHandled)
	{
		if (ReplaySystem::IsPlaybackRequested())
		{
			ApplyPlaybackInitialState();
			R->EAX(Game::Seed);
			return 0x52FDF9;
		}

		return 0;
	}

	gReplay.InitRandomHandled = true;
	if (!ReplaySystem::IsPlaybackRequested())
		return 0;

	const auto* pConfig = GetConfig();
	if (!pConfig)
		return 0;

	ReplayHeader header {};
	if (!ReadReplayHeaderFromPath(pConfig->ReplayFile, header))
	{
		Debug::Log("[Replay] Failed to read replay header from %s.\n", pConfig->ReplayFile);
		return 0;
	}

	gReplay.PlaybackHeader = header;
	gReplay.HasPlaybackHeader = true;
	ApplyPlaybackInitialState();

	R->EAX(Game::Seed);
	return 0x52FDF9;
}

// Hooks Clear_Scenario's ten byte `mov [edx+214h], 1000000`; a 0x5 hook would split the immediate.
// Return 0, not 0x685663 - Phobos hooks this address too.
//
// Syringe re-executes that reset after every hook here has run, so this body still sees the
// previous scenario's counter and applies the reset by hand for BuildReplayHeader's benefit.
// Nothing written to UniqueID here survives; the playback restore is in the 0x686B6A hook below.
DEFINE_HOOK(0x685659, ScenarioClass_Start_ReplayInit, 0xA)
{
	auto* const pScenario = R->EDX<ScenarioClass*>();
	if (pScenario)
		pScenario->UniqueID = 1000000;

	const auto* pConfig = GetConfig();
	if (!pConfig)
		return 0;

	if (ReplaySystem::IsPlaybackRequested())
	{
		ApplyPlaybackInitialState();
		StartReplayPlayback(pConfig->ReplayFile);
	}
	else if (pConfig->EnableReplayRecording)
	{
		StartReplayRecording();
	}
	else
	{
		StopReplaySystem();
	}

	return 0;
}

// Read_Scenario_INI, just after its `call Clear_Scenario`. Order is
// Select_Game -> Init_Random -> ... -> Read_Scenario_INI -> Clear_Scenario, so this is the first
// point past the UniqueID reset and before any object exists - every earlier restore is undone.
// It has to be pinned because object UniqueIDs come from ++ScenarioClass::UniqueID and the
// recorded selection is stored by them. The hooked `cmp Session, ebx` is also the `jnz` target
// that skips Clear_Scenario, so both paths are covered.
DEFINE_HOOK(0x686B6A, ReadScenarioINI_ReplayApplyState, 0x6)
{
	if (gReplay.Playback)
		ApplyPlaybackInitialState();

	return 0;
}

// MapClass::Reset_Shroud (thiscall, stack: +0 retaddr, +4 house). Gameplay reshrouds the local
// player constantly during a normal match - gap generators, spy satellite loss, shroud crates,
// map triggers - and whenever it targets PlayerPtr its body is a full map-cell walk plus a forced
// full-screen redraw and radar rebuild. During full-map-reveal playback (ReplayShroudEnabled=false
// or spectator view) MaintainFullMapReveal notices the resulting Visionary==false on the very next
// frame and immediately re-runs the equally expensive Reveal (0x577D90), so every reshroud event
// in the recording pays for two full-map passes - the intermittent CPU spikes during playback.
// Skipping the reshroud for the local player in that mode removes both: the map simply never
// leaves "revealed", which is what the setting asks for anyway. Other houses' cheap MapIsClear
// bookkeeping (the only work this function does for house != PlayerPtr) is left untouched.
DEFINE_HOOK(0x577AB0, MapClass_ResetShroud_SkipDuringFullRevealPlayback, 0x8)
{
	if (gReplay.Playback && PlaybackWantsFullMapReveal()
		&& R->Stack<HouseClass*>(0x4) == HouseClass::CurrentPlayer)
	{
		// Emulate the function's own `retn 4` without running its body: pop the return address
		// and the stack argument, then resume the caller there.
		const DWORD returnAddress = R->Stack<DWORD>(0x0);
		R->ESP(R->ESP() + 0x8);
		return returnAddress;
	}

	return 0;
}

// Capture visible replay state before dialog handling can skip the normal replay path.
DEFINE_HOOK(0x55D878, MainLoop_RecordPlaybackFrameState, 0x6)
{
	if (gReplay.Recording)
	{
		RecordFrameState();
	}

	if (gReplay.Playback)
	{
		ApplyReplayTimingFromCurrentGameSpeed();
		RestoreFrameState();
	}

	return 0;
}

DEFINE_HOOK(0x64820E, Queue_AI_Multiplayer_RecordPlaybackEvents, 0x7)
{
	if (gReplay.Recording)
	{
		RecordEventsForCurrentFrame();
	}

	if (gReplay.Playback)
	{
		RemoveReplayGameplayEventsFromDoList();
		PlaybackFrameEvents();
	}

	return 0x64821C;
}

// Replay playback does not pump live network traffic.
DEFINE_HOOK(0x55D8E3, MainLoop_SkipIPXPumpDuringReplayPlayback, 0x5)
{
	if (gReplay.Playback)
	{
		return 0x55D8E8;
	}

	return 0;
}

DEFINE_HOOK(0x55D25C, GameExit_FlushReplayBuffers, 0x6)
{
	StopReplaySystem();
	return 0;
}

DEFINE_HOOK(0x55CF13, GameExit_Sell_FlushReplayBuffers, 0x5)
{
	StopReplaySystem();
	return 0;
}

DEFINE_HOOK(0x6BEC60, Game_Exit_FlushReplayBuffers, 0x5)
{
	StopReplaySystem();
	return 0;
}

// TechnoClass::Create_Gap (thiscall, no stack args - hooked at the true entry, before any
// prologue runs, so 0x6FB460 - this function's own bare `retn` - is reachable directly).
// Whenever an enemy gap generator/jammer comes online, this walks every cell in its jam radius and,
// for each cell not allied with PlayerPtr, bumps that cell's ShroudCounter and GapsCoveringCell -
// directly re-shrouding patches of the map around itself from PlayerPtr's point of view. That is
// per-cell state, not the house-wide Visionary flag MaintainFullMapReveal watches, so an enemy gap
// generator re-fogs the ground around itself during full-map-reveal playback regardless of the
// Reset_Shroud fix above, and keeps doing it every time the generator's power flips back on (its
// own GeneratingGap latch only blocks re-entry while power stays up) - matching what was reported:
// fine until the first gap generator goes up, and recurring from then on as power fluctuates.
// Skip the function outright for the local player during full-map-reveal playback, so nothing can
// re-shroud PlayerPtr's view in the first place.
DEFINE_HOOK(0x6FB170, TechnoClass_CreateGap_SkipDuringFullRevealPlayback, 0x5)
{
	if (gReplay.Playback && PlaybackWantsFullMapReveal())
		return 0x6FB460;

	return 0;
}

// Playback is not waiting on anyone, so the network delay budget never has to expire.
DEFINE_HOOK(0x647866, Queue_AI_Multiplayer_OverrideDelayTime, 0x5)
{
	if (gReplay.Playback)
	{
		*reinterpret_cast<int*>(NETWORK_DELAY_TIME_ADDRESS) = std::numeric_limits<int>::max();
	}

	return 0;
}

// Mark all players loaded during replay startup, and skip SessionClass::Callback entirely.
// This is the function's true entry point (`mov al, Debug_Map_DEBUGDEBUG`, 5 bytes, before any of
// its stack frame is set up), which is why 0x69B156 - the function's own bare `retn 4` - is a safe
// jump target here: the raw [retAddr][arg] stack this hook sees is exactly what `retn 4` expects.
// Every progress-screen tick during multiplayer scenario load runs through here; once player 0's
// progress crosses 99.95% the original code broadcasts a "guaranteed progress" message to every
// other player over IPX/NullModem, then busy-waits (Sleep(20) in a loop) up to 240 SystemTimerClass
// ticks - about 4 seconds - for the send queue to drain below 5. The broadcast loop is gated on
// Players.ActiveCount > 1, and playback has no real peer to drain the queue, so with more than one
// player in the recording it always burns the full ~4 seconds - the load-in stall right after the
// progress bar reaches 100%. Skipping the function outright removes both the pointless network
// chatter and the stall; progress is forced to 100 directly below instead of through this callback.
DEFINE_HOOK(0x69AE90, WaitForPlayers_ReplayMarkOthersLoaded, 0x5)
{
	if (!gReplay.Playback)
		return 0;

	if (!gReplay.PlayersMarkedLoaded)
	{
		gReplay.PlayersMarkedLoaded = true;
		for (int i = 0; i < 8; ++i)
		{
			ProgressScreenClass::Instance.PlayerProgresses[i] = 100;
		}
	}

	return 0x69B156;
}

// --- Side-channel recording taps --------------------------------------------------------------
// Chat, beacons and taunts bypass EventClass::DoList and are recorded here.
// Some local beacon calls use -1/-1; resolve them before writing the replay.
namespace
{

bool ResolveFlaggedBeaconSlot(int& house, int& slot)
{
	for (int h = 0; h < 8; ++h)
	{
		for (int s = 0; s < 3; ++s)
		{
			BeaconClass* pBeacon = BeaconManagerClass::Instance.Beacons[h][s];
			if (pBeacon && (pBeacon->Bitfield & BEACON_FLAG_LOCAL) != 0)
			{
				house = h;
				slot = s;
				return true;
			}
		}
	}

	return false;
}

} // namespace

// BeaconManagerClass::Place. __thiscall(ECX=this); stack (relative to ESP at entry, before the
// prologue's `sub esp` runs): +0 retaddr, +4 house, +8 coord.X, +0xC coord.Y, +0x10 coord.Z,
// +0x14 houseBeaconId (-1 = auto-assign a free slot).
DEFINE_HOOK(0x430BA0, BeaconManagerClass_Place_RecordPlacement, 0x6)
{
	if (gReplay.Recording)
	{
		const int house = R->Stack<int>(0x4);
		CoordStruct coord;
		coord.X = R->Stack<int>(0x8);
		coord.Y = R->Stack<int>(0xC);
		coord.Z = R->Stack<int>(0x10);
		const int slot = R->Stack<int>(0x14);

		ReplaySystem::RecordBeaconPlace(house, coord, slot);
	}

	return 0;
}

// Beacon delete. Stack: +4 house, +8 slot; -1/-1 means the active local beacon.
DEFINE_HOOK(0x4311C0, BeaconPlacement_Delete_RecordDeletion, 0x6)
{
	if (gReplay.Recording)
	{
		int house = R->Stack<int>(0x4);
		int slot = R->Stack<int>(0x8);

		bool shouldRecord = true;
		if (house == -1 && slot == -1)
			shouldRecord = ResolveFlaggedBeaconSlot(house, slot);

		if (shouldRecord)
			ReplaySystem::RecordBeaconDelete(house, slot);
	}

	return 0;
}

// Beacon text. Skip local compose previews; record final and remote-applied text.
// Stack: +4 text, +8 house, +0xC slot, +0x10 broadcast.
DEFINE_HOOK(0x431450, BeaconPlacement_Message_RecordText, 0x6)
{
	if (gReplay.Recording)
	{
		const wchar_t* text = R->Stack<const wchar_t*>(0x4);
		int house = R->Stack<int>(0x8);
		int slot = R->Stack<int>(0xC);
		const int broadcast = R->Stack<int>(0x10);

		bool shouldRecord = broadcast != 0 || house != -1 || slot != -1;
		// Committed local text still needs its concrete beacon slot.
		if (shouldRecord && house == -1 && slot == -1)
			shouldRecord = ResolveFlaggedBeaconSlot(house, slot);

		if (shouldRecord)
			ReplaySystem::RecordBeaconText(house, slot, text);
	}

	return 0;
}

// Taunts_752B70. __fastcall(ECX=command).
DEFINE_HOOK(0x752B70, Taunts_RecordPlayback, 0x5)
{
	if (gReplay.Recording)
		ReplaySystem::RecordTaunt(R->ECX<int>());

	return 0;
}
