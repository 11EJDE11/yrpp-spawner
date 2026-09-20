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

#include "ReplaySideChannels.h"
#include "ReplaySystem.h"

#include <Utilities/Debug.h>

#include <BeaconManagerClass.h>
#include <ColorScheme.h>
#include <MessageListClass.h>
#include <RulesClass.h>
#include <Unsorted.h>
#include <VocClass.h>
#include <VoxClass.h>

#include <deque>
#include <utility>

using namespace Replay;

namespace ReplaySystem
{
	namespace SideChannels
	{
		namespace
		{
			std::deque<SideChannelRecord> PendingEvents;
			std::vector<SideChannelRecord> Scratch;

			void Push(SideChannelRecord&& record)
			{
				record.FrameNumber = Unsorted::CurrentFrame;
				PendingEvents.push_back(std::move(record));
			}

			int GetChatMessageDurationFrames()
			{
				constexpr int TicksPerMinute = 900;
				constexpr double DefaultMessageDelay = 0.6; // Rules' own default, if Rules is not loaded.

				const double messageDelay = RulesClass::Instance
					? RulesClass::Instance->MessageDelay
					: DefaultMessageDelay;

				return static_cast<int>(messageDelay * TicksPerMinute);
			}
		}

		void Reset()
		{
			PendingEvents.clear();
			Scratch.clear();
		}

		const std::vector<SideChannelRecord>& DrainThroughFrame(int frameNumber)
		{
			// Keep playback parsing bounded and discard overflow from the same frame.
			std::vector<SideChannelRecord>& sideChannelForFrame = Scratch;
			sideChannelForFrame.clear();
			bool droppedSideChannelEvent = false;
			while (!PendingEvents.empty()
				&& PendingEvents.front().FrameNumber <= frameNumber)
			{
				if (sideChannelForFrame.size() < static_cast<size_t>(SideChannelMaxEventsPerFrame))
					sideChannelForFrame.push_back(PendingEvents.front());
				else
					droppedSideChannelEvent = true;

				PendingEvents.pop_front();
			}

			if (droppedSideChannelEvent)
				Debug::Log("[Replay] Dropped excess side-channel events on frame %d.\n", frameNumber);

			return Scratch;
		}

		bool ValidateRecord(SideChannelRecord& record)
		{
			record.SenderName[SideChannelNameLength - 1] = L'\0';
			record.Text[SideChannelTextLength - 1] = L'\0';

			switch (static_cast<SideChannelEventType>(record.Type))
			{
			case SideChannelEventType::ChatMessage:
				if (record.House < 0 || record.House >= MaxHouses)
					return false;

				// Indexes ColorScheme::Array in TextLabelClass; fall back to the first scheme.
				if (record.Aux < 0 || record.Aux >= ColorScheme::Array.Count)
					record.Aux = 0;

				return true;

			case SideChannelEventType::BeaconPlace:
				// -1 asks the engine to pick a free slot, which is how a local placement is recorded.
				if (record.House < 0 || record.House >= MaxHouses
					|| record.Aux < -1 || record.Aux >= MaxBeaconSlots)
				{
					return false;
				}

				return record.Coord.X >= 0 && record.Coord.X < MaxMapLeptonCoord
					&& record.Coord.Y >= 0 && record.Coord.Y < MaxMapLeptonCoord;

			case SideChannelEventType::BeaconDelete:
			case SideChannelEventType::BeaconText:
				return record.House >= 0 && record.House < MaxHouses
					&& record.Aux >= 0 && record.Aux < MaxBeaconSlots;

			case SideChannelEventType::Taunt:
				// PlayTaunt range-checks the command itself, but a value that came off disk should
				// not be bounded only by a function we do not own.
				return VoxClass::IsValidTauntCommand(record.Aux);

			default:
				return false;
			}
		}

		void ApplyRecord(const SideChannelRecord& record, bool visible, bool seeking)
		{
			if (!visible)
				return;

			const auto eventType = static_cast<SideChannelEventType>(record.Type);
			if (seeking
				&& (eventType == SideChannelEventType::ChatMessage
					|| eventType == SideChannelEventType::Taunt))
			{
				return;
			}

			switch (eventType)
			{
			case SideChannelEventType::ChatMessage:
				MessageListClass::Instance.AddMessage(
					record.SenderName, record.House, record.Text, record.Aux,
					TextPrintType::UseGradPal | TextPrintType::FullShadow | TextPrintType::Point6Grad,
					GetChatMessageDurationFrames(), false);
				if (RulesClass::Instance)
					VocClass::PlayGlobal(RulesClass::Instance->IncomingMessage, 0x2000, 1.0f);
				break;

			case SideChannelEventType::BeaconPlace:
				BeaconManagerClass::Instance.PlaceBeacon(record.House, record.Coord, record.Aux);
				break;

			case SideChannelEventType::BeaconDelete:
				BeaconManagerClass::Instance.DeleteBeacon(record.House, record.Aux);
				break;

			case SideChannelEventType::BeaconText:
				BeaconManagerClass::Instance.EditBeaconMessage(record.Text, record.House, record.Aux, true);
				break;

			case SideChannelEventType::Taunt:
				VoxClass::PlayTaunt(record.Aux);
				break;

			default:
				break;
			}
		}
	}
}

void ReplaySystem::RecordChatMessage(int houseIndex, const wchar_t* senderName, const wchar_t* message, int colorSchemeIndex)
{
	if (!ReplaySystem::IsRecordingActive())
		return;

	SideChannelRecord record {};
	record.Type = static_cast<uint8_t>(SideChannelEventType::ChatMessage);
	record.House = houseIndex;
	record.Aux = colorSchemeIndex;
	if (senderName)
		wcsncpy_s(record.SenderName, senderName, _TRUNCATE);
	if (message)
		wcsncpy_s(record.Text, message, _TRUNCATE);

	ReplaySystem::SideChannels::Push(std::move(record));
}

void ReplaySystem::RecordTaunt(int tauntCommand)
{
	if (!ReplaySystem::IsRecordingActive())
		return;

	SideChannelRecord record {};
	record.Type = static_cast<uint8_t>(SideChannelEventType::Taunt);
	record.Aux = tauntCommand;

	ReplaySystem::SideChannels::Push(std::move(record));
}

void ReplaySystem::RecordBeaconPlace(int houseIndex, const CoordStruct& coord, int beaconSlot)
{
	if (!ReplaySystem::IsRecordingActive())
		return;

	SideChannelRecord record {};
	record.Type = static_cast<uint8_t>(SideChannelEventType::BeaconPlace);
	record.House = houseIndex;
	record.Aux = beaconSlot;
	record.Coord = coord;

	ReplaySystem::SideChannels::Push(std::move(record));
}

void ReplaySystem::RecordBeaconDelete(int houseIndex, int beaconSlot)
{
	if (!ReplaySystem::IsRecordingActive())
		return;

	SideChannelRecord record {};
	record.Type = static_cast<uint8_t>(SideChannelEventType::BeaconDelete);
	record.House = houseIndex;
	record.Aux = beaconSlot;

	ReplaySystem::SideChannels::Push(std::move(record));
}

void ReplaySystem::RecordBeaconText(int houseIndex, int beaconSlot, const wchar_t* text)
{
	if (!ReplaySystem::IsRecordingActive())
		return;

	SideChannelRecord record {};
	record.Type = static_cast<uint8_t>(SideChannelEventType::BeaconText);
	record.House = houseIndex;
	record.Aux = beaconSlot;
	if (text)
		wcsncpy_s(record.Text, text, _TRUNCATE);

	ReplaySystem::SideChannels::Push(std::move(record));
}
