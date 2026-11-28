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

// Whenever an object goes away - a shell lands, an animation ends, a unit dies - the game's
// Detach_This_From_All (0x7258D0) calls the Detach of every object there is, so that whichever of
// them still points at it can let go. Nearly none ever do, but each call runs down six classes of
// comparisons and through the hooks other extensions have put in them, a few hundred cycles for
// every unit, soldier and building in the game, for every shell. In a long game with thousands of
// objects and a battle going on that was a third of the whole frame, and it grows with the size of
// everyone's base and army.
//
// Three things are done about it, none of which changes what any Detach does or the order they
// run in. The first is a matter of reading the game's code; the other two each rest on one stated
// thing more, and on a shell or an animation always being announced a last time, in full, by its
// destructor.
//
// For the classes that fill that list - units, infantry, aircraft, buildings, terrain objects,
// animations, shells, particles and their systems, overlays, smudges, factories - the question is
// asked first: does this object hold the departing pointer in any of the places its Detach looks?
// The same fields, read straight from the object, and only when one matches (or the object has one
// of the rarer attachments whose handling is left to the game) is its Detach called, which then does
// exactly what it always did. Everything else in the list is called as before.
//
// That still reads every object once per departure. Most departures, though, come together: at the
// end of each frame Remove_All_Inactive (0x725C70) deletes everything that died during it, one
// object after another with nothing in between but their destructors. So the first shell or
// animation deleted in such a sweep has the whole list read once for all of them: every object that
// mentions any shell or animation still waiting to be deleted is noted, and from then on in that
// sweep only the noted objects (and any that were not there to be looked at) are asked. This rests
// on one thing beyond the reading of the Detach functions: that while the sweep runs nothing takes
// up a new pointer to a shell or animation already waiting to be deleted. Destructors have no
// reason to. And should any Detach in the sweep find a pointer to act on, what it then does is not
// trusted either: the notes are thrown away and made afresh.
//
// The other half of the departures are the same shells and animations a little earlier: each is
// also announced when it leaves the map, in the middle of the frame, where nothing can be read once
// for many. There units and buildings are looked at only where a pointer to such an object can be
// (see CaresOnLeaving).
//
// Newer Phobos builds hook FootClass::Detach at 0x4D9992, and that hook also lets go of two pointers
// Phobos keeps for a unit outside the game's object, which nothing here can see, so a unit cannot
// simply be passed over. With such a Phobos that one hook is run for every unit that is passed over,
// handed the registers and the stack the game has at that address, so Phobos' own code does whatever
// it does there; the rest of the unit's Detach, the larger part of the cost, is still left out.
//
// Measured. The Eclipsed Horizons survival game (8 players, 107,500 frames, logic only), this switched
// in and out every 250 frames: 1.23x over the whole game, 1.40x through frames 72,000-84,000. With the
// empty cell map (Perf.EmptyCells) as well: 1.28x over the game (1.38x at 72,000-84,000, 27.9 -> 20.2
// ms a frame), 1.19x with drawing on (36.5 -> 27.8 ms at 72,000-84,000); 30 ladder games 0.97-1.15x,
// median 1.055x, the larger the game the more. A run with nothing switched reads 1.005x without drawing
// and 1.03x with. Every frame's CRC as recorded in all of them.

#include "Perf.h"

#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <AbstractClass.h>
#include <Notifications.h>

#include <Windows.h>
#include <emmintrin.h>
#include <cstring>

namespace
{
	// The lists Detach_This_From_All walks: every object that wants to hear when an object goes away,
	// and the ones that want to hear about animations.
	enum class Listeners { Objects, Anims };

	// Set for the call in hand: Detach_This_From_All was called as an object left the map.
	bool FromLimbo = false;

	using Vector = DynamicVectorClass<AbstractClass*>;

	Vector* ListOf(Listeners list)
	{
		return &(list == Listeners::Objects ? PointerExpiredNotification::NotifyInvalidObject : PointerExpiredNotification::NotifyInvalidAnim).Array;
	}

	// An object as the code here reads it: its first word is its vtable.
	inline void** At(const Vector* vector, int i) { return reinterpret_cast<void**>(vector->Items[i]); }

	// The flag is handed on exactly as the game's loop pushes it, a whole register of which only
	// the low byte is the flag.
	using DetachFn = void(__thiscall*)(void*, void*, DWORD);
	inline void Tell(void** pObject, void* pTarget, DWORD removed)
	{
		reinterpret_cast<DetachFn>((*reinterpret_cast<void***>(pObject))[10])(pObject, pTarget, removed);
	}

	constexpr DWORD UnitVTable = 0x7F5C70, InfantryVTable = 0x7EB058, BuildingVTable = 0x7E3EBC, AircraftVTable = 0x7E22A4;
	constexpr DWORD TerrainVTable = 0x7F522C, AnimVTable = 0x7E3354, BulletVTable = 0x7E46E4, ParticleSystemVTable = 0x7EFB9C;
	constexpr DWORD ParticleVTable = 0x7EF954, OverlayVTable = 0x7EF3D4, SmudgeVTable = 0x7F32FC, FactoryVTable = 0x7E88D0;

	using Object = const BYTE*;
	inline DWORD Field(Object o, size_t offset) { return *reinterpret_cast<const DWORD*>(o + offset); }

	// The pointers an object's Detach compares, as runs of 32-bit values in the object (From up to
	// To) and lists outside it. Whatever else lies in a run is looked at along with the pointers: a
	// chance match there only means the object's Detach gets called, as it always was, and finds
	// nothing. The same goes for the few fields the game only clears when the target is gone for good.
	struct Run { unsigned short From, To; };

	// ObjectClass::Detach (0x5F5230): the next object in the cell (+0x30), the tag (+0x34), the
	// parachute (+0x88).
	// TechnoClass::Detach (0x7077C0): the transporter (+0x11C), the animation behind it and the
	// deploy animation (+0x12C, +0x130); drain target, drainer and their animation (+0x1CC..+0x1D4);
	// the archived target and the house (+0x218, +0x21C); the temporal aimed at it (+0x278); the
	// airstrike (+0x294); +0x2A8..+0x2E7 the unit rocking it, the locomotor pair, what it is and was
	// shooting at, the mind-control animation, the houses and owners behind a mind control or a
	// spawn, the bunker; its eight particle systems and the wave (+0x304..+0x327); what moves it
	// about and its old team (+0x428, +0x434); the building a unit is tied to (+0x500).
	constexpr Run TechnoRuns[] = {
		{ 0x28, 0x38 }, { 0x80, 0x90 }, { 0x110, 0x120 }, { 0x124, 0x134 }, { 0x1C8, 0x1D8 }, { 0x210, 0x220 }, { 0x270, 0x280 },
		{ 0x290, 0x2A0 }, { 0x2A8, 0x2E8 }, { 0x304, 0x314 }, { 0x314, 0x324 }, { 0x318, 0x328 }, { 0x428, 0x438 }, { 0x4F8, 0x508 },
	};
	// FootClass::Detach (0x4D9960): where it is going and was going (+0x5A4, +0x5A8), the two
	// attack-move targets (+0x5C8, +0x5CC), its team and the next member (+0x5D4, +0x5D8). Then the
	// last pointers of UnitClass (0x7446E0: +0x6C4, +0x6C8), InfantryClass (0x51AA10: +0x6C0) and
	// AircraftClass (0x41B660: +0x6C4, +0x6CC).
	constexpr Run FootRuns[] = { { 0x5A0, 0x5B0 }, { 0x5C8, 0x5D8 }, { 0x5CC, 0x5DC }, { 0x6C0, 0x6D0 } };
	// BuildingClass::Detach (0x44E8F0) and the slots BuildingClass::Detach_Animation (0x451B40) looks
	// through: its type and factory (+0x520, +0x524), who to repay and the firestorm animation
	// (+0x540, +0x548), its 21 animations (+0x55C..+0x5AF), 8 fires (+0x5C8), 3 upgrades (+0x5EC),
	// light (+0x600) and light source (+0x614).
	constexpr Run BuildingRuns[] = {
		{ 0x520, 0x530 }, { 0x540, 0x550 }, { 0x55C, 0x59C }, { 0x59C, 0x5AC }, { 0x5A0, 0x5B0 }, { 0x5C8, 0x5E8 }, { 0x5EC, 0x5FC },
		{ 0x5F4, 0x604 }, { 0x608, 0x618 },
	};
	constexpr Run ObjectRuns[] = { { 0x28, 0x38 }, { 0x80, 0x90 } };
	// TerrainClass::Detach (0x71CFD0): one more pointer, +0xC8.
	constexpr Run TerrainRuns[] = { { 0x28, 0x38 }, { 0x80, 0x90 }, { 0xC0, 0xD0 } };
	// AnimClass::Detach (0x425150): its owner and what it is attached to (+0xC8, +0xCC), two more (+0x17C, +0x180).
	constexpr Run AnimRuns[] = { { 0x28, 0x38 }, { 0x80, 0x90 }, { 0xC0, 0xD0 }, { 0x178, 0x188 } };
	// BulletClass::Detach (0x4684E0): type and owner (+0xAC, +0xB0), target (+0x10C), two more (+0x130, +0x154).
	constexpr Run BulletRuns[] = { { 0x28, 0x38 }, { 0x80, 0x90 }, { 0xA8, 0xB8 }, { 0x100, 0x110 }, { 0x124, 0x134 }, { 0x148, 0x158 } };
	// ParticleSystemClass::Detach (0x62FE90): owner (+0xAC), two targets (+0xE0, +0xE4), and its particles.
	constexpr Run ParticleSystemRuns[] = { { 0x28, 0x38 }, { 0x80, 0x90 }, { 0xA8, 0xB8 }, { 0xD8, 0xE8 } };
	// FactoryClass::Detach (0x4CA580): what it is building, +0x58.
	constexpr Run FactoryRuns[] = { { 0x50, 0x60 } };

	// What is being looked for: one departing object, or any of a set of them.
	struct One
	{
		DWORD Target;
		__m128i Wide;

		explicit One(DWORD target) : Target(target), Wide(_mm_set1_epi32(static_cast<int>(target))) { }
		bool Wanted(DWORD value) const { return value == Target; }

		template <size_t N>
		bool InRuns(Object o, const Run(&runs)[N]) const
		{
			__m128i matches = _mm_setzero_si128();
			for (auto const& run : runs)
			{
				for (size_t at = run.From; at < run.To; at += 16)
					matches = _mm_or_si128(matches, _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(o + at)), Wide));
			}

			return _mm_movemask_epi8(matches) != 0;
		}
	};

	constexpr int TableBits = 18;
	inline unsigned Slot(DWORD pointer) { return (pointer * 0x9E3779B1u) >> (32 - TableBits); }
	struct BitTable
	{
		BYTE Bits[1 << (TableBits - 3)];

		void Clear() { memset(Bits, 0, sizeof(Bits)); }
		void Set(DWORD pointer) { const unsigned slot = Slot(pointer); Bits[slot >> 3] |= static_cast<BYTE>(1u << (slot & 7)); }
		void Reset(DWORD pointer) { const unsigned slot = Slot(pointer); Bits[slot >> 3] &= static_cast<BYTE>(~(1u << (slot & 7))); }
		// Never for a null field, whose slot a waiting pointer may share.
		bool Has(DWORD pointer) const { const unsigned slot = Slot(pointer); return ((Bits[slot >> 3] >> (slot & 7)) & 1) & static_cast<unsigned>(pointer != 0); }
	};

	struct Many
	{
		const BitTable* Members;

		bool Wanted(DWORD value) const { return Members->Has(value); }

		template <size_t N>
		bool InRuns(Object o, const Run(&runs)[N]) const
		{
			for (auto const& run : runs)
			{
				for (size_t at = run.From; at < run.To; at += 4)
				{
					if (Members->Has(Field(o, at)))
						return true;
				}
			}

			return false;
		}
	};

	template <typename What>
	inline bool Holds(Object items, int count, const What& what)
	{
		for (int i = 0; i < count; ++i)
		{
			if (what.Wanted(Field(items, i * 4)))
				return true;
		}

		return false;
	}

	// A DynamicVectorClass of pointers inside the object: its items at +4, its count at +0x10.
	template <typename What>
	inline bool VectorHolds(Object o, size_t vector, const What& what)
	{
		const int count = static_cast<int>(Field(o, vector + 0x10));
		return count > 0 && Holds(reinterpret_cast<Object>(Field(o, vector + 4)), count, what);
	}

	enum : int { No = 0, Ask = 1, Match = 2 };

	// TechnoClass::Detach and the classes under it - RadioClass (0x65AAC0), ObjectClass, CargoClass
	// (0x4734B0) - beyond the runs: Match when one of its lists holds the target, Ask when it has an
	// attachment that is handed the target in turn.
	template <typename What>
	inline int TechnoLists(Object o, const What& what, bool removed)
	{
		// The lists of what it is and has been attacking.
		if (VectorHolds(o, 0x458, what) || VectorHolds(o, 0x470, what))
			return Match;

		if (removed)
		{
			// Its contacts, and its passengers: a chain through each one's next object.
			const int contacts = static_cast<int>(Field(o, 0xE8));
			if (contacts > 0 && Holds(reinterpret_cast<Object>(Field(o, 0xE4)), contacts, what))
				return Match;

			for (Object rider = reinterpret_cast<Object>(Field(o, 0x118)); rider; rider = reinterpret_cast<Object>(Field(rider, 0x30)))
			{
				if (what.Wanted(reinterpret_cast<DWORD>(rider)))
					return Match;
			}
		}

		// A spawn manager, a temporal of its own, an airstrike or (when the target is gone for good) a
		// capture manager: those are few, and their owners are simply asked.
		return (Field(o, 0x2D0) | Field(o, 0x274) | Field(o, 0x294) | (removed ? Field(o, 0x2BC) : 0)) ? Ask : No;
	}

	template <typename What>
	inline int FootCares(Object o, const What& what, bool removed)
	{
		// Itself; its queued way points, two lists.
		if (what.Wanted(reinterpret_cast<DWORD>(o)) || what.InRuns(o, TechnoRuns) || what.InRuns(o, FootRuns))
			return Match;

		const int queued = static_cast<int>(Field(o, 0x5BC)), planned = static_cast<int>(Field(o, 0x598));
		if (queued > 0 && Holds(reinterpret_cast<Object>(Field(o, 0x5B0)), queued, what))
			return Match;
		if (planned > 0 && Holds(reinterpret_cast<Object>(Field(o, 0x58C)), planned, what))
			return Match;

		// While a parasite is on it, the parasite is handed the target in turn.
		const int lists = TechnoLists(o, what, removed);
		return lists ? lists : Field(o, 0x694) ? Ask : No;
	}

	template <typename What>
	inline int BuildingCares(Object o, const What& what, bool removed)
	{
		if (what.Wanted(reinterpret_cast<DWORD>(o)) || what.InRuns(o, TechnoRuns) || what.InRuns(o, BuildingRuns))
			return Match;

		// Whoever is overpowering it and its occupants.
		if (VectorHolds(o, 0x66C, what) || VectorHolds(o, 0x684, what))
			return Match;

		return TechnoLists(o, what, removed);
	}

	// Whether this object's Detach could do anything about what is wanted. bulletsAsked is for the
	// one case where it is the target and not the listener that decides: a shell told of the removal
	// of a shell whose type is scaled takes it off the game's list of those (BulletClass::Detach's tail).
	template <typename What>
	inline int Cares(void** pObject, const What& what, bool removed, bool bulletsAsked)
	{
		Object const o = reinterpret_cast<Object>(pObject);
		switch (Field(o, 0))
		{
		case UnitVTable:
		case InfantryVTable:
		case AircraftVTable:
			return FootCares(o, what, removed);
		case BuildingVTable:
			return BuildingCares(o, what, removed);
		case TerrainVTable:
			return what.InRuns(o, TerrainRuns) ? Match : No;
		case AnimVTable:
			return what.InRuns(o, AnimRuns) ? Match : No;
		case BulletVTable:
			return what.InRuns(o, BulletRuns) ? Match : bulletsAsked ? Ask : No;
		case ParticleSystemVTable:
			return what.InRuns(o, ParticleSystemRuns) || VectorHolds(o, 0xBC, what) ? Match : No;
		case ParticleVTable:
		case OverlayVTable:
		case SmudgeVTable:
			// These have ObjectClass's Detach as it is.
			return what.InRuns(o, ObjectRuns) ? Match : No;
		case FactoryVTable:
			return what.InRuns(o, FactoryRuns) ? Match : No;
		default:
			return Ask;
		}
	}

	// A shell or an animation is announced twice: when it leaves the map, in the middle of the frame
	// (ObjectClass::Detach_All, from Limbo), and again when it is deleted. The first time, units and
	// buildings are only looked at where a pointer to such an object can be: the fields that take any
	// object - the next object in the cell, the archived target, the target and the last one, where a
	// unit is going and its queued way points, what it is and has been attacking - and, for an
	// animation, the fields that hold animations. A field declared for a house, a team, another unit
	// does not hold a shell. The second announcement looks at everything as before, so even a pointer
	// that had no business being where it was is let go of before the object's memory is.
	constexpr Run AnyObjectRuns[] = { { 0x28, 0x38 }, { 0x210, 0x220 }, { 0x2B0, 0x2C0 } };
	constexpr Run AnyObjectFootRuns[] = { { 0x5A0, 0x5B0 }, { 0x5C8, 0x5D8 } };
	// The parachute (+0x88), the animation behind and the deploy animation (+0x12C, +0x130), the drain
	// animation (+0x1D4), the mind-control animation (+0x2C8).
	constexpr Run AnimRunsOfTechno[] = { { 0x80, 0x90 }, { 0x124, 0x134 }, { 0x1C8, 0x1D8 }, { 0x2C0, 0x2D0 } };
	// A building's firestorm and psychic warning animations (+0x548, +0x54C), its 21 animations and 8 fires.
	constexpr Run AnimRunsOfBuilding[] = { { 0x540, 0x550 }, { 0x55C, 0x59C }, { 0x59C, 0x5AC }, { 0x5A0, 0x5B0 }, { 0x5C8, 0x5E8 } };

	inline int CaresOnLeaving(void** pObject, const One& what, bool anim, bool removed, bool bulletsAsked)
	{
		Object const o = reinterpret_cast<Object>(pObject);
		const DWORD vtable = Field(o, 0);
		const bool foot = vtable == UnitVTable || vtable == InfantryVTable || vtable == AircraftVTable;
		if (!foot && vtable != BuildingVTable)
			return Cares(pObject, what, removed, bulletsAsked);

		if (what.InRuns(o, AnyObjectRuns) || (anim && what.InRuns(o, AnimRunsOfTechno)))
			return Match;

		if (foot)
		{
			if (what.InRuns(o, AnyObjectFootRuns))
				return Match;

			const int queued = static_cast<int>(Field(o, 0x5BC)), planned = static_cast<int>(Field(o, 0x598));
			if (queued > 0 && Holds(reinterpret_cast<Object>(Field(o, 0x5B0)), queued, what))
				return Match;
			if (planned > 0 && Holds(reinterpret_cast<Object>(Field(o, 0x58C)), planned, what))
				return Match;
		}
		else if (anim && what.InRuns(o, AnimRunsOfBuilding))
		{
			return Match;
		}

		if (VectorHolds(o, 0x458, what) || VectorHolds(o, 0x470, what))
			return Match;

		// A spawn manager and an airstrike have targets of their own.
		return (Field(o, 0x2D0) | Field(o, 0x294)) ? Ask : No;
	}

	inline bool IsScalableBullet(void* pTarget)
	{
		auto const whatAmI = (*reinterpret_cast<int(__thiscall***)(void*)>(pTarget))[11];
		if (whatAmI(pTarget) != 8)
			return false;

		Object const type = reinterpret_cast<Object>(Field(static_cast<Object>(pTarget), 0xAC));
		return type && type[0x2EC] != 0;
	}

	// Phobos' hook at 0x4D9992 in FootClass::Detach, run for the units passed over; null when it is not there.
	struct HookRegisters { DWORD Origin, Flags, EDI, ESI, EBP, ESP, EBX, EDX, ECX, EAX; };
	using HookFn = DWORD(__cdecl*)(HookRegisters*);
	HookFn FootHook = nullptr;
	constexpr DWORD FootHookAt = 0x4D9992, FootHookSkipsTo = 0x4D99D3;

	// Runs it for one unit whose Detach would do nothing: registers and stack as FootClass::Detach has
	// them there (ESI the unit, EDI the target, EBX the flag, EBP zero, EAX the parasite on it - none,
	// or the unit would have been asked). False when the hook answers in a way not allowed for, in which
	// case the unit's Detach is called after all.
	bool RunFootHook(void** pObject, void* pTarget, DWORD removedFlag)
	{
		Object const o = reinterpret_cast<Object>(pObject);
		const DWORD target = reinterpret_cast<DWORD>(pTarget), unit = reinterpret_cast<DWORD>(pObject);
		// Four saved registers, twelve bytes of locals, the return address, the two arguments.
		DWORD frame[10] = { 0, 0, 0, 0, 0, 0, 0, 0, target, removedFlag };
		HookRegisters registers { FootHookAt, 0, target, unit, 0, reinterpret_cast<DWORD>(frame), removedFlag, 0, 0, Field(o, 0x694) };
		const DWORD parasite = registers.EAX;
		const DWORD next = FootHook(&registers);
		return (next == 0 || next == FootHookSkipsTo) && registers.ESI == unit && registers.EDI == target && registers.EBX == removedFlag
			&& registers.EBP == 0 && registers.EAX == parasite && frame[8] == target && frame[9] == removedFlag && Field(o, 0x694) == parasite;
	}

	// An object whose Detach is not being called. True if it was called after all.
	inline bool PassOver(void** pObject, void* pTarget, DWORD removedFlag)
	{
		if (!FootHook)
			return false;

		const DWORD vtable = Field(reinterpret_cast<Object>(pObject), 0);
		if ((vtable != UnitVTable && vtable != InfantryVTable && vtable != AircraftVTable) || RunFootHook(pObject, pTarget, removedFlag))
			return false;

		Tell(pObject, pTarget, removedFlag);
		return true;
	}

	// One object at a time: everything in the list is looked at for this target.
	void NotifyEach(Vector* vector, void* pTarget, DWORD removedFlag, int from)
	{
		const bool removed = static_cast<BYTE>(removedFlag) != 0;
		const bool bulletsAsked = removed && IsScalableBullet(pTarget);
		const One what(reinterpret_cast<DWORD>(pTarget));
		// The list is read afresh each time round, as the game does: a Detach may change it.
		for (int i = from; i < vector->Count; ++i)
		{
			void** const pObject = At(vector, i);
			if (Cares(pObject, what, removed, bulletsAsked))
				Tell(pObject, pTarget, removedFlag);
			else
				PassOver(pObject, pTarget, removedFlag);
		}
	}

	// Every listener that was there when the lists were last read, and whether it is to be asked: an
	// open-addressed table of their addresses, the low bit (always clear in an address) set on the
	// ones to ask.
	struct NoteTable
	{
		static constexpr unsigned Size = 1u << 15;
		static constexpr DWORD Gone = 2;
		DWORD Entries[Size];

		void Clear() { memset(Entries, 0, sizeof(Entries)); }
		static unsigned Start(DWORD pointer) { return (pointer * 0x9E3779B1u) >> 17; }

		void Add(DWORD pointer, bool ask)
		{
			for (unsigned at = Start(pointer);; at = (at + 1) & (Size - 1))
			{
				if (!Entries[at] || (Entries[at] & ~1u) == pointer)
				{
					Entries[at] = pointer | (ask || (Entries[at] & 1) ? 1u : 0u);
					return;
				}
			}
		}

		// 0 = not there when the lists were read, 1 = there and to be passed over, 2 = there and to be asked.
		int Find(DWORD pointer) const
		{
			for (unsigned at = Start(pointer); Entries[at]; at = (at + 1) & (Size - 1))
			{
				if ((Entries[at] & ~1u) == pointer)
					return 1 + static_cast<int>(Entries[at] & 1);
			}

			return 0;
		}

		void Remove(DWORD pointer)
		{
			for (unsigned at = Start(pointer); Entries[at]; at = (at + 1) & (Size - 1))
			{
				if ((Entries[at] & ~1u) == pointer)
				{
					Entries[at] = Gone;
					return;
				}
			}
		}
	};

	void NotifyLeaving(Vector* vector, void* pTarget, DWORD removedFlag, bool anim)
	{
		const bool removed = static_cast<BYTE>(removedFlag) != 0;
		const bool bulletsAsked = removed && IsScalableBullet(pTarget);
		const One what(reinterpret_cast<DWORD>(pTarget));
		for (int i = 0; i < vector->Count; ++i)
		{
			void** const pObject = At(vector, i);
			if (CaresOnLeaving(pObject, what, anim, removed, bulletsAsked))
				Tell(pObject, pTarget, removedFlag);
			else
				PassOver(pObject, pTarget, removedFlag);
		}
	}

	// The sweep. Waiting holds the shells and animations Remove_All_Inactive has yet to delete as of
	// the last time the lists were read; Notes, every listener that was there to be read then, marked
	// if it mentioned one of them (or has to be asked whatever the target).
	bool InSweep = false, NotesValid = false;
	int SweepWorth = 0, Rebuilds = 0;
	unsigned Generation = 0;
	BitTable Waiting;
	NoteTable Notes;
	constexpr int MaxWaiting = 4096;
	DWORD WaitingList[MaxWaiting];
	int WaitingCount = 0;

	inline bool IsShellOrAnim(DWORD pointer)
	{
		const DWORD vtable = *reinterpret_cast<const DWORD*>(pointer);
		return vtable == BulletVTable || vtable == AnimVTable;
	}

	Vector* const Pending = &AbstractClass::PendingDelete;

	// How many shells and animations the sweep about to start will delete: with only a couple, reading
	// the lists once per deletion is the cheaper way.
	int CountWaiting()
	{
		int count = 0;
		for (int i = 0; i < Pending->Count; ++i)
		{
			auto const pointer = reinterpret_cast<DWORD>(Pending->Items[i]);
			if (IsShellOrAnim(pointer) && !reinterpret_cast<Object>(pointer)[0x90])
				++count;
		}

		return count;
	}

	bool IsWaiting(DWORD pointer)
	{
		if (!Waiting.Has(pointer))
			return false;

		for (int i = 0; i < WaitingCount; ++i)
		{
			if (WaitingList[i] == pointer)
				return true;
		}

		return false;
	}

	void TakeNotes(DWORD current)
	{
		++Generation;
		++Rebuilds;
		WaitingCount = 0;
		NotesValid = false;
		if (ListOf(Listeners::Objects)->Count + ListOf(Listeners::Anims)->Count > static_cast<int>(NoteTable::Size / 2))
			return;

		Waiting.Clear();
		Notes.Clear();
		WaitingList[WaitingCount++] = current;
		Waiting.Set(current);
		for (int i = 0; i < Pending->Count; ++i)
		{
			auto const pointer = reinterpret_cast<DWORD>(Pending->Items[i]);
			if (pointer != current && IsShellOrAnim(pointer) && !reinterpret_cast<Object>(pointer)[0x90])
			{
				if (WaitingCount == MaxWaiting)
					return;

				WaitingList[WaitingCount++] = pointer;
				Waiting.Set(pointer);
			}
		}

		const Many what { &Waiting };
		for (auto const list : { Listeners::Objects, Listeners::Anims })
		{
			Vector* const vector = ListOf(list);
			for (int i = 0; i < vector->Count; ++i)
			{
				void** const pObject = At(vector, i);
				// Gone for good is what the sweep's targets are, and scaled shells are few enough to
				// have every shell asked about them.
				Notes.Add(reinterpret_cast<DWORD>(pObject), Cares(pObject, what, true, true) != No);
			}
		}

		NotesValid = true;
	}

	void NotifyNoted(Vector* vector, void* pTarget, DWORD removedFlag)
	{
		const bool bulletsAsked = IsScalableBullet(pTarget);
		const One what(reinterpret_cast<DWORD>(pTarget));
		const unsigned generation = Generation;
		for (int i = 0; i < vector->Count; ++i)
		{
			void** const pObject = At(vector, i);
			auto const pointer = reinterpret_cast<DWORD>(pObject);
			const int cares = Notes.Find(pointer) == 1 ? No : Cares(pObject, what, true, bulletsAsked);
			if (!cares)
			{
				if (!PassOver(pObject, pTarget, removedFlag))
					continue;
			}
			else
			{
				Tell(pObject, pTarget, removedFlag);
			}

			if (cares != Ask)
			{
				// It let go of something, and did whatever follows from that: the notes are no longer
				// to be relied on.
				NotesValid = false;
			}

			// And if notes were thrown away or taken again while it ran (it may have had other objects
			// deleted), the rest of the list is read in full for this target.
			if (!NotesValid || Generation != generation)
			{
				NotifyEach(vector, pTarget, removedFlag, i + 1);
				return;
			}
		}
	}
}

// The game's loop, asking only the objects that hold the pointer somewhere their Detach looks.
static void Notify(Listeners list, void* pTarget, DWORD removedFlag)
{
	Vector* const vector = ListOf(list);
	const bool removed = static_cast<BYTE>(removedFlag) != 0;
	auto const target = reinterpret_cast<DWORD>(pTarget);
	if (removed && InSweep)
	{
		// Its address may be given to a new object before the sweep is over.
		if (NotesValid)
			Notes.Remove(target);
		if (SweepWorth && IsShellOrAnim(target))
		{
			if (!NotesValid && Rebuilds < 8)
				TakeNotes(target);

			if (NotesValid && IsWaiting(target))
			{
				NotifyNoted(vector, pTarget, removedFlag);
				return;
			}
		}
	}
	else if (FromLimbo && IsShellOrAnim(target))
	{
		NotifyLeaving(vector, pTarget, removedFlag, *reinterpret_cast<const DWORD*>(target) == AnimVTable);
		return;
	}

	NotifyEach(vector, pTarget, removedFlag, 0);
}

namespace
{
	// The hooks known to be in the functions read above, by address, length, DLL and name: Ares 3.0p1's,
	// the Phobos we ship, and three that newer Phobos builds add. Each only narrows when a field the game
	// already compares is cleared, except two newer Phobos ones: 0x707A2E is where the object's target is
	// the one going away, a case always handed to the game, and 0x4D9992 is run as described at the top.
	const Perf::KnownHook KnownHooks[] = {
		{ 0x707A47, 10, L"Ares.dll", "TechnoClass_PointerGotInvalid_LastTarget" },
		{ 0x707B09, 6, L"Ares.dll", "TechnoClass_PointerGotInvalid_ResetMindControl" },
		{ 0x707B19, 6, L"Ares.dll", "TechnoClass_PointerGotInvalid_SpawnCloakOwner" },
		{ 0x4D99AA, 6, L"Ares.dll", "FootClass_PointerGotInvalid_Parasite" },
		{ 0x4D9A83, 6, L"Ares.dll", "FootClass_PointerGotInvalid_OccupierVehicleThief" },
		{ 0x7077FD, 6, L"Phobos.dll", "TechnoClass_PointerExpired_SpawnOwnerFix" },
		{ 0x707B23, 6, L"Phobos.dll", "TechnoClass_PointerExpired_RemoveSpawnee" },
		{ 0x4D9A1B, 6, L"Phobos.dll", "FootClass_PointerExpired_RemoveDestination" },
		{ 0x44E910, 6, L"Phobos.dll", "BuildingClass_PointerExpired_C4ExpFix" },
		{ 0x425174, 6, L"Phobos.dll", "AnimClass_Detach_Cloak" },
		{ 0x468503, 6, L"Phobos.dll", "BulletClass_PointerExpired_OwnerFix" },
		{ 0x707A2E, 5, L"Phobos.dll", "TechnoClass_PointerExpired_TargetExpired" },
		{ 0x4D9992, 7, L"Phobos.dll", "FootClass_PointerGotInvalid" },
	};

	// The game's code at 0x4D9992, or Phobos' hook there, which is then run as described at the top.
	bool FootHookAccepted()
	{
		static const BYTE game[] = { 0x3B, 0xC7, 0x75, 0x14, 0x39, 0x68, 0x6C };
		if (memcmp(reinterpret_cast<const void*>(FootHookAt), game, sizeof(game)) == 0)
			return true;

		static const char name[] = "FootClass_PointerGotInvalid";
		if (!Perf::HookListed(L"Phobos.dll", FootHookAt, sizeof(game), name))
			return false;

		FootHook = reinterpret_cast<HookFn>(GetProcAddress(GetModuleHandleA("Phobos.dll"), name));
		if (!FootHook)
			return false;

		Debug::Log("[Perf] Detach: Phobos' hook at 004D9992 is run for the units passed over.\n");
		return true;
	}
}

// The comparisons above are a reading of fifteen of the game's functions as they stand with the known
// hooks in them. Should any of them have been changed in some other way - another hook, a patch, a
// replaced virtual - the reading may no longer hold, and the game's loop is left to run as it is.
static bool Usable()
{
	static int state = 0;
	if (state)
		return state > 0;

	struct Function { DWORD Start, End, Hash; const char* Name; };
	static const Function functions[] = {
		{ 0x5F5230, 0x5F527D, 0xA2FF8963, "ObjectClass::Detach" },
		{ 0x65AAC0, 0x65AB0B, 0x98EA3E31, "RadioClass::Detach" },
		{ 0x4734B0, 0x4734FA, 0x8DB55554, "CargoClass::Detach" },
		{ 0x7077C0, 0x707CAE, 0x5A241C65, "TechnoClass::Detach" },
		{ 0x4D9960, 0x4D9BF5, 0xCA9FDA72, "FootClass::Detach" },
		{ 0x7446E0, 0x744718, 0xEFCEC6D6, "UnitClass::Detach" },
		{ 0x51AA10, 0x51AA3A, 0x3378D3CF, "InfantryClass::Detach" },
		{ 0x41B660, 0x41B698, 0x5403B093, "AircraftClass::Detach" },
		{ 0x44E8F0, 0x44EB08, 0x65D7095B, "BuildingClass::Detach" },
		{ 0x451B40, 0x451E15, 0xADA6AF5E, "BuildingClass::Detach_Animation" },
		{ 0x71CFD0, 0x71CFFA, 0x38FC31A8, "TerrainClass::Detach" },
		{ 0x425150, 0x4251E7, 0x9A684893, "AnimClass::Detach" },
		{ 0x4684E0, 0x468668, 0x88F8FE3C, "BulletClass::Detach" },
		{ 0x62FE90, 0x62FF1F, 0x06EC2FF3, "ParticleSystemClass::Detach" },
		{ 0x4CA580, 0x4CA595, 0x67460BF1, "FactoryClass::Detach" },
	};
	static const DWORD detachSlots[][2] = {
		{ UnitVTable, 0x7446E0 }, { InfantryVTable, 0x51AA10 }, { AircraftVTable, 0x41B660 }, { BuildingVTable, 0x44E8F0 },
		{ TerrainVTable, 0x71CFD0 }, { AnimVTable, 0x425150 }, { BulletVTable, 0x4684E0 }, { ParticleSystemVTable, 0x62FE90 },
		{ ParticleVTable, 0x5F5230 }, { OverlayVTable, 0x5F5230 }, { SmudgeVTable, 0x5F5230 }, { FactoryVTable, 0x4CA580 },
	};

	state = 1;
	for (auto const& function : functions)
	{
		DWORD hash = 0x811C9DC5;
		for (DWORD address = function.Start; address < function.End; ++address)
		{
			const BYTE value = Perf::InKnownHook(address, KnownHooks) ? 0 : *reinterpret_cast<const BYTE*>(address);
			hash = (hash ^ value) * 0x01000193;
		}

		if (hash != function.Hash)
		{
			Debug::Log("[Perf] Detach: %s is not as expected; the game's own loop is left in place.\n", function.Name);
			state = -1;
		}
		else if (!Perf::OnlyKnownHooks(function.Start, function.End, KnownHooks, "Detach"))
		{
			state = -1;
		}
	}

	for (auto const& slot : detachSlots)
	{
		if (reinterpret_cast<const DWORD*>(slot[0])[10] != slot[1])
		{
			Debug::Log("[Perf] Detach: the Detach of the class at %08X has been replaced; the game's own loop is left in place.\n", slot[0]);
			state = -1;
		}
	}

	if (state > 0 && !FootHookAccepted())
	{
		Debug::Log("[Perf] Detach: the hook at 004D9992 is not one that is known; the game's own loop is left in place.\n");
		state = -1;
	}

	return state > 0;
}

namespace
{
	// Detach_This_From_All's caller: ObjectClass::Detach_All, as an object leaves the map.
	constexpr DWORD CalledFromLimbo = 0x5F5316;

	bool Run_(Listeners list, void* pTarget, DWORD removed, DWORD caller)
	{
		FromLimbo = caller == CalledFromLimbo;
		if (!Usable())
			return false;

		Notify(list, pTarget, removed);
		return true;
	}

	// Nothing but this DLL's own hook in the anim loop (0x725A2E-0x725A47), so that the game's loop can
	// be run from here, call for call, when the shortcut stands down.
	bool AnimLoopPlain()
	{
		static int state = 0;
		if (!state)
			state = Perf::OnlyKnownHooks(0x725A33, 0x725A47, KnownHooks, "Detach (animation loop)") ? 1 : -1;

		return state > 0;
	}

	// The game's Remove_All_Inactive, entered past the jump placed on its first two instructions.
	__declspec(naked) void OriginalRemoveAllInactive()
	{
		__asm
		{
			push ecx
			mov eax, ds:[0xB0F6A8]
			mov edx, 0x725C76
			jmp edx
		}
	}

	void RemoveAllInactive()
	{
		InSweep = true;
		NotesValid = false;
		Rebuilds = 0;
		SweepWorth = CountWaiting() >= 4;
		OriginalRemoveAllInactive();
		InSweep = false;
		NotesValid = false;
	}
}

DEFINE_FUNCTION_JUMP(LJMP, 0x725C70, RemoveAllInactive)

// Detach_This_From_All's loop over the objects to tell: ESI = what is going away, EDI = whether for good.
DEFINE_HOOK(0x72593E, DetachThisFromAll_TellObjects, 0x5)
{
	GET(void*, pTarget, ESI);
	GET(DWORD, removed, EDI);
	GET_STACK(DWORD, caller, 0x14);
	return Run_(Listeners::Objects, pTarget, removed, caller) ? 0x725961 : 0;
}

// Its loop over the objects told about animations. The hook is on the loop's first instruction, which
// every pass jumps back to (the one before the loop, 0x725A1F, is Ares' hook), so when the shortcut
// stands down the game's loop is run from here, call for call, rather than through the hook each pass.
DEFINE_HOOK(0x725A2E, DetachThisFromAll_TellAboutAnim, 0x5)
{
	enum { LoopDone = 0x725A47 };

	GET(void*, pTarget, ESI);
	GET(DWORD, removed, EDI);
	GET_STACK(DWORD, caller, 0x14);
	if (Run_(Listeners::Anims, pTarget, removed, caller))
		return LoopDone;

	if (!AnimLoopPlain())
		return 0;

	GET(int, from, EBX);
	Vector* const vector = ListOf(Listeners::Anims);
	for (int i = from; i < vector->Count; ++i)
		Tell(At(vector, i), pTarget, removed);

	return LoopDone;
}

void Perf::Detach::Check()
{
	Usable();
	AnimLoopPlain();
}
