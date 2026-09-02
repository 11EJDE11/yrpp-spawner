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

#include "ReplayControls.h"
#include "ReplayOverlay.h"
#include "ReplaySeek.h"
#include "ReplaySystem.h"
#include "ReplaySystem.Internal.h"

#include <Spawner/Spawner.h>
#include <Utilities/Debug.h>
#include <Ext/Event/Body.h>
#include <Ext/INIClass/Body.h>

#include <AbstractClass.h>
#include <BeaconManagerClass.h>
#include <ColorScheme.h>
#include <EventClass.h>
#include <GameModeOptionsClass.h>
#include <GameOptionsClass.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <MessageListClass.h>
#include <MouseClass.h>
#include <RulesClass.h>
#include <SessionClass.h>
#include <TechnoClass.h>
#include <TacticalClass.h>
#include <Timer.h>
#include <Unsorted.h>
#include <VocClass.h>
#include <VoxClass.h>

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

			return DefaultRecordingPath;
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

		// The playback frame rate, in frames per second: whatever the viewer's speed controls last
		// asked for, falling back to the rate the recorded game speed implies.
		int GetPlaybackTargetFPS()
		{
			if (ReplayState.PlaybackFPS > 0)
				return ReplayState.PlaybackFPS;

			return GetReplayFPSFromGameSpeed(GameOptionsClass::Instance.GameSpeed);
		}

		// Milliseconds off the performance counter. The engine's own clocks are 60Hz ticks or whole
		// milliseconds, and neither can express the faster end of the playback speed ladder.
		double PlaybackClockMilliseconds()
		{
			static double ticksPerMillisecond = 0.0;
			if (ticksPerMillisecond == 0.0)
			{
				LARGE_INTEGER frequency {};
				if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart == 0)
					return 0.0;

				ticksPerMillisecond = static_cast<double>(frequency.QuadPart) / 1000.0;
			}

			LARGE_INTEGER counter {};
			if (!QueryPerformanceCounter(&counter))
				return 0.0;

			return static_cast<double>(counter.QuadPart) / ticksPerMillisecond;
		}

		// Holds a frame back to the speed the viewer asked for, then makes the engine's own wait a
		// no-op so the two do not add up. The deadline is accumulated rather than recomputed from the
		// current time, so rates that are not a whole number of milliseconds come out right on
		// average. Overrunning it by more than a frame resets it, so playback carries on from where
		// it is instead of sprinting through a backlog after a pause or a slow frame.
		void ApplyPlaybackFramePacing()
		{
			if (!ReplayState.Playback)
				return;

			// A seek is not being watched, so it runs as fast as the machine manages. The engine
			// still has to be told not to wait, which is what the two timers below do.
			if (ReplaySystem::Seek::IsSeeking())
			{
				Unsorted::GameFrameTimer.TimeLeft = 0;
				Unsorted::NetworkFrameTimer.TimeLeft = 0;
				return;
			}

			if (ReplayState.PlaybackFPS <= 0)
				return;

			const double frameMilliseconds = 1000.0 / ReplayState.PlaybackFPS;
			const double now = PlaybackClockMilliseconds();

			if (now > 0.0)
			{
				if (ReplayState.PlaybackNextFrameDue == 0.0
					|| now > ReplayState.PlaybackNextFrameDue + frameMilliseconds)
				{
					ReplayState.PlaybackNextFrameDue = now;
				}

				for (double remaining = ReplayState.PlaybackNextFrameDue - PlaybackClockMilliseconds();
					remaining > 0.0;
					remaining = ReplayState.PlaybackNextFrameDue - PlaybackClockMilliseconds())
				{
					// Sleep(1) can overshoot by a whole scheduler tick, so it covers the bulk and the
					// last couple of milliseconds are given up with Sleep(0).
					Sleep(remaining > 2.0 ? 1 : 0);
				}

				ReplayState.PlaybackNextFrameDue += frameMilliseconds;
			}

			Unsorted::GameFrameTimer.TimeLeft = 0;
			Unsorted::NetworkFrameTimer.TimeLeft = 0;
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

		bool SkipRaw(size_t size)
		{
			char scratch[512];

			while (size > 0)
			{
				const size_t chunk = std::min(size, sizeof(scratch));
				if (!ReadRaw(scratch, chunk))
					return false;

				size -= chunk;
			}

			return true;
		}

		// Ends the current deflate block, so everything recorded up to now decodes without any of the
		// bytes that follow it. Called on a frame cadence rather than a byte one: what matters after a
		// crash is how many frames of play survived.
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
			if (written - ReplayState.BytesAtLastDiskFlush >= DiskFlushIntervalBytes)
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

		// Reads one key out of a raw INI buffer without building an INI object over it: handing the
		// whole map file to CCINIClass just to read its name is the most expensive thing recording
		// start does. Handles what a map file's [Basic] section contains and nothing more - case,
		// whitespace and ';' comments.
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

		// Blanks every address value in the embedded spawn.ini. Section-unaware: it matches on key
		// name alone, so a new section carrying an address is covered without anyone remembering to
		// add it here. Line endings and every other key are preserved byte for byte.
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
			header.Magic = ReplayMagic;
			header.Version = ReplayVersion;
			header.HeaderSize = sizeof(ReplayHeader);

			const ScopedINIFile spawnIni { "spawn.ini" };

			// ScenarioClass is no use for the map name here: this runs from inside Clear_Scenario,
			// which has already blanked it, and its UIName is a 32 byte buffer besides.
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
			header.RecordedGameSpeed = static_cast<uint32_t>(std::clamp(GameOptionsClass::Instance.GameSpeed, 0, MaxGameSpeedIndex));
			header.RecordedUnixTime = static_cast<uint64_t>(time(nullptr));
			// Both stamped by StopReplaySystem; a recording that dies with the process keeps these zeroed,
			// which is how a reader recognizes it as incomplete.
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

			// spawnmap.ini is only this game's map when spawn.ini points at it. A campaign names its
			// scenario inside the game's own mixes instead, and whatever spawnmap.ini is left in the
			// game directory then belongs to whichever game wrote it last.
			const auto* const pRecordingConfig = GetConfig();
			const bool scenarioIsSpawnMap = pRecordingConfig
				&& _stricmp(pRecordingConfig->ScenarioName, "spawnmap.ini") == 0;

			if (scenarioIsSpawnMap && !ReadRequiredFile("spawnmap.ini", spawnMap))
			{
				Debug::Log("[Replay] Required file spawnmap.ini was not found or could not be read.\n");
				return false;
			}

			if (!scenarioIsSpawnMap)
				spawnMap.clear();

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

		const char* DescribeReplayOpenFailure(ReplayOpenFailure failure)
		{
			switch (failure)
			{
			case ReplayOpenFailure::NotAReplay:
				return "the file is not a replay";
			case ReplayOpenFailure::UnsupportedVersion:
				return "the replay was recorded in a newer format than this version of the game can read";
			case ReplayOpenFailure::Malformed:
				return "the replay header is damaged";
			case ReplayOpenFailure::Unreadable:
			default:
				return "the replay could not be read";
			}
		}

		// Split out from IsReplayHeaderValid so the reason survives as far as the message the player sees.
		ReplayOpenFailure ClassifyReplayHeader(const ReplayHeader& header)
		{
			if (header.Magic != ReplayMagic)
				return ReplayOpenFailure::NotAReplay;

			if (!IsReplayVersionSupported(header.Version))
				return ReplayOpenFailure::UnsupportedVersion;

			// A shorter header than this build's is missing fields it reads; a longer one is fine, and is
			// the whole point of HeaderSize.
			if (header.HeaderSize < sizeof(ReplayHeader))
				return ReplayOpenFailure::Malformed;

			if (!IsReplayGameSpeedIndexValid(header.RecordedGameSpeed))
				return ReplayOpenFailure::Malformed;

			return ReplayOpenFailure::None;
		}

		bool ReadReplayHeaderFromHandle(HANDLE file, ReplayHeader& outHeader, ReplayOpenFailure& outFailure)
		{
			outHeader = {};

			if (!ReadRawFromHandle(file, &outHeader, sizeof(outHeader)))
			{
				outFailure = ReplayOpenFailure::Unreadable;
				return false;
			}

			outFailure = ClassifyReplayHeader(outHeader);
			return outFailure == ReplayOpenFailure::None;
		}

		bool ReadReplayHeaderFromHandle(HANDLE file, ReplayHeader& outHeader)
		{
			ReplayOpenFailure failure = ReplayOpenFailure::None;
			return ReadReplayHeaderFromHandle(file, outHeader, failure);
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

		bool OpenPlaybackReplayStream(const char* replayPath, ReplayOpenFailure& outFailure)
		{
			CloseReplayFile();
			outFailure = ReplayOpenFailure::None;

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
			{
				outFailure = ReplayOpenFailure::Unreadable;
				return false;
			}

			ReplayHeader header {};
			if (!ReadReplayHeaderFromHandle(ReplayState.ReplayFile, header, outFailure))
			{
				CloseReplayFile();
				return false;
			}

			// The sizes come straight off disk, so check them against the file before seeking.
			const uint64_t payloadSize = static_cast<uint64_t>(header.SpawnIniSize) + header.SpawnMapSize;
			const uint64_t streamOffset = static_cast<uint64_t>(header.HeaderSize) + payloadSize;

			LARGE_INTEGER fileSize {};
			if (!GetFileSizeEx(ReplayState.ReplayFile, &fileSize)
				|| streamOffset > static_cast<uint64_t>(fileSize.QuadPart))
			{
				Debug::Log("[Replay] Replay declares embedded file sizes that do not fit the file.\n");
				outFailure = ReplayOpenFailure::Malformed;
				CloseReplayFile();
				return false;
			}

			LARGE_INTEGER streamStart {};
			streamStart.QuadPart = static_cast<LONGLONG>(streamOffset);

			if (!SetFilePointerEx(ReplayState.ReplayFile, streamStart, nullptr, FILE_BEGIN))
			{
				outFailure = ReplayOpenFailure::Unreadable;
				CloseReplayFile();
				return false;
			}

			// The frame stream is always deflated; the header and the two INIs above it never are.
			if (!ReplayState.Reader.Start(ReplayState.ReplayFile))
			{
				Debug::Log("[Replay] Failed to start reading the compressed replay stream.\n");
				outFailure = ReplayOpenFailure::Unreadable;
				CloseReplayFile();
				return false;
			}

			// Kept so a seek can restart the decompressor here. See ReplaySeek.h.
			ReplayState.PlaybackStreamOffset = streamOffset;
			return true;
		}

		void ResetRuntimeFlagsForScenario()
		{
			ReplayState.Recording = false;
			ReplayState.Playback = false;
			ReplayState.SpectatorView = false;
			ReplayState.PlaybackFPS = 0;
			ReplayState.PlaybackNextFrameDue = 0.0;
			ReplayState.PlaybackPaused = false;
			ReplayState.ExpectedEventsThisFrame = 0;
			ReplayState.BytesAtLastDiskFlush = 0;
			ReplayState.LastSyncFlushFrame = 0;
			ReplayState.ExpectedGameCRC = 0;
			ReplayState.HasExpectedGameCRC = false;
			ReplayState.ExpectedCensus = { 0, 0 };
			ReplayState.HasExpectedCensus = false;
			ReplayState.ExpectedRandomState = { 0, 0 };
			ReplayState.HasExpectedRandomState = false;
			ReplayState.CensusMismatchReported = false;
			ReplayState.LastRecordedGameSpeed = -1;
			ReplayState.DivergenceReported = false;
			ReplayState.CheckedFrameCount = 0;
			ReplayState.MismatchedFrameCount = 0;
			ReplayState.LoggedMismatchCount = 0;
			ReplayState.LoggedRecoveryCount = 0;
			ReplayState.FirstMismatchFrame = -1;
			ReplayState.LastMismatchFrame = -1;
			ReplayState.LastCheckMismatched = false;
			ReplayState.PendingFrameStates.clear();
			ReplayState.PendingSideChannelEvents.clear();
			ReplayState.CapturedFrameEventsFrame = -1;
			ReplayState.CapturedFrameEvents.clear();
			ReplayState.HasPlaybackHeader = false;
			memset(&ReplayState.PlaybackHeader, 0, sizeof(ReplayState.PlaybackHeader));
			ReplayState.HasPendingPlaybackFrame = false;
			ReplayState.PlaybackStreamEnded = false;
			ReplayState.PreparedPlaybackFrame = -1;
			ReplayState.PlaybackStreamOffset = 0;
			ReplayState.HighestPlayedFrame = 0;
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
			ReplaySystem::Seek::OnPlaybackStopped();
			ResetRandomDrawTrace();
			ResetMissionTrace();
			ResetPathRequestTrace();
			ResetLandingZoneTrace();
			ResetUpdateOrderTrace();
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

		// Which spawn.ini player slot playback reproduces the screen of; slot 0, the recording player,
		// is both the default and the fallback. A pure function of spawn.ini rather than of
		// ReplayState, because it is read once before the replay system starts and twice after.
		int ResolveViewPlayerIndex()
		{
			const auto* pConfig = GetConfig();
			if (!pConfig || !ReplaySystem::IsPlaybackRequested())
				return 0;

			// A campaign recording has one player slot, so there is nothing to switch to.
			if (pConfig->IsCampaign)
				return 0;

			const int requested = pConfig->ReplayViewPlayer;
			if (requested <= 0 || requested >= static_cast<int>(std::size(pConfig->Players)))
				return 0;

			// AI slots have no [OtherN] section and so never get a NodeNameType, which is what the house
			// lookup in ApplyViewPlayerToCurrentPlayer walks. Only human slots can be watched from.
			return pConfig->Players[requested].IsHuman ? requested : 0;
		}

		// StartScenario creates one NodeNameType per human spawn.ini slot, in slot order, so a slot's node
		// is preceded by exactly the human slots below it.
		int NodeIndexForPlayerSlot(int playerSlot)
		{
			const auto* pConfig = GetConfig();
			if (!pConfig)
				return -1;

			int nodeIndex = 0;
			for (int slot = 0; slot < playerSlot; ++slot)
			{
				if (pConfig->Players[slot].IsHuman)
					++nodeIndex;
			}

			return nodeIndex;
		}

		// Hands the local screen to another player in the recording. Only a change of viewpoint: the
		// recorded event stream is house indexed and is replayed unchanged, and CurrentPlayer is what
		// the engine renders, shrouds, voices and builds the sidebar for.
		void ApplyViewPlayerToCurrentPlayer()
		{
			const auto* const pConfig = GetConfig();
			const int requested = pConfig ? pConfig->ReplayViewPlayer : -1;
			const int playerSlot = ResolveViewPlayerIndex();

			if (playerSlot <= 0)
			{
				// -1 and 0 both mean the recording player and are silent. Anything else named a slot
				// that cannot be watched from, so report it.
				if (requested > 0 && ReplaySystem::IsPlaybackRequested())
				{
					Debug::Log("[Replay] ReplayViewPlayer=%d is not a human player slot; watching from the "
						"recording player.\n", requested);
				}

				return;
			}

			auto& nodes = NodeNameType::Array;
			const int nodeIndex = NodeIndexForPlayerSlot(playerSlot);

			if (nodeIndex < 0 || nodeIndex >= nodes.Count)
			{
				Debug::Log("[Replay] ReplayViewPlayer=%d has no player node; watching from the recording player.\n",
					playerSlot);
				return;
			}

			const auto* const pNode = nodes.GetItem(nodeIndex);
			if (!pNode || pNode->HouseIndex < 0 || pNode->HouseIndex >= HouseClass::Array.Count)
			{
				Debug::Log("[Replay] ReplayViewPlayer=%d did not resolve to a house; watching from the recording player.\n",
					playerSlot);
				return;
			}

			auto* const pHouse = HouseClass::Array.GetItem(pNode->HouseIndex);
			if (!pHouse || !pHouse->IsHumanPlayer)
			{
				Debug::Log("[Replay] ReplayViewPlayer=%d resolved to a house that is not a human player; "
					"watching from the recording player.\n", playerSlot);
				return;
			}

			// Moved rather than just set: Assign_Houses gives both to the recording player's house,
			// and leaving IsInPlayerControl behind would describe a state no peer ever had.
			if (HouseClass::CurrentPlayer)
				HouseClass::CurrentPlayer->IsInPlayerControl = false;

			HouseClass::CurrentPlayer = pHouse;
			pHouse->IsInPlayerControl = true;

			Debug::Log("[Replay] Watching from spawn.ini player slot %d (%ls), house index %d.\n",
				playerSlot, pHouse->UIName, pNode->HouseIndex);
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

		// Allow these to remain in DoList during playback so local controls work. GameSpeed is not among
		// them: its requested value is harvested before the removal pass, and executing it would overwrite
		// the game speed that simulation code reads and playback pins to the recorded one.
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

			// DoList holds up to MAX_EVENTS * 128 events of 111 bytes and playback runs this every
			// frame, usually with nothing to remove, so scan first and only copy when needed.
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

			// Treat local GameSpeed events as replay playback-speed changes.
			for (int i = 0; i < EventClass::DoList.Count; ++i)
			{
				const auto& event = EventClass::DoList[i];
				if (event.Frame == currentFrame && event.Type == EventType::GameSpeed)
				{
					const int requestedFPS = GetReplayFPSFromGameSpeed(
						std::clamp(event.GameSpeed.GameSpeed, 0, MaxGameSpeedIndex));

					ReplayState.PlaybackFPS = requestedFPS;
					ReplayState.PlaybackNextFrameDue = 0.0;
				}
			}

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
			const bool hasGameCRC = capture.HasGameCRC;

			// HasGameSpeed is deliberately not part of this test. A speed change alone does not earn
			// a frame record; LastRecordedGameSpeed is only updated once one is written, so the
			// change rides along with the next frame that has something else to say.
			if (eventsThisFrame == 0 && !tacticalPosChanged && !selectionChanged && !hasSideChannelEvents
				&& !hasGameCRC && !capture.HasCensus)
			{
				return true;
			}

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
			if (hasGameCRC)
				header.Flags |= FrameRecordFlag_GameCRC;
			if (capture.HasCensus)
				header.Flags |= FrameRecordFlag_ObjectCensus;

			if (capture.HasRandomState)
				header.Flags |= FrameRecordFlag_RandomState;
			if (capture.HasGameSpeed)
				header.Flags |= FrameRecordFlag_GameSpeed;

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

				// One call for the whole block: every WriteRaw is a tdefl_compress call, and
				// box-selecting a hundred units would otherwise mean a hundred of them.
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

			if (hasGameCRC && !WriteRaw(&capture.GameCRC, sizeof(capture.GameCRC)))
				return false;

			if (capture.HasCensus && !WriteRaw(&capture.Census, sizeof(capture.Census)))
				return false;

			if (capture.HasRandomState
				&& !WriteRaw(&capture.RandomState, sizeof(capture.RandomState)))
			{
				return false;
			}

			if (capture.HasGameSpeed)
			{
				if (!WriteRaw(&capture.GameSpeed, sizeof(capture.GameSpeed)))
					return false;

				ReplayState.LastRecordedGameSpeed = capture.GameSpeed;
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
					&& sideChannelForFrame.size() < static_cast<size_t>(SideChannelMaxEventsPerFrame))
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

			// The main loop runs faster than the game frame, so the same frame is usually captured
			// several times over. Overwriting the pending entry in place reuses its buffer.
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

			// A single player game's options dialog writes the game speed straight out with no event
			// queued, so the stream is the only place playback can learn about it. The simulation
			// reads the value, so a change playback missed diverges from that frame on.
			capture.GameSpeed = static_cast<int32_t>(GameOptionsClass::Instance.GameSpeed);
			capture.HasGameSpeed = capture.GameSpeed != ReplayState.LastRecordedGameSpeed;
		}

		bool WriteReplayEndOfStreamMarker()
		{
			FrameRecordHeader marker {};
			marker.FrameNumber = -1;
			return WriteRaw(&marker, sizeof(marker));
		}

		// Seeks back and stamps the frame count and the clean-shutdown flag into the header. Only
		// reached once recording has finished cleanly, so a recording that dies with the process
		// leaves both fields zero - the only signal a reader has that the stream is cut short.
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

		// Replays are shared, so records off disk are untrusted: the text arrays need not be
		// terminated, and House and Aux index straight into engine arrays. False means the record
		// cannot be made safe.
		bool SanitizeSideChannelRecord(SideChannelRecord& record)
		{
			record.SenderName[SideChannelNameLength - 1] = L'\0';
			record.Text[SideChannelTextLength - 1] = L'\0';

			switch (static_cast<SideChannelEventType>(record.Type))
			{
			case SideChannelEventType::ChatMessage:
				if (record.House < 0 || record.House >= MaxHouses)
					return false;

				// Indexes ColorScheme::Array in TextLabelClass; fall back to the first scheme.
				if (record.Aux < 0 || record.Aux >= ColorScheme::Array.Count)
					record.Aux = 0;

				return true;

			case SideChannelEventType::BeaconPlace:
				// -1 asks the engine to pick a free slot, which is how a local placement is recorded.
				if (record.House < 0 || record.House >= MaxHouses
					|| record.Aux < -1 || record.Aux >= MaxBeaconSlots)
				{
					return false;
				}

				return record.Coord.X >= 0 && record.Coord.X < MaxMapLeptonCoord
					&& record.Coord.Y >= 0 && record.Coord.Y < MaxMapLeptonCoord;

			case SideChannelEventType::BeaconDelete:
			case SideChannelEventType::BeaconText:
				return record.House >= 0 && record.House < MaxHouses
					&& record.Aux >= 0 && record.Aux < MaxBeaconSlots;

			case SideChannelEventType::Taunt:
				// PlayTaunt range-checks the command itself, but a value that came off disk should
				// not be bounded only by a function we do not own.
				return VoxClass::IsValidTauntCommand(record.Aux);

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

			if ((record.Flags & ~KnownFrameRecordFlags) != 0u)
			{
				Debug::Log("[Replay] Frame %d carries unknown block flags (0x%08X); "
					"the replay was recorded in a newer format than this build can read.\n",
					record.FrameNumber, record.Flags & ~KnownFrameRecordFlags);
				return false;
			}

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

				if (sideChannelCount < 0 || sideChannelCount > SideChannelMaxEventsPerFrame)
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

			if ((record.Flags & FrameRecordFlag_GameCRC) != 0u
				&& !ReadRaw(&record.GameCRC, sizeof(record.GameCRC)))
			{
				return false;
			}

			if ((record.Flags & FrameRecordFlag_ObjectCensus) != 0u
				&& !ReadRaw(&record.Census, sizeof(record.Census)))
			{
				return false;
			}

			if ((record.Flags & FrameRecordFlag_RandomState) != 0u
				&& !ReadRaw(&record.RandomState, sizeof(record.RandomState)))
			{
				return false;
			}

			if ((record.Flags & FrameRecordFlag_GameSpeed) != 0u
				&& !ReadRaw(&record.GameSpeed, sizeof(record.GameSpeed)))
			{
				return false;
			}

			if ((record.Flags & FrameRecordFlag_Extensions) != 0u)
			{
				uint32_t extensionBytes = 0;
				if (!ReadRaw(&extensionBytes, sizeof(extensionBytes)))
					return false;

				if (extensionBytes > MaxFrameExtensionBytes)
				{
					Debug::Log("[Replay] Frame %d declares a %u byte extension block; refusing it.\n",
						record.FrameNumber, extensionBytes);
					return false;
				}

				if (!SkipRaw(extensionBytes))
					return false;
			}

			return true;
		}

		// Restarts the decompressor at the top of the frame stream and walks forward to the first
		// record at or after the target. Deflate has no random access and an index of decodable
		// points would have to live in the file, but the frame stream is small and inflating it is
		// far cheaper than the simulation a seek is about to run anyway.
		bool RepositionPlaybackStreamToFrame(int targetFrame)
		{
			if (ReplayState.ReplayFile == INVALID_HANDLE_VALUE)
				return false;

			LARGE_INTEGER streamStart {};
			streamStart.QuadPart = static_cast<LONGLONG>(ReplayState.PlaybackStreamOffset);

			if (!SetFilePointerEx(ReplayState.ReplayFile, streamStart, nullptr, FILE_BEGIN)
				|| !ReplayState.Reader.Start(ReplayState.ReplayFile))
			{
				Debug::Log("[Replay] Failed to rewind the replay stream while seeking.\n");
				return false;
			}

			ReplayState.HasPendingPlaybackFrame = false;
			ReplayState.PlaybackStreamEnded = false;
			ReplayState.PreparedPlaybackFrame = -1;
			ReplayState.PendingPlaybackFrame = {};
			ReplayState.ExpectedEventsThisFrame = 0;
			ReplayState.HasExpectedGameCRC = false;
			ReplayState.HasExpectedCensus = false;

			for (;;)
			{
				PlaybackFrameRecord record {};
				if (!ReadNextPlaybackFrameRecord(record))
				{
					Debug::Log("[Replay] Failed to read the replay stream while seeking to frame %d.\n",
						targetFrame);
					return false;
				}

				if (record.EndOfStream)
				{
					ReplayState.PlaybackStreamEnded = true;
					return true;
				}

				if (record.FrameNumber >= targetFrame)
				{
					ReplayState.PendingPlaybackFrame = std::move(record);
					ReplayState.HasPendingPlaybackFrame = true;
					return true;
				}

				// Records are sparse and the state they carry is sticky, so the ones stepped over still
				// have to be applied - otherwise the seek lands with the viewport and the simulation speed
				// left at whatever the last record before the rewind happened to say.
				if ((record.Flags & FrameRecordFlag_TacticalPos) != 0u)
				{
					ReplayState.LockedViewportPos = record.TacticalPos;
					ReplayState.HasLockedViewportPos = true;
				}

				if ((record.Flags & FrameRecordFlag_GameSpeed) != 0u)
				{
					const int gameSpeed = std::clamp(static_cast<int>(record.GameSpeed), 0, MaxGameSpeedIndex);
					GameOptionsClass::Instance.GameSpeed = gameSpeed;
					GameModeOptionsClass::Instance.GameSpeed = gameSpeed;
				}

				// The frame's events sit in the stream immediately after its record.
				if (!SkipRaw(static_cast<size_t>(record.EventCountThisFrame) * sizeof(EventClass)))
				{
					Debug::Log("[Replay] Failed to step over frame %d's events while seeking.\n",
						record.FrameNumber);
					return false;
				}
			}
		}

		void ApplyPlaybackSelection(const PlaybackFrameRecord& frameRecord)
		{
			if (!ReplayState.SelectUnits)
				return;

			const int maxSelectionCount = std::max(AbstractClass::Array.Count, 0);
			if (frameRecord.SelectedObjectCount < 0 || frameRecord.SelectedObjectCount > maxSelectionCount)
			{
				Debug::Log("[Replay] Skipping a recorded selection of %d objects; %d objects exist.\n",
					frameRecord.SelectedObjectCount, maxSelectionCount);
				return;
			}

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

		int GetChatMessageDurationFrames()
		{
			constexpr int TicksPerMinute = 900;
			constexpr double DefaultMessageDelay = 0.6; // Rules' own default, if Rules is not loaded.

			const double messageDelay = RulesClass::Instance
				? RulesClass::Instance->MessageDelay
				: DefaultMessageDelay;

			return static_cast<int>(messageDelay * TicksPerMinute);
		}

		// Replays recorded chat, beacons and taunts without using the network.
		void ApplySideChannelEvent(const SideChannelRecord& record)
		{
			if (!ReplayState.ShowChatAndBeacons)
				return;

			// A seek runs through hundreds of frames in a second or two. Chat and taunts are
			// momentary, and replaying them all would land the lot on screen at once with their
			// lifetimes starting from the seek rather than from when they were said. Beacons are
			// state that outlives the frame that placed them, so those are applied either way.
			const auto eventType = static_cast<SideChannelEventType>(record.Type);
			if (ReplaySystem::Seek::IsSeeking()
				&& (eventType == SideChannelEventType::ChatMessage
					|| eventType == SideChannelEventType::Taunt))
			{
				return;
			}

			switch (eventType)
			{
			case SideChannelEventType::ChatMessage:
				MessageListClass::Instance.AddMessage(
					record.SenderName, record.House, record.Text, record.Aux,
					TextPrintType::UseGradPal | TextPrintType::FullShadow | TextPrintType::Point6Grad,
					GetChatMessageDurationFrames(), false);
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
				BeaconManagerClass::Instance.EditBeaconMessage(record.Text, record.House, record.Aux, true);
				break;

			case SideChannelEventType::Taunt:
				VoxClass::PlayTaunt(record.Aux);
				break;

			default:
				break;
			}
		}

		constexpr int DivergenceMessageDurationFrames = 1800;
		constexpr const wchar_t* DivergenceMessage = L"Replay playback has diverged from the recording.";
		constexpr int MaxLoggedDivergences = 10;
		constexpr int MaxLoggedDivergenceRecoveries = 3;

		// Taken at the same point on both sides - after Compute_Game_CRC has made its own draw -
		// so the two are directly comparable.
		// Roughly eight megabytes of call sites. Enough for tens of thousands of frames of a normal
		// skirmish, and a hard stop so a long watch cannot grow without bound.
		constexpr size_t MaxTracedRandomDraws = 2000000;

		FrameRandomState CurrentRandomState()
		{
			FrameRandomState state {};
			if (ScenarioClass::Instance)
			{
				state.Next1 = ScenarioClass::Instance->Random.Next1;
				state.Next2 = ScenarioClass::Instance->Random.Next2;
			}

			return state;
		}

		FrameObjectCensus CurrentObjectCensus()
		{
			FrameObjectCensus census {};
			census.AbstractCount = AbstractClass::Array.Count;
			census.ScenarioUniqueID = ScenarioClass::Instance ? ScenarioClass::Instance->UniqueID : 0;
			return census;
		}

		// Reports the first frame on which playback is not holding the same set of objects the
		// recording held, then stays quiet: after the first mismatch the two runs are different games
		// and every frame after would say so again. Sharper than the frame hash, which can stay clean
		// for thousands of frames after a divergence has happened.
		void CheckObjectCensusForCurrentFrame()
		{
			if (!ReplayState.Playback || !ReplayState.HasExpectedCensus)
				return;

			ReplayState.HasExpectedCensus = false;

			const FrameObjectCensus census = CurrentObjectCensus();
			const FrameObjectCensus& expected = ReplayState.ExpectedCensus;
			if (census.AbstractCount == expected.AbstractCount
				&& census.ScenarioUniqueID == expected.ScenarioUniqueID)
			{
				return;
			}

			if (ReplayState.CensusMismatchReported)
				return;

			ReplayState.CensusMismatchReported = true;
			Debug::Log("[Replay] Frame %d holds a different set of objects than the recording did "
				"(objects %d, recorded %d; next unique ID %d, recorded %d). Something created or destroyed "
				"an object on one side only - playback will diverge from here.\n",
				Unsorted::CurrentFrame,
				census.AbstractCount, expected.AbstractCount,
				census.ScenarioUniqueID, expected.ScenarioUniqueID);
		}

		// See ReplaySeek.cpp. The traces below are hooked into the randomiser and into every mission
		// change, so they are asked on paths the simulation runs constantly.
		bool DiagnosticsWanted()
		{
			const auto* const pConfig = GetConfig();
			return pConfig && pConfig->ReplayDiagnostics;
		}

		#pragma region Randomiser draw trace

		// Seeking backwards replays frames that have already been played once, and the first pass is a
		// known-good reference: it is the run that matched the recording. So every draw from the
		// scenario randomiser is remembered by frame - the address that asked for it, and where the
		// randomiser stood when it did - and when a frame comes round a second time the sequence is
		// checked against what it was the first time.
		//
		// The first line that differs names the code that went a different way after the keyframe load,
		// which is the code whose state the load failed to carry across. No setting to turn on and
		// nothing to diff by hand: watch a replay, seek back over what you watched, read the log.
		//
		// The cursor is kept as well as the caller because the ranged draw (0x65C7E0) rejects and
		// redraws until the value fits the range, so one call can advance the table any number of
		// times. Two runs can reach the same call site in the same order and still part company on
		// how far they moved the table; comparing the cursor catches that as well.
		uint32_t UniqueIDOfAbstract(const AbstractClass* pAbstract)
		{
			return pAbstract ? static_cast<uint32_t>(pAbstract->UniqueID) : 0u;
		}

		struct RandomDrawSite
		{
			uint32_t Caller;
			int32_t Cursor;
			// Which object was asking, where the call site bothers to say. Tells a same-object-at-a
			// -different-time difference from a different-object-entirely one, which need looking at in
			// completely different places.
			uint32_t Context;
		};

		// What the object asking for a draw looked like at the moment it asked. This was the object
		// pointer once, read back later when a mismatch was reported, and that was wrong twice over.
		// A keyframe load destroys and rebuilds every object in the world, so a pointer set before the
		// load is dangling after it - which is exactly when a mismatch is reported, and the report
		// crashed reading through it. The attribution was wrong as well: the pointer outlived the draw
		// that named it, so a mismatch on a later unnamed draw would name whichever object had last
		// happened to set one. Copying the fields out at the call site fixes both - nothing is read
		// through afterwards, and the snapshot travels with the draw it belongs to.
		struct RandomDrawAsker
		{
			bool Valid;
			uint32_t Object;
			int32_t Mission;
			uint32_t Target;
			int32_t TimerStart;
			int32_t TimerLeft;
		};

		// Set just before a call that is about to draw, by a hook on a function whose object is
		// worth naming. Consumed by the next draw, so it can never attach itself to an unrelated one.
		uint32_t PendingRandomDrawContext = 0;
		RandomDrawAsker PendingRandomDrawAsker {};
		// The asker of the draw being compared right now, moved across from the pending one.
		RandomDrawAsker CurrentRandomDrawAsker {};

		// Every draw the simulation makes this frame, so an object update can be measured by how
		// many of them it consumed. See the object update order trace.
		unsigned int DrawsThisFrame = 0;

		std::unordered_map<int, std::vector<RandomDrawSite>> RandomDrawsByFrame;
		size_t TracedRandomDrawCount = 0;
		int TracedRandomFrame = -1;
		// Where this frame's draws are appended, on a frame being seen for the first time.
		std::vector<RandomDrawSite>* TracedRandomTarget = nullptr;
		// What this frame drew the first time round, on a frame being replayed.
		const std::vector<RandomDrawSite>* TracedRandomReference = nullptr;
		size_t TracedRandomCompareIndex = 0;
		bool TracedRandomMismatchReported = false;

		void ResetRandomDrawTrace()
		{
			RandomDrawsByFrame.clear();
			TracedRandomDrawCount = 0;
			TracedRandomFrame = -1;
			TracedRandomTarget = nullptr;
			TracedRandomReference = nullptr;
			TracedRandomCompareIndex = 0;
			TracedRandomMismatchReported = false;
			PendingRandomDrawContext = 0;
			PendingRandomDrawAsker = RandomDrawAsker {};
			CurrentRandomDrawAsker = RandomDrawAsker {};
		}

		// A caller address is only worth printing if it can be looked up afterwards. gamemd is fixed
		// at its preferred base, but Ares and Phobos are relocatable, and the same call site came
		// back as 71798C1F on one run and 71788C1F on the next - two addresses for one place, and no
		// way to resolve either without the base they were relative to. So anything outside the
		// executable is named module and offset, which is what a disassembler wants.
		const char* DescribeCodeAddress(uint32_t address, char* buffer, size_t size)
		{
			HMODULE hModule = nullptr;
			if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
				| GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(address), &hModule) || !hModule)
			{
				sprintf_s(buffer, size, "%08X", address);
				return buffer;
			}

			char path[MAX_PATH] = { 0 };
			if (!GetModuleFileNameA(hModule, path, sizeof(path)))
			{
				sprintf_s(buffer, size, "%08X", address);
				return buffer;
			}

			const char* pName = strrchr(path, '\\');
			pName = pName ? pName + 1 : path;

			const auto base = reinterpret_cast<uint32_t>(hModule);
			sprintf_s(buffer, size, "%s+0x%X", pName, address - base);
			return buffer;
		}

		void ReportRandomDrawMismatch(const char* what, int frame, size_t drawIndex,
			const RandomDrawSite& before, const RandomDrawSite& now, const RandomDrawAsker& asker)
		{
			if (TracedRandomMismatchReported)
				return;

			TracedRandomMismatchReported = true;
			char beforeCaller[MAX_PATH + 32] = { 0 };
			char nowCaller[MAX_PATH + 32] = { 0 };

			Debug::Log("[Replay] Frame %d used the randomiser differently the second time round: %s at "
				"draw %u. First pass: caller %s, cursor %d, object %u. After the keyframe load: "
				"caller %s, cursor %d, object %u.\n",
				frame, what, static_cast<unsigned int>(drawIndex + 1),
				DescribeCodeAddress(before.Caller, beforeCaller, sizeof(beforeCaller)),
				before.Cursor, before.Context,
				DescribeCodeAddress(now.Caller, nowCaller, sizeof(nowCaller)),
				now.Cursor, now.Context);

			// The gate on this call is Frame - TargetingTimer.StartTime >= TimeLeft, so those two
			// numbers say whether the object was scheduled to look for a target on this frame at all.
			if (asker.Valid)
			{
				Debug::Log("[Replay] The object asking was %u: mission %d, target %u, targeting timer "
					"started frame %d with %d to run, on frame %d.\n",
					asker.Object, asker.Mission, asker.Target, asker.TimerStart, asker.TimerLeft,
					static_cast<int>(Unsorted::CurrentFrame));
			}
		}

		// A frame boundary: decide whether this frame is being seen for the first time or replayed,
		// and close out the frame that just ended.
		void BeginRandomDrawTraceFrame(int frame)
		{
			// A replayed frame that stopped short drew fewer times than it did before, which is just as
			// much a difference as drawing from somewhere else.
			if (TracedRandomReference && TracedRandomCompareIndex < TracedRandomReference->size())
			{
				ReportRandomDrawMismatch("it stopped drawing early", TracedRandomFrame,
					TracedRandomCompareIndex, (*TracedRandomReference)[TracedRandomCompareIndex],
					RandomDrawSite { 0u, 0 }, RandomDrawAsker {});
			}

			TracedRandomFrame = frame;
			TracedRandomTarget = nullptr;
			TracedRandomReference = nullptr;
			TracedRandomCompareIndex = 0;

			const auto it = RandomDrawsByFrame.find(frame);
			if (it != RandomDrawsByFrame.end())
			{
				TracedRandomReference = &it->second;
				return;
			}

			if (TracedRandomDrawCount < MaxTracedRandomDraws
				&& ChargeDiagnosticMemory(sizeof(std::vector<RandomDrawSite>) + 2 * sizeof(RandomDrawSite)))
				TracedRandomTarget = &RandomDrawsByFrame[frame];
		}

		void SetRandomDrawContext(const TechnoClass* pTechno)
		{
			if (!DiagnosticsWanted())
				return;

			PendingRandomDrawContext = UniqueIDOfAbstract(pTechno);
			PendingRandomDrawAsker = pTechno
				? RandomDrawAsker { true, PendingRandomDrawContext,
					static_cast<int32_t>(pTechno->CurrentMission), UniqueIDOfAbstract(pTechno->Target),
					pTechno->TargetingTimer.StartTime, pTechno->TargetingTimer.TimeLeft }
				: RandomDrawAsker {};
		}

		void TraceRandomDraw(const void* randomiser, const void* caller)
		{
			if (!DiagnosticsWanted())
				return;

			if (ScenarioClass::Instance && randomiser == &ScenarioClass::Instance->Random)
				++DrawsThisFrame;

			// Only the scenario's own randomiser drives the simulation. The map generator and the shell
			// menus have their own, and draws from those mean nothing here.
			if (!ReplayState.Playback || !ScenarioClass::Instance
				|| randomiser != &ScenarioClass::Instance->Random)
			{
				return;
			}

			// A keyframe load draws from the randomiser itself - every object it constructs does, through
			// TechnoClass::TechnoClass and its like - and ScenarioClass::Load restores the frame counter
			// before any of that runs, so those draws land on the frame being loaded. They are wiped a
			// moment later when the keyframe restores the randomiser, so they are not part of the
			// simulation and comparing them against the first pass only ever reports the load itself.
			if (Seek::IsLoadInProgress())
				return;

			const int frame = static_cast<int>(Unsorted::CurrentFrame);
			if (frame != TracedRandomFrame)
				BeginRandomDrawTraceFrame(frame);

			const RandomDrawSite site {
				reinterpret_cast<uint32_t>(caller),
				ScenarioClass::Instance->Random.Next1,
				PendingRandomDrawContext
			};

			PendingRandomDrawContext = 0;
			CurrentRandomDrawAsker = PendingRandomDrawAsker;
			PendingRandomDrawAsker = RandomDrawAsker {};

			if (TracedRandomReference)
			{
				if (TracedRandomCompareIndex >= TracedRandomReference->size())
				{
					ReportRandomDrawMismatch("it drew more times than before", frame,
						TracedRandomCompareIndex, RandomDrawSite { 0u, 0 }, site,
						CurrentRandomDrawAsker);
				}
				else
				{
					const RandomDrawSite& before = (*TracedRandomReference)[TracedRandomCompareIndex];
					if (before.Caller != site.Caller)
					{
						ReportRandomDrawMismatch("it drew from somewhere else", frame,
							TracedRandomCompareIndex, before, site, CurrentRandomDrawAsker);
					}
					else if (before.Cursor != site.Cursor)
					{
						ReportRandomDrawMismatch("the same caller found the randomiser elsewhere", frame,
							TracedRandomCompareIndex, before, site, CurrentRandomDrawAsker);
					}
				}

				++TracedRandomCompareIndex;
				return;
			}

			if (TracedRandomTarget && ChargeDiagnosticMemory(2 * sizeof(RandomDrawSite)))
			{
				TracedRandomTarget->push_back(site);
				++TracedRandomDrawCount;
			}
		}

		#pragma endregion Randomiser draw trace

		#pragma region Mission assignment trace

		// The same idea as the draw trace, for the thing the draw trace cannot see. A mission change
		// is how one object's behaviour turns into another's, and it happens on paths that consume no
		// randomness at all - so a run that assigns a mission the first time round and not the second
		// looks identical to every check that watches the randomiser.
		//
		// Every assignment is recorded by frame with the object, the mission and the code that asked.
		// A frame replayed after a seek is checked against what it did the first time, and the first
		// difference names the caller to go and read.
		struct MissionAssignment
		{
			uint32_t Object;
			int32_t Mission;
			uint32_t Caller;
		};

		std::unordered_map<int, std::vector<MissionAssignment>> MissionsByFrame;
		size_t TracedMissionCount = 0;
		int TracedMissionFrame = -1;
		std::vector<MissionAssignment>* TracedMissionTarget = nullptr;
		const std::vector<MissionAssignment>* TracedMissionReference = nullptr;
		size_t TracedMissionCompareIndex = 0;
		bool TracedMissionMismatchReported = false;

		constexpr size_t MaxTracedMissions = 400000;

		void ResetMissionTrace()
		{
			MissionsByFrame.clear();
			TracedMissionCount = 0;
			TracedMissionFrame = -1;
			TracedMissionTarget = nullptr;
			TracedMissionReference = nullptr;
			TracedMissionCompareIndex = 0;
			TracedMissionMismatchReported = false;
		}

		void ReportMissionMismatch(const char* what, int frame, size_t index,
			const MissionAssignment& before, const MissionAssignment& now)
		{
			if (TracedMissionMismatchReported)
				return;

			TracedMissionMismatchReported = true;
			Debug::Log("[Replay] Frame %d assigned missions differently the second time round: %s at "
				"assignment %u. First pass: object %u given mission %d by %08X. After the keyframe "
				"load: object %u given mission %d by %08X.\n",
				frame, what, static_cast<unsigned int>(index + 1),
				before.Object, before.Mission, before.Caller,
				now.Object, now.Mission, now.Caller);
		}

		void BeginMissionTraceFrame(int frame)
		{
			if (TracedMissionReference && TracedMissionCompareIndex < TracedMissionReference->size())
			{
				ReportMissionMismatch("it stopped assigning early", TracedMissionFrame,
					TracedMissionCompareIndex, (*TracedMissionReference)[TracedMissionCompareIndex],
					MissionAssignment { 0u, 0, 0u });
			}

			TracedMissionFrame = frame;
			TracedMissionTarget = nullptr;
			TracedMissionReference = nullptr;
			TracedMissionCompareIndex = 0;

			const auto it = MissionsByFrame.find(frame);
			if (it != MissionsByFrame.end())
			{
				TracedMissionReference = &it->second;
				return;
			}

			if (TracedMissionCount < MaxTracedMissions
				&& ChargeDiagnosticMemory(sizeof(std::vector<MissionAssignment>) + 2 * sizeof(MissionAssignment)))
				TracedMissionTarget = &MissionsByFrame[frame];
		}

		void TraceMissionAssignment(const void* object, int mission, const void* caller)
		{
			if (!DiagnosticsWanted() || !ReplayState.Playback || Seek::IsLoadInProgress())
				return;

			const int frame = static_cast<int>(Unsorted::CurrentFrame);
			if (frame != TracedMissionFrame)
				BeginMissionTraceFrame(frame);

			const MissionAssignment entry {
				UniqueIDOfAbstract(static_cast<const AbstractClass*>(object)),
				static_cast<int32_t>(mission),
				reinterpret_cast<uint32_t>(caller)
			};

			if (TracedMissionReference)
			{
				if (TracedMissionCompareIndex >= TracedMissionReference->size())
				{
					ReportMissionMismatch("it assigned more than before", frame, TracedMissionCompareIndex,
						MissionAssignment { 0u, 0, 0u }, entry);
				}
				else
				{
					const MissionAssignment& before = (*TracedMissionReference)[TracedMissionCompareIndex];
					if (before.Object != entry.Object || before.Mission != entry.Mission
						|| before.Caller != entry.Caller)
					{
						ReportMissionMismatch("a different assignment", frame, TracedMissionCompareIndex,
							before, entry);
					}
				}

				++TracedMissionCompareIndex;
				return;
			}

			if (TracedMissionTarget)
			{
				TracedMissionTarget->push_back(entry);
				++TracedMissionCount;
			}
		}

		#pragma endregion Mission assignment trace

		#pragma region Pathfinder request trace

		// Everything the pathfinder is asked about now checks out across a load - the cells, the map's own
		// copy of their passability and zone, and the thirteen movement zone tables the zone lookup ends
		// in - and so does every field of the unit doing the asking. What is left is the possibility that
		// two runs ask different questions, or ask at different moments.
		//
		// So the questions are recorded. FootClass::Basic_Path (0x4D3920) is the one entry point every
		// route goes through; each call is written down by frame with who asked and what they asked for,
		// and a frame replayed after a seek is checked against what it was the first time. Either the
		// calls match - in which case the pathfinder returned two different answers to the same question,
		// and the difference is inside Find_Path - or they do not, and the report names the frame where
		// the asking itself changed.
		struct PathRequest
		{
			uint32_t Object;
			int32_t DestX;
			int32_t DestY;
			int32_t PathOffset;
			int32_t Avoidance;
			bool operator==(const PathRequest&) const = default;
		};

		std::unordered_map<int, std::vector<PathRequest>> PathRequestsByFrame;
		size_t TracedPathRequestCount = 0;
		int TracedPathFrame = -1;
		std::vector<PathRequest>* TracedPathTarget = nullptr;
		const std::vector<PathRequest>* TracedPathReference = nullptr;
		size_t TracedPathCompareIndex = 0;
		bool TracedPathMismatchReported = false;

		constexpr size_t MaxTracedPathRequests = 400000;

		void ResetPathRequestTrace()
		{
			PathRequestsByFrame.clear();
			TracedPathRequestCount = 0;
			TracedPathFrame = -1;
			TracedPathTarget = nullptr;
			TracedPathReference = nullptr;
			TracedPathCompareIndex = 0;
			TracedPathMismatchReported = false;
		}

		void ReportPathRequestMismatch(const char* what, int frame, size_t index,
			const PathRequest& before, const PathRequest& now)
		{
			if (TracedPathMismatchReported)
				return;

			TracedPathMismatchReported = true;
			Debug::Log("[Replay] Frame %d asked the pathfinder differently the second time round: %s at "
				"request %u. First pass: object %u wanted cell %d,%d from step %d avoiding %d. After the "
				"keyframe load: object %u wanted cell %d,%d from step %d avoiding %d.\n",
				frame, what, static_cast<unsigned int>(index + 1),
				before.Object, before.DestX, before.DestY, before.PathOffset, before.Avoidance,
				now.Object, now.DestX, now.DestY, now.PathOffset, now.Avoidance);
		}

		void BeginPathRequestFrame(int frame)
		{
			if (TracedPathReference && TracedPathCompareIndex < TracedPathReference->size())
			{
				ReportPathRequestMismatch("it stopped asking early", TracedPathFrame,
					TracedPathCompareIndex, (*TracedPathReference)[TracedPathCompareIndex], PathRequest {});
			}

			TracedPathFrame = frame;
			TracedPathTarget = nullptr;
			TracedPathReference = nullptr;
			TracedPathCompareIndex = 0;

			const auto it = PathRequestsByFrame.find(frame);
			if (it != PathRequestsByFrame.end())
			{
				TracedPathReference = &it->second;
				return;
			}

			if (TracedPathRequestCount < MaxTracedPathRequests
				&& ChargeDiagnosticMemory(sizeof(std::vector<PathRequest>) + 2 * sizeof(PathRequest)))
				TracedPathTarget = &PathRequestsByFrame[frame];
		}

		void TracePathRequest(const void* object, int cell, int pathOffset, int avoidance)
		{
			if (!DiagnosticsWanted() || !ReplayState.Playback || Seek::IsLoadInProgress())
				return;

			const int frame = static_cast<int>(Unsorted::CurrentFrame);
			if (frame != TracedPathFrame)
				BeginPathRequestFrame(frame);

			const PathRequest entry {
				UniqueIDOfAbstract(static_cast<const AbstractClass*>(object)),
				static_cast<int16_t>(cell & 0xFFFF),
				static_cast<int16_t>((cell >> 16) & 0xFFFF),
				pathOffset,
				avoidance
			};

			if (TracedPathReference)
			{
				if (TracedPathCompareIndex >= TracedPathReference->size())
				{
					ReportPathRequestMismatch("it asked more than before", frame, TracedPathCompareIndex,
						PathRequest {}, entry);
				}
				else
				{
					const PathRequest& before = (*TracedPathReference)[TracedPathCompareIndex];
					if (!(before == entry))
						ReportPathRequestMismatch("a different question", frame, TracedPathCompareIndex,
							before, entry);
				}

				++TracedPathCompareIndex;
				return;
			}

			if (TracedPathTarget)
			{
				TracedPathTarget->push_back(entry);
				++TracedPathRequestCount;
			}
		}

		#pragma endregion Pathfinder request trace

		#pragma region Aircraft landing zone trace

		// AircraftClass::New_LZ (0x418E20) is where the frame-7508 divergence first shows: the recording
		// drew a random number there and the run after the keyframe load did not. The draw is buried five
		// conditions deep, and only the last two of them are expensive to ask about:
		//
		//     if (oldlz
		//         && (!Team || !TeamClass::Is_Leaving_Map(Team))
		//         && (!Is_LZ_Clear(oldlz) || !Cell_Seems_Ok(oldlz->Destination_Coord()))
		//         && !ScenarioInit
		//         && Rule->LZScanRadius / 256 > 0)
		//     { ... Random2Class::operator()(&Scen->RandomNumber, 0, 7) ... }
		//
		// The randomiser trace can only see the draw, so a run that never reached it and a run that
		// reached it and turned back look the same. This records the entry instead, with the three inputs
		// that are free to read - which aircraft, which zone, and what its team is doing - so the next
		// report says whether the two runs disagreed before the landing zone was even examined, or only
		// about whether it was clear.
		struct LandingZoneRequest
		{
			// A ladder of four points along the path to the landing zone scan, so the first one that
			// differs names the condition that changed rather than only its consequence:
			//
			//   1  FlyLocomotionClass::Nearing_Target entered, with the coord it was given
			//   2  the destination cell handed to CellClass::Cell_Building
			//   3  RadioClass::Has_Contact_Index asked, so a building was found and the aircraft is
			//      not a hunter-seeker
			//   0  AircraftClass::New_LZ entered, which is where the six random draws happen
			int32_t Site;
			uint32_t Aircraft;
			// Whatever the site is about besides the aircraft - the building, at site 3.
			uint32_t Other;
			// Who called, at site 0. New_LZ has two call sites and they mean different things:
			// Nearing_Target at 0x4CF0FB and Stop_Moving at 0x4CD117.
			uint32_t Caller;
			// The first frame above Caller still inside the executable. When a hook in a mod DLL is
			// the caller, this is the engine function it was hooked onto, which names the decision
			// context without that DLL's symbols or a source tree that matches the shipped build.
			uint32_t EngineCaller;
			uint32_t Team;
			int32_t ZoneX;
			int32_t ZoneY;
			int32_t LeavingMap;
			// Inputs to the off-map removal branch in AircraftClass::AI. They are -1 at
			// every other trace site. Site 9 is the MapClass::In_Radar call at 0x414FB6.
			int32_t InRadar;
			int32_t IsInPlayfield;
			int32_t IsALoaner;
			int32_t Mission;
			uint32_t Target;
			int32_t MapWidth;
			int32_t MapHeight;

			// EngineCaller is deliberately left out. It is worked out by scanning the stack for the
			// first executable address above the caller, which is a guess: a stale value left in an
			// unused slot reads exactly like a live return address. Defaulting the comparison swept
			// it in, and two runs that agreed about everything real were reported as differing
			// because the scan landed on different rubbish - which masked the divergence being
			// chased. It is context for reading a report, never evidence of one.
			bool operator==(const LandingZoneRequest& other) const
			{
				return Site == other.Site && Aircraft == other.Aircraft && Other == other.Other
					&& Team == other.Team && ZoneX == other.ZoneX && ZoneY == other.ZoneY
					&& LeavingMap == other.LeavingMap && Caller == other.Caller
					&& InRadar == other.InRadar && IsInPlayfield == other.IsInPlayfield
					&& IsALoaner == other.IsALoaner && Mission == other.Mission
					&& Target == other.Target && MapWidth == other.MapWidth
					&& MapHeight == other.MapHeight;
			}
		};

		std::unordered_map<int, std::vector<LandingZoneRequest>> LandingZonesByFrame;
		size_t TracedLandingZoneCount = 0;
		int TracedLandingZoneFrame = -1;
		std::vector<LandingZoneRequest>* TracedLandingZoneTarget = nullptr;
		const std::vector<LandingZoneRequest>* TracedLandingZoneReference = nullptr;
		size_t TracedLandingZoneCompareIndex = 0;
		bool TracedLandingZoneMismatchReported = false;

		constexpr size_t MaxTracedLandingZones = 200000;

		void ResetLandingZoneTrace()
		{
			LandingZonesByFrame.clear();
			TracedLandingZoneCount = 0;
			TracedLandingZoneFrame = -1;
			TracedLandingZoneTarget = nullptr;
			TracedLandingZoneReference = nullptr;
			TracedLandingZoneCompareIndex = 0;
			TracedLandingZoneMismatchReported = false;
		}

		void ReportLandingZoneMismatch(const char* what, int frame, size_t index,
			const LandingZoneRequest& before, const LandingZoneRequest& now)
		{
			if (TracedLandingZoneMismatchReported)
				return;

			TracedLandingZoneMismatchReported = true;

			char beforeCaller[MAX_PATH + 32] = { 0 };
			char nowCaller[MAX_PATH + 32] = { 0 };
			Debug::Log("[Replay] Frame %d looked for a landing zone differently the second time round: %s "
				"at request %u. First pass: site %d, aircraft %u, other %u, cell %d,%d, called from "
				"%s. After the keyframe load: site %d, aircraft %u, other %u, cell %d,%d, called "
				"from %s.\n", frame, what, static_cast<unsigned int>(index + 1),
				before.Site, before.Aircraft, before.Other, before.ZoneX, before.ZoneY,
				DescribeCodeAddress(before.Caller, beforeCaller, sizeof(beforeCaller)),
				now.Site, now.Aircraft, now.Other, now.ZoneX, now.ZoneY,
				DescribeCodeAddress(now.Caller, nowCaller, sizeof(nowCaller)));

			if (before.EngineCaller || now.EngineCaller)
			{
				char beforeEngine[MAX_PATH + 32] = { 0 };
				char nowEngine[MAX_PATH + 32] = { 0 };
				Debug::Log("[Replay]   entered from %s the first time round, %s the second.\n",
					DescribeCodeAddress(before.EngineCaller, beforeEngine, sizeof(beforeEngine)),
					DescribeCodeAddress(now.EngineCaller, nowEngine, sizeof(nowEngine)));
			}

			if (before.Site == 9 || now.Site == 9)
			{
				Debug::Log("[Replay]   off-map inputs: radar %d, in-playfield %d, loaner %d, mission %d, "
					"target %u, team %u/leaving %d, map %dx%d; were %d, %d, %d, %d, %u, "
					"%u/%d, %dx%d.\n",
					now.InRadar, now.IsInPlayfield, now.IsALoaner, now.Mission, now.Target,
					now.Team, now.LeavingMap, now.MapWidth, now.MapHeight,
					before.InRadar, before.IsInPlayfield, before.IsALoaner, before.Mission,
					before.Target, before.Team, before.LeavingMap, before.MapWidth,
					before.MapHeight);
			}
		}

		void BeginLandingZoneFrame(int frame)
		{
			if (TracedLandingZoneReference
				&& TracedLandingZoneCompareIndex < TracedLandingZoneReference->size())
			{
				ReportLandingZoneMismatch("it stopped looking early", TracedLandingZoneFrame,
					TracedLandingZoneCompareIndex,
					(*TracedLandingZoneReference)[TracedLandingZoneCompareIndex], LandingZoneRequest {});
			}

			TracedLandingZoneFrame = frame;
			TracedLandingZoneTarget = nullptr;
			TracedLandingZoneReference = nullptr;
			TracedLandingZoneCompareIndex = 0;

			const auto it = LandingZonesByFrame.find(frame);
			if (it != LandingZonesByFrame.end())
			{
				TracedLandingZoneReference = &it->second;
				return;
			}

			if (TracedLandingZoneCount < MaxTracedLandingZones
				&& ChargeDiagnosticMemory(sizeof(std::vector<LandingZoneRequest>) + 2 * sizeof(LandingZoneRequest)))
				TracedLandingZoneTarget = &LandingZonesByFrame[frame];
		}

		void TraceLandingZoneCell(int site, const TechnoClass* pAircraft, int cellX, int cellY,
			const AbstractClass* pOther, const void* caller, const void* engineCaller,
			int inRadar, int isInPlayfield, int isALoaner, int mission,
			const AbstractClass* pTarget, int mapWidth, int mapHeight)
		{
			if (!DiagnosticsWanted() || !ReplayState.Playback || Seek::IsLoadInProgress())
				return;

			const int frame = static_cast<int>(Unsorted::CurrentFrame);
			if (frame != TracedLandingZoneFrame)
				BeginLandingZoneFrame(frame);

			const auto* const pFoot = abstract_cast<const FootClass*>(pAircraft);
			const TeamClass* const pTeam = pFoot ? pFoot->Team : nullptr;

			const LandingZoneRequest entry {
				site,
				UniqueIDOfAbstract(pAircraft),
				UniqueIDOfAbstract(pOther),
				reinterpret_cast<uint32_t>(caller),
				reinterpret_cast<uint32_t>(engineCaller),
				UniqueIDOfAbstract(pTeam),
				cellX,
				cellY,
				pTeam ? (pTeam->IsLeavingMap ? 1 : 0) : -1,
				inRadar,
				isInPlayfield,
				isALoaner,
				mission,
				UniqueIDOfAbstract(pTarget),
				mapWidth,
				mapHeight
			};

			if (TracedLandingZoneReference)
			{
				if (TracedLandingZoneCompareIndex >= TracedLandingZoneReference->size())
				{
					ReportLandingZoneMismatch("it looked more than before", frame,
						TracedLandingZoneCompareIndex, LandingZoneRequest {}, entry);
				}
				else
				{
					const LandingZoneRequest& before =
						(*TracedLandingZoneReference)[TracedLandingZoneCompareIndex];
					if (!(before == entry))
					{
						ReportLandingZoneMismatch("a different aircraft or zone", frame,
							TracedLandingZoneCompareIndex, before, entry);
					}
				}

				++TracedLandingZoneCompareIndex;
				return;
			}

			if (TracedLandingZoneTarget)
			{
				TracedLandingZoneTarget->push_back(entry);
				++TracedLandingZoneCount;
			}
		}

		#pragma endregion Aircraft landing zone trace

		#pragma region Object update order trace

		// LogicClass::AI ends with the loop that runs the simulation:
		//
		//     for (m = 0; m < Logic.Objects.ActiveCount; ++m) {
		//         v50 = Logic.Objects.Vector[m];
		//         v50->AI(v50);                       // 0x55B610
		//     }
		//
		// Every object's update goes through that one call, in that one order, and everything downstream
		// of it - including Ares, whose particle handler is hooked onto ParticleSystemClass::Update -
		// draws from the synchronised randomiser in whatever order this loop hands out.
		//
		// Frame 8251 has the recording drawing from Ares' smoke handler where the run after the load draws
		// from BuildingClass::Repair_AI, and the two runs are back in step by the next frame. That is two
		// objects swapping places, not a missing object or a lost piece of state, and nothing that watches
		// state at the top of a frame can see it: the layer watch compares the logic queue and finds it
		// identical, because the queue is identical - it is what happens while walking it that differs.
		//
		// So the walk itself is recorded: every object the loop touches, in order, every frame. A frame
		// replayed after a seek is checked against the order it ran the first time, and the first
		// disagreement names both objects and where in the frame it happened.
		constexpr size_t MaxTracedUpdateEntries = 6000000;

		// What each object's update consumed from the randomiser, which is the thing the order trace
		// on its own cannot see: frame 8251 runs the same objects in the same order in both passes
		// and still draws differently, so the difference is inside one object's update rather than
		// in which updates ran.
		struct UpdateEntry
		{
			uint32_t Object;
			uint32_t Draws;
			bool operator==(const UpdateEntry&) const = default;
		};

		uint32_t PendingUpdateObject = 0;
		unsigned int PendingUpdateMark = 0;
		bool HavePendingUpdate = false;

		std::unordered_map<int, std::vector<UpdateEntry>> UpdateOrderByFrame;
		size_t TracedUpdateCount = 0;
		int TracedUpdateFrame = -1;
		std::vector<UpdateEntry>* TracedUpdateTarget = nullptr;
		const std::vector<UpdateEntry>* TracedUpdateReference = nullptr;
		size_t TracedUpdateCompareIndex = 0;
		bool TracedUpdateMismatchReported = false;

		void ResetUpdateOrderTrace()
		{
			UpdateOrderByFrame.clear();
			TracedUpdateCount = 0;
			TracedUpdateFrame = -1;
			TracedUpdateTarget = nullptr;
			TracedUpdateReference = nullptr;
			TracedUpdateCompareIndex = 0;
			TracedUpdateMismatchReported = false;
			DrawsThisFrame = 0;
			HavePendingUpdate = false;
		}

		void DescribeUpdatedObject(const char* when, uint32_t id)
		{
			const AbstractClass* pFound = nullptr;
			for (int i = 0; i < AbstractClass::Array.Count && !pFound; ++i)
			{
				if (auto* const pItem = AbstractClass::Array.Items[i])
				{
					if (UniqueIDOfAbstract(pItem) == id)
						pFound = pItem;
				}
			}

			if (!pFound)
			{
				Debug::Log("[Replay]   %s object %u, which is not in the world now.\n", when, id);
				return;
			}

			const auto* const pTechno = abstract_cast<const TechnoClass*>(pFound);
			Debug::Log("[Replay]   %s object %u, abstract type %d (%s).\n", when, id,
				static_cast<int>(pFound->WhatAmI()),
				pTechno && pTechno->get_ID() ? pTechno->get_ID() : "<not a techno>");
		}

		void ReportUpdateOrderMismatch(const char* what, int frame, size_t index,
			const UpdateEntry& before, const UpdateEntry& now)
		{
			if (TracedUpdateMismatchReported)
				return;

			TracedUpdateMismatchReported = true;
			Debug::Log("[Replay] Frame %d ran its objects in a different order the second time round: %s "
				"at position %u of the update loop.\n", frame, what,
				static_cast<unsigned int>(index + 1));

			DescribeUpdatedObject("first pass ran", before.Object);
			DescribeUpdatedObject("after the keyframe load it ran", now.Object);
			Debug::Log("[Replay]   it drew %u random numbers there the first time round, %u the "
				"second.\n", before.Draws, now.Draws);
		}

		// An update is closed out by the next one starting, or by the frame ending. Only then is it
		// known how much of the randomiser it consumed.
		void ClosePendingUpdate()
		{
			if (!HavePendingUpdate)
				return;

			HavePendingUpdate = false;
			const UpdateEntry entry { PendingUpdateObject, DrawsThisFrame - PendingUpdateMark };

			if (TracedUpdateReference)
			{
				if (TracedUpdateCompareIndex >= TracedUpdateReference->size())
					ReportUpdateOrderMismatch("it ran more objects than before", TracedUpdateFrame,
						TracedUpdateCompareIndex, UpdateEntry {}, entry);
				else if (!((*TracedUpdateReference)[TracedUpdateCompareIndex] == entry))
					ReportUpdateOrderMismatch("a different object, or the same one drawing differently",
						TracedUpdateFrame, TracedUpdateCompareIndex,
						(*TracedUpdateReference)[TracedUpdateCompareIndex], entry);

				++TracedUpdateCompareIndex;
				return;
			}

			if (TracedUpdateTarget && ChargeDiagnosticMemory(2 * sizeof(UpdateEntry)))
			{
				TracedUpdateTarget->push_back(entry);
				++TracedUpdateCount;
			}
		}

		void BeginUpdateOrderFrame(int frame)
		{
			ClosePendingUpdate();

			if (TracedUpdateReference && TracedUpdateCompareIndex < TracedUpdateReference->size())
			{
				ReportUpdateOrderMismatch("it stopped early", TracedUpdateFrame, TracedUpdateCompareIndex,
					(*TracedUpdateReference)[TracedUpdateCompareIndex], UpdateEntry {});
			}

			DrawsThisFrame = 0;

			TracedUpdateFrame = frame;
			TracedUpdateTarget = nullptr;
			TracedUpdateReference = nullptr;
			TracedUpdateCompareIndex = 0;

			const auto it = UpdateOrderByFrame.find(frame);
			if (it != UpdateOrderByFrame.end())
			{
				TracedUpdateReference = &it->second;
				return;
			}

			if (TracedUpdateCount < MaxTracedUpdateEntries
				&& ChargeDiagnosticMemory(sizeof(std::vector<UpdateEntry>)
					+ 2 * sizeof(UpdateEntry)))
				TracedUpdateTarget = &UpdateOrderByFrame[frame];
		}

		void TraceObjectUpdate(const void* object)
		{
			if (!DiagnosticsWanted() || !ReplayState.Playback || Seek::IsLoadInProgress())
				return;

			const int frame = static_cast<int>(Unsorted::CurrentFrame);
			if (frame != TracedUpdateFrame)
				BeginUpdateOrderFrame(frame);

			// The previous object's update ends where this one begins.
			ClosePendingUpdate();

			PendingUpdateObject = UniqueIDOfAbstract(static_cast<const AbstractClass*>(object));
			PendingUpdateMark = DrawsThisFrame;
			HavePendingUpdate = true;
		}

		#pragma endregion Object update order trace

		// Each trace compares a replayed frame against its first pass as its hook fires, so a frame
		// that asked five times the first time round and none at all the second reported nothing:
		// there was no call left to notice the shortfall. The landing zone trace ran into exactly
		// that - the run after the load never entered New_LZ again, and its silence said nothing.
		//
		// Opening each frame from here instead fixes both halves of it. Every frame gets its list
		// opened whether or not anything asks, so a frame that stops asking is caught by the next
		// frame opening over it, and a frame that asks when the first pass asked nothing at all now
		// has an empty list to be measured against instead of quietly recording over it.
		//
		// 192MB. Generous next to what any one store needs and small enough to leave the game the
		// address space it wants: gamemd with a large map, Ares and Phobos loaded, and the keyframe
		// savegames on top of it, all inside 2GB.
		constexpr size_t MaxDiagnosticBytes = 192u * 1024u * 1024u;
		size_t DiagnosticBytesUsed = 0;
		bool DiagnosticBudgetReported = false;

		// Charged before a store grows, never after, so the allocation that would have gone over is
		// the one that does not happen.
		bool ChargeDiagnosticMemory(size_t bytes)
		{
			if (DiagnosticBytesUsed + bytes > MaxDiagnosticBytes)
			{
				if (!DiagnosticBudgetReported)
				{
					DiagnosticBudgetReported = true;
					Debug::Log("[Replay] The diagnostics have used the whole %u MB they are allowed, on "
						"frame %d. They will stop recording new frames from here; frames already recorded "
						"are still compared, and playback itself is unaffected.\n",
						static_cast<unsigned int>(MaxDiagnosticBytes / (1024u * 1024u)),
						static_cast<int>(Unsorted::CurrentFrame));
				}

				return false;
			}

			DiagnosticBytesUsed += bytes;
			return true;
		}

		void ResetDiagnosticMemory()
		{
			DiagnosticBytesUsed = 0;
			DiagnosticBudgetReported = false;
		}

		// This runs at the top of the frame, before anything in it has run.
		void ServiceTraces()
		{
			if (!DiagnosticsWanted() || !ReplayState.Playback)
				return;

			const int frame = static_cast<int>(Unsorted::CurrentFrame);

			if (frame != TracedMissionFrame)
				BeginMissionTraceFrame(frame);
			if (frame != TracedPathFrame)
				BeginPathRequestFrame(frame);
			if (frame != TracedLandingZoneFrame)
				BeginLandingZoneFrame(frame);
			if (frame != TracedUpdateFrame)
				BeginUpdateOrderFrame(frame);
		}

		// A trace that reports nothing is only worth something if it can be told apart from a trace
		// that never ran. One pathfinder run came back silent and there was no way to know whether
		// the questions matched or the hook had simply never fired.
		void ReportTraceCoverage()
		{
			if (!DiagnosticsWanted())
				return;

			Debug::Log("[Replay] Traces recorded %u randomiser draws, %u mission assignments, %u "
				"pathfinder requests and %u landing zone searches over %u, %u, %u and %u frames.\n",
				static_cast<unsigned int>(TracedRandomDrawCount),
				static_cast<unsigned int>(TracedMissionCount),
				static_cast<unsigned int>(TracedPathRequestCount),
				static_cast<unsigned int>(TracedLandingZoneCount),
				static_cast<unsigned int>(RandomDrawsByFrame.size()),
				static_cast<unsigned int>(MissionsByFrame.size()),
				static_cast<unsigned int>(PathRequestsByFrame.size()),
				static_cast<unsigned int>(LandingZonesByFrame.size()));
		}

		void CaptureGameCRCForCurrentFrame()
		{
			if (!ReplayState.Recording && !ReplayState.Playback)
				return;

			const uint32_t gameCRC = static_cast<uint32_t>(EventClass::CurrentFrameCRC);
			const int frameNumber = Unsorted::CurrentFrame;

			if (ReplayState.Recording)
			{
				if (ReplayState.PendingFrameStates.empty()
					|| ReplayState.PendingFrameStates.back().FrameNumber != frameNumber)
				{
					return;
				}

				PendingRecordedFrameCapture& capture = ReplayState.PendingFrameStates.back();
				capture.GameCRC = gameCRC;
				capture.HasGameCRC = true;
				capture.Census = CurrentObjectCensus();
				capture.HasCensus = true;
				capture.RandomState = CurrentRandomState();
				capture.HasRandomState = true;
				return;
			}

			CheckObjectCensusForCurrentFrame();

			if (!ReplayState.HasExpectedGameCRC)
				return;

			ReplayState.HasExpectedGameCRC = false;
			++ReplayState.CheckedFrameCount;

			if (ReplayState.ExpectedGameCRC == gameCRC)
			{
				if (ReplayState.LastCheckMismatched
					&& ReplayState.LoggedRecoveryCount < MaxLoggedDivergenceRecoveries)
				{
					++ReplayState.LoggedRecoveryCount;
					Debug::Log("[Replay] Playback matched the recording again on frame %d, after "
						"mismatching from frame %d to %d.\n", frameNumber,
						ReplayState.FirstMismatchFrame, ReplayState.LastMismatchFrame);
				}

				ReplayState.LastCheckMismatched = false;
				return;
			}

			++ReplayState.MismatchedFrameCount;
			if (ReplayState.FirstMismatchFrame < 0)
			{
				ReplayState.FirstMismatchFrame = frameNumber;
				ReportTraceCoverage();
			}

			ReplayState.LastMismatchFrame = frameNumber;
			ReplayState.LastCheckMismatched = true;

			if (ReplayState.LoggedMismatchCount < MaxLoggedDivergences)
			{
				++ReplayState.LoggedMismatchCount;
				// Compute_Game_CRC (0x64DAB0) hashes the object arrays and then draws from the scenario
				// randomiser, so a mismatched hash has two very different explanations. If the recording
				// says the randomiser was in the same place, the objects themselves differ - some state
				// the simulation reads was not carried across. If it was somewhere else, a draw was made
				// on one side and not the other, and the objects may be identical.
				const FrameRandomState randomState = CurrentRandomState();

				if (ReplayState.HasExpectedRandomState)
				{
					const bool randomiserMatches =
						randomState.Next1 == ReplayState.ExpectedRandomState.Next1
						&& randomState.Next2 == ReplayState.ExpectedRandomState.Next2;

					Debug::Log("[Replay] Playback diverged from the recording on frame %d "
						"(recorded CRC %08X, playback CRC %08X; randomiser at %d/%d, recorded %d/%d - %s).\n",
						frameNumber, ReplayState.ExpectedGameCRC, gameCRC,
						randomState.Next1, randomState.Next2,
						ReplayState.ExpectedRandomState.Next1, ReplayState.ExpectedRandomState.Next2,
						randomiserMatches
							? "randomiser in step, so the objects differ"
							: "randomiser drifted, so a draw was made on one side only");
				}
				else
				{
					Debug::Log("[Replay] Playback diverged from the recording on frame %d "
						"(recorded CRC %08X, playback CRC %08X; randomiser at %d/%d, not recorded).\n",
						frameNumber, ReplayState.ExpectedGameCRC, gameCRC,
						randomState.Next1, randomState.Next2);
				}

				if (ReplayState.LoggedMismatchCount == MaxLoggedDivergences)
				{
					Debug::Log("[Replay] Further per-frame divergences will not be logged; "
						"a total is written when playback ends.\n");
				}
			}

			if (!ReplayState.DivergenceReported)
			{
				ReplayState.DivergenceReported = true;

				MessageListClass::Instance.PrintMessage(
					DivergenceMessage,
					DivergenceMessageDurationFrames,
					ColorScheme::White,
					/* bSilent: */ true);
			}
		}

		void ComputeAndCaptureGameCRCForCurrentFrame()
		{
			if (!ReplayState.Recording && !ReplayState.Playback)
				return;

			Game::ComputeFrameCRC();
			CaptureGameCRCForCurrentFrame();
		}


		void LogPlaybackDivergenceSummary()
		{
			ReportTraceCoverage();

			if (!ReplayState.Playback || ReplayState.CheckedFrameCount == 0)
				return;

			if (ReplayState.MismatchedFrameCount == 0)
			{
				Debug::Log("[Replay] Playback matched the recording on all %d checked frames.\n",
					ReplayState.CheckedFrameCount);
				return;
			}

			Debug::Log("[Replay] Playback diverged on %d of %d checked frames (first frame %d, "
				"last frame %d).\n", ReplayState.MismatchedFrameCount, ReplayState.CheckedFrameCount,
				ReplayState.FirstMismatchFrame, ReplayState.LastMismatchFrame);
		}


		void StopReplaySystem()
		{
			LogPlaybackDivergenceSummary();

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

			auto* const pTactical = TacticalClass::Instance;
			const Point2D& target = ReplayState.LockedViewportPos;

			// Avoid forcing a full repaint when the viewport is already correct.
			if (pTactical->TacticalCoord1.X == target.X && pTactical->TacticalCoord1.Y == target.Y)
				return;

			pTactical->TacticalCoord1 = target;
			pTactical->TacticalCoord2 = target;
			pTactical->RecalculateViewport();
			pTactical->Redrawing = true;
		}

		// Whether playback should show the whole map instead of the recording player's shroud.
		bool PlaybackWantsFullMapReveal()
		{
			return ReplayState.Playback
				&& (!ReplayState.ShroudEnabled || ReplayState.SpectatorView);
		}

		void RestoreFrameState()
		{
			if (!ReplayState.Playback)
				return;

			const int currentFrame = static_cast<int>(Unsorted::CurrentFrame);
			if (ReplayState.PreparedPlaybackFrame == currentFrame)
				return;

			// Set this before reading: every successful or empty sparse frame is prepared exactly
			// once. A read failure stops playback and ResetRuntimeFlagsForScenario clears it.
			ReplayState.PreparedPlaybackFrame = currentFrame;

			ReplayState.ExpectedEventsThisFrame = 0;
			ReplayState.HasExpectedGameCRC = false;
			ReplayState.HasExpectedRandomState = false;
			ReplayState.HighestPlayedFrame = std::max(ReplayState.HighestPlayedFrame,
				currentFrame);

			// Keep the viewport locked on frames without replay records.
			ApplyLockedViewport();

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

			if ((frameRecord.Flags & FrameRecordFlag_GameCRC) != 0u)
			{
				ReplayState.ExpectedGameCRC = frameRecord.GameCRC;
				ReplayState.HasExpectedGameCRC = true;
			}

			if ((frameRecord.Flags & FrameRecordFlag_ObjectCensus) != 0u)
			{
				ReplayState.ExpectedCensus = frameRecord.Census;
				ReplayState.HasExpectedCensus = true;
			}

			if ((frameRecord.Flags & FrameRecordFlag_RandomState) != 0u)
			{
				ReplayState.ExpectedRandomState = frameRecord.RandomState;
				ReplayState.HasExpectedRandomState = true;
			}

			// Applied from the same point in the frame the recording read it from.
			if ((frameRecord.Flags & FrameRecordFlag_GameSpeed) != 0u)
			{
				const int gameSpeed = std::clamp(static_cast<int>(frameRecord.GameSpeed), 0, MaxGameSpeedIndex);
				GameOptionsClass::Instance.GameSpeed = gameSpeed;
				GameModeOptionsClass::Instance.GameSpeed = gameSpeed;
			}

			ReplayState.HasPendingPlaybackFrame = false;
		}

		// Opens a fresh capture buffer for the frame. The events themselves are appended by
		// RecordExecutedEvent as the engine consumes them.
		void CaptureEventsForCurrentFrame()
		{
			if (!ReplayState.Recording)
				return;

			ReplayState.CapturedFrameEvents.clear();
			ReplayState.CapturedFrameEventsFrame = Unsorted::CurrentFrame;
		}

		// Called from the one instruction where Execute_DoList marks a queue entry consumed, so the
		// recording follows the engine's own decision rather than a before/after test of IsExec.
		void RecordExecutedEvent(const EventClass* pEvent)
		{
			if (!ReplayState.Recording || !pEvent)
				return;

			// Execute_DoList runs inside the frame's Queue_AI, after CaptureEventsForCurrentFrame has
			// opened the buffer. Re-opening it here keeps the frame attribution right if a hook order
			// change ever means the engine reaches this site first.
			if (ReplayState.CapturedFrameEventsFrame != Unsorted::CurrentFrame)
			{
				ReplayState.CapturedFrameEvents.clear();
				ReplayState.CapturedFrameEventsFrame = Unsorted::CurrentFrame;
			}

			auto& recorded = ReplayState.CapturedFrameEvents.emplace_back(*pEvent);
			// Events are stored unexecuted so the same event is always the same bytes on disk.
			recorded.IsExecuted = false;
		}

		void RecordCapturedEventsForCurrentFrame()
		{
			if (!ReplayState.Recording)
				return;

			const int frameNumber = Unsorted::CurrentFrame;
			auto& capturedEvents = ReplayState.CapturedFrameEvents;
			const bool hasCapturedEventsForFrame = ReplayState.CapturedFrameEventsFrame == frameNumber;
			const int eventsThisFrame = hasCapturedEventsForFrame
				? static_cast<int>(capturedEvents.size())
				: 0;

			FlushPendingRecordedFramesThrough(frameNumber, eventsThisFrame);
			if (!ReplayState.Recording)
				return;

			if (hasCapturedEventsForFrame)
			{
				for (const auto& event : capturedEvents)
				{
					if (!WriteRaw(&event, sizeof(EventClass)))
					{
						Debug::Log("[Replay] Failed writing event to replay stream.\n");
						StopReplaySystem();
						return;
					}
				}
			}

			capturedEvents.clear();
			ReplayState.CapturedFrameEventsFrame = -1;

			if (frameNumber - ReplayState.LastSyncFlushFrame >= SyncFlushFrameInterval)
			{
				ReplayState.LastSyncFlushFrame = frameNumber;
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
				// EventClass has no default constructor, so read into raw storage.
				alignas(EventClass) char eventBuffer[sizeof(EventClass)] = { 0 };
				EventClass* replayEvent = reinterpret_cast<EventClass*>(eventBuffer);

				if (!ReadRaw(replayEvent, sizeof(EventClass)))
				{
					Debug::Log("[Replay] Event stream ended unexpectedly during playback.\n");
					StopReplaySystem();
					return;
				}

				// Every event on the frame was recorded, so the whole batch is read to keep the stream
				// aligned, but only the ones that drive the simulation get injected.
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

		// Closes out the recording of a mission that has just ended, and stops any scenario after it
		// from being recorded. A replay embeds the spawn.ini and spawnmap.ini the client wrote before
		// the game started, and those describe the launched mission only.
		void FinishRecordingAtMissionEnd()
		{
			if (!ReplayState.Recording)
				return;

			Debug::Log("[Replay] Mission over at frame %d; finishing the recording in %s. Later missions in "
				"this campaign are not recorded.\n", ReplayState.LastWrittenFrameNumber, GetRecordingOutputPath());

			StopReplaySystem();
			ReplayState.RecordingFinishedForSession = true;
		}

		void StartReplayRecording()
		{
			// A campaign mission that has already been recorded owns the output file for the rest of the
			// launch; see FinishRecordingAtMissionEnd.
			if (ReplayState.RecordingFinishedForSession)
			{
				Debug::Log("[Replay] Not recording this scenario: %s already holds this campaign's first "
					"mission.\n", GetRecordingOutputPath());
				StopReplaySystem();
				return;
			}

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

			const auto* pConfig = GetConfig();

			// ReplayPlaybackSpeed is a frame rate, one of the rungs of the viewer's speed ladder,
			// not a game speed index. Zero or less watches at the speed it was recorded at.
			const int requestedFPS = pConfig ? pConfig->ReplayPlaybackSpeed : 0;

			int recordedGameSpeed = std::clamp(GameOptionsClass::Instance.GameSpeed, 0, MaxGameSpeedIndex);
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
				recordedGameSpeed = std::clamp(static_cast<int>(ReplayState.PlaybackHeader.RecordedGameSpeed), 0, MaxGameSpeedIndex);

			// Keep one pacing path active even when no override was requested. A zero sentinel made
			// normal playback rely on the engine timers while seeks and controls used the replay
			// clock, allowing a pause/single-step transition to resume without a valid deadline.
			ReplayState.PlaybackFPS = requestedFPS > 0
				? requestedFPS
				: GetReplayFPSFromGameSpeed(recordedGameSpeed);

			// The speed the recording started at, applied once and never re-pinned: a change the
			// player made during the game is in the event stream and is replayed like any other.
			GameOptionsClass::Instance.GameSpeed = recordedGameSpeed;
			GameModeOptionsClass::Instance.GameSpeed = recordedGameSpeed;

			ReplayState.ShroudEnabled = pConfig ? pConfig->ReplayShroudEnabled : false;
			ReplayState.LockViewport = pConfig ? pConfig->ReplayLockedViewport : true;
			ReplayState.SelectUnits = pConfig ? pConfig->ReplaySelectUnits : true;
			ReplayState.SpectatorView = ReplaySystem::IsSpectatorPlayback();
			ReplayState.ShowChatAndBeacons = pConfig ? pConfig->ReplayShowChatAndBeacons : true;

			// Both of these reproduce the recording player's own screen, so neither means anything
			// once the viewpoint has been handed to someone else.
			if (ResolveViewPlayerIndex() > 0)
			{
				ReplayState.LockViewport = false;
				ReplayState.SelectUnits = false;
			}

			strncpy_s(ReplayState.PlaybackPath, sizeof(ReplayState.PlaybackPath), replayPath, _TRUNCATE);

			ReplaySystem::Controls::InitControlBarVisibility();
			ReplaySystem::Seek::OnPlaybackStarted();

			ReplayOpenFailure failure = ReplayOpenFailure::None;
			if (!OpenPlaybackReplayStream(ReplayState.PlaybackPath, failure))
			{
				StopReplaySystem();

				// StartScenario already skipped CreateConnections, so there is no live session to
				// fall back to. This message is all the player gets, so it names the reason.
				Debug::FatalErrorAndExit("[Replay] Cannot play %s: %s.",
					ReplayState.PlaybackPath, DescribeReplayOpenFailure(failure));
			}
		}
	}
}

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

int ReplaySystem::GetViewPlayerIndex()
{
	return ResolveViewPlayerIndex();
}

void ReplaySystem::ApplyPlaybackViewPlayer()
{
	ApplyViewPlayerToCurrentPlayer();
}

bool ReplaySystem::IsSpectatorPlayback()
{
	const auto* pConfig = GetConfig();

	return pConfig && IsPlaybackRequested() && pConfig->ReplaySpectator;
}

void ReplaySystem::ApplyPlaybackSpectator()
{
	if (!IsSpectatorPlayback())
		return;

	HouseClass* const pPlayer = HouseClass::CurrentPlayer;
	if (!pPlayer)
		return;

	Game::ObserverMode = true;
	if (pPlayer->MakeObserver())
		TabClass::Instance.ThumbActive = false;

	Debug::Log("[Replay] Watching the replay from an observer seat.\n");
}

void ReplaySystem::OnGameStartReset()
{
	StopReplaySystem();
	ReplayState.InitRandomHandled = false;
	ReplayState.RecordingFinishedForSession = false;
	ReplayState.PlaybackPath[0] = '\0';
}
