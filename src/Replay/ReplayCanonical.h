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

// The canonical form of a recording: the part every player's copy of the same game shares, and the
// SHA-256 chain sealed over it. See docs/replay-format.md, "Canonical hash". No game dependency
// beyond the EventClass layout, so readers outside the game can be checked against it.

#include "ReplayFormat.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

class EventClass;

namespace Replay
{
	class Sha256
	{
	public:
		Sha256();
		~Sha256();
		Sha256(const Sha256&) = delete;
		Sha256& operator=(const Sha256&) = delete;

		bool Begin();
		bool Update(const void* data, size_t size);
		// Ends the hash. Begin starts the next one.
		bool Finish(uint8_t (&out)[SealHashBytes]);

		static bool Compute(const void* data, size_t size, uint8_t (&out)[SealHashBytes]);

	private:
		void* Hash = nullptr;
		std::vector<uint8_t> HashObject;
	};

	// What the canonical description is built from: the header's simulation start state, the map,
	// and the players. Everything here is the same in every player's copy.
	struct CanonicalPlayer
	{
		bool IsHuman = false;
		bool IsObserver = false;
		int32_t Color = -1;
		int32_t Country = -1;
		int32_t Difficulty = -1;
		std::string NameUtf8;
	};

	// Every player slot spawn.ini describes, read with a deliberately small INI reader that readers outside
	// the game can reproduce exactly. See docs/replay-format.md.
	std::vector<CanonicalPlayer> ReadCanonicalPlayers(const char* spawnIni, size_t size);

	std::vector<uint8_t> BuildCanonicalDescriptor(const ReplayHeader& header, const void* map, size_t mapSize,
		const std::vector<CanonicalPlayer>& players);

	// The payload bytes an event of this type carries over the network: the vanilla table, then the
	// spawner's own types. -1 for a type this build does not know.
	int GetEventPayloadLength(uint8_t type);

	// Whether an event type belongs to the canonical form: everything every peer executes the same,
	// so neither the network's timing and bookkeeping events nor types whose payload length this
	// build does not know.
	bool IsCanonicalEventType(uint8_t type);

	// Appends one frame's canonical bytes. events are the frame's executed events in the order the
	// engine ran them. A frame belongs to the chain only when it has a hash or a canonical event - both
	// of which the file always records - so this appends nothing and returns false for any other.
	bool AppendCanonicalFrame(std::vector<uint8_t>& out, int frame, bool hasCRC, uint16_t crc,
		const EventClass* events, size_t eventCount);

	// A chat message's fingerprint: the first ChatFingerprintBytes of
	// SHA-256("YRCHAT1" || salt || uint8 sender house || the message's UTF-16LE code units, without the terminator).
	bool ComputeChatFingerprint(const uint8_t (&salt)[ChatSaltBytes], int houseIndex, const wchar_t* message,
		uint8_t (&fingerprint)[ChatFingerprintBytes]);

	// The chain over the canonical frames. Seals close every SealIntervalFrames of game time, whether
	// or not those frames carried a record.
	class SealChain
	{
	public:
		bool Begin(const std::vector<uint8_t>& descriptor);
		bool IsActive() const { return this->Active; }

		// Frames in increasing order. Any interval that ends before frame is sealed first, into
		// closedSeals.
		bool AddFrame(int frame, const std::vector<uint8_t>& canonicalBytes, std::vector<SealRecord>& closedSeals);

		// The seal over whatever followed the last full interval, through lastFrame.
		bool Finish(int lastFrame, SealRecord& finalSeal);

		const std::vector<SealRecord>& Seals() const { return this->AllSeals; }
		void Reset();

	private:
		bool CloseInterval(int throughFrame, SealRecord& seal);

		Sha256 Window;
		uint8_t Previous[SealHashBytes] = {};
		int IntervalEnd = SealIntervalFrames;
		std::vector<SealRecord> AllSeals;
		bool Active = false;
	};
}
