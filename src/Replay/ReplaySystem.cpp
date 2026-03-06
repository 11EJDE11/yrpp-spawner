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
#include <Ext/INIClass/Body.h>

#include <EventClass.h>
#include <MapClass.h>
#include <ProgressScreenClass.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <unordered_map>
#include <vector>

constexpr uint32_t REPLAY_MAGIC = 0x4A455259u;
constexpr uint32_t REPLAY_VERSION = 1;
constexpr int REPLAY_FLUSH_INTERVAL_FRAMES = 300;
constexpr const char* DEFAULT_RECORDING_PATH = "replay.dat";

enum FrameRecordFlags : uint32_t
{
	FrameRecordFlag_None = 0u,
	FrameRecordFlag_TacticalPos = 1u << 0,
	FrameRecordFlag_Selection = 1u << 1
};

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
	bool HeaderPrepared = false;
	bool PlayersMarkedLoaded = false;

	bool ShroudEnabled = false;
	bool LockViewport = true;
	bool SelectUnits = true;

	int ExpectedEventsThisFrame = 0;

	HANDLE ReplayFile = INVALID_HANDLE_VALUE;

	char PlaybackPath[MAX_PATH] = { 0 };
	std::deque<PendingRecordedFrameCapture> PendingFrameStates;
	ReplayHeader PlaybackHeader = {};
	bool HasPlaybackHeader = false;

	bool HasPendingPlaybackFrame = false;
	bool PlaybackStreamEnded = false;
	PlaybackFrameRecord PendingPlaybackFrame = {};

	bool HasLastWrittenFrameState = false;
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
	return WriteRawToHandle(gReplay.ReplayFile, data, size);
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

	if (!ReadRequiredFile("spawnmap.ini", spawnMap))
	{
		Debug::Log("[Replay] Required file spawnmap.ini was not found or could not be read.\n");
		return false;
	}

	const ReplayHeader header = BuildReplayHeader(
		static_cast<uint32_t>(spawnIni.size()),
		static_cast<uint32_t>(spawnMap.size())
	);

	HANDLE file = CreateFileA(
		DEFAULT_RECORDING_PATH,
		GENERIC_WRITE,
		FILE_SHARE_READ,
		nullptr,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr
	);

	if (file == INVALID_HANDLE_VALUE)
		return false;

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
		&& header.Version == REPLAY_VERSION;
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

	const bool ok = ReadRawFromHandle(file, &outHeader, sizeof(outHeader)) && IsReplayHeaderValid(outHeader);
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

	gReplay.ReplayFile = CreateFileA(
		DEFAULT_RECORDING_PATH,
		GENERIC_WRITE,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr
	);

	if (gReplay.ReplayFile == INVALID_HANDLE_VALUE)
	{
		gReplay.ReplayFile = CreateFileA(
			DEFAULT_RECORDING_PATH,
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
	if (!ReadRaw(&header, sizeof(header)) || !IsReplayHeaderValid(header))
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
	gReplay.ExpectedEventsThisFrame = 0;
	gReplay.PlayersMarkedLoaded = false;
	gReplay.PendingFrameStates.clear();
	gReplay.HasPlaybackHeader = false;
	memset(&gReplay.PlaybackHeader, 0, sizeof(gReplay.PlaybackHeader));
	gReplay.HasPendingPlaybackFrame = false;
	gReplay.PlaybackStreamEnded = false;
	gReplay.PendingPlaybackFrame = {};
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

	// SessionClass::Resume sets WaitingForPlayersAutoHideFrame = CurrentFrame + 3, which
	// triggers SessionClass_HideWaitingForPlayersScreen ~3 frames later. In observer/replay
	// mode the "waiting for players" panel object at 0x0087F770 is never initialized, so
	// the call to FUN_0054f720(ECX=[0x0087F770]) crashes at *(ECX+0x318). Zero out the
	// timer so MainLoop never invokes HideWaitingForPlayersScreen during playback.
	//*reinterpret_cast<int*>(0x00B07784u) = 0;
}

// Timing events aren't needed as we skip any networking stuff in playback.
// Event 48 is from the spawner's Protocol Zero timing.
bool IsTimingEvent(EventType eventType)
{
	const auto type = static_cast<unsigned char>(eventType);
	if (type >= 0x30 && type <= 0x3F)
		return true;

	switch (eventType)
	{
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

// Skip OPTIONS event. If we recorded this, then the options menu would open
// during playback if the user in the recording opened it.
bool IsReplayableGameplayEvent(const EventClass& event)
{
	return event.Type != EventType::Options && !IsTimingEvent(event.Type);
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

	RemoveDoListEvents([](const EventClass& event)
	{
		return event.Frame == static_cast<unsigned int>(Unsorted::CurrentFrame)
			&& IsReplayableGameplayEvent(event);
	});
}

// Record information about this frame (if differs from last frame).
bool WriteFrameCapture(const PendingRecordedFrameCapture& capture, int eventsThisFrame)
{
	const bool tacticalPosChanged = !gReplay.HasLastWrittenFrameState
		|| capture.FrameState.TacticalPos.X != gReplay.LastRecordedTacticalPos.X
		|| capture.FrameState.TacticalPos.Y != gReplay.LastRecordedTacticalPos.Y;

	const bool selectionChanged = !gReplay.HasLastWrittenFrameState
		|| capture.FrameState.SelectedObjectCount != gReplay.LastRecordedSelectionCount
		|| capture.FrameState.SelectedObjectCRC != gReplay.LastRecordedSelectionCRC
		|| capture.SelectedObjectIDs != gReplay.LastRecordedSelectionIDs;

	if (eventsThisFrame == 0 && !tacticalPosChanged && !selectionChanged)
		return true;

	FrameRecordHeader header {};
	header.FrameNumber = capture.FrameState.FrameNumber;
	header.EventCountThisFrame = eventsThisFrame;
	header.Flags = FrameRecordFlag_None;
	if (tacticalPosChanged)
		header.Flags |= FrameRecordFlag_TacticalPos;
	if (selectionChanged)
		header.Flags |= FrameRecordFlag_Selection;

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

	gReplay.HasLastWrittenFrameState = true;
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

		const int eventCount = pendingFrame == frameNumber ? currentFrameEventCount : 0;
		if (!WriteFrameCapture(capture, eventCount))
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
		frameState.TacticalPos = TacticalClass::Instance->TacticalPos;
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

	constexpr uint32_t knownFlags = FrameRecordFlag_TacticalPos | FrameRecordFlag_Selection;
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

void StopReplaySystem()
{
	if (gReplay.Recording)
	{
		FlushPendingRecordedFramesThrough(std::numeric_limits<int>::max(), 0);
		if (gReplay.ReplayFile != INVALID_HANDLE_VALUE && !WriteReplayEndOfStreamMarker())
			Debug::Log("[Replay] Failed to write replay end-of-stream marker.\n");
	}

	AbortReplaySystem();
}

void RestoreFrameState()
{
	if (!gReplay.Playback || !gReplay.RunPlayback)
		return;

	gReplay.ExpectedEventsThisFrame = 0;

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

	if ((frameRecord.Flags & FrameRecordFlag_TacticalPos) != 0u
		&& gReplay.LockViewport
		&& TacticalClass::Instance)
	{
		TacticalClass::Instance->TacticalPos = frameRecord.TacticalPos;
	}

	if ((frameRecord.Flags & FrameRecordFlag_Selection) != 0u)
	{
		ApplyPlaybackSelection(frameRecord);
	}

	gReplay.HasPendingPlaybackFrame = false;
	if (!gReplay.RunPlayback)
		return;

	if (!gReplay.ShroudEnabled && Unsorted::CurrentFrame == 0)
	{
		for (const auto pHouse : HouseClass::Array)
		{
			if (pHouse)
				MapClass::Instance.Reveal(pHouse);
		}
	}
}

void RecordEventsForCurrentFrame()
{
	const int doListCount = EventClass::DoList.Count;
	int eventsThisFrame = 0;

	for (int i = 0; i < doListCount; ++i)
	{
		const auto& event = EventClass::DoList[i];
		if (event.Frame == static_cast<unsigned int>(Unsorted::CurrentFrame)
			&& IsReplayableGameplayEvent(event))
		{
			++eventsThisFrame;
		}
	}

	FlushPendingRecordedFramesThrough(Unsorted::CurrentFrame, eventsThisFrame);
	if (!gReplay.Recording)
		return;

	for (int i = 0; i < doListCount; ++i)
	{
		const auto& event = EventClass::DoList[i];
		if (event.Frame == static_cast<unsigned int>(Unsorted::CurrentFrame)
			&& IsReplayableGameplayEvent(event))
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
	gReplay.HeaderPrepared = true;

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

	const auto* pConfig = GetConfig();
	gReplay.ShroudEnabled = pConfig ? pConfig->ReplayShroudEnabled : false;
	gReplay.LockViewport = pConfig ? pConfig->ReplayLockedViewport : true;
	gReplay.SelectUnits = pConfig ? pConfig->ReplaySelectUnits : true;

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
	gReplay.HeaderPrepared = false;
	gReplay.PlaybackPath[0] = '\0';
}

// Todo: merge the two InitRandom hooks.
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

DEFINE_HOOK(0x52FE43, InitRandom_AfterInit_SaveState, 0x5)
{
	if (gReplay.HeaderPrepared)
		return 0;

	const auto* pConfig = GetConfig();
	if (!pConfig || ReplaySystem::IsPlaybackRequested() || !pConfig->EnableReplayRecording)
		return 0;

	if (!ScenarioClass::Instance)
		return 0;

	gReplay.HeaderPrepared = WriteInitialReplayFile();
	if (!gReplay.HeaderPrepared)
		Debug::Log("[Replay] Failed to prepare replay header file.\n");

	return 0;
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
//
//namespace
//{
//	constexpr int ReplayProbeFrameThreshold = 95;
//
//	using NoArgBoolFn = bool(__cdecl*)();
//	using NoArgVoidFn = void(__cdecl*)();
//
//	void LogMainLoopProbe(const char* probeName, REGISTERS* R)
//	{
//		if (Unsorted::CurrentFrame > ReplayProbeFrameThreshold)
//		{
//			Debug::Log("[Replay][Probe] frame=%d origin=%06X %s\n",
//				Unsorted::CurrentFrame, R->Origin(), probeName);
//		}
//	}
//}
//
//// Run_Game_Scenarios loop probes around MainLoop / ShowSpecialDialog / ProcessPendingSessionExitState_Thunk.
//DEFINE_HOOK(0x48CE6D, RunGameScenarios_MainLoopProbe_InitSpecialDialogReset, 0x6)
//{
//	LogMainLoopProbe("RunGameScenarios.InitSpecialDialogReset", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x48CE73, RunGameScenarios_MainLoopProbe_InitUnderParMessageFlag, 0x6)
//{
//	LogMainLoopProbe("RunGameScenarios.InitUnderParMessageFlag", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x48CE79, RunGameScenarios_MainLoopProbe_LoadSessionModeContext, 0x5)
//{
//	LogMainLoopProbe("RunGameScenarios.LoadSessionModeContext", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x48CE7E, RunGameScenarios_MainLoopProbe_SetDialogGateFlag, 0x7)
//{
//	LogMainLoopProbe("RunGameScenarios.SetDialogGateFlag", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x48CE93, RunGameScenarios_MainLoopProbe_ProcessPendingExit_BeforeDialog, 0x5)
//{
//	enum { Continue = 0x48CE98 };
//
//	LogMainLoopProbe("RunGameScenarios.ProcessPendingExit.BeforeDialog", R);
//
//	const auto processPendingExitThunk = reinterpret_cast<NoArgBoolFn>(0x0055CFD0);
//	R->EAX(static_cast<DWORD>(processPendingExitThunk() ? 1u : 0u));
//	return Continue;
//}
//
//DEFINE_HOOK(0x48CE9C, RunGameScenarios_MainLoopProbe_ShowSpecialDialogCall, 0x5)
//{
//	enum { Continue = 0x48CEA1 };
//
//	LogMainLoopProbe("RunGameScenarios.ShowSpecialDialog", R);
//
//	const auto showSpecialDialog = reinterpret_cast<NoArgVoidFn>(0x0048C8B0);
//	showSpecialDialog();
//	return Continue;
//}
//
//DEFINE_HOOK(0x48CEA1, RunGameScenarios_MainLoopProbe_ProcessPendingExit_AfterDialog, 0x5)
//{
//	enum { Continue = 0x48CEA6 };
//
//	LogMainLoopProbe("RunGameScenarios.ProcessPendingExit.AfterDialog", R);
//
//	const auto processPendingExitThunk = reinterpret_cast<NoArgBoolFn>(0x0055CFD0);
//	R->EAX(static_cast<DWORD>(processPendingExitThunk() ? 1u : 0u));
//	return Continue;
//}

//DEFINE_HOOK(0x48CEAF, RunGameScenarios_MainLoopProbe_PreScenarioResume, 0x5)
//{
//	LogMainLoopProbe("RunGameScenarios.PreScenarioResume", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x48CEBF, RunGameScenarios_MainLoopProbe_ClearAttractFlag, 0x6)
//{
//	LogMainLoopProbe("RunGameScenarios.ClearAttractFlag", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x48CEC7, RunGameScenarios_MainLoopProbe_ClearLoopStateFlag, 0x6)
//{
//	LogMainLoopProbe("RunGameScenarios.ClearLoopStateFlag", R);
//	return 0;
//}
//
//
//DEFINE_HOOK(0x55D88E, MainLoop_Probe_ReadInputActiveFlag, 0x5)
//{
//	LogMainLoopProbe("MainLoop.ReadInputActiveFlag", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D8C9, MainLoop_Probe_ReadCurrentFrame, 0x5)
//{
//	LogMainLoopProbe("MainLoop.ReadCurrentFrame", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D8F7, MainLoop_Probe_ReadRecordPlaybackFlags, 0x6)
//{
//	LogMainLoopProbe("MainLoop.ReadRecordPlaybackFlags", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55DA42, MainLoop_Probe_CheckPlaybackBit, 0x6)
//{
//	LogMainLoopProbe("MainLoop.CheckPlaybackBit", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D3BB, MainLoop_Probe_SyncDelay_ReadCurrentFrame, 0x5)
//{
//	LogMainLoopProbe("MainLoop.SyncDelay.ReadCurrentFrame", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D440, MainLoop_Probe_ModeDispatch_ReadSessionModeA, 0x5)
//{
//	LogMainLoopProbe("MainLoop.ModeDispatch.ReadSessionModeA", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D597, MainLoop_Probe_ModeDispatch_ReadSessionModeB, 0x5)
//{
//	LogMainLoopProbe("MainLoop.ModeDispatch.ReadSessionModeB", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D7C2, MainLoop_Probe_RenderGate_ReadSessionMode, 0x5)
//{
//	LogMainLoopProbe("MainLoop.RenderGate.ReadSessionMode", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D821, MainLoop_Probe_ReadSessionObject, 0x5)
//{
//	LogMainLoopProbe("MainLoop.ReadSessionObject", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D859, MainLoop_Probe_ReadIsActiveFlag, 0x6)
//{
//	LogMainLoopProbe("MainLoop.ReadIsActiveFlag", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D903, MainLoop_Probe_ClearPendingInputByte, 0x7)
//{
//	LogMainLoopProbe("MainLoop.ClearPendingInputByte", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D925, MainLoop_Probe_QueueAI_ReadEngineContext, 0x6)
//{
//	LogMainLoopProbe("MainLoop.QueueAI.ReadEngineContext", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D940, MainLoop_Probe_QueueAI_SetEventContextA, 0x5)
//{
//	LogMainLoopProbe("MainLoop.QueueAI.SetEventContextA", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D975, MainLoop_Probe_QueueAI_ReadEventListA, 0x6)
//{
//	LogMainLoopProbe("MainLoop.QueueAI.ReadEventListA", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55DA48, MainLoop_Probe_PlaybackReset_ClearRecordPtrA, 0x6)
//{
//	LogMainLoopProbe("MainLoop.PlaybackReset.ClearRecordPtrA", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55DA4E, MainLoop_Probe_PlaybackReset_ClearRecordPtrB, 0x6)
//{
//	LogMainLoopProbe("MainLoop.PlaybackReset.ClearRecordPtrB", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55DB1B, MainLoop_Probe_DoListLoop_SetGuardFlagA, 0x7)
//{
//	LogMainLoopProbe("MainLoop.DoListLoop.SetGuardFlagA", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55DB75, MainLoop_Probe_DoListLoop_ClearGuardFlag, 0x7)
//{
//	LogMainLoopProbe("MainLoop.DoListLoop.ClearGuardFlag", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55DBB9, MainLoop_Probe_EndPhase_LoadGScreen, 0x5)
//{
//	LogMainLoopProbe("MainLoop.EndPhase.LoadGScreen", R);
//	return 0;
//}
//DEFINE_HOOK(0x0055dbc3, MainLoop_LayerSort, 0x5)
//{
//	LogMainLoopProbe("MainLoop.LayerSort", R);
//	return 0;
//}
//DEFINE_HOOK(0x0055dbcd, MainLoop_AfterLayerSort, 0x6)
//{
//	LogMainLoopProbe("MainLoop.AfterLayerSort", R);
//	return 0;
//}
//DEFINE_HOOK(0x0055dc09, MainLoop_1, 0x2)
//{
//	LogMainLoopProbe("MainLoop.1", R);
//	return 0;
//}
//DEFINE_HOOK(0x0055dc99, MainLoop_4, 0x5)
//{
//	LogMainLoopProbe("MainLoop.4", R);
//	return 0;
//}
//DEFINE_HOOK(0x0055dca3, MainLoop_5, 0x5)
//{
//	LogMainLoopProbe("MainLoop.5", R);
//	return 0;
//}
//DEFINE_HOOK(0x0055dcec, MainLoop_9, 0x5)
//{
//	LogMainLoopProbe("MainLoop.9", R);
//	return 0;
//}
//DEFINE_HOOK(0x0055dd01, MainLoop_10, 0x5)
//{
//	LogMainLoopProbe("MainLoop.10", R);
//	return 0;
//}
//DEFINE_HOOK(0x0055dd2a, MainLoop_11, 0x5)
//{
//	LogMainLoopProbe("MainLoop.11", R);
//	return 0;
//}
//DEFINE_HOOK(0x0055dd47, MainLoop_12, 0x5)
//{
//	LogMainLoopProbe("MainLoop.12", R);
//	return 0;
//}
//DEFINE_HOOK(0x0055dd64, MainLoop_13, 0x7)
//{
//	LogMainLoopProbe("MainLoop.13", R);
//	return 0;
//}
//DEFINE_HOOK(0x0055deb6, MainLoop_14, 0x7)
//{
//	LogMainLoopProbe("MainLoop.14", R);
//	return 0;
//}
//
//
//DEFINE_HOOK(0x0055dda0, MainLoop_16, 0x5)
//{
//	LogMainLoopProbe("MainLoop.16", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x0055de13, MainLoop_17, 0x6)
//{
//	LogMainLoopProbe("MainLoop.17", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x0055de2b, MainLoop_18, 0x6)
//{
//	LogMainLoopProbe("MainLoop.18", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x0055de4f, MainLoop_19, 0x5)
//{
//	LogMainLoopProbe("MainLoop.19", R);
//	return 0;
//}
//
//
//DEFINE_HOOK(0x0055de73, MainLoop_20, 0x6)
//{
//	LogMainLoopProbe("MainLoop.20", R);
//	return 0;
//}
//
////DEFINE_HOOK(0x0055de81, MainLoop_21, 0x6)
////{
////	LogMainLoopProbe("MainLoop.21", R);
////	Unsorted::CurrentFrame = Unsorted::CurrentFrame + 1;
////	return 0x0055de9f;
////}
//
//DEFINE_HOOK(0x0055e171, MainLoop_22, 0x6)
//{
//	LogMainLoopProbe("MainLoop.22", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x0055de89, MainLoop_23, 0x2)
//{
//	LogMainLoopProbe("MainLoop.23", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x00725c71, MainLoop_24, 0x5)
//{
//	LogMainLoopProbe("MainLoop.24", R);
//	return 0;
//}
//DEFINE_HOOK(0x0055de8f, MainLoop_25, 0x5)
//{
//	LogMainLoopProbe("MainLoop.25", R);
//	return 0;
//}
//DEFINE_HOOK(0x0055de8d, MainLoop_26, 0x2)
//{
//	LogMainLoopProbe("MainLoop.26", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D360, MainLoop_Probe_Entry_ReadIsActiveByte, 0x5)
//{
//	LogMainLoopProbe("MainLoop.Entry.ReadIsActiveByte", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D37C, MainLoop_Probe_Entry_SetInMainLoopFlag, 0x7)
//{
//	LogMainLoopProbe("MainLoop.Entry.SetInMainLoopFlag", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D387, MainLoop_Probe_SleepGate_LoadSleepProc, 0x6)
//{
//	LogMainLoopProbe("MainLoop.SleepGate.LoadSleepProc", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D38D, MainLoop_Probe_SleepGate_LoadSessionMode, 0x5)
//{
//	LogMainLoopProbe("MainLoop.SleepGate.LoadSessionMode", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D3CA, MainLoop_Probe_PreSessionUpdate_LoadSessionCtx, 0x5)
//{
//	LogMainLoopProbe("MainLoop.PreSessionUpdate.LoadSessionCtx", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D3DA, MainLoop_Probe_PreSessionUpdate_SaveTickValue, 0x5)
//{
//	LogMainLoopProbe("MainLoop.PreSessionUpdate.SaveTickValue", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D3EF, MainLoop_Probe_SpecialDialog_CheckFlag, 0x7)
//{
//	LogMainLoopProbe("MainLoop.SpecialDialog.CheckFlag", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D401, MainLoop_Probe_SpecialDialog_LoadDialogStateA, 0x5)
//{
//	LogMainLoopProbe("MainLoop.SpecialDialog.LoadDialogStateA", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D410, MainLoop_Probe_SpecialDialog_LoadDialogStateB, 0x6)
//{
//	LogMainLoopProbe("MainLoop.SpecialDialog.LoadDialogStateB", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D456, MainLoop_Probe_ModeDispatch_CheckGameType, 0x7)
//{
//	LogMainLoopProbe("MainLoop.ModeDispatch.CheckGameType", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D463, MainLoop_Probe_ModeDispatch_CheckReplayFlags, 0x7)
//{
//	LogMainLoopProbe("MainLoop.ModeDispatch.CheckReplayFlags", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D47B, MainLoop_Probe_ModeDispatch_SaveDelayA, 0x5)
//{
//	LogMainLoopProbe("MainLoop.ModeDispatch.SaveDelayA", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D486, MainLoop_Probe_ModeDispatch_SaveDelayB, 0x6)
//{
//	LogMainLoopProbe("MainLoop.ModeDispatch.SaveDelayB", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D491, MainLoop_Probe_DelayCalc_LoadDelayDivisor, 0x6)
//{
//	LogMainLoopProbe("MainLoop.DelayCalc.LoadDelayDivisor", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D4A4, MainLoop_Probe_DelayCalc_SetDefaultDivisor, 0x5)
//{
//	LogMainLoopProbe("MainLoop.DelayCalc.SetDefaultDivisor", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D4E0, MainLoop_Probe_DelayCalc_SaveMinDelay, 0x6)
//{
//	LogMainLoopProbe("MainLoop.DelayCalc.SaveMinDelay", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D50E, MainLoop_Probe_DelayCalc_SaveDelayStart, 0x5)
//{
//	LogMainLoopProbe("MainLoop.DelayCalc.SaveDelayStart", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D51C, MainLoop_Probe_DelayCalc_SetScaleConstant, 0x5)
//{
//	LogMainLoopProbe("MainLoop.DelayCalc.SetScaleConstant", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D528, MainLoop_Probe_DelayCalc_SaveDelayWindow, 0x6)
//{
//	LogMainLoopProbe("MainLoop.DelayCalc.SaveDelayWindow", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D56D, MainLoop_Probe_DelayClamp_LoadBaseline, 0x5)
//{
//	LogMainLoopProbe("MainLoop.DelayClamp.LoadBaseline", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D59C, MainLoop_Probe_LatencyPhase_LoadPeerDelayBaseline, 0x6)
//{
//	LogMainLoopProbe("MainLoop.LatencyPhase.LoadPeerDelayBaseline", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D5A7, MainLoop_Probe_LatencyPhase_StoreFrameDelayAccumulator, 0x6)
//{
//	LogMainLoopProbe("MainLoop.LatencyPhase.StoreFrameDelayAccumulator", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D5BC, MainLoop_Probe_LatencyPhase_InitPlayerScanList, 0x5)
//{
//	LogMainLoopProbe("MainLoop.LatencyPhase.InitPlayerScanList", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D5D0, MainLoop_Probe_LatencyPhase_LoadPlayerDelayTableBase, 0x5)
//{
//	LogMainLoopProbe("MainLoop.LatencyPhase.LoadPlayerDelayTableBase", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D5DD, MainLoop_Probe_LatencyPhase_AdvancePlayerScanCursor, 0x5)
//{
//	LogMainLoopProbe("MainLoop.LatencyPhase.AdvancePlayerScanCursor", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D606, MainLoop_Probe_LatencyPhase_LoadDelayWindowStartA, 0x5)
//{
//	LogMainLoopProbe("MainLoop.LatencyPhase.LoadDelayWindowStartA", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D60B, MainLoop_Probe_LatencyPhase_LoadDelayWindowSpanA, 0x6)
//{
//	LogMainLoopProbe("MainLoop.LatencyPhase.LoadDelayWindowSpanA", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D618, MainLoop_Probe_LatencyPhase_ReadDelayTimerSourceA, 0x5)
//{
//	LogMainLoopProbe("MainLoop.LatencyPhase.ReadDelayTimerSourceA", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D681, MainLoop_Probe_LatencyPhase_LoadDelayWindowStartB, 0x5)
//{
//	LogMainLoopProbe("MainLoop.LatencyPhase.LoadDelayWindowStartB", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D686, MainLoop_Probe_LatencyPhase_LoadDelayWindowSpanB, 0x6)
//{
//	LogMainLoopProbe("MainLoop.LatencyPhase.LoadDelayWindowSpanB", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D690, MainLoop_Probe_LatencyPhase_ReadDelayTimerSourceB, 0x5)
//{
//	LogMainLoopProbe("MainLoop.LatencyPhase.ReadDelayTimerSourceB", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D6FD, MainLoop_Probe_LatencyPhase_LoadDelayWindowStartC, 0x5)
//{
//	LogMainLoopProbe("MainLoop.LatencyPhase.LoadDelayWindowStartC", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D702, MainLoop_Probe_LatencyPhase_LoadDelayWindowSpanC, 0x6)
//{
//	LogMainLoopProbe("MainLoop.LatencyPhase.LoadDelayWindowSpanC", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D70C, MainLoop_Probe_LatencyPhase_ReadDelayTimerSourceC, 0x5)
//{
//	LogMainLoopProbe("MainLoop.LatencyPhase.ReadDelayTimerSourceC", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D79E, MainLoop_Probe_RenderState_LoadScenarioSpeed, 0x6)
//{
//	LogMainLoopProbe("MainLoop.RenderState.LoadScenarioSpeed", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D7B1, MainLoop_Probe_RenderState_StoreRenderTickStart, 0x5)
//{
//	LogMainLoopProbe("MainLoop.RenderState.StoreRenderTickStart", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D7B6, MainLoop_Probe_RenderState_StoreRenderTickEnd, 0x6)
//{
//	LogMainLoopProbe("MainLoop.RenderState.StoreRenderTickEnd", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D7BC, MainLoop_Probe_RenderState_StoreRenderDelayBudget, 0x6)
//{
//	LogMainLoopProbe("MainLoop.RenderState.StoreRenderDelayBudget", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55D7D4, MainLoop_Probe_RenderState_LoadSessionRenderState, 0x6)
//{
//	LogMainLoopProbe("MainLoop.RenderState.LoadSessionRenderState", R);
//	return 0;
//}
//
//DEFINE_HOOK(0x55DECF, MainLoop_Probe_EarlyExit_ReturnTruePath, 0x5)
//{
//	LogMainLoopProbe("MainLoop.EarlyExit.ReturnTruePath", R);
//	return 0;
//}

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
		RestoreFrameState();
	}

	if ((gReplay.Recording || gReplay.Playback)
		&& gReplay.ReplayFile != INVALID_HANDLE_VALUE
		&& Unsorted::CurrentFrame > 0
		&& (Unsorted::CurrentFrame % REPLAY_FLUSH_INTERVAL_FRAMES) == 0)
	{
		FlushFileBuffers(gReplay.ReplayFile);
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

// Keep the original Queue_AI_Multiplayer flow intact (including Wait_For_Players/input handling),
// but avoid emitting network packets during replay playback to prevent network side effects.
//Note: This is blocking the Escape/options menu in playback.
//DEFINE_HOOK(0x00647F93, Queue_AI_Multiplayer_SkipSendPacketsDuringPlayback, 0x6)
//{
//	if (gReplay.Playback && gReplay.RunPlayback)
//	{
//		R->EAX(0);
//		return 0x00647FB9;
//	}
//
//	// Preserve overwritten instruction: MOV ECX,[0x00AFA400]
//	R->ECX(*reinterpret_cast<int*>(0x00AFA400));
//	return 0x00647F99;
//}

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

//Todo: add flush on desync (might be covered by the above, need to test)


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

// SessionClass::Resume (0x69BAF1) calls ShowWaitingForPlayersScreen then sets
// WaitingForPlayersAutoHideFrame = CurrentFrame + 3. In observer/replay mode the
// "waiting for players" panel object ([0x0087F770]) is null, so both Show and Hide crash.
// Skip the entire block when playback is active.
//DEFINE_HOOK(0x69BB01, SessionClass_Resume_SkipWaitingForPlayersPanel, 0x5)
//{
//	enum { Skip = 0x69BB15 };
//	return ReplaySystem::IsPlaybackActive() ? Skip : 0;
//}
//
//// SyncDelay_Start (0x55E160) synchronises frame timing. In multiplayer mode it runs an
//// NFT (network frame timing) busy-wait loop that calls Call_Back() and TacticalClass
//// vtable functions. In observer/replay mode these calls crash once NFTTimer_Accumulated
//// goes non-zero (typically around frame 100). Skip directly to the FrameTimer delay path
//// (0x55E2B4) used by Campaign/Skirmish, which is safe in observer mode.
//DEFINE_HOOK(0x0055e160, SyncDelay_SkipNFTLoopInPlayback, 0x6)
//{
//	enum { FrameTimerPath = 0x55E2B4 };
//	return ReplaySystem::IsPlaybackActive() ? FrameTimerPath : 0;
//}

//DEFINE_HOOK(0x006c6f50, Send_Statistics_Override, 0x5)
//{
//	return 0x006c87c2;
//}
//DEFINE_HOOK(0x0055e1bc, SyncDelay_SkipNFTLoopInPlayback, 0x6)
//{
//	enum { FrameTimerPath = 0x55E2B4 };
//	return ReplaySystem::IsPlaybackActive() ? FrameTimerPath : 0;
//}
