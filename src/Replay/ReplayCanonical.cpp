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

#include "ReplayCanonical.h"

#include <Ext/Event/Body.h>
#include <EventClass.h>

#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>

#pragma comment(lib, "bcrypt.lib")

namespace Replay
{
	namespace
	{
		BCRYPT_ALG_HANDLE Sha256Provider()
		{
			static BCRYPT_ALG_HANDLE provider = []
			{
				BCRYPT_ALG_HANDLE handle = nullptr;
				if (BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
					handle = nullptr;
				return handle;
			}();

			return provider;
		}

		void AppendBytes(std::vector<uint8_t>& out, const void* data, size_t size)
		{
			const auto* bytes = static_cast<const uint8_t*>(data);
			out.insert(out.end(), bytes, bytes + size);
		}

		template <typename T>
		void AppendValue(std::vector<uint8_t>& out, const T& value)
		{
			AppendBytes(out, &value, sizeof(value));
		}

		// MEGAMISSION payload: Whom (id, rtti) at 0-4, the mission at 5, the network unit-list slot at 6,
		// Target at 7-11, Destination at 12-16, Follow at 17-21, then one more byte.
		constexpr size_t MegaMissionWhomEnd = 5;
		constexpr size_t MegaMissionSlot = 6;
		constexpr size_t MegaMissionFollowBegin = 17;
		constexpr size_t MegaMissionFollowEnd = 22;

		std::string Lower(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return text;
		}

		std::string Trim(const std::string& text)
		{
			const size_t first = text.find_first_not_of(" \t\r");
			if (first == std::string::npos)
				return std::string();

			const size_t last = text.find_last_not_of(" \t\r");
			return text.substr(first, last - first + 1);
		}

		// Just enough of an INI reader for the canonical description, and simple enough that readers
		// outside the game can reproduce it exactly: [section] lines, key=value lines, section and key
		// names compared without case, values trimmed, the first occurrence kept, nothing else.
		using IniSections = std::map<std::string, std::map<std::string, std::string>>;

		IniSections ParseIni(const char* text, size_t size)
		{
			IniSections sections;
			std::map<std::string, std::string>* pSection = nullptr;

			size_t position = 0;
			while (position < size)
			{
				size_t end = position;
				while (end < size && text[end] != '\n')
					++end;

				const std::string line = Trim(std::string(text + position, end - position));
				position = end + 1;

				if (line.size() >= 2 && line.front() == '[' && line.back() == ']')
				{
					pSection = &sections[Lower(Trim(line.substr(1, line.size() - 2)))];
					continue;
				}

				const size_t equals = line.find('=');
				if (!pSection || equals == std::string::npos)
					continue;

				pSection->emplace(Lower(Trim(line.substr(0, equals))), Trim(line.substr(equals + 1)));
			}

			return sections;
		}

		const std::string* FindValue(const IniSections& ini, const char* section, const char* key)
		{
			const auto sectionIt = ini.find(section);
			if (sectionIt == ini.end())
				return nullptr;

			const auto keyIt = sectionIt->second.find(key);
			return keyIt == sectionIt->second.end() ? nullptr : &keyIt->second;
		}

		int32_t ReadInt(const IniSections& ini, const char* section, const char* key)
		{
			const std::string* pValue = FindValue(ini, section, key);
			return pValue ? static_cast<int32_t>(std::strtol(pValue->c_str(), nullptr, 10)) : -1;
		}

		bool ReadBool(const IniSections& ini, const char* section, const char* key)
		{
			const std::string* pValue = FindValue(ini, section, key);
			return pValue && !pValue->empty() && std::strchr("1tTyY", (*pValue)[0]) != nullptr;
		}
	}

	Sha256::Sha256() = default;

	Sha256::~Sha256()
	{
		if (this->Hash)
			BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(this->Hash));
	}

	bool Sha256::Begin()
	{
		if (this->Hash)
		{
			BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(this->Hash));
			this->Hash = nullptr;
		}

		const BCRYPT_ALG_HANDLE provider = Sha256Provider();
		if (!provider)
			return false;

		if (this->HashObject.empty())
		{
			DWORD objectLength = 0, written = 0;
			if (BCryptGetProperty(provider, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
				sizeof(objectLength), &written, 0) < 0)
			{
				return false;
			}
			this->HashObject.resize(objectLength);
		}

		BCRYPT_HASH_HANDLE hash = nullptr;
		if (BCryptCreateHash(provider, &hash, this->HashObject.data(), static_cast<ULONG>(this->HashObject.size()),
			nullptr, 0, 0) < 0)
		{
			return false;
		}

		this->Hash = hash;
		return true;
	}

	bool Sha256::Update(const void* data, size_t size)
	{
		return this->Hash && (size == 0
			|| BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(this->Hash),
				static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0) >= 0);
	}

	bool Sha256::Finish(uint8_t (&out)[SealHashBytes])
	{
		if (!this->Hash)
			return false;

		const bool ok = BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(this->Hash), out, SealHashBytes, 0) >= 0;
		BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(this->Hash));
		this->Hash = nullptr;
		return ok;
	}

	bool Sha256::Compute(const void* data, size_t size, uint8_t (&out)[SealHashBytes])
	{
		Sha256 hash;
		return hash.Begin() && hash.Update(data, size) && hash.Finish(out);
	}

	int GetEventPayloadLength(uint8_t type)
	{
		if (type < VanillaEventTypeCount)
			return VanillaEventLength[type];

		const auto extType = static_cast<EventTypeExt>(type);
		if (EventExt::IsValidType(extType))
			return static_cast<int>(EventExt::GetDataSize(extType));

		return -1;
	}

	bool IsCanonicalEventType(uint8_t type)
	{
		switch (type)
		{
		// Network timing and bookkeeping: each peer sees its own set, and the local FRAMEINFO never
		// enters DoList at all.
		case 0:  // EMPTY
		case 25: // FRAMESYNC
		case 26: // MESSAGE
		case 27: // RESPONSE_TIME
		case 28: // FRAMEINFO
		case 31: // ADDPLAYER
		case 32: // TIMING
		case 33: // PROCESS_TIME
		case 34: // PAGEUSER
		case 35: // REMOVEPLAYER
		case 36: // LATENCYFUDGE
		case 37: // MEGAFRAMEINFO
		case 38: // PACKETTIMING
		case 39: // ABOUTTOEXIT
		case 40: // FALLBACKHOST
		case 41: // ADDRESSCHANGE
			return false;
		}

		if (type < VanillaEventTypeCount)
			return true;

		return static_cast<EventTypeExt>(type) == EventTypeExt::ChatFingerprint;
	}

	// Every player slot spawn.ini describes: the eight sections the spawner reads players from, and
	// the AI slots in the HouseColors/HouseCountries/HouseHandicaps lists.
	std::vector<CanonicalPlayer> ReadCanonicalPlayers(const char* spawnIni, size_t size)
	{
		static const char* const playerSections[8] = {
			"settings", "other1", "other2", "other3", "other4", "other5", "other6", "other7" };
		static const char* const multiTags[8] = {
			"multi1", "multi2", "multi3", "multi4", "multi5", "multi6", "multi7", "multi8" };

		const IniSections ini = ParseIni(spawnIni, size);
		std::vector<CanonicalPlayer> players;

		for (int i = 0; i < 8; ++i)
		{
			CanonicalPlayer player {};
			const std::string* pName = FindValue(ini, playerSections[i], "name");

			if (pName && !pName->empty())
			{
				player.IsHuman = true;
				player.NameUtf8 = *pName;
				player.Color = ReadInt(ini, playerSections[i], "color");
				player.Country = ReadInt(ini, playerSections[i], "side");
				player.IsObserver = ReadBool(ini, playerSections[i], "isspectator");
			}
			else
			{
				player.Color = ReadInt(ini, "housecolors", multiTags[i]);
				player.Country = ReadInt(ini, "housecountries", multiTags[i]);
				player.Difficulty = ReadInt(ini, "househandicaps", multiTags[i]);
				if (player.Color < 0 && player.Country < 0 && player.Difficulty < 0)
					continue;
			}

			players.push_back(std::move(player));
		}

		return players;
	}

	std::vector<uint8_t> BuildCanonicalDescriptor(const ReplayHeader& header, const void* map, size_t mapSize,
		const std::vector<CanonicalPlayer>& players)
	{
		std::vector<uint8_t> out;
		AppendBytes(out, "YRRPSEAL", 8);
		AppendValue(out, uint32_t { 1 });
		AppendValue(out, header.GameMode);
		AppendValue(out, header.UniqueIDCounter);
		AppendValue(out, header.Seed);
		AppendValue(out, header.RandomNext1);
		AppendValue(out, header.RandomNext2);
		AppendBytes(out, header.RandomizerTable, sizeof(header.RandomizerTable));
		AppendValue(out, header.RecordedGameSpeed);

		uint8_t mapHash[SealHashBytes] = {};
		Sha256::Compute(map, mapSize, mapHash);
		AppendBytes(out, mapHash, sizeof(mapHash));

		// Each player's own copy lists itself first, so the order is made independent of whose it is.
		std::vector<std::vector<uint8_t>> entries;
		for (const auto& player : players)
		{
			std::vector<uint8_t> entry;
			AppendValue(entry, static_cast<uint8_t>(player.IsHuman));
			AppendValue(entry, static_cast<uint8_t>(player.IsObserver));
			AppendValue(entry, player.Color);
			AppendValue(entry, player.Country);
			AppendValue(entry, player.Difficulty);
			AppendValue(entry, static_cast<uint32_t>(player.NameUtf8.size()));
			AppendBytes(entry, player.NameUtf8.data(), player.NameUtf8.size());
			entries.push_back(std::move(entry));
		}

		std::sort(entries.begin(), entries.end());
		AppendValue(out, static_cast<uint32_t>(entries.size()));
		for (const auto& entry : entries)
			AppendBytes(out, entry.data(), entry.size());

		return out;
	}

	bool AppendCanonicalFrame(std::vector<uint8_t>& out, int frame, bool hasCRC, uint16_t crc,
		const EventClass* events, size_t eventCount)
	{
		uint32_t canonicalCount = 0;
		for (size_t i = 0; i < eventCount; ++i)
		{
			if (IsCanonicalEventType(static_cast<uint8_t>(events[i].Type)))
				++canonicalCount;
		}

		if (!hasCRC && canonicalCount == 0)
			return false;

		AppendVarint(out, static_cast<uint32_t>(frame));
		AppendValue(out, static_cast<uint8_t>(hasCRC));
		if (hasCRC)
			AppendValue(out, crc);
		AppendVarint(out, canonicalCount);

		// The previous canonical event, when it was a MEGAMISSION: its payload as sent, and its house.
		bool hasPreviousMegaMission = false;
		uint8_t previousMegaMission[MaxEventPayload] = {};
		uint8_t previousHouse = 0;
		uint8_t payload[MaxEventPayload] = {};

		for (size_t i = 0; i < eventCount; ++i)
		{
			const auto* raw = reinterpret_cast<const uint8_t*>(&events[i]);
			const uint8_t type = raw[0];
			const uint8_t house = raw[2];
			if (!IsCanonicalEventType(type))
				continue;

			const int length = GetEventPayloadLength(type);
			std::memcpy(payload, raw + EventPayloadOffset, static_cast<size_t>(length));

			if (type == static_cast<uint8_t>(EventType::MegaMission))
			{
				// The unit-list slot is the sender's network bookkeeping; only receivers hold it.
				payload[MegaMissionSlot] = 0;

				// A group order travels as one full order and the other units' Whom alone, so every
				// receiver gives the rest the first one's Follow. Only the sender has each unit's own.
				const bool continuesGroup = hasPreviousMegaMission && previousHouse == house
					&& std::memcmp(previousMegaMission + MegaMissionWhomEnd, payload + MegaMissionWhomEnd,
						MegaMissionFollowBegin - MegaMissionWhomEnd) == 0
					&& std::memcmp(previousMegaMission + MegaMissionFollowEnd, payload + MegaMissionFollowEnd,
						static_cast<size_t>(length) - MegaMissionFollowEnd) == 0;

				// Kept as it was sent, before Follow is cleared, for the next order to be compared with.
				std::memcpy(previousMegaMission, payload, static_cast<size_t>(length));
				hasPreviousMegaMission = true;
				previousHouse = house;

				if (continuesGroup)
					std::memset(payload + MegaMissionFollowBegin, 0, MegaMissionFollowEnd - MegaMissionFollowBegin);
			}
			else
			{
				hasPreviousMegaMission = false;
			}

			AppendValue(out, type);
			AppendValue(out, house);
			AppendBytes(out, payload, static_cast<size_t>(length));
		}

		return true;
	}

	bool ComputeChatFingerprint(const uint8_t (&salt)[ChatSaltBytes], int houseIndex, const wchar_t* message,
		uint8_t (&fingerprint)[ChatFingerprintBytes])
	{
		const uint8_t house = static_cast<uint8_t>(houseIndex);
		const size_t length = message ? wcslen(message) : 0;

		Sha256 hash;
		uint8_t digest[SealHashBytes] = {};
		if (!hash.Begin() || !hash.Update("YRCHAT1", 7) || !hash.Update(salt, sizeof(salt))
			|| !hash.Update(&house, sizeof(house)) || !hash.Update(message, length * sizeof(wchar_t))
			|| !hash.Finish(digest))
		{
			return false;
		}

		std::memcpy(fingerprint, digest, sizeof(fingerprint));
		return true;
	}

	void SealChain::Reset()
	{
		this->Active = false;
		this->IntervalEnd = SealIntervalFrames;
		this->AllSeals.clear();
		std::memset(this->Previous, 0, sizeof(this->Previous));
	}

	bool SealChain::Begin(const std::vector<uint8_t>& descriptor)
	{
		this->Reset();
		this->Active = Sha256::Compute(descriptor.data(), descriptor.size(), this->Previous)
			&& this->Window.Begin() && this->Window.Update(this->Previous, sizeof(this->Previous));
		return this->Active;
	}

	bool SealChain::CloseInterval(int throughFrame, SealRecord& seal)
	{
		if (!this->Window.Finish(this->Previous))
		{
			this->Active = false;
			return false;
		}

		seal.ThroughFrame = throughFrame;
		std::memcpy(seal.Hash, this->Previous, sizeof(seal.Hash));
		this->AllSeals.push_back(seal);

		this->Active = this->Window.Begin() && this->Window.Update(this->Previous, sizeof(this->Previous));
		return this->Active;
	}

	bool SealChain::AddFrame(int frame, const std::vector<uint8_t>& canonicalBytes, std::vector<SealRecord>& closedSeals)
	{
		if (!this->Active)
			return false;

		while (frame >= this->IntervalEnd)
		{
			SealRecord seal {};
			if (!this->CloseInterval(this->IntervalEnd - 1, seal))
				return false;

			closedSeals.push_back(seal);
			this->IntervalEnd += SealIntervalFrames;
		}

		return this->Window.Update(canonicalBytes.data(), canonicalBytes.size());
	}

	bool SealChain::Finish(int lastFrame, SealRecord& finalSeal)
	{
		if (!this->Active)
			return false;

		const bool ok = this->CloseInterval(lastFrame, finalSeal);
		this->Active = false;
		return ok;
	}
}
