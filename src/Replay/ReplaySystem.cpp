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

#include <Spawner/Spawner.h>
#include <Utilities/Debug.h>
#include <Ext/Event/Body.h>
#include <Ext/INIClass/Body.h>

#include <BeaconManagerClass.h>
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
#include <ctime>
#include <cstring>
#include <deque>
#include <limits>
#include <unordered_map>
#include <vector>

constexpr uint32_t REPLAY_MAGIC = 0x4A455259u;
constexpr uint32_t REPLAY_VERSION = 1;
// Forcing a physical disk commit can block the game thread for tens of milliseconds on a
// mechanical drive, so this is set past what a game realistically produces: measured
// recordings run around 0.5 MB, with the longest observed at 2.5 MB. In practice the only
// flush that happens is the one when recording closes. Data already written survives a game
// crash regardless - the OS owns the page cache - so this only guards against power loss.
constexpr uint64_t REPLAY_FLUSH_INTERVAL_BYTES = 50ull * 1024 * 1024;
constexpr const char* DEFAULT_RECORDING_PATH = "replay.dat";

enum FrameRecordFlags : uint32_t
{
	FrameRecordFlag_None = 0u,
	FrameRecordFlag_TacticalPos = 1u << 0,
	FrameRecordFlag_Selection = 1u << 1,
	FrameRecordFlag_SideChannel = 1u << 2
};

// Chat, beacons and taunts travel over IPXManagerClass's "global channel" (Send_Global_Message /
// Network_Call_Back), entirely separate from the deterministic EventClass::DoList queue the rest of
// this file records. They can't be replayed the same way (there's nothing deterministic to re-inject),
// so instead we record what was displayed/heard and just reproduce that directly on the matching frame.
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

#pragma pack(push, 1)
struct SideChannelRecord
{
	int32_t FrameNumber = 0; // filled in when queued, not by the caller
	uint8_t Type = 0;        // SideChannelEventType
	int32_t House = -1;      // owning/sending house index; -1 if not house-scoped
	// Per-type: ColorSchemeIndex for ChatMessage, beacon slot (0-2, or -1) for the Beacon* events,
	// the raw taunt command byte for Taunt.
	int32_t Aux = 0;
	CoordStruct Coord = {};                     // BeaconPlace only
	wchar_t SenderName[SIDECHANNEL_NAME_LENGTH] = {}; // ChatMessage only
	wchar_t Text[SIDECHANNEL_TEXT_LENGTH] = {};       // ChatMessage / BeaconText only
};
#pragma pack(pop)

#pragma pack(push, 1)
struct ReplayHeader
{
	uint32_t Magic;
	uint32_t Version;
	char MapName[260];
	uint32_t StartFrame;
	uint8_t ProducerVersionMajor;
	uint8_t ProducerVersionMinor;
	uint8_t ProducerVersionRevision;
	uint8_t ProducerVersionPatch;
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

	// Wall-clock time the recording started. A file's mtime does not survive copying, so the
	// client shows this instead.
	uint64_t RecordedUnixTime;
	// Last recorded frame number, stamped in when recording closes cleanly. Stays 0 if the game
	// crashed or was killed mid-recording, which is how the client tells a complete replay from a
	// truncated one - the frame stream itself carries no length.
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
#pragma pack(pop)

struct PendingRecordedFrameCapture
{
	FrameStateRecord FrameState;
	std::vector<uint32_t> SelectedObjectIDs;
};

struct ReplayRuntimeState
{
	bool Recording = false;
	bool Playback = false;
	bool RunPlayback = true;
	bool InitRandomHandled = false;
	bool PlayersMarkedLoaded = false;

	bool ShroudEnabled = false;
	bool LockViewport = true;
	bool SelectUnits = true;
	bool SpectatorView = false;
	// Playback-only: whether recorded chat/beacons/taunts actually get reproduced. Recording of
	// this data is unconditional - this only gates whether it's played back.
	bool ShowChatAndBeacons = true;
	int PlaybackSpeedIndex = -1;

	int ExpectedEventsThisFrame = 0;
	uint64_t BytesSinceFlush = 0;

	HANDLE ReplayFile = INVALID_HANDLE_VALUE;

	char PlaybackPath[MAX_PATH] = { 0 };
	std::deque<PendingRecordedFrameCapture> PendingFrameStates;
	std::deque<SideChannelRecord> PendingSideChannelEvents;
	ReplayHeader PlaybackHeader = {};
	bool HasPlaybackHeader = false;

	bool HasPendingPlaybackFrame = false;
	bool PlaybackStreamEnded = false;

	// The recorded viewport position is only stored on frames where it changed, so playback has
	// to hold on to the last one and keep re-asserting it. Without this the camera is only
	// corrected on the frames the recorder happened to scroll on, leaving it free to roam in
	// between - which is not a lock at all.
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

// CreateFileA will not create missing directories and the client writes recordings into a
// subfolder, so make each component of the path up front. Failures are ignored here - the
// CreateFileA that follows reports the real problem.
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
	return gameSpeedIndex <= 6u;
}

int GetReplayFPSFromGameSpeed(int gameSpeed)
{
	gameSpeed = std::clamp(gameSpeed, 0, 6);

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
		const int recordedGameSpeed = std::clamp(static_cast<int>(gReplay.PlaybackHeader.RecordedGameSpeed), 0, 6);
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
	if (!WriteRawToHandle(gReplay.ReplayFile, data, size))
		return false;

	gReplay.BytesSinceFlush += size;
	if (gReplay.BytesSinceFlush >= REPLAY_FLUSH_INTERVAL_BYTES)
	{
		FlushFileBuffers(gReplay.ReplayFile);
		gReplay.BytesSinceFlush = 0;
	}

	return true;
}

bool ReadRaw(void* buffer, size_t size)
{
	return ReadRawFromHandle(gReplay.ReplayFile, buffer, size);
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

bool TryReadStringFromSpawnIni(const char* key, char* outBuffer, size_t outBufferSize)
{
	if (!key || !outBuffer || outBufferSize == 0)
		return false;

	outBuffer[0] = '\0';

	CCINIClass* pSpawnIni = CCINIClass::LoadINIFile("spawn.ini");
	if (!pSpawnIni)
		return false;

	const int charsRead = INIClassExt::ReadString_WithoutAresHook(
		pSpawnIni,
		"Settings",
		key,
		"",
		outBuffer,
		outBufferSize
	);
	CCINIClass::UnloadINIFile(pSpawnIni);

	return charsRead > 0 && outBuffer[0] != '\0';
}

bool TryReadMapNameFromSpawnMapIni(char* outMapName, size_t outMapNameSize)
{
	if (!outMapName || outMapNameSize == 0)
		return false;

	outMapName[0] = '\0';

	CCINIClass* pMapIni = CCINIClass::LoadINIFile("spawnmap.ini");
	if (!pMapIni)
		return false;

	const int charsRead = INIClassExt::ReadString_WithoutAresHook(
		pMapIni,
		"Basic",
		"Name",
		"",
		outMapName,
		outMapNameSize
	);
	CCINIClass::UnloadINIFile(pMapIni);

	return charsRead > 0 && outMapName[0] != '\0';
}

bool TryReadMapNameFromSpawnIni(char* outMapName, size_t outMapNameSize)
{
	return TryReadStringFromSpawnIni("UIMapName", outMapName, outMapNameSize);
}

bool TryParseVersionString(const char* versionText, uint8_t& outMajor, uint8_t& outMinor, uint8_t& outRevision, uint8_t& outPatch)
{
	if (!versionText || versionText[0] == '\0')
		return false;

	unsigned int parsed[4] = { 0, 0, 0, 0 };
	char extra = '\0';
	const int count = sscanf_s(
		versionText,
		"%u.%u.%u.%u%c",
		&parsed[0], &parsed[1], &parsed[2], &parsed[3],
		&extra,
		static_cast<unsigned int>(sizeof(extra))
	);

	// Must parse at least major, and reject trailing non-version characters.
	if (count < 1 || count > 4)
		return false;

	for (unsigned int value : parsed)
	{
		if (value > std::numeric_limits<uint8_t>::max())
			return false;
	}

	outMajor = static_cast<uint8_t>(parsed[0]);
	outMinor = static_cast<uint8_t>(parsed[1]);
	outRevision = static_cast<uint8_t>(parsed[2]);
	outPatch = static_cast<uint8_t>(parsed[3]);
	return true;
}

bool TryReadSpawnerVersionFromSpawnIni(uint8_t& outMajor, uint8_t& outMinor, uint8_t& outRevision, uint8_t& outPatch)
{
	char versionBuffer[64] = { 0 };
	if (!TryReadStringFromSpawnIni("SpawnerVersion", versionBuffer, sizeof(versionBuffer)))
		return false;

	return TryParseVersionString(versionBuffer, outMajor, outMinor, outRevision, outPatch);
}

bool TryReadGameClientVersionFromSpawnIni(char* outVersion, size_t outVersionSize)
{
	return TryReadStringFromSpawnIni("GameClientVersion", outVersion, outVersionSize);
}

ReplayHeader BuildReplayHeader(uint32_t spawnIniSize, uint32_t spawnMapSize)
{
	ReplayHeader header {};
	header.Magic = REPLAY_MAGIC;
	header.Version = REPLAY_VERSION;

	if (!TryReadMapNameFromSpawnMapIni(header.MapName, sizeof(header.MapName))
		&& !TryReadMapNameFromSpawnIni(header.MapName, sizeof(header.MapName))
		&& ScenarioClass::Instance)
		strncpy_s(header.MapName, sizeof(header.MapName), ScenarioClass::Instance->FileName, _TRUNCATE);

	header.StartFrame = 0;
	header.ProducerVersionMajor = VERSION_MAJOR;
	header.ProducerVersionMinor = VERSION_MINOR;
	header.ProducerVersionRevision = VERSION_REVISION;
	header.ProducerVersionPatch = VERSION_PATCH;
	TryReadSpawnerVersionFromSpawnIni(
		header.ProducerVersionMajor,
		header.ProducerVersionMinor,
		header.ProducerVersionRevision,
		header.ProducerVersionPatch
	);
	strcpy_s(header.GameVersionString, "1.006");
	TryReadGameClientVersionFromSpawnIni(header.GameClientVersion, sizeof(header.GameClientVersion));
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
	header.RecordedGameSpeed = static_cast<uint32_t>(std::clamp(GameOptionsClass::Instance.GameSpeed, 0, 6));
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
		EnsureParentDirectoryExists(outputPath);

		gReplay.ReplayFile = CreateFileA(
			outputPath,
			GENERIC_WRITE,
			FILE_SHARE_READ,
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr
		);
	}

	if (gReplay.ReplayFile == INVALID_HANDLE_VALUE)
		return false;

	SetFilePointer(gReplay.ReplayFile, 0, nullptr, FILE_END);
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

	const uint32_t payloadSize = header.SpawnIniSize + header.SpawnMapSize;
	if (payloadSize > 0)
	{
		SetFilePointer(gReplay.ReplayFile, static_cast<LONG>(payloadSize), nullptr, FILE_CURRENT);
	}

	return true;
}

void ResetRuntimeFlagsForScenario()
{
	gReplay.Recording = false;
	gReplay.Playback = false;
	gReplay.RunPlayback = true;
	gReplay.SpectatorView = false;
	gReplay.PlaybackSpeedIndex = -1;
	gReplay.ExpectedEventsThisFrame = 0;
	gReplay.BytesSinceFlush = 0;
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

	// If HideWaitingForPlayersScreen ever crashes during playback, the cause is the panel at
	// 0x0087F770 being uninitialised in replay mode; zeroing the auto-hide timer at 0x00B07784
	// stops MainLoop reaching it. Not needed since players are marked loaded up front.
}

// Network and timing bookkeeping that only meant something to live peers. These are still recorded
// (they carry the latency and process-time data), they just must not be replayed.
bool IsTimingEvent(EventType eventType)
{
	// Spawner extension events, currently just ProtocolZero's ResponseTime2. Asking EventExt rather
	// than testing a 0x30-0x3F range means a future extension event is covered automatically, and
	// that Ares' events at 0x60/0x61 are not swept up by accident.
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

// What actually drives the simulation, and so the only thing playback injects. Timing and network
// events have nothing to sync with offline, and OPTIONS would reopen menus from the original
// session; both are read past instead.
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

	std::vector<EventClass> preservedEvents;
	preservedEvents.reserve(originalCount);

	bool removedAny = false;
	for (int i = 0; i < originalCount; ++i)
	{
		const auto& event = doList[i];
		if (shouldRemove(event))
		{
			removedAny = true;
			continue;
		}

		preservedEvents.push_back(event);
	}

	if (!removedAny)
		return;

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
			const int requestedSpeed = std::clamp(event.GameSpeed.GameSpeed, 0, 6);
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

// Record information about this frame (if differs from last frame, or there's side-channel data).
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

		for (const auto uniqueID : capture.SelectedObjectIDs)
		{
			if (!WriteRaw(&uniqueID, sizeof(uniqueID)))
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

		// Side-channel hooks (chat/beacons/taunts) tag events with the frame they fired on, which
		// should always match a pending frame capture here since RecordFrameState() runs every
		// recorded frame unconditionally. The <= is just a safety net if a hook ever fires slightly
		// out of order relative to this flush - worst case, the event attaches to the next frame.
		//
		// Capped at SIDECHANNEL_MAX_EVENTS_PER_FRAME to match what ReadNextPlaybackFrameRecord will
		// accept - anything past that is left in the queue rather than dropped, so it just spills
		// into the next frame's side-channel block instead of making this one unreadable.
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

	if (TacticalClass::Instance)
		frameState.TacticalPos = TacticalClass::Instance->TacticalCoord1;
	else
		frameState.TacticalPos = { 0, 0 };

	auto& currentObjects = ObjectClass::CurrentObjects;
	capture.SelectedObjectIDs.reserve(currentObjects.Count);

	uint32_t crc = 0;
	for (int i = 0; i < currentObjects.Count; ++i)
	{
		ObjectClass* pObj = currentObjects.Items[i];
		if (!pObj)
			continue;

		const uint32_t uniqueID = static_cast<uint32_t>(pObj->UniqueID);
		crc += uniqueID;
		capture.SelectedObjectIDs.push_back(uniqueID);
	}

	frameState.SelectedObjectCount = static_cast<int32_t>(capture.SelectedObjectIDs.size());
	frameState.SelectedObjectCRC = crc;

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

// Seek back into the header and stamp the final frame count. A recording that never reaches this
// keeps TotalFrames == 0, which is what lets the client warn that a replay is truncated.
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
		for (int i = 0; i < record.SelectedObjectCount; ++i)
		{
			if (!ReadRaw(&record.SelectedObjectIDs[i], sizeof(uint32_t)))
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

		record.SideChannelEvents.resize(static_cast<size_t>(sideChannelCount));
		for (int i = 0; i < sideChannelCount; ++i)
		{
			if (!ReadRaw(&record.SideChannelEvents[i], sizeof(SideChannelRecord)))
				return false;
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
		gReplay.RunPlayback = false;
		StopReplaySystem();
		return;
	}

	auto& currentObjects = ObjectClass::CurrentObjects;
	uint32_t currentCRC = 0;
	for (int i = 0; i < currentObjects.Count; ++i)
	{
		ObjectClass* pObj = currentObjects.Items[i];
		if (pObj)
			currentCRC += static_cast<uint32_t>(pObj->UniqueID);
	}

	if (gReplay.SelectUnits
		&& (currentCRC != frameRecord.SelectedObjectCRC || currentObjects.Count != frameRecord.SelectedObjectCount))
	{
		MapClass::UnselectAll();

		std::unordered_map<int, ObjectClass*> objectByUniqueID;
		objectByUniqueID.reserve(static_cast<size_t>(std::max(AbstractClass::Array.Count, 0)));
		for (int i = 0; i < AbstractClass::Array.Count; ++i)
		{
			AbstractClass* pAbs = AbstractClass::Array.GetItem(i);
			if (!pAbs)
				continue;

			if (auto* pObject = abstract_cast<ObjectClass*>(pAbs))
				objectByUniqueID.emplace(pAbs->UniqueID, pObject);
		}

		for (const auto uniqueID : frameRecord.SelectedObjectIDs)
		{
			const auto it = objectByUniqueID.find(static_cast<int>(uniqueID));
			if (it != objectByUniqueID.end() && it->second)
				it->second->Select();
		}
	}
}

// Reproduces a recorded chat/beacon/taunt event, exactly as it was seen/heard on the recording
// client. This does not go anywhere near the network - it's the same display/audio calls the game
// itself makes on receipt, just fed from the replay file instead of IPXManagerClass.
void ApplySideChannelEvent(const SideChannelRecord& event_)
{
	if (!gReplay.ShowChatAndBeacons)
		return;

	// Duration is approximated: the game derives it from a runtime ticks-per-minute value we can't
	// read back out of the replay file, so this just needs to be "long enough to read comfortably".
	constexpr int SIDECHANNEL_MESSAGE_DURATION_FRAMES = 1800;

	switch (static_cast<SideChannelEventType>(event_.Type))
	{
	case SideChannelEventType::ChatMessage:
		MessageListClass::Instance.AddMessage(
			event_.SenderName, event_.House, event_.Text, event_.Aux,
			TextPrintType::UseGradPal | TextPrintType::FullShadow | TextPrintType::Point6Grad,
			SIDECHANNEL_MESSAGE_DURATION_FRAMES, false);
		if (RulesClass::Instance)
			VocClass::PlayGlobal(RulesClass::Instance->IncomingMessage, 0x2000, 1.0f);
		break;

	case SideChannelEventType::BeaconPlace:
		BeaconManagerClass::Instance.PlaceBeacon(event_.House, event_.Coord, event_.Aux);
		break;

	case SideChannelEventType::BeaconDelete:
		BeaconManagerClass::Instance.DeleteBeacon(event_.House, event_.Aux);
		break;

	case SideChannelEventType::BeaconText:
	{
		// Not exposed in BeaconManagerClass.h; call BeaconPlacement::Message_431450 directly, the
		// same function both the local compose UI and Network_Call_Back's beacon-text case use.
		using BeaconMessageFn = char(__thiscall*)(BeaconManagerClass*, const wchar_t*, int, int, char);
		reinterpret_cast<BeaconMessageFn>(0x431450)(&BeaconManagerClass::Instance, event_.Text, event_.House, event_.Aux, 1);
		break;
	}

	case SideChannelEventType::Taunt:
	{
		// Not exposed in BeaconManagerClass.h-style typed headers; call the game's taunt-sound
		// selector directly, same as Network_Call_Back does for a received taunt.
		using TauntsFn = int(__fastcall*)(int);
		reinterpret_cast<TauntsFn>(0x752B70)(event_.Aux);
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

			if (!WriteRecordedTotalFramesToHeader())
				Debug::Log("[Replay] Failed to stamp the total frame count into the replay header.\n");
		}
	}

	AbortReplaySystem();
}

// Pins the camera to the recorded position. Called every frame during playback, not just on the
// frames that carry a position, because the recording only stores one when it changed.
//
// Mirrors the built-in replay viewport restore (sub_6D6000). Writing TacticalPos alone is not
// enough: TacticalClass::Update overwrites it from TacticalCoord1 every frame, and Draw keys off
// Redrawing/TacticalCoord1 to decide on a full repaint.
void ApplyLockedViewport()
{
	if (!gReplay.LockViewport || !gReplay.HasLockedViewportPos || !TacticalClass::Instance)
		return;

	auto* tc = TacticalClass::Instance;
	const Point2D& target = gReplay.LockedViewportPos;

	// Only touch the viewport when it has actually drifted. Forcing Redrawing every frame would
	// mean a full-screen repaint every frame for no reason.
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

void RestoreFrameState()
{
	if (!gReplay.Playback || !gReplay.RunPlayback)
		return;

	gReplay.ExpectedEventsThisFrame = 0;

	// Re-assert before anything else, so the lock holds on frames that carry no record at all -
	// which is most of them, since frames are only written when something changed.
	ApplyLockedViewport();

	if (!gReplay.HasPendingPlaybackFrame && !gReplay.PlaybackStreamEnded)
	{
		PlaybackFrameRecord nextRecord {};
		if (!ReadNextPlaybackFrameRecord(nextRecord))
		{
			Debug::Log("[Replay] Failed to read frame state during playback.\n");
			gReplay.RunPlayback = false;
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
		gReplay.RunPlayback = false;
		StopReplaySystem();
		return;
	}

	if (frameRecord.FrameNumber > Unsorted::CurrentFrame)
		return;

	gReplay.ExpectedEventsThisFrame = frameRecord.EventCountThisFrame;

	if ((frameRecord.Flags & FrameRecordFlag_TacticalPos) != 0u)
	{
		// Tracked even when the viewport is not locked, so that ticking the option mid-session
		// would have something to snap to rather than waiting for the recorder's next scroll.
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
	if (!gReplay.RunPlayback)
		return;

	// Reveal the whole map when the viewer asked for no shroud, and always for a spectator - an
	// observer is meant to see everything. Observer mode itself is set during load in
	// Spawner::StartGame, which is early enough for the sidebar to be built for it.
	if ((!gReplay.ShroudEnabled || gReplay.SpectatorView) && Unsorted::CurrentFrame == 0)
	{
		for (const auto pHouse : HouseClass::Array)
		{
			if (pHouse)
				MapClass::Instance.Reveal(pHouse);
		}
	}
}

// Records every event on this frame, including the timing ones that are never replayed - they carry
// the per-player latency and process-time data, which makes a replay useful for working out who was
// lagging. Playback decides what to skip, since that is the only place it matters.
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
}

void PushSideChannelEvent(SideChannelRecord&& record)
{
	record.FrameNumber = Unsorted::CurrentFrame;
	gReplay.PendingSideChannelEvents.push_back(std::move(record));
}

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

void PlaybackFrameEvents()
{
	if (!gReplay.RunPlayback || !gReplay.Playback)
		return;

	const int eventsToReplay = gReplay.ExpectedEventsThisFrame;
	gReplay.ExpectedEventsThisFrame = 0;

	for (int i = 0; i < eventsToReplay; ++i)
	{
		char eventBuffer[sizeof(EventClass)] = { 0 };
		EventClass* replayEvent = reinterpret_cast<EventClass*>(eventBuffer);

		if (!ReadRaw(replayEvent, sizeof(EventClass)))
		{
			Debug::Log("[Replay] Event stream ended unexpectedly during playback.\n");
			gReplay.RunPlayback = false;
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
			gReplay.RunPlayback = false;
			StopReplaySystem();
			return;
		}
	}
}

void StartReplayRecording()
{
	AbortReplaySystem();
	ResetRuntimeFlagsForScenario();
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
		return;
	}

}

void StartReplayPlayback(const char* replayPath)
{
	AbortReplaySystem();
	ResetRuntimeFlagsForScenario();
	gReplay.Playback = true;
	gReplay.RunPlayback = true;

	gReplay.PlaybackSpeedIndex = std::clamp(GameOptionsClass::Instance.GameSpeed, 0, 6);

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
		recordedGameSpeed = std::clamp(static_cast<int>(gReplay.PlaybackHeader.RecordedGameSpeed), 0, 6);

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
		Debug::Log("[Replay] Failed to open replay file for playback: %s\n", gReplay.PlaybackPath);
		StopReplaySystem();
		return;
	}

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

DEFINE_HOOK(0x685659, ScenarioClass_Start_ReplayInit, 0x5)
{
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

// Record/Restore the frame state. Doing it a little earlier than the game's normal path for replays
// This way we avoid the SpecialDialog skip. If you do change this, make sure to test if playback
// works when the options menu is open.
DEFINE_HOOK(0x0055d878, MainLoop_RecordPlaybackFrameState, 0x6)
{
	if (gReplay.Recording)
	{
		RecordFrameState();
	}

	if (gReplay.Playback && gReplay.RunPlayback)
	{
		ApplyReplayTimingFromCurrentGameSpeed();
		RestoreFrameState();
	}

	return 0;
}

DEFINE_HOOK(0x0064820E, Queue_AI_Multiplayer_RecordPlaybackEvents, 0x7)
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

	return 0x0064821C;
}
// MainLoop normally pumps IPX periodically for online games.
// Built-in playback avoids this network path; do the same for replay playback.
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


DEFINE_HOOK(0x647866, Queue_AI_Multiplayer_OverrideDelayTime, 0x5)
{
	if (gReplay.Playback)
	{
		*reinterpret_cast<int*>(0x00AFA458) = std::numeric_limits<int>::max();
	}

	return 0;
}

// When loading the game in playback, force other players to "loaded"
DEFINE_HOOK(0x0069AFEB, WaitForPlayers_ReplayMarkOthersLoaded, 0x6)
{
	if (!gReplay.Playback || gReplay.PlayersMarkedLoaded)
		return 0;

	gReplay.PlayersMarkedLoaded = true;
	for (int i = 0; i < 8; ++i)
	{
		ProgressScreenClass::Instance.PlayerProgresses[i] = 100;
	}

	return 0;
}

// --- Alternate network stream recording taps -------------------------------------------------
//
// Chat, beacons and taunts all travel over IPXManagerClass's "global channel", not the
// deterministic EventClass::DoList queue the rest of this file replays. For beacons and taunts
// the game itself funnels both the locally-triggered path and the remote-received path through
// the same function, so hooking that one function's entry captures every occurrence regardless
// of who triggered it. Chat is recorded from within src/Misc/InGameChat.cpp's existing hooks
// instead (see ReplaySystem::RecordChatMessage), since those already sit at the exact point
// both the local echo and a received message have their final House/Name/Text resolved.

// Local "delete my beacon" and "set my beacon's text" both call through with house=-1, slot=-1,
// meaning "whichever beacon has Bitfield bit 0x2 set" (BeaconPlacement::Message_431170 does this
// same lookup to find the beacon the player is currently placing/composing). That flag is set by
// UI flow we don't otherwise capture or replay, so recording the raw -1/-1 and replaying it later
// finds nothing on a freshly-reconstructed BeaconManagerClass and silently no-ops. Resolving the
// actual house/slot here, at the exact point the game itself is about to do the same lookup,
// makes the recorded event self-contained instead.
bool ResolveFlaggedBeaconSlot(int& house, int& slot)
{
	for (int h = 0; h < 8; ++h)
	{
		for (int s = 0; s < 3; ++s)
		{
			BeaconClass* pBeacon = BeaconManagerClass::Instance.Beacons[h][s];
			if (pBeacon && (pBeacon->Bitfield & 2) != 0)
			{
				house = h;
				slot = s;
				return true;
			}
		}
	}

	return false;
}

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

// BeaconPlacement::Delete. __thiscall(ECX=this); stack: +0 retaddr, +4 house (or -1 for "any"),
// +8 houseBeaconId (or -1). The local "delete beacon" hotkey (Delete_Command, 0x731A10) always
// calls this with -1/-1 - see ResolveFlaggedBeaconSlot above.
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

// BeaconPlacement::Message_431450 (beacon text). __thiscall(ECX=this); stack: +0 retaddr,
// +4 strDestination (wchar_t*, may be null), +8 house (-1 = local player's own beacon),
// +0xC houseBeaconId (-1 alongside house==-1 for "find my own beacon"), +0x10 broadcast (bool).
//
// house==-1 && slot==-1 && !broadcast happens on every keystroke while composing locally (a live
// preview that never reaches the network) - skip those, or the replay fills up with noise. Every
// other call is either the local player's final committed/cleared text (broadcast=true) or a
// remote-applied update carrying an explicit house/slot, both of which are worth keeping.
DEFINE_HOOK(0x431450, BeaconPlacement_Message_RecordText, 0x6)
{
	if (gReplay.Recording)
	{
		const wchar_t* text = R->Stack<const wchar_t*>(0x4);
		int house = R->Stack<int>(0x8);
		int slot = R->Stack<int>(0xC);
		const int broadcast = R->Stack<int>(0x10);

		bool shouldRecord = broadcast != 0 || house != -1 || slot != -1;
		// house/slot are -1/-1 for every local call, committed or not - see
		// ResolveFlaggedBeaconSlot above. Only worth resolving for the ones we're keeping.
		if (shouldRecord && house == -1 && slot == -1)
			shouldRecord = ResolveFlaggedBeaconSlot(house, slot);

		if (shouldRecord)
			ReplaySystem::RecordBeaconText(house, slot, text);
	}

	return 0;
}

// Taunts_752B70 - resolves a taunt command byte to a wav and plays it. __fastcall(ECX=Command).
// Both the local trigger (TauntCommandClass::Process, before it sends) and the remote-received
// path (Network_Call_Back's YRPACKET_29 case) call this same function, so it's a clean single tap.
DEFINE_HOOK(0x752B70, Taunts_RecordPlayback, 0x5)
{
	if (gReplay.Recording)
		ReplaySystem::RecordTaunt(R->ECX<int>());

	return 0;
}
