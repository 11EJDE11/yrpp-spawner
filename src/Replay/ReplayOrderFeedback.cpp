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

#include "ReplayOrderFeedback.h"
#include "ReplaySeek.h"
#include "ReplaySystem.h"

#include <BuildingTypeClass.h>
#include <EventClass.h>
#include <FactoryClass.h>
#include <HouseClass.h>
#include <ObjectTypeClass.h>
#include <Randomizer.h>
#include <TacticalClass.h>
#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <Unsorted.h>
#include <VoxClass.h>

namespace ReplaySystem
{
	namespace OrderFeedback
	{
		namespace
		{
			// What this frame's injected events have asked for. Stamped with the frame, so a frame
			// that stopped injecting part way leaves nothing behind for the next.
			struct FrameFeedback
			{
				int Frame = -1;
				bool LinesDue = false;
				bool Acknowledged = false;
				const char* EvaLine = nullptr;
			};

			FrameFeedback Pending;

			FrameFeedback& ForThisFrame()
			{
				const int frame = Unsorted::CurrentFrame;
				if (Pending.Frame != frame)
				{
					Pending = FrameFeedback {};
					Pending.Frame = frame;
				}

				return Pending;
			}

			// TechnoClass::Player_Assign_Mission (0x6FFBE0), the switch it plays the clicked unit's
			// acknowledgement from. The recorded mission is the one it sent, after its own Move and
			// Attack to AttackMove conversion, so the same switch picks the same response.
			void Acknowledge(const EventClass& event)
			{
				// The TargetClass conversions are not const, so work on a copy.
				auto order = event.MegaMission;
				auto* const pTechno = order.Whom.As_Techno();
				if (!pTechno || !pTechno->IsAlive)
					return;

				switch (static_cast<Mission>(order.Mission))
				{
				case Mission::Harvest:
					pTechno->VoiceHarvest();
					break;

				case Mission::Attack:
					// The engine passes the target as it is, a cell included for an attack on the ground.
					pTechno->VoiceAttack(reinterpret_cast<ObjectClass*>(order.Target.As_Abstract()));
					break;

				case Mission::Move:
				case Mission::AttackMove:
					pTechno->VoiceMove();
					break;

				case Mission::Enter:
					pTechno->VoiceEnter();
					break;

				case Mission::Capture:
					pTechno->VoiceCapture();
					break;

				case Mission::Unload:
					pTechno->VoiceDeploy();
					break;

				default:
				{
					const auto& voices = pTechno->GetTechnoType()->VoiceSpecialAttack;
					if (voices.Count > 0)
					{
						// NonCriticalRandomNumber, as the engine uses: not part of the simulation.
						const auto pick = static_cast<unsigned int>(Randomizer::Global.Random());
						pTechno->QueueVoice(voices[static_cast<int>(pick % static_cast<unsigned int>(voices.Count))]);
					}
					break;
				}
				}
			}

			struct ProductionOrder
			{
				TechnoTypeClass* Type = nullptr;
				FactoryClass* Factory = nullptr;
			};

			// The type the order names and the house factory that builds it, as
			// SidebarClass::StripClass::SelectClass::Action (0x6AAD00) looks them up.
			ProductionOrder ResolveProductionOrder(const HouseClass* pHouse, AbstractType rtti, int heapID, bool naval)
			{
				ProductionOrder order {};
				order.Type = ObjectTypeClass::GetTechnoType(rtti, heapID);
				if (!order.Type)
					return order;

				BuildCat buildCat = BuildCat::DontCare;
				if (rtti == AbstractType::BuildingType || rtti == AbstractType::Building)
					buildCat = static_cast<BuildingTypeClass*>(order.Type)->BuildCat;

				order.Factory = pHouse->GetPrimaryFactory(rtti, naval, buildCat);
				return order;
			}

			bool IsStopped(const FactoryClass* pFactory)
			{
				return !pFactory->Production.Rate || pFactory->IsSuspended;
			}

			bool IsBuildingThis(const FactoryClass* pFactory, const TechnoTypeClass* pType)
			{
				return pFactory->Object && pFactory->Object->GetTechnoType() == pType;
			}

			// The click speaks Training or Building when it starts the factory, or resumes the item it
			// had paused; adding to a queue that is already running says nothing.
			const char* LineForProduce(const HouseClass* pHouse, const EventClass& event)
			{
				const auto order = ResolveProductionOrder(pHouse, event.Produce.RTTIType,
					event.Produce.HeapID, event.Produce.IsNaval != 0);
				if (!order.Type)
					return nullptr;

				const auto* const pFactory = order.Factory;
				const bool resumes = pFactory && IsBuildingThis(pFactory, order.Type) && IsStopped(pFactory);
				const bool starts = !pFactory
					|| (IsStopped(pFactory) && !pFactory->Object && pFactory->QueuedObjects.Count == 0);

				if (!resumes && !starts)
					return nullptr;

				return event.Produce.RTTIType == AbstractType::InfantryType ? "EVA_Training" : "EVA_Building";
			}

			// Cancelled is said for the item the factory has stopped on - paused, or finished and
			// waiting to be placed. Taking one off the queue only clicks.
			// ABANDON_ALL is declared as 12 unknown bytes, but the click builds it with the same
			// constructor as ABANDON (EventClass_PRODUCTIONRELATED, 0x4C6970), so it reads the same.
			const char* LineForAbandon(const HouseClass* pHouse, const EventClass& event)
			{
				const auto order = ResolveProductionOrder(pHouse, event.Abandon.RTTIType,
					event.Abandon.HeapID, event.Abandon.IsNaval != 0);
				if (!order.Type || !order.Factory)
					return nullptr;

				return IsBuildingThis(order.Factory, order.Type) && IsStopped(order.Factory)
					? "EVA_Canceled"
					: nullptr;
			}
		}

		void OnEventInjected(const EventClass& event)
		{
			const HouseClass* const pWatched = HouseClass::CurrentPlayer;
			if (!pWatched || event.HouseIndex != pWatched->ArrayIndex || Seek::IsSeeking())
				return;

			auto& feedback = ForThisFrame();

			// The spectator seat watches the recording player's house as an observer, so it shows
			// their lines but does not speak for them.
			const bool speaks = !ReplaySystem::IsSpectatorPlayback();

			switch (event.Type)
			{
			case EventType::MegaMission:
				feedback.LinesDue = true;

				// Planned orders, added or carried out, were never acknowledged: Player_Assign_Mission
				// stores them before it reaches the voices. One unit answers for the click, as
				// AllowVoice (0x822CF2) arranges live.
				if (speaks && !feedback.Acknowledged
					&& event.MegaMission.PlanningState == PlanningOrderState::None)
				{
					feedback.Acknowledged = true;
					Acknowledge(event);
				}
				break;

			case EventType::MegaMissionF:
				feedback.LinesDue = true;
				break;

			case EventType::Produce:
				if (speaks && !feedback.EvaLine)
					feedback.EvaLine = LineForProduce(pWatched, event);
				break;

			case EventType::Suspend:
				// The click puts a running item on hold; every suspend it sends says so.
				if (speaks && !feedback.EvaLine)
					feedback.EvaLine = "EVA_OnHold";
				break;

			case EventType::Abandon:
			case EventType::AbandonAll:
				if (speaks && !feedback.EvaLine)
					feedback.EvaLine = LineForAbandon(pWatched, event);
				break;

			default:
				break;
			}
		}

		void OnFrameInjected()
		{
			auto& feedback = ForThisFrame();

			if (!Seek::IsSeeking())
			{
				// The click restarted the action line timer (Mouse_Left_Release, 0x4ABFAE). The order
				// restarts it as it runs instead, when the units have their new destination.
				if (feedback.LinesDue)
					TacticalClass::StartDrawActionLineTimer();

				if (feedback.EvaLine)
					VoxClass::Play(feedback.EvaLine);
			}

			Pending = FrameFeedback {};
		}
	}
}
