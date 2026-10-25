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
#include <ProgressScreenClass.h>
#include <SessionClass.h>
#include <Unsorted.h>
#include <WWMouseClass.h>

// skip error "А mouse is required for playing Yurts Revenge" - remove the GetSystemMetrics check
DEFINE_JUMP(LJMP, 0x6BD8A4, 0x6BD8C2); // WinMain

// Prevents accidental exit when pressing the spacebar while waiting
// Remove focus from the Leave Game button in the player waiting window
DEFINE_HOOK(0x648CCC, WaitForPlayers_RemoveFocusFromLeaveGameBtn, 0x6)
{
	Imports::SetFocus(0);

	return 0;
}

// A patch to prevent framerate drops when a player spams the 'type select' key
// Skip call GScreenClass::FlagToRedraw(1)
DEFINE_JUMP(LJMP, 0x732CED, 0x732CF9); // End_Type_Select_Command

DEFINE_HOOK(0x649851, WaitForPlayers_OnlineOptimizations, 0x5)
{
	Sleep(3); // Sleep yields the remaining CPU cycle time to any other processes

	return 0x6488B0;
}

// Open campaign briefing when pressing Tab
DEFINE_HOOK(0x55E08F, KeyboardProcess_PressTab, 0x5)
{
	Game::SpecialDialog = SessionClass::IsCampaign() ? 9 : 8;

	return 0x55E099;
}

// Do_Blit's 50ms wait guards the cursor restore after the primary-surface blit, but that restore
// (0x7B92D0) and the draw before it (0x7B90C0) are both no-ops unless IsCaptured(). Loading screens
// pass mouse_captured = true while the mouse is uncaptured, so vanilla sleeps 40+ times per scenario
// load guarding nothing - about two seconds. When captured, this is the vanilla path unchanged.
DEFINE_HOOK(0x4F4B8E, GScreenClass_DoBlit_SkipUncapturedCursorWait, 0x8)
{
	// Do_Blit already tested WWMouse for null at 0x4F4B8A.
	if (WWMouseClass::Instance->IsCaptured())
		Sleep(50);

	return 0x4F4B96;
}

// A player's loading progress as another reports it can only go up. Reports at 100% are sent reliably,
// the ones before unreliably, so a 99% one that arrives after the 100% (reordered on the way) set the
// player back to 99% for good: the loading screen waited for it until it timed out and dropped that
// player, who had gone on into the game with everyone else - a game that split at frame 0.
// 0x48DAA3 is `fild GPacket.Chat_ID` (6 bytes, absolute) in Network_Call_Back's NET_PROGRESS_REPORT
// case; ESI is the sender's index in Players, as ProgressScreenClass::Init (0x642A60) zeroes them.
// The scale at +0x48 is a double (0x643C98 loads it with fld qword), not the int YRpp declares.
DEFINE_HOOK(0x48DAA3, NetworkCallBack_ProgressNeverBack, 0x6)
{
	enum { Skip = 0x48DAD3 };

	GET(int, index, ESI);
	const int percent = *reinterpret_cast<const int*>(0xA8D667);

	auto& screen = ProgressScreenClass::Instance;
	const double scale = *reinterpret_cast<const double*>(reinterpret_cast<const char*>(&screen) + 0x48);
	if (index >= 0 && index < 8 && scale * 0.01 * percent < screen.PlayerProgresses[index])
		return Skip;

	return 0;
}
