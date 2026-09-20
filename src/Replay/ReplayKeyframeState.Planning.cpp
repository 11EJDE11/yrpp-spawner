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

#include "ReplayKeyframeState.Internal.h"

#include <Utilities/Debug.h>

#include <FootClass.h>
#include <HouseClass.h>
#include <Memory.h>
#include <PlanningTokenClass.h>
#include <TechnoClass.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace ReplaySystem::KeyframeState::Detail
{
	// Savegames omit waypoint routes, while pending planning events can survive a load.
	// Restore the keyframe's route graph and pending packets together so units resume
	// their saved orders instead of losing routes or executing commands from the future.
	#pragma region Planning tokens

	std::array<DynamicVectorClass<PlanningNodeClass*>*, 3> PlanningManagerNodeLists()
	{
		return {
			&PlanningNodeClass::Unknown1,
			&PlanningNodeClass::Unknown2,
			&PlanningNodeClass::Unknown3
		};
	}

	bool CapturePlanningState(PlanningSnapshot& snapshot)
	{
		snapshot = {};

		const auto& pendingEvents = PlanningTokenClass::PendingEvents;
		snapshot.PendingEvents.reserve(static_cast<size_t>(std::max(pendingEvents.Count, 0)));
		for (int i = 0; i < pendingEvents.Count; ++i)
		{
			std::array<unsigned char, sizeof(EventClass)> packet {};
			if (pendingEvents.Items[i])
				memcpy(packet.data(), pendingEvents.Items[i], sizeof(EventClass));
			snapshot.PendingEvents.push_back(std::move(packet));
		}

		std::unordered_map<const PlanningNodeClass*, uint32_t> nodeIndices;
		std::vector<const PlanningNodeClass*> nodes;
		const auto indexNode = [&nodeIndices, &nodes](const PlanningNodeClass* pNode)
		{
			if (!pNode)
				return InvalidPlanningNode;

			const auto found = nodeIndices.find(pNode);
			if (found != nodeIndices.end())
				return found->second;

			const uint32_t index = static_cast<uint32_t>(nodes.size());
			nodes.push_back(pNode);
			nodeIndices.emplace(pNode, index);
			return index;
		};

		const auto managerLists = PlanningManagerNodeLists();
		for (size_t listIndex = 0; listIndex < managerLists.size(); ++listIndex)
		{
			const auto& list = *managerLists[listIndex];
			auto& savedList = snapshot.ManagerNodeLists[listIndex];
			savedList.reserve(static_cast<size_t>(std::max(list.Count, 0)));
			for (int i = 0; i < list.Count; ++i)
				savedList.push_back(indexNode(list.Items[i]));
		}

		snapshot.Tokens.reserve(static_cast<size_t>(std::max(PlanningTokenClass::Array.Count, 0)));
		for (int i = 0; i < PlanningTokenClass::Array.Count; ++i)
		{
			const auto* const pToken = PlanningTokenClass::Array.Items[i];
			if (!pToken)
				continue;

			PlanningTokenSnapshot token {};
			token.OwnerId = UniqueIDOf(pToken->OwnerUnit);
			token.Field1C = pToken->field_1C;
			memcpy(token.CurrentEvent.data(), &pToken->CurrentEvent, sizeof(EventClass));
			token.Field8C = pToken->field_8C;
			token.ClosedLoopNodeCount = pToken->ClosedLoopNodeCount;
			token.StepsToClosedLoop = pToken->StepsToClosedLoop;
			token.Field98 = pToken->field_98;
			token.Field99 = pToken->field_99;
			token.Nodes.reserve(static_cast<size_t>(std::max(pToken->PlanningNodes.Count, 0)));
			for (int n = 0; n < pToken->PlanningNodes.Count; ++n)
				token.Nodes.push_back(indexNode(pToken->PlanningNodes.Items[n]));

			snapshot.Tokens.push_back(std::move(token));
		}

		snapshot.Nodes.reserve(nodes.size());
		for (const auto* const pNode : nodes)
		{
			PlanningNodeSnapshot node {};
			node.Field18 = pNode->field_18;
			node.Field1C = pNode->field_1C;
			memcpy(node.Packet.data(), &pNode->Packet, sizeof(EventClass));
			node.FieldA8 = pNode->field_A8;
			node.FieldAC = pNode->field_AC;
			node.BranchNumber = pNode->BranchNumber;
			node.FieldB4 = pNode->field_B4;

			node.Members.reserve(static_cast<size_t>(std::max(pNode->PlanningMembers.Count, 0)));
			for (int m = 0; m < pNode->PlanningMembers.Count; ++m)
			{
				const auto* const pMember = pNode->PlanningMembers.Items[m];
				if (!pMember)
				{
					node.Members.push_back({});
					continue;
				}

				PlanningMemberSnapshot member {};
				member.Present = true;
				member.OwnerId = UniqueIDOf(pMember->Owner);
				if (pMember->Packet)
					memcpy(member.Packet.data(), pMember->Packet, sizeof(EventClass));
				member.Field8 = pMember->field_8;
				member.FieldC = pMember->field_C;
				node.Members.push_back(std::move(member));
			}

			node.Branches.reserve(static_cast<size_t>(std::max(pNode->PlanningBranches.Count, 0)));
			for (int b = 0; b < pNode->PlanningBranches.Count; ++b)
			{
				const auto* const pBranch = pNode->PlanningBranches.Items[b];
				if (!pBranch)
				{
					node.Branches.push_back({});
					continue;
				}

				PlanningBranchSnapshot branch {};
				branch.Present = true;
				memcpy(branch.Packet.data(), &pBranch->Packet, sizeof(EventClass));
				branch.MemberCount = pBranch->MemberCount;
				branch.MemberIndex = pBranch->MemberIndex;
				node.Branches.push_back(std::move(branch));
			}

			snapshot.Nodes.push_back(std::move(node));
		}

		const auto& activeOwners = PlanningTokenClass::ActiveRouteOwners;
		snapshot.ActiveRouteOwners.reserve(static_cast<size_t>(std::max(activeOwners.Count, 0)));
		for (int i = 0; i < activeOwners.Count; ++i)
			snapshot.ActiveRouteOwners.push_back(UniqueIDOf(activeOwners.Items[i]));

		memcpy(snapshot.HouseRouteCounts.data(), PlanningTokenClass::HouseRouteCounts,
			sizeof(snapshot.HouseRouteCounts));
		return true;
	}

	bool RestorePlanningState(const PlanningSnapshot& snapshot, int keyframeFrame)
	{
		std::unordered_map<uint32_t, TechnoClass*> technoById;
		technoById.reserve(static_cast<size_t>(std::max(TechnoClass::Array.Count, 0)));
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			if (auto* const pTechno = TechnoClass::Array.Items[i])
				technoById.emplace(UniqueIDOf(pTechno), pTechno);
		}

		const auto findTechno = [&technoById](uint32_t id) -> TechnoClass*
		{
			const auto found = technoById.find(id);
			return found == technoById.end() ? nullptr : found->second;
		};

		const auto validNodeIndex = [&snapshot](uint32_t index)
		{
			return index == InvalidPlanningNode || index < snapshot.Nodes.size();
		};

		for (const auto& token : snapshot.Tokens)
		{
			if (!token.OwnerId || !findTechno(token.OwnerId))
			{
				Debug::Log("[Replay] Keyframe %d planning route owner %u is missing after load.\n",
					keyframeFrame, token.OwnerId);
				return false;
			}
			if (!std::all_of(token.Nodes.begin(), token.Nodes.end(), validNodeIndex))
				return false;
		}

		for (const auto& node : snapshot.Nodes)
		{
			for (const auto& member : node.Members)
			{
				if (!member.Present)
					continue;

				if (!member.OwnerId || !findTechno(member.OwnerId))
				{
					Debug::Log("[Replay] Keyframe %d planning member owner %u is missing after "
						"load.\n", keyframeFrame, member.OwnerId);
					return false;
				}
			}
		}

		for (const auto& list : snapshot.ManagerNodeLists)
		{
			if (!std::all_of(list.begin(), list.end(), validNodeIndex))
				return false;
		}
		for (uint32_t ownerId : snapshot.ActiveRouteOwners)
		{
			if (!ownerId || !findTechno(ownerId))
				return false;
		}

		auto& pendingEvents = PlanningTokenClass::PendingEvents;
		for (int i = 0; i < pendingEvents.Count; ++i)
		{
			if (pendingEvents.Items[i])
				YRMemory::Deallocate(pendingEvents.Items[i]);
		}
		pendingEvents.Count = 0;

		PlanningTokenClass::ClearAll();
		for (auto& pair : technoById)
			pair.second->PlanningToken = nullptr;


		std::vector<PlanningNodeClass*> nodes;
		nodes.reserve(snapshot.Nodes.size());
		size_t memberCount = 0;

		for (const auto& savedNode : snapshot.Nodes)
		{
			auto* const memory = static_cast<PlanningNodeClass*>(
				YRMemory::AllocateChecked(sizeof(PlanningNodeClass)));
			auto* const pNode = PlanningNodeClass::Construct(memory, savedNode.Field18);
			pNode->field_1C = savedNode.Field1C;
			memcpy(&pNode->Packet, savedNode.Packet.data(), sizeof(EventClass));
			pNode->field_A8 = savedNode.FieldA8;
			pNode->field_AC = savedNode.FieldAC;
			pNode->BranchNumber = savedNode.BranchNumber;
			pNode->field_B4 = savedNode.FieldB4;

			for (const auto& savedBranch : savedNode.Branches)
			{
				if (!savedBranch.Present)
				{
					if (!pNode->PlanningBranches.AddItem(nullptr))
						return false;
					continue;
				}

				auto* const pBranch = static_cast<PlanningBranchClass*>(
					YRMemory::AllocateChecked(sizeof(PlanningBranchClass)));
				memcpy(&pBranch->Packet, savedBranch.Packet.data(), sizeof(EventClass));
				pBranch->MemberCount = savedBranch.MemberCount;
				pBranch->MemberIndex = savedBranch.MemberIndex;
				if (!pNode->PlanningBranches.AddItem(pBranch))
					return false;
			}

			for (const auto& savedMember : savedNode.Members)
			{
				if (!savedMember.Present)
				{
					if (!pNode->PlanningMembers.AddItem(nullptr))
						return false;
					continue;
				}

				auto* const pMember = static_cast<PlanningMemberClass*>(
					YRMemory::AllocateChecked(sizeof(PlanningMemberClass)));
				pMember->Owner = findTechno(savedMember.OwnerId);
				pMember->Packet = static_cast<EventClass*>(
					YRMemory::AllocateChecked(sizeof(EventClass)));
				memcpy(pMember->Packet, savedMember.Packet.data(), sizeof(EventClass));
				pMember->field_8 = savedMember.Field8;
				pMember->field_C = savedMember.FieldC;
				if (!pNode->PlanningMembers.AddItem(pMember))
					return false;
				++memberCount;
			}

			nodes.push_back(pNode);
		}

		const auto nodeAt = [&nodes](uint32_t index) -> PlanningNodeClass*
		{
			return index == InvalidPlanningNode ? nullptr : nodes[index];
		};

		for (const auto& savedToken : snapshot.Tokens)
		{
			auto* const pOwner = findTechno(savedToken.OwnerId);
			auto* const memory = static_cast<PlanningTokenClass*>(
				YRMemory::AllocateChecked(sizeof(PlanningTokenClass)));
			auto* const pToken = PlanningTokenClass::Construct(memory, pOwner);

			pToken->field_1C = savedToken.Field1C;
			memcpy(&pToken->CurrentEvent, savedToken.CurrentEvent.data(), sizeof(EventClass));
			pToken->field_8C = savedToken.Field8C;
			pToken->ClosedLoopNodeCount = savedToken.ClosedLoopNodeCount;
			pToken->StepsToClosedLoop = savedToken.StepsToClosedLoop;
			pToken->field_98 = savedToken.Field98;
			pToken->field_99 = savedToken.Field99;
			for (uint32_t nodeIndex : savedToken.Nodes)
			{
				if (!pToken->PlanningNodes.AddItem(nodeAt(nodeIndex)))
					return false;
			}

			if (!PlanningTokenClass::Array.AddItem(pToken))
				return false;
			pOwner->PlanningToken = pToken;
		}

		const auto managerLists = PlanningManagerNodeLists();
		for (size_t listIndex = 0; listIndex < managerLists.size(); ++listIndex)
		{
			for (uint32_t nodeIndex : snapshot.ManagerNodeLists[listIndex])
			{
				if (!managerLists[listIndex]->AddItem(nodeAt(nodeIndex)))
					return false;
			}
		}

		auto& activeOwners = PlanningTokenClass::ActiveRouteOwners;
		for (uint32_t ownerId : snapshot.ActiveRouteOwners)
		{
			if (!activeOwners.AddItem(findTechno(ownerId)))
				return false;
		}
		memcpy(PlanningTokenClass::HouseRouteCounts, snapshot.HouseRouteCounts.data(),
			sizeof(snapshot.HouseRouteCounts));

		for (const auto& savedPacket : snapshot.PendingEvents)
		{
			auto* const pPacket = static_cast<EventClass*>(
				YRMemory::AllocateChecked(sizeof(EventClass)));
			memcpy(pPacket, savedPacket.data(), sizeof(EventClass));
			if (!pendingEvents.AddItem(pPacket))
			{
				YRMemory::Deallocate(pPacket);
				return false;
			}
		}

		PlanningSnapshot rebuilt {};
		if (!CapturePlanningState(rebuilt) || rebuilt != snapshot)
		{
			Debug::Log("[Replay] Keyframe %d planning graph did not reproduce exactly after "
				"rebuilding it.\n", keyframeFrame);
			return false;
		}

		if (!snapshot.Tokens.empty())
		{
			Debug::Log("[Replay] Keyframe %d rebuilt %d planning routes (%d shared nodes, %d "
				"members, %d pending events).\n", keyframeFrame,
				static_cast<int>(snapshot.Tokens.size()),
				static_cast<int>(snapshot.Nodes.size()), static_cast<int>(memberCount),
				static_cast<int>(snapshot.PendingEvents.size()));
		}
		return true;
	}

	#pragma endregion Planning tokens
}
