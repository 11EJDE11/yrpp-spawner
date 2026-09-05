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

#include "ReplayFile.h"

#include <Spawner/Spawner.Config.h>
#include <Ext/INIClass/Body.h>
#include <Utilities/Debug.h>
#include <version.h>

#include <CCINIClass.h>
#include <GameOptionsClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <Unsorted.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <limits>
#include <vector>

namespace Replay
{
	namespace
	{
		constexpr const char* DefaultRecordingPath = "replay.yrrp";
		constexpr const char* SpawnMapFileName = "spawnmap.ini";

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

		bool WriteRawToHandle(HANDLE file, const void* data, size_t size)
		{
			if (file == INVALID_HANDLE_VALUE)
				return false;

			DWORD bytesWritten = 0;
			return WriteFile(file, data, static_cast<DWORD>(size), &bytesWritten, nullptr)
				&& bytesWritten == static_cast<DWORD>(size);
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

		ReplayHeader BuildReplayHeader(uint32_t spawnIniSize, uint32_t spawnMapSize)
		{
			ReplayHeader header {};
			header.Magic = ReplayMagic;
			header.Version = ReplayVersion;
			header.HeaderSize = sizeof(ReplayHeader);

			const ScopedINIFile spawnIni { "spawn.ini" };

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
	}

	const char* GetRecordingOutputPath(const SpawnerConfig* pConfig)
	{
		if (pConfig && pConfig->ReplayFileOut[0] != '\0')
			return pConfig->ReplayFileOut;

		return DefaultRecordingPath;
	}

	bool WriteInitialReplayFile(const SpawnerConfig* pConfig)
	{
		std::vector<char> spawnIni;
		std::vector<char> spawnMap;

		if (!ReadRequiredFile("spawn.ini", spawnIni))
		{
			Debug::Log("[Replay] Required file spawn.ini was not found or could not be read.\n");
			return false;
		}

		SanitizeSpawnIniForReplay(spawnIni);

		const bool scenarioIsSpawnMap = pConfig
			&& _stricmp(pConfig->ScenarioName, SpawnMapFileName) == 0;

		if (scenarioIsSpawnMap && !ReadRequiredFile(SpawnMapFileName, spawnMap))
		{
			Debug::Log("[Replay] Required file spawnmap.ini was not found or could not be read.\n");
			return false;
		}

		if (!scenarioIsSpawnMap)
			spawnMap.clear();

		// Sizes describe the sanitized bytes embedded below, so playback finds the frame stream.
		const ReplayHeader header = BuildReplayHeader(
			static_cast<uint32_t>(spawnIni.size()),
			static_cast<uint32_t>(spawnMap.size())
		);

		const char* const outputPath = GetRecordingOutputPath(pConfig);
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
}
