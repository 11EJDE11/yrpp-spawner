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

// How it goes:
//  1. A player whose wait times out doesn't remove anyone; it asks the game master to remove the
//     player it can't hear (KICK_REQUEST). The master's own timeouts go straight to step 2.
//  2. The master tells everyone (KICK_PREPARE). Each player replies with the last frame it executed
//     (KICK_STATE); nobody can execute further, as everyone is waiting on someone by then.
//  3. The cutoff is the furthest of those frames: someone executed the removed player's orders up
//     to it, and that can't be undone. The master names a player that got that far (KICK_COMMIT),
//     and that player sends the removed player's orders between the lowest of the frames and the
//     cutoff (KICK_EVENTS).
//  4. Each player makes its copy of the removed player's orders the agreed one - its own up to the
//     frame it executed, the sent ones up to the cutoff, nothing after - and then removes the
//     player. So everyone plays the frames up to the cutoff with the same orders.
// Every packet is passed on once by each player that gets it, so players who can't hear the master
// still take part, and packets may arrive in any order. A player that can't get the orders it is
// missing leaves the game rather than play on without them.
//
// When the one to remove is the master itself, the deputy (the connected player with the lowest
// house id after the master) takes its place for that decision, unless the master answers it: then
// it is the deputy that can't hear the master, and the master will remove the deputy. While others
// say they still hear the master, the deputy asks through every round before deciding, since a
// master that hears nobody sends but can't answer.
// A master (or deputy) that hears from nobody is the one cut off; it calls its decision off and
// removes everyone alone, as the engine would in time. It then sends nothing more, and the others
// remove it like a player gone silent. Its asking may still reach the others, so a player leaves
// only on a decision to remove it, not on being asked about, and a decision that stalls leaves the
// others to ask the deputy.
// Any other player that hears nobody asks the master to remove someone everyone else hears, and then
// can't answer: the master removes that player instead. Its asking still arrives once the others
// have removed it, and is ignored. When its asking comes to nothing it too removes everyone alone.

#include "KickAgreement.h"
#include "PacketBundling.h"

#include <Ext/Session/Body.h>
#include <Spawner/GlobalPacketExt.h>
#include <Spawner/Spawner.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <EventClass.h>
#include <HouseClass.h>
#include <IPXConnClass.h>
#include <IPXManagerClass.h>
#include <SessionClass.h>
#include <TheirSync.h>
#include <Unsorted.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <vector>

namespace
{
	using namespace KickAgreement;

	constexpr int MaxPlayers = SessionExt::MaxPlayers;
	constexpr int HistoryFrames = 600;           // how far back the executed orders are kept
	constexpr DWORD GatherTimeoutMs = 5000;      // the master's wait for everyone's last frame, each time it asks
	constexpr int GatherRounds = 3;              // how many times it asks before deciding without those silent
	constexpr DWORD RelayTimeoutMs = 8000;       // a player's wait for the orders it is missing
	constexpr int EventsPerPacket = 3;
	constexpr int MaxRelayPackets = 127;         // what Seq and Total can number
	constexpr DWORD HeardRecentlyMs = 3000;      // still hears a player: something arrived from it this recently
	constexpr int CancelledFrame = -2;           // KICK_COMMIT's Frame when a deputy calls its decision off
	constexpr DWORD AnnounceEveryMs = 1000;
	constexpr DWORD MaxHoldMs = 30000;           // the longest frames wait for a decision: every round and the relay, with time to spare
	static_assert(sizeof(EventClass) == sizeof(ExtGlobalPacketType{}.Kick.Events[0]));

	// The orders each house has had executed in the last HistoryFrames frames.
	std::vector<EventClass> History[MaxPlayers];

	// Removed houses, and the last frame their orders count on.
	int CutoffFrame[MaxPlayers] = { -1, -1, -1, -1, -1, -1, -1, -1 };

	// The removal being agreed on here, if any.
	struct DecisionState
	{
		bool Active = false;
		int ID = 0;
		int Master = -1;
		int Target = -1;
		int MyExecuted = -1;     // the last frame this player executed when it was asked
		int TimeoutsWhileActive = 0;
		DWORD HoldStarted = 0;   // when frames started waiting for this decision
		bool HoldGivenUp = false;

		// The master only.
		int Requester = -1;      // the player that asked for it, if anyone did
		bool Gathering = false;
		DWORD GatherStarted = 0;
		int GatherRound = 0;
		bool Expected[MaxPlayers] = {};
		bool HaveState[MaxPlayers] = {};
		int Executed[MaxPlayers] = {};
		bool HearsTarget[MaxPlayers] = {};

		bool Committed = false;
		int Cutoff = -1;
		int FirstFrame = -1;
		int Source = -1;
		DWORD CommittedAt = 0;

		// The orders sent by the source, by packet.
		int RelayTotal = -1;
		bool RelaySeen[MaxRelayPackets] = {};
		std::vector<EventClass> Relayed;
	} Decision;

	bool RemovingMe = false;   // the master decided to remove this player
	bool RemovalPending = false; // the master asked how far everyone got, to remove this player
	int PendingTimeouts = 0;   // timeouts since then
	bool LeaveGame = false;    // this player couldn't get the removed player's orders it was missing
	bool CutOff = false;       // nobody answered this player, and it removed everyone the engine's way
	bool RemoveEveryone = false; // that is to happen at the next Call_Back
	int AnnouncesLeft = 5;     // the host's repeats of its announcement as the game master
	DWORD LastAnnounce = 0;
	int NextDecisionID = 1;
	int NextRequestID = 1;
	int RequestTimeouts = 0;   // timeouts since this player asked the master

	// When this player last removed someone by agreement. Everyone else had been waiting about as
	// long, so their own waits may time out moments after it: those are not a reason to remove
	// anyone else.
	DWORD LastRemovalTick = 0;
	constexpr DWORD AfterRemovalGraceMs = 15000;

	std::vector<unsigned long long> Seen; // packets already handled and passed on
	std::vector<int> Cancelled;           // decisions called off, as Decision_Key gives them

	int Decision_Key(int master, int id)
	{
		return ((master & 0xFF) << 16) | (id & 0xFFFF);
	}

	void Note_Cancelled(int master, int id)
	{
		const int key = Decision_Key(master, id);
		if (std::find(Cancelled.begin(), Cancelled.end(), key) == Cancelled.end())
			Cancelled.push_back(key);
	}

	bool Is_Cancelled(int master, int id)
	{
		return std::find(Cancelled.begin(), Cancelled.end(), Decision_Key(master, id)) != Cancelled.end();
	}

	int Me()
	{
		return HouseClass::CurrentPlayer ? HouseClass::CurrentPlayer->ArrayIndex : -1;
	}

	// The game master, kept to a connected player: one that left or was removed the engine's way is
	// replaced here, the same on every machine.
	int Master()
	{
		SessionExt::Update_Master_After_Player_Removal();
		return SessionClass::Instance.MasterPlayerID();
	}

	// Who removes the master if it is the one that has gone: the connected player with the lowest
	// house id after it.
	int Deputy()
	{
		const int master = Master();
		int deputy = -1;
		auto& players = NodeNameType::Array;
		for (int i = 0; i < players.Count; i++)
		{
			const int house = players[i] ? players[i]->HouseIndex : -1;
			if (house >= 0 && house != master && (deputy == -1 || house < deputy))
				deputy = house;
		}
		return deputy;
	}

	// Whether anything has arrived from this house in the last moments.
	bool Heard_Recently(int house)
	{
		auto& players = NodeNameType::Array;
		for (int i = 1; i < players.Count; i++)
		{
			NodeNameType* node = players[i];
			if (node == nullptr || node->HouseIndex != house)
				continue;

			// The spawner numbers each player's address by its slot, from 1.
			const DWORD last = PacketBundling::LastReceived(static_cast<int>(node->Address.sin_addr.s_addr) - 1);
			return last != 0 && GetTickCount() - last < HeardRecentlyMs;
		}
		return false;
	}

	// Whether the house is still a player here: one removed is gone from the list.
	bool Connected(int house)
	{
		auto& players = NodeNameType::Array;
		for (int i = 0; i < players.Count; i++)
		{
			if (players[i] != nullptr && players[i]->HouseIndex == house)
				return true;
		}
		return false;
	}

	bool Heard_Anyone()
	{
		auto& players = NodeNameType::Array;
		for (int i = 1; i < players.Count; i++)
		{
			if (players[i] != nullptr && Heard_Recently(players[i]->HouseIndex))
				return true;
		}
		return false;
	}

	int Connection_Index(int house)
	{
		auto& ipx = IPXManagerClass::Instance;
		for (int i = 0; i < static_cast<int>(ipx.NumConnections); i++)
		{
			if (ipx.Connection[i] != nullptr && ipx.Connection[i]->ID == house)
				return i;
		}
		return -1;
	}

	bool Valid_House(int house)
	{
		return house >= 0 && house < MaxPlayers;
	}

	// Kick_Player_Now (0x64B240), as network_64B410 calls it: drops the connection, shifts the
	// per-connection sync data along and queues the REMOVEPLAYER event.
	void Kick_Player_Now(int connection_index)
	{
		using KickFn = int(__fastcall*)(void* net, int index, TheirSync* their, int error);
		reinterpret_cast<KickFn>(0x64B240)(&IPXManagerClass::Instance, connection_index, TheirSync::Array, 1);
	}

	template <typename Queue, typename Predicate>
	int Remove_From(Queue& list, Predicate drop)
	{
		std::vector<EventClass> keep;
		keep.reserve(list.Count);

		int dropped = 0;
		for (int i = 0; i < list.Count; i++)
		{
			if (drop(list[i]))
				dropped++;
			else
				keep.push_back(list[i]);
		}

		if (dropped == 0)
			return 0;

		list.Init();
		for (const auto& event : keep)
			list.Add(event);

		return dropped;
	}

	template <typename Predicate>
	int Remove_From_DoList(Predicate drop)
	{
		return Remove_From(EventClass::DoList, drop);
	}

	// A player that is cut off removes the rest by itself. Those who still hear it would run its
	// REMOVEPLAYER orders too, and remove a player they hear, so the orders are taken back out of
	// what it sends and run here alone, on the frame Send_Packets would have given them (0x649E03).
	// Once it hears someone again, its removals are agreed ones and go out like everyone's.
	void Keep_Removals_Local()
	{
		if (!CutOff || Heard_Anyone())
			return;

		const int frame = Unsorted::CurrentFrame + Game::Network::MaxAhead;
		const char me = static_cast<char>(Me());
		Remove_From(EventClass::OutList, [frame, me](const EventClass& queued)
		{
			if (queued.Type != EventType::RemovePlayer)
				return false;

			EventClass event(queued);
			event.Frame = frame;
			event.HouseIndex = me;
			event.IsExecuted = false;
			EventClass::DoList.Add(event);
			Debug::Log("[KickAgreement] Cut off: removing house %d here only, at frame %d.\n",
				event.RemovePlayer.HouseID, frame);
			return true;
		});
	}

	// Nobody answers this player, so it is the one cut off. Without the others' orders its frames stop
	// matching theirs, and those who still hear it would run its removals, so it removes everyone alone
	// and sends them nothing more.
	void Remove_Everyone()
	{
		CutOff = true;
		for (int index = static_cast<int>(IPXManagerClass::Instance.NumConnections) - 1; index >= 0; index--)
			Kick_Player_Now(index);
		Keep_Removals_Local();
	}

	unsigned long long Key(const ExtGlobalPacketType& packet)
	{
		const auto& kick = packet.Kick;
		return (static_cast<unsigned long long>(packet.Command & 0xFF) << 48)
			| (static_cast<unsigned long long>(kick.DecisionID & 0xFFFF) << 32)
			| (static_cast<unsigned long long>(kick.MasterID & 0xFF) << 24)
			| (static_cast<unsigned long long>(kick.FromID & 0xFF) << 16)
			| (static_cast<unsigned long long>(kick.TargetID & 0xFF) << 8)
			| static_cast<unsigned long long>(kick.Seq & 0xFF);
	}

	void Broadcast(const ExtGlobalPacketType& packet)
	{
		auto& players = NodeNameType::Array;
		for (int i = 1; i < players.Count; i++)
		{
			NodeNameType* node = players[i];
			if (node == nullptr)
				continue;

			auto copy = packet;
			IPXManagerClass::Instance.Send_Global_Message(&copy, sizeof(copy), 1,
				reinterpret_cast<IPXAddressClass*>(&node->Address), 0, 0);
		}
		IPXManagerClass::Instance.Service();
	}

	// Sends a packet of our own, marked as seen so it isn't handled again when it comes back.
	void Send(ExtGlobalPacketType& packet)
	{
		packet.Kick.FromID = static_cast<char>(Me());
		Seen.push_back(Key(packet));
		Broadcast(packet);
	}

	ExtGlobalPacketType Make(int command)
	{
		ExtGlobalPacketType packet {};
		packet.Command = command;
		packet.Kick.DecisionID = Decision.ID;
		packet.Kick.MasterID = static_cast<char>(Decision.Master);
		packet.Kick.TargetID = static_cast<char>(Decision.Target);
		return packet;
	}

	void Reset_Decision()
	{
		Decision = DecisionState {};
		RequestTimeouts = 0;
	}

	// A decision by the deputy, to remove the master.
	bool By_Deputy(int master_id)
	{
		return master_id != Master();
	}

	// Makes this player's copy of the removed player's orders the agreed one, then removes it.
	void Apply()
	{
		const int target = Decision.Target;
		const int cutoff = Decision.Cutoff;
		const int executed = Unsorted::CurrentFrame - 1;

		if (executed != Decision.MyExecuted)
		{
			Debug::Log("[KickAgreement] Executed frame %d since reporting %d - the removal may not be in sync.\n",
				executed, Decision.MyExecuted);
		}

		// Orders after the last frame this player executed are replaced by the sent ones, if it is
		// behind; a player that got to the cutoff only drops those after it.
		const int keep_through = (std::min)(executed, cutoff);
		const int dropped = Remove_From_DoList([target, keep_through](const EventClass& event)
		{
			const bool drop = event.HouseIndex == target && static_cast<int>(event.Frame) > keep_through;
			if (drop && event.Type != EventType::FrameInfo)
				Debug::Log("[KickAgreement]   dropped its order for frame %u (type %d)\n", event.Frame, static_cast<int>(event.Type));
			return drop;
		});

		int added = 0;
		for (const auto& relayed : Decision.Relayed)
		{
			EventClass event(relayed);
			const int frame = static_cast<int>(event.Frame);
			if (event.HouseIndex == target && frame > keep_through && frame <= cutoff)
			{
				event.IsExecuted = false;
				if (EventClass::DoList.Add(event))
				{
					added++;
					Debug::Log("[KickAgreement]   took its order for frame %d (type %d)\n", frame, static_cast<int>(event.Type));
				}
			}
		}

		LastRemovalTick = GetTickCount();
		RemovalPending = false; // whoever meant to remove this player has moved on, or is the one removed
		PendingTimeouts = 0;

		CutoffFrame[target] = cutoff;

		Debug::Log("[KickAgreement] Removing house %d: its orders count up to frame %d. This player executed "
			"frame %d, dropped %d of its orders and took %d sent ones.\n", target, cutoff, executed, dropped, added);

		const int index = Connection_Index(target);
		if (index >= 0)
			Kick_Player_Now(index);

		SessionExt::Update_Master_After_Player_Removal();
		Reset_Decision();
	}

	void Try_Apply()
	{
		if (!Decision.Committed)
			return;

		const bool needs_orders = Decision.MyExecuted < Decision.Cutoff && Decision.Source != Me();
		if (needs_orders)
		{
			if (Decision.RelayTotal < 0)
				return;

			if (Decision.RelayTotal == 0)
			{
				Debug::Log("[KickAgreement] House %d had too many orders to send; leaving the game rather than play "
					"on without them.\n", Decision.Target);
				LeaveGame = true;
				Reset_Decision();
				return;
			}

			for (int i = 0; i < Decision.RelayTotal; i++)
			{
				if (!Decision.RelaySeen[i])
					return;
			}
		}

		Apply();
	}

	// The removed player's orders from first to last, as this player executed them.
	void Send_Orders(int first, int last)
	{
		std::vector<const EventClass*> orders;
		for (const auto& event : History[Decision.Target])
		{
			const int frame = static_cast<int>(event.Frame);
			if (frame >= first && frame <= last)
				orders.push_back(&event);
		}

		const int total = (std::max)(1, static_cast<int>((orders.size() + EventsPerPacket - 1) / EventsPerPacket));
		if (total > MaxRelayPackets)
		{
			// Part of them would put those who get them out of sync with those who don't need them.
			// Total 0 tells them so; they leave instead.
			Debug::Log("[KickAgreement] %d orders to send is too many (at most %d).\n",
				static_cast<int>(orders.size()), MaxRelayPackets * EventsPerPacket);

			auto packet = Make(EXT_NET_KICK_EVENTS);
			packet.Kick.Total = 0;
			packet.Kick.Frame = last;
			packet.Kick.FirstFrame = first;
			Send(packet);
			return;
		}

		const int packets = total;
		Debug::Log("[KickAgreement] Sending house %d's %d orders for frames %d to %d in %d packets.\n",
			Decision.Target, static_cast<int>(orders.size()), first, last, packets);

		for (int seq = 0; seq < packets; seq++)
		{
			auto packet = Make(EXT_NET_KICK_EVENTS);
			packet.Kick.Seq = static_cast<char>(seq);
			packet.Kick.Total = static_cast<char>(packets);
			packet.Kick.Frame = last;
			packet.Kick.FirstFrame = first;

			int count = 0;
			for (size_t i = seq * EventsPerPacket; i < orders.size() && count < EventsPerPacket; i++, count++)
			{
				std::memcpy(packet.Kick.Events[count], orders[i], sizeof(EventClass));
				Debug::Log("[KickAgreement]   sending its order for frame %u (type %d)\n",
					orders[i]->Frame, static_cast<int>(orders[i]->Type));
			}

			packet.Kick.Count = static_cast<char>(count);
			Send(packet);
		}
	}

	void On_Commit(int cutoff, int first, int source)
	{
		Decision.Committed = true;
		Decision.Cutoff = cutoff;
		Decision.FirstFrame = first;
		Decision.Source = source;
		Decision.CommittedAt = GetTickCount();

		Debug::Log("[KickAgreement] Decision %d: house %d's orders count up to frame %d; house %d sends frames %d on. "
			"This player executed frame %d.\n", Decision.ID, Decision.Target, cutoff, source, first, Decision.MyExecuted);

		if (source == Me() && first <= cutoff)
			Send_Orders(first, cutoff);

		Try_Apply();
	}

	// Tells everyone this player's decision is off, which frees those holding for it and the player it named.
	void Call_Off()
	{
		auto packet = Make(EXT_NET_KICK_COMMIT);
		packet.Kick.Frame = CancelledFrame;
		Send(packet);
		Note_Cancelled(Decision.Master, Decision.ID);
		Reset_Decision();
	}

	// A deputy that finds the master is still there: the master will remove the deputy instead.
	void Cancel()
	{
		Debug::Log("[KickAgreement] Decision %d: the game master, house %d, is still there; calling the removal off.\n",
			Decision.ID, Decision.Target);
		Call_Off();
	}

	// The master: everyone it expects has said how far it got, or it has waited long enough.
	void Commit()
	{
		Decision.Gathering = false;

		int cutoff = -1;
		int lowest = INT_MAX;
		int source = -1;
		for (int house = 0; house < MaxPlayers; house++)
		{
			if (!Decision.HaveState[house])
				continue;

			lowest = (std::min)(lowest, Decision.Executed[house]);
			if (Decision.Executed[house] > cutoff)
			{
				cutoff = Decision.Executed[house];
				source = house;
			}
		}

		// The master sends the orders itself if it got as far as anyone.
		if (Decision.HaveState[Me()] && Decision.Executed[Me()] == cutoff)
			source = Me();

		auto packet = Make(EXT_NET_KICK_COMMIT);
		packet.Kick.Frame = cutoff;
		packet.Kick.FirstFrame = lowest + 1;
		packet.Kick.SourceID = static_cast<char>(source);
		Send(packet);

		On_Commit(cutoff, lowest + 1, source);
	}

	bool Everyone_Answered()
	{
		for (int house = 0; house < MaxPlayers; house++)
		{
			if (Decision.Expected[house] && !Decision.HaveState[house])
				return false;
		}
		return true;
	}

	// Someone still hears the master the deputy means to remove. A master that is there answers
	// itself, which calls the decision off; one that hears nobody can't, so the deputy waits out its
	// asking and then removes it.
	bool Master_Still_Heard()
	{
		if (!By_Deputy(Decision.Master))
			return false;

		for (int house = 0; house < MaxPlayers; house++)
		{
			if (Decision.HaveState[house] && Decision.HearsTarget[house])
				return true;
		}
		return false;
	}

	void Check_Gather()
	{
		if (Everyone_Answered() && !Master_Still_Heard())
			Commit();
	}

	// The player that asked the master to remove another hasn't answered, while everyone who did still
	// hears the one it named: it is the one asking that hears nobody.
	bool Requester_Cut_Off()
	{
		const int requester = Decision.Requester;
		if (By_Deputy(Decision.Master) || !Valid_House(requester) || Decision.HaveState[requester])
			return false;

		for (int house = 0; house < MaxPlayers; house++)
		{
			if (Decision.HaveState[house] && !Decision.HearsTarget[house])
				return false;
		}
		return true;
	}

	// The master starts removing a player.
	void Start_Decision(int target, int requester = -1)
	{
		Reset_Decision();
		Decision.Active = true;
		Decision.ID = NextDecisionID++;
		Decision.Master = Me();
		Decision.Target = target;
		Decision.Requester = requester;
		Decision.MyExecuted = Unsorted::CurrentFrame - 1;
		Decision.Gathering = true;
		Decision.GatherStarted = GetTickCount();
		Decision.GatherRound = 0;

		auto& ipx = IPXManagerClass::Instance;
		for (int i = 0; i < static_cast<int>(ipx.NumConnections); i++)
		{
			const int house = ipx.Connection[i] ? ipx.Connection[i]->ID : -1;
			if (Valid_House(house) && house != target)
				Decision.Expected[house] = true;
		}

		Decision.HaveState[Me()] = true;
		Decision.Executed[Me()] = Decision.MyExecuted;
		Decision.HearsTarget[Me()] = Heard_Recently(target);

		Debug::Log(target == Master()
			? "[KickAgreement] Decision %d: the deputy is removing the game master, house %d (frame %d).\n"
			: "[KickAgreement] Decision %d: the game master is removing house %d (frame %d).\n",
			Decision.ID, target, Unsorted::CurrentFrame);

		auto packet = Make(EXT_NET_KICK_PREPARE);
		Send(packet);
		Check_Gather();
	}

	bool Is_Current(const ExtGlobalPacketType& packet)
	{
		return Decision.Active && packet.Kick.DecisionID == Decision.ID && packet.Kick.MasterID == Decision.Master;
	}

	// Whether this player takes part in the decision the packet belongs to, starting it from the packet
	// if it is the first of its packets to arrive: one passed on by someone else may overtake the
	// PREPARE. False if it is about this player, or this player is busy with another decision.
	bool Join(const ExtGlobalPacketType& packet)
	{
		const auto& kick = packet.Kick;
		const bool by_deputy = By_Deputy(kick.MasterID);

		// The master is heard by someone, so a deputy removing it is the one cut off: the master's
		// decision wins, and the deputy calls its own off.
		if (!by_deputy && Decision.Active && By_Deputy(Decision.Master) && !Decision.Committed)
		{
			if (Decision.Master == Me())
				Cancel();
			else
				Reset_Decision();
		}

		if (kick.TargetID == Me())
		{
			// A deputy that can't hear this master asks whether anyone does; On_Prepare answers.
			if (by_deputy && Me() == Master() && packet.Command == EXT_NET_KICK_PREPARE)
				return false;

			// Only the decision removes this player: a master that hears nobody asks too, but then
			// removes the player alone, and it is the master that has gone.
			if (packet.Command == EXT_NET_KICK_PREPARE)
			{
				if (!RemovingMe && !RemovalPending)
					Debug::Log("[KickAgreement] House %d means to remove this player.\n", kick.MasterID);

				RemovalPending = true;
				return false;
			}

			if (!RemovingMe)
				Debug::Log("[KickAgreement] House %d is removing this player.\n", kick.MasterID);

			RemovingMe = true;
			return false;
		}

		if (Is_Current(packet))
			return true;

		// A late packet of a decision called off.
		if (Is_Cancelled(kick.MasterID, kick.DecisionID))
			return false;

		// A late packet of a decision already applied here.
		if (CutoffFrame[kick.TargetID] >= 0)
			return false;

		// Its orders are being settled; a newer decision waits until this one is applied.
		if (Decision.Active && Decision.Committed)
			return false;

		// A deputy's decision doesn't replace the master's.
		if (by_deputy && Decision.Active && !By_Deputy(Decision.Master))
			return false;

		Reset_Decision();
		Decision.Active = true;
		Decision.ID = kick.DecisionID;
		Decision.Master = kick.MasterID;
		Decision.Target = kick.TargetID;
		Decision.MyExecuted = Unsorted::CurrentFrame - 1;

		Debug::Log("[KickAgreement] Decision %d: house %d is removing house %d; this player executed frame %d.\n",
			Decision.ID, Decision.Master, Decision.Target, Decision.MyExecuted);
		return true;
	}

	void On_Prepare(const ExtGlobalPacketType& packet)
	{
		const auto& kick = packet.Kick;

		// A late packet of one removed since: a player that hears nobody keeps asking after the rest
		// have removed it.
		if (!Connected(kick.MasterID))
			return;

		if (kick.TargetID == Me() && Me() == Master() && By_Deputy(kick.MasterID))
		{
			// This master is still here: telling the deputy so calls its decision off.
			Debug::Log("[KickAgreement] Decision %d: house %d can't hear this game master; telling it this one is here.\n",
				kick.DecisionID, kick.MasterID);

			ExtGlobalPacketType reply {};
			reply.Command = EXT_NET_KICK_STATE;
			reply.Kick.DecisionID = kick.DecisionID;
			reply.Kick.MasterID = kick.MasterID;
			reply.Kick.TargetID = kick.TargetID;
			reply.Kick.Seq = kick.Seq;
			reply.Kick.Count = 1;
			reply.Kick.Frame = Unsorted::CurrentFrame - 1;
			Send(reply);
			return;
		}

		if (!Join(packet))
			return;

		// The master asks again when it hasn't heard from everyone; each reply is a new packet (its
		// Seq is the round) so that it is passed on again too. Count says whether this player still
		// hears the one being removed, which calls a deputy's removal of the master off.
		auto reply = Make(EXT_NET_KICK_STATE);
		reply.Kick.Frame = Decision.MyExecuted;
		reply.Kick.Seq = kick.Seq;
		reply.Kick.Count = Heard_Recently(Decision.Target) ? 1 : 0;
		Send(reply);
	}

	void On_Events(const ExtGlobalPacketType& packet)
	{
		const auto& kick = packet.Kick;
		if (kick.Total < 0 || kick.Total > MaxRelayPackets)
			return;

		if (kick.Total == 0)
		{
			Decision.RelayTotal = 0;
			Try_Apply();
			return;
		}

		if (kick.Seq < 0 || kick.Seq >= kick.Total || Decision.RelaySeen[kick.Seq])
			return;

		Decision.RelaySeen[kick.Seq] = true;
		Decision.RelayTotal = kick.Total;

		for (int i = 0; i < kick.Count && i < EventsPerPacket; i++)
			Decision.Relayed.push_back(*reinterpret_cast<const EventClass*>(kick.Events[i]));

		Try_Apply();
	}

	bool Applicable()
	{
		return SessionClass::IsMultiplayer()
			&& HouseClass::CurrentPlayer != nullptr
			&& Valid_House(Master())
			&& IPXManagerClass::Instance.NumConnections >= 2; // three players or more
	}
}

TimeoutAction KickAgreement::On_Timeout(int connection_index)
{
	if (LastRemovalTick != 0 && GetTickCount() - LastRemovalTick < AfterRemovalGraceMs)
	{
		Debug::Log("[KickAgreement] Timed out just after a removal; waiting on.\n");
		return TimeoutAction::KeepWaiting;
	}

	if (!Applicable())
		return TimeoutAction::Vanilla;

	if (RemoveEveryone)
		return TimeoutAction::KeepWaiting;

	if (CutOff)
	{
		if (!Heard_Anyone())
			return TimeoutAction::Vanilla;
		CutOff = false;
	}

	if (RemovingMe || LeaveGame)
	{
		Debug::Log(RemovingMe ? "[KickAgreement] Leaving the game: the game master is removing this player.\n"
			: "[KickAgreement] Leaving the game: couldn't get the removed player's orders.\n");
		return TimeoutAction::GiveUp;
	}

	auto& ipx = IPXManagerClass::Instance;
	const int target = (connection_index >= 0 && connection_index < static_cast<int>(ipx.NumConnections)
		&& ipx.Connection[connection_index]) ? ipx.Connection[connection_index]->ID : -1;

	if (!Valid_House(target))
		return TimeoutAction::Vanilla;

	if (RemovalPending)
	{
		// The master meant to remove this player and never decided: it is the master that has gone,
		// and this player asks about it like the rest.
		if (++PendingTimeouts < 2)
			return TimeoutAction::KeepWaiting;

		Debug::Log("[KickAgreement] The removal of this player never came.\n");
		RemovalPending = false;
		PendingTimeouts = 0;
	}

	if (Decision.Active)
	{
		// The master's own decision always completes: it decides without those who don't answer, and
		// leaves if it can't get the orders it needs.
		if (Decision.Master == Me())
			return TimeoutAction::KeepWaiting;

		// A removal that should take a second has taken a whole timeout more: the one deciding has
		// gone, and this player asks about it like any other.
		if (++Decision.TimeoutsWhileActive < 2)
			return TimeoutAction::KeepWaiting;

		Debug::Log("[KickAgreement] Decision %d didn't complete.\n", Decision.ID);
		Reset_Decision();
	}

	// The master decides, or its deputy when it is the master this player can't hear.
	const int decider = target == Master() ? Deputy() : Master();
	if (decider == Me())
	{
		Start_Decision(target);
		return TimeoutAction::KeepWaiting;
	}

	if (RequestTimeouts++ == 0)
	{
		Debug::Log("[KickAgreement] Asking house %d to remove house %d.\n", decider, target);

		ExtGlobalPacketType packet {};
		packet.Command = EXT_NET_KICK_REQUEST;
		packet.Kick.DecisionID = NextRequestID++;
		packet.Kick.MasterID = static_cast<char>(decider);
		packet.Kick.TargetID = static_cast<char>(target);
		Send(packet);
		return TimeoutAction::KeepWaiting;
	}

	if (RequestTimeouts >= 3)
	{
		RequestTimeouts = 0;

		// The wait is part way through the connections; they are removed in Call_Back instead.
		if (!Heard_Anyone())
		{
			Debug::Log("[KickAgreement] House %d didn't act and nobody is heard; removing everyone the engine's way.\n", decider);
			RemoveEveryone = true;
			return TimeoutAction::KeepWaiting;
		}

		Debug::Log("[KickAgreement] House %d didn't act; removing house %d the engine's way.\n", decider, target);
		return TimeoutAction::Vanilla;
	}

	return TimeoutAction::KeepWaiting;
}

bool KickAgreement::Handle_Global_Packet(const ExtGlobalPacketType* packet, const IPXAddressClass& address)
{
	if (packet == nullptr || packet->Command < EXT_NET_KICK_REQUEST || packet->Command > EXT_NET_KICK_EVENTS)
		return false;

	// Only a player still in the game here takes part, or passes packets on: one from anyone else,
	// or from a player removed here, is dropped. Whose decision a packet carries is not checked
	// against who is master: players can briefly disagree on that, and one left out would split.
	if (SessionExt::Sender_House(address) < 0)
		return true;

	// Each packet is handled and passed on once.
	const auto key = Key(*packet);
	if (std::find(Seen.begin(), Seen.end(), key) != Seen.end())
		return true;

	Seen.push_back(key);
	Broadcast(*packet);

	if (!Applicable())
		return true;

	const auto& kick = packet->Kick;

	switch (packet->Command)
	{
	case EXT_NET_KICK_REQUEST:
		if (!Decision.Active && Valid_House(kick.TargetID) && Valid_House(kick.FromID) && kick.TargetID != Me()
			&& (kick.TargetID == Master() ? Deputy() : Master()) == Me()
			&& Connected(kick.TargetID) && Connected(kick.FromID)
			&& CutoffFrame[kick.TargetID] < 0 && CutoffFrame[kick.FromID] < 0)
		{
			Debug::Log("[KickAgreement] House %d asks to remove house %d.\n", kick.FromID, kick.TargetID);
			Start_Decision(kick.TargetID, kick.FromID);
		}
		break;

	case EXT_NET_KICK_PREPARE:
		if (Me() != kick.MasterID && Valid_House(kick.TargetID))
			On_Prepare(*packet);
		break;

	case EXT_NET_KICK_STATE:
		if (Is_Current(*packet) && Decision.Gathering && Valid_House(kick.FromID) && !Decision.HaveState[kick.FromID])
		{
			// The master itself, answering a deputy that can't hear it.
			if (kick.FromID == Decision.Target)
			{
				if (By_Deputy(Decision.Master))
					Cancel();
				break;
			}

			Decision.HaveState[kick.FromID] = true;
			Decision.Executed[kick.FromID] = kick.Frame;
			Decision.HearsTarget[kick.FromID] = kick.Count != 0;
			Check_Gather();
		}
		break;

	case EXT_NET_KICK_COMMIT:
		if (kick.Frame == CancelledFrame)
		{
			Note_Cancelled(kick.MasterID, kick.DecisionID);
			if (kick.TargetID == Me())
				RemovalPending = false;
			if (Is_Current(*packet))
			{
				Debug::Log("[KickAgreement] Decision %d was called off.\n", kick.DecisionID);
				Reset_Decision();
			}
			break;
		}

		if (Me() != kick.MasterID && Valid_House(kick.TargetID) && Join(*packet) && !Decision.Committed)
			On_Commit(kick.Frame, kick.FirstFrame, kick.SourceID);
		break;

	case EXT_NET_KICK_EVENTS:
		if (Me() == kick.MasterID ? Is_Current(*packet) : (Valid_House(kick.TargetID) && Join(*packet)))
			On_Events(*packet);
		break;
	}

	return true;
}

void KickAgreement::Update()
{
	const DWORD now = GetTickCount();

	// The host's announcement before the load can arrive before a player listens, and that player
	// would take another for the master. It is sent again through the first seconds of the game.
	if (AnnouncesLeft > 0 && Unsorted::CurrentFrame > 0 && now - LastAnnounce >= AnnounceEveryMs
		&& SessionClass::IsMultiplayer() && IPXManagerClass::Instance.NumConnections > 0 && Spawner::GetConfig()->Host)
	{
		AnnouncesLeft--;
		LastAnnounce = now;
		SessionExt::Announce_Master();
	}

	if (RemoveEveryone)
	{
		RemoveEveryone = false;
		Remove_Everyone();
	}

	if (!Decision.Active)
		return;

	if (Decision.Gathering && now - Decision.GatherStarted > GatherTimeoutMs)
	{
		// Someone who executed further than the rest and isn't heard would be cut off from orders it
		// executed, so the master asks again; only those silent through every round are left out - by
		// then the others can't hear them either.
		const bool everyone = Everyone_Answered();
		if (++Decision.GatherRound < GatherRounds)
		{
			Debug::Log(everyone ? "[KickAgreement] Others still hear the game master, but it doesn't answer; asking again.\n"
				: "[KickAgreement] Not everyone said how far it got; asking again.\n");
			Decision.GatherStarted = now;
			auto packet = Make(EXT_NET_KICK_PREPARE);
			packet.Kick.Seq = static_cast<char>(Decision.GatherRound);
			Send(packet);
			return;
		}

		bool anyone = false;
		for (int house = 0; house < MaxPlayers; house++)
			anyone |= house != Me() && Decision.HaveState[house];

		if (!anyone)
		{
			// Nobody answered. Calling the decision off first frees those it reached, and the player it
			// named.
			Debug::Log("[KickAgreement] Nobody said how far it got; removing everyone the engine's way.\n");
			Call_Off();
			Remove_Everyone();
			return;
		}

		if (Requester_Cut_Off())
		{
			// Removing the one it named would remove a player everyone else hears; the master removes
			// the one that asked instead.
			const int requester = Decision.Requester;
			Debug::Log("[KickAgreement] House %d asked to remove house %d, but doesn't answer while everyone else "
				"hears house %d; removing house %d instead.\n", requester, Decision.Target, Decision.Target, requester);
			Call_Off();
			Start_Decision(requester);
			return;
		}

		Debug::Log(everyone ? "[KickAgreement] The game master never answered; removing it.\n"
			: "[KickAgreement] Not everyone said how far it got; deciding without them.\n");
		Commit();
		return;
	}

	if (Decision.Committed && now - Decision.CommittedAt > RelayTimeoutMs)
	{
		// Playing on with only part of them would be out of sync with everyone who has them all.
		Debug::Log("[KickAgreement] Only got %d of house %d's orders sent; leaving the game.\n",
			static_cast<int>(Decision.Relayed.size()), Decision.Target);
		LeaveGame = true;
		Reset_Decision();
	}
}

bool KickAgreement::Hold_Frames()
{
	// The cutoff is the furthest anyone said it got. One that ran on past what it said would run
	// orders of the player being removed that those who stopped may never get.
	if (!Decision.Active || Decision.MyExecuted < 0 || Decision.HoldGivenUp)
		return false;

	const DWORD now = GetTickCount();
	if (Decision.HoldStarted == 0)
	{
		Decision.HoldStarted = now;
		Debug::Log("[KickAgreement] Decision %d: waiting at frame %d until it is settled.\n",
			Decision.ID, Unsorted::CurrentFrame);
	}
	else if (now - Decision.HoldStarted > MaxHoldMs)
	{
		Debug::Log("[KickAgreement] Decision %d is still open after %u s; playing on.\n",
			Decision.ID, static_cast<unsigned>(MaxHoldMs / 1000));
		Decision.HoldGivenUp = true;
		return false;
	}

	return true;
}

void KickAgreement::Reset()
{
	for (auto& orders : History)
		orders.clear();
	std::fill(std::begin(CutoffFrame), std::end(CutoffFrame), -1);
	Reset_Decision();
	RemovingMe = false;
	RemovalPending = false;
	PendingTimeouts = 0;
	LeaveGame = false;
	CutOff = false;
	RemoveEveryone = false;
	LastRemovalTick = 0;
	Seen.clear();
	Cancelled.clear();
}

void KickAgreement::Before_Execute_DoList()
{
	const int frame = Unsorted::CurrentFrame;

	// The engine's removals while cut off were queued in the wait just done, before Send_Packets
	// sends them next frame.
	Keep_Removals_Local();

	bool any_cutoff = false;
	for (int house = 0; house < MaxPlayers; house++)
		any_cutoff |= CutoffFrame[house] >= 0;

	if (any_cutoff)
	{
		Remove_From_DoList([](const EventClass& event)
		{
			const int house = event.HouseIndex;
			return Valid_House(house) && CutoffFrame[house] >= 0 && static_cast<int>(event.Frame) > CutoffFrame[house];
		});
	}

	auto& list = EventClass::DoList;
	for (int i = 0; i < list.Count; i++)
	{
		const EventClass& event = list[i];
		const int house = event.HouseIndex;
		if (Valid_House(house) && !event.IsExecuted && static_cast<int>(event.Frame) == frame
			&& event.Type != EventType::FrameInfo)
		{
			History[house].push_back(event);
		}
	}

	for (auto& orders : History)
	{
		const auto old = std::find_if(orders.begin(), orders.end(),
			[frame](const EventClass& event) { return static_cast<int>(event.Frame) >= frame - HistoryFrames; });
		orders.erase(orders.begin(), old);
	}
}

/**
 *  network_64B410, the timed-out wait, just before it removes the connection it heard from
 *  last. [esp+10h] is that connection's index.
 */
static __declspec(naked) void Network_64B410_GiveUp()
{
	__asm
	{
		xor eax, eax
		pop edi
		pop ebx
		pop esi
		pop ebp
		add esp, 8
		retn
	}
}

DEFINE_HOOK(0x64B472, Network_64B410_AgreeOnKick, 0x6)
{
	enum { KeepWaiting = 0x64B480 };

	GET_STACK(int, connection_index, 0x10);

	switch (KickAgreement::On_Timeout(connection_index))
	{
	case TimeoutAction::KeepWaiting:
		return KeepWaiting;

	case TimeoutAction::GiveUp:
		return reinterpret_cast<DWORD>(&Network_64B410_GiveUp);

	default:
		return 0;
	}
}

// Call_Back, after Network_Call_Back has handled what came in. 0x48D0BE is `mov eax, Session`.
DEFINE_HOOK(0x48D0BE, CallBack_KickAgreement, 0x5)
{
	KickAgreement::Update();
	return 0;
}

// Wait_For_Players, with everyone's orders in hand (0x649BAC is `mov eax, [esp+14h]` / `xor edi, edi`).
// While a removal is settled the frame waits as it does for a late player, in the engine's stall
// path, which runs Call_Back and so the agreement. [esp+18h] is first_time. The stall path is entered
// at 0x6497E8, `mov ebx, [esp+20h]`: on the way here ebx was reused for a frame number, and the loop
// needs it to be the connection manager again.
DEFINE_HOOK(0x649BAC, WaitForPlayers_HoldForKickAgreement, 0x6)
{
	enum { Stall = 0x6497E8 };

	GET_STACK(int, first_time, 0x18);
	if (!first_time && KickAgreement::Hold_Frames())
		return Stall;

	return 0;
}

DEFINE_HOOK(0x64C380, Execute_DoList_KickAgreement, 0xA)
{
	KickAgreement::Before_Execute_DoList();
	return 0;
}
