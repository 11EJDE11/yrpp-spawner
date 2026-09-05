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
#include <Utilities/Debug.h>

#include <GameOptionsClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <Unsorted.h>

#include <algorithm>
#include <ctime>
#include <cstring>
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

		ReplayHeader BuildReplayHeader(uint32_t spawnIniSize, uint32_t spawnMapSize)
		{
			ReplayHeader header {};
			header.Magic = ReplayMagic;
			header.Version = ReplayVersion;
			header.HeaderSize = sizeof(ReplayHeader);

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

		const bool scenarioIsSpawnMap = pConfig
			&& _stricmp(pConfig->ScenarioName, SpawnMapFileName) == 0;

		if (scenarioIsSpawnMap && !ReadRequiredFile(SpawnMapFileName, spawnMap))
		{
			Debug::Log("[Replay] Required file spawnmap.ini was not found or could not be read.\n");
			return false;
		}

		if (!scenarioIsSpawnMap)
			spawnMap.clear();

		// Embed the launch INIs
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
