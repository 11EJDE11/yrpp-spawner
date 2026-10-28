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

#include "Body.h"

#include <Spawner/GlobalPacketExt.h>

#include <SessionClass.h>
#include <HouseClass.h>
#include <IPX.h>
#include <IPXConnClass.h>
#include <IPXManagerClass.h>

#include <cwchar>

void SessionExt::Set_Master(int house_id)
{
	SessionClass::Instance.MasterPlayerID() = house_id;

	wchar_t* const master_name = SessionClass::Instance.MasterPlayerName();
	master_name[0] = L'\0';

	if (HouseClass::Array.Count > house_id && house_id >= 0)
	{
		if (HouseClass* house = HouseClass::Array[house_id])
		{
			// MasterPlayerName is wchar_t[21]; UIName is the same width.
			wcsncpy(master_name, house->UIName, 20);
			master_name[20] = L'\0';
		}
	}
}

void SessionExt::Announce_Master()
{
	HouseClass* const me = HouseClass::CurrentPlayer;
	if (me == nullptr)
		return;

	// Record ourselves as the master locally; we never receive our own packet.
	Set_Master(me->ArrayIndex);

	ExtGlobalPacketType packet {};
	packet.Command = EXT_NET_HOST_ANNOUNCE;
	packet.Heartbeat.HouseID = static_cast<char>(me->ArrayIndex);
	packet.Heartbeat.IsHost = 1;

	// Send to every other player. As in the engine's own beacons, index 0 is the
	// local player, so start at 1. (Addresses come from the connections that were
	// just created.)
	auto& players = NodeNameType::Array;
	for (int i = 1; i < players.Count; i++)
	{
		NodeNameType* const node = players[i];
		if (node == nullptr)
			continue;

		IPXManagerClass::Instance.Send_Global_Message(
			&packet, sizeof(packet), 1,
			reinterpret_cast<IPXAddressClass*>(&node->Address), 0, 0);
	}
	IPXManagerClass::Instance.Service();
}

void SessionExt::Update_Master_After_Player_Removal()
{
	// Mirrors Vinifera: decide the master from the connected-players list
	// (NodeNameType::Array == SessionClass::Players, 0xA8DA74), which
	// Destroy_Connection shrinks immediately. A departed house is only flagged
	// defeated when its queued E_REMOVEPLAYER event runs, so the houses' Defeated
	// flag would lag. node->HouseIndex is the house id (the same value
	// Set_Master/MasterPlayerID use).
	const int current = SessionClass::Instance.MasterPlayerID();
	auto& players = NodeNameType::Array;

	// If the current master is still connected, there is nothing to do.
	if (current != -1)
	{
		for (int i = 0; i < players.Count; i++)
		{
			NodeNameType* const node = players[i];
			if (node != nullptr && node->HouseIndex == current)
				return;
		}
	}

	// Otherwise promote the connected player with the lowest house id. This is
	// deterministic, so every remaining client agrees without negotiation.
	int new_master = -1;
	for (int i = 0; i < players.Count; i++)
	{
		NodeNameType* const node = players[i];
		if (node == nullptr)
			continue;

		const int id = node->HouseIndex;
		if (id >= 0 && (new_master == -1 || id < new_master))
			new_master = id;
	}

	if (new_master != -1 && new_master != current)
		Set_Master(new_master);
}

int SessionExt::Sender_House(const IPXAddressClass& address)
{
	// IPXAddressClass::operator==, as Network_Call_Back matches a sign-off to its sender (0x48D84C).
	using SameFn = bool(__fastcall*)(const IPXAddressClass* self, void* edx, const IPXAddressClass* other);
	const auto same = reinterpret_cast<SameFn>(0x53EE70);

	auto& ipx = IPXManagerClass::Instance;
	for (int i = 0; i < static_cast<int>(ipx.NumConnections); i++)
	{
		const IPXConnClass* const connection = ipx.Connection[i];
		if (connection != nullptr && same(&address, nullptr, &connection->Address))
			return connection->ID;
	}

	return -1;
}
