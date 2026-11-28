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
#include <TechnoClass.h>
#include <TechnoTypeClass.h>

namespace
{
	static_assert(sizeof(WeaponStruct) == 28);

	// The game's function, line for line: the way for Ares' extra slots. VeterancyStruct::IsElite
	// (0x750010) compares with the game's 2.0f at 0x7E37B4, as YRpp's IsElite does.
	WeaponStruct* GameWay(TechnoClass* pThis, int slot)
	{
		if (slot == -1)
			return nullptr;

		if (pThis->Veterancy.IsElite())
		{
			auto const pWeapon = pThis->GetTechnoType()->GetEliteWeapon(slot);
			if (pWeapon && pWeapon->WeaponType)
				return pWeapon;
		}

		return pThis->GetTechnoType()->GetWeapon(slot);
	}

	WeaponStruct* __fastcall GetWeapon(TechnoClass* pThis, void*, int slot)
	{
		// Ares' hooks only act from slot 18 (TechnoTypeClass::MaxWeapons) up.
		if (slot >= TechnoTypeClass::MaxWeapons)
			return GameWay(pThis, slot);

		if (slot == -1)
			return nullptr;

		auto const pType = pThis->GetTechnoType();
		if (pThis->Veterancy.IsElite() && pType->EliteWeapon[slot].WeaponType)
			return &pType->EliteWeapon[slot];

		return &pType->Weapon[slot];
	}

	// FNV-1a over a stretch of the game's code.
	DWORD HashOf(DWORD start, DWORD end)
	{
		DWORD hash = 0x811C9DC5;
		for (DWORD address = start; address < end; ++address)
			hash = (hash ^ *reinterpret_cast<const BYTE*>(address)) * 0x01000193;

		return hash;
	}

	// Ares 3.0p1's hooks on the two type functions, which serve its extra slots and hand the game's own
	// straight back.
	const Perf::KnownHook AresWeaponHooks[] = {
		{ 0x7177C0, 11, L"Ares.dll", "TechnoTypeClass_GetWeapon" },
		{ 0x7177E0, 11, L"Ares.dll", "TechnoTypeClass_GetEliteWeapon" },
	};
}

void Perf::Weapons::Apply()
{
	// The whole of TechnoClass::Get_Weapon and of the elite test it calls must be the game's code. The
	// two type functions must read the tables read here (their first bytes are Ares' hooks at run time,
	// so only the table offset after the hook is compared), and be hooked by Ares alone.
	if (HashOf(0x70E140, 0x70E195) != 0xA8FFDF62
		|| HashOf(0x750010, 0x750028) != 0x55DC1300
		|| *reinterpret_cast<const DWORD*>(0x7177D0) != offsetof(TechnoTypeClass, Weapon)
		|| *reinterpret_cast<const DWORD*>(0x7177F0) != offsetof(TechnoTypeClass, EliteWeapon))
	{
		Debug::Log("[Perf] Weapons: TechnoClass::Get_Weapon is not as expected; leaving it alone.\n");
		return;
	}

	if (!Perf::OnlyKnownHooks(0x7177C0, 0x7177F8, AresWeaponHooks, "Weapons"))
		return;

	Patch::Apply_LJMP(0x70E140, &GetWeapon);
}
