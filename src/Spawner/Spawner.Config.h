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

#pragma once
#include <Main.h>

class CCINIClass;

class SpawnerConfig
{

	// Used to create NodeNameType
	// The order of entries may differ from HouseConfig
	struct PlayerConfig
	{
		bool IsHuman;
		wchar_t Name[20];
		int Color;
		int Country;
		int Difficulty;
		bool IsObserver;
		char Ip[0x20];
		int Port;

		PlayerConfig()
			: IsHuman { false }
			, Name { L"" }
			, Color { -1 }
			, Country { -1 }
			, Difficulty { -1 }
			, IsObserver { false }
			, Ip { "0.0.0.0" }
			, Port { -1 }
		{ }

		void LoadFromINIFile(CCINIClass* pINI, int index);
	};

	// Used to augment the generated HouseClass
	// The order of entries may differ from PlayerConfig
	struct HouseConfig
	{
		bool IsObserver;
		int SpawnLocations;
		double CreditsFactor;
		int HandicapDifficulty;
		int Alliances[8];

		HouseConfig()
			: IsObserver { false }
			, SpawnLocations { -2 }
			, CreditsFactor { 1.0 }
			, HandicapDifficulty { -1 }
			, Alliances { -1, -1, -1, -1, -1, -1, -1, -1 }
		{ }

		void LoadFromINIFile(CCINIClass* pINI, int index);
	};

public:
	// Game Mode Options
	int  MPModeIndex;
	bool Bases;
	int  Credits;
	bool BridgeDestroy;
	bool Crates;
	bool ShortGame;
	bool SuperWeapons;
	bool BuildOffAlly;
	int  GameSpeed;
	bool MultiEngineer;
	int  UnitCount;
	int  AIPlayers;
	int  AIDifficulty;
	bool AlliesAllowed;
	bool HarvesterTruce;
	bool FogOfWar;
	bool MCVRedeploy;
	wchar_t UIGameMode[60];
	bool SpecialHouseIsAlly;

	// SaveGame Options
	bool LoadSaveGame;
	char SavedGameDir[MAX_PATH]; // Nested paths are also supported, e.g. "Saved Games\\Yuri's Revenge"
	char SaveGameName[60];
	int CustomMissionID;
	int AutoSaveCount;
	int AutoSaveInterval;
	int NextAutoSaveNumber;

	// Replay Options
	bool EnableReplayRecording;
	// Where a recording is written, relative to the game directory. Missing folders are created.
	char ReplayFileOut[MAX_PATH];
	// The replay to watch. Empty means no playback.
	char ReplayFile[MAX_PATH];
	bool ReplayShroudEnabled;
	bool ReplayLockedViewport;
	bool ReplaySelectUnits;
	// How fast to watch a replay, in frames per second - a rung of SpeedLadder in ReplayControls.h.
	// Zero or less watches it at the speed it was recorded at.
	int  ReplayPlaybackSpeed;
	// Watch as an observer: reveals the whole map and makes cloaked and disguised units and radar
	// blips visible.
	bool ReplaySpectator;
	// Playback only. Recording chat, beacons and taunts is unconditional.
	bool ReplayShowChatAndBeacons;
	// Which spawn.ini player slot playback is watched from: 0 or -1 is the recording player, N is
	// [OtherN]. Only human slots are accepted; anything else falls back to the recording player.
	int  ReplayViewPlayer;
	// Draw the on-screen playback controls - seek bar, transport buttons, clock - during playback.
	bool ReplayControlBar;
	// How many frames apart playback drops a savegame keyframe, so seeking backwards restarts from
	// one instead of replaying from the beginning. Zero or less takes none, which leaves seeking
	// forward-only. See ReplaySeek.h.
	int  ReplayKeyframeInterval;

	// Per-frame state watchers that compare a seek against the frames playback already went
	// through, and name the first object, cell or randomiser draw the two disagree about. They
	// walk every cell on the map every frame, so they are far too slow to leave on: this is for
	// chasing a divergence, not for watching a replay.
	// They also keep everything they record for the life of the replay, inside a 192MB budget
	// they report reaching, so leaving them on costs memory as well as time.
	bool ReplayDiagnostics;

	// A seek draws one frame in sixty so a long one does not look like a hang. That makes the
	// frames it replays differ from the frames playback drew the first time round, which matters
	// only if anything on the draw path writes state the simulation reads - as AircraftClass::
	// Draw_It once did, calling Set_Height(0) for its shadow and recalculating the cell under it.
	// That one is patched out in Bugfixes.Desyncs.cpp; this exists to answer whether anything
	// else does the same, by drawing every frame of a seek so the two passes match.
	//
	// That test came back negative - the divergence was identical either way - so this is off
	// and a seek draws one frame in sixty again. Kept because it costs nothing and re-tests the
	// whole question in one run.
	bool ReplaySeekRenderEveryFrame;

	// Scenario Options
	int  Seed;
	int  TechLevel;
	bool IsCampaign;
	int  Tournament;
	DWORD WOLGameID;
	char ScenarioName[260];
	char MapHash[0xff];
	wchar_t UIMapName[45];
	bool ReadMissionSection;

	// Network Options
	int Protocol;
	int FrameSendRate;
	int ReconnectTimeout;
	int ConnTimeout;
	int MaxAhead;
	int PreCalcMaxAhead;
	byte MaxLatencyLevel;
	bool ForceMultiplayer;

	// Tunnel Options
	int  TunnelId;
	char TunnelIp[0x20];
	int  TunnelPort;
	int  ListenPort;

	// Players Options
	PlayerConfig Players[8];

	// Houses Options
	HouseConfig Houses[8];

	// Extended Options
	bool Ra2Mode;
	bool DisableGameSpeed;
	bool QuickMatch;
	bool SkipScoreScreen;
	bool WriteStatistics;
	bool AINamesByDifficulty;
	bool ContinueWithoutHumans;
	bool DefeatedBecomesObserver;
	bool Observer_ShowAIOnSidebar;
#ifdef IS_CNCNET_YR_VER
	bool DisableChat;
#endif

	SpawnerConfig() // default values
		// Game Mode Options
		: MPModeIndex { 1 }
		, Bases { true }
		, Credits { 10000 }
		, BridgeDestroy { true }
		, Crates { false }
		, ShortGame { false }
		, SuperWeapons { true }
		, BuildOffAlly { false }
		, GameSpeed { 0 }
		, MultiEngineer { false }
		, UnitCount { 0 }
		, AIPlayers { 0 }
		, AIDifficulty { 1 }
		, AlliesAllowed { false }
		, HarvesterTruce { false }
		, FogOfWar { false }
		, MCVRedeploy { true }
		, UIGameMode { L"" }
		, SpecialHouseIsAlly { true }

		// SaveGame
		, LoadSaveGame { false }
		, SavedGameDir { "Saved Games" }
		, SaveGameName { "" }
		, CustomMissionID { 0 }
		, AutoSaveCount { 5 }
		, AutoSaveInterval { 7200 }
		, NextAutoSaveNumber { 0 }
		, EnableReplayRecording { false }
		, ReplayFileOut { "replay.yrrp" }
		, ReplayFile { "" }
		, ReplayShroudEnabled { false }
		, ReplayLockedViewport { true }
		, ReplaySelectUnits { true }
		, ReplayPlaybackSpeed { 0 }
		, ReplaySpectator { false }
		, ReplayShowChatAndBeacons { true }
		, ReplayViewPlayer { -1 }
		, ReplayControlBar { true }
		, ReplayKeyframeInterval { 750 }
		, ReplayDiagnostics { true }
		, ReplaySeekRenderEveryFrame { false }

		// Scenario Options
		, Seed { 0 }
		, TechLevel { 10 }
		, IsCampaign { false }
		, Tournament { 0 }
		, WOLGameID { 0xDEADBEEF }
		, ScenarioName { "spawnmap.ini" }
		, MapHash { "" }
		, UIMapName { L"" }
		, ReadMissionSection { false }

		// Network Options
		, Protocol { 2 }
		, FrameSendRate { 4 }
		, ReconnectTimeout { 2400 }
		, ConnTimeout { 3600 }
		, MaxAhead { -1 }
		, PreCalcMaxAhead { 0 }
		, MaxLatencyLevel { 0xFF }
		, ForceMultiplayer { false }

		// Tunnel Options
		, TunnelId { 0 }
		, TunnelIp { "0.0.0.0" }
		, TunnelPort { 0 }
		, ListenPort { 1234 }

		// Players Options
		, Players {
			PlayerConfig(),
			PlayerConfig(),
			PlayerConfig(),
			PlayerConfig(),

			PlayerConfig(),
			PlayerConfig(),
			PlayerConfig(),
			PlayerConfig()
		}

		// Houses Options
		, Houses {
			HouseConfig(),
			HouseConfig(),
			HouseConfig(),
			HouseConfig(),

			HouseConfig(),
			HouseConfig(),
			HouseConfig(),
			HouseConfig()
		}

		// Extended Options
		, Ra2Mode { false }
		, DisableGameSpeed { false }
		, QuickMatch { false }
		, SkipScoreScreen { Main::GetConfig()->SkipScoreScreen }
		, WriteStatistics { false }
		, AINamesByDifficulty { false }
		, ContinueWithoutHumans { false }
		, DefeatedBecomesObserver { false }
		, Observer_ShowAIOnSidebar { false }
#ifdef IS_CNCNET_YR_VER
		, DisableChat { false }
#endif
	{ }

	void LoadFromINIFile(CCINIClass* pINI);
};
