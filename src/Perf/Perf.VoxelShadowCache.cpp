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

// UnitClass::Draw_Voxel drops a unit's shadow cache key, so its shadow is rendered from the voxel
// every frame, for a BalloonHover, tracked or wheeled unit above the ground. Phobos' hook at 0x73C43F
// (UnitClass_DrawAsVXL_Shadow_IsLocomotorFix2) adds units lifted by a magnetron, but sets AL where
// 0x73C445 tests CL, the low byte of a pointer, so every unit above the ground drops its key -
// Siege Choppers too, which are none of these. A hundred of them took half the frame.
//
// This applies the rule Phobos meant at 0x73C459, which every path that drops the key goes through.
// It is clear of the bytes Phobos' hook returns to, so it stays correct once Phobos is fixed.
// Phobos #2484 fixes the hook; remove this with Phobos 0.5.

#include <UnitClass.h>
#include <UnitTypeClass.h>

#include <Utilities/Macro.h>

#include <unordered_map>

namespace
{
	// A BalloonHover unit above the ground - a Kirov - has its shadow masked by its own body, drawn
	// where the locomotor's shadow point puts it. Its key is the body's, which says nothing about that
	// point, so the game drops it and renders the shadow every frame. Each body key and shadow point
	// pair gets a key of its own instead, so the same shadow over the same body is drawn from the cache.
	// Bit 30 keeps these clear of every key the game makes (bits 0-21, and bit 31 on the ground).
	std::unordered_map<unsigned long long, int> BalloonShadowKeys;
	constexpr int BalloonKeyBase = 0x40000000;
	constexpr int MaxBalloonKeys = 0x00FFFFFF;

	bool KeyBalloonShadow(UnitClass* pThis, VoxelIndexKey& key)
	{
		if (!key.Is_Valid_Key() || pThis->GetHeight() <= 0)
			return false;

		auto const pLocomotor = pThis->Locomotor.GetInterfacePtr();
		if (!pLocomotor)
			return false;

		const Point2D point = pLocomotor->Shadow_Point();
		const unsigned long long pair = static_cast<unsigned int>(key.Value)
			| static_cast<unsigned long long>(static_cast<unsigned short>(point.X)) << 32
			| static_cast<unsigned long long>(static_cast<unsigned short>(point.Y)) << 48;

		auto const [it, added] = BalloonShadowKeys.try_emplace(pair, static_cast<int>(BalloonShadowKeys.size()));
		if (it->second > MaxBalloonKeys)
			return false;

		key = BalloonKeyBase | it->second;
		return true;
	}
}

DEFINE_HOOK(0x73C459, UnitClass_DrawAsVXL_ShadowCacheKey, 0x5)
{
	enum { KeepKey = 0x73C47A };

	GET(UnitClass* const, pThis, EBP);
	REF_STACK(VoxelIndexKey, key, 0x14);

	auto const pType = pThis->Type;
	if (pType->BalloonHover && !pThis->IsAttackedByLocomotor)
		return KeyBalloonShadow(pThis, key) ? KeepKey : 0;

	const bool mayDrop = pThis->IsAttackedByLocomotor
		|| pType->SpeedType == SpeedType::Track || pType->SpeedType == SpeedType::Wheel;

	return mayDrop ? 0 : KeepKey;
}

// The voxel cache is one 4 MB block (0x40F270), and a game full of Siege Choppers fills it, which throws
// every cached image away. Phobos 0.5 makes it 128 MB with this same patch; remove this with Phobos 0.5.
DEFINE_PATCH(0x40F271, 0x00, 0x00, 0x00, 0x08);
