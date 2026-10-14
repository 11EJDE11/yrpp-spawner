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

#pragma once

#include <cstddef>
#include <cstdint>

// Who a chat message went to, worked out by its sender and carried in the message packet, so every
// receiver and a replay can show it next to the sender's name.
enum class ChatLabel : uint8_t
{
	Unknown = 0, // sent by a client that does not say, or recorded before it was carried
	All = 1,     // every other player
	Team = 2,    // the sender's allies
	Private = 3, // some other set of players, ticked in the diplomacy menu
};

namespace InGameChat
{
	// The label as stored: anything out of range reads as Unknown.
	ChatLabel ToLabel(uint8_t value);

	// "Name (Team)" into out, or the name alone for Unknown.
	void ComposeSenderName(wchar_t* out, size_t capacity, const wchar_t* name, ChatLabel label);
}
