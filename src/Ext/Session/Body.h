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

// Re-created bits of Vinifera's SessionClassExtension. The game's native
// master/host fields (MasterPlayerID / MasterPlayerName) exist but nothing sets
// them in a LAN-mode game, so the master is recorded through here.

#pragma once

class IPXAddressClass;

namespace SessionExt
{
	// The engine supports up to 8 multiplayer houses.
	constexpr int MaxPlayers = 8;

	// Assigns the game master/host, writing the engine's native MasterPlayerID
	// and MasterPlayerName.
	void Set_Master(int house_id);

	// Called on the host at game start: records itself as the master and tells
	// every other player who the host is (EXT_NET_HOST_ANNOUNCE), so MasterPlayerID
	// is the same on all machines. Mirrors Vinifera's
	// SessionClassExtension::Announce_Master.
	void Announce_Master();

	// Recomputes the master after a player has gone: if the current master is no
	// longer connected (or none was announced), promotes the connected player with
	// the lowest house id.
	void Update_Master_After_Player_Removal();

	// The house of the connected player a global packet came from, or -1 when it
	// came from no player still in the game here.
	int Sender_House(const IPXAddressClass& address);
}
