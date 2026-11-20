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

// The observer sidebar as four pages, one per sidebar tab: Players, Production, Army and Stats.
//
// For an observer the game draws one strip: a tile per player with kills, units and credits
// (SidebarClass::StripClass::Draw_It from 0x6AA05B), and leaves the four tab buttons disabled, as no
// strip has cameos. Here the tabs get their own icons - made from the game's own tab art, with the
// cameo-type icon swapped for a flag, a gear, a tank and a bar chart - and are enabled, and the
// strip draws the page of the active tab instead. Changing tab goes through the game's own paths:
// clicking a tab, or the StructureTab / DefenseTab / InfantryTab / UnitTab hotkeys (Q W E R by
// default), which only act on a tab with cameos (SidebarClass::StripClass::Has_Cameos, made true
// for the observer). The scroll buttons and the SidebarUp / SidebarDown hotkeys scroll the page.
//
// Players: a tile per player grouped by team (team header with its army value), or two lines per
// player in Compact. Clicking a tile moves the camera to the player's construction yard, then on
// through any others and their MCVs.
// Production: each player's active queues and superweapon timers as half-size cameos.
// Army: each player's units on the field by total value. Stats: bars comparing the players.
//
// Everything here only reads the simulation, and only to draw it. Income is counted as houses are
// paid (HouseClass::Refund_Money) and value lost as objects die (TechnoClass::Record_The_Kill_Object);
// the rest is read from the houses each update.

#include "Observers.Production.h"

#include <Spawner/Spawner.h>
#include <Utilities/Macro.h>

#include <AircraftTypeClass.h>
#include <BitFont.h>
#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <ConvertClass.h>
#include <Drawing.h>
#include <FactoryClass.h>
#include <FileFormats/SHP.h>
#include <FileSystem.h>
#include <HouseClass.h>
#include <HouseTypeClass.h>
#include <InfantryTypeClass.h>
#include <MapClass.h>
#include <RulesClass.h>
#include <SessionClass.h>
#include <ShapeButtonClass.h>
#include <SidebarClass.h>
#include <SuperClass.h>
#include <SuperWeaponTypeClass.h>
#include <Surface.h>
#include <TacticalClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <UnitClass.h>
#include <UnitTypeClass.h>
#include <Unsorted.h>
#include <VocClass.h>
#include <WWMouseClass.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <vector>

using namespace ObserverProduction;

static_assert(sizeof(ShapeButtonClass) == 0x60, "ShapeButtonClass must match the game's TabShapes stride");
static_assert(offsetof(ShapeButtonClass, ShapeData) == 0x58, "ShapeButtonClass layout slipped");
static_assert(offsetof(ShapeButtonClass, IsShapeLoaded) == 0x5C, "ShapeButtonClass layout slipped");
static_assert(offsetof(SidebarClass, Tabs) == 0x1544, "SidebarClass layout slipped");
static_assert(sizeof(StripClass) == 0xF94, "StripClass must match the game's stride");

namespace ObserverSidebar
{
	namespace
	{
#pragma region Game data

		template <typename T>
		T& At(uintptr_t address) { return *reinterpret_cast<T*>(address); }

		// SidebarClass::Init_For_House loads these.
		SHPStruct* Side2Shape() { return At<SHPStruct*>(0xB0B46C); }
		SHPStruct* ObserverTile(int side)
		{
			switch (side)
			{
			case 1: return At<SHPStruct*>(0xB0B494); // OBSSOVI.SHP
			case 2: return At<SHPStruct*>(0xB0B498); // OBSYURI.SHP
			default: return At<SHPStruct*>(0xB0B490); // OBSALLI.SHP
			}
		}
		// USAI.SHP ... YRII.SHP, by country, as StripClass::Draw_It picks them.
		SHPStruct* CountryFlag(int country)
		{
			return country >= 0 && country <= 9 ? At<SHPStruct*>(0xB0B4A4 + 4 * country) : nullptr;
		}
		ConvertClass* SidebarDrawer() { return At<ConvertClass*>(0x87F6CC); }
		ConvertClass* ObserverDrawer() { return At<ConvertClass*>(0x87F6D0); }
		ConvertClass* YuriFlagDrawer() { return At<ConvertClass*>(0xB0FC08); }
		int ObjectHeight() { return At<int>(0xB0B500); }
		ShapeButtonClass& ScrollUpButton() { return At<ShapeButtonClass>(0xB0B408); }
		ShapeButtonClass& ScrollDownButton() { return At<ShapeButtonClass>(0xB0B328); }

		constexpr int ScrollUpKey = 0x8000 | 200;   // R-UP.SHP's button
		constexpr int ScrollDownKey = 0x8000 | 201; // R-DN.SHP's button
		constexpr int FramesPerSecond = 15;          // game timers: Rules minutes are 900 frames

		enum class Page : int
		{
			Players,
			Production,
			Army,
			Stats,
			Count
		};

		bool Enabled()
		{
			return !Spawner::Enabled || Spawner::GetConfig()->Observer_SidebarPages;
		}

		bool Active()
		{
			return Enabled() && HouseClass::IsCurrentPlayerObserver() && !SessionClass::IsCampaign();
		}

		Page CurrentPage()
		{
			const int tab = SidebarClass::Instance.ActiveTabIndex;
			return tab >= 0 && tab < static_cast<int>(Page::Count) ? static_cast<Page>(tab) : Page::Players;
		}

		// The houses the observer sidebar lists, as Observers.cpp's ShowHouseOnObserverSidebar.
		bool Listed(const HouseClass* pHouse)
		{
			if (pHouse->Type->MultiplayPassive || pHouse->IsInitiallyObserver())
				return false;

			const bool showAI = Spawner::Enabled && Spawner::GetConfig()->Observer_ShowAIOnSidebar;
			return showAI || pHouse->IsHumanPlayer;
		}

		int Seconds(int frames) { return (std::max(frames, 0) + FramesPerSecond - 1) / FramesPerSecond; }

#pragma endregion Game data

#pragma region Tracking

		// Money each house has earned: every payment through HouseClass::Refund_Money but refunds and
		// the AI's starting bonus. Ares pays harvesters and slaves through it too (its unload hooks),
		// so the game's own HarvestedCredits is never added to.
		std::vector<int> Earned;

		int EarnedBy(const HouseClass* pHouse)
		{
			const int index = pHouse->ArrayIndex;
			return index >= 0 && index < static_cast<int>(Earned.size()) ? Earned[index] : 0;
		}

		// What it had earned a minute ago, from samples a second apart.
		struct IncomeTrack
		{
			std::deque<std::pair<int, int>> Samples; // frame, earned
		};

		std::vector<IncomeTrack> Income;
		constexpr int IncomeWindow = 60 * FramesPerSecond;

		void SampleIncome(const HouseClass* pHouse, int frame)
		{
			const int index = pHouse->ArrayIndex;
			if (index >= static_cast<int>(Income.size()))
				Income.resize(index + 1);

			auto& samples = Income[index].Samples;
			if (!samples.empty() && frame < samples.back().first)
				samples.clear();

			if (samples.empty() || frame - samples.back().first >= FramesPerSecond)
				samples.emplace_back(frame, EarnedBy(pHouse));

			while (samples.size() > 2 && samples[1].first <= frame - IncomeWindow)
				samples.pop_front();
		}

		int IncomePerMinute(const HouseClass* pHouse)
		{
			const int index = pHouse->ArrayIndex;
			if (index >= static_cast<int>(Income.size()))
				return 0;

			const auto& samples = Income[index].Samples;
			if (samples.size() < 2)
				return 0;

			const int span = samples.back().first - samples.front().first;
			if (span < 10 * FramesPerSecond)
				return 0;

			const int earned = samples.back().second - samples.front().second;
			return static_cast<int>(static_cast<long long>(earned) * IncomeWindow / span);
		}

		// Cost of what each house has lost, with a snapshot every half minute so a replay seek back
		// can pick up where it was.
		std::vector<int> Lost;
		std::vector<std::pair<int, std::vector<int>>> LostHistory;
		int LastTrackedFrame = -1;
		constexpr int LostSnapshotEvery = 30 * FramesPerSecond;

		void TrackFrame(int frame)
		{
			if (frame < LastTrackedFrame)
			{
				while (!LostHistory.empty() && LostHistory.back().first > frame)
					LostHistory.pop_back();

				if (LostHistory.empty())
					Lost.clear();
				else
					Lost = LostHistory.back().second;
			}

			if (LostHistory.empty() || frame - LostHistory.back().first >= LostSnapshotEvery)
				LostHistory.emplace_back(frame, Lost);

			LastTrackedFrame = frame;
		}

		void AddLost(const HouseClass* pHouse, int value)
		{
			const int index = pHouse->ArrayIndex;
			if (index < 0)
				return;

			if (index >= static_cast<int>(Lost.size()))
				Lost.resize(index + 1);

			Lost[index] += value;
		}

		int LostValue(const HouseClass* pHouse)
		{
			const int index = pHouse->ArrayIndex;
			return index >= 0 && index < static_cast<int>(Lost.size()) ? Lost[index] : 0;
		}

		// Value of what the house has destroyed, from the game's kill counters by type.
		template <typename TType>
		int KilledValue(UnitTrackerClass& tracker, const DynamicVectorClass<TType*>& types)
		{
			if (tracker.InNetworkFormat)
				return 0;

			int value = 0;
			const int count = std::min(types.Count, static_cast<int>(std::size(tracker.UnitTotals)));
			for (int i = 0; i < count; ++i)
			{
				if (const int killed = tracker.UnitTotals[i])
					value += killed * types.Items[i]->GetCost();
			}

			return value;
		}

		int DestroyedValue(HouseClass* pHouse)
		{
			return KilledValue(pHouse->KilledUnitTypes, UnitTypeClass::Array)
				+ KilledValue(pHouse->KilledInfantryTypes, InfantryTypeClass::Array)
				+ KilledValue(pHouse->KilledAircraftTypes, AircraftTypeClass::Array)
				+ KilledValue(pHouse->KilledBuildingTypes, BuildingTypeClass::Array);
		}

#pragma endregion Tracking

#pragma region Model

		struct SuperInfo
		{
			const SuperWeaponTypeClass* Type;
			int SecondsLeft;
			bool Ready;
			bool Paused;
		};

		struct PlayerInfo
		{
			HouseClass* House;
			int Team;
			bool Defeated;
			int Credits;
			int Income;
			int Army;
			int PowerOutput;
			int PowerDrain;
			int Harvesters;
			int Destroyed;
			int Lost;
			bool HasRow[static_cast<int>(Slot::Count)];
			ProductionRow Rows[static_cast<int>(Slot::Count)];
			std::vector<SuperInfo> Supers;
			std::vector<TypeCount> Units; // highest total value first
			int Infantry;
			int Vehicles;
			int Aircraft;
			int Ships;
		};

		std::vector<PlayerInfo> Players;
		int TeamCount = 0;
		bool ShowTeams = false;

		// UI state.
		bool Compact = false;
		int Scroll[static_cast<int>(Page::Count)] {};
		int MaxScroll[static_cast<int>(Page::Count)] {};
		bool Dirty = true;
		DWORD Signature = 0;
		int LastUpdateFrame = -1;

		bool IsBaseUnit(const TechnoTypeClass* pType)
		{
			for (const auto pBase : RulesClass::Instance->BaseUnit)
			{
				if (pBase == pType)
					return true;
			}

			return false;
		}

		void CollectUnits(std::vector<int>& rowOf)
		{
			for (const auto pTechno : TechnoClass::Array)
			{
				if (!pTechno || !pTechno->IsAlive || pTechno->Health <= 0 || !pTechno->Owner)
					continue;

				const int houseIndex = pTechno->Owner->ArrayIndex;
				if (houseIndex < 0 || houseIndex >= static_cast<int>(rowOf.size()) || rowOf[houseIndex] < 0)
					continue;

				const auto what = pTechno->WhatAmI();
				if (what == AbstractType::Building)
					continue;

				// In limbo: inside a transport counts, still in the factory or dying does not.
				if (pTechno->InLimbo && !pTechno->Transporter)
					continue;

				const auto pType = pTechno->GetTechnoType();
				if (!pType || pType->Spawned || pType->MissileSpawn)
					continue;

				auto& player = Players[rowOf[houseIndex]];
				if (what == AbstractType::Unit)
				{
					const auto pUnitType = static_cast<const UnitTypeClass*>(pType);
					if (pUnitType->Harvester || pType->ResourceGatherer)
					{
						++player.Harvesters;
						continue;
					}

					if (IsBaseUnit(pType))
						continue;
				}

				switch (what)
				{
				case AbstractType::Infantry: ++player.Infantry; break;
				case AbstractType::Aircraft: ++player.Aircraft; break;
				default: pType->Naval ? ++player.Ships : ++player.Vehicles; break;
				}

				player.Army += pType->GetCost();
				AddToGroups(player.Units, pType);
			}

			for (auto& player : Players)
			{
				std::stable_sort(player.Units.begin(), player.Units.end(), [](const TypeCount& a, const TypeCount& b)
				{
					return a.Type->GetCost() * a.Count > b.Type->GetCost() * b.Count;
				});
			}
		}

		void Collect()
		{
			const int frame = Unsorted::CurrentFrame;
			TrackFrame(frame);

			Players.clear();
			std::vector<int> rowOf(HouseClass::Array.Count, -1);

			for (const auto pHouse : HouseClass::Array)
			{
				if (!pHouse || !Listed(pHouse))
					continue;

				SampleIncome(pHouse, frame);

				PlayerInfo player {};
				player.House = pHouse;
				player.Defeated = pHouse->Defeated;
				player.Credits = std::max(0, static_cast<int>(pHouse->Available_Money()));
				player.Income = IncomePerMinute(pHouse);
				player.PowerOutput = pHouse->PowerOutput;
				player.PowerDrain = pHouse->PowerDrain;
				player.Destroyed = DestroyedValue(pHouse);
				player.Lost = LostValue(pHouse);

				// Teams: allied with an earlier listed player, else a team of its own.
				player.Team = -1;
				for (const auto& other : Players)
				{
					if (other.House->IsMutualAlly(pHouse))
					{
						player.Team = other.Team;
						break;
					}
				}

				if (player.Team < 0)
					player.Team = TeamCount++;

				if (!player.Defeated)
				{
					for (int slot = 0; slot < static_cast<int>(Slot::Count); ++slot)
					{
						const auto s = static_cast<Slot>(slot);
						player.HasRow[slot] = GetRow(FactoryFor(pHouse, s), pHouse, s, player.Rows[slot]);
					}

					for (const auto pSuper : pHouse->Supers)
					{
						if (!pSuper || !pSuper->IsPresent || !pSuper->Type || !pSuper->Type->ShowTimer)
							continue;

						const int left = pSuper->RechargeTimer.GetTimeLeft();
						player.Supers.push_back({ pSuper->Type, Seconds(left), pSuper->IsReady || left <= 0,
							pSuper->IsSuspended });
					}

					std::stable_sort(player.Supers.begin(), player.Supers.end(), [](const SuperInfo& a, const SuperInfo& b)
					{
						return a.Ready != b.Ready ? a.Ready : a.SecondsLeft < b.SecondsLeft;
					});
				}

				rowOf[pHouse->ArrayIndex] = static_cast<int>(Players.size());
				Players.push_back(std::move(player));
			}

			// Team numbers in the order the teams first appear.
			std::vector<int> renumber(TeamCount, -1);
			int teams = 0;
			for (auto& player : Players)
			{
				if (renumber[player.Team] < 0)
					renumber[player.Team] = teams++;

				player.Team = renumber[player.Team];
			}

			TeamCount = 0;
			ShowTeams = teams > 1 && teams < static_cast<int>(Players.size());
			std::stable_sort(Players.begin(), Players.end(), [](const PlayerInfo& a, const PlayerInfo& b)
			{
				return a.Team < b.Team;
			});

			for (int i = 0; i < static_cast<int>(Players.size()); ++i)
				rowOf[Players[i].House->ArrayIndex] = i;

			CollectUnits(rowOf);
		}

#pragma endregion Model

#pragma region Drawing primitives

		RectangleStruct Clip {};

		struct Hit
		{
			RectangleStruct Area;
			enum class Kind { Density, Tile } What;
			HouseClass* House;
			bool CompactValue;
		};

		std::vector<Hit> Hits;

		int Rgb(int r, int g, int b) { return Drawing::RGB_To_Int(r, g, b); }
		int HouseColor(const PlayerInfo& player)
		{
			if (player.Defeated)
				return Rgb(124, 124, 132);

			const auto& c = player.House->Color;
			return Rgb(c.R, c.G, c.B);
		}

		int White() { return Rgb(255, 255, 255); }
		int Label() { return Rgb(156, 196, 255); }
		int Muted() { return Rgb(128, 148, 180); }
		int Neutral() { return Rgb(201, 216, 240); }
		int Good() { return Rgb(127, 224, 127); }

		RectangleStruct Intersect(const RectangleStruct& a, const RectangleStruct& b)
		{
			const int left = std::max(a.X, b.X);
			const int top = std::max(a.Y, b.Y);
			const int right = std::min(a.X + a.Width, b.X + b.Width);
			const int bottom = std::min(a.Y + a.Height, b.Y + b.Height);
			return { left, top, std::max(0, right - left), std::max(0, bottom - top) };
		}

		void Fill(int x, int y, int width, int height, int color)
		{
			auto rect = Intersect({ x, y, width, height }, Clip);
			if (rect.Width > 0 && rect.Height > 0)
				DSurface::Sidebar->FillRect(&rect, color);
		}

		void Shade(int x, int y, int width, int height, int opacity, ColorStruct color = { 0, 0, 0 })
		{
			auto rect = Intersect({ x, y, width, height }, Clip);
			if (rect.Width > 0 && rect.Height > 0)
				DSurface::Sidebar->FillRectTrans(&rect, &color, opacity);
		}

		void Shape(ConvertClass* pDrawer, SHPStruct* pShape, int frame, int x, int y)
		{
			if (!pDrawer || !pShape)
				return;

			// Relative to the bounds' origin, as CC_Draw_Shape places it.
			Point2D position { x - Clip.X, y - Clip.Y };
			DSurface::Sidebar->DrawSHP(pDrawer, pShape, frame, &position, &Clip, BlitterFlags::bf_400,
				0, 0, ZGradient::Ground, 1000, 0, nullptr, 0, 0, 0);
		}

		int TextWidth(const wchar_t* pText)
		{
			int width = 0, height = 0;
			BitFont::Instance->GetTextDimension(pText, &width, &height, 4000);
			return width;
		}

		// The 8-point font at its cell's top-left; the glyphs start two pixels down.
		// Fancy_Text_Print treats the text as a printf format, so a lone % would be swallowed.
		void Text(const wchar_t* pText, int x, int y, int color, TextPrintType align = static_cast<TextPrintType>(0))
		{
			wchar_t escaped[256];
			size_t length = 0;
			for (auto p = pText; *p && length + 2 < std::size(escaped); ++p)
			{
				if (*p == L'%')
					escaped[length++] = L'%';

				escaped[length++] = *p;
			}
			escaped[length] = 0;

			// Relative to the bounds' origin, as the game's text drawing places it.
			Point2D location { x - Clip.X, y - Clip.Y };
			DSurface::Sidebar->DrawText(escaped, &Clip, &location, color, 0,
				TextPrintType::Point8 | TextPrintType::FullShadow | align);
		}

		void TextRight(const wchar_t* pText, int right, int y, int color)
		{
			Text(pText, right, y, color, TextPrintType::Right);
		}

		// The text cut down to the width, ending in an ellipsis.
		void Fit(const wchar_t* pText, int maxWidth, wchar_t (&out)[64])
		{
			wcsncpy_s(out, pText ? pText : L"", _TRUNCATE);
			if (TextWidth(out) <= maxWidth)
				return;

			size_t length = wcslen(out);
			while (length > 1)
			{
				out[--length] = L'\0';
				wchar_t candidate[64];
				swprintf_s(candidate, L"%s\u2026", out);
				if (TextWidth(candidate) <= maxWidth)
				{
					wcscpy_s(out, candidate);
					return;
				}
			}
		}

		void Money(int value, wchar_t (&out)[32], const wchar_t* pPrefix = L"$")
		{
			value = std::max(0, value);
			if (value >= 10000)
			{
				const int tenths = value / 100;
				if (tenths % 10)
					swprintf_s(out, L"%s%d.%dk", pPrefix, tenths / 10, tenths % 10);
				else
					swprintf_s(out, L"%s%dk", pPrefix, tenths / 10);
			}
			else if (value >= 1000)
			{
				swprintf_s(out, L"%s%d,%03d", pPrefix, value / 1000, value % 1000);
			}
			else
			{
				swprintf_s(out, L"%s%d", pPrefix, value);
			}
		}

		void Short(int value, wchar_t (&out)[32], const wchar_t* pPrefix = L"$")
		{
			value = std::max(0, value);
			if (value >= 1000)
			{
				const int tenths = value / 100;
				if (tenths % 10)
					swprintf_s(out, L"%s%d.%dk", pPrefix, tenths / 10, tenths % 10);
				else
					swprintf_s(out, L"%s%dk", pPrefix, tenths / 10);
			}
			else
			{
				swprintf_s(out, L"%s%d", pPrefix, value);
			}
		}

		void Clock(int seconds, wchar_t (&out)[32])
		{
			swprintf_s(out, L"%d:%02d", seconds / 60, seconds % 60);
		}

		int PowerColor(const PlayerInfo& player)
		{
			if (player.PowerOutput < player.PowerDrain)
				return Rgb(255, 75, 58);

			if (player.PowerOutput - player.PowerDrain < player.PowerOutput / 10)
				return Rgb(242, 196, 60);

			return Rgb(88, 216, 88);
		}

		int StateColor(RowState state)
		{
			switch (state)
			{
			case RowState::OnHold:    return Rgb(230, 200, 30);
			case RowState::NoFunds:   return Rgb(240, 130, 20);
			case RowState::Ready:     return Rgb(90, 170, 255);
			case RowState::Cancelled: return Rgb(220, 40, 40);
			default:                  return Rgb(40, 200, 40);
			}
		}

		void Bar(int x, int y, int width, int height, int color, int filled)
		{
			Fill(x, y, width, height, 0);
			Fill(x, y, std::clamp(filled, 0, width), height, color);
		}

#pragma endregion Drawing primitives

#pragma region Shapes

		// A frame of a shape as palette indices on its full canvas, 0 for transparent.
		bool DecodeFrame(SHPStruct* pShape, int frame, std::vector<uint8_t>& pixels, int& width, int& height)
		{
			const auto pFile = pShape ? pShape->GetData() : nullptr;
			if (!pFile || frame >= pFile->Frames)
				return false;

			width = pFile->Width;
			height = pFile->Height;
			pixels.assign(static_cast<size_t>(width) * height, 0);

			const auto& header = pFile->GetFrameHeader(frame);
			const auto pData = pShape->GetPixels(frame);
			if (!pData || header.Width <= 0 || header.Height <= 0)
				return true;

			auto put = [&](int x, int y, uint8_t value)
			{
				const int px = header.Left + x;
				const int py = header.Top + y;
				if (px >= 0 && px < width && py >= 0 && py < height)
					pixels[static_cast<size_t>(py) * width + px] = value;
			};

			if (header.Flags & 2)
			{
				// Scanlines, each led by its length; with bit 1 also set, runs of 0 are packed as 0, count.
				const bool packed = (header.Flags & 1) != 0;
				auto pLine = pData;
				for (int y = 0; y < header.Height; ++y)
				{
					const int length = *reinterpret_cast<const uint16_t*>(pLine);
					if (length < 2)
						break;

					auto p = pLine + 2;
					const auto pEnd = pLine + length;
					int x = 0;
					while (p < pEnd && x < header.Width)
					{
						const uint8_t value = *p++;
						if (packed && value == 0)
						{
							if (p >= pEnd)
								break;

							x += *p++;
						}
						else
						{
							put(x++, y, value);
						}
					}

					pLine = pEnd;
				}
			}
			else
			{
				for (int y = 0; y < header.Height; ++y)
				{
					for (int x = 0; x < header.Width; ++x)
						put(x, y, pData[y * header.Width + x]);
				}
			}

			return true;
		}

		// A shape built here: header, frame headers and raw frames in one block.
		std::vector<uint8_t> BuildShape(int width, int height, const std::vector<std::vector<uint8_t>>& frames)
		{
			const size_t headerSize = 8 + frames.size() * sizeof(SHPFrame);
			const size_t frameSize = static_cast<size_t>(width) * height;
			std::vector<uint8_t> data(headerSize + frames.size() * frameSize, 0);

			const uint16_t fileHeader[4] = { 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height),
				static_cast<uint16_t>(frames.size()) };
			memcpy(data.data(), fileHeader, sizeof(fileHeader));

			for (size_t i = 0; i < frames.size(); ++i)
			{
				SHPFrame frame {};
				frame.Left = 0;
				frame.Top = 0;
				frame.Width = static_cast<short>(width);
				frame.Height = static_cast<short>(height);
				frame.Flags = 0;
				frame.Offset = static_cast<int>(headerSize + i * frameSize);
				memcpy(data.data() + 8 + i * sizeof(SHPFrame), &frame, sizeof(frame));
				memcpy(data.data() + frame.Offset, frames[i].data(), frameSize);
			}

			return data;
		}

		// Icons for the four pages, 16x16.
		const char* const FlagIcon[16] = {
			"##..............",
			"##.#####........",
			"########.####...",
			"#############.##",
			"################",
			"################",
			"################",
			"################",
			"##.....#########",
			"##.........#####",
			"##..............",
			"##..............",
			"##..............",
			"##..............",
			"####............",
			"####............" };
		const char* const TankIcon[16] = {
			"................",
			"................",
			"................",
			"......######....",
			".....########...",
			".....###########",
			".....########...",
			"..############..",
			".##############.",
			"################",
			"################",
			".##.##.##.##.##.",
			"..############..",
			"................",
			"................",
			"................" };
		const char* const BarsIcon[16] = {
			"................",
			"............###.",
			"............###.",
			"............###.",
			"........###.###.",
			"........###.###.",
			"........###.###.",
			"....###.###.###.",
			"....###.###.###.",
			"....###.###.###.",
			"###.###.###.###.",
			"###.###.###.###.",
			"###.###.###.###.",
			"###.###.###.###.",
			"################",
			"................" };

		bool IconPixel(int tab, int x, int y)
		{
			switch (tab)
			{
			case 0: return FlagIcon[y][x] == '#';
			case 2: return TankIcon[y][x] == '#';
			case 3: return BarsIcon[y][x] == '#';
			default:
			{
				// A gear: eight teeth round a ring.
				const double dx = x - 7.5, dy = y - 7.5;
				const double radius = std::sqrt(dx * dx + dy * dy);
				const bool tooth = std::cos(std::atan2(dy, dx) * 8.0) > 0.25;
				return radius > 2.4 && ((radius <= 7.6 && tooth) || radius <= 5.6);
			}
			}
		}

		uint8_t MostCommon(const std::vector<uint8_t>& values)
		{
			int counts[256] {};
			for (const auto value : values)
				++counts[value];

			return static_cast<uint8_t>(std::max_element(std::begin(counts), std::end(counts)) - std::begin(counts));
		}

		struct TabArt
		{
			SHPStruct* Source = nullptr; // the game's tab shape this was made from
			std::vector<uint8_t> Data;
			SHPStruct* Shape() { return Data.empty() ? nullptr : reinterpret_cast<SHPStruct*>(Data.data()); }
		};

		TabArt Tabs[4];

		// The game's tab button with its cameo-type icon swapped for the page's. Frame 2 (disabled) is
		// the button without an icon; each other frame's background is frame 2 mapped to that frame's
		// colours, with the icon in the colour the frame's own icon has.
		bool MakeTab(int tab, SHPStruct* pSource, TabArt& art)
		{
			std::vector<uint8_t> blank;
			int width = 0, height = 0;
			if (!DecodeFrame(pSource, 2, blank, width, height) || width < 22 || height < 22)
				return false;

			std::vector<std::vector<uint8_t>> frames;
			for (int index = 0; index < 5; ++index)
			{
				std::vector<uint8_t> source;
				int w = 0, h = 0;
				if (index == 2 || !DecodeFrame(pSource, index, source, w, h) || w != width || h != height)
				{
					frames.push_back(blank);
					continue;
				}

				// Where the frame differs from the blank one is its icon.
				int map[256];
				for (auto& m : map)
					m = -1;

				std::vector<uint8_t> iconColors;
				for (size_t i = 0; i < source.size(); ++i)
				{
					if (source[i] != blank[i])
					{
						iconColors.push_back(source[i]);
					}
					else if (blank[i])
					{
						map[blank[i]] = source[i];
					}
				}

				if (iconColors.empty())
				{
					frames.push_back(source);
					continue;
				}

				const uint8_t iconColor = MostCommon(iconColors);
				std::vector<uint8_t> frame(blank.size());
				for (size_t i = 0; i < blank.size(); ++i)
					frame[i] = blank[i] && map[blank[i]] >= 0 ? static_cast<uint8_t>(map[blank[i]]) : blank[i];

				const int left = (width - 16) / 2;
				const int top = (height - 16) / 2;
				for (int y = 0; y < 16; ++y)
				{
					for (int x = 0; x < 16; ++x)
					{
						if (IconPixel(tab, x, y))
							frame[static_cast<size_t>(top + y) * width + left + x] = iconColor;
					}
				}

				frames.push_back(std::move(frame));
			}

			art.Source = pSource;
			art.Data = BuildShape(width, height, frames);
			return true;
		}

		// Half-size cameos, shrunk once per shape and cached in the surface's pixel format.
		constexpr int MiniWidth = 28;
		constexpr int MiniHeight = 22;

		struct Mini
		{
			bool Valid = false;
			uint16_t Pixels[MiniWidth * MiniHeight] {};
			bool Opaque[MiniWidth * MiniHeight] {};
		};

		std::unordered_map<const SHPStruct*, Mini> Minis;

		void Unpack(uint16_t color, int& r, int& g, int& b)
		{
			r = ((color >> Drawing::RedShiftLeft) & (0xFF >> Drawing::RedShiftRight)) << Drawing::RedShiftRight;
			g = ((color >> Drawing::GreenShiftLeft) & (0xFF >> Drawing::GreenShiftRight)) << Drawing::GreenShiftRight;
			b = ((color >> Drawing::BlueShiftLeft) & (0xFF >> Drawing::BlueShiftRight)) << Drawing::BlueShiftRight;
		}

		const Mini* GetMini(SHPStruct* pShape)
		{
			if (!pShape)
				return nullptr;

			auto& mini = Minis[pShape];
			if (mini.Valid)
				return &mini;

			const auto pDrawer = FileSystem::CAMEO_PAL;
			std::vector<uint8_t> pixels;
			int width = 0, height = 0;
			if (!pDrawer || pDrawer->BytesPerPixel != 2 || !DecodeFrame(pShape, 0, pixels, width, height)
				|| width <= 0 || height <= 0)
			{
				return nullptr;
			}

			const auto pPalette = reinterpret_cast<const uint16_t*>(pDrawer->PaletteData);
			for (int y = 0; y < MiniHeight; ++y)
			{
				const int y0 = y * height / MiniHeight;
				const int y1 = std::max(y0 + 1, (y + 1) * height / MiniHeight);
				for (int x = 0; x < MiniWidth; ++x)
				{
					const int x0 = x * width / MiniWidth;
					const int x1 = std::max(x0 + 1, (x + 1) * width / MiniWidth);
					int r = 0, g = 0, b = 0, opaque = 0, total = 0;
					for (int sy = y0; sy < y1; ++sy)
					{
						for (int sx = x0; sx < x1; ++sx)
						{
							++total;
							const uint8_t index = pixels[static_cast<size_t>(sy) * width + sx];
							if (!index)
								continue;

							int pr, pg, pb;
							Unpack(pPalette[index], pr, pg, pb);
							r += pr;
							g += pg;
							b += pb;
							++opaque;
						}
					}

					const int at = y * MiniWidth + x;
					mini.Opaque[at] = opaque * 2 >= total;
					if (opaque)
						mini.Pixels[at] = static_cast<uint16_t>(Rgb(r / opaque, g / opaque, b / opaque));
				}
			}

			mini.Valid = true;
			return &mini;
		}

		// A half-size cameo with a black edge, or a dark box when there is no shape to shrink.
		void DrawMini(SHPStruct* pShape, int x, int y)
		{
			Fill(x - 1, y - 1, MiniWidth + 2, MiniHeight + 2, 0);

			const auto pMini = GetMini(pShape);
			const auto pSurface = DSurface::Sidebar;
			if (!pMini || pSurface->GetBytesPerPixel() != 2)
			{
				Fill(x, y, MiniWidth, MiniHeight, Rgb(24, 32, 48));
				return;
			}

			const auto area = Intersect({ x, y, MiniWidth, MiniHeight }, Clip);
			if (area.Width <= 0 || area.Height <= 0)
				return;

			const auto pBase = static_cast<uint8_t*>(pSurface->Lock(0, 0));
			if (!pBase)
				return;

			const int pitch = pSurface->GetPitch();
			for (int py = area.Y; py < area.Y + area.Height; ++py)
			{
				auto pRow = reinterpret_cast<uint16_t*>(pBase + static_cast<size_t>(py) * pitch);
				for (int px = area.X; px < area.X + area.Width; ++px)
				{
					const int at = (py - y) * MiniWidth + (px - x);
					if (pMini->Opaque[at])
						pRow[px] = pMini->Pixels[at];
				}
			}

			pSurface->Unlock();
		}

		SHPStruct* CameoOf(const TechnoTypeClass* pType)
		{
			return pType ? pType->GetCameo() : nullptr;
		}

#pragma endregion Shapes

#pragma region Pages

		int Left = 0;    // left edge of the strip's cameo columns
		constexpr int Width = 121; // two cameo columns, as the game's observer tiles
		constexpr int TileHeight = 96;

		void TeamHeader(int team, int y)
		{
			int army = 0;
			for (const auto& player : Players)
			{
				if (player.Team == team && !player.Defeated)
					army += player.Army;
			}

			Shade(Left, y, Width, 14, 85, { 6, 14, 30 });
			Fill(Left, y + 13, Width, 1, Rgb(45, 74, 122));

			wchar_t text[32], value[32];
			swprintf_s(text, L"Team %d", team + 1);
			Text(text, Left + 4, y - 1, Label());
			Short(army, value, L"Army $");
			TextRight(value, Left + Width - 3, y - 1, Neutral());
		}

		// The player's active queues from the `first`th, at most maxCount side by side.
		void ProductionMinis(const PlayerInfo& player, int x, int y, int first, int maxCount, int pitch)
		{
			int shown = 0;
			int active = 0;
			for (int slot = 0; slot < static_cast<int>(Slot::Count) && shown < maxCount; ++slot)
			{
				if (!player.HasRow[slot] || active++ < first)
					continue;

				const auto& row = player.Rows[slot];
				const int mx = x + shown * pitch;
				DrawMini(CameoOf(row.Type), mx, y);
				if (row.State == RowState::OnHold || row.State == RowState::NoFunds || row.State == RowState::Cancelled)
					Shade(mx, y, MiniWidth, MiniHeight, 45);

				const bool full = row.State != RowState::Building;
				Bar(mx, y + MiniHeight + 1, MiniWidth, 2, StateColor(row.State),
					full ? MiniWidth : MiniWidth * row.Stage / FullStage);

				if (const int queued = row.QueuedCount())
				{
					wchar_t badge[16];
					swprintf_s(badge, L"+%d", queued);
					TextRight(badge, mx + MiniWidth + 1, y - 3, White());
				}

				++shown;
			}
		}

		int DrawTile(const PlayerInfo& player, int y)
		{
			const auto pHouse = player.House;
			const int color = HouseColor(player);
			wchar_t text[64], value[32];

			if (player.Defeated)
			{
				Shade(Left, y, Width, 15, 85, { 30, 30, 34 });
				Fit(pHouse->UIName, 70, text);
				Text(text, Left + 4, y - 1, color);
				TextRight(L"defeated", Left + Width - 3, y - 1, Rgb(106, 106, 114));
				return 18;
			}

			if (Compact)
			{
				Shade(Left, y, Width, 31, 90, { 4, 8, 16 });
				Fill(Left, y, 2, 31, color);
				Fill(Left + 6, y + 7, 4, 4, PowerColor(player));
				Money(player.Credits, value);
				TextRight(value, Left + Width - 4, y - 1, color);
				Fit(pHouse->UIName, Width - 13 - 4 - TextWidth(value) - 5, text);
				Text(text, Left + 13, y - 1, color);
				Short(player.Army, value, L"Army ");
				Text(value, Left + 6, y + 13, Rgb(185, 199, 221));
				Short(player.Income, value, L"+");
				wcscat_s(value, L"/m");
				TextRight(value, Left + Width - 4, y + 13, Good());
				Hits.push_back({ { Left, y, Width, 31 }, Hit::Kind::Tile, pHouse, false });
				return 33;
			}

			Shape(ObserverDrawer(), ObserverTile(pHouse->Type->SideIndex), 0, Left, y);
			Shade(Left + 3, y + 3, Width - 6, TileHeight - 6, 42);

			const int country = pHouse->Type->ArrayIndex2;
			int flagWidth = 0;
			if (const auto pFlag = CountryFlag(country))
			{
				flagWidth = pFlag->Width;
				Shape(country == 9 ? YuriFlagDrawer() : ObserverDrawer(), pFlag, 0, Left + Width - pFlag->Width - 3, y + 3);
			}

			Fit(pHouse->UIName, Width - 5 - flagWidth - 6, text);
			Text(text, Left + 5, y + 2, color);

			Money(player.Credits, value);
			Text(value, Left + 5, y + 20, color);
			Short(player.Income, value, L"+");
			wcscat_s(value, L"/m");
			TextRight(value, Left + 117, y + 20, Good());

			Short(player.Army, value, L"Army $");
			Text(value, Left + 5, y + 35, color);
			const int surplus = player.PowerOutput - player.PowerDrain;
			swprintf_s(value, L"\u26A1%s%d", surplus >= 0 ? L"+" : L"", surplus);
			TextRight(value, Left + 117, y + 35, PowerColor(player));

			ProductionMinis(player, Left + 5, y + 54, 0, 4, MiniWidth);

			if (!player.Supers.empty())
			{
				const auto& super = player.Supers.front();
				wchar_t time[32], name[64], line[96];
				if (super.Ready)
					wcscpy_s(time, L"READY");
				else
					Clock(super.SecondsLeft, time);

				wchar_t more[16] = L"";
				if (player.Supers.size() > 1)
					swprintf_s(more, L" +%d", static_cast<int>(player.Supers.size()) - 1);

				const int reserved = TextWidth(time) + TextWidth(more) + 6;
				Fit(super.Type->UIName, 110 - reserved, name);
				swprintf_s(line, L"%s %s%s", name, time, more);
				const int lineColor = super.Ready ? Rgb(255, 225, 74) : super.Paused ? Muted() : Rgb(232, 232, 232);
				Text(line, Left + 5, y + 77, lineColor);
			}

			Hits.push_back({ { Left, y, Width, TileHeight }, Hit::Kind::Tile, pHouse, false });
			return TileHeight + 4;
		}

		int DrawPlayers(int top)
		{
			int y = top;
			int team = -1;
			for (const auto& player : Players)
			{
				if (ShowTeams && player.Team != team)
				{
					if (team >= 0)
						y += 4;

					team = player.Team;
					TeamHeader(team, y);
					y += 16;
				}

				y += DrawTile(player, y);
			}

			return y - top;
		}

		void Panel(int top, int height)
		{
			Shade(Left - 2, top, Width + 5, height, 94, { 3, 6, 12 });
			Fill(Left - 2, top, Width + 5, 1, Rgb(27, 41, 68));
			Fill(Left - 2, top, 1, height, Rgb(27, 41, 68));
		}

		void PlayerHeader(const PlayerInfo& player, int y, const wchar_t* pRight, int rightColor)
		{
			wchar_t name[64];
			const int rightWidth = pRight ? TextWidth(pRight) + 6 : 0;
			Fit(player.House->UIName, Width - 2 - rightWidth, name);
			Text(name, Left + 2, y, HouseColor(player));
			if (pRight)
				TextRight(pRight, Left + Width, y, rightColor);
		}

		void Rule(int y)
		{
			Fill(Left + 1, y, Width - 1, 1, Rgb(26, 39, 64));
		}

		int DrawProduction(int top)
		{
			int y = top + 2;
			for (const auto& player : Players)
			{
				if (player.Defeated)
					continue;

				wchar_t value[32];
				Money(player.Credits, value);
				PlayerHeader(player, y, value, player.Credits < 500 ? Rgb(244, 138, 44) : Neutral());
				y += 17;

				int active = 0;
				for (int slot = 0; slot < static_cast<int>(Slot::Count); ++slot)
					active += player.HasRow[slot];

				if (!active)
				{
					Text(L"Nothing in production", Left + 2, y - 2, Rgb(111, 129, 158));
					y += 16;
				}
				else
				{
					// Four to a row.
					for (int first = 0; first < active; first += 4)
					{
						ProductionMinis(player, Left + 2, y, first, 4, MiniWidth + 2);
						y += MiniHeight + 7;
					}
				}

				if (!player.Supers.empty())
				{
					int i = 0;
					for (const auto& super : player.Supers)
					{
						if (i == 4)
						{
							i = 0;
							y += MiniHeight + 7;
						}

						const int mx = Left + 2 + i * (MiniWidth + 2);
						DrawMini(super.Type->SidebarImage, mx, y);
						Shade(mx, y + 11, MiniWidth, 11, 35);

						wchar_t time[32];
						if (super.Ready)
							wcscpy_s(time, L"RDY");
						else
							Clock(super.SecondsLeft, time);

						Text(time, mx + MiniWidth / 2, y + 7, super.Ready ? Rgb(255, 225, 74) : super.Paused ? Muted() : White(),
							TextPrintType::Center);

						const int total = std::max(1, super.Type->RechargeTime);
						const int elapsed = total - std::min(total, super.SecondsLeft * FramesPerSecond);
						Bar(mx, y + MiniHeight + 1, MiniWidth, 2, super.Ready ? Rgb(255, 210, 58) : Rgb(155, 123, 255),
							super.Ready ? MiniWidth : MiniWidth * elapsed / total);
						++i;
					}

					y += MiniHeight + 7;
				}

				y += 2;
				Rule(y);
				y += 4;
			}

			return y - top;
		}

		int DrawArmy(int top)
		{
			int y = top + 2;
			for (const auto& player : Players)
			{
				if (player.Defeated)
					continue;

				wchar_t value[32];
				Short(player.Army, value);
				PlayerHeader(player, y, value, Neutral());
				y += 17;

				const int shown = std::min(static_cast<int>(player.Units.size()), 8);
				for (int i = 0; i < shown; ++i)
				{
					const auto& group = player.Units[i];
					const int mx = Left + 2 + (i % 4) * (MiniWidth + 2);
					const int my = y + (i / 4) * (MiniHeight + 3);
					DrawMini(CameoOf(group.Type), mx, my);
					wchar_t count[16];
					swprintf_s(count, L"%d", group.Count);
					TextRight(count, mx + MiniWidth + 1, my + 9, White());
				}

				y += (shown + 3) / 4 * (MiniHeight + 3);

				wchar_t footer[96] = L"";
				auto append = [&](const wchar_t* pLabel, int count)
				{
					if (!count)
						return;

					wchar_t part[32];
					swprintf_s(part, L"%s%s %d", footer[0] ? L" \u00B7 " : L"", pLabel, count);
					wcscat_s(footer, part);
				};

				if (player.Units.size() > 8)
					swprintf_s(footer, L"+%d more", static_cast<int>(player.Units.size()) - 8);

				append(L"Inf", player.Infantry);
				append(L"Veh", player.Vehicles);
				append(L"Air", player.Aircraft);
				append(L"Sea", player.Ships);
				if (!footer[0])
					wcscpy_s(footer, L"No units");

				wchar_t fitted[64];
				Fit(footer, Width - 2, fitted);
				Text(fitted, Left + 2, y - 1, Rgb(128, 148, 180));
				y += 17;
				Rule(y);
				y += 4;
			}

			return y - top;
		}

		int DrawStats(int top)
		{
			struct Metric
			{
				const wchar_t* Label;
				int PlayerInfo::* Field;
				bool IsMoney;
			};

			static constexpr Metric Metrics[] = {
				{ L"Credits", &PlayerInfo::Credits, true },
				{ L"Income / min", &PlayerInfo::Income, true },
				{ L"Army value", &PlayerInfo::Army, true },
				{ L"Value destroyed", &PlayerInfo::Destroyed, true },
				{ L"Value lost", &PlayerInfo::Lost, true },
				{ L"Harvesters", &PlayerInfo::Harvesters, false },
			};

			int y = top + 1;
			for (const auto& metric : Metrics)
			{
				Text(metric.Label, Left + 2, y, Label());
				y += 16;

				int max = 1;
				for (const auto& player : Players)
				{
					if (!player.Defeated)
						max = std::max(max, player.*metric.Field);
				}

				for (const auto& player : Players)
				{
					if (player.Defeated)
						continue;

					const int value = player.*metric.Field;
					Shade(Left + 2, y + 3, 76, 6, 20, { 255, 255, 255 });
					const auto& c = player.House->Color;
					Fill(Left + 2, y + 3, std::max(1, 76 * value / max), 6, Rgb(c.R, c.G, c.B));

					wchar_t text[32];
					if (metric.IsMoney)
						Short(value, text);
					else
						swprintf_s(text, L"%d", value);

					TextRight(text, Left + Width, y - 3, value == max ? White() : Rgb(169, 183, 204));
					y += 12;
				}

				y += 5;
			}

			return y - top;
		}

#pragma endregion Pages
	}

	bool IsActive()
	{
		return Active();
	}

	// The strip's area for the page: the cameo columns, down to the bottom of the visible rows.
	void Draw(const StripClass* pStrip, int rows)
	{
		const int top = pStrip->Location.Y;
		const int height = rows * ObjectHeight();
		Left = pStrip->Location.X - DSurface::SidebarBounds.X;
		Clip = Intersect({ 0, top, DSurface::Sidebar->GetWidth(), height },
			{ 0, 0, DSurface::Sidebar->GetWidth(), DSurface::Sidebar->GetHeight() });

		// The game's own tiles are opaque; the pages are not, so the background goes back first.
		if (const auto pRow = Side2Shape())
		{
			for (int y = top; y < top + height; y += std::max<int>(1, pRow->Height))
				Shape(SidebarDrawer(), pRow, 0, 0, y);
		}

		Hits.clear();
		const auto page = CurrentPage();
		const int pageIndex = static_cast<int>(page);
		int contentTop = top + 1;
		int visible = height - 2;

		if (page == Page::Players)
		{
			// Detailed | Compact, above the list.
			constexpr int BarHeight = 15;
			Shade(Left, contentTop, Width, BarHeight - 1, 95, { 4, 10, 22 });
			const int on = White(), off = Rgb(111, 134, 168);
			Text(L"Detailed", Left + 6, contentTop - 2, Compact ? off : on);
			TextRight(L"Compact", Left + Width - 6, contentTop - 2, Compact ? on : off);
			Hits.push_back({ { Left, contentTop, Width / 2, BarHeight }, Hit::Kind::Density, nullptr, false });
			Hits.push_back({ { Left + Width / 2, contentTop, Width - Width / 2, BarHeight }, Hit::Kind::Density, nullptr, true });
			contentTop += BarHeight + 1;
			visible -= BarHeight + 1;
			Clip = Intersect(Clip, { 0, contentTop, Clip.Width, visible });
		}
		else
		{
			Panel(contentTop, visible);
		}

		const int scroll = Scroll[pageIndex];
		int contentHeight = 0;
		switch (page)
		{
		case Page::Players: contentHeight = DrawPlayers(contentTop - scroll); break;
		case Page::Production: contentHeight = DrawProduction(contentTop - scroll); break;
		case Page::Army: contentHeight = DrawArmy(contentTop - scroll); break;
		default: contentHeight = DrawStats(contentTop - scroll); break;
		}

		MaxScroll[pageIndex] = std::max(0, contentHeight - visible);
		if (Scroll[pageIndex] > MaxScroll[pageIndex])
		{
			Scroll[pageIndex] = MaxScroll[pageIndex];
			Dirty = true;
		}

		// Where the page goes on, a thumb on the right edge.
		if (MaxScroll[pageIndex] > 0)
		{
			const int track = visible - 4;
			const int thumb = std::max(18, track * visible / std::max(1, contentHeight));
			const int thumbTop = contentTop + 2 + (track - thumb) * Scroll[pageIndex] / MaxScroll[pageIndex];
			Fill(Left + Width + 4, contentTop + 2, 2, track, Rgb(30, 52, 92));
			Fill(Left + Width + 4, thumbTop, 2, thumb, Rgb(90, 169, 255));
		}

		// Hits outside the visible part are out of reach.
		Hits.erase(std::remove_if(Hits.begin(), Hits.end(), [](const Hit& hit)
		{
			const auto visibleArea = Intersect(hit.Area, Clip);
			return hit.What == Hit::Kind::Tile && (visibleArea.Width <= 0 || visibleArea.Height <= 0);
		}), Hits.end());
	}

	void RequestRedraw()
	{
		auto& sidebar = SidebarClass::Instance;
		auto& strip = sidebar.Tabs[std::clamp(sidebar.ActiveTabIndex, 0, 3)];
		strip.NeedsRedraw = true;
		MapClass::Instance.MarkNeedsRedraw(0);
	}

	void ScrollBy(int direction)
	{
		const int page = static_cast<int>(CurrentPage());
		const int before = Scroll[page];
		Scroll[page] = std::clamp(before + direction * ObjectHeight(), 0, MaxScroll[page]);
		if (Scroll[page] != before)
		{
			VocClass::PlayGlobal(RulesClass::Instance->GUITabSound, 0x2000, 1.0f);
			RequestRedraw();
		}
		else
		{
			VocClass::PlayGlobal(RulesClass::Instance->ScoldSound, 0x2000, 1.0f);
		}
	}

	// The camera to the house's construction yards, then its MCVs, one after another.
	void JumpTo(HouseClass* pHouse)
	{
		static std::unordered_map<const HouseClass*, int> next;

		std::vector<CoordStruct> targets;
		for (const auto pBuilding : pHouse->Buildings)
		{
			if (pBuilding && pBuilding->Type->ConstructionYard && !pBuilding->InLimbo && pBuilding->IsAlive)
				targets.push_back(pBuilding->GetCenterCoords());
		}

		for (const auto pTechno : TechnoClass::Array)
		{
			if (pTechno && pTechno->Owner == pHouse && pTechno->WhatAmI() == AbstractType::Unit && pTechno->IsAlive
				&& !pTechno->InLimbo && IsBaseUnit(pTechno->GetTechnoType()))
			{
				targets.push_back(pTechno->GetCenterCoords());
			}
		}

		if (targets.empty())
		{
			for (const auto pBuilding : pHouse->Buildings)
			{
				if (pBuilding && !pBuilding->InLimbo && pBuilding->IsAlive)
				{
					targets.push_back(pBuilding->GetCenterCoords());
					break;
				}
			}
		}

		if (targets.empty())
		{
			VocClass::PlayGlobal(RulesClass::Instance->ScoldSound, 0x2000, 1.0f);
			return;
		}

		int& index = next[pHouse];
		if (index >= static_cast<int>(targets.size()))
			index = 0;

		auto coords = targets[index++];
		TacticalClass::Instance->SetTacticalPosition(&coords);

		// As Jump_Home_Command (0x7315A0) finishes: stop following and redraw.
		reinterpret_cast<void(__thiscall*)(void*, ObjectClass*)>(0x4AEB30)(&MapClass::Instance, nullptr);
		MapClass::Instance.MarkNeedsRedraw(1);
		VocClass::PlayGlobal(RulesClass::Instance->GUIMainButtonSound, 0x2000, 1.0f);
	}

	void Click(int x, int y)
	{
		for (const auto& hit : Hits)
		{
			const auto& a = hit.Area;
			if (x < a.X || x >= a.X + a.Width || y < a.Y || y >= a.Y + a.Height)
				continue;

			if (hit.What == Hit::Kind::Density)
			{
				if (Compact != hit.CompactValue)
				{
					Compact = hit.CompactValue;
					Scroll[static_cast<int>(Page::Players)] = 0;
					VocClass::PlayGlobal(RulesClass::Instance->GUITabSound, 0x2000, 1.0f);
					RequestRedraw();
				}
			}
			else if (hit.House)
			{
				JumpTo(hit.House);
			}

			return;
		}
	}

	// Each frame, before the sidebar takes its input: the tabs in the pages' art and enabled, the
	// scroll buttons enabled when the page goes on, and the page redrawn when what it shows changed.
	void Update()
	{
		auto& buttons = SidebarClass::TabButtons;
		for (int i = 0; i < 4; ++i)
		{
			auto& button = buttons[i];
			auto& art = Tabs[i];
			if (button.ShapeData && button.ShapeData != art.Shape())
			{
				if (art.Source != button.ShapeData || art.Data.empty())
					MakeTab(i, button.ShapeData, art);

				if (const auto pShape = art.Shape())
				{
					// Not the button's to delete, and the game's shape is not ours to delete either.
					button.IsShapeLoaded = false;
					reinterpret_cast<void(__thiscall*)(ShapeButtonClass*, SHPStruct*, int, int)>(0x69DE00)(&button, pShape, 0, 0);
					button.NeedsRedraw = true;
				}
			}

			if (button.Disabled)
			{
				button.Enable();
				button.NeedsRedraw = true;
			}

			const bool on = i == SidebarClass::Instance.ActiveTabIndex;
			if (button.IsOn != on)
			{
				on ? button.TurnOn() : button.TurnOff();
				button.NeedsRedraw = true;
			}
		}

		const bool canScroll = MaxScroll[static_cast<int>(CurrentPage())] > 0;
		for (auto pButton : { &ScrollUpButton(), &ScrollDownButton() })
		{
			if (canScroll == pButton->Disabled)
			{
				canScroll ? pButton->Enable() : pButton->Disable();
				pButton->NeedsRedraw = true;
			}
		}

		const int frame = Unsorted::CurrentFrame;
		if (frame != LastUpdateFrame && (frame - LastUpdateFrame >= 4 || frame < LastUpdateFrame))
		{
			LastUpdateFrame = frame;
			Collect();
		}

		// What the page shows, hashed: a change redraws it.
		DWORD hash = 2166136261u;
		auto mix = [&hash](int value)
		{
			hash = (hash ^ static_cast<DWORD>(value)) * 16777619u;
		};

		const auto page = CurrentPage();
		mix(static_cast<int>(page));
		mix(Scroll[static_cast<int>(page)]);
		mix(Compact);
		for (const auto& player : Players)
		{
			mix(player.House->ArrayIndex);
			mix(player.Defeated);
			mix(player.Team);
			switch (page)
			{
			case Page::Players:
				mix(player.Credits);
				mix(player.Income);
				mix(player.Army);
				mix(player.PowerOutput - player.PowerDrain);
				break;
			case Page::Army:
				mix(player.Army);
				for (const auto& group : player.Units)
				{
					mix(reinterpret_cast<int>(group.Type));
					mix(group.Count);
				}
				break;
			case Page::Stats:
				mix(player.Credits);
				mix(player.Income);
				mix(player.Army);
				mix(player.Destroyed);
				mix(player.Lost);
				mix(player.Harvesters);
				break;
			default:
				mix(player.Credits < 500);
				mix(player.Credits);
				break;
			}

			if (page == Page::Players || page == Page::Production)
			{
				for (int slot = 0; slot < static_cast<int>(Slot::Count); ++slot)
				{
					mix(player.HasRow[slot]);
					if (!player.HasRow[slot])
						continue;

					const auto& row = player.Rows[slot];
					mix(reinterpret_cast<int>(row.Type));
					mix(static_cast<int>(row.State));
					mix(row.Stage);
					mix(row.QueuedCount());
				}

				for (const auto& super : player.Supers)
				{
					mix(reinterpret_cast<int>(super.Type));
					mix(super.SecondsLeft);
					mix(super.Ready);
					mix(super.Paused);
				}
			}
		}

		if (hash != Signature || Dirty)
		{
			Signature = hash;
			Dirty = false;
			RequestRedraw();
		}
	}

	void OnPaid(const HouseClass* pHouse, int amount, uintptr_t returnAddress)
	{
		switch (returnAddress)
		{
		case 0x4CA04B: // FactoryClass::Abandon: production cancelled
		case 0x446E94: // BuildingClass::Grand_Opening: a free unit that could not be placed
		case 0x446EE2:
		case 0x686A78: // Read_Scenario_INI: the AI's starting bonus
			return;
		default:
			break;
		}

		const int index = pHouse->ArrayIndex;
		if (index < 0 || amount <= 0)
			return;

		if (index >= static_cast<int>(Earned.size()))
			Earned.resize(index + 1);

		Earned[index] += amount;
	}

	void OnKilled(TechnoClass* pVictim, ObjectClass* pSource)
	{
		if (!pSource || !pVictim->Owner)
			return;

		const auto pType = pVictim->GetTechnoType();
		if (!pType || pType->DontScore)
			return;

		if (pVictim->WhatAmI() == AbstractType::Building && pType->Insignificant)
			return;

		AddLost(pVictim->Owner, pType->GetCost());
	}
}

// StripClass::Draw_It for the observer: the page instead of the game's tiles. Both ways into the
// observer's drawing meet at 0x6AA05B (the top of the function jumps straight there), with the number
// of visible rows in EAX; the strip is at [esp+24h].
DEFINE_HOOK(0x6AA05B, StripClass_DrawIt_ObserverPages, 0xB)
{
	enum { Done = 0x6AA59B };

	if (!ObserverSidebar::IsActive())
		return 0;

	GET_STACK(StripClass*, pStrip, 0x24);
	GET(int, rows, EAX);

	ObserverSidebar::Draw(pStrip, rows);
	return Done;
}

// SidebarClass::AI, before it handles input: keep the pages up to date, and scroll the page with
// the scroll buttons rather than the list of players.
DEFINE_HOOK(0x6A7780, SidebarClass_AI_ObserverPages, 0x7)
{
	if (!ObserverSidebar::IsActive())
		return 0;

	GET_STACK(int*, pInput, 0x4);

	ObserverSidebar::Update();

	if (pInput)
	{
		const int key = *pInput & ~0x4000;
		if (key == (0x8000 | 200) || key == (0x8000 | 201))
		{
			auto& button = *reinterpret_cast<ShapeButtonClass*>(key == (0x8000 | 200) ? 0xB0B408 : 0xB0B328);
			button.IsPressed = false;
			ObserverSidebar::ScrollBy(key == (0x8000 | 200) ? -1 : 1);
			*pInput = 0;
		}
	}

	return 0;
}

// A click on the strip lands on one of its cameo buttons (SelectClass::Action); for the observer it
// goes to the page instead. The game finds no cameo there and does nothing more.
DEFINE_HOOK(0x6AAD00, SelectClass_Action_ObserverPages, 0x6)
{
	GET_STACK(int, flags, 0x4);

	if ((flags & static_cast<int>(GadgetFlag::LeftPress)) && ObserverSidebar::IsActive())
	{
		Point2D mouse {};
		WWMouseClass::Instance->GetCoords(&mouse);
		ObserverSidebar::Click(mouse.X - DSurface::SidebarBounds.X, mouse.Y);
	}

	return 0;
}

// The tab hotkeys only change to a tab with cameos; every page has something.
DEFINE_HOOK(0x6ABAC0, StripClass_HasCameos_ObserverPages, 0x5)
{
	enum { Return = 0x6ABACA };

	if (!ObserverSidebar::IsActive())
		return 0;

	R->EAX(1);
	return Return;
}

// The SidebarUp / SidebarDown hotkeys.
DEFINE_HOOK(0x5371B0, SidebarUpCommand_ObserverPages, 0x9)
{
	enum { Return = 0x5371BE };

	if (!ObserverSidebar::IsActive())
		return 0;

	ObserverSidebar::ScrollBy(-1);
	R->EAX(1);
	return Return;
}

DEFINE_HOOK(0x537240, SidebarDownCommand_ObserverPages, 0x9)
{
	enum { Return = 0x53724E };

	if (!ObserverSidebar::IsActive())
		return 0;

	ObserverSidebar::ScrollBy(1);
	R->EAX(1);
	return Return;
}

// HouseClass::Refund_Money: every payment to a house; the return address says what for.
DEFINE_HOOK(0x4F9950, HouseClass_RefundMoney_ObserverIncome, 0xA)
{
	GET(HouseClass*, pHouse, ECX);
	GET_STACK(uintptr_t, returnAddress, 0x0);
	GET_STACK(int, amount, 0x4);

	ObserverSidebar::OnPaid(pHouse, amount, returnAddress);
	return 0;
}

// TechnoClass::Record_The_Kill_Object, past its prologue: the object in ESI, what killed it at
// [esp+14h].
DEFINE_HOOK(0x702D46, TechnoClass_RecordTheKill_ObserverLost, 0x8)
{
	GET(TechnoClass*, pVictim, ESI);
	GET_STACK(ObjectClass*, pSource, 0x14);

	ObserverSidebar::OnKilled(pVictim, pSource);
	return 0;
}
