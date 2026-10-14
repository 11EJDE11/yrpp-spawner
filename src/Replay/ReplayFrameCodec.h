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

#include "ReplayFormat.h"

#include <vector>

class EventClass;

namespace Replay
{
	class File;

	struct PlaybackFrameRecord
	{
		int32_t FrameNumber = 0;
		int32_t EventCountThisFrame = 0;
		uint32_t Flags = FrameRecordFlag_None;
		Point2D TacticalPos = { 0, 0 };
		int32_t SelectedObjectCount = 0;
		std::vector<uint32_t> SelectedObjectIDs;
		std::vector<uint32_t> SelectionTriggerObjectIDs;
		std::vector<SideChannelRecord> SideChannelEvents;
		// The low 16 bits of the recorded hash; only those are stored.
		uint16_t GameCRC = 0;
		int32_t GameSpeed = 0;
		bool EndOfStream = false;
	};

	// One frame's worth of visible state, captured in the main loop and written out once the
	// queue hook knows how many events the frame carried.
	struct RecordedFrameCapture
	{
		int FrameNumber = 0;
		Point2D TacticalPos { 0, 0 };
		std::vector<uint32_t> SelectedObjectIDs;
		// Appended to during the frame rather than sampled once, because the spring happens in
		// the input pass after this capture is made and before Queue_AI writes it out.
		std::vector<uint32_t> SelectionTriggerObjectIDs;
		// Filled in later than the rest of the capture: the engine has not computed the hash
		// when this struct is created. See CaptureGameCRCForCurrentFrame.
		uint32_t GameCRC = 0;
		bool HasGameCRC = false;
		int32_t GameSpeed = 0;
		bool HasGameSpeed = false;
		// Every HouseStatsIntervalFrames, taken at the same point in the frame as GameCRC.
		std::vector<HouseStatsSample> HouseStats;
		bool HasHouseStats = false;
		// Refund_Money payments made during the frame, summed per house and caller.
		std::vector<MoneyInRecord> MoneyIn;
		// Canonical seals that closed by this frame.
		std::vector<SealRecord> Seals;
	};

	// Tracks the last serialized values so unchanged viewer state can be omitted.
	// Capturing engine state and deciding when each frame is ready belong to the caller.
	class FrameWriter
	{
	public:
		void Reset();
		bool WriteFrame(File& file, const RecordedFrameCapture& capture, int eventsThisFrame,
			const std::vector<SideChannelRecord>& sideChannelEvents);
		// The events of the frame WriteFrame has just written, in the order the engine ran them.
		bool WriteEvents(File& file, const std::vector<EventClass>& events);
		bool WriteEndMarker(File& file);

		int LastGameSpeed() const { return this->LastRecordedGameSpeed; }
		int LastFrameNumber() const { return this->LastWrittenFrameNumber; }

	private:
		bool HasLastWrittenFrameState = false;
		int32_t LastWrittenFrameNumber = -1;
		Point2D LastRecordedTacticalPos = { 0, 0 };
		std::vector<uint32_t> LastRecordedSelectionIDs;
		// -1 makes the first capture establish the recorded speed.
		int LastRecordedGameSpeed = -1;
		// Reused for each record and block so writing does not allocate on the game thread.
		std::vector<uint8_t> Record;
		std::vector<uint8_t> Block;
	};

	// Reads frame blocks without applying them to the simulation. Gameplay events follow
	// the blocks and must be read or skipped before asking for the next frame.
	class FrameReader
	{
	public:
		// Reset after restarting the file stream, including when seeking backwards.
		void Reset()
		{
			this->LastReadFrame = -1;
			this->LastTacticalPos = { 0, 0 };
		}
		// Structural checks live here; the caller supplies engine-dependent side-channel validation.
		bool ReadFrame(File& file, PlaybackFrameRecord& record,
			bool (&validateSideChannel)(SideChannelRecord&));
		// One event of the frame ReadFrame last returned. Bytes the record does not carry are zero.
		bool ReadEvent(File& file, EventClass& event);
		bool SkipEvents(File& file, int count);

	private:
		int LastReadFrame = -1;
		// Camera positions are stored as a change from the one before.
		Point2D LastTacticalPos = { 0, 0 };
		std::vector<uint8_t> Block;
	};
}
