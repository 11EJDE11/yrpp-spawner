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

// Ares and Phobos index HarvestersPerRefinery, AISlaveMinerNumber and
// FillEarliestTeamProbability by AI difficulty without a bounds check. RA2 rules
// define fewer than three entries (AISlaveMinerNumber none at all), so normal and
// hard AI read out of bounds. Short lists are padded with their last entry, or
// zero if empty; entries the rules define are unchanged.

#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <RulesClass.h>

namespace
{
	// AIDifficulty is easy, normal and hard, and GetAIDifficultyIndex returns it unchanged.
	constexpr int AIDifficultyCount = 3;

	void PadDifficultyList(TypeList<int>& list, const char* name)
	{
		if (list.Count >= AIDifficultyCount)
			return;

		const int had = list.Count;
		const int fill = had > 0 ? list.Items[had - 1] : 0;

		while (list.Count < AIDifficultyCount)
			list.AddItem(fill);

		Debug::Log("Rules list %s had %d entries and the AI production code indexes it by "
			"difficulty; padded to %d with %d so it is not read past its end.\n",
			name, had, AIDifficultyCount, fill);
	}
}

DEFINE_HOOK(0x668EF5, RulesClass_Process_PadAIDifficultyLists, 0x5)
{
	GET(RulesClass* const, pRules, EDI);

	if (pRules)
	{
		PadDifficultyList(pRules->HarvestersPerRefinery, "HarvestersPerRefinery");
		PadDifficultyList(pRules->AISlaveMinerNumber, "AISlaveMinerNumber");
		PadDifficultyList(pRules->FillEarliestTeamProbability, "FillEarliestTeamProbability");
	}

	return 0;
}
