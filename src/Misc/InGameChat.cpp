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

#include "InGameChat.h"

#include <Spawner/Spawner.h>
#include <Utilities/Macro.h>
#include <HouseClass.h>
#include <HouseTypeClass.h>
#include <StringTable.h>
#include <Unsorted.h>
#include <Replay/ReplaySystem.h>

#include <MessageListClass.h>
#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

// This corrects the processing of Unicode player names
// and prohibits incoming messages from players with whom chat is disabled

#pragma pack(push, 1)
struct GlobalPacket_NetMessage
{
	DEFINE_REFERENCE(GlobalPacket_NetMessage, Instance, 0xA8D638u);

	int Command;
	wchar_t PlayerName[21];
	byte HouseIndex;
	byte ChatID;
	wchar_t Message[112];
	byte Color;
	byte CRC;
	// Past the vanilla fields. The packet goes out at the full GlobalPacketType size (455 bytes) and
	// arrives whole in this buffer, so the spawner's own fields ride along here: the salt of the
	// message's ChatFingerprint, and who the sender sent it to.
	uint32_t ExtMagic;
	uint8_t Salt[16];
	uint8_t HasSalt;
	uint8_t Label;      // ChatLabel
	uint8_t Recipients; // a bit per house index
};

#pragma pack(pop)

static_assert(offsetof(GlobalPacket_NetMessage, ExtMagic) == 274);
static_assert(sizeof(GlobalPacket_NetMessage) <= 455);

// What Message_Input's send loop reads off each house (0x55EEAA): its type, then the type's Multiplay.
static_assert(offsetof(HouseClass, Type) == 0x34);
static_assert(offsetof(HouseTypeClass, Multiplay) == 0x1A5);

namespace
{
	constexpr uint32_t ChatExtMagic = 0x32435259; // 'YRC2'

	// Message_Input's chat scope, set by the key that opened the prompt (0x55E550): 1 (Enter) the
	// ChatMask houses, 2 (Backspace) allies, 3 (backslash) everyone. Zero once the observer check has
	// refused the send.
	int& ChatScope = *reinterpret_cast<int*>(0xABCE18);

	// Set between the send loop and the local print of the same message.
	bool HasOutgoingSalt = false;
	uint8_t OutgoingSalt[16] = {};
	ChatLabel OutgoingLabel = ChatLabel::Unknown;
	uint8_t OutgoingRecipients = 0;

	// The sender name the message list prints, with the label after it.
	wchar_t ReceivedSenderName[48] = {};
	wchar_t OutgoingSenderName[48] = {};

	bool HasExtension()
	{
		return GlobalPacket_NetMessage::Instance.ExtMagic == ChatExtMagic;
	}

	const uint8_t* ReceivedSalt()
	{
		return HasExtension() && GlobalPacket_NetMessage::Instance.HasSalt
			? GlobalPacket_NetMessage::Instance.Salt
			: nullptr;
	}

	// Who Message_Input's send loop (0x55EE8B-0x55F03F) sends to: every multiplayer house but the
	// sender's own that is masked in ChatMask for scope 1, allied for scope 2, or any for scope 3.
	// The loop then sends to the connection of that house's name, which only a human player has.
	void ComputeRecipients(int scope, ChatLabel& label, uint8_t& recipients)
	{
		label = ChatLabel::Unknown;
		recipients = 0;

		const HouseClass* const pSender = HouseClass::CurrentPlayer;
		if (!pSender || scope < 1 || scope > 3)
			return;

		uint8_t players = 0; // everyone else, observers aside
		uint8_t allies = 0;
		const int count = std::min(HouseClass::Array.Count, 8);
		for (int i = 0; i < count; ++i)
		{
			const HouseClass* const pHouse = HouseClass::Array.GetItem(i);
			if (!pHouse || pHouse == pSender || !pHouse->Type || !pHouse->Type->Multiplay || !pHouse->IsHumanPlayer)
				continue;

			const uint8_t bit = static_cast<uint8_t>(1u << i);
			const bool ally = pSender->IsAlliedWith(pHouse);
			if (!pHouse->IsObserver())
			{
				players |= bit;
				if (ally)
					allies |= bit;
			}

			if ((scope == 1 && Game::ChatMask[i]) || (scope == 2 && ally) || scope == 3)
				recipients |= bit;
		}

		const uint8_t reached = recipients & players;
		if (scope == 3 || (scope == 1 && reached == players))
			label = ChatLabel::All;
		else if (scope == 2 || (allies != 0 && reached == allies))
			label = ChatLabel::Team;
		else
			label = ChatLabel::Private;
	}
}

ChatLabel InGameChat::ToLabel(uint8_t value)
{
	return value <= static_cast<uint8_t>(ChatLabel::Private) ? static_cast<ChatLabel>(value) : ChatLabel::Unknown;
}

void InGameChat::ComposeSenderName(wchar_t* out, size_t capacity, const wchar_t* name, ChatLabel label)
{
	if (!out || capacity == 0)
		return;

	const wchar_t* pLabel = nullptr;
	switch (label)
	{
	case ChatLabel::All:
		pLabel = StringTable::TryFetchString("TXT_CHAT_LABEL_ALL", L"All");
		break;
	case ChatLabel::Team:
		pLabel = StringTable::TryFetchString("TXT_CHAT_LABEL_TEAM", L"Team");
		break;
	case ChatLabel::Private:
		pLabel = StringTable::TryFetchString("TXT_CHAT_LABEL_PRIVATE", L"Private");
		break;
	default:
		break;
	}

	if (pLabel && *pLabel)
		swprintf_s(out, capacity, L"%ls (%ls)", name ? name : L"", pLabel);
	else
		wcsncpy_s(out, capacity, name ? name : L"", _TRUNCATE);
}

static bool inline IsDisableChatEnabled()
{
	return Spawner::Enabled && Spawner::GetConfig()->DisableChat;
}

// Don't send message to others when DisableChat is active.
// Mirrors: hack 0x0055EF38, 0x0055EF3E in chat_disable.asm
DEFINE_HOOK(0x55EF38, MessageSend_DisableChat, 0x6)
{
	static int LastDisableChatFeedbackFrame = -1000;

	if (IsDisableChatEnabled())
	{
		const int currentFrame = Unsorted::CurrentFrame;

		if (currentFrame < LastDisableChatFeedbackFrame)
			LastDisableChatFeedbackFrame = -1000; // new match started

		if (currentFrame - LastDisableChatFeedbackFrame >= 90)
		{
			MessageListClass::Instance.PrintMessage(L"Chat is disabled. Message not sent.");
			LastDisableChatFeedbackFrame = currentFrame;
		}

		HasOutgoingSalt = false;
		OutgoingLabel = ChatLabel::Unknown;
		OutgoingRecipients = 0;
		GlobalPacket_NetMessage::Instance.ExtMagic = 0;
		return 0x55F056; // skip the send
	}

	// The message is about to go out: give it a fresh salt, for its ChatFingerprint, and say who it
	// is going to.
	HasOutgoingSalt = ChatScope != 0 && !ReplaySystem::IsPlaybackRequested()
		&& ReplaySystem::GenerateChatSalt(OutgoingSalt);
	ComputeRecipients(ChatScope, OutgoingLabel, OutgoingRecipients);

	auto& packet = GlobalPacket_NetMessage::Instance;
	packet.ExtMagic = ChatExtMagic;
	packet.HasSalt = HasOutgoingSalt;
	if (HasOutgoingSalt)
		memcpy(packet.Salt, OutgoingSalt, sizeof(OutgoingSalt));
	else
		memset(packet.Salt, 0, sizeof(packet.Salt));
	packet.Label = static_cast<uint8_t>(OutgoingLabel);
	packet.Recipients = OutgoingRecipients;

	return 0; // execute original: cmp edi, ebx; mov [esp+0x14], ebx
}

// After receiving a message, don't play sound if AddMessage returned NULL
// (i.e. the message was suppressed). Mirrors: hack 0x0048D97E in chat_disable.asm
DEFINE_HOOK(0x48D97E, NetworkCallBack_NetMessage_Sound, 0x5)
{
	if (!Spawner::Enabled)
		return 0;

	if (!R->EAX<void*>())
		return 0x48D99A; // skip sound

	return 0; // execute original: mov eax, [0x8871E0]
}

// In diplomacy dialog, make chat checkbox non-interactive for each player,
// matching the existing Player_MuteSWLaunches disabled-checkbox behavior.
// Replaces `mov eax, Player_MuteSWLaunches` (5 bytes at 0x657F8E).
// The subsequent `push 0; test eax, eax; jnz 0x657FC0` remains intact and
// branches to the disabled-checkbox path when EAX is non-zero.
DEFINE_HOOK(0x657F8E, RadarClass_Diplomacy_DisableChatToggleUI, 0x5)
{
	R->EAX(IsDisableChatEnabled() ? 1 : Unsorted::MuteSWLaunches);
	return 0;
}

// The non-interactive branch (loc_657FC0) sets BM_SETCHECK(1) before disabling.
// Force it back to OFF for DisableChat so visuals match the intended locked state.
DEFINE_HOOK(0x657FDB, RadarClass_Diplomacy_ForceDisabledChatVisualOff, 0x5)
{
	if (IsDisableChatEnabled())
	{
		GET(HWND, hWnd, EBP);
		SendMessageA(hWnd, BM_SETCHECK, BST_UNCHECKED, 0);
	}

	return 0;
}

// Continuously enforce DisableChat by resetting ChatMask every frame,
// preventing re-enabling chat from the alliance menu.
DEFINE_HOOK(0x55DDA5, MainLoop_AfterRender_DisableChat, 0x5)
{
	auto const Original = reinterpret_cast<int(__thiscall*)(void*)>(0x5D4430);
	GET(void*, pThis, ECX);
	Original(pThis);

	if (IsDisableChatEnabled())
	{
		for (int i = 0; i < 8; ++i)
			Game::ChatMask[i] = false;
	}

	return 0x55DDAA;
}

DEFINE_HOOK(0x48D92B, NetworkCallBack_NetMessage_Print, 0x5)
{
	if (!Spawner::Enabled)
		return 0;

	enum { SkipMessage = 0x48DAD3, PrintMessage = 0x48D937 };

	const int houseIndex = GlobalPacket_NetMessage::Instance.HouseIndex;
	HouseClass* pRecordHouse = (houseIndex < 8 && HouseClass::Array.ValidIndex(houseIndex))
		? HouseClass::Array.GetItem(houseIndex) : nullptr;

	const ChatLabel label = HasExtension()
		? InGameChat::ToLabel(GlobalPacket_NetMessage::Instance.Label) : ChatLabel::Unknown;
	const uint8_t recipients = HasExtension() ? GlobalPacket_NetMessage::Instance.Recipients : 0;

	// Recorded ahead of the DisableChat and ChatMask filters below, so a replay holds what was
	// sent rather than what this client chose to display
	if (pRecordHouse)
	{
		ReplaySystem::RecordChatMessage(houseIndex, pRecordHouse->UIName,
			GlobalPacket_NetMessage::Instance.Message, pRecordHouse->ColorSchemeIndex, ReceivedSalt(),
			label, recipients);
	}

	if (IsDisableChatEnabled())
		return SkipMessage;

	if (houseIndex < 8 && Game::ChatMask[houseIndex])
	{
		if (pRecordHouse)
		{
			GlobalPacket_NetMessage::Instance.Color = (byte)pRecordHouse->ColorSchemeIndex;
			InGameChat::ComposeSenderName(ReceivedSenderName, std::size(ReceivedSenderName), pRecordHouse->UIName, label);
			R->ESI(ReceivedSenderName);
			return PrintMessage;
		}
	}

	return SkipMessage;
}

DEFINE_HOOK(0x48D95B, NetworkCallBack_NetMessage_SetColor, 0x6)
{
	if (!Spawner::Enabled)
		return 0;

	R->EAX(R->ECX());
	return 0x48D966;
}

DEFINE_HOOK(0x55EDD2, MessageInput_Write, 0x5)
{
	if (!Spawner::Enabled)
		return 0;

	HouseClass* pHouse = HouseClass::CurrentPlayer;
	wcscpy_s(GlobalPacket_NetMessage::Instance.PlayerName, pHouse->UIName);
	GlobalPacket_NetMessage::Instance.HouseIndex = (byte)pHouse->ArrayIndex;

	return 0x55EE00;
}

DEFINE_HOOK(0x55F0A8, MessageInput_Print, 0x5)
{
	if (!Spawner::Enabled)
		return 0;

	// The local player's own message, which never comes back round through NetworkCallBack
	if (HouseClass* pHouse = HouseClass::CurrentPlayer)
	{
		if (HasOutgoingSalt)
			ReplaySystem::QueueChatFingerprint(OutgoingSalt, pHouse->ArrayIndex, GlobalPacket_NetMessage::Instance.Message);

		ReplaySystem::RecordChatMessage(pHouse->ArrayIndex, GlobalPacket_NetMessage::Instance.PlayerName,
			GlobalPacket_NetMessage::Instance.Message, pHouse->ColorSchemeIndex, HasOutgoingSalt ? OutgoingSalt : nullptr,
			OutgoingLabel, OutgoingRecipients);
	}

	InGameChat::ComposeSenderName(OutgoingSenderName, std::size(OutgoingSenderName),
		GlobalPacket_NetMessage::Instance.PlayerName, OutgoingLabel);

	HasOutgoingSalt = false;
	OutgoingLabel = ChatLabel::Unknown;
	OutgoingRecipients = 0;

	R->EAX(OutgoingSenderName);
	return 0x55F0B2;
}
