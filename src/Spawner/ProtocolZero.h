/**
*  yrpp-spawner
*
*  Copyright(C) 2023-present CnCNet
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

class EventExt;

class ProtocolZero
{
private:
	static constexpr int SendResponseTimeInterval = 30;
	static constexpr int SendResponseTimeFrame = 8 * SendResponseTimeInterval;

public:
	static bool Enable;
	// Report max(engine response time, clean measured round trip) rather than the
	// engine's figure alone. Avg_Response_Time is a 256-sample mean fed only by
	// acknowledged reliable packets, so it climbs about one tick per 400 frames -
	// a link that degrades suddenly takes minutes to reach the rung it needs, and
	// the game is jittery for all of it. The clean estimate settles in seconds.
	//
	// Taking the maximum can only ever report a HIGHER figure than today, so it
	// can never select less headroom than current behaviour. Under sustained loss
	// the engine's figure is the larger of the two and still wins, which keeps the
	// loss margin its inflation accidentally provides - and keeps descents, which
	// track that same slow figure, exactly as conservative as they are now.

	// Minimum per-connection timeout, in 16 ms engine ticks. The engine's own
	// floor is 120 (1.92 s), which drops a client that stalls briefly for local
	// reasons. Zero keeps vanilla behaviour.
	static int ConnectionTimeoutFloor;
	static unsigned char MaxLatencyLevel;
	static int WorstMaxAhead;
	static int NextSendFrame;

	static void SendResponseTime2();
	static void HandleResponseTime2(EventExt* event);
};
