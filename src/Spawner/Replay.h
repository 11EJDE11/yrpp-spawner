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

	// Open events.dat, read recorded Seed, seek past header. Call before Game::InitRandom().
	static void SetupPlayback();

	// Apply playback patches before scenario startup so private SelfCRC accepts them as the expected .text state.
	static void ApplyPlaybackOptions();

	// Enforce playback-only options each frame and discard local input events before native playback executes recorded events.
	static void ApplyPlaybackFrameOptions();
};
