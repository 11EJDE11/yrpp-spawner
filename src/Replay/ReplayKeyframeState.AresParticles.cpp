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
#include <ParticleSystemClass.h>
#include <ParticleTypeClass.h>

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace ReplaySystem::KeyframeState::Detail
{
	// Carry Ares particle-extension state that the engine savegame does not preserve.
	// Rebuild movement/draw records and resolve their object/type references after load;
	// the signature checks below gate access to Ares' private layout and allocator.
	#pragma region Ares particle-system state

	struct AresVectorView
	{
		unsigned char* Begin;
		unsigned char* End;
		unsigned char* Capacity;
	};

	struct AresParticleExtView
	{
		ParticleSystemClass* Owner;
		int Initialized;
		int Behave;
		ParticleTypeClass* HeldParticleType;
		AresVectorView MovementData;
		AresVectorView DrawData;
	};

	static_assert(sizeof(AresVectorView) == 0x0C);
	static_assert(offsetof(AresParticleExtView, Behave) == 0x08);
	static_assert(offsetof(AresParticleExtView, HeldParticleType) == 0x0C);
	static_assert(offsetof(AresParticleExtView, MovementData) == 0x10);
	static_assert(offsetof(AresParticleExtView, DrawData) == 0x1C);

	constexpr size_t AresDrawLinkedParticleTypeOffset = 0x24;
	constexpr size_t MaximumAresParticleRecords = 1u << 20;

	struct AresParticleApi
	{
		using FindExtension = AresParticleExtView* (__thiscall*)(void*, ParticleSystemClass*);
		using AllocateRecords = unsigned char* (__stdcall*)(unsigned int);
		using AdoptRecords = void (__thiscall*)(AresVectorView*, unsigned char*,
			unsigned int, unsigned int);

		unsigned char* Module = nullptr;
		void* ExtensionMap = nullptr;
		FindExtension Find = nullptr;
		AllocateRecords Allocate = nullptr;
		AdoptRecords Adopt = nullptr;
		bool Compatible = false;
	};

	bool BytesMatch(const unsigned char* address, const unsigned char* expected, size_t count)
	{
		return address && memcmp(address, expected, count) == 0;
	}

	const AresParticleApi& GetAresParticleApi()
	{
		static const AresParticleApi api = []()
		{
			AresParticleApi result {};
			result.Module = reinterpret_cast<unsigned char*>(GetModuleHandleA("Ares.dll"));
			if (!result.Module)
			{
				Debug::Log("[Replay] Ares is not loaded; its particle state will not travel with a "
					"keyframe.\n");
				return result;
			}

			const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(result.Module);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE)
				return result;
			const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
				result.Module + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE
				|| nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386
				|| nt->OptionalHeader.SizeOfImage <= 0xC2B84)
				return result;

			// ExtContainer::Find, vector<44-byte-record>::allocate and its
			// _Change_array/adopt helper in the exact reversed release build.
			const unsigned char findSignature[] = {
				0x83, 0xEC, 0x08, 0x53, 0x8B, 0x5C, 0x24, 0x10,
				0x0F, 0xB6, 0xC3, 0x35, 0xC5, 0x9D, 0x1C, 0x81
			};
			const unsigned char allocateSignature[] = {
				0x8B, 0x44, 0x24, 0x04, 0x3D, 0x5D, 0x74, 0xD1,
				0x05, 0x77, 0x41, 0x6B, 0xC0, 0x2C
			};
			const unsigned char adoptSignature[] = {
				0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x37, 0x85, 0xF6
			};

			auto* const find = result.Module + 0x58900;
			auto* const allocate = result.Module + 0x29730;
			auto* const adopt = result.Module + 0x296B0;
			const bool findOk = BytesMatch(find, findSignature, sizeof(findSignature));
			const bool allocateOk = BytesMatch(allocate, allocateSignature, sizeof(allocateSignature));
			const bool adoptOk = BytesMatch(adopt, adoptSignature, sizeof(adoptSignature));
			if (!findOk || !allocateOk || !adoptOk)
			{
				Debug::Log("[Replay] This Ares build does not match the one the particle state was "
					"reversed from (Find %s, allocate %s, adopt %s); its particle state will not "
					"travel with a keyframe.\n", findOk ? "matches" : "does not match",
					allocateOk ? "matches" : "does not match", adoptOk ? "matches" : "does not match");
				return result;
			}

			result.ExtensionMap = result.Module + 0xC2B84;
			result.Find = reinterpret_cast<AresParticleApi::FindExtension>(find);
			result.Allocate = reinterpret_cast<AresParticleApi::AllocateRecords>(allocate);
			result.Adopt = reinterpret_cast<AresParticleApi::AdoptRecords>(adopt);
			result.Compatible = true;
			return result;
		}();
		return api;
	}

	bool AresVectorCount(const AresVectorView& vector, size_t& count, size_t& capacity)
	{
		count = 0;
		capacity = 0;
		if (!vector.Begin && !vector.End && !vector.Capacity)
			return true;
		if (!vector.Begin || !vector.End || !vector.Capacity)
			return false;

		const uintptr_t begin = reinterpret_cast<uintptr_t>(vector.Begin);
		const uintptr_t end = reinterpret_cast<uintptr_t>(vector.End);
		const uintptr_t cap = reinterpret_cast<uintptr_t>(vector.Capacity);
		if (end < begin || cap < end
			|| (end - begin) % AresParticleRecordSize
			|| (cap - begin) % AresParticleRecordSize)
			return false;

		count = (end - begin) / AresParticleRecordSize;
		capacity = (cap - begin) / AresParticleRecordSize;
		return count <= MaximumAresParticleRecords
			&& capacity <= MaximumAresParticleRecords;
	}

	int ParticleTypeIndex(const ParticleTypeClass* pType)
	{
		if (!pType)
			return -1;
		for (int i = 0; i < ParticleTypeClass::Array.Count; ++i)
		{
			if (ParticleTypeClass::Array.Items[i] == pType)
				return i;
		}
		return -2;
	}

	ParticleTypeClass* ParticleTypeAt(int index)
	{
		if (index < 0)
			return nullptr;
		return index < ParticleTypeClass::Array.Count
			? ParticleTypeClass::Array.Items[index]
			: nullptr;
	}

	bool CopyAresVector(const AresVectorView& source,
		std::vector<std::array<unsigned char, AresParticleRecordSize>>& destination)
	{
		size_t count = 0;
		size_t capacity = 0;
		if (!AresVectorCount(source, count, capacity))
			return false;

		destination.resize(count);
		if (count)
			memcpy(destination.data(), source.Begin, count * AresParticleRecordSize);
		return true;
	}

	bool RestoreAresVector(const AresParticleApi& api, AresVectorView& destination,
		const unsigned char* source, size_t count)
	{
		if (count > MaximumAresParticleRecords)
			return false;

		size_t oldCount = 0;
		size_t capacity = 0;
		if (!AresVectorCount(destination, oldCount, capacity))
			return false;

		if (count <= capacity)
		{
			if (count)
				memcpy(destination.Begin, source, count * AresParticleRecordSize);
			destination.End = count
				? destination.Begin + count * AresParticleRecordSize
				: destination.Begin;
			return true;
		}

		auto* const memory = api.Allocate(static_cast<unsigned int>(count));
		if (!memory)
			return count == 0;
		memcpy(memory, source, count * AresParticleRecordSize);
		api.Adopt(&destination, memory, static_cast<unsigned int>(count),
			static_cast<unsigned int>(count));
		return true;
	}

	bool CaptureAresParticleState(AresParticleSnapshot& snapshot)
	{
		snapshot = {};
		const auto& api = GetAresParticleApi();
		if (!api.Compatible)
			return true;

		snapshot.Captured = true;
		snapshot.Systems.reserve(static_cast<size_t>(
			std::max(ParticleSystemClass::Array.Count, 0)));

		for (int i = 0; i < ParticleSystemClass::Array.Count; ++i)
		{
			auto* const pSystem = ParticleSystemClass::Array.Items[i];
			if (!pSystem)
				continue;
			auto* const pExt = api.Find(api.ExtensionMap, pSystem);
			if (!pExt || pExt->Owner != pSystem)
				return false;

			AresParticleSystemSnapshot saved {};
			saved.OwnerId = UniqueIDOf(pSystem);
			saved.Behave = pExt->Behave;
			saved.HeldParticleTypeIndex = ParticleTypeIndex(pExt->HeldParticleType);
			if (saved.HeldParticleTypeIndex == -2
				|| !CopyAresVector(pExt->MovementData, saved.MovementData))
				return false;

			std::vector<std::array<unsigned char, AresParticleRecordSize>> draw;
			if (!CopyAresVector(pExt->DrawData, draw))
				return false;
			saved.DrawData.reserve(draw.size());
			for (auto& bytes : draw)
			{
				AresParticleRecordSnapshot item {};
				item.Bytes = bytes;
				ParticleTypeClass* pType = nullptr;
				memcpy(&pType, item.Bytes.data() + AresDrawLinkedParticleTypeOffset,
					sizeof(pType));
				item.LinkedParticleTypeIndex = ParticleTypeIndex(pType);
				if (item.LinkedParticleTypeIndex == -2)
					return false;
				memset(item.Bytes.data() + AresDrawLinkedParticleTypeOffset, 0,
					sizeof(pType));
				saved.DrawData.push_back(std::move(item));
			}
			snapshot.Systems.push_back(std::move(saved));
		}
		return true;
	}

	bool RestoreAresParticleState(const AresParticleSnapshot& snapshot, int keyframeFrame)
	{
	// This used to fail silently in a dozen places, so a run where it restored nothing looked
	// exactly like a run where it had nothing to do. Every way out now says which it was.
	auto Give_Up = [keyframeFrame](const char* why)
	{
		Debug::Log("[Replay] Keyframe %d could not restore the Ares particle state: %s.\n",
			keyframeFrame, why);
		return false;
	};

		if (!snapshot.Captured)
		{
			Debug::Log("[Replay] Keyframe %d holds no Ares particle state to restore.\n",
				keyframeFrame);
			return true;
		}
		const auto& api = GetAresParticleApi();
		if (!api.Compatible)
			return Give_Up("the Ares build does not match the one this was reversed from");

		std::unordered_map<uint32_t, ParticleSystemClass*> systems;
		systems.reserve(static_cast<size_t>(std::max(ParticleSystemClass::Array.Count, 0)));
		for (int i = 0; i < ParticleSystemClass::Array.Count; ++i)
		{
			auto* const pSystem = ParticleSystemClass::Array.Items[i];
			if (pSystem)
				systems.emplace(UniqueIDOf(pSystem), pSystem);
		}
		if (systems.size() != snapshot.Systems.size())
			{
				Debug::Log("[Replay] Keyframe %d holds %u particle systems but the load left %u.\n",
					keyframeFrame, static_cast<unsigned int>(snapshot.Systems.size()),
					static_cast<unsigned int>(systems.size()));
				return Give_Up("the number of particle systems changed");
			}

		int changedSystems = 0;
		size_t movementRecords = 0;
		size_t drawRecords = 0;
		for (const auto& saved : snapshot.Systems)
		{
			const auto found = systems.find(saved.OwnerId);
			if (found == systems.end())
				return Give_Up("a particle system is missing after the load");
			auto* const pExt = api.Find(api.ExtensionMap, found->second);
			if (!pExt || pExt->Owner != found->second)
				return Give_Up("a particle system has no Ares extension, or one belonging to something else");

			size_t liveMovement = 0;
			size_t movementCapacity = 0;
			size_t liveDraw = 0;
			size_t drawCapacity = 0;
			if (!AresVectorCount(pExt->MovementData, liveMovement, movementCapacity)
				|| !AresVectorCount(pExt->DrawData, liveDraw, drawCapacity))
				return Give_Up("an Ares particle vector does not look like a vector");

			const bool changed = pExt->Behave != saved.Behave
				|| ParticleTypeIndex(pExt->HeldParticleType) != saved.HeldParticleTypeIndex
				|| liveMovement != saved.MovementData.size()
				|| liveDraw != saved.DrawData.size();

			pExt->Behave = saved.Behave;
			pExt->HeldParticleType = ParticleTypeAt(saved.HeldParticleTypeIndex);
			if (saved.HeldParticleTypeIndex >= 0 && !pExt->HeldParticleType)
				return Give_Up("the held particle type is no longer in the array");

			if (!RestoreAresVector(api, pExt->MovementData,
				saved.MovementData.empty() ? nullptr : saved.MovementData.front().data(),
				saved.MovementData.size()))
				return Give_Up("the movement record vector could not be rebuilt");

			std::vector<std::array<unsigned char, AresParticleRecordSize>> draw;
			draw.reserve(saved.DrawData.size());
			for (const auto& item : saved.DrawData)
			{
				auto bytes = item.Bytes;
				auto* const pType = ParticleTypeAt(item.LinkedParticleTypeIndex);
				if (item.LinkedParticleTypeIndex >= 0 && !pType)
					return Give_Up("a draw record names a particle type no longer in the array");
				memcpy(bytes.data() + AresDrawLinkedParticleTypeOffset, &pType,
					sizeof(pType));
				draw.push_back(bytes);
			}
			if (!RestoreAresVector(api, pExt->DrawData,
				draw.empty() ? nullptr : draw.front().data(), draw.size()))
				return Give_Up("the draw record vector could not be rebuilt");

			changedSystems += changed ? 1 : 0;
			movementRecords += saved.MovementData.size();
			drawRecords += saved.DrawData.size();
		}

		AresParticleSnapshot rebuilt {};
		if (!CaptureAresParticleState(rebuilt) || rebuilt != snapshot)
		{
			Debug::Log("[Replay] Keyframe %d Ares particle state did not reproduce exactly "
				"after restoring it.\n", keyframeFrame);
			return Give_Up("the state read back differently from what was written");
		}

		if (changedSystems)
		{
			Debug::Log("[Replay] Keyframe %d restored Ares particle state for %d systems "
				"(%u movement records, %u draw records; %d systems differed after load).\n",
				keyframeFrame, static_cast<int>(snapshot.Systems.size()),
				static_cast<unsigned int>(movementRecords),
				static_cast<unsigned int>(drawRecords), changedSystems);
		}
		return true;
	}

	#pragma endregion Ares particle-system state
}
