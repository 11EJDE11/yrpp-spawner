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

#pragma once

// The on-screen playback controls: a transport row, a clock, a speed readout and a seek bar, drawn
// along the bottom of the tactical view during replay playback.
//
// Drawn into the composite surface after the sidebar, the message list and the tooltips but before
// the mouse cursor, so nothing in the game covers it and the cursor stays on top of it.
//
// Input is taken ahead of the game's own, from the point MouseClass::AI hands off to the rest of
// the input chain. While the pointer is over the panel - or holding the seek handle, wherever it
// has been dragged to - the whole chain below is skipped, so a click on a button is not also a
// click on the map underneath it and the bottom screen edge does not scroll the view.

namespace ReplaySystem
{
	namespace Overlay
	{
		// Runs the panel's input for the frame: hover, button presses and seek-handle dragging.
		// True means the panel took the mouse and the input chain below it has to be skipped for
		// this frame, which is what keeps a click on a button off the map behind it. Call it every
		// frame regardless, so the button-press edge is still tracked while the pointer is away.
		bool ProcessMouseInput(int mouseX, int mouseY);

		// Draws the panel. Does nothing when playback is not running or the bar is hidden.
		void Draw();

		// Drops any half-finished press or drag - a keyframe load rebuilds the world underneath the
		// panel, and a drag begun before it means nothing afterwards.
		void CancelInteraction();
	}
}
