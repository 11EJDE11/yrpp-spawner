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

class HouseClass;

// Seeking within a replay.
//
// Playback is a deterministic recurrence - state(N) comes from state(0) and the events up to N - so
// seeking forward is just running the simulation without drawing it, and seeking backwards needs an
// earlier state to restart from. Rather than put those states in the replay file, where they would
// dwarf the events many times over, playback drops one every ReplayKeyframeInterval frames as it
// watches, into the engine's own savegame format. That only makes the part of the replay already
// watched cheap to rewind into, which is the part a viewer wants to rewind into.
//
// A seek therefore runs in two halves: pick the newest keyframe at or before the target and load it
// (skipped entirely when the target is ahead of where playback already is), then run frames with
// the frame pacing, the drawing and the sound switched off until the target arrives.
//
// Nothing here touches the replay file format. Keyframes are scratch files under the spawner's
// SavedGameDir, written when playback starts and deleted when it ends.

namespace ReplaySystem
{
	namespace Seek
	{
		// Frames between keyframes, from ReplayKeyframeInterval. Zero when keyframes are switched
		// off, which leaves seeking forward-only.
		int KeyframeInterval();

		// Opens the keyframe store for a replay and drops the frame-zero keyframe. Called once, as
		// playback starts.
		void OnPlaybackStarted();

		// Deletes every keyframe written for this playback.
		void OnPlaybackStopped();

		// Clears samples retained by the expensive divergence watches without touching keyframes
		// or an in-progress seek. Used when starting a fresh mid-replay diagnostic window.
		void ResetDiagnostics();

		// Top of the frame, before anything has run: finishes a seek that asked for a load, and
		// takes this frame's keyframe if one is due. Both need a whole, quiescent game state, so
		// this is the only point either happens.
		void ServiceFrameStart();

		// Asks for the target frame. Backwards needs a keyframe at or before it; forwards always
		// works. False means the seek could not be started, and playback carries on untouched.
		//
		// pauseOnArrival freezes playback once the target lands. It has to be asked for here rather
		// than by pausing afterwards, because a seek runs on the frame counter advancing and a
		// pause is exactly what stops it.
		bool RequestSeek(int targetFrame, bool pauseOnArrival = false);

		// True from RequestSeek until the target frame arrives. Playback is running flat out and
		// mostly not drawing while this holds.
		bool IsSeeking();

		// Whether this frame's GScreenClass::Render should be skipped. A seek still draws one frame
		// in every SeekRenderInterval, so a long one does not look like a hang.
		bool ShouldSkipRenderThisFrame();

		// Called once per rendered frame while seeking, to pace the occasional drawn frame.
		void CountRenderedFrame();

		// The earliest frame a backwards seek can reach, which is the oldest keyframe still held.
		int EarliestSeekableFrame();

		// Frames that hold a keyframe, oldest first, for the seek bar to tick. Count is capped by
		// the caller's array size; returns how many were written.
		int CollectKeyframeFrames(int* outFrames, int maxFrames);

		// The frame playback has reached, which is the engine's own frame counter.
		int CurrentFrame();

		// How long the replay is, in frames: the recording's own count when it shut down cleanly,
		// otherwise the furthest frame playback has seen. Never less than CurrentFrame.
		int TotalFrames();

		// Frames per second the recording ran at, for turning frame numbers into clock times.
		int RecordedFPS();

		// True while a keyframe load is running, so the scenario-start hooks leave the in-flight
		// playback alone instead of reopening the replay from the beginning.
		bool IsLoadInProgress();

		// Mirrors HouseExt::UpdateHarvesterProduction's gate and writes down every input to it, for a
		// short window after each keyframe boundary. That function returns before any of the AI's
		// production rolls happen, so a gate that answers differently after a load moves the
		// randomiser with nothing else visibly wrong - which is what the Boot Camp seek traces kept
		// finding. Both passes log the same frames, so the two runs can be diffed directly.
		void TraceHouseProductionGate(HouseClass* pHouse);
	}
}
