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

// TechnoClass::Get_Weapon (0x70E140) asks the unit's type for a weapon slot through
// TechnoTypeClass::Get_Weapon / Get_Elite_Weapon. Ares hooks both to serve its extra slots (18 and up);
// for the game's own 18 slots its hook hands straight back, but every call still pays for the hook's
// entry and exit, and the target scan asks several times for every object it looks at. This version
// reads the game's own slots directly and goes through the hooked functions only for the others.
//
// Measured. The Eclipsed Horizons survival game (8 players, 107,500 frames, logic only), this switched
// in and out every 250 frames: 1.054x over the whole game, 1.07-1.14x in its later stretches (a run
// with nothing switched reads 1.005x); 30 ladder games 1.03x in all. The game asks about 88,000 times a
// frame there. 14.8 billion answers were compared with the game's own way: none different.

#include "Perf.h"

#include <Utilities/Debug.h>
#include <Utilities/Macro.h>
#include <Utilities/Patch.h>

#include <FootClass.h>

namespace
{
	constexpr int OwnSlots = 18;            // TechnoTypeClass::MaxWeapons: below this the Ares hooks do nothing
	constexpr size_t SlotSize = 28;
	constexpr size_t WeaponsAt = 0x898;     // TechnoTypeClass::Get_Weapon, 0x7177C0
	constexpr size_t EliteWeaponsAt = 0xA94; // TechnoTypeClass::Get_Elite_Weapon, 0x7177E0

	using TypeWeaponFn = void*(__thiscall*)(void* pType, int slot);

	// VeterancyStruct::IsElite (0x750010): at or above the constant at 0x7E37B4.
	inline bool IsElite(const BYTE* pTechno)
	{
		return *reinterpret_cast<const float*>(pTechno + 0x150) >= *reinterpret_cast<const float*>(0x7E37B4);
	}

	inline void* TypeOf(BYTE* pTechno)
	{
		return (*reinterpret_cast<void*(__thiscall***)(void*)>(pTechno))[0x84 / 4](pTechno);
	}

	// The game's function, line for line: the way for Ares' extra slots.
	void* GameWay(BYTE* pThis, int slot)
	{
		if (slot == -1)
			return nullptr;

		if (IsElite(pThis))
		{
			auto const pWeapon = reinterpret_cast<TypeWeaponFn>(0x7177E0)(TypeOf(pThis), slot);
			if (pWeapon && *reinterpret_cast<void**>(pWeapon))
				return pWeapon;
		}

		return reinterpret_cast<TypeWeaponFn>(0x7177C0)(TypeOf(pThis), slot);
	}

	void* __fastcall GetWeapon(BYTE* pThis, void*, int slot)
	{
		if (slot >= OwnSlots)
			return GameWay(pThis, slot);

		if (slot == -1)
			return nullptr;

		if (IsElite(pThis))
		{
			auto const pWeapon = reinterpret_cast<BYTE*>(TypeOf(pThis)) + EliteWeaponsAt + slot * static_cast<int>(SlotSize);
			if (*reinterpret_cast<void**>(pWeapon))
				return pWeapon;
		}

		return reinterpret_cast<BYTE*>(TypeOf(pThis)) + WeaponsAt + slot * static_cast<int>(SlotSize);
	}
}

namespace
{
	// FNV-1a over a stretch of the game's code, with the places of known hooks left out.
	DWORD HashOf(DWORD start, DWORD end, DWORD hookAt = 0, DWORD hookLength = 0)
	{
		DWORD hash = 0x811C9DC5;
		for (DWORD address = start; address < end; ++address)
		{
			const BYTE value = address >= hookAt && address < hookAt + hookLength ? 0 : *reinterpret_cast<const BYTE*>(address);
			hash = (hash ^ value) * 0x01000193;
		}

		return hash;
	}
}

void Perf::Weapons::Apply()
{
	// The whole of TechnoClass::Get_Weapon and of the elite test it calls must be the game's code, and
	// the two type functions must read the tables this reads (their first bytes are Ares' hooks at run
	// time, so only the table offset after the hook is compared).
	if (HashOf(0x70E140, 0x70E195) != 0xA8FFDF62
		|| HashOf(0x750010, 0x750028) != 0x55DC1300
		|| *reinterpret_cast<const DWORD*>(0x7177D0) != WeaponsAt
		|| *reinterpret_cast<const DWORD*>(0x7177F0) != EliteWeaponsAt)
	{
		Debug::Log("[Perf] Weapons: TechnoClass::Get_Weapon is not as expected; leaving it alone.\n");
		return;
	}

	Patch::Apply_LJMP(0x70E140, &GetWeapon);
}
