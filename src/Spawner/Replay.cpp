/**
*  yrpp-spawner
*
*  Copyright(C) 2022-present CnCNet
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

#include "Replay.h"
#include "Spawner.h"

#include <CCFileClass.h>
#include <SessionClass.h>
#include <Unsorted.h>
#include <Utilities/Debug.h>

#include <Windows.h>
#include <cstdio>

// The game's native function that writes BuildLevel/Seed/ScenarioName/Whom/Special/Options
// to the open RecordFile. Same format as Save_Recording_Values (0x5318C0).
// RecordFile header layout (464 bytes total):
//   offset 0:   BuildLevel  (4)
//   offset 4:   Seed        (4)
//   offset 8:   Scen.Scenario (4)
//   offset 12:  ScenarioName (260)
//   offset 272: Whom        (4)
//   offset 276: Special     (4)
//   offset 280: Options     (184)
//   offset 464: event stream begins
using RecordingValuesFunc = char(__thiscall*)(void*);
static auto Save_Recording_Values = reinterpret_cast<RecordingValuesFunc>(0x5318C0);

void Replay::PrepareRecording()
{
	SessionClass::Instance.Record = 1;
	Debug::Log("[Spawner] Replay: recording prepared\n");
}

void Replay::StartRecording()
{
	auto& recordFile = SessionClass::Instance.RecordFile;

	if (!recordFile.Open(FileAccessMode::Write))
	{
		Debug::Log("[Spawner] Replay: failed to open RECORD.BIN for writing\n");
		SessionClass::Instance.Record = 0;
		return;
	}

	// Write the standard recording header (BuildLevel, Seed, ScenarioName, Whom, Special, Options)
	Save_Recording_Values(&recordFile);

	// Create a flag file so the client knows a recording was started and RECORD.BIN is valid.
	// The client deletes this file after packaging the replay.
	HANDLE hFlag = CreateFileA(
		"spawn_recording_active.flag",
		GENERIC_WRITE, 0, NULL,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFlag != INVALID_HANDLE_VALUE)
		CloseHandle(hFlag);

	Debug::Log("[Spawner] Replay: recording started (RECORD.BIN open)\n");
}

void Replay::SetupPlayback()
{
	auto pConfig = Spawner::GetConfig();

	char eventsPath[MAX_PATH];
	size_t dirLen = strlen(pConfig->ReplayDataDir);
	bool hasTrailingSlash = dirLen > 0 && (pConfig->ReplayDataDir[dirLen - 1] == '\\' || pConfig->ReplayDataDir[dirLen - 1] == '/');
	snprintf(eventsPath, sizeof(eventsPath), hasTrailingSlash ? "%sevents.dat" : "%s\\events.dat", pConfig->ReplayDataDir);

	auto& recordFile = SessionClass::Instance.RecordFile;
	recordFile.SetFileName(eventsPath);

	if (!recordFile.Open(FileAccessMode::Read))
	{
		Debug::Log("[Spawner] Replay: failed to open events.dat for playback: %s\n", eventsPath);
		return;
	}

	// Read BuildLevel (discard) and Seed from the header, then seek to the event stream.
	int buildLevel = 0;
	int recordedSeed = 0;
	recordFile.ReadBytes(&buildLevel, sizeof(buildLevel));
	recordFile.ReadBytes(&recordedSeed, sizeof(recordedSeed));

	// Override the seed set from spawn.ini so Init_Random restores the recorded RNG state.
	Game::Seed = recordedSeed;

	// Seek past the rest of the header (464 bytes total; already read 8).
	recordFile.Seek(456, FileSeekMode::Current);

	// Tell Queue_AI to read events from RecordFile instead of accepting player input.
	SessionClass::Instance.Play = 1;

	// TrapPrintCRC defaults to 0, which makes the playback path's check
	// "if (frame >= TrapPrintCRC)" fire immediately at frame 0, dumping CRCs
	// and calling Emergency_Exit(0). Disable it by setting it to INT_MAX.
	auto& TrapPrintCRC = *reinterpret_cast<int*>(0xa8e30cu);
	TrapPrintCRC = 0x7FFFFFFF;

	Debug::Log("[Spawner] Replay: playback set up from %s (Seed=%08x)\n", eventsPath, recordedSeed);
}
