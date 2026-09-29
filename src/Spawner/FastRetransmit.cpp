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

	// Jacobson/Karels in fixed point. Round trips are measured in 16 ms ticks, so
	// the estimate lives in the low single digits, and a plain integer
	// srtt = (7*srtt + sample)/8 truncates every increment away: srtt can then
	// only ratchet down, and a link that slows by less than 8 ticks is never
	// tracked at all. Scaling srtt by 8 and rttvar by 4 keeps the residue, which
	// is the whole point of the classic scaled form.
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
		// Engine time of the last timer backoff, so a queue with several overdue
		// entries backs off once per round rather than once per entry.
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

	// How often the vanilla ceiling actually bound, and by how much, so a log
	// can show whether the backoff was trying to run past stock timing.
	int g_vanillaCaps = 0;
	int g_worstOvershoot = 0;
	int g_lastUncapped = 0;

	// Last clamped value per connection. One value shared by all connections
	// made several capped links alternate, so every read counted as new: 163,470
	// in Game 21 with four connections capped at once. Connections are created
	// at game start, so the pointer is a stable key for the match.
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

	// Never let an adaptive timer fire LATER than the engine's own RetryDelta.
	//
	// Add_Delay stamps a packet's delay sample from FirstTime - the first
	// transmission - so every tick we spend waiting before a resend lands in
	// Avg_Response_Time. The engine then recomputes RetryDelta as
	// Response_Time() + 10, which is the ceiling here, so a timer allowed to
	// exceed it feeds a loop that ratchets both figures up together: longer
	// waits produce larger samples, larger samples raise the ceiling, the
	// higher ceiling permits longer waits. That drives the reported response
	// toward the wire field's 126-tick limit, inflates the latency level, and
	// stretches the connection Timeout - all of it strictly worse than the
	// stock game, which has no such loop because its timer never moves.
	//
	// Clamping here makes the guarantee one-directional: this PR can retransmit
	// sooner than stock, never later. Congestion response does not disappear
	// with it - RetryDelta is itself derived from the measured response, so the
	// ceiling still rises on a genuinely bad link, just no faster than vanilla
	// would have raised it.
	int CapToVanilla(const ConnectionClass* connection, int ticks)
	{
		if (!connection || ticks <= 0)
			return ticks;

		const int vanilla = static_cast<int>(connection->RetryDelta);
		if (vanilla <= 0 || ticks <= vanilla)
			return ticks;

		// Count distinct clamped values, not every read. The ceiling is applied on
		// each check and again whenever diagnostics inspect a timer, so a plain
		// increment counted queue traffic rather than ceiling events - it reached
		// 1.5 million in one match while another client logged 191.
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

	// Only live private connections are estimated. The global, router and
	// multicast channels share ConnectionClass::Service_Send_Queue but are not
	// in Connection[], so they fall through to the engine's own RetryDelta.
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

	// Finds this connection's slot, allocating a free one if it has none.
	// MaxPeers exceeds the engine's connection array, so with dead slots pruned
	// there is always room and a live peer is never evicted.
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

	// Logs a peer's estimate when it moves, at most once a second, so a long
	// match does not fill the log with noise.
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

		// Capture the connection-timeout cap as well: subsequent Set_Timing
		// calls must not move a deadline that is already armed.
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

			// A fresh, unambiguous RTT sample can supersede a base captured
			// during earlier loss. Keep LastTime anchored to the actual send;
			// a shortened interval may already be due on the next service pass.
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

	// Delivery delay: the same quantity the engine's own Avg_Response_Time
	// measures - time from a packet's FIRST send to its acknowledgement, so
	// retransmit waiting is included - but as a fast EWMA instead of a
	// 256-sample mean.
	//
	// This exists because the Karn-filtered estimate below cannot drive the
	// latency level. Karn discards every retransmitted sample, so that figure is
	// true round-trip time and reads 1 tick on a LAN while the engine reports
	// 25-31 under loss. The difference is not error, it is retransmit cost - and
	// retransmit cost is exactly what delays a command reaching its peers, so it
	// belongs in the latency level. Taking max(engine, clean) therefore never
	// picked the fast figure, and a raise from level 4 to 9 took 4604 frames.
	//
	// Sampling the same input the engine samples, at alpha = 1/4, reaches a step
	// change in about ten acknowledgements rather than a few hundred.
	if (peer->deliverySamples == 0)
		peer->scaledDelivery = delayTicks * 4;
	else
		peer->scaledDelivery += delayTicks - (peer->scaledDelivery >> 2);
	++peer->deliverySamples;

	// Karn: an acknowledgement that follows a retransmission cannot be
	// attributed to a particular transmission, so discard it - unless we have
	// nothing at all, in which case take it as a provisional upper bound so a
	// link that is losing its first transmissions is still measurable. The first
	// clean sample replaces that seed rather than blending with it.
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

	// Only a packet that actually waited at least this timeout can justify
	// increasing the allowance for new packets. Loss alone is not an RTT sample.
	if (capturedTicks < peer->rto)
		return;

	// A stalled link has several overdue entries in the queue at once and this
	// runs for each of them. Back off at most once per timeout interval so the
	// estimate grows one step per retransmission round, not one step per packet.
	if (peer->backedOff && (nowTicks - peer->lastBackoffTime) < peer->rto)
		return;

	// Grow by half; the next clean sample recomputes the estimate from scratch
	// and discards the growth.
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
		// The estimate may first become available with packets already in
		// flight. Adopt it once; later peer increases cannot delay this packet.
		CaptureTimer(timer, entry, measured, entry->FirstTime);
		ArmTimer(timer, connection, entry->SendCount, entry->LastTime);
	}

	// Re-clamp on every check, not just when the timer was armed.
	//
	// RetryDelta is recomputed as Response_Time() + 10 and falls quickly once a
	// link recovers, so a timer that was legal when armed can outlive that drop
	// and sit above the engine's current figure - measured at 48 ticks against a
	// live RetryDelta of 17. Vanilla re-reads RetryDelta on every comparison, so
	// for that packet we really would have been slower than stock, which is the
	// one thing this option promises never to do.
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

	// Called after Send(), before the engine increments SendCount. Preserve
	// the packet's base (which only clean samples can lower): peer RTO growth
	// is for newly sent packets,
	// not another multiplier on this packet's retry history.
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
		// A provisional estimate came from an ambiguous acknowledgement, so it
		// carries retransmit delay the engine's figure already accounts for.
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

// ConnectionClass::Service_Send_Queue, at the point an ACK'd PACKET_DATA_ACK
// entry's round-trip is about to be folded into the queue's response time.
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

// ConnectionClass::Service_Send_Queue, replacing the two loads that feed the
// "has RetryDelta elapsed since this entry was last sent" comparison:
//   48C4AE  mov eax, [edi+28h]   ; this->RetryDelta
//   48C4B1  mov edx, ebp         ; now
//   48C4B3  sub edx, ecx / cmp edx, eax / jbe ...
// We substitute this connection's own measured timeout for the global one.
DEFINE_HOOK(0x48C4AE, ConnectionClass_ServiceSendQueue_RetryTimer, 0x5)
{
	enum { Compare = 0x48C4B3 };

	// Even with the adaptive timer disabled, actual resends must still
	// feed the independently configurable packet-redundancy loss gauge.
	GET(const ConnectionClass*, conn, EDI);
	GET(const SendQueueType*, entry, ESI);
	GET(int, now, EBP);
	GET(int, lastTime, ECX);

	const int vanilla = static_cast<int>(conn->RetryDelta);
	const int sendCount = entry->SendCount;
	const int elapsed = now - lastTime;

	// Zero when this connection has no estimate of its own: the lobby/global
	// channels, and private peers not measured yet. Those keep vanilla timing
	// exactly, including its backoff-free behaviour.
	const int measured = FastRetransmit::RetryTicks(conn);
	if (measured <= 0)
	{
		// LastTime starts at zero, so a first send also passes the elapsed-time
		// test. Only an entry already sent is evidence of packet loss.
		if (sendCount > 0 && elapsed > vanilla)
			PacketRedundancy::NoteResend(conn);
		return 0;
	}

	// The engine's own "connection has gone bad" test, logged with the context it
	// never prints. Service_Send_Queue marks a connection bad when ONE queued
	// entry has been unacknowledged for longer than conn->Timeout - it is the age
	// of a single packet, not a measure of overall silence - and it only
	// evaluates that test when a retransmit is due, which is exactly here.
	//
	// The flag itself does very little: it sets IPXManagerClass::BadConnection,
	// whose only accessor (Get_Bad_Connection, 0x5422C0) has no callers and is in
	// no vtable, and it makes IPXManagerClass::Service return 0, which
	// Queue_AI_Multiplayer discards at 0x647F63. So this is worth recording
	// precisely because it fires often and costs nothing - without the age and
	// the timeout printed beside it, a log full of "gone bad" says nothing about
	// why.
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
		// Pass the timeout this packet really waited under, not the peer's
		// current estimate (which may have changed since the last send).
		FastRetransmit::NoteRetransmit(conn, eff, now);
		PacketRedundancy::NoteResend(conn);
	}

	R->EAX(eff);
	R->EDX(now);
	return Compare;
}

// Immediately after the engine's virtual Send call. EBP is the same timestamp
// the engine is about to store in LastTime; SendCount still has its old value.
//   48C4E5 mov eax, [esi+0Ch]
//   48C4E8 mov [esi+8], ebp
DEFINE_HOOK(0x48C4E5, ConnectionClass_ServiceSendQueue_ArmRetry, 0x6)
{
	GET(const ConnectionClass*, conn, EDI);
	GET(const SendQueueType*, entry, ESI);
	GET(int, now, EBP);
	FastRetransmit::NoteSend(conn, entry, now);
	return 0;
}
