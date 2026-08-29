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

namespace ReplaySystem
{
	bool IsPlaybackRequested();
	bool IsPlaybackActive();
	bool IsSpectatorPlayback();
	void OnGameStartReset();
	int GetViewPlayerIndex();
	void ApplyPlaybackViewPlayer();
	void ApplyPlaybackSpectator();

	// Recording taps for chat, beacons and taunts, which bypass EventClass::DoList. Safe to call
	// when not recording.
	void RecordChatMessage(int houseIndex, const wchar_t* senderName, const wchar_t* message, int colorSchemeIndex);
	void RecordTaunt(int tauntCommand);
	void RecordBeaconPlace(int houseIndex, const CoordStruct& coord, int beaconSlot);
	void RecordBeaconDelete(int houseIndex, int beaconSlot);
	void RecordBeaconText(int houseIndex, int beaconSlot, const wchar_t* text);
}
