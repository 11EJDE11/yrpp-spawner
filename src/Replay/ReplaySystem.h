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

#include <GeneralStructures.h>
#include <Misc/InGameChat.h>

#include <cstdint>

class HouseClass;
class TechnoClass;

namespace ReplaySystem
{
	bool IsPlaybackRequested();
	bool IsPlaybackActive();
	bool IsRecordingActive();
	bool IsSpectatorPlayback();
	void OnGameStartReset();
	int GetViewPlayerIndex();
	void ApplyPlaybackViewPlayer();
	void ApplyPlaybackSpectator();
	// The observer seat again, after a keyframe load. HouseClass::Observer is a bare global that no
	// savegame carries and the load rebuilds every house, so a seek leaves it on a dead one.
	void ReapplyPlaybackSpectator();

	// Hands the viewer the house being watched at the start of the next frame: playback stops and
	// the game carries on as an ordinary one, still with no network. Refused with a notice while
	// seeking, from a spectator or observer seat, or once that house has been defeated.
	void RequestTakeOver();

	// Recording taps for chat, beacons and taunts, which bypass EventClass::DoList. Safe to call
	// when not recording.
	void RecordTaunt(int tauntCommand);
	void RecordBeaconPlace(int houseIndex, const CoordStruct& coord, int beaconSlot);
	void RecordBeaconDelete(int houseIndex, int beaconSlot);
	void RecordBeaconText(int houseIndex, int beaconSlot, const wchar_t* text);

	// Statistics taps, both safe when not recording. Send_Statistics_Packet rewrites the houses'
	// built counts, so the recording's end-of-game snapshot is taken as it starts; the packet it
	// builds - what stats.dmp holds - is kept in the replay whether or not stats.dmp is written.
	void OnStatisticsPacketStarting();
	void RecordStatisticsPacket(const void* data, int length);

	// HouseClass::Refund_Money, told apart by the call site it returns to.
	void RecordIncome(HouseClass* pHouse, int amount, uintptr_t returnAddress);
	// TechnoClass::Record_The_Kill_Object / Record_The_Kill_House: the object is being lost.
	// The first only counts a loss for a type that scores; the second counts it regardless.
	void RecordObjectLost(TechnoClass* pTechno, bool scoringTypesOnly);

	// Chat fingerprints, multiplayer only: the sender puts a fresh salt in the chat packet and queues a
	// ChatFingerprint event of the message, and the message is recorded with the salt it was sent with
	// on both sides. pSalt is null for a message that carried none. See docs/replay-format.md.
	bool GenerateChatSalt(uint8_t (&salt)[16]);
	void QueueChatFingerprint(const uint8_t (&salt)[16], int houseIndex, const wchar_t* message);
	// label is a ChatLabel, recipients a bit per house index; both 0 when the sender did not say.
	void RecordChatMessage(int houseIndex, const wchar_t* senderName, const wchar_t* message, int colorSchemeIndex,
		const uint8_t* pSalt, ChatLabel label, uint8_t recipients);
}
