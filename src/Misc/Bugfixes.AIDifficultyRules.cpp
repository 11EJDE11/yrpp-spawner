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

// Ares and Phobos both replace the engine's AI production picker, and both index three rules lists
// with the house's AI difficulty without checking the list is that long:
//
//   Ares    Ext/House/Hooks.100.cpp:50-51,62   Phobos  Ext/House/Body.cpp:167-168,180
//     pRules->HarvestersPerRefinery[AIDiff]
//     pRules->AISlaveMinerNumber[AIDiff]
//     RulesClass::Instance->FillEarliestTeamProbability[AIDiff]
//
// A YR ruleset gives all three an entry per difficulty and the index is fine. An RA2 one does not:
// on RA2 rules HarvestersPerRefinery is a single value and AISlaveMinerNumber - slave miners being
// a Yuri unit - is absent altogether, so a house at difficulty 1 or 2 reads off the end of the
// array. AISlaveMinerNumber is worse than out of range: an empty TypeList has a null Items, so the
// read is through a null pointer.
//
// The consequence is not a crash, because the value read is only compared against:
//
//     maxHarvesters = HarvestersPerRefinery[AIDiff] * pThis->CountResourceDestinations;
//     if (... && CountResourceGatherers < maxHarvesters && ...) { return true; }  // skips the rolls
//
// It is worse than a crash. Whatever happens to sit past the end of the array decides whether the
// house's whole production block runs, and that block draws from the synchronised randomiser. The
// heap does not have to hold the same thing twice, so the same frame can take a different branch
// the second time it is run - which is exactly what replay seeking does after loading a keyframe.
// One house skipping its two production draws puts the randomiser two draws out and every timer
// downstream of it drifts.
//
// Neither extension is ours to change, so the lists are made long enough that the index they use is
// always in range. Entries the ruleset actually defines are never touched, so a YR ruleset with a
// full set of three sees no change at all; only the indices that were previously read out of bounds
// gain a defined value. A short list is extended by repeating its last entry, which is the usual
// reading of a ruleset that gives one value for every difficulty. An empty one is filled with zero,
// which makes the comparisons it feeds fail closed - the AI declines to queue something rather than
// deciding at random.

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

// RulesClass::Process, just past the last of the section readers - RulesClass::AI, ::IQ and
// ::Objects have all run, so the three lists hold whatever this INI had to say about them. The
// stolen bytes are a single absolute push, and Process is re-entered for the map's own rules
// overrides, so this fires again after each one and pads whatever that pass left short.
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
