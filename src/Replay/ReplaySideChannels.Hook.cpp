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

#include "ReplaySystem.h"
#include "ReplayFormat.h"

#include <Utilities/Macro.h>
#include <BeaconManagerClass.h>

using namespace Replay;

#pragma region Side-channel recording taps

// Chat, beacons and taunts bypass EventClass::DoList and are recorded here.

namespace
{
	// Some local beacon calls pass -1 for both house and slot, meaning the beacon the local player
	// has selected. Resolve it before writing the replay.
	bool ResolveSelectedBeaconSlot(int& house, int& slot)
	{
		for (int h = 0; h < MaxHouses; ++h)
		{
			for (int s = 0; s < MaxBeaconSlots; ++s)
			{
				BeaconClass* pBeacon = BeaconManagerClass::Instance.Beacons[h][s];
				if (pBeacon && pBeacon->IsSelected())
				{
					house = h;
					slot = s;
					return true;
				}
			}
		}

		return false;
	}
}

// Record beacon placement.
DEFINE_HOOK(0x430BA0, BeaconManagerClass_Place_RecordPlacement, 0x6)
{
	if (ReplaySystem::IsRecordingActive())
	{
		const int house = R->Stack<int>(0x4);
		CoordStruct coord;
		coord.X = R->Stack<int>(0x8);
		coord.Y = R->Stack<int>(0xC);
		coord.Z = R->Stack<int>(0x10);
		const int slot = R->Stack<int>(0x14);

		ReplaySystem::RecordBeaconPlace(house, coord, slot);
	}

	return 0;
}

// Record beacon deletion.
DEFINE_HOOK(0x4311C0, BeaconPlacement_Delete_RecordDeletion, 0x6)
{
	if (ReplaySystem::IsRecordingActive())
	{
		int house = R->Stack<int>(0x4);
		int slot = R->Stack<int>(0x8);

		bool shouldRecord = true;
		if (house == -1 && slot == -1)
			shouldRecord = ResolveSelectedBeaconSlot(house, slot);

		if (shouldRecord)
			ReplaySystem::RecordBeaconDelete(house, slot);
	}

	return 0;
}

// BeaconManagerClass::EditBeaconMessage. Skips the local compose preview and records the final and
// remote-applied text.
DEFINE_HOOK(0x431450, BeaconPlacement_Message_RecordText, 0x6)
{
	if (ReplaySystem::IsRecordingActive())
	{
		const wchar_t* text = R->Stack<const wchar_t*>(0x4);
		int house = R->Stack<int>(0x8);
		int slot = R->Stack<int>(0xC);
		const int broadcast = R->Stack<int>(0x10);

		bool shouldRecord = broadcast != 0 || house != -1 || slot != -1;
		// Committed local text still needs its concrete beacon slot.
		if (shouldRecord && house == -1 && slot == -1)
			shouldRecord = ResolveSelectedBeaconSlot(house, slot);

		if (shouldRecord)
			ReplaySystem::RecordBeaconText(house, slot, text);
	}

	return 0;
}

// Taunt playback, which happens outside the event system.
DEFINE_HOOK(0x752B70, Taunts_RecordPlayback, 0x5)
{
	if (ReplaySystem::IsRecordingActive())
		ReplaySystem::RecordTaunt(R->ECX<int>());

	return 0;
}

#pragma endregion Side-channel recording taps
