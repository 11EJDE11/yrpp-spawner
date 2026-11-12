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


namespace ReplaySystem
{
	namespace Overlay
	{
		bool ProcessMouseInput(int mouseX, int mouseY);

		// Draws the panel, or while the bar is hidden the tab that shows it again (only with the mouse over
		// where the bar sits). Does nothing outside playback.
		void Draw();

		// Drops any half-finished press or drag - a keyframe load rebuilds the world underneath the
		// panel, and a drag begun before it means nothing afterwards.
		void CancelInteraction();
	}
}
