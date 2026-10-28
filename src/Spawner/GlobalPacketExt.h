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

// New, spawner-added global network packet types.
//
// This mirrors Vinifera's ExtGlobalPacketType: the engine's out-of-band global
// packet (GlobalPacketType) is 0x1C7 = 455 bytes, and the IPX layer accepts a
// raw buffer of that size, so an alternative struct of the same size can ride
// the very same channel. Command sits at offset 0 (matching GlobalPacketType),
// which is what Network_Call_Back's dispatch switch reads; commands the engine
// does not know fall through to its `default` case, which is where the receiver
// for these is meant to be hooked in.

#pragma once

// Command values for the spawner's own global packets.
//
// The engine's native command values run up to ~0x2F. These are placed well
// above that range so they never collide and always reach the unknown-command
// (default) dispatch path.
enum ExtNetCommandType : int
{
	EXT_NET_DESYNC_HEARTBEAT = 0xE0, // Periodic keep-alive while the desync dialog is open; also detects departures.
	EXT_NET_DESYNC_CONTINUE  = 0xE1, // The host's decision to continue the game without the desynced players.
	EXT_NET_DESYNC_CHAT      = 0xE2, // A chat line typed in the desync dialog while game logic is halted.
	EXT_NET_HOST_ANNOUNCE    = 0xE3, // The host announcing itself at game start so everyone records the master.
	EXT_NET_DESYNC_LOAD      = 0xE4, // The host's decision to go back to a save everyone made at the same frame.
	EXT_NET_DESYNC_READY     = 0xE5, // A player has nothing unacknowledged left in flight and can load.
	EXT_NET_DESYNC_GO        = 0xE6, // The host telling everyone to load now.
	EXT_NET_KICK_REQUEST     = 0xE7, // A player that timed out waiting asks the game master to remove the player it can't hear.
	EXT_NET_KICK_PREPARE     = 0xE8, // The game master is removing a player: everyone says how far it has played.
	EXT_NET_KICK_STATE       = 0xE9, // A player's reply to KICK_PREPARE: the last frame it executed.
	EXT_NET_KICK_COMMIT      = 0xEA, // The game master's decision: the removed player's orders count up to a cutoff frame.
	EXT_NET_KICK_EVENTS      = 0xEB, // The removed player's orders up to the cutoff, for those who did not get them.
	EXT_NET_DESYNC_RESUME    = 0xEC, // A player has the decision to continue and resumes; it will no longer leave from the dialog.
};

#pragma pack(push, 1)
struct ExtGlobalPacketType
{
	// Must alias GlobalPacketType::Command (offset 0) so the engine dispatch
	// reads it correctly.
	int Command;

	// Sender's display name (ANSI; converted from the wide UIName on send).
	char Name[32];

	union
	{
		struct
		{
			char HouseID; // Sender's house (ArrayIndex).
			char IsHost;  // Non-zero if the sender is the game master.
		} Heartbeat;

		struct
		{
			char SenderHouseID;
			char Text[200]; // ANSI chat text.
		} Chat;

		struct
		{
			int SaveFrame;   // The frame the save to load was made on.
			int DesyncFrame; // The frame the desync was found on.
		} Load;

		// Agreeing on the removal of a player that some can't hear (KickAgreement).
		// Every one of these is passed on once by each player that gets it, so it
		// also reaches the players that can't hear its sender.
		struct
		{
			int DecisionID;   // The game master's number for this removal.
			char MasterID;    // The game master's house.
			char TargetID;    // The house being removed.
			char FromID;      // The house that sent this one.
			char Seq;         // KICK_EVENTS: which packet of the set this is.
			char Total;       // KICK_EVENTS: how many packets the set has.
			char Count;       // KICK_EVENTS: how many orders this packet holds; KICK_STATE: 1 if the sender still hears the house being removed.
			char SourceID;    // KICK_COMMIT: the house that sends the orders.
			int Frame;        // KICK_STATE: last frame executed; KICK_COMMIT/EVENTS: the cutoff (-2 on a COMMIT: called off).
			int FirstFrame;   // KICK_COMMIT/EVENTS: the first frame whose orders are sent.
			char Events[3][111]; // KICK_EVENTS: EventClass copies.
		} Kick;

		// Forces the whole struct to the engine's GlobalPacketType size so the
		// IPX layer treats it identically.
		char _padding[455 - sizeof(int) - 32];
	};
};
#pragma pack(pop)

static_assert(sizeof(ExtGlobalPacketType) == 455, "ExtGlobalPacketType must match the engine's GlobalPacketType (0x1C7 bytes)");
