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
#include "ReplayFile.h"

#include <Utilities/Debug.h>
#include <EventClass.h>

#include <algorithm>

namespace Replay
{
	namespace
	{
		bool SkipBytes(File& file, size_t size)
		{
			char scratch[512];

			while (size > 0)
			{
				const size_t chunk = std::min(size, sizeof(scratch));
				if (!file.Read(scratch, chunk))
					return false;

				size -= chunk;
			}

			return true;
		}
	}

	void FrameWriter::Reset()
	{
		this->HasLastWrittenFrameState = false;
		this->LastWrittenFrameNumber = 0;
		this->LastRecordedTacticalPos = { 0, 0 };
		this->LastRecordedSelectionIDs.clear();
		this->LastRecordedGameSpeed = -1;
	}

	// Optional blocks retain their on-disk order; gameplay events are appended separately.
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

		if (eventsThisFrame == 0 && !tacticalPosChanged && !selectionChanged && !hasSideChannelEvents
			&& !hasGameCRC && !capture.HasGameSpeed && !hasSelectionTriggers)
		{
			return true;
		}

		FrameRecordHeader header {};
		header.FrameNumber = capture.FrameNumber;
		header.EventCountThisFrame = eventsThisFrame;
		header.Flags = FrameRecordFlag_None;
		if (tacticalPosChanged)
			header.Flags |= FrameRecordFlag_TacticalPos;
		if (selectionChanged)
			header.Flags |= FrameRecordFlag_Selection;
		if (hasSideChannelEvents)
			header.Flags |= FrameRecordFlag_SideChannel;
		if (hasGameCRC)
			header.Flags |= FrameRecordFlag_GameCRC;
		if (capture.HasGameSpeed)
			header.Flags |= FrameRecordFlag_GameSpeed;
		if (hasSelectionTriggers)
			header.Flags |= FrameRecordFlag_SelectionTriggers;

		if (!file.Write(&header, sizeof(header)))
			return false;

		if ((header.Flags & FrameRecordFlag_TacticalPos) != 0u
			&& !file.Write(&capture.TacticalPos, sizeof(capture.TacticalPos)))
		{
			return false;
		}

		if ((header.Flags & FrameRecordFlag_Selection) != 0u)
		{
			const int32_t selectedObjectCount = static_cast<int32_t>(capture.SelectedObjectIDs.size());
			if (!file.Write(&selectedObjectCount, sizeof(selectedObjectCount)))
				return false;

			// One call for the whole block: each file write is a tdefl_compress call, and
			// box-selecting a hundred units would otherwise mean a hundred of them.
			if (!capture.SelectedObjectIDs.empty()
				&& !file.Write(capture.SelectedObjectIDs.data(),
					capture.SelectedObjectIDs.size() * sizeof(uint32_t)))
			{
				return false;
			}
		}

		if (hasSideChannelEvents)
		{
			const int32_t count = static_cast<int32_t>(sideChannelEvents.size());
			if (!file.Write(&count, sizeof(count))
				|| !file.Write(sideChannelEvents.data(),
					sideChannelEvents.size() * sizeof(SideChannelRecord)))
			{
				return false;
			}
		}

		if (hasGameCRC && !file.Write(&capture.GameCRC, sizeof(capture.GameCRC)))
			return false;


		if (capture.HasGameSpeed)
		{
			if (!file.Write(&capture.GameSpeed, sizeof(capture.GameSpeed)))
				return false;

			this->LastRecordedGameSpeed = capture.GameSpeed;
		}

		if (hasSelectionTriggers)
		{
			const auto count = static_cast<int32_t>(capture.SelectionTriggerObjectIDs.size());
			if (!file.Write(&count, sizeof(count))
				|| !file.Write(capture.SelectionTriggerObjectIDs.data(),
					capture.SelectionTriggerObjectIDs.size() * sizeof(uint32_t)))
			{
				return false;
			}
		}

		this->HasLastWrittenFrameState = true;
		this->LastWrittenFrameNumber = capture.FrameNumber;
		this->LastRecordedTacticalPos = capture.TacticalPos;
		this->LastRecordedSelectionIDs = capture.SelectedObjectIDs;

		return true;
	}

	bool FrameWriter::WriteEvents(File& file, const std::vector<EventClass>& events)
	{
		return events.empty() || file.Write(events.data(), events.size() * sizeof(EventClass));
	}

	bool FrameWriter::WriteEndMarker(File& file)
	{
		FrameRecordHeader marker {};
		marker.FrameNumber = -1;
		return file.Write(&marker, sizeof(marker));
	}

	bool FrameReader::ReadFrame(File& file, PlaybackFrameRecord& record,
		bool (&validateSideChannel)(SideChannelRecord&))
	{
		FrameRecordHeader header {};
		if (!file.Read(&header, sizeof(header)))
			return false;

		record = PlaybackFrameRecord {};
		record.FrameNumber = header.FrameNumber;
		record.EventCountThisFrame = header.EventCountThisFrame;
		record.Flags = header.Flags;

		if (record.FrameNumber == -1)
		{
			record.EndOfStream = header.EventCountThisFrame == 0
				&& header.Flags == FrameRecordFlag_None;
			return record.EndOfStream;
		}

		if (record.FrameNumber < 0
			|| record.FrameNumber <= this->LastReadFrame
			|| record.EventCountThisFrame < 0
			|| record.EventCountThisFrame > MaxEventsPerFrame)
		{
			return false;
		}

		if ((record.Flags & ~KnownFrameRecordFlags) != 0u)
		{
			Debug::Log("[Replay] Frame %d carries unknown block flags (0x%08X); "
				"the replay was recorded in a newer format than this build can read.\n",
				record.FrameNumber, record.Flags & ~KnownFrameRecordFlags);
			return false;
		}

		if ((record.Flags & FrameRecordFlag_TacticalPos) != 0u
			&& !file.Read(&record.TacticalPos, sizeof(record.TacticalPos)))
		{
			return false;
		}

		if ((record.Flags & FrameRecordFlag_Selection) != 0u)
		{
			if (!file.Read(&record.SelectedObjectCount, sizeof(record.SelectedObjectCount)))
				return false;

			if (record.SelectedObjectCount < 0 || record.SelectedObjectCount > 4096)
				return false;

			record.SelectedObjectIDs.resize(static_cast<size_t>(record.SelectedObjectCount), 0u);
			if (!record.SelectedObjectIDs.empty()
				&& !file.Read(record.SelectedObjectIDs.data(),
					record.SelectedObjectIDs.size() * sizeof(uint32_t)))
			{
				return false;
			}
		}

		if ((record.Flags & FrameRecordFlag_SideChannel) != 0u)
		{
			int32_t sideChannelCount = 0;
			if (!file.Read(&sideChannelCount, sizeof(sideChannelCount)))
				return false;

			if (sideChannelCount < 0 || sideChannelCount > SideChannelMaxEventsPerFrame)
				return false;

			// Read every record to keep the stream aligned, but keep only the ones that survive
			// validation - the rest would index out of an engine array or run off a text buffer.
			record.SideChannelEvents.reserve(static_cast<size_t>(sideChannelCount));
			for (int i = 0; i < sideChannelCount; ++i)
			{
				SideChannelRecord sideChannelRecord {};
				if (!file.Read(&sideChannelRecord, sizeof(SideChannelRecord)))
					return false;

				if (sideChannelRecord.FrameNumber == record.FrameNumber
					&& validateSideChannel(sideChannelRecord))
					record.SideChannelEvents.push_back(sideChannelRecord);
				else
					Debug::Log("[Replay] Discarded an out-of-range side-channel record during playback.\n");
			}
		}

		if ((record.Flags & FrameRecordFlag_GameCRC) != 0u
			&& !file.Read(&record.GameCRC, sizeof(record.GameCRC)))
		{
			return false;
		}

		if ((record.Flags & FrameRecordFlag_ObjectCensus) != 0u
			&& !SkipBytes(file, sizeof(FrameObjectCensus)))
		{
			return false;
		}

		if ((record.Flags & FrameRecordFlag_RandomState) != 0u
			&& !SkipBytes(file, sizeof(FrameRandomState)))
		{
			return false;
		}

		if ((record.Flags & FrameRecordFlag_GameSpeed) != 0u
			&& (!file.Read(&record.GameSpeed, sizeof(record.GameSpeed))
				|| !IsReplayGameSpeedIndexValid(static_cast<uint32_t>(record.GameSpeed))))
		{
			return false;
		}

		if ((record.Flags & FrameRecordFlag_SelectionTriggers) != 0u)
		{
			int32_t triggerCount = 0;
			if (!file.Read(&triggerCount, sizeof(triggerCount)))
				return false;

			if (triggerCount <= 0 || triggerCount > MaxSelectionTriggersPerFrame)
				return false;

			record.SelectionTriggerObjectIDs.resize(static_cast<size_t>(triggerCount), 0u);
			if (!file.Read(record.SelectionTriggerObjectIDs.data(),
				record.SelectionTriggerObjectIDs.size() * sizeof(uint32_t)))
			{
				return false;
			}
		}

		if ((record.Flags & FrameRecordFlag_Extensions) != 0u)
		{
			uint32_t extensionBytes = 0;
			if (!file.Read(&extensionBytes, sizeof(extensionBytes)))
				return false;

			if (extensionBytes > MaxFrameExtensionBytes)
			{
				Debug::Log("[Replay] Frame %d declares a %u byte extension block; refusing it.\n",
					record.FrameNumber, extensionBytes);
				return false;
			}

			if (!SkipBytes(file, extensionBytes))
				return false;
		}

		this->LastReadFrame = record.FrameNumber;
		return true;
	}

	bool FrameReader::ReadEvent(File& file, EventClass& event)
	{
		return file.Read(&event, sizeof(event));
	}

	bool FrameReader::SkipEvents(File& file, int count)
	{
		return SkipBytes(file, static_cast<size_t>(count) * sizeof(EventClass));
	}
}
