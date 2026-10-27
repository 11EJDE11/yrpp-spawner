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

#pragma once

struct ExtGlobalPacketType;

// When a wait for a player times out, the engine removes the player that machine heard from last,
// each machine by itself. If only two players lose each other while both still reach the rest,
// each removes the other and the rest keep both: the game desyncs. Instead the game master decides
// who goes, everyone agrees on the frame up to which that player's orders count, and whoever is
// missing some of those orders is sent them by a player who has them.
namespace KickAgreement
{
	// What the timed-out wait should do (the hook at 0x64B472 in network_64B410).
	enum class TimeoutAction
	{
		Vanilla,     // remove the player, as the engine does
		KeepWaiting, // a removal is being agreed on; wait on
		GiveUp,      // this player is the one being removed
	};

	TimeoutAction On_Timeout(int connection_index);

	// One of our global packets arrived. Returns whether it was a kick agreement one.
	bool Handle_Global_Packet(const ExtGlobalPacketType* packet);

	// Each pass of Call_Back, which the wait for players runs too.
	void Update();

	// Whether the next frame waits: this player has said how far it got for a removal not yet settled.
	bool Hold_Frames();

	// At the start of Execute_DoList: keep a copy of the orders executed this frame, and drop the
	// orders of removed players after their cutoff.
	void Before_Execute_DoList();

	// Forgets everything: after a save is loaded mid-game, the frames it knows are of another timeline.
	void Reset();
}
