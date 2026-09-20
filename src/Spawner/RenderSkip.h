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

/**
*  RenderSkip - drops render frames while this client is the one holding the
*  game up.
*
*  Main_Loop measures ProcessingTicks from before GScreenClass::Input to after
*  LogicClass::AI, so the render is inside the "Process" figure the multiplayer
*  debug overlay reports and Queue_AI_Multiplayer ships to the other players.
*  Network waiting is outside it. In a large battle that figure can reach 60ms
*  or more. ProtocolZero does not remove the engine's frame-rate negotiation:
*  Queue_AI_Multiplayer still collects the peers' reported process times and
*  still emits ordinary timing events, and those events can move the negotiated
*  rate. What it does is pin PreCalcFrameRate to 60 on every latency change, so
*  the negotiated rate no longer falls to match the slowest client. A struggling
*  client therefore falls behind and stalls every peer through the MaxAhead gate
*  instead of everyone being slowed to its speed. Skipping renders is the local
*  half of that trade; it also feeds back into the shared negotiation through
*  the process-time reports, which is intended - a cheaper frame is a lower
*  report.
*
*  Skipping the render is safe by precedent: Main_Loop already skips both Input
*  and Render whenever the window loses focus, for arbitrarily many consecutive
*  frames. If that path touched sync-critical state, alt-tabbing out of a
*  multiplayer game would desync it. RadBeam::Draw_All reads RequestedFPS but is
*  reached only from Tactical::Render, so it is inside that same skipped path.
*
*  The engine already renders MORE when it has slack - Sync_Delay draws an extra
*  frame when at least 10ms of the NFTTimer budget remains. This is the missing
*  other half: rendering less when there is none.
*/

#pragma once

class RenderSkip
{
public:
	static bool Enabled;

	// Never drop more than this many frames in a row. One means strict
	// alternation - draw, skip, draw, skip - which halves the render cost with
	// even pacing. Larger values buy more headroom but arrive as bursts: at 4,
	// the display shows one frame in five for the duration, which reads as a
	// stutter rather than a lower frame rate.
	static int MaxConsecutive;

	// Per-frame processing budget in milliseconds. Zero derives it from the
	// requested frame rate.
	static int BudgetMs;

	static void Reset();

	// One measured render, in milliseconds. Feeds the estimate of what a fully
	// rendered frame would cost, which is what the release decision needs.
	static void NoteRenderCost(int ms);

	// Called in place of Main_Loop's render. Advances the throttle state.
	static bool ShouldRenderThisFrame();

	// Gates Sync_Delay's opportunistic Input/Tactical::AI/Render block, which
	// the engine enters only when the frame budget has slack left.
	static bool ThrottleActive();
};
