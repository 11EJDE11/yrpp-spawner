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

#include "ReplayFrameCodec.h"
#include "ReplayCanonical.h"
#include "ReplayFile.h"

#include <Utilities/Debug.h>
#include <EventClass.h>

#include <algorithm>
#include <cstring>

namespace Replay
{
	namespace
	{
		constexpr uint8_t FrameInfoType = static_cast<uint8_t>(EventType::FrameInfo);
		// FRAMEINFO in memory: CRC at +7, CommandCount at +11, Delay at +13. On disk: CommandCount,
		// Delay, then the FrameInfoCheck the recorder put in place of the CRC.
		constexpr size_t FrameInfoCRCOffset = 7;
		constexpr size_t FrameInfoCommandCountOffset = 11;
		constexpr size_t FrameInfoDelayOffset = 13;
		constexpr size_t FrameInfoStoredBytes = 4;

		template <typename T>
		void AppendValue(std::vector<uint8_t>& out, const T& value)
		{
			const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
			out.insert(out.end(), bytes, bytes + sizeof(value));
		}

		void AppendBytes(std::vector<uint8_t>& out, const void* data, size_t size)
		{
			const auto* bytes = static_cast<const uint8_t*>(data);
			out.insert(out.end(), bytes, bytes + size);
		}

		void AppendString(std::vector<uint8_t>& out, const wchar_t* text, size_t capacity)
		{
			char utf8[SideChannelTextLength * 3 + 1] = {};
			const int length = static_cast<int>(wcsnlen(text, capacity));
			const int written = length > 0
				? WideCharToMultiByte(CP_UTF8, 0, text, length, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr)
				: 0;

			AppendVarint(out, static_cast<uint32_t>(std::max(written, 0)));
			AppendBytes(out, utf8, static_cast<size_t>(std::max(written, 0)));
		}

		bool ReadVarint(File& file, uint32_t& value)
		{
			value = 0;
			for (int shift = 0; shift < 35; shift += 7)
			{
				uint8_t byte = 0;
				if (!file.Read(&byte, 1))
					return false;

				value |= static_cast<uint32_t>(byte & 0x7Fu) << shift;
				if ((byte & 0x80u) == 0)
					return true;
			}

			return false;
		}

		// Bounds-checked reads out of one block's bytes.
		struct BlockCursor
		{
			const uint8_t* Data;
			size_t Size;
			size_t Position = 0;

			bool Bytes(void* out, size_t count)
			{
				if (count > this->Size - this->Position)
					return false;

				std::memcpy(out, this->Data + this->Position, count);
				this->Position += count;
				return true;
			}

			template <typename T>
			bool Value(T& out) { return this->Bytes(&out, sizeof(out)); }

			bool Varint(uint32_t& value)
			{
				value = 0;
				for (int shift = 0; shift < 35; shift += 7)
				{
					uint8_t byte = 0;
					if (!this->Value(byte))
						return false;

					value |= static_cast<uint32_t>(byte & 0x7Fu) << shift;
					if ((byte & 0x80u) == 0)
						return true;
				}

				return false;
			}

			bool SignedVarint(int32_t& value)
			{
				uint32_t raw = 0;
				if (!this->Varint(raw))
					return false;

				value = ZigZagDecode(raw);
				return true;
			}

			bool IDList(std::vector<uint32_t>& ids, uint32_t maxCount, bool allowEmpty)
			{
				uint32_t count = 0;
				if (!this->Varint(count) || count > maxCount || (!allowEmpty && count == 0))
					return false;

				ids.resize(count);
				for (auto& id : ids)
				{
					if (!this->Varint(id))
						return false;
				}

				return true;
			}

			bool String(wchar_t* out, size_t capacity)
			{
				uint32_t length = 0;
				if (!this->Varint(length) || length > SideChannelTextLength * 3)
					return false;

				char utf8[SideChannelTextLength * 3] = {};
				if (!this->Bytes(utf8, length))
					return false;

				const int written = length > 0
					? MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(length), out, static_cast<int>(capacity - 1))
					: 0;
				out[std::clamp(written, 0, static_cast<int>(capacity - 1))] = L'\0';
				return true;
			}
		};

		bool ReadSideChannelRecord(BlockCursor& cursor, SideChannelRecord& record)
		{
			int32_t x = 0, y = 0, z = 0;
			if (!cursor.Value(record.Type)
				|| !cursor.SignedVarint(record.House)
				|| !cursor.SignedVarint(record.Aux)
				|| !cursor.SignedVarint(x) || !cursor.SignedVarint(y) || !cursor.SignedVarint(z)
				|| !cursor.String(record.SenderName, SideChannelNameLength)
				|| !cursor.String(record.Text, SideChannelTextLength))
			{
				return false;
			}

			record.Coord = { x, y, z };

			if (record.Type == static_cast<uint8_t>(SideChannelEventType::ChatMessage))
			{
				uint8_t hasSalt = 0;
				if (!cursor.Value(hasSalt) || hasSalt > 1)
					return false;

				record.HasSalt = hasSalt != 0;
				if (record.HasSalt && !cursor.Bytes(record.Salt, sizeof(record.Salt)))
					return false;
			}

			return true;
		}
	}

	void FrameWriter::Reset()
	{
		this->HasLastWrittenFrameState = false;
		this->LastWrittenFrameNumber = -1;
		this->LastRecordedTacticalPos = { 0, 0 };
		this->LastRecordedSelectionIDs.clear();
		this->LastRecordedGameSpeed = -1;
	}

	bool FrameWriter::WriteFrame(File& file, const RecordedFrameCapture& capture, int eventsThisFrame,
		const std::vector<SideChannelRecord>& sideChannelEvents)
	{
		const bool tacticalPosChanged = !this->HasLastWrittenFrameState
			|| capture.TacticalPos.X != this->LastRecordedTacticalPos.X
			|| capture.TacticalPos.Y != this->LastRecordedTacticalPos.Y;

		const bool selectionChanged = !this->HasLastWrittenFrameState
			|| capture.SelectedObjectIDs != this->LastRecordedSelectionIDs;

		const bool hasSideChannelEvents = !sideChannelEvents.empty();
		const bool hasGameCRC = capture.HasGameCRC;
		const bool hasSelectionTriggers = !capture.SelectionTriggerObjectIDs.empty();
		const bool hasHouseStats = capture.HasHouseStats && !capture.HouseStats.empty();
		const bool hasMoneyIn = !capture.MoneyIn.empty();
		const bool hasSeals = !capture.Seals.empty();

		if (eventsThisFrame == 0 && !tacticalPosChanged && !selectionChanged && !hasSideChannelEvents
			&& !hasGameCRC && !capture.HasGameSpeed && !hasSelectionTriggers && !hasHouseStats && !hasMoneyIn
			&& !hasSeals)
		{
			return true;
		}

		const int32_t frameDelta = capture.FrameNumber - this->LastWrittenFrameNumber;
		if (frameDelta <= 0)
		{
			Debug::Log("[Replay] Frame %d does not follow frame %d; not writing it.\n",
				capture.FrameNumber, this->LastWrittenFrameNumber);
			return false;
		}

		auto& record = this->Record;
		auto& block = this->Block;
		record.clear();

		AppendVarint(record, (static_cast<uint32_t>(frameDelta) << 1) | (hasGameCRC ? 1u : 0u));
		if (hasGameCRC)
			AppendValue(record, static_cast<uint16_t>(capture.GameCRC & 0xFFFFu));
		AppendVarint(record, static_cast<uint32_t>(eventsThisFrame));

		const auto writeBlock = [&](FrameBlockTag tag)
		{
			AppendVarint(record, tag);
			AppendVarint(record, static_cast<uint32_t>(block.size()));
			AppendBytes(record, block.data(), block.size());
		};

		if (tacticalPosChanged)
		{
			block.clear();
			AppendVarint(block, ZigZagEncode(capture.TacticalPos.X - this->LastRecordedTacticalPos.X));
			AppendVarint(block, ZigZagEncode(capture.TacticalPos.Y - this->LastRecordedTacticalPos.Y));
			writeBlock(FrameBlock_TacticalPos);
		}

		if (selectionChanged)
		{
			block.clear();
			const size_t count = std::min(capture.SelectedObjectIDs.size(), static_cast<size_t>(MaxSelectedObjects));
			AppendVarint(block, static_cast<uint32_t>(count));
			for (size_t i = 0; i < count; ++i)
				AppendVarint(block, capture.SelectedObjectIDs[i]);
			writeBlock(FrameBlock_Selection);
		}

		if (hasSideChannelEvents)
		{
			block.clear();
			AppendVarint(block, static_cast<uint32_t>(sideChannelEvents.size()));
			for (const auto& event : sideChannelEvents)
			{
				AppendValue(block, event.Type);
				AppendVarint(block, ZigZagEncode(event.House));
				AppendVarint(block, ZigZagEncode(event.Aux));
				AppendVarint(block, ZigZagEncode(event.Coord.X));
				AppendVarint(block, ZigZagEncode(event.Coord.Y));
				AppendVarint(block, ZigZagEncode(event.Coord.Z));
				AppendString(block, event.SenderName, SideChannelNameLength);
				AppendString(block, event.Text, SideChannelTextLength);

				if (event.Type == static_cast<uint8_t>(SideChannelEventType::ChatMessage))
				{
					AppendValue(block, static_cast<uint8_t>(event.HasSalt));
					if (event.HasSalt)
						AppendBytes(block, event.Salt, sizeof(event.Salt));
				}
			}
			writeBlock(FrameBlock_SideChannel);
		}

		if (capture.HasGameSpeed)
		{
			block.clear();
			AppendValue(block, static_cast<uint8_t>(capture.GameSpeed));
			writeBlock(FrameBlock_GameSpeed);
		}

		if (hasSelectionTriggers)
		{
			block.clear();
			AppendVarint(block, static_cast<uint32_t>(capture.SelectionTriggerObjectIDs.size()));
			for (const auto id : capture.SelectionTriggerObjectIDs)
				AppendVarint(block, id);
			writeBlock(FrameBlock_SelectionTriggers);
		}

		if (hasHouseStats)
		{
			block.clear();
			AppendVarint(block, static_cast<uint32_t>(capture.HouseStats.size()));
			AppendBytes(block, capture.HouseStats.data(), capture.HouseStats.size() * sizeof(HouseStatsSample));
			writeBlock(FrameBlock_HouseStats);
		}

		if (hasMoneyIn)
		{
			block.clear();
			const size_t count = std::min<size_t>(capture.MoneyIn.size(), MaxMoneyInPerFrame);
			AppendVarint(block, static_cast<uint32_t>(count));
			AppendBytes(block, capture.MoneyIn.data(), count * sizeof(MoneyInRecord));
			writeBlock(FrameBlock_MoneyIn);
		}

		for (const auto& seal : capture.Seals)
		{
			block.clear();
			AppendBytes(block, &seal, sizeof(seal));
			writeBlock(FrameBlock_Seal);
		}

		AppendVarint(record, FrameBlock_End);

		// One call for the whole record: each file write is a tdefl_compress call.
		if (!file.Write(record.data(), record.size()))
			return false;

		if (capture.HasGameSpeed)
			this->LastRecordedGameSpeed = capture.GameSpeed;

		this->HasLastWrittenFrameState = true;
		this->LastWrittenFrameNumber = capture.FrameNumber;
		this->LastRecordedTacticalPos = capture.TacticalPos;
		this->LastRecordedSelectionIDs = capture.SelectedObjectIDs;

		return true;
	}

	bool FrameWriter::WriteEvents(File& file, const std::vector<EventClass>& events)
	{
		if (events.empty())
			return true;

		auto& record = this->Record;
		record.clear();

		for (const auto& event : events)
		{
			const auto* raw = reinterpret_cast<const uint8_t*>(&event);
			const uint8_t type = raw[0];

			AppendValue(record, type);
			AppendValue(record, raw[2]);
			AppendVarint(record, ZigZagEncode(static_cast<int32_t>(event.Frame) - this->LastWrittenFrameNumber));

			if (type == FrameInfoType)
			{
				AppendBytes(record, raw + FrameInfoCommandCountOffset, 2);
				AppendValue(record, raw[FrameInfoDelayOffset]);
				AppendValue(record, raw[FrameInfoCRCOffset]);
				continue;
			}

			const int length = GetEventPayloadLength(type);
			if (length >= 0)
			{
				AppendBytes(record, raw + EventPayloadOffset, static_cast<size_t>(length));
			}
			else
			{
				// Another module's event: its length is not known here, so the whole union goes in.
				AppendVarint(record, static_cast<uint32_t>(MaxEventPayload));
				AppendBytes(record, raw + EventPayloadOffset, MaxEventPayload);
			}
		}

		return file.Write(record.data(), record.size());
	}

	bool FrameWriter::WriteEndMarker(File& file)
	{
		const uint8_t marker = 0;
		return file.Write(&marker, sizeof(marker));
	}

	bool FrameReader::ReadFrame(File& file, PlaybackFrameRecord& record,
		bool (&validateSideChannel)(SideChannelRecord&))
	{
		record = PlaybackFrameRecord {};

		uint32_t head = 0;
		if (!ReadVarint(file, head))
			return false;

		if (head == 0)
		{
			record.FrameNumber = -1;
			record.EndOfStream = true;
			return true;
		}

		const uint32_t frameDelta = head >> 1;
		if (frameDelta == 0 || frameDelta > 0x3FFFFFFFu)
			return false;

		record.FrameNumber = this->LastReadFrame + static_cast<int32_t>(frameDelta);

		if ((head & 1u) != 0u)
		{
			if (!file.Read(&record.GameCRC, sizeof(record.GameCRC)))
				return false;
			record.Flags |= FrameRecordFlag_GameCRC;
		}

		uint32_t eventCount = 0;
		if (!ReadVarint(file, eventCount) || eventCount > static_cast<uint32_t>(MaxEventsPerFrame))
			return false;
		record.EventCountThisFrame = static_cast<int32_t>(eventCount);

		for (;;)
		{
			uint32_t tag = 0;
			if (!ReadVarint(file, tag))
				return false;

			if (tag == FrameBlock_End)
				break;

			uint32_t length = 0;
			if (!ReadVarint(file, length) || length > MaxFrameBlockBytes)
				return false;

			this->Block.resize(length);
			if (length > 0 && !file.Read(this->Block.data(), length))
				return false;

			BlockCursor cursor { this->Block.data(), this->Block.size() };
			bool ok = true;

			switch (tag)
			{
			case FrameBlock_TacticalPos:
			{
				int32_t dx = 0, dy = 0;
				ok = cursor.SignedVarint(dx) && cursor.SignedVarint(dy);
				this->LastTacticalPos.X += dx;
				this->LastTacticalPos.Y += dy;
				record.TacticalPos = this->LastTacticalPos;
				record.Flags |= FrameRecordFlag_TacticalPos;
				break;
			}

			case FrameBlock_Selection:
				ok = cursor.IDList(record.SelectedObjectIDs, MaxSelectedObjects, true);
				record.SelectedObjectCount = static_cast<int32_t>(record.SelectedObjectIDs.size());
				record.Flags |= FrameRecordFlag_Selection;
				break;

			case FrameBlock_SideChannel:
			{
				uint32_t count = 0;
				ok = cursor.Varint(count) && count <= static_cast<uint32_t>(SideChannelMaxEventsPerFrame);
				for (uint32_t i = 0; ok && i < count; ++i)
				{
					// Read every record to keep the block aligned, but keep only the ones that survive
					// validation - the rest would index out of an engine array.
					SideChannelRecord sideChannel {};
					ok = ReadSideChannelRecord(cursor, sideChannel);
					sideChannel.FrameNumber = record.FrameNumber;
					if (!ok)
						break;

					if (validateSideChannel(sideChannel))
						record.SideChannelEvents.push_back(sideChannel);
					else
						Debug::Log("[Replay] Discarded an out-of-range side-channel record during playback.\n");
				}
				record.Flags |= FrameRecordFlag_SideChannel;
				break;
			}

			case FrameBlock_GameSpeed:
			{
				uint8_t gameSpeed = 0;
				ok = cursor.Value(gameSpeed) && IsReplayGameSpeedIndexValid(gameSpeed);
				record.GameSpeed = gameSpeed;
				record.Flags |= FrameRecordFlag_GameSpeed;
				break;
			}

			case FrameBlock_SelectionTriggers:
				ok = cursor.IDList(record.SelectionTriggerObjectIDs, MaxSelectionTriggersPerFrame, false);
				record.Flags |= FrameRecordFlag_SelectionTriggers;
				break;

			default:
				// House statistics, payments and seals are for readers outside the game, and a tag this
				// build does not know is skipped whole.
				break;
			}

			if (!ok)
			{
				Debug::Log("[Replay] Frame %d has a malformed block %u.\n", record.FrameNumber, tag);
				return false;
			}
		}

		this->LastReadFrame = record.FrameNumber;
		return true;
	}

	bool FrameReader::ReadEvent(File& file, EventClass& event)
	{
		auto* raw = reinterpret_cast<uint8_t*>(&event);
		std::memset(raw, 0, sizeof(EventClass));

		uint8_t type = 0, house = 0;
		uint32_t frameDelta = 0;
		if (!file.Read(&type, 1) || !file.Read(&house, 1) || !ReadVarint(file, frameDelta))
			return false;

		raw[0] = type;
		raw[2] = house;
		event.Frame = static_cast<unsigned int>(this->LastReadFrame + ZigZagDecode(frameDelta));

		if (type == FrameInfoType)
		{
			uint8_t stored[FrameInfoStoredBytes] = {};
			if (!file.Read(stored, sizeof(stored)))
				return false;

			std::memcpy(raw + FrameInfoCommandCountOffset, stored, 2);
			raw[FrameInfoDelayOffset] = stored[2];
			raw[FrameInfoCRCOffset] = stored[3];
			return true;
		}

		int length = GetEventPayloadLength(type);
		if (length < 0)
		{
			uint32_t storedLength = 0;
			if (!ReadVarint(file, storedLength) || storedLength > MaxEventPayload)
				return false;
			length = static_cast<int>(storedLength);
		}

		return length == 0 || file.Read(raw + EventPayloadOffset, static_cast<size_t>(length));
	}

	bool FrameReader::SkipEvents(File& file, int count)
	{
		alignas(EventClass) uint8_t scratch[sizeof(EventClass)];
		for (int i = 0; i < count; ++i)
		{
			if (!this->ReadEvent(file, *reinterpret_cast<EventClass*>(scratch)))
				return false;
		}

		return true;
	}
}
