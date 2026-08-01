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

#include "PacketRedundancy.h"

#include <windows.h>

bool PacketRedundancy::Enabled = true;
int  PacketRedundancy::Copies = 2;
bool PacketRedundancy::Adaptive = false;
int  PacketRedundancy::Emitted = 0;

namespace
{
	/**
	 *  Where the packet code sits in the bytes handed to Tunnel::SendTo.
	 *
	 *  The engine sends from &WinsockBufferType::CRC with length BufferLen + 4
	 *  (UDPInterfaceClass::Message_Handler, 0x7B3D89), so the buffer is
	 *  [CRC:4][CommHeaderType...]. CommHeaderType is MagicNumber:u16 then Code:u8,
	 *  putting Code at byte 6. Both the open and the Hardened Tunnel::SendTo
	 *  receive this same buffer, so the offset holds for either.
	 */
	constexpr size_t WireCodeOffset = 6;

	//!< ConnectionEnum::PACKET_DATA_ACK, from ConnectionClass::Send_Packet 0x48BF40.
	constexpr unsigned char PacketDataAck = 0;

	// Adaptive loss gauge. Each observed resend bumps it; it decays with wall
	// time. Accumulating rather than latching gives hysteresis, so redundancy
	// does not flap on and off packet to packet.
	constexpr int GaugeBump = 1000;      //!< per observed resend
	constexpr int GaugeCap = 5000;       //!< about five seconds of memory
	constexpr int GaugeUnitsPerMs = 1;   //!< decays 1000 units per second

	int   Gauge = 0;
	DWORD LastDecayTick = 0;

	void DecayGauge()
	{
		const DWORD now = GetTickCount();

		if (LastDecayTick == 0)
		{
			LastDecayTick = now;
			return;
		}

		const DWORD elapsed = now - LastDecayTick;
		LastDecayTick = now;

		const int decay = (int)(elapsed * GaugeUnitsPerMs);
		Gauge = (decay >= Gauge) ? 0 : (Gauge - decay);
	}
}

void PacketRedundancy::Reset()
{
	Gauge = 0;
	LastDecayTick = 0;
	Emitted = 0;
}

void PacketRedundancy::NoteResend()
{
	DecayGauge();

	Gauge += GaugeBump;
	if (Gauge > GaugeCap)
		Gauge = GaugeCap;
}

int PacketRedundancy::CopiesFor(const char* buf, size_t len)
{
	if (!Enabled || Copies < 2)
		return 1;

	if (!buf || len <= WireCodeOffset)
		return 1;

	// Only reliable, command-bearing packets are worth duplicating. Beacons and
	// bare ACKs are already sent constantly and cost nothing to lose.
	if ((unsigned char)buf[WireCodeOffset] != PacketDataAck)
		return 1;

	if (Adaptive)
	{
		DecayGauge();
		if (Gauge <= 0)
			return 1;   // link looks clean, do not spend the bandwidth
	}

	Emitted += Copies - 1;
	return Copies;
}
