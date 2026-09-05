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

#include <Utilities/Macro.h>
#include <UnitClass.h>
#include <EBolt.h>

// Fix crash at 6F9DB6
DEFINE_JUMP(LJMP, 0x5F58CB, 0x5F58D2); // ObjectClass::Mark

// Fix crash at 727B48
DEFINE_HOOK(0x727B44, TriggerTypeClass_ComputeCRC_FixCrash, 0x6)
{
	GET(HouseTypeClass*, pHouseType, EAX);
	if (!pHouseType)
		return 0x727B55;

	return 0;
}

// Fix crash at 4722F0
DEFINE_HOOK(0x70F82A, TechnoClass_GetMyOwner_FixCrash, 0x7)
{
	GET(TechnoClass*, pTechno, EAX);
	if (!pTechno->CaptureManager)
		return 0x70F837;

	return 0;
}

// Fix crash at 6F49DE
DEFINE_HOOK(0x6F49D8, TechnoClass_Revealed_FixCrash, 0x6)
{
	GET(HouseClass*, pHouse, EAX);
	if (!pHouse)
		return 0x6F4A31;

	return 0;
}

// Fix crash at 70AF6C
DEFINE_HOOK(0x70AF6C, TechnoClass_70AF50_FixCrash, 0x9)
{
	GET(TechnoClass*, pTechno, EAX);
	if (!pTechno)
		return 0x70B1C7;

	return 0;
}

// Fix crash at 65DC17
DEFINE_HOOK(0x65DC17, DoReinforcements_FixCrash, 0x6)
{
	GET(HouseClass*, pHouse, ECX);
	if (!pHouse)
	{
		R->EAX(0);
		return 0x70B1C7;
	}

	return 0;
}

// Fix crash at 705DEE on exit.
//
// Prog_End (0x6BE1C0) frees the colour schemes and nulls ColorSchemes.Vector before it calls
// Destroy_Vectors (0x6BE280), so any object still alive at that point is destroyed with no colour
// schemes left.
void DestroyGameObjectVectors()
{
	reinterpret_cast<void(__fastcall*)()>(0x534450)(); // Destroy_Vectors
}

DEFINE_HOOK(0x6BE222, ProgEnd_DestroyVectorsBeforeColorSchemes, 0x6)
{
	DestroyGameObjectVectors();
	return 0;
}

// The call this replaces, now that the vectors are already empty.
DEFINE_JUMP(LJMP, 0x6BE280, 0x6BE285); // Prog_End
