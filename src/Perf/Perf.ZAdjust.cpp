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

// FootClass::Z_Adjust (0x4DAFC0) is how far a vehicle's, infantryman's or aircraft's image is moved in
// depth for the bridge, column, tunnel and cliff it stands by. It looks up the unit's cell and a dozen
// of its neighbours, some of them several times over, and drawing one unit asks for it three or four
// times: Draw_It, Draw_Object, and the voxel body and shadow. A screen of tanks spent a tenth of its
// frame here.
//
// This is the original's top level, calling the game's own checks. While the object layers are drawn
// (Tactical::Render_Layers) the answer is kept per object and given back for as long as the object
// has not moved, turned or got on or off a bridge. Everything else it reads - the map's cells, the
// locomotor, the unit's mission - is the simulation's, which does not run while the layers are drawn.
// Outside that, every call computes the answer afresh, as the original does.

#include <FootClass.h>
#include <TechnoTypeClass.h>

#include <Utilities/Macro.h>

#include <cstring>

namespace
{
	struct CachedZAdjust
	{
		const FootClass* Object;
		unsigned int Pass;
		CoordStruct Location;
		BYTE Facing[sizeof(FacingClass)];
		bool OnBridge;
		int Value;
	};

	// Open addressing, an entry belonging to the pass that wrote it, so nothing is cleared between
	// passes. A full table just stops caching for the rest of the pass.
	constexpr size_t CacheSize = 4096;
	constexpr size_t MaxProbes = 16;
	CachedZAdjust Cache[CacheSize];
	unsigned int Pass = 0;
	bool DrawingLayers = false;

	CachedZAdjust* FindSlot(const FootClass* pObject)
	{
		size_t index = (reinterpret_cast<uintptr_t>(pObject) >> 4) * 2654435761u % CacheSize;
		for (size_t probe = 0; probe < MaxProbes; ++probe, index = (index + 1) % CacheSize)
		{
			auto& slot = Cache[index];
			if (slot.Pass != Pass || slot.Object == pObject)
				return &slot;
		}

		return nullptr;
	}

	auto const IsZFudgeColumn = reinterpret_cast<int(__thiscall*)(TechnoClass*)>(0x703E70);
	auto const IsZFudgeTunnel = reinterpret_cast<int(__thiscall*)(TechnoClass*)>(0x704000);
	auto const IsZFudgeCliff = reinterpret_cast<int(__thiscall*)(TechnoClass*)>(0x704240);
	auto const IsZFudgeBridge = reinterpret_cast<bool(__thiscall*)(TechnoClass*)>(0x703B10);
	auto const GetZAdjustment = reinterpret_cast<int(__thiscall*)(TechnoClass*)>(0x704350);

	int ComputeZAdjust(FootClass* pThis)
	{
		int adjust = 0;
		if (auto const pLocomotor = pThis->Locomotor.GetInterfacePtr())
			adjust = pLocomotor->Z_Adjust();

		int fudge = IsZFudgeColumn(pThis) * pThis->GetTechnoType()->ZFudgeColumn;
		const int tunnel = IsZFudgeTunnel(pThis) * pThis->GetTechnoType()->ZFudgeTunnel;
		const int cliff = IsZFudgeCliff(pThis) * pThis->GetTechnoType()->ZFudgeCliff;
		const int bridge = IsZFudgeBridge(pThis) ? pThis->GetTechnoType()->ZFudgeBridge : 0;

		if (fudge <= tunnel)
			fudge = tunnel;
		if (fudge <= cliff)
			fudge = cliff;
		if (fudge <= bridge)
			fudge = bridge;

		return adjust + fudge + GetZAdjustment(pThis);
	}

	int __fastcall ZAdjust(FootClass* pThis, void*)
	{
		auto const pCached = DrawingLayers ? FindSlot(pThis) : nullptr;
		if (!pCached)
			return ComputeZAdjust(pThis);

		auto& cached = *pCached;
		if (cached.Pass == Pass && cached.Location == pThis->Location && cached.OnBridge == pThis->OnBridge
			&& !memcmp(cached.Facing, &pThis->PrimaryFacing, sizeof(cached.Facing)))
		{
			return cached.Value;
		}

		cached.Object = pThis;
		cached.Pass = Pass;
		cached.Location = pThis->Location;
		cached.OnBridge = pThis->OnBridge;
		memcpy(cached.Facing, &pThis->PrimaryFacing, sizeof(cached.Facing));
		cached.Value = ComputeZAdjust(pThis);
		return cached.Value;
	}
}

DEFINE_FUNCTION_JUMP(LJMP, 0x4DAFC0, ZAdjust);

DEFINE_HOOK(0x6D8DB0, Tactical_RenderLayers_ZAdjustCacheBegin, 0x6)
{
	// Pass 0 is what the table starts out holding, so it is never a live pass.
	if (++Pass == 0)
		Pass = 1;

	DrawingLayers = true;
	return 0;
}

DEFINE_HOOK(0x6D97BF, Tactical_RenderLayers_ZAdjustCacheEnd, 0x6)
{
	DrawingLayers = false;
	return 0;
}
