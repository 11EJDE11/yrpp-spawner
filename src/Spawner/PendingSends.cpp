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

#include "PendingSends.h"

#include <Helpers/Macro.h>
#include <IPXManagerClass.h>
#include <IPXConnClass.h>
#include <CommBufferClass.h>
#include <SessionClass.h>
#include <Utilities/Debug.h>

#include <deque>
#include <vector>

namespace
{
	struct Pending
	{
		int House;
		std::vector<char> Data;
	};

	// Oldest first. A house's packets go out in the order they were made.
	std::deque<Pending> Waiting;

	int Deferred = 0;
	int DroppedNoAck = 0;
	DWORD LastLog = 0;

	constexpr size_t MaxWaiting = 512;

	bool Has_Room(const IPXConnClass* connection)
	{
		const auto* queue = connection->Queue;
		return queue && queue->SendCount < queue->MaxSend;
	}

	bool Has_Waiting(int house)
	{
		for (const auto& pending : Waiting)
		{
			if (pending.House == house)
				return true;
		}
		return false;
	}

	IPXConnClass* Connection_Of(int house)
	{
		auto& ipx = IPXManagerClass::Instance;
		for (int i = 0; i < static_cast<int>(ipx.NumConnections); i++)
		{
			if (ipx.Connection[i] && ipx.Connection[i]->ID == house)
				return ipx.Connection[i];
		}
		return nullptr;
	}

	void Log_Throttled(const char* what, int house)
	{
		const DWORD now = GetTickCount();
		if (now - LastLog < 1000)
			return;
		LastLog = now;
		Debug::Log("[PendingSends] %s house %d: %d waiting, %d sent late so far.\n",
			what, house, static_cast<int>(Waiting.size()), Deferred);
	}
}

void PendingSends::Reset()
{
	Waiting.clear();
	Deferred = 0;
	DroppedNoAck = 0;
	LastLog = 0;
}

void PendingSends::Flush()
{
	for (auto it = Waiting.begin(); it != Waiting.end();)
	{
		IPXConnClass* const connection = Connection_Of(it->House);
		if (!connection)
		{
			// That player has gone.
			it = Waiting.erase(it);
			continue;
		}

		// A house's packets go out in order: once one can't, the rest of its packets wait too.
		bool earlier_waits = false;
		for (auto prior = Waiting.begin(); prior != it; ++prior)
			earlier_waits |= prior->House == it->House;

		if (earlier_waits || !Has_Room(connection))
		{
			++it;
			continue;
		}

		if (connection->Send_Packet(it->Data.data(), static_cast<int>(it->Data.size()), 1, 0))
		{
			++Deferred;
			it = Waiting.erase(it);
		}
		else
		{
			++it;
		}
	}
}

void PendingSends::GetStats(int& deferred, int& waiting, int& droppedNoAck)
{
	deferred = Deferred;
	waiting = static_cast<int>(Waiting.size());
	droppedNoAck = DroppedNoAck;
}

// IPXManagerClass::Send_Private_Message (0x5414C0) to every connection, outside the packet-router
// mode: 0x54153A is where it checks every send queue for room before sending to any. The bytes here
// include a short relative jump, so the hook never returns to them: it leaves through the function's
// own exits, 0x54158A (sent, returns 1) or 0x5415D6 (counts an overflow, returns 0).
DEFINE_HOOK(0x54153A, IPXManagerClass_SendToAll_PerConnection, 0x6)
{
	enum { Sent = 0x54158A };

	GET(IPXManagerClass*, pThis, ESI);
	GET_STACK(void*, buf, 0x14);
	GET_STACK(int, length, 0x18);
	GET_STACK(int, ack, 0x1C);

	PendingSends::Flush();

	bool any_refused = false;
	for (int i = 0; i < static_cast<int>(pThis->NumConnections); i++)
	{
		IPXConnClass* const connection = pThis->Connection[i];
		if (!connection)
			continue;

		const int house = connection->ID;
		if (!Has_Waiting(house) && Has_Room(connection))
		{
			connection->Send_Packet(buf, length, ack, 0);
			continue;
		}

		any_refused = true;
		if (!ack)
		{
			// Unreliable anyway: the next one supersedes it.
			++DroppedNoAck;
			continue;
		}

		if (Waiting.size() >= MaxWaiting)
		{
			Log_Throttled("No room left to keep orders for", house);
			continue;
		}

		Pending pending;
		pending.House = house;
		pending.Data.assign(static_cast<const char*>(buf), static_cast<const char*>(buf) + length);
		Waiting.push_back(std::move(pending));
		Log_Throttled("Send queue full; holding orders for", house);
	}

	if (any_refused)
		++pThis->SendOverflows;

	return Sent;
}

// Each IPXManagerClass::Service, which the game loop and the wait for players both run.
DEFINE_HOOK(0x541820, IPXManagerClass_Service_PendingSends, 0x6)
{
	PendingSends::Flush();
	return 0;
}
