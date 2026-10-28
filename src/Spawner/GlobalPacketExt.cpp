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

#include "GlobalPacketExt.h"
#include "KickAgreement.h"

#include <Ext/Session/Body.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <SessionClass.h>

/**
 *  Receive dispatch. Network_Call_Back routes received global packets through a
 *  switch on GPacket.Command; commands outside the engine's range (ours are
 *  0xE0+) fall to the default case at 0x48DAC4, which would call
 *  Process_Global_Packet. Intercept there: if it is one of ours, handle it and
 *  skip the engine's default handling; otherwise let it run.
 *
 *  0x48DAC4 is `mov edx, offset GAddress` (5 bytes, absolute), so `return 0`
 *  safely restores it and continues into the original mov/call.
 */
DEFINE_HOOK(0x48DAC4, NetworkCallBack_ExtGlobalPacket, 0x5)
{
	enum { SkipDefaultHandler = 0x48DAD3 }; // continue past Process_Global_Packet

	auto* const packet = reinterpret_cast<ExtGlobalPacketType*>(&SessionClass::GlobalReceivePacket);
	const auto& address = SessionClass::GlobalReceiveAddress;

	if (packet->Command == EXT_NET_HOST_ANNOUNCE)
	{
		// Sent by the host at game start, so MasterPlayerID is the same everywhere. Only the host
		// itself announces itself.
		const int house = packet->Heartbeat.HouseID;
		if (house >= 0 && house < SessionExt::MaxPlayers && SessionExt::Sender_House(address) == house
			&& SessionClass::Instance.MasterPlayerID() != house)
		{
			Debug::Log("[Spawner] House %d is the game master.\n", house);
			SessionExt::Set_Master(house);
		}
		return SkipDefaultHandler;
	}

	if (KickAgreement::Handle_Global_Packet(packet, address))
		return SkipDefaultHandler;

	return 0; // not one of ours: run the engine's Process_Global_Packet
}
