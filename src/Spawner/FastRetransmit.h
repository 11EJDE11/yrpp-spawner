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

/**
 *  A clean round-trip estimate, and a retransmit timer built on it.
 *
 *  The engine recomputes its retransmit timing every 128 frames in
 *  Queue_AI_Multiplayer (0x6476D0):
 *
 *      RetryDelta = Response_Time() + 10
 *
 *  ...where Response_Time() is CommBufferClass::Avg_Response_Time, fed by
 *  Add_Delay at 0x48C43F for every ACKed entry -- *including retransmitted ones*.
 *  The delay recorded for a retransmitted packet spans from its first send, so it
 *  contains the retry wait itself.
 *
 *  That is a positive feedback loop. Loss causes retransmits, retransmits inflate
 *  the average, the inflated average lengthens RetryDelta, and the longer wait
 *  inflates the next measurement further. The reported figure climbs into the
 *  tens of ticks on a link whose real round trip is a handful, so the timer
 *  barely shrinks at precisely the moment it most needs to.
 *
 *  Karn's algorithm is the standard fix: never take a round-trip sample from a
 *  packet that was retransmitted. We track our own smoothed value from first-send
 *  ACKs only and use it to *shorten* the engine's RetryDelta, never to lengthen
 *  it.
 *
 *  Shortening alone would be dangerous. Vanilla has no backoff whatsoever
 *  (MaxRetries = -1, re-blast every RetryDelta forever), so on a badly lossy link
 *  a short fixed timer turns into a flood that makes the loss worse. So the
 *  second half of this is a capped backoff on repeat attempts for the same
 *  entry: aggressive on the first retry, progressively patient after that.
 *
 *  Units throughout are SystemTimer ticks (1/60 s).
 */
class FastRetransmit
{
public:
	static bool Enabled;

	//!< Added to the smoothed round trip to absorb ordinary jitter.
	static const int MarginTicks = 2;

	//!< Never drive the retry wait below this, however good the link looks.
	static const int MinTicks = 2;

	static bool Backoff;

	//!< Each prior attempt adds this many halves of the base delay: 1 => 1x,
	//!< 1.5x, 2x, 2.5x ...
	static const int BackoffStepHalves = 1;

	//!< Cap on prior attempts counted, bounding the growth (=> up to 3x base).
	static const int BackoffCap = 4;

	//!< Smoothed round trip in ticks, 0 until a clean sample exists.
	static int SmoothedRTT();

	//!< Folds one measurement in. Samples from retransmitted packets are
	//!< discarded (Karn) and instead reported as evidence of loss.
	static void SampleRTT(int delayTicks, int sendCount);

	static void Reset();
};
