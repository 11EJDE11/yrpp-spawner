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

class EventClass;

namespace ReplaySystem
{
	// What a player sees and hears when they give an order comes partly from the click, not the
	// order: the action lines, the unit's acknowledgement and the sidebar's EVA line. Playback has
	// the orders but not the clicks, so it plays these from the watched player's orders instead.
	// Purely local - nothing here touches the simulation.
	namespace OrderFeedback
	{
		// Each recorded gameplay event as it is injected, before any of the frame's events run, so
		// the house's factories are still in the state the click saw.
		void OnEventInjected(const EventClass& event);

		// Once the frame's events are all injected.
		void OnFrameInjected();

		// Whether the watched player is in planning mode - holding the key that plans waypoints - as
		// far as their orders show: from the first order they add to a route until the commit that
		// releasing the key sends.
		bool IsWatchedPlayerPlanning();

		// Forgets everything, for a new playback or a stream repositioned by a seek.
		void Reset();
	}
}
