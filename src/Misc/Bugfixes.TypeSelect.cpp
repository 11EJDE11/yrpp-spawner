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
#include <Utilities/Debug.h>
#include <Unsorted.h>
#include <WWKeyboardClass.h>

// Type select (T by default) is a held-key mode. TypeSelectCommandClass::Process sets
// Unsorted::TypeSelecting when the key goes down and clears it when the key comes up. While the flag is
// set, clicking a unit or dragging a box selects every unit of that type, not just that unit.
// Only the key-up clears the flag, and vanilla can lose the key-up:
// - Put_Key_Message (0x54F25B) adds the live Ctrl/Alt state to key-ups as well as key-downs.
//   Keyboard_Process then looks the key-up up as Ctrl+T or Alt+T, because TypeSelect's Check_Key1
//   (0x536880) only accepts Shift. So letting go of T while holding Ctrl or Alt (Ctrl+1 to make a
//   team, force fire, force move) never reaches the command.
// - An open chat line swallows every key (MessageListClass::Input), locked input drops it, and
//   after an Alt+Tab it goes to another window.
// The flag then stays set until the key is pressed and released again.
//
// Fix: clear the flag once the bound key has been physically up for a few frames with no input
// queued. Checking the queue keeps a normal tap working. A key-up still waiting in the queue keeps
// the queue non-empty, so Finish_Type_Selection reads it first and does the tap selection.
namespace TypeSelectFix
{
	// Virtual key of the last press that started type select.
	static BYTE Key = 0;
	static int UpFrames = 0;
	// The queue is read one event per frame and Windows messages are pumped once per frame.
	// Three frames is long enough for a pending key-up to have reached the queue.
	constexpr int UpFramesToClear = 3;
}

DEFINE_HOOK(0x5368B0, TypeSelectCommandClass_Process_RememberKey, 0x7)
{
	GET_STACK(const int, input, 0x4);

	if (!(input & static_cast<int>(WWKey::Release)))
		TypeSelectFix::Key = static_cast<BYTE>(input & 0xFF);

	return 0;
}

// Main_Loop, just before GScreenClass::Input reads this frame's event. Running here means the flag
// is cleared before a click is handled.
DEFINE_HOOK(0x55D897, MainLoop_ClearStuckTypeSelect, 0x8)
{
	using namespace TypeSelectFix;

	if (!Unsorted::TypeSelecting || !Key)
	{
		UpFrames = 0;
		return 0;
	}

	const auto pKeyboard = WWKeyboardClass::Instance;
	const bool inputQueued = pKeyboard->Head != pKeyboard->Tail;
	const bool keyHeld = (GetAsyncKeyState(Key) & 0x8000) != 0;

	if (inputQueued || keyHeld)
	{
		UpFrames = 0;
		return 0;
	}

	if (++UpFrames >= UpFramesToClear)
	{
		Unsorted::TypeSelecting = false;
		UpFrames = 0;
		Debug::Log("Type select: key 0x%02X is up but its release never arrived; cleared the stuck type select mode.\n", Key);
	}

	return 0;
}
