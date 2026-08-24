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
#include "ReplaySystem.Internal.h"

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
#include <RulesClass.h>
#include <SessionClass.h>
#include <TacticalClass.h>
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

namespace ReplaySystem
{

namespace Internal
{

ReplayRuntimeState ReplayState;

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

void ApplyReplayTimingFromCurrentGameSpeed()
{
	if (ReplayState.Playback && ReplayState.HasPlaybackHeader)
	{
		// Keep simulation speed locked to the replay's recorded speed.
		const int recordedGameSpeed = std::clamp(static_cast<int>(ReplayState.PlaybackHeader.RecordedGameSpeed), 0, MAX_GAME_SPEED_INDEX);
		GameOptionsClass::Instance.GameSpeed = recordedGameSpeed;
		GameModeOptionsClass::Instance.GameSpeed = recordedGameSpeed;
	}

	const int speedIndex = (ReplayState.Playback && ReplayState.PlaybackSpeedIndex >= 0)
		? ReplayState.PlaybackSpeedIndex
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
	if (ReplayState.Writer.IsActive())
		return ReplayState.Writer.Write(data, size);

	return WriteRawToHandle(ReplayState.ReplayFile, data, size);
}

bool ReadRaw(void* buffer, size_t size)
{
	if (ReplayState.Reader.IsActive())
		return ReplayState.Reader.Read(buffer, size);

	return ReadRawFromHandle(ReplayState.ReplayFile, buffer, size);
}

// Ends the current deflate block, so everything recorded up to now can be decoded without any of
// the bytes that follow it. Called on a frame cadence rather than a byte one, because what
// matters after a crash is how many frames of play survived, not how many bytes.
void SyncFlushRecordingStream()
{
	if (!ReplayState.Writer.IsActive())
		return;

	if (!ReplayState.Writer.SyncFlush())
	{
		Debug::Log("[Replay] Failed to flush the replay stream; stopping the recording.\n");
		AbortReplaySystem();
		return;
	}

	// Committing to the disk is far more expensive, so it stays on a rare byte-count schedule.
	const uint64_t written = ReplayState.Writer.CompressedBytesWritten();
	if (written - ReplayState.BytesAtLastDiskFlush >= REPLAY_FLUSH_INTERVAL_BYTES)
	{
		FlushFileBuffers(ReplayState.ReplayFile);
		ReplayState.BytesAtLastDiskFlush = written;
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

// Blanks every address value in the embedded spawn.ini. Deliberately section-unaware: it matches
// on key name alone, so a new section carrying an address is covered without anyone remembering to
// add it here. Line endings and every other key are preserved byte for byte.
bool EqualsIgnoreCase(const char* a, size_t aLength, const char* b)
{
	const size_t bLength = strlen(b);
	if (aLength != bLength)
		return false;

	for (size_t i = 0; i < aLength; ++i)
	{
		if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i])))
			return false;
	}

	return true;
}

// Reads one key out of a raw INI buffer, without building an INI object over it. Used for the
// embedded spawnmap.ini, which is the whole map file: handing a couple of megabytes to CCINIClass
// just to read the map name is the most expensive thing recording start does. Handles what a map
// file's [Basic] section actually contains - case-insensitive section and key names, whitespace
// around either, and ';' comments - and nothing more.
bool TryReadIniValueFromBuffer(const std::vector<char>& text, const char* sectionName,
	const char* keyName, char* outBuffer, size_t outBufferSize)
{
	if (!outBuffer || outBufferSize == 0)
		return false;

	outBuffer[0] = '\0';

	const char* const base = text.data();
	const size_t size = text.size();
	bool inSection = false;

	size_t lineStart = 0;
	while (lineStart < size)
	{
		size_t lineEnd = lineStart;
		while (lineEnd < size && base[lineEnd] != '\n')
			++lineEnd;

		size_t begin = lineStart;
		size_t end = lineEnd;
		if (end > begin && base[end - 1] == '\r')
			--end;

		// A ';' anywhere outside a value starts a comment; map files do not quote values.
		for (size_t i = begin; i < end; ++i)
		{
			if (base[i] == ';')
			{
				end = i;
				break;
			}
		}

		while (begin < end && isspace(static_cast<unsigned char>(base[begin])))
			++begin;
		while (end > begin && isspace(static_cast<unsigned char>(base[end - 1])))
			--end;

		if (begin < end && base[begin] == '[')
		{
			const size_t close = static_cast<size_t>(
				std::find(base + begin, base + end, ']') - base);
			inSection = close < end
				&& EqualsIgnoreCase(base + begin + 1, close - begin - 1, sectionName);
		}
		else if (inSection && begin < end)
		{
			const size_t separator = static_cast<size_t>(
				std::find(base + begin, base + end, '=') - base);

			if (separator < end)
			{
				size_t keyEnd = separator;
				while (keyEnd > begin && isspace(static_cast<unsigned char>(base[keyEnd - 1])))
					--keyEnd;

				if (EqualsIgnoreCase(base + begin, keyEnd - begin, keyName))
				{
					size_t valueStart = separator + 1;
					while (valueStart < end && isspace(static_cast<unsigned char>(base[valueStart])))
						++valueStart;

					if (valueStart >= end)
						return false;

					const size_t length = std::min(end - valueStart, outBufferSize - 1);
					memcpy(outBuffer, base + valueStart, length);
					outBuffer[length] = '\0';
					return true;
				}
			}
		}

		lineStart = lineEnd + 1;
	}

	return false;
}

void SanitizeSpawnIniForReplay(std::vector<char>& spawnIni)
{
	static constexpr const char* AddressKeys[] = { "Ip", "IPv6", "LanIP" };
	static constexpr const char* BlankedAddress = "0.0.0.0";

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
				if (EqualsIgnoreCase(base + keyStart, keyEnd - keyStart, addressKey))
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

ReplayHeader BuildReplayHeader(const std::vector<char>& spawnMap, uint32_t spawnIniSize, uint32_t spawnMapSize)
{
	ReplayHeader header {};
	header.Magic = REPLAY_MAGIC;
	header.Version = REPLAY_VERSION;

	const ScopedINIFile spawnIni { "spawn.ini" };

	// ScenarioClass is not an option for the map name: this runs from inside Clear_Scenario, which
	// has already blanked it, and its UIName is a 32 byte buffer besides.
	if (!TryReadIniValueFromBuffer(spawnMap, "Basic", "Name", header.MapName, sizeof(header.MapName))
		&& !TryReadString(spawnIni.Get(), "Settings", "UIMapName", header.MapName, sizeof(header.MapName))
		&& ScenarioClass::Instance)
		strncpy_s(header.MapName, sizeof(header.MapName), ScenarioClass::Instance->FileName, _TRUNCATE);

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
	// Both stamped by StopReplaySystem; a recording that dies with the process keeps these zeroed,
	// which is how a reader recognises it as incomplete.
	header.TotalFrames = 0;
	header.Flags = ReplayHeaderFlag_None;
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
		spawnMap,
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
	ReplayState.Writer.Reset();
	ReplayState.Reader.Reset();

	if (ReplayState.ReplayFile != INVALID_HANDLE_VALUE)
	{
		FlushFileBuffers(ReplayState.ReplayFile);
		CloseHandle(ReplayState.ReplayFile);
		ReplayState.ReplayFile = INVALID_HANDLE_VALUE;
	}
}

bool OpenRecordingReplayStream()
{
	CloseReplayFile();

	const char* const outputPath = GetRecordingOutputPath();

	ReplayState.ReplayFile = CreateFileA(
		outputPath,
		GENERIC_WRITE,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr
	);

	if (ReplayState.ReplayFile == INVALID_HANDLE_VALUE)
	{
		// WriteInitialReplayFile has just written this file. Recreating it would start the deflate
		// stream at offset 0 and produce a headerless file no reader can parse.
		Debug::Log("[Replay] Could not reopen the replay file for recording: %s\n", outputPath);
		return false;
	}

	SetFilePointer(ReplayState.ReplayFile, 0, nullptr, FILE_END);

	// Everything from here on - and nothing before it - is deflated.
	if (!ReplayState.Writer.Start(ReplayState.ReplayFile))
	{
		Debug::Log("[Replay] Failed to start the compressed replay stream.\n");
		CloseReplayFile();
		return false;
	}

	ReplayState.BytesAtLastDiskFlush = 0;
	ReplayState.LastSyncFlushFrame = 0;
	return true;
}

bool OpenPlaybackReplayStream(const char* replayPath)
{
	CloseReplayFile();

	ReplayState.ReplayFile = CreateFileA(
		replayPath,
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr
	);

	if (ReplayState.ReplayFile == INVALID_HANDLE_VALUE)
		return false;

	ReplayHeader header {};
	if (!ReadReplayHeaderFromHandle(ReplayState.ReplayFile, header))
	{
		CloseReplayFile();
		return false;
	}

	// The sizes come straight off disk, so check them against the file before seeking.
	const uint64_t payloadSize = static_cast<uint64_t>(header.SpawnIniSize) + header.SpawnMapSize;

	LARGE_INTEGER fileSize {};
	if (!GetFileSizeEx(ReplayState.ReplayFile, &fileSize)
		|| sizeof(ReplayHeader) + payloadSize > static_cast<uint64_t>(fileSize.QuadPart))
	{
		Debug::Log("[Replay] Replay declares embedded file sizes that do not fit the file.\n");
		CloseReplayFile();
		return false;
	}

	LARGE_INTEGER payloadOffset {};
	payloadOffset.QuadPart = static_cast<LONGLONG>(payloadSize);

	if (payloadSize > 0 && !SetFilePointerEx(ReplayState.ReplayFile, payloadOffset, nullptr, FILE_CURRENT))
	{
		CloseReplayFile();
		return false;
	}

	// The frame stream is always deflated; the header and the two INIs above it never are.
	if (!ReplayState.Reader.Start(ReplayState.ReplayFile))
	{
		Debug::Log("[Replay] Failed to start reading the compressed replay stream.\n");
		CloseReplayFile();
		return false;
	}

	return true;
}

void ResetRuntimeFlagsForScenario()
{
	ReplayState.Recording = false;
	ReplayState.Playback = false;
	ReplayState.SpectatorView = false;
	ReplayState.PlaybackSpeedIndex = -1;
	ReplayState.ExpectedEventsThisFrame = 0;
	ReplayState.BytesAtLastDiskFlush = 0;
	ReplayState.LastSyncFlushFrame = 0;
	ReplayState.ProgressBarForcedComplete = false;
	ReplayState.PendingFrameStates.clear();
	ReplayState.PendingSideChannelEvents.clear();
	ReplayState.HasPlaybackHeader = false;
	memset(&ReplayState.PlaybackHeader, 0, sizeof(ReplayState.PlaybackHeader));
	ReplayState.HasPendingPlaybackFrame = false;
	ReplayState.PlaybackStreamEnded = false;
	ReplayState.PendingPlaybackFrame = {};
	ReplayState.LockedViewportPos = { 0, 0 };
	ReplayState.HasLockedViewportPos = false;
	ReplayState.HasLastWrittenFrameState = false;
	ReplayState.LastWrittenFrameNumber = 0;
	ReplayState.LastRecordedTacticalPos = { 0, 0 };
	ReplayState.LastRecordedSelectionIDs.clear();
}

void AbortReplaySystem()
{
	ResetRuntimeFlagsForScenario();
	CloseReplayFile();
}

void ApplyPlaybackInitialState()
{
	if (!ReplayState.HasPlaybackHeader)
		return;

	Game::Seed = ReplayState.PlaybackHeader.Seed;
	if (ScenarioClass::Instance)
	{
		ScenarioClass::Instance->Random.Next1 = ReplayState.PlaybackHeader.RandomNext1;
		ScenarioClass::Instance->Random.Next2 = ReplayState.PlaybackHeader.RandomNext2;
		memcpy(ScenarioClass::Instance->Random.Table, ReplayState.PlaybackHeader.RandomizerTable, sizeof(ReplayState.PlaybackHeader.RandomizerTable));
		ScenarioClass::Instance->UniqueID = ReplayState.PlaybackHeader.UniqueIDCounter;
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

// Allow these to remain in DoList during playback so local controls work. GameSpeed is deliberately
// not among them: its requested value is harvested before the removal pass, and letting the engine
// execute it would write Options.GameSpeed - which simulation code reads and playback pins to the
// recorded speed - for the remainder of the frame.
bool IsLocalPlaybackControlEvent(const EventClass& event)
{
	switch (event.Type)
	{
	case EventType::Options:
	case EventType::Exit:
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
	std::vector<EventClass>& preservedEvents = ReplayState.PreservedEventsScratch;
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
	if (!ReplayState.Playback)
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
			if (requestedSpeed != ReplayState.PlaybackSpeedIndex)
			{
				ReplayState.PlaybackSpeedIndex = requestedSpeed;
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

// Fills rather than returns, so the caller's buffer is reused: this runs on every main loop
// iteration of a recording game.
void FillSelectedObjectIDs(std::vector<uint32_t>& ids)
{
	auto& currentObjects = ObjectClass::CurrentObjects;
	ids.clear();
	ids.reserve(currentObjects.Count);

	for (int i = 0; i < currentObjects.Count; ++i)
	{
		ObjectClass* pObj = currentObjects.Items[i];
		if (pObj)
			ids.push_back(static_cast<uint32_t>(pObj->UniqueID));
	}
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

// Record this frame when deterministic events or visible replay state changed.
bool WriteFrameCapture(const PendingRecordedFrameCapture& capture, int eventsThisFrame,
	const std::vector<SideChannelRecord>& sideChannelEvents)
{
	const bool tacticalPosChanged = !ReplayState.HasLastWrittenFrameState
		|| capture.TacticalPos.X != ReplayState.LastRecordedTacticalPos.X
		|| capture.TacticalPos.Y != ReplayState.LastRecordedTacticalPos.Y;

	const bool selectionChanged = !ReplayState.HasLastWrittenFrameState
		|| capture.SelectedObjectIDs != ReplayState.LastRecordedSelectionIDs;

	const bool hasSideChannelEvents = !sideChannelEvents.empty();

	if (eventsThisFrame == 0 && !tacticalPosChanged && !selectionChanged && !hasSideChannelEvents)
		return true;

	FrameRecordHeader header {};
	header.FrameNumber = capture.FrameNumber;
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
		&& !WriteRaw(&capture.TacticalPos, sizeof(capture.TacticalPos)))
	{
		return false;
	}

	if ((header.Flags & FrameRecordFlag_Selection) != 0u)
	{
		const int32_t selectedObjectCount = static_cast<int32_t>(capture.SelectedObjectIDs.size());
		if (!WriteRaw(&selectedObjectCount, sizeof(selectedObjectCount)))
			return false;

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

	ReplayState.HasLastWrittenFrameState = true;
	ReplayState.LastWrittenFrameNumber = capture.FrameNumber;
	ReplayState.LastRecordedTacticalPos = capture.TacticalPos;
	ReplayState.LastRecordedSelectionIDs = capture.SelectedObjectIDs;

	return true;
}

void FlushPendingRecordedFramesThrough(int frameNumber, int currentFrameEventCount)
{
	while (!ReplayState.PendingFrameStates.empty())
	{
		const auto& capture = ReplayState.PendingFrameStates.front();
		const int pendingFrame = capture.FrameNumber;
		if (pendingFrame > frameNumber)
			break;

		// Keep playback parsing bounded; excess side-channel records spill into later frames.
		std::vector<SideChannelRecord>& sideChannelForFrame = ReplayState.SideChannelScratch;
		sideChannelForFrame.clear();
		while (!ReplayState.PendingSideChannelEvents.empty()
			&& ReplayState.PendingSideChannelEvents.front().FrameNumber <= pendingFrame
			&& sideChannelForFrame.size() < static_cast<size_t>(SIDECHANNEL_MAX_EVENTS_PER_FRAME))
		{
			sideChannelForFrame.push_back(ReplayState.PendingSideChannelEvents.front());
			ReplayState.PendingSideChannelEvents.pop_front();
		}

		const int eventCount = pendingFrame == frameNumber ? currentFrameEventCount : 0;
		if (!WriteFrameCapture(capture, eventCount, sideChannelForFrame))
		{
			Debug::Log("[Replay] Failed to write frame capture.\n");
			AbortReplaySystem();
			return;
		}

		ReplayState.PendingFrameStates.pop_front();
	}
}

void RecordFrameState()
{
	if (!ReplayState.Recording)
		return;

	const int frameNumber = Unsorted::CurrentFrame;

	// The main loop runs faster than the game frame, so the same frame is usually captured several
	// times over. Overwriting the pending entry in place reuses its buffer instead of building and
	// moving a fresh capture on every pass.
	if (ReplayState.PendingFrameStates.empty()
		|| ReplayState.PendingFrameStates.back().FrameNumber != frameNumber)
	{
		ReplayState.PendingFrameStates.emplace_back();
		ReplayState.PendingFrameStates.back().FrameNumber = frameNumber;
	}

	PendingRecordedFrameCapture& capture = ReplayState.PendingFrameStates.back();

	capture.TacticalPos = TacticalClass::Instance
		? TacticalClass::Instance->TacticalCoord1
		: Point2D { 0, 0 };

	FillSelectedObjectIDs(capture.SelectedObjectIDs);
}

bool WriteReplayEndOfStreamMarker()
{
	FrameRecordHeader marker {};
	marker.FrameNumber = -1;
	return WriteRaw(&marker, sizeof(marker));
}

// Seeks back and stamps the frame count and the clean-shutdown flag into the header. Only ever
// reached once recording has finished cleanly, so a recording that dies with the process leaves
// both fields zero - the only signal a reader has that the frame stream is cut short.
bool StampCleanShutdownIntoHeader()
{
	if (ReplayState.ReplayFile == INVALID_HANDLE_VALUE)
		return false;

	static_assert(offsetof(ReplayHeader, Flags) == offsetof(ReplayHeader, TotalFrames) + sizeof(uint32_t),
		"TotalFrames and Flags are stamped in one write and have to stay adjacent");

	const LONG headerOffset = static_cast<LONG>(offsetof(ReplayHeader, TotalFrames));
	if (SetFilePointer(ReplayState.ReplayFile, headerOffset, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
		return false;

	const uint32_t tail[2] = {
		static_cast<uint32_t>(std::max(0, ReplayState.LastWrittenFrameNumber)),
		ReplayHeaderFlag_CleanShutdown
	};

	DWORD bytesWritten = 0;
	const bool ok = WriteFile(ReplayState.ReplayFile, tail, sizeof(tail), &bytesWritten, nullptr) != FALSE
		&& bytesWritten == sizeof(tail);

	SetFilePointer(ReplayState.ReplayFile, 0, nullptr, FILE_END);
	return ok;
}

// Replays are shared, so records off disk are untrusted: the text arrays need not be terminated
// and House/Aux index straight into engine arrays. False means the record cannot be made safe.
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
		if (record.House < 0 || record.House >= MAX_HOUSES
			|| record.Aux < -1 || record.Aux >= MAX_BEACON_SLOTS)
		{
			return false;
		}

		return record.Coord.X >= 0 && record.Coord.X < MAX_MAP_LEPTON_COORD
			&& record.Coord.Y >= 0 && record.Coord.Y < MAX_MAP_LEPTON_COORD;

	case SideChannelEventType::BeaconDelete:
	case SideChannelEventType::BeaconText:
		return record.House >= 0 && record.House < MAX_HOUSES
			&& record.Aux >= 0 && record.Aux < MAX_BEACON_SLOTS;

	case SideChannelEventType::Taunt:
		// Taunts_752B70 range-checks the command itself, but do not rely on a function we do not
		// own to be the only bound on a value that came off disk.
		return record.Aux >= 0 && record.Aux < MAX_TAUNT_COMMAND_COUNT;

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
		if (!ReadRaw(&record.SelectedObjectCount, sizeof(record.SelectedObjectCount)))
			return false;

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

	if (!ReplayState.SelectUnits)
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
	if (!ReplayState.ShowChatAndBeacons)
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
		using BeaconMessageFn = char(__thiscall*)(BeaconManagerClass*, const wchar_t*, int, int, char);
		reinterpret_cast<BeaconMessageFn>(BEACON_MANAGER_MESSAGE_ADDRESS)(
			&BeaconManagerClass::Instance, record.Text, record.House, record.Aux, 1);
		break;
	}

	case SideChannelEventType::Taunt:
	{
		using TauntsFn = int(__fastcall*)(int);
		reinterpret_cast<TauntsFn>(TAUNTS_ADDRESS)(record.Aux);
		break;
	}

	default:
		break;
	}
}

void StopReplaySystem()
{
	if (ReplayState.Recording)
	{
		FlushPendingRecordedFramesThrough(std::numeric_limits<int>::max(), 0);
		if (ReplayState.ReplayFile != INVALID_HANDLE_VALUE)
		{
			if (!WriteReplayEndOfStreamMarker())
				Debug::Log("[Replay] Failed to write replay end-of-stream marker.\n");

			// The header stamp seeks to the start of the file, so the stream has to be terminated
			// and fully written out first.
			if (ReplayState.Writer.IsActive() && !ReplayState.Writer.Finish())
				Debug::Log("[Replay] Failed to finish the compressed replay stream.\n");

			if (!StampCleanShutdownIntoHeader())
				Debug::Log("[Replay] Failed to stamp the total frame count into the replay header.\n");
		}
	}

	AbortReplaySystem();
}

// Pins the camera to the last recorded viewport position.
void ApplyLockedViewport()
{
	if (!ReplayState.LockViewport || !ReplayState.HasLockedViewportPos || !TacticalClass::Instance)
		return;

	auto* tc = TacticalClass::Instance;
	const Point2D& target = ReplayState.LockedViewportPos;

	// Avoid forcing a full repaint when the viewport is already correct.
	if (tc->TacticalCoord1.X == target.X && tc->TacticalCoord1.Y == target.Y)
		return;

	tc->TacticalCoord1 = target;
	tc->TacticalCoord2 = target;

	const auto RecalcViewport
		= reinterpret_cast<void(__thiscall*)(TacticalClass*)>(TACTICAL_RECALCULATE_VIEWPORT_ADDRESS);
	RecalcViewport(tc);

	tc->Redrawing = true;
}

// Whether playback should show the whole map instead of the recording player's shroud.
bool PlaybackWantsFullMapReveal()
{
	return ReplayState.Playback && (!ReplayState.ShroudEnabled || ReplayState.SpectatorView);
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
	if (!ReplayState.Playback)
		return;

	ReplayState.ExpectedEventsThisFrame = 0;

	// Keep the viewport locked on frames without replay records.
	ApplyLockedViewport();

	MaintainFullMapReveal();

	if (!ReplayState.HasPendingPlaybackFrame && !ReplayState.PlaybackStreamEnded)
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
			ReplayState.PlaybackStreamEnded = true;
		}
		else
		{
			ReplayState.PendingPlaybackFrame = std::move(nextRecord);
			ReplayState.HasPendingPlaybackFrame = true;
		}
	}

	if (!ReplayState.HasPendingPlaybackFrame)
		return;

	const auto& frameRecord = ReplayState.PendingPlaybackFrame;
	if (frameRecord.FrameNumber < Unsorted::CurrentFrame)
	{
		Debug::Log("[Replay] Frame mismatch during playback (expected %u got %d).\n",
			Unsorted::CurrentFrame, frameRecord.FrameNumber);
		StopReplaySystem();
		return;
	}

	if (frameRecord.FrameNumber > Unsorted::CurrentFrame)
		return;

	ReplayState.ExpectedEventsThisFrame = frameRecord.EventCountThisFrame;

	if ((frameRecord.Flags & FrameRecordFlag_TacticalPos) != 0u)
	{
		// Track this even when locking is disabled.
		ReplayState.LockedViewportPos = frameRecord.TacticalPos;
		ReplayState.HasLockedViewportPos = true;

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

	ReplayState.HasPendingPlaybackFrame = false;
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
	if (!ReplayState.Recording)
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

	if (Unsorted::CurrentFrame - ReplayState.LastSyncFlushFrame >= REPLAY_SYNC_FLUSH_FRAME_INTERVAL)
	{
		ReplayState.LastSyncFlushFrame = Unsorted::CurrentFrame;
		SyncFlushRecordingStream();
	}
}

void PushSideChannelEvent(SideChannelRecord&& record)
{
	record.FrameNumber = Unsorted::CurrentFrame;
	ReplayState.PendingSideChannelEvents.push_back(std::move(record));
}

void PlaybackFrameEvents()
{
	if (!ReplayState.Playback)
		return;

	const int eventsToReplay = ReplayState.ExpectedEventsThisFrame;
	ReplayState.ExpectedEventsThisFrame = 0;

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
	ReplayState.Recording = true;

	const auto* pConfig = GetConfig();
	ReplayState.ShroudEnabled = pConfig ? pConfig->ReplayShroudEnabled : false;
	ReplayState.LockViewport = pConfig ? pConfig->ReplayLockedViewport : true;
	ReplayState.SelectUnits = pConfig ? pConfig->ReplaySelectUnits : true;

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
	ReplayState.Playback = true;

	const auto* pPlaybackConfig = GetConfig();

	// GameOptionsClass::GameSpeed is only a fallback: it still holds what StartScenario applied
	// from spawn.ini, and is overwritten with the recorded speed a few lines down. Reading the
	// playback speed from its own key means this no longer depends on that ordering.
	const int requestedSpeed = (pPlaybackConfig && pPlaybackConfig->ReplayPlaybackSpeed >= 0)
		? pPlaybackConfig->ReplayPlaybackSpeed
		: GameOptionsClass::Instance.GameSpeed;

	ReplayState.PlaybackSpeedIndex = std::clamp(requestedSpeed, 0, MAX_GAME_SPEED_INDEX);

	int recordedGameSpeed = ReplayState.PlaybackSpeedIndex;
	if (!ReplayState.HasPlaybackHeader)
	{
		ReplayHeader header {};
		if (ReadReplayHeaderFromPath(replayPath, header))
		{
			ReplayState.PlaybackHeader = header;
			ReplayState.HasPlaybackHeader = true;
		}
	}

	if (ReplayState.HasPlaybackHeader)
		recordedGameSpeed = std::clamp(static_cast<int>(ReplayState.PlaybackHeader.RecordedGameSpeed), 0, MAX_GAME_SPEED_INDEX);

	GameOptionsClass::Instance.GameSpeed = recordedGameSpeed;
	GameModeOptionsClass::Instance.GameSpeed = recordedGameSpeed;

	ApplyReplayTimingFromCurrentGameSpeed();

	const auto* pConfig = GetConfig();
	ReplayState.ShroudEnabled = pConfig ? pConfig->ReplayShroudEnabled : false;
	ReplayState.LockViewport = pConfig ? pConfig->ReplayLockedViewport : true;
	ReplayState.SelectUnits = pConfig ? pConfig->ReplaySelectUnits : true;
	ReplayState.SpectatorView = pConfig ? pConfig->ReplaySpectator : false;
	ReplayState.ShowChatAndBeacons = pConfig ? pConfig->ReplayShowChatAndBeacons : true;

	strncpy_s(ReplayState.PlaybackPath, sizeof(ReplayState.PlaybackPath), replayPath, _TRUNCATE);

	if (!OpenPlaybackReplayStream(ReplayState.PlaybackPath))
	{
		StopReplaySystem();

		// StartScenario already skipped CreateConnections because ReplayFile was set, so there is no
		// live session to fall back to - carrying on leaves a game that can never advance a frame.
		Debug::FatalErrorAndExit("[Replay] Failed to open replay file for playback: %s",
			ReplayState.PlaybackPath);
	}
}

} // namespace Internal

} // namespace ReplaySystem

// The public API below is defined outside Internal, so pull its names in.
using namespace ReplaySystem::Internal;

void ReplaySystem::RecordChatMessage(int houseIndex, const wchar_t* senderName, const wchar_t* message, int colorSchemeIndex)
{
	if (!ReplayState.Recording)
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
	if (!ReplayState.Recording)
		return;

	SideChannelRecord record {};
	record.Type = static_cast<uint8_t>(SideChannelEventType::Taunt);
	record.Aux = tauntCommand;

	PushSideChannelEvent(std::move(record));
}

void ReplaySystem::RecordBeaconPlace(int houseIndex, const CoordStruct& coord, int beaconSlot)
{
	if (!ReplayState.Recording)
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
	if (!ReplayState.Recording)
		return;

	SideChannelRecord record {};
	record.Type = static_cast<uint8_t>(SideChannelEventType::BeaconDelete);
	record.House = houseIndex;
	record.Aux = beaconSlot;

	PushSideChannelEvent(std::move(record));
}

void ReplaySystem::RecordBeaconText(int houseIndex, int beaconSlot, const wchar_t* text)
{
	if (!ReplayState.Recording)
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
	return ReplayState.Playback;
}

void ReplaySystem::OnGameStartReset()
{
	StopReplaySystem();
	ReplayState.InitRandomHandled = false;
	ReplayState.PlaybackPath[0] = '\0';
}
