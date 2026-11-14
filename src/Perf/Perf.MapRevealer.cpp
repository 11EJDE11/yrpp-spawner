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

// Ares replaces MapClass's three area-reveal functions with its MapRevealer:
//   0x5673A0 RevealArea0 (TechnoClass::Look and others): Reveal0, then UpdateShroud over the radius
//   0x5678E0 RevealArea1 (Sight_From): Reveal1
//   0x567DA0 RevealArea2 (FootClass::AI on a height change): UpdateShroud over a ring
// Every unit runs UpdateShroud whenever it moves a cell or changes height, for every house - not only
// the houses the local player sees through - and each cell of it costs a Cell_Shadow, which is nine
// cell lookups.
//
// This is the same algorithm, step for step, as Ares 3.0p1's compiled code (Ares.dll 0x10069660,
// 0x10069B50, 0x10069FD0, 0x1006A2E0-0x1006A580, hooks 0x1004F530/5C0/630), with two changes that
// cannot alter what it does:
//   - the cell spread order comes from a table built once with the same enumerator;
//   - UpdateShroud reads the shroud bits of the area once into a grid and computes each cell's
//     shadow from it. Nothing in the loop writes those bits (Tactical::Recalc_Visibility 0x6DA7D0
//     only touches the visibility cache and the redraw list), so the shadows are the ones
//     Cell_Shadow would return. Near the map edge, where a lookup can fall outside the cell array
//     and touch InvalidCell, the literal cell-by-cell sequence runs instead.
//
// Syringe has put a jump to Ares' hook at each address by the time WinMain runs; Apply replaces it
// with a jump here. Only Ares 3.0p1, the build this was checked against, is taken over.

#include "Perf.h"

#include <CellClass.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <MouseClass.h>
#include <RulesClass.h>
#include <SessionClass.h>
#include <TacticalClass.h>
#include <YRMath.h>
#include <Helpers/Enumerators.h>

#include <Utilities/Debug.h>
#include <Utilities/Macro.h>
#include <Utilities/Patch.h>

#include <Windows.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace
{
	// The game's own TacticalClass::AdjustForZ. YRpp's inline version has the same formula in SSE
	// doubles; the game's rounds through the x87 unit, and the base cell has to come out as Ares
	// gets it.
	int __fastcall GameAdjustForZ(int height)
		JMP_STD(0x6D20E0);

	// Cell coordinates wrap at 16 bits, as Ares' arithmetic on them does.
	CellStruct Add(CellStruct a, CellStruct b)
	{
		return { static_cast<short>(a.X + b.X), static_cast<short>(a.Y + b.Y) };
	}

	CellStruct Sub(CellStruct a, CellStruct b)
	{
		return { static_cast<short>(a.X - b.X), static_cast<short>(a.Y - b.Y) };
	}

	constexpr auto Revealed = CellFlags::CenterRevealed | CellFlags::EdgeRevealed;

	// CellSpreadEnumerator's order, ring by ring. A ring's order does not depend on the radius it is
	// part of, so any start and radius is one contiguous slice.
	class SpreadTable
	{
	public:
		const CellStruct* Begin(size_t ring) const { return Offsets.data() + RingStart[ring]; }
		const CellStruct* End(size_t ring) const { return Offsets.data() + RingStart[ring + 1]; }

		void EnsureRings(size_t radius)
		{
			// An enumerator started and stopped on ring r yields exactly ring r.
			for (size_t ring = RingStart.size() - 1; ring <= radius; ++ring)
			{
				for (CellSpreadEnumerator it(ring, ring); it; ++it)
					Offsets.push_back(*it);

				RingStart.push_back(Offsets.size());
			}
		}

	private:
		std::vector<CellStruct> Offsets;
		std::vector<size_t> RingStart { 0 };
	};

	SpreadTable Spread;

	class Revealer
	{
	public:
		// 0x10069FD0
		explicit Revealer(const CoordStruct& coords)
		{
			const int adjust = (GameAdjustForZ(coords.Z) / -30) << 8;
			BaseCell = CellClass::Coord2Cell(coords + CoordStruct { adjust, adjust, 0 });
			CellOffset = Sub(Sub(BaseCell, CellClass::Coord2Cell(coords)), CellStruct { 2, 2 });

			auto const& session = SessionClass::Instance;
			RequiredChecks = SessionClass::IsMultiplayer() && session.MPGameMode && !session.MPGameMode->vt_entry_04();

			MapWidth = MapClass::Instance.MapRect.Width;
			MapHeight = MapClass::Instance.MapRect.Height;
			CheckedCells[0] = { 7, static_cast<short>(MapWidth + 5) };
			CheckedCells[1] = { 13, static_cast<short>(MapWidth + 11) };
			CheckedCells[2] = { static_cast<short>(MapHeight + 13), static_cast<short>(MapHeight + MapWidth - 15) };
		}

		// 0x10069660
		void Reveal0(const CoordStruct& coords, int radius, HouseClass* pHouse, bool onlyOutline, bool unknown,
			bool fog, bool allowRevealByHeight, bool add) const
		{
			RevealImpl(coords, radius, pHouse, onlyOutline, allowRevealByHeight, [=](CellClass* pCell)
			{
				pCell->Flags &= ~CellFlags::IsPlot;

				if (!IsCellAllowed(pCell->MapCoords))
					return;

				if (fog)
				{
					if ((pCell->Flags & Revealed) != Revealed && (pCell->AltFlags & AltCellFlags::Mapped))
						MouseClass::Instance.MapCellFoggedness(&pCell->MapCoords, HouseClass::CurrentPlayer);
				}
				else if ((pCell->AltFlags & AltCellFlags::Clear) != AltCellFlags::Clear || (pCell->Flags & Revealed) != Revealed)
				{
					if (unknown)
						return;

					if (add)
						MouseClass::Instance.RevealFogShroud(&pCell->MapCoords, HouseClass::CurrentPlayer, false);
					else
						pCell->Unshroud();
				}
			});
		}

		// 0x10069B50
		void Reveal1(const CoordStruct& coords, int radius, HouseClass* pHouse, bool onlyOutline, bool fog,
			bool allowRevealByHeight, bool add) const
		{
			RevealImpl(coords, radius, pHouse, onlyOutline, allowRevealByHeight, [=](CellClass* pCell)
			{
				pCell->Flags &= ~CellFlags::IsPlot;

				if (fog)
				{
					if ((pCell->Flags & Revealed) != Revealed && (pCell->AltFlags & AltCellFlags::Mapped))
						MouseClass::Instance.MapCellFoggedness(&pCell->MapCoords, HouseClass::CurrentPlayer);
				}
				else if (IsCellAllowed(pCell->MapCoords))
				{
					MouseClass::Instance.RevealFogShroud(&pCell->MapCoords, HouseClass::CurrentPlayer, add);
				}
			});
		}

		// 0x1006A580
		void UpdateShroud(size_t start, size_t radius, bool fog) const
		{
			if (fog)
				return;

			radius = std::min(radius, CellSpreadEnumerator::Max);
			start = std::min(start, CellSpreadEnumerator::Max - 3);
			if (start > radius)
				return;

			Spread.EnsureRings(radius);
			const CellStruct* const pFirst = Spread.Begin(start);
			const CellStruct* const pLast = Spread.End(radius);

			ShroudGrid grid;
			if (grid.Read(BaseCell, static_cast<int>(radius) + 1))
			{
				for (auto pOffset = pFirst; pOffset != pLast; ++pOffset)
					SetVisibility(grid.CellAt(*pOffset), grid.Shadow(*pOffset));

				return;
			}

			for (auto pOffset = pFirst; pOffset != pLast; ++pOffset)
			{
				const CellStruct cell = Add(BaseCell, *pOffset);
				CellClass* const pCell = MapClass::Instance.GetCellAt(cell);
				SetVisibility(pCell, TacticalClass::Instance->GetOcclusion(cell, false));
			}
		}

	private:
		// The AltFlags of every cell an UpdateShroud reads, around its base cell. Only usable when all
		// of them are real cells, so that no lookup would have touched InvalidCell.
		class ShroudGrid
		{
		public:
			bool Read(CellStruct base, int reach)
			{
				Reach = reach;
				Side = 2 * reach + 1;
				if (Side > MaxSide)
					return false;

				for (int y = -reach; y <= reach; ++y)
				{
					for (int x = -reach; x <= reach; ++x)
					{
						CellClass* const pCell = MapClass::Instance.TryGetCellAt(Add(base, CellStruct { static_cast<short>(x), static_cast<short>(y) }));
						if (!pCell)
							return false;

						const int slot = Slot(x, y);
						Cells[slot] = pCell;
						Flags[slot] = pCell->AltFlags;
					}
				}

				return true;
			}

			CellClass* CellAt(CellStruct offset) const
			{
				return Cells[Slot(offset.X, offset.Y)];
			}

			// Tactical::Cell_Shadow (0x6D8700) without lookups; see Perf.CellShadow.cpp.
			int Shadow(CellStruct offset) const
			{
				const AltCellFlags centre = Flags[Slot(offset.X, offset.Y)];
				if (!(centre & AltCellFlags::Mapped))
					return (centre & AltCellFlags::Clear) ? -1 : -2;

				struct Neighbour { int X, Y, Bit; };
				static constexpr Neighbour Neighbours[] = {
					{ -1, -1, 0x40 }, { 0, -1, 0x80 }, { 1, -1, 0x01 },
					{ -1, 0, 0x20 }, { 1, 0, 0x02 },
					{ -1, 1, 0x10 }, { 0, 1, 0x08 }, { 1, 1, 0x04 },
				};

				int index = 0;
				for (auto const& neighbour : Neighbours)
				{
					if (!(Flags[Slot(offset.X + neighbour.X, offset.Y + neighbour.Y)] & AltCellFlags::Mapped))
						index |= neighbour.Bit;
				}

				return ShadowFrames[index];
			}

		private:
			static constexpr int MaxSide = 2 * 25 + 1;
			static inline const signed char* const ShadowFrames = reinterpret_cast<const signed char*>(0x7F4194);

			int Slot(int x, int y) const { return (y + Reach) * Side + (x + Reach); }

			int Reach = 0;
			int Side = 0;
			CellClass* Cells[MaxSide * MaxSide];
			AltCellFlags Flags[MaxSide * MaxSide];
		};

		static void SetVisibility(CellClass* pCell, int shadow)
		{
			if (pCell->Visibility == shadow)
				return;

			pCell->Visibility = static_cast<char>(shadow);
			pCell->VisibilityChanged = true;
			TacticalClass::Instance->RegisterCellAsVisible(pCell);
		}

		// 0x1006A3F0
		bool IsCellAllowed(const CellStruct& cell) const
		{
			return !RequiredChecks || std::none_of(std::begin(CheckedCells), std::end(CheckedCells),
				[&cell](const CellStruct& checked) { return checked == cell; });
		}

		bool IsCellAvailable(const CellStruct& cell) const
		{
			const int sum = cell.X + cell.Y;
			return sum > MapWidth
				&& cell.X - cell.Y < MapWidth
				&& cell.Y - cell.X < MapWidth
				&& sum <= MapWidth + 2 * MapHeight;
		}

		// 0x1006A2E0. Ares reads Level as an unsigned byte.
		bool CheckLevel(const CellStruct& offset, int level) const
		{
			const CellStruct relation { static_cast<short>(Math::sgn(-offset.X)), static_cast<short>(Math::sgn(-offset.Y)) };
			CellClass* const pCell = MapClass::Instance.GetCellAt(Add(Add(Sub(BaseCell, CellOffset), offset), relation));
			return static_cast<unsigned char>(pCell->Level) < level + CellClass::BridgeLevels;
		}

		static bool AffectsHouse(HouseClass* pHouse)
		{
			auto const pPlayer = HouseClass::CurrentPlayer;
			if (pHouse == pPlayer)
				return true;

			if (!pHouse || !pPlayer)
				return false;

			return pHouse->RadarVisibleTo.Contains(pPlayer)
				|| (RulesClass::Instance->AllyReveal && pHouse->IsAlliedWith(pPlayer));
		}

		// The loop both reveals share.
		template <typename Process>
		void RevealImpl(const CoordStruct& coords, int radius, HouseClass* pHouse, bool onlyOutline,
			bool allowRevealByHeight, Process process) const
		{
			const int level = coords.Z / Unsorted::LevelHeight;

			if (!AffectsHouse(pHouse) || !IsCellAvailable(BaseCell) || radius <= 0)
				return;

			const size_t spread = std::min(static_cast<size_t>(radius), CellSpreadEnumerator::Max);
			const size_t limit = (spread + 1) * (spread + 1);

			const bool revealByHeight = RulesClass::Instance->RevealByHeight;
			const size_t start = (!revealByHeight && onlyOutline && spread > 2) ? spread - 3 : 0u;
			const bool checkLevel = allowRevealByHeight && revealByHeight;

			Spread.EnsureRings(spread);
			for (auto pOffset = Spread.Begin(start); pOffset != Spread.End(spread); ++pOffset)
			{
				const CellStruct offset = *pOffset;
				const CellStruct cell = Add(BaseCell, offset);

				if (IsCellAvailable(cell)
					&& std::abs(offset.X) <= static_cast<int>(spread)
					&& offset.MagnitudeSquared() < limit
					&& (!checkLevel || CheckLevel(offset, level)))
				{
					process(MapClass::Instance.GetCellAt(cell));
				}
			}
		}

		CellStruct BaseCell;
		CellStruct CellOffset;
		CellStruct CheckedCells[3];
		bool RequiredChecks;
		int MapWidth;
		int MapHeight;
	};
}

// The three replaced functions, with the originals' arguments.
class MapClassPerf final : public MapClass
{
public:
	void _RevealArea0(const CoordStruct& coords, int radius, HouseClass* pHouse, bool onlyOutline, bool unknown,
		bool fog, bool allowRevealByHeight, bool add)
	{
		const Revealer revealer(coords);
		revealer.Reveal0(coords, radius, pHouse, onlyOutline, unknown, fog, allowRevealByHeight, add);
		revealer.UpdateShroud(0, static_cast<size_t>(std::max(radius, 0)), false);
	}

	void _RevealArea1(const CoordStruct& coords, int radius, HouseClass* pHouse, bool onlyOutline, bool unknown,
		bool fog, bool allowRevealByHeight, bool add)
	{
		Revealer(coords).Reveal1(coords, radius, pHouse, onlyOutline, fog, allowRevealByHeight, add);
	}

	void _RevealArea2(const CoordStruct& coords, int start, int radius, bool fog)
	{
		Revealer(coords).UpdateShroud(static_cast<size_t>(std::max(start, 0)), static_cast<size_t>(std::max(radius, 0)), fog);
	}
};

namespace
{
	_GET_FUNCTION_ADDRESS(MapClassPerf::_RevealArea0, RevealArea0)
	_GET_FUNCTION_ADDRESS(MapClassPerf::_RevealArea1, RevealArea1)
	_GET_FUNCTION_ADDRESS(MapClassPerf::_RevealArea2, RevealArea2)

	// The prologues of Ares 3.0p1's hook bodies and MapRevealer functions. None holds a relocated
	// address, so they read the same wherever Ares.dll is loaded.
	struct Prologue
	{
		DWORD Rva;
		BYTE Bytes[16];
	};

	constexpr Prologue AresPrologues[] = {
		{ 0x4F530, { 0x83, 0xEC, 0x30, 0x8B, 0x44, 0x24, 0x34, 0x53, 0x56, 0x57, 0x8B, 0x48, 0x14, 0x0F, 0xB6, 0x41 } },
		{ 0x4F5C0, { 0x83, 0xEC, 0x2C, 0x8B, 0x44, 0x24, 0x30, 0x53, 0x56, 0x57, 0x8B, 0x48, 0x14, 0x0F, 0xB6, 0x41 } },
		{ 0x4F630, { 0x83, 0xEC, 0x20, 0x8B, 0x44, 0x24, 0x24, 0x56, 0x57, 0x8B, 0x48, 0x14, 0x8A, 0x41, 0x10, 0x8B } },
		{ 0x69660, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x5C, 0x8B, 0x45, 0x08, 0x53, 0x56, 0x8B, 0x31 } },
		{ 0x69B50, { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x5C, 0x8B, 0x45, 0x08, 0x53, 0x56, 0x8B, 0x31 } },
		{ 0x69FD0, { 0x51, 0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14, 0x8B, 0xD9, 0x8B, 0x4F, 0x08, 0xE8, 0x2E, 0xCF } },
		{ 0x6A580, { 0x83, 0xEC, 0x28, 0x80, 0x7C, 0x24, 0x34, 0x00, 0x0F, 0x85, 0x1E, 0x02, 0x00, 0x00, 0x8B, 0x44 } },
	};

	// Ares 3.0p1 is Ares.dll with timestamp 0x61DAA114.
	bool IsAres30p1()
	{
		auto const pBase = reinterpret_cast<const BYTE*>(GetModuleHandleA("Ares.dll"));
		if (!pBase)
			return false;

		auto const pHeaders = reinterpret_cast<const IMAGE_NT_HEADERS32*>(pBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(pBase)->e_lfanew);
		if (pHeaders->FileHeader.TimeDateStamp != 0x61DAA114 || pHeaders->OptionalHeader.SizeOfImage != 0xDB000)
			return false;

		return std::all_of(std::begin(AresPrologues), std::end(AresPrologues), [pBase](const Prologue& prologue)
			{ return !memcmp(pBase + prologue.Rva, prologue.Bytes, sizeof(prologue.Bytes)); });
	}

	// Syringe's jump to Ares' hook is still in place.
	bool IsHookedByAres(DWORD address)
	{
		return *reinterpret_cast<const BYTE*>(address) == 0xE9;
	}
}

namespace Perf
{
	namespace MapRevealer
	{
		void Apply()
		{
			if (!IsAres30p1())
			{
				Debug::Log("[Perf] MapRevealer: not the Ares build this reproduces; leaving it alone.\n");
				return;
			}

			if (!IsHookedByAres(0x5673A0) || !IsHookedByAres(0x5678E0) || !IsHookedByAres(0x567DA0))
			{
				Debug::Log("[Perf] MapRevealer: the reveal functions are not hooked as expected; leaving them alone.\n");
				return;
			}

			Patch::Apply_LJMP(0x5673A0, RevealArea0());
			Patch::Apply_LJMP(0x5678E0, RevealArea1());
			Patch::Apply_LJMP(0x567DA0, RevealArea2());
		}
	}
}
