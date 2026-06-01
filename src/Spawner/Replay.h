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

class Replay
{
public:
	static bool IsPlaybackActive();

	// Enable recording mode (set Session.Record flag). Call before Game::InitRandom().
	static void PrepareRecording();

	// Open RECORD.BIN and write the recording header. Call after scenario starts successfully.
	static void StartRecording();

	// Open events.dat and restore the recorded header. Call before Game::InitRandom().
	static bool SetupPlayback();

	// Apply playback code patches before scenario startup, so the patched code is in place before the game runs.
	static void ApplyPlaybackOptions();

	// Keep playback timing/spectator state in sync and discard local input events each frame, before native playback executes the recorded events.
	static void ApplyPlaybackFrameOptions();
};
