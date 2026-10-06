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

#include "FastRetransmit.h"
#include "PacketRedundancy.h"
#include "NetDiagnostics.h"

#include <windows.h>
#include <vector>

#include <Helpers/Macro.h>
#include <GeneralDefinitions.h>
#include <IPXManagerClass.h>
#include <ConnectionClass.h>
#include <IPXConnClass.h>
#include <Utilities/Debug.h>

bool FastRetransmit::Enabled = true;
bool FastRetransmit::Backoff = true;

namespace
{
	struct PacketTimer
	{
		bool initialized;
		int packetId;
		int firstTime;
		int lastTime;
		int sendCount;
		int baseTicks;
		int delayTicks;
	};

	// Jacobson/Karels in fixed point (srtt * 8, rttvar * 4). Round trips are only
	// a few ticks, so unscaled integer smoothing would truncate every increase.
	struct PeerEstimator
	{
		const ConnectionClass* connection;
		bool initialized;
		// Set while the estimate comes from an ambiguous (retransmitted)
		// acknowledgement; the first clean sample replaces it outright.
		bool provisional;
		int scaledSrtt;   // srtt * 8
		int scaledRttvar; // rttvar * 4
		int rto;
		int cleanSamples;
		// Fast EWMA of the delivery delay, scaled by 4 (alpha = 1/4).
		int scaledDelivery;
		int deliverySamples;
		// Physical queue slots do not move when another entry is removed.
		const SendQueueType* queueEntries;
		std::vector<PacketTimer> timers;
		// Limits backoff to once per round rather than once per overdue entry.
		bool backedOff;
		int lastBackoffTime;
		int backoffs;
		// Throttling for the diagnostic log.
		int loggedRto;
		DWORD lastLogTick;
	};

	PeerEstimator Peers[FastRetransmit::MaxPeers] = {};

	int ClampTicks(int value)
	{
		if (value < FastRetransmit::MinTicks)
			return FastRetransmit::MinTicks;
		if (value > FastRetransmit::MaxTicks)
			return FastRetransmit::MaxTicks;
		return value;
	}

	// How often the RetryDelta ceiling applied, and by how much.
	int g_vanillaCaps = 0;
	int g_worstOvershoot = 0;
	int g_lastUncapped = 0;

	// Last clamped value per connection, so only distinct values are counted.
	const ConnectionClass* g_capConnection[8] = {};
	int g_capLast[8] = {};

	int& LastCapFor(const ConnectionClass* connection)
	{
		for (int k = 0; k < 8; ++k)
			if (g_capConnection[k] == connection)
				return g_capLast[k];
		for (int k = 0; k < 8; ++k)
			if (!g_capConnection[k])
			{
				g_capConnection[k] = connection;
				return g_capLast[k];
			}
		g_capConnection[0] = connection;
		g_capLast[0] = 0;
		return g_capLast[0];
	}

	// Never retransmit later than the engine's RetryDelta. Waiting longer feeds
	// into the response time that RetryDelta is derived from, so both would
	// ratchet up together.
	int CapToVanilla(const ConnectionClass* connection, int ticks)
	{
		if (!connection || ticks <= 0)
			return ticks;

		const int vanilla = static_cast<int>(connection->RetryDelta);
		if (vanilla <= 0 || ticks <= vanilla)
			return ticks;

		// Count distinct clamped values; this runs on every timer check.
		int& last = LastCapFor(connection);
		if (ticks != last)
			++g_vanillaCaps;
		last = ticks;

		const int overshoot = ticks - vanilla;
		if (overshoot > g_worstOvershoot)
			g_worstOvershoot = overshoot;
		g_lastUncapped = ticks;
		return vanilla;
	}

	int PrivateConnectionCount()
	{
		int nconn = static_cast<int>(IPXManagerClass::Instance.NumConnections);
		const int arraySize = sizeof(IPXManagerClass::Instance.Connection) / sizeof(IPXManagerClass::Instance.Connection[0]);
		if (nconn < 0) nconn = 0;
		if (nconn > arraySize) nconn = arraySize;
		return nconn;
	}

	// Only private connections are estimated; the global channels keep RetryDelta.
	bool IsLiveConnection(const ConnectionClass* connection)
	{
		if (!connection)
			return false;

		const int nconn = PrivateConnectionCount();
		for (int i = 0; i < nconn; ++i)
			if (IPXManagerClass::Instance.Connection[i] == connection)
				return true;
		return false;
	}

	void PruneDeadSlots()
	{
		for (int i = 0; i < FastRetransmit::MaxPeers; ++i)
			if (Peers[i].connection && !IsLiveConnection(Peers[i].connection))
				Peers[i] = PeerEstimator {};
	}

	PeerEstimator* FindSlot(const ConnectionClass* connection)
	{
		if (!connection)
			return nullptr;

		for (int i = 0; i < FastRetransmit::MaxPeers; ++i)
			if (Peers[i].connection == connection)
				return &Peers[i];
		return nullptr;
	}

	// MaxPeers exceeds the engine's connection count, so there is always a free slot.
	PeerEstimator* FindOrCreateSlot(const ConnectionClass* connection)
	{
		if (!connection)
			return nullptr;

		PeerEstimator* freeSlot = nullptr;
		for (int i = 0; i < FastRetransmit::MaxPeers; ++i)
		{
			if (Peers[i].connection == connection)
				return &Peers[i];
			if (!Peers[i].connection && !freeSlot)
				freeSlot = &Peers[i];
		}

		if (!freeSlot)
			return nullptr;

		*freeSlot = PeerEstimator {};
		freeSlot->connection = connection;
		return freeSlot;
	}

	void LogEstimate(PeerEstimator* peer, int slot)
	{
		const DWORD now = GetTickCount();
		if (peer->rto == peer->loggedRto)
			return;
		if (peer->lastLogTick != 0 && (now - peer->lastLogTick) < 1000)
			return;

		peer->loggedRto = peer->rto;
		peer->lastLogTick = now;

	}

	PacketTimer* TimerFor(PeerEstimator* peer, const ConnectionClass* connection,
		const SendQueueType* entry, bool create)
	{
		if (!peer || !peer->initialized || !entry || !entry->Buffer
			|| entry->Buffer->Code != ConnectionEnum::PACKET_DATA_ACK)
			return nullptr;

		const auto* queue = connection->Queue;
		if (!queue || !queue->SendQueue || queue->MaxSend <= 0)
			return nullptr;
		const auto index = entry - queue->SendQueue;
		if (index < 0 || index >= queue->MaxSend)
			return nullptr;

		if (peer->queueEntries != queue->SendQueue
			|| peer->timers.size() != static_cast<size_t>(queue->MaxSend))
		{
			if (!create)
				return nullptr;
			peer->timers.assign(queue->MaxSend, PacketTimer {});
			peer->queueEntries = queue->SendQueue;
		}
		return &peer->timers[index];
	}

	bool Matches(const PacketTimer* timer, const SendQueueType* entry)
	{
		return timer && timer->initialized && entry->SendCount > 0
			&& timer->packetId == entry->Buffer->PacketID
			&& timer->firstTime == entry->FirstTime;
	}

	void ArmTimer(PacketTimer* timer, const ConnectionClass* connection, int sendCount, int lastTime)
	{
		int prior = FastRetransmit::Backoff ? sendCount - 1 : 0;
		if (prior < 0) prior = 0;
		if (prior > FastRetransmit::BackoffCap) prior = FastRetransmit::BackoffCap;
		int delay = timer->baseTicks
			+ (timer->baseTicks * prior * FastRetransmit::BackoffStepHalves) / 2;
		delay = ClampTicks(delay);

		// Capture the timeout cap too, so later Set_Timing calls don't move an armed deadline.
		const unsigned int timeout = connection->Timeout;
		if (timeout != 0xFFFFFFFFu && timeout > 0)
		{
			int cap = static_cast<int>(timeout / 2);
			if (cap < timer->baseTicks) cap = timer->baseTicks;
			if (delay > cap) delay = cap;
		}
		timer->delayTicks = CapToVanilla(connection, delay);
		timer->sendCount = sendCount;
		timer->lastTime = lastTime;
	}

	void CaptureTimer(PacketTimer* timer, const SendQueueType* entry, int baseTicks, int firstTime)
	{
		*timer = PacketTimer {};
		timer->initialized = true;
		timer->packetId = entry->Buffer->PacketID;
		timer->firstTime = firstTime;
		timer->baseTicks = baseTicks;
	}

	void ShortenPendingTimers(PeerEstimator* peer)
	{
		const auto* connection = peer->connection;
		const auto* queue = connection->Queue;
		if (!queue || peer->queueEntries != queue->SendQueue
			|| peer->timers.size() != static_cast<size_t>(queue->MaxSend))
			return;

		for (size_t i = 0; i < peer->timers.size(); ++i)
		{
			auto& timer = peer->timers[i];
			const auto* entry = &queue->SendQueue[i];
			if (!(entry->Flags & COMMQUEUE_IS_ACTIVE) || (entry->Flags & COMMQUEUE_IS_READ)
				|| !Matches(&timer, entry) || timer.sendCount != entry->SendCount
				|| timer.lastTime != entry->LastTime || timer.baseTicks <= peer->rto)
				continue;

			// A clean sample replaces a base captured during earlier loss.
			const int before = timer.delayTicks;
			timer.baseTicks = peer->rto;
			ArmTimer(&timer, connection, timer.sendCount, timer.lastTime);
			if (timer.delayTicks > before)
				timer.delayTicks = before;
		}
	}

	void Recompute(PeerEstimator* peer)
	{
		// rto = srtt + max(4 * rttvar, margin); scaledRttvar already is 4*rttvar.
		int margin = peer->scaledRttvar;
		if (margin < FastRetransmit::MarginTicks)
			margin = FastRetransmit::MarginTicks;
		peer->rto = ClampTicks((peer->scaledSrtt >> 3) + margin);
		LogEstimate(peer, static_cast<int>(peer - Peers));
	}

	void AddSample(PeerEstimator* peer, int delayTicks)
	{
		if (!peer->initialized)
		{
			peer->initialized = true;
			peer->scaledSrtt = delayTicks * 8;
			peer->scaledRttvar = (delayTicks > 1 ? (delayTicks + 1) / 2 : 1) * 4;
		}
		else
		{
			int err = delayTicks - (peer->scaledSrtt >> 3);
			peer->scaledSrtt += err; // srtt += err/8

			if (err < 0)
				err = -err;
			peer->scaledRttvar += err - (peer->scaledRttvar >> 2); // rttvar += (|err| - rttvar)/4
			if (peer->scaledRttvar < 4)
				peer->scaledRttvar = 4; // rttvar floor of one tick
		}

		Recompute(peer);
	}
}

void FastRetransmit::Reset()
{
	for (int i = 0; i < MaxPeers; ++i)
		Peers[i] = PeerEstimator {};
}

void FastRetransmit::SampleRTT(const ConnectionClass* connection, int delayTicks, int sendCount)
{
	if (!Enabled)
		return;

	if (sendCount <= 0)
		return;
	if (delayTicks < 0 || delayTicks > MaxTicks)
		return;
	if (!IsLiveConnection(connection))
		return;

	PruneDeadSlots();
	PeerEstimator* peer = FindOrCreateSlot(connection);
	if (!peer)
		return;

	// Delivery delay: first send to acknowledgement, retransmits included, as the
	// engine measures it but with a fast EWMA. Unlike the Karn estimate below it
	// includes retransmit cost, which is what the latency level needs.
	if (peer->deliverySamples == 0)
		peer->scaledDelivery = delayTicks * 4;
	else
		peer->scaledDelivery += delayTicks - (peer->scaledDelivery >> 2);
	++peer->deliverySamples;

	// Karn: ignore acknowledgements of retransmitted packets, except as a
	// provisional first estimate that the first clean sample replaces.
	if (sendCount != 1)
	{
		if (!peer->initialized)
		{
			AddSample(peer, delayTicks);
			peer->provisional = true;
		}
		return;
	}

	if (peer->provisional)
	{
		peer->initialized = false;
		peer->provisional = false;
	}

	AddSample(peer, delayTicks);
	++peer->cleanSamples;
	ShortenPendingTimers(peer);
}

void FastRetransmit::NoteRetransmit(const ConnectionClass* connection, int capturedTicks, int nowTicks)
{
	if (!Enabled)
		return;

	PeerEstimator* peer = FindSlot(connection);
	if (!peer || !peer->initialized)
		return;

	// Only a packet that waited the full timeout justifies backing off.
	if (capturedTicks < peer->rto)
		return;

	// Once per timeout interval, not once per overdue packet.
	if (peer->backedOff && (nowTicks - peer->lastBackoffTime) < peer->rto)
		return;

	// Grow by half; the next clean sample replaces it.
	peer->rto = ClampTicks(peer->rto + (peer->rto + 1) / 2);
	peer->backedOff = true;
	peer->lastBackoffTime = nowTicks;
	++peer->backoffs;
}

int FastRetransmit::RetryTicks(const ConnectionClass* connection)
{
	if (!Enabled)
		return 0;

	const PeerEstimator* peer = FindSlot(connection);
	if (!peer || !peer->initialized)
		return 0;
	return ClampTicks(peer->rto);
}

int FastRetransmit::PacketRetryTicks(const ConnectionClass* connection, const SendQueueType* entry)
{
	const int measured = RetryTicks(connection);
	if (measured <= 0 || entry->SendCount <= 0)
		return CapToVanilla(connection, measured);

	PacketTimer* timer = TimerFor(FindSlot(connection), connection, entry, true);
	if (!timer)
		return CapToVanilla(connection, measured);
	if (!Matches(timer, entry))
	{
		// Packet sent before the estimate existed: adopt it once.
		CaptureTimer(timer, entry, measured, entry->FirstTime);
		ArmTimer(timer, connection, entry->SendCount, entry->LastTime);
	}

	// Re-clamp on every check: RetryDelta can drop after the timer was armed.
	return CapToVanilla(connection, timer->delayTicks);
}

void FastRetransmit::NoteSend(const ConnectionClass* connection, const SendQueueType* entry, int nowTicks)
{
	const int measured = RetryTicks(connection);
	if (measured <= 0)
		return;
	PacketTimer* timer = TimerFor(FindSlot(connection), connection, entry, true);
	if (!timer)
		return;
	if (!Matches(timer, entry))
		CaptureTimer(timer, entry, measured, entry->SendCount == 0 ? nowTicks : entry->FirstTime);

	// Called before the engine increments SendCount.
	ArmTimer(timer, connection, entry->SendCount + 1, nowTicks);
}

bool FastRetransmit::GetPacketStats(const ConnectionClass* connection, const SendQueueType* entry, PacketStats& out)
{
	if (!Enabled)
		return false;
	const PacketTimer* timer = TimerFor(FindSlot(connection), connection, entry, false);
	if (!Matches(timer, entry) || timer->sendCount != entry->SendCount || timer->lastTime != entry->LastTime)
		return false;
	out.BaseTicks = timer->baseTicks;
	// Report what the timer would actually fire at, not the stored value.
	out.DelayTicks = CapToVanilla(connection, timer->delayTicks);
	return true;
}

bool FastRetransmit::GetStats(const ConnectionClass* connection, PeerStats& out)
{
	const PeerEstimator* peer = FindSlot(connection);
	if (!peer || !peer->initialized)
		return false;

	out.Srtt = peer->scaledSrtt >> 3;
	out.RttVar = peer->scaledRttvar >> 2;
	out.Rto = peer->rto;
	out.CleanSamples = peer->cleanSamples;
	out.Backoffs = peer->backoffs;
	out.Provisional = peer->provisional;
	return true;
}

void FastRetransmit::GetCapStats(int& caps, int& worstOvershoot, int& lastUncapped)
{
	caps = g_vanillaCaps;
	worstOvershoot = g_worstOvershoot;
	lastUncapped = g_lastUncapped;
}

int FastRetransmit::WorstDeliveryDelay()
{
	int worst = -1;
	for (int i = 0; i < MaxPeers; ++i)
	{
		if (!Peers[i].connection || Peers[i].deliverySamples <= 0)
			continue;
		const int delay = Peers[i].scaledDelivery >> 2;
		if (delay > worst)
			worst = delay;
	}
	return worst;
}

int FastRetransmit::WorstSmoothedRTT()
{
	int worst = -1;
	for (int i = 0; i < MaxPeers; ++i)
	{
		if (!Peers[i].connection || !Peers[i].initialized || Peers[i].provisional)
			continue;
		const int srtt = Peers[i].scaledSrtt >> 3;
		if (srtt > worst)
			worst = srtt;
	}
	return worst;
}

int FastRetransmit::ActivePeers()
{
	return PrivateConnectionCount();
}

int FastRetransmit::InitializedPeers()
{
	int count = 0;
	for (int i = 0; i < MaxPeers; ++i)
		if (Peers[i].connection && Peers[i].initialized)
			++count;
	return count;
}

int FastRetransmit::CleanSamples()
{
	int total = 0;
	for (int i = 0; i < MaxPeers; ++i)
		total += Peers[i].cleanSamples;
	return total;
}

// An acknowledged packet's round trip, as the engine records it.
DEFINE_HOOK(0x48C436, ConnectionClass_ServiceSendQueue_RTTSample, 0x8)
{
	if (FastRetransmit::Enabled)
	{
		GET(const ConnectionClass*, conn, EDI);
		GET(const SendQueueType*, entry, EBX);
		GET(int, now, EBP);

		FastRetransmit::SampleRTT(conn, now - entry->FirstTime, entry->SendCount);
	}
	return 0;
}

// Uses the connection's own timeout in the retransmit check instead of RetryDelta.
DEFINE_HOOK(0x48C4AE, ConnectionClass_ServiceSendQueue_RetryTimer, 0x5)
{
	enum { Compare = 0x48C4B3 };

	GET(const ConnectionClass*, conn, EDI);
	GET(const SendQueueType*, entry, ESI);
	GET(int, now, EBP);
	GET(int, lastTime, ECX);

	const int vanilla = static_cast<int>(conn->RetryDelta);
	const int sendCount = entry->SendCount;
	const int elapsed = now - lastTime;

	// No estimate yet: keep the engine's timing, but still feed the loss gauge.
	const int measured = FastRetransmit::RetryTicks(conn);
	if (measured <= 0)
	{
		if (sendCount > 0 && elapsed > vanilla)
			PacketRedundancy::NoteResend(conn);
		return 0;
	}

	// Logs the engine's "connection gone bad" test (one packet unacknowledged
	// for longer than Timeout) with the packet's age.
	if (NetDiagnostics::Enabled && sendCount > 0)
	{
		const unsigned int connTimeout = conn->Timeout;
		const int age = entry->LastTime - entry->FirstTime;
		if (connTimeout != 0xFFFFFFFFu && age > static_cast<int>(connTimeout))
			NetDiagnostics::LogBadConnection(conn, age, (int)connTimeout, sendCount, (int)conn->RetryDelta);
	}

	const int eff = FastRetransmit::PacketRetryTicks(conn, entry);
	if (sendCount > 0 && elapsed > eff)
	{
		FastRetransmit::NoteRetransmit(conn, eff, now);
		PacketRedundancy::NoteResend(conn);
	}

	R->EAX(eff);
	R->EDX(now);
	return Compare;
}

// After a packet is sent, before LastTime and SendCount are updated.
DEFINE_HOOK(0x48C4E5, ConnectionClass_ServiceSendQueue_ArmRetry, 0x6)
{
	GET(const ConnectionClass*, conn, EDI);
	GET(const SendQueueType*, entry, ESI);
	GET(int, now, EBP);
	FastRetransmit::NoteSend(conn, entry, now);
	return 0;
}
