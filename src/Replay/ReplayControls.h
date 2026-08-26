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

// The viewer-side controls for replay playback: pause, and a playback speed that is allowed to run
// past the 60 FPS ceiling the in-game speed slider tops out at. Both are plain CommandClass
// commands, so they are bound from the in-game keyboard options like any other command and ship
// with no binding of their own.
//
// Nothing here is part of the replay file format - a recording says what speed it was played at,
// never what speed someone watched it back at.

namespace ReplaySystem
{

namespace Controls
{

// Playback speed ladder, in frames per second. The first seven rungs are exactly the rates the
// engine's own game-speed slider produces (index 6 -> 10 FPS, ..., index 0 -> 60 FPS), so the
// options dialog and these hotkeys agree wherever their ranges overlap. The rungs past 60 FPS have
// no slider position and exist only for playback: a live game paces itself against the other
// players and has no way to ask for them.
//
// The top of the ladder is spaced by what the engine can actually distinguish rather than by round
// numbers. Main_Loop paces a frame off `NFTTimer.Accumulated = 1000 / RequestedFPS` milliseconds
// (0x55D522), so above 240 FPS only the rungs that land on a different millisecond mean anything -
// 300 -> 3ms, 500 -> 2ms, 1000 -> 1ms - and anything past 1000 divides to 0, which Sync_Delay reads
// as "do not wait at all". 2000 is that uncapped rung: what it delivers is however fast the machine
// can simulate and draw a frame, which is the real ceiling well before the timer is.
constexpr int SPEED_LADDER[] = { 10, 12, 15, 20, 30, 45, 60, 90, 120, 180, 240, 300, 500, 1000, 2000 };
constexpr int SPEED_LADDER_COUNT = static_cast<int>(sizeof(SPEED_LADDER) / sizeof(SPEED_LADDER[0]));

// True while playback is frozen. The main loop still renders, scrolls and takes input; only the
// simulation is held.
bool IsPlaybackPaused();

// Toggles the freeze. Does nothing outside playback.
void TogglePlaybackPause();

// Walks the ladder by one rung: +1 faster, -1 slower. Does nothing outside playback.
void StepPlaybackSpeed(int direction);

// The game-speed index (0 fastest .. 6 slowest) the options dialog should show and compare against
// for the current playback speed. Speeds past 60 FPS have no slider position of their own and
// report as 0, the fastest one the slider has.
int GetPlaybackGameSpeedIndex();

// Applies a game-speed index chosen from the options dialog or replayed from a GameSpeed event.
void SetPlaybackGameSpeedIndex(int gameSpeedIndex);

// Adds the replay commands to CommandClass::Array so the keyboard options dialog lists them.
void RegisterReplayCommands();

} // namespace Controls

} // namespace ReplaySystem
