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

// A unit looking for something to shoot at asks about every cell within its range, one
// TechnoClass::Evaluate_Cell (0x6F8960) each, and nearly all of them hold nothing: the function looks
// the cell up, finds no object on it and says no. Each such look reads a different cell out of
// memory, and with a few thousand units scanning it is one of the larger costs of a frame.
//
// A map of one bit per cell is kept here: clear means the cell exists and has no object on it, in
// which case the answer is the game's own answer for such a cell - nothing found, the two results
// zeroed - without reading the cell. Set means anything else, and the game's code is asked.
//
// The bit is set whenever an object is put on a cell: CellClass::Overlap_Down (0x47E8A0) is the one
// place in the game that makes either list of a cell's objects non-empty. It is cleared when a set
// bit's cell is looked at and found empty. The whole map is made afresh after anything that builds
// cells or fills them some other way: a cell constructed or destroyed, a saved game loaded.
//
// For a cell with nothing on it the game's function also works out the zone the cell is in when
// asked to; that reads the map and changes nothing.
//
// Measured. The Eclipsed Horizons survival game (8 players, 107,500 frames, logic only), this switched
// in and out every 250 frames: 1.08x over the whole game (a run with nothing switched reads 1.005x).
// With Perf.Detach as well, see there. 2.9 billion questions in that game and 2.6 billion in 30 ladder
// games were each checked against the cell itself: none wrongly called empty.

#include "Perf.h"

#include <Utilities/Debug.h>
#include <Utilities/Macro.h>
#include <Utilities/Patch.h>

#include <FootClass.h>
#include <CellClass.h>
#include <MapClass.h>
#include <TechnoClass.h>

#include <Windows.h>

namespace
{
	constexpr unsigned CellCount = 0x40000;
	BYTE Occupied[CellCount / 8];
	bool Stale = true;

	inline bool HasNothing(const CellClass* pCell)
	{
		return !pCell->FirstObject && !pCell->AltObject;
	}

	void MakeAfresh()
	{
		memset(Occupied, 0xFF, sizeof(Occupied));
		auto const& cells = MapClass::Instance.Cells;
		const unsigned count = static_cast<unsigned>(cells.Capacity) < CellCount ? static_cast<unsigned>(cells.Capacity) : CellCount;
		for (unsigned i = 0; i < count; ++i)
		{
			auto const pCell = cells.Items[i];
			if (pCell && HasNothing(pCell))
				Occupied[i >> 3] &= ~(1 << (i & 7));
		}

		Stale = false;
	}

	// The game's own, entered past the jumps placed on their first instructions. Both are __thiscall,
	// so EDX holds nothing on entry; a jump through it, unlike push and ret, leaves the CPU's return
	// prediction intact for the game's own returns.
	__declspec(naked) void OriginalEvaluateCell()
	{
		__asm
		{
			mov eax, [esp + 0x14]
			push ebx
			mov edx, 0x6F8965
			jmp edx
		}
	}

	__declspec(naked) void OriginalOverlapDown()
	{
		__asm
		{
			mov al, [esp + 8]
			sub esp, 0x14
			mov edx, 0x47E8A7
			jmp edx
		}
	}

	using EvaluateCellFn = bool(__thiscall*)(TechnoClass*, int, int, CellStruct*, int, void**, int*, int);
	using OverlapDownFn = void(__thiscall*)(CellClass*, ObjectClass*, bool);

	bool __fastcall EvaluateCell(TechnoClass* pThis, void*, int method, int mask, CellStruct* pWhere, int range, void** ppObject, int* pValue, int zone)
	{
		// The index as the game works it out.
		const unsigned index = static_cast<unsigned>(pWhere->X + (pWhere->Y << 9));
		if (index < CellCount)
		{
			if (Stale)
				MakeAfresh();

			const BYTE bit = static_cast<BYTE>(1 << (index & 7));
			bool nothing = !(Occupied[index >> 3] & bit);
			if (!nothing)
			{
				// Set bits are not cleared as objects leave; they are here, when the cell is looked at.
				auto const& cells = MapClass::Instance.Cells;
				auto const pCell = index < static_cast<unsigned>(cells.Capacity) ? cells.Items[index] : nullptr;
				if (pCell && HasNothing(pCell))
				{
					Occupied[index >> 3] &= ~bit;
					nothing = true;
				}
			}

			if (nothing)
			{
				*ppObject = nullptr;
				*pValue = 0;
				return false;
			}
		}

		return reinterpret_cast<EvaluateCellFn>(&OriginalEvaluateCell)(pThis, method, mask, pWhere, range, ppObject, pValue, zone);
	}

	void __fastcall OverlapDown(CellClass* pCell, void*, ObjectClass* pObject, bool onBridge)
	{
		const unsigned index = static_cast<unsigned>(pCell->MapCoords.X + (pCell->MapCoords.Y << 9));
		auto const& cells = MapClass::Instance.Cells;
		if (index < CellCount && index < static_cast<unsigned>(cells.Capacity) && cells.Items[index] == pCell)
			Occupied[index >> 3] |= static_cast<BYTE>(1 << (index & 7));
		else
			Stale = true; // not where its coordinates say: nothing can be said about the map until it is made afresh

		reinterpret_cast<OverlapDownFn>(&OriginalOverlapDown)(pCell, pObject, onBridge);
	}
}

namespace
{
	// The hooks Phobos has in Evaluate_Cell: one that notes who owns the transport the scanner rides in,
	// and two that newer builds add to the handling of an object found on the cell. None of them is
	// reached, or changes anything, for a cell with nothing on it.
	const Perf::KnownHook KnownHooks[] = {
		{ 0x6F89F4, 6, L"Phobos.dll", "TechnoClass_ThreatEvals_OpenToppedOwner" },
		{ 0x6F8A92, 10, L"Phobos.dll", "TechnoClass_CheckAutoTarget_AttackFriendlies" },
		{ 0x6F8BBC, 10, L"Phobos.dll", "TechnoClass_CheckAutoTarget_AttackFriendlies" },
	};

	// FNV-1a over a stretch of the game's code, with the places of those hooks left out, so that it
	// comes out the same whether or not they are there.
	DWORD HashOf(DWORD start, DWORD end)
	{
		DWORD hash = 0x811C9DC5;
		for (DWORD address = start; address < end; ++address)
		{
			const BYTE value = Perf::InKnownHook(address, KnownHooks) ? 0 : *reinterpret_cast<const BYTE*>(address);
			hash = (hash ^ value) * 0x01000193;
		}

		return hash;
	}
}

DEFINE_HOOK(0x47BBF0, CellClass_CTOR_EmptyCellsStale, 0x6)
{
	Stale = true;
	return 0;
}

DEFINE_HOOK(0x47BB60, CellClass_DTOR_EmptyCellsStale, 0x6)
{
	Stale = true;
	return 0;
}

DEFINE_HOOK(0x67E68A, LoadGame_EmptyCellsStale, 0x5)
{
	Stale = true;
	return 0;
}

void Perf::EmptyCells::Apply()
{
	static const BYTE evaluateCell[] = { 0x8B, 0x44, 0x24, 0x14, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x57 };
	static const BYTE overlapDown[] = { 0x8A, 0x44, 0x24, 0x08, 0x83, 0xEC, 0x14, 0x84, 0xC0, 0x53 };
	// For a cell with nothing on it the whole of Evaluate_Cell is passed over, so the whole of it has to
	// be what was read: the game's code, with nothing in it but the hooks listed above, placed by Phobos. Overlap_Down is
	// still run in full after the bit is set, so only its first bytes, which the jump replaces, matter.
	if (memcmp(reinterpret_cast<const void*>(0x6F8960), evaluateCell, sizeof(evaluateCell)) != 0
		|| memcmp(reinterpret_cast<const void*>(0x47E8A0), overlapDown, sizeof(overlapDown)) != 0
		|| HashOf(0x6F8960, 0x6F8C10) != 0xDEC75DFF)
	{
		Debug::Log("[Perf] EmptyCells: Evaluate_Cell or Overlap_Down is not as expected; leaving them alone.\n");
		return;
	}

	if (!Perf::OnlyKnownHooks(0x6F8960, 0x6F8C10, KnownHooks, "EmptyCells"))
		return;

	Patch::Apply_LJMP(0x47E8A0, &OverlapDown);
	Patch::Apply_LJMP(0x6F8960, &EvaluateCell);
}
