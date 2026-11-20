/**
*  yrpp-spawner
*
*  Copyright(C) 2022-present CnCNet
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

// What a selected object is producing or carrying, for observers and - behind a setting - allies.
//
// Production. The game draws the cameo of what a selected factory builds over it
// (BuildingClass::Draw_Spied_Selection, 0x43E7B0) for a house that spied it. Ares replaces that
// function and extends it to observers, but reads one factory per building: for a construction yard
// HouseClass::Fetch_Factory with BuildCat::DontCare, which is the buildings queue, never the defenses
// queue (Fetch_Factory returns DefenseFactory only for BuildCat::Combat). It shows no progress either.
// Phobos moves the call from the first pass of Tactical::Render_Layers (0x6D9430, jumped over) to
// its second pass, just before each object's Draw_Extras (0x6D9781). Ares's hook sits at the
// function's entry and runs ahead of ours, so the call is taken over at Phobos's site instead: for a
// building whose production this viewer may see, the spawner hook there - which runs before Phobos's
// own, the DLLs being passed to Syringe as Ares, CnCNet-Spawner, Phobos - does the Draw_Extras call
// itself and steps past it, so neither Phobos nor Ares draws for that building. Everyone else keeps
// the Ares behaviour, spied buildings included. Without the Phobos relocation in place the takeover
// stays off, since the call would still come from the first pass.
//
// The panel is drawn after all layers (0x6D4684, next to the flying strings of Ares and Phobos), so
// later objects' health bars do not cover it.
//
// Contents. A selected transport (passengers, an IFV's gunner included), a garrisoned building, a bio
// reactor (passengers too) or a tank bunker shows what is inside, grouped by type.
//
// All of this only reads the simulation, and only to draw it.

#include <Spawner/Spawner.h>
#include <Utilities/Macro.h>

#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <Drawing.h>
#include <FactoryClass.h>
#include <FileSystem.h>
#include <FootClass.h>
#include <HouseClass.h>
#include <InfantryClass.h>
#include <ObjectClass.h>
#include <Surface.h>
#include <TacticalClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace SelectionInfo
{
	namespace
	{
		struct Settings
		{
			bool ObserverProduction;
			bool ObserverPassengers;
			bool AlliesProduction;
			bool AlliesPassengers;
		};

		Settings GetSettings()
		{
			if (Spawner::Enabled)
			{
				const auto pConfig = Spawner::GetConfig();
				return {
					pConfig->Observer_ShowProduction,
					pConfig->Observer_ShowPassengers,
					pConfig->Allies_ShowProduction,
					pConfig->Allies_ShowPassengers
				};
			}

			return { true, true, false, false };
		}

		// Phobos's DEFINE_JUMP(LJMP, 0x6D9430, 0x6D95A1): the first-pass call is skipped and the
		// second-pass one at 0x6D9781 is the only one left.
		bool PhobosMovedSpiedSelection()
		{
			static const bool moved = []()
			{
				const auto pCode = reinterpret_cast<const unsigned char*>(0x6D9430);
				if (pCode[0] != 0xE9)
					return false;

				int relative = 0;
				memcpy(&relative, pCode + 1, sizeof(relative));
				return 0x6D9430 + 5 + relative == 0x6D95A1;
			}();

			return moved;
		}

		bool ViewerSeesProduction(const HouseClass* pOwner)
		{
			const auto pPlayer = HouseClass::CurrentPlayer;
			if (!pOwner || !pPlayer)
				return false;

			const auto settings = GetSettings();
			if (HouseClass::IsCurrentPlayerObserver())
				return settings.ObserverProduction;

			// Their own production is on their sidebar.
			return settings.AlliesProduction && pOwner != pPlayer && pOwner->IsMutualAlly(pPlayer);
		}

		bool ViewerSeesContents(const HouseClass* pOwner)
		{
			const auto pPlayer = HouseClass::CurrentPlayer;
			if (!pOwner || !pPlayer)
				return false;

			const auto settings = GetSettings();
			if (HouseClass::IsCurrentPlayerObserver())
				return settings.ObserverPassengers;

			// Not their own: with a big selection those panels would cover the screen.
			return settings.AlliesPassengers && pOwner != pPlayer && pOwner->IsMutualAlly(pPlayer);
		}

#pragma region Production state

		// The house-wide queues a human player's sidebar feeds.
		enum class Slot : int
		{
			Building,
			Defense,
			Infantry,
			Vehicle,
			Ship,
			Aircraft
		};

		bool SlotOf(const TechnoTypeClass* pType, Slot& slot)
		{
			if (!pType)
				return false;

			switch (pType->WhatAmI())
			{
			case AbstractType::BuildingType:
				slot = static_cast<const BuildingTypeClass*>(pType)->BuildCat == BuildCat::Combat
					? Slot::Defense : Slot::Building;
				return true;
			case AbstractType::InfantryType:
				slot = Slot::Infantry;
				return true;
			case AbstractType::UnitType:
				slot = pType->Naval ? Slot::Ship : Slot::Vehicle;
				return true;
			case AbstractType::AircraftType:
				slot = Slot::Aircraft;
				return true;
			default:
				return false;
			}
		}

		// The queues a factory building of this type serves: a construction yard both of its own.
		int SlotsOf(const BuildingTypeClass* pType, Slot (&slots)[2])
		{
			switch (pType->Factory)
			{
			case AbstractType::BuildingType:
				slots[0] = Slot::Building;
				slots[1] = Slot::Defense;
				return 2;
			case AbstractType::InfantryType:
				slots[0] = Slot::Infantry;
				return 1;
			case AbstractType::UnitType:
				slots[0] = pType->Naval ? Slot::Ship : Slot::Vehicle;
				return 1;
			case AbstractType::AircraftType:
				slots[0] = Slot::Aircraft;
				return 1;
			default:
				return 0;
			}
		}

		// A human house builds through one factory per queue, shared by every building that serves it
		// (what Ares shows too). The AI builds one object at a time in the building's own factory
		// (BuildingClass::Factory_AI, 0x4500F0).
		FactoryClass* FactoryFor(const BuildingClass* pBuilding, Slot slot)
		{
			const auto pOwner = pBuilding->Owner;

			if (!pOwner->IsControlledByHuman())
			{
				const auto pFactory = pBuilding->Factory;
				Slot productSlot;
				if (pFactory && pFactory->Object && SlotOf(pFactory->Object->GetTechnoType(), productSlot)
					&& productSlot == slot)
				{
					return pFactory;
				}

				return nullptr;
			}

			switch (slot)
			{
			case Slot::Building: return pOwner->Primary_ForBuildings;
			case Slot::Defense:  return pOwner->Primary_ForDefenses;
			case Slot::Infantry: return pOwner->Primary_ForInfantry;
			case Slot::Vehicle:  return pOwner->Primary_ForVehicles;
			case Slot::Ship:     return pOwner->Primary_ForShips;
			case Slot::Aircraft: return pOwner->Primary_ForAircraft;
			default:             return nullptr;
			}
		}

		// Production that was cancelled leaves nothing behind in the factory, so the abandon is
		// remembered for a few seconds to show it.
		struct CancelRecord
		{
			const HouseClass* House;
			Slot QueueSlot;
			const TechnoTypeClass* Type;
			int Stage;
			DWORD Tick;
		};

		std::vector<CancelRecord> Cancels;
		constexpr DWORD CancelShownForMs = 3000;
		constexpr size_t CancelsKept = 64;

		const CancelRecord* RecentCancel(const HouseClass* pHouse, Slot slot)
		{
			const DWORD now = GetTickCount();
			for (auto it = Cancels.rbegin(); it != Cancels.rend(); ++it)
			{
				if (now - it->Tick > CancelShownForMs)
					break;

				if (it->House == pHouse && it->QueueSlot == slot)
					return &*it;
			}

			return nullptr;
		}

		constexpr int FullStage = 54; // FactoryClass production always runs 54 steps

		enum class RowState
		{
			Building,
			OnHold,
			NoFunds,
			Ready,
			Cancelled
		};

		struct TypeCount
		{
			const TechnoTypeClass* Type;
			int Count;
		};

		void AddToGroups(std::vector<TypeCount>& groups, const TechnoTypeClass* pType)
		{
			if (!pType)
				return;

			for (auto& group : groups)
			{
				if (group.Type == pType)
				{
					++group.Count;
					return;
				}
			}

			groups.push_back({ pType, 1 });
		}

		struct ProductionRow
		{
			const TechnoTypeClass* Type;
			RowState State;
			int Stage;
			std::vector<TypeCount> Queue;
		};

		bool GetRow(const BuildingClass* pBuilding, Slot slot, ProductionRow& row)
		{
			const auto pFactory = FactoryFor(pBuilding, slot);
			if (pFactory && pFactory->Object)
			{
				row.Type = pFactory->Object->GetTechnoType();
				if (!row.Type)
					return false;

				row.Stage = std::clamp(pFactory->Production.Value, 0, FullStage);

				// FactoryClass::Has_Completed (0x4CA130) / Suspend (0x4C9E60) / AI (0x4C9B20): a finished
				// product suspends the factory; a hold suspends it before; OnHold is set by AI when a
				// production step could not be paid for.
				if (row.Stage >= FullStage)
					row.State = RowState::Ready;
				else if (pFactory->IsSuspended)
					row.State = RowState::OnHold;
				else if (pFactory->OnHold)
					row.State = RowState::NoFunds;
				else
					row.State = RowState::Building;

				row.Queue.clear();
				for (const auto pQueued : pFactory->QueuedObjects)
					AddToGroups(row.Queue, pQueued);

				return true;
			}

			if (const auto pCancel = RecentCancel(pBuilding->Owner, slot))
			{
				row.Type = pCancel->Type;
				row.Stage = pCancel->Stage;
				row.State = RowState::Cancelled;
				row.Queue.clear();
				return true;
			}

			return false;
		}

#pragma endregion Production state

#pragma region Drawing

		constexpr int CameoWidth = 60;
		constexpr int CameoHeight = 48;
		constexpr int CellGap = 4;
		constexpr int LineHeight = 13;
		constexpr int BarHeight = 4;

		int Rgb(int r, int g, int b)
		{
			return Drawing::RGB_To_Int(r, g, b);
		}

		// Client coordinates are relative to the tactical view, which the surface coordinates of the
		// fills are not.
		RectangleStruct ToSurface(const RectangleStruct& client)
		{
			const auto& view = DSurface::ViewBounds;
			const int left = std::max(client.X + view.X, view.X);
			const int top = std::max(client.Y + view.Y, view.Y);
			const int right = std::min(client.X + view.X + client.Width, view.X + view.Width);
			const int bottom = std::min(client.Y + view.Y + client.Height, view.Y + view.Height);
			return { left, top, right - left, bottom - top };
		}

		void Fill(const RectangleStruct& client, int color)
		{
			auto rect = ToSurface(client);
			if (rect.Width > 0 && rect.Height > 0)
				DSurface::Temp->FillRect(&rect, color);
		}

		void FillTranslucent(const RectangleStruct& client, ColorStruct color, int opacity)
		{
			auto rect = ToSurface(client);
			if (rect.Width > 0 && rect.Height > 0)
				DSurface::Temp->FillRectTrans(&rect, &color, opacity);
		}

		// Fancy_Text_Print treats the text as a printf format, so a lone % would be swallowed.
		void DrawTextAt(const wchar_t* pText, int x, int top, int color, TextPrintType align)
		{
			wchar_t escaped[256];
			size_t length = 0;
			for (auto p = pText; *p && length + 2 < std::size(escaped); ++p)
			{
				if (*p == L'%')
					escaped[length++] = L'%';

				escaped[length++] = *p;
			}
			escaped[length] = L'\0';

			auto bounds = DSurface::ViewBounds;
			Point2D location { x, top };
			DSurface::Temp->DrawText(escaped, &bounds, &location, color, 0,
				TextPrintType::Point8 | align | TextPrintType::FullShadow);
		}

		void Text(const wchar_t* pText, int centreX, int top, int color)
		{
			DrawTextAt(pText, centreX, top, color, TextPrintType::Center);
		}

		void TextRight(const wchar_t* pText, int right, int top, int color)
		{
			DrawTextAt(pText, right, top, color, TextPrintType::Right);
		}

		// How Ares draws the production cameo: centred, in the cameo palette.
		void Cameo(const TechnoTypeClass* pType, int left, int top)
		{
			if (const auto pShape = pType->GetCameo())
			{
				Point2D centre { left + CameoWidth / 2, top + CameoHeight / 2 };
				DSurface::Temp->DrawSHP(FileSystem::CAMEO_PAL, pShape, 0, &centre, &DSurface::ViewBounds,
					BlitterFlags::Centered | BlitterFlags::bf_400 | BlitterFlags::Alpha,
					0, 0, ZGradient::Ground, 1000, 0, nullptr, 0, 0, 0);
			}
			else
			{
				FillTranslucent({ left, top, CameoWidth, CameoHeight }, ColorStruct { 0, 0, 0 }, 60);
				Text(pType->UIName ? pType->UIName : L"?", left + CameoWidth / 2, top + CameoHeight / 2 - 6,
					Rgb(255, 255, 255));
			}
		}

		// A count in the top-right corner of a cameo.
		void CameoBadge(const wchar_t* pText, int cameoLeft, int cameoTop)
		{
			TextRight(pText, cameoLeft + CameoWidth - 2, cameoTop - 1, Rgb(255, 255, 255));
		}

		// Where Phobos centres the cameo over a building: half way up its foundation and height.
		Point2D BuildingAnchor(const BuildingClass* pBuilding)
		{
			const auto [point, visible] = TacticalClass::Instance->CoordsToClient(pBuilding->GetCenterCoords());
			const auto pType = pBuilding->Type;
			const int foundationHeight = pType->GetFoundationHeight(false);
			constexpr int CellHeightInPixels = 30;
			const int yOffset = (CellHeightInPixels * (foundationHeight + pType->Height)) >> 2;
			return { point.X, point.Y - yOffset };
		}

		// The production of one building: one cell per active queue, side by side and centred on the
		// anchor, each a cameo with the number queued behind it in the corner, a progress bar, the
		// percentage and, on hold or short of money, why it is not progressing. Returns the top of what
		// was drawn, or the anchor's Y when there was nothing to draw.
		int DrawProduction(const BuildingClass* pBuilding, const Point2D& anchor)
		{
			Slot slots[2];
			const int slotCount = SlotsOf(pBuilding->Type, slots);

			ProductionRow rows[2];
			int rowCount = 0;
			for (int i = 0; i < slotCount; ++i)
			{
				if (GetRow(pBuilding, slots[i], rows[rowCount]))
					++rowCount;
			}

			if (!rowCount)
				return anchor.Y;

			const int cellWidth = CameoWidth + CellGap;
			const int totalWidth = rowCount * cellWidth - CellGap;
			const int left = anchor.X - totalWidth / 2;
			const int cameoTop = anchor.Y - CameoHeight / 2;

			for (int i = 0; i < rowCount; ++i)
			{
				const auto& row = rows[i];
				const int x = left + i * cellWidth;
				const int centreX = x + CameoWidth / 2;

				Cameo(row.Type, x, cameoTop);

				int queued = 0;
				for (const auto& group : row.Queue)
					queued += group.Count;

				if (queued > 0)
				{
					wchar_t badge[16];
					swprintf_s(badge, L"+%d", queued);
					CameoBadge(badge, x, cameoTop);
				}

				const int percent = row.Stage * 100 / FullStage;
				int barColor = Rgb(40, 200, 40);
				wchar_t progress[16];
				const wchar_t* pReason = nullptr;
				swprintf_s(progress, L"%d%%", percent);
				switch (row.State)
				{
				case RowState::Ready:
					barColor = Rgb(90, 170, 255);
					swprintf_s(progress, L"Ready");
					break;
				case RowState::OnHold:
					barColor = Rgb(230, 200, 30);
					pReason = L"On hold";
					break;
				case RowState::NoFunds:
					barColor = Rgb(240, 130, 20);
					pReason = L"No funds";
					break;
				case RowState::Cancelled:
					barColor = Rgb(220, 40, 40);
					swprintf_s(progress, L"Cancelled");
					break;
				default:
					break;
				}

				// Progress bar under the cameo, clear of the name printed along its bottom.
				const int barTop = cameoTop + CameoHeight + 1;
				Fill({ x, barTop, CameoWidth, BarHeight }, Rgb(0, 0, 0));
				const int filled = row.State == RowState::Cancelled ? CameoWidth : CameoWidth * row.Stage / FullStage;
				Fill({ x, barTop, filled, BarHeight }, barColor);

				// The percentage under the bar, and why it is stuck on a line of its own.
				const int lines = pReason ? 2 : 1;
				const int textTop = barTop + BarHeight;
				FillTranslucent({ x, textTop, CameoWidth, lines * LineHeight + 2 }, ColorStruct { 0, 0, 0 }, 55);

				// The font draws a couple of pixels below its top.
				const int y = textTop - 1;
				Text(progress, centreX, y, Rgb(255, 255, 255));
				if (pReason)
					Text(pReason, centreX, y + LineHeight, barColor);
			}

			return cameoTop;
		}

		// What the object holds, grouped by type in the order first met.
		int CollectContents(const TechnoClass* pTechno, std::vector<TypeCount>& groups, int& capacity)
		{
			groups.clear();
			capacity = 0;
			int count = 0;

			for (auto pPassenger = pTechno->Passengers.GetFirstPassenger(); pPassenger;
				pPassenger = abstract_cast<FootClass*>(pPassenger->NextObject))
			{
				AddToGroups(groups, pPassenger->GetTechnoType());
				++count;
			}

			if (const auto pTechnoType = pTechno->GetTechnoType())
				capacity = pTechnoType->Passengers;

			if (const auto pBuilding = specific_cast<const BuildingClass*>(pTechno))
			{
				for (const auto pOccupant : pBuilding->Occupants)
				{
					AddToGroups(groups, pOccupant->GetTechnoType());
					++count;
				}

				if (pBuilding->Type->MaxNumberOccupants > 0)
					capacity = std::max(capacity, pBuilding->Type->MaxNumberOccupants);

				// A tank bunker links to the vehicle inside it.
				if (const auto pBunkered = abstract_cast<FootClass*>(pBuilding->BunkerLinkedItem))
				{
					AddToGroups(groups, pBunkered->GetTechnoType());
					++count;
					capacity = std::max(capacity, 1);
				}
			}

			return count;
		}

		// Rows of cameos with a count on each, bottom edge at `bottom`, centred on centreX, with
		// "held/capacity" above them.
		void DrawContents(const std::vector<TypeCount>& groups, int count, int capacity, int centreX, int bottom)
		{
			constexpr int PerRow = 5;
			const int groupCount = static_cast<int>(groups.size());
			const int rowCount = (groupCount + PerRow - 1) / PerRow;
			const int cellWidth = CameoWidth + 2;
			const int headerTop = bottom - rowCount * (CameoHeight + 2) - LineHeight;

			wchar_t header[32];
			if (capacity > 0)
				swprintf_s(header, L"%d/%d", count, capacity);
			else
				swprintf_s(header, L"%d", count);

			const int widest = std::min(groupCount, PerRow) * cellWidth - 2;
			FillTranslucent({ centreX - widest / 2, headerTop, widest, LineHeight }, ColorStruct { 0, 0, 0 }, 55);
			// The font draws a couple of pixels below its top.
			Text(header, centreX, headerTop - 2, Rgb(255, 255, 255));

			for (int row = 0; row < rowCount; ++row)
			{
				const int first = row * PerRow;
				const int inRow = std::min(PerRow, groupCount - first);
				const int rowLeft = centreX - (inRow * cellWidth - 2) / 2;
				const int top = headerTop + LineHeight + row * (CameoHeight + 2);

				for (int i = 0; i < inRow; ++i)
				{
					const auto& group = groups[first + i];
					const int x = rowLeft + i * cellWidth;
					Cameo(group.Type, x, top);

					if (group.Count > 1)
					{
						wchar_t badge[16];
						swprintf_s(badge, L"x%d", group.Count);
						CameoBadge(badge, x, top);
					}
				}
			}
		}

#pragma endregion Drawing

		bool IsDrawnThisFrame(const ObjectClass* pObject)
		{
			return pObject->IsSelected && pObject->IsOnMap && !pObject->InLimbo && pObject->IsVisible;
		}
	}

	// Whether this building's selection drawing is the spawner's rather than Ares's.
	bool TakesOverProduction(const BuildingClass* pBuilding)
	{
		return pBuilding->IsSelected
			&& pBuilding->IsOnMap
			&& pBuilding->Type->Factory != AbstractType::None
			&& PhobosMovedSpiedSelection()
			&& ViewerSeesProduction(pBuilding->Owner);
	}

	void RememberCancel(const FactoryClass* pFactory)
	{
		if (!pFactory->Object || !pFactory->Owner)
			return;

		const auto pType = pFactory->Object->GetTechnoType();
		Slot slot;
		if (!SlotOf(pType, slot))
			return;

		if (Cancels.size() >= CancelsKept)
			Cancels.erase(Cancels.begin());

		Cancels.push_back({ pFactory->Owner, slot, pType,
			std::clamp(pFactory->Production.Value, 0, FullStage), GetTickCount() });
	}

	void Draw()
	{
		constexpr int MaxContentPanels = 8;
		int contentPanels = 0;
		std::vector<TypeCount> groups;

		for (const auto pObject : ObjectClass::CurrentObjects)
		{
			if (!pObject || !IsDrawnThisFrame(pObject))
				continue;

			const auto pTechno = abstract_cast<TechnoClass*>(pObject);
			if (!pTechno)
				continue;

			const auto pBuilding = specific_cast<BuildingClass*>(pTechno);
			int contentsBottom = 0;
			bool haveAnchor = false;
			int centreX = 0;

			if (pBuilding)
			{
				const auto anchor = BuildingAnchor(pBuilding);
				centreX = anchor.X;
				contentsBottom = anchor.Y;
				haveAnchor = true;

				if (TakesOverProduction(pBuilding))
				{
					const int top = DrawProduction(pBuilding, anchor);
					if (top != anchor.Y)
					{
						contentsBottom = top - 2;
					}
					else
					{
						// Nothing in production: the building's own info, as Ares draws it then.
						const Point2D location { anchor.X - 10, anchor.Y + 10 };
						pBuilding->DrawExtraInfo(location, anchor, DSurface::ViewBounds);
					}
				}
			}

			if (contentPanels >= MaxContentPanels || !ViewerSeesContents(pTechno->Owner))
				continue;

			int capacity = 0;
			const int count = CollectContents(pTechno, groups, capacity);
			if (!count)
				continue;

			if (!haveAnchor)
			{
				// Above the health bar and pips.
				const auto [point, visible] = TacticalClass::Instance->CoordsToClient(pTechno->GetCenterCoords());
				centreX = point.X;
				contentsBottom = point.Y - 34;
			}

			DrawContents(groups, count, capacity, centreX, contentsBottom);
			++contentPanels;
		}
	}
}

// Ahead of Phobos's Tactical_RenderLayers_DrawInfoTipAndSpiedSelection at the same address. Here the
// game is about to push the bounds and call Draw_Extras for the object in EBX at the point in EAX.
DEFINE_HOOK(0x6D9781, Tactical_RenderLayers_TakeOverSpiedSelection, 0x5)
{
	enum { AfterDrawExtras = 0x6D978F };

	GET(ObjectClass*, pObject, EBX);
	GET(Point2D*, pLocation, EAX);

	const auto pBuilding = specific_cast<BuildingClass*>(pObject);
	if (!pBuilding || !SelectionInfo::TakesOverProduction(pBuilding))
		return 0;

	pObject->DrawExtras(pLocation, &DSurface::ViewBounds);
	return AfterDrawExtras;
}

DEFINE_HOOK(0x6D4684, Tactical_Render_DrawSelectionInfo, 0x6)
{
	SelectionInfo::Draw();
	return 0;
}

// FactoryClass::Abandon, called by HouseClass::Abandon_Production for a player's cancel and by
// HouseClass::AI_Raise_Money when the AI cancels to free money. Its other callers replace or tear
// down the factory and are not cancels.
DEFINE_HOOK(0x4C9FF0, FactoryClass_Abandon_RememberCancel, 0x6)
{
	GET(FactoryClass*, pFactory, ECX);
	GET_STACK(DWORD, returnAddress, 0x0);

	if (returnAddress == 0x4FABAB || returnAddress == 0x4FE07F)
		SelectionInfo::RememberCancel(pFactory);

	return 0;
}
