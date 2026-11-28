/**
*  yrpp-spawner
*
*  Copyright(C) 2023-present CnCNet
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

/**
*  PendingSends - no player's orders are lost for want of room to one peer.
*
*  The engine sends each packet of orders to every connection at once (IPXManagerClass,
*  0x5414C0), and when any one connection's send queue is full it sends it to none of them.
*  Send_Packets (0x649CA0) doesn't look at the result: the orders already run here and count in
*  the FRAMEINFO's command total, so every peer waits for orders that never come, and the game
*  stalls with every player still heard. Here a packet goes to each connection with room, and one
*  that needs an acknowledgement waits for the others, in order, until there is room.
*/

#pragma once

class PendingSends
{
public:
	static void Reset();

	// Sends what is waiting to connections that now have room. Each IPXManagerClass::Service.
	static void Flush();

	// Packets sent late, waiting now, refused a connection with no room (with no acknowledgement
	// needed, so dropped for that connection alone), and order packets dropped because the waiting
	// list was full.
	static void GetStats(int& deferred, int& waiting, int& droppedNoAck, int& droppedFull);
};
