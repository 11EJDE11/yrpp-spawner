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
*
*  Modal "Synchronization Error" dialog shown when a multiplayer game goes out
*  of sync. The game master gets Continue/Quit; everyone else waits for the
*  master's decision. Both variants have a chat box. Game logic is halted while
*  the dialog is open, but the network is serviced so chat, sign-offs and the
*  decision still flow.
*/

#include "DesyncDialog.h"
#include "DesyncDialog.Resource.h"

#include <Ext/Session/Body.h>
#include <Spawner/Spawner.h>
#include <Spawner/GlobalPacketExt.h>
#include <Utilities/Debug.h>

#include <UI.h>
#include <OwnerDraw.h>
#include <SessionClass.h>
#include <HouseClass.h>
#include <EventClass.h>
#include <Surface.h>
#include <PCX.h>
#include <MapClass.h>
#include <ScenarioClass.h>
#include <IPX.h>
#include <IPXManagerClass.h>
#include <WWMouseClass.h>
#include <StringTable.h>
#include <Unsorted.h>
#include <MessageListClass.h>
#include <RulesClass.h>
#include <WWMessageBox.h>
#include <GameStrings.h>
#include <LoadOptionsClass.h>
#include <IPXManagerClass.h>
#include <ConnectionClass.h>
#include <CommBufferClass.h>
#include <IPXConnClass.h>
#include <WinsockInterfaceClass.h>

// Netcode pieces of this spawner that keep their own state across frames, and
// so must start over when a save is loaded mid-game.
#if __has_include(<Spawner/FrameGate.h>)
#include <Spawner/FrameGate.h>
#define DESYNC_RELOAD_FRAMEGATE
#endif
#if __has_include(<Spawner/FastRetransmit.h>)
#include <Spawner/FastRetransmit.h>
#define DESYNC_RELOAD_FASTRETRANSMIT
#endif
#if __has_include(<Spawner/PacketBundling.h>)
#include <Spawner/PacketBundling.h>
#define DESYNC_RELOAD_BUNDLING
#endif
#if __has_include(<Spawner/PacketRedundancy.h>)
#include <Spawner/PacketRedundancy.h>
#define DESYNC_RELOAD_REDUNDANCY
#endif
#if __has_include(<Replay/ReplaySystem.Internal.h>)
#include <Replay/ReplaySystem.Internal.h>
#define DESYNC_RELOAD_REPLAY
#endif
#if __has_include(<Spawner/KickAgreement.h>)
#include <Spawner/KickAgreement.h>
#define DESYNC_RELOAD_KICKAGREEMENT
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <windowsx.h>


DesyncDialogClass DesyncDialog;


namespace
{
	enum
	{
		HEARTBEAT_TIMER = 1,
		QUIT_ENABLE_TIMER = 2,
	};

	constexpr int HEARTBEAT_INTERVAL_MS = 1000;
	constexpr int HEARTBEAT_TIMEOUT_MS = 25000;
	constexpr int QUIT_ENABLE_DELAY_MS = 10000;
	constexpr int CHAT_BACKLOG_MAX = 50;

	// Chat message buffer size; matches the engine's global chat message field.
	constexpr int MAX_MESSAGE_LENGTH = 112;

	// Engine global-packet command for a player signing off.
	constexpr int NET_SIGN_OFF = 0xA;

	// The chat edit's hint text, from the string table (English fallback).
	const wchar_t* Chat_Edit_Placeholder()
	{
		return StringTable::TryFetchString("GUI:DesyncChatHint", L"Type here to chat...");
	}

	/**
	 *  Player list column positions, in pixels within the listbox client
	 *  area (the same units the game's lobby player lists use).
	 */
	constexpr int PLAYER_LIST_HOST_COL_X = 2;
	constexpr int PLAYER_LIST_NAME_COL_X = 20;
	constexpr int PLAYER_LIST_STATUS_COL_WIDTH = 56;

	/**
	 *  The x-position of the status column. Computed from the listbox's
	 *  actual size, because the dialog is not rescaled by the game and so
	 *  its pixel size depends on the system's font metrics.
	 */
	int Player_List_Status_Col_X(HWND list)
	{
		RECT rect {};
		GetClientRect(list, &rect);
		return rect.right - PLAYER_LIST_STATUS_COL_WIDTH;
	}

	/**
	 *  Converts a wide string to ANSI into the given buffer (NUL-terminated).
	 */
	void Wide_To_Ansi(const wchar_t* src, char* dst, int dst_size)
	{
		if (dst_size <= 0)
			return;

		const int written = WideCharToMultiByte(CP_ACP, 0, src ? src : L"", -1, dst, dst_size, nullptr, nullptr);
		if (written <= 0)
			dst[0] = '\0';
		else
			dst[dst_size - 1] = '\0';
	}

	/**
	 *  The local player's display name as ANSI.
	 */
	void Local_Player_Name(char* dst, int dst_size)
	{
		const wchar_t* name = L"";
		if (HouseClass::CurrentPlayer != nullptr)
			name = HouseClass::CurrentPlayer->UIName;
		Wide_To_Ansi(name, dst, dst_size);
	}

	/**
	 *  Sends one of our global packets to every other player in the session.
	 *  Addresses each peer exactly the way the engine's own in-game packets
	 *  (beacons) do: SessionClass::Players[i]->Address, with index 0 being the
	 *  local player.
	 */
	void Broadcast_Global_Packet(ExtGlobalPacketType& packet, int ack_req)
	{
		auto& players = NodeNameType::Array;
		for (int i = 1; i < players.Count; i++) {
			NodeNameType* node = players[i];
			if (node == nullptr)
				continue;

			IPXManagerClass::Instance.Send_Global_Message(
				&packet, sizeof(packet), ack_req,
				reinterpret_cast<IPXAddressClass*>(&node->Address), 0, 0);
		}
		IPXManagerClass::Instance.Service();
	}

	/**
	 *  What has to happen around loading a save mid-game, beyond the load.
	 */
	namespace Reload_Hooks
	{
		void Before_Load()
		{
#ifdef DESYNC_RELOAD_REPLAY
			// A recording cannot go back in time; it ends at the desync, which is what is wanted to
			// find out what went wrong. The load would start a new one over it, so recording stops.
			ReplaySystem::Internal::StopReplaySystem();
			Spawner::GetConfig()->EnableReplayRecording = false;
#endif
		}

		void After_Load()
		{
#ifdef DESYNC_RELOAD_FRAMEGATE
			// Its watermarks are frames of the timeline left behind.
			FrameGate::Reset();
#endif
#ifdef DESYNC_RELOAD_FASTRETRANSMIT
			FastRetransmit::Reset();
#endif
#ifdef DESYNC_RELOAD_BUNDLING
			PacketBundling::Reset();
#endif
#ifdef DESYNC_RELOAD_REDUNDANCY
			PacketRedundancy::Reset();
#endif
#ifdef DESYNC_RELOAD_KICKAGREEMENT
			// Its order history and cutoffs are frames of the timeline left behind.
			KickAgreement::Reset();
#endif

			// Loading replaces the surfaces but leaves Temp on the old hidden one, and
			// leaves the session's sound and input stopped.
			DSurface::Temp = DSurface::Hidden;
			SessionClass::Instance.Resume();

			if (Spawner::GetConfig()->AutoSaveInterval > 0) {
				Spawner::NextAutoSaveFrame = Unsorted::CurrentFrame + Spawner::GetConfig()->AutoSaveInterval;
			}
		}
	}

	/**
	 *  Tells the player in the in-game message list that someone was dropped
	 *  for going out of sync.
	 */
	void Print_Player_Removed(int house_id)
	{
		if (house_id < 0 || house_id >= HouseClass::Array.Count || HouseClass::Array[house_id] == nullptr) {
			return;
		}

		HouseClass* const house = HouseClass::Array[house_id];

		wchar_t message[160];
		std::swprintf(message, std::size(message),
			StringTable::TryFetchString("GUI:DesyncPlayerRemoved", L"%ls went out of sync and has been removed."),
			house->UIName);

		MessageListClass::Instance.PrintMessage(message, RulesClass::Instance->MessageDelay,
			house->ColorSchemeIndex, /* bSilent: */ false);
	}
}


/**
 *  Shows the dialog and pumps it until a decision has been made.
 *
 *  Game logic is halted for the duration: this function does not return
 *  until the master has decided what to do (or we have decided to quit).
 *  The network is serviced the whole time, so chat, sign-offs and the
 *  master's decision still come through.
 */
DesyncDialogOutcomeType DesyncDialogClass::Run()
{
	Debug::Log("DesyncDialog: opening on frame %d.\n", Unsorted::CurrentFrame);

	/**
	 *  Freeze game logic while the dialog is open: with Session.Suspended (which
	 *  is the same field the engine calls SystemResponseMessages) non-zero,
	 *  OwnerDraw::DialogMessageHandler services Call_Back() each iteration instead
	 *  of running Main_Loop(), so the world stays put while the network keeps
	 *  flowing. Lock user input so the dialog owns the keyboard and mouse.
	 */
	SessionClass::Instance.Suspended()++;
	const bool was_input_locked = Unsorted::UserInputLocked;
	Unsorted::UserInputLocked = true;

	/**
	 *  Hand the mouse to the dialog the way the engine's own in-game network
	 *  dialogs do (e.g. the reconnect/kick dialog, Wait_For_Players @0x648C7C):
	 *  release the captured software cursor so Windows draws the system cursor
	 *  over the dialog, and show it. Without this the game's software cursor
	 *  stays drawn on the frozen backbuffer (game logic is suspended) - a stale
	 *  duplicate cursor that doesn't move. Re-captured on close below.
	 */
	const bool was_mouse_captured = WWMouseClass::Instance->IsCaptured() != 0;
	if (was_mouse_captured) {
		WWMouseClass::Instance->ReleaseMouse();
	}
	WWMouseClass::Instance->ShowCursor();

	Decision = 0;
	ContinueReceived = false;
	std::fill(std::begin(PlayerLeft), std::end(PlayerLeft), false);
	std::fill(std::begin(LastHeartbeatFrom), std::end(LastHeartbeatFrom), std::chrono::steady_clock::now());
	ChatBacklog.clear();

	Create_Dialog();

	DesyncDialogOutcomeType outcome;

	if (Window == nullptr) {

		/**
		 *  If the dialog could not be created for whatever reason, fall back
		 *  to continuing without the desynced players.
		 */
		Debug::Log("DesyncDialog: failed to create the dialog!\n");
		outcome = DESYNC_OUTCOME_CONTINUE;

	} else while (true) {

		/**
		 *  Pump the dialog the way the engine runs its own in-game dialogs:
		 *  DialogMessageHandler dispatches input, repaints the dialog, and (while
		 *  we are suspended) services the network via Call_Back. If it ever
		 *  reports that the game itself has ended, bail out.
		 */
		if (UI::Updated() || QuitPending) {
			outcome = DESYNC_OUTCOME_QUIT;
			break;
		}

		Check_Heartbeat_Timeouts();
		Update_Chat_Placeholder();

		/**
		 *  The game master chose to go back to a save.
		 */
		if (Decision == IDC_DESYNC_LOAD && LoadDecidedFrame < 0) {
			const int save_frame = Selected_Save_Frame();
			if (save_frame >= 0) {
				Send_Load(save_frame);
				LoadDecidedFrame = save_frame;
				LoadDecidedAt = std::chrono::steady_clock::now();
				EnableWindow(GetDlgItem(Window, IDC_DESYNC_LOAD), FALSE);
				EnableWindow(GetDlgItem(Window, IDC_DESYNC_CONTINUE), FALSE);
			}
		}

		/**
		 *  Once all this player sent has been acknowledged, it is ready to load;
		 *  once everyone is (or has had long enough), the game master says to.
		 */
		if (LoadDecidedFrame >= 0 && PendingReloadFrame < 0) {
			if (!ReadySent && Everything_Sent_Acknowledged()) {
				ReadySent = true;
				if (!IsHostDialog) {
					Send_Ready();
				}
			}

			if (IsHostDialog && ReadySent
				&& (Everyone_Ready() || std::chrono::steady_clock::now() - LoadDecidedAt > std::chrono::seconds(10))) {
				Send_Go(LoadDecidedFrame);
				PendingReloadFrame = LoadDecidedFrame;
			}
		}

		if (PendingReloadFrame >= 0) {
			outcome = DESYNC_OUTCOME_LOAD;
			break;
		}

		if (Decision == IDC_DESYNC_QUIT) {
			outcome = DESYNC_OUTCOME_QUIT;
			break;
		}

		if (ContinueReceived || Decision == IDC_DESYNC_CONTINUE) {

			if (Decision == IDC_DESYNC_CONTINUE) {
				Send_Continue();
			}
			outcome = DESYNC_OUTCOME_CONTINUE;
			break;
		}

		Decision = 0;
	}

	Destroy_Dialog();

	Unsorted::UserInputLocked = was_input_locked;
	SessionClass::Instance.Suspended()--;

	// Give the mouse back to the game (re-capture the software cursor), matching
	// the open above.
	if (was_mouse_captured) {
		WWMouseClass::Instance->CaptureMouse();
	}

	MapClass::Instance.RedrawSidebar(2); // GScreenClass::Flag_To_Redraw @0x4F42F0: full redraw

	Debug::Log("DesyncDialog: closed with outcome %d.\n", static_cast<int>(outcome));
	return outcome;
}


/**
 *  Creates the dialog appropriate for the local player - the decision
 *  dialog for the game master, the wait dialog for everyone else.
 */
void DesyncDialogClass::Create_Dialog()
{
	IsHostDialog = SessionClass::Instance.Am_I_Master();

	/**
	 *  BeginDialog finds our template via the hooked Fetch_Resource, which
	 *  falls back to this DLL for resources the game's own resources don't
	 *  have, and registers the dialog with the message loop.
	 */
	Window = UI::BeginDialog(MAKEINTRESOURCE(IsHostDialog ? IDD_DESYNC_HOST : IDD_DESYNC_WAIT), &Dialog_Proc, 0);
	Debug::Log("DesyncDialog: BeginDialog returned %p (host=%d).\n", static_cast<void*>(Window), IsHostDialog);
	if (Window == nullptr) {
		return;
	}

	/**
	 *  The owner-draw framework lays the dialog out: because our ids are accepted
	 *  by TrySetDialogLayoutBand1 (hooked in DesyncDialog.Hook.cpp), WM_INITDIALOG
	 *  has already moved us full-screen, scaled the controls, and tagged the
	 *  element so OwnerDraw::Draw_Menu paints the in-game screen (BKGD*.SHP +
	 *  sidebar) behind us - the same treatment the engine's own full-screen
	 *  in-game dialogs get. So there's nothing to size or centre here.
	 *
	 *  Set up the player list columns: player name, host icon, status.
	 *  The name column must be added first: the listbox automatically
	 *  gives every new row a PRIMARY cell (which draws the row's string)
	 *  in the first column that was added, and INVALID cells in the rest.
	 */
	HWND list = GetDlgItem(Window, IDC_DESYNC_PLAYER_LIST);
	if (list != nullptr) {
		const int status_x = Player_List_Status_Col_X(list);
		SendMessage(list, WW_LB_ADDCOLUMN, status_x - PLAYER_LIST_NAME_COL_X - 6, PLAYER_LIST_NAME_COL_X);
		SendMessage(list, WW_LB_ADDCOLUMN, 0, PLAYER_LIST_HOST_COL_X);
		SendMessage(list, WW_LB_ADDCOLUMN, 0, status_x);
	}

	/**
	 *  The chat list is also a cell-based owner-draw listbox, so it needs a
	 *  column for the row text to land in. One full-width column is enough.
	 *  (wParam = column width, lParam = column x; see WW_LB_ADDCOLUMN.)
	 */
	HWND chat_list = GetDlgItem(Window, IDC_DESYNC_CHAT_LIST);
	if (chat_list != nullptr) {
		RECT chat_client {};
		GetClientRect(chat_list, &chat_client);
		// Full-width column so the chat text spans the same width as the chat
		// edit box below it (both are the full control width).
		SendMessage(chat_list, WW_LB_ADDCOLUMN, chat_client.right, 0);
	}

	Update_Player_List();

	if (IsHostDialog) {
		HWND save_list = GetDlgItem(Window, IDC_DESYNC_SAVE_LIST);
		if (save_list != nullptr) {
			RECT save_client {};
			GetClientRect(save_list, &save_client);
			SendMessage(save_list, WW_LB_ADDCOLUMN, save_client.right, 0);
		}
		Fill_Save_List();
		EnableWindow(GetDlgItem(Window, IDC_DESYNC_LOAD), Has_Save_To_Offer() ? TRUE : FALSE);
		EnableWindow(GetDlgItem(Window, IDC_DESYNC_CONTINUE), TRUE);
	} else {

		/**
		 *  The Quit button starts out disabled (so the player doesn't
		 *  instantly quit out of reflex) and is enabled after a delay.
		 */
		SetTimer(Window, QUIT_ENABLE_TIMER, QUIT_ENABLE_DELAY_MS, nullptr);
	}

	SetTimer(Window, HEARTBEAT_TIMER, HEARTBEAT_INTERVAL_MS, nullptr);

	Refill_Chat_List();

	ShowWindow(Window, SW_SHOWNORMAL);
	SetForegroundWindow(Window);

	/**
	 *  Seed the chat edit's hint text unless it already has focus; thereafter
	 *  Update_Chat_Placeholder (polled from the pump) clears it while the player
	 *  is typing and restores it when the box is left empty.
	 */
	HWND edit = GetDlgItem(Window, IDC_DESYNC_CHAT_EDIT);
	if (edit != nullptr && GetFocus() != edit) {
		SendMessage(edit, WW_SETTEXTW, 0, reinterpret_cast<LPARAM>(Chat_Edit_Placeholder()));
		ChatPlaceholderActive = true;
	}

	Debug::Log("DesyncDialog: Create_Dialog complete.\n");
}


/**
 *  Destroys the dialog.
 */
void DesyncDialogClass::Destroy_Dialog()
{
	if (Window != nullptr) {
		KillTimer(Window, HEARTBEAT_TIMER);
		UI::EndDialog(Window);
		Window = nullptr;
	}
}


/**
 *  If the game master has left and we have been promoted in their place,
 *  replace the wait dialog with the decision dialog.
 */
void DesyncDialogClass::Morph_To_Host_Dialog_If_Needed()
{
	if (!Is_Active() || IsHostDialog) {
		return;
	}

	if (!SessionClass::Instance.Am_I_Master()) {
		return;
	}

	Debug::Log("DesyncDialog: we are the new game master, switching to the decision dialog.\n");

	Destroy_Dialog();
	Create_Dialog();
}


/**
 *  Refills the player list with every player's name and status.
 */
void DesyncDialogClass::Update_Player_List()
{
	if (!Is_Active()) {
		return;
	}

	HWND list = GetDlgItem(Window, IDC_DESYNC_PLAYER_LIST);
	if (list == nullptr) {
		return;
	}

	ListBox_ResetContent(list);

	const int status_x = Player_List_Status_Col_X(list);
	const int host_id = SessionClass::Instance.MasterPlayerID();

	for (int i = 0; i < SessionExt::MaxPlayers && i < HouseClass::Array.Count; i++) {

		HouseClass* house = HouseClass::Array[i];
		if (house == nullptr || !house->IsHumanPlayer) {
			continue;
		}

		/**
		 *  The owner-draw listbox is cell-based; rows are added with the engine's
		 *  own WW_LB_ADDSTRINGW (the plain LB_ADDSTRING does not build the cell and
		 *  returns LB_ERR). The new row's PRIMARY cell - the row string - lands in
		 *  the first column added (the name column), and the host/status columns are
		 *  filled in below with WW_LB_SETCELLTEXT.
		 */
		const int row = static_cast<int>(SendMessage(list, WW_LB_ADDSTRINGW, 0, reinterpret_cast<LPARAM>(house->UIName)));
		if (row < 0) {
			continue;
		}

		/**
		 *  The game master gets the same host icon the lobby uses.
		 */
		if (i == host_id) {
			// WWUIListBoxCell holds WideWstring members whose default ctor is
			// explicit, so it cannot be brace-initialized; build it field by
			// field (the unset wide strings keep their null buffer, which the
			// framework's copy treats as empty).
			WWUIListBoxCell host_cell;
			host_cell.Format = WWUIListBoxCellFormat::Image;
			host_cell.TextColor = 0;
			PCX::Instance.LoadFile("wolhost.pcx");
			host_cell.Image = PCX::Instance.GetSurface("wolhost.pcx");
			host_cell.Value = 0;
			SendMessage(list, WW_LB_SETCELLTEXT, MAKEWPARAM(PLAYER_LIST_HOST_COL_X, row), reinterpret_cast<LPARAM>(&host_cell));
		}

		const wchar_t* status;
		COLORREF color;
		if (PlayerLeft[i]) {
			status = StringTable::TryFetchString("GUI:DesyncStatusQuit", L"Quit");
			color = RGB(200, 0, 0);
		} else if (SessionExt::Is_Out_of_Sync(i)) {
			status = StringTable::TryFetchString("GUI:DesyncStatusDesynced", L"Desynced");
			color = RGB(200, 200, 0);
		} else {
			status = StringTable::TryFetchString("GUI:DesyncStatusOK", L"OK");
			color = RGB(0, 200, 0);
		}

		WWUIListBoxCell status_cell;
		status_cell.Format = WWUIListBoxCellFormat::Text;
		status_cell.PrimaryText = status; // WideWstring, deep-copied by the message
		status_cell.TextColor = color;
		status_cell.Image = nullptr;
		status_cell.Value = 0;
		SendMessage(list, WW_LB_SETCELLTEXT, MAKEWPARAM(status_x, row), reinterpret_cast<LPARAM>(&status_cell));
	}

	InvalidateRect(list, nullptr, FALSE);
}


/**
 *  Refills the chat list from the backlog after the dialog is (re-)created.
 */
void DesyncDialogClass::Refill_Chat_List()
{
	if (!Is_Active()) {
		return;
	}

	HWND list = GetDlgItem(Window, IDC_DESYNC_CHAT_LIST);
	if (list == nullptr) {
		return;
	}

	ListBox_ResetContent(list);
	for (const auto& line : ChatBacklog) {
		SendMessage(list, WW_LB_ADDSTRINGA, 0, reinterpret_cast<LPARAM>(line.c_str()));
	}
	ListBox_SetTopIndex(list, ListBox_GetCount(list) - 1);
}


/**
 *  Appends a line to the chat list (and the backlog).
 */
void DesyncDialogClass::Append_Chat_Line(const char* line)
{
	ChatBacklog.emplace_back(line);
	if (static_cast<int>(ChatBacklog.size()) > CHAT_BACKLOG_MAX) {
		ChatBacklog.erase(ChatBacklog.begin());
	}

	if (!Is_Active()) {
		return;
	}

	HWND list = GetDlgItem(Window, IDC_DESYNC_CHAT_LIST);
	if (list == nullptr) {
		return;
	}

	SendMessage(list, WW_LB_ADDSTRINGA, 0, reinterpret_cast<LPARAM>(line));
	while (ListBox_GetCount(list) > CHAT_BACKLOG_MAX) {
		ListBox_DeleteString(list, 0);
	}
	ListBox_SetTopIndex(list, ListBox_GetCount(list) - 1);
}


/**
 *  Sends the message currently in the chat edit box to the other players.
 *
 *  The in-game chat UI is not running while the game is frozen, so the dialog
 *  carries its own chat over a dedicated global packet and echoes it locally.
 */
void DesyncDialogClass::Send_Chat()
{
	if (!Is_Active()) {
		return;
	}

	HWND edit = GetDlgItem(Window, IDC_DESYNC_CHAT_EDIT);
	if (edit == nullptr) {
		return;
	}

	/**
	 *  The box is showing its hint text, not a real message - nothing to send.
	 */
	if (ChatPlaceholderActive) {
		return;
	}

	/**
	 *  The chat edit is an owner-draw NewEdit: it keeps its text in the engine's
	 *  own buffer rather than the Win32 window text, so read and clear it with the
	 *  WW_*TEXT messages, not Get/SetWindowText.
	 */
	char buf[MAX_MESSAGE_LENGTH];
	buf[0] = '\0';
	SendMessage(edit, WW_GETTEXTA, sizeof(buf), reinterpret_cast<LPARAM>(buf));
	if (buf[0] == '\0') {
		return;
	}

	SendMessage(edit, WW_SETTEXTW, 0, reinterpret_cast<LPARAM>(L""));

	/**
	 *  Repaint the edit right away. The owner-draw edit only redraws itself on
	 *  a text change when it has focus, so clearing it from the Send button
	 *  (where the button, not the edit, is focused) would otherwise leave the
	 *  old text on screen until the next pump cycle. Pressing Enter doesn't hit
	 *  this because the edit is already focused.
	 */
	InvalidateRect(edit, nullptr, TRUE);
	UpdateWindow(edit);

	SetFocus(edit);

	ExtGlobalPacketType packet {};
	packet.Command = EXT_NET_DESYNC_CHAT;
	Local_Player_Name(packet.Name, sizeof(packet.Name));
	if (HouseClass::CurrentPlayer != nullptr)
		packet.Chat.SenderHouseID = static_cast<char>(HouseClass::CurrentPlayer->ArrayIndex);
	std::strncpy(packet.Chat.Text, buf, sizeof(packet.Chat.Text) - 1);
	Broadcast_Global_Packet(packet, 1);

	/**
	 *  Show our own message locally (the broadcast only reaches the others).
	 */
	Notify_Chat(packet.Name, buf);
}


/**
 *  Manages the hint text in the chat edit box: the hint is shown while the box
 *  is empty and unfocused, and cleared once the player clicks into it.
 *
 *  Polled from the pump loop: the owner-draw NewEdit keeps its text in the
 *  engine's own buffer (the WW_*TEXT messages, not the Win32 window text) and
 *  sends no focus notifications, so there is nothing to drive this off events.
 *  We track focus with GetFocus instead.
 */
void DesyncDialogClass::Update_Chat_Placeholder()
{
	if (!Is_Active()) {
		return;
	}

	HWND edit = GetDlgItem(Window, IDC_DESYNC_CHAT_EDIT);
	if (edit == nullptr) {
		return;
	}

	if (GetFocus() == edit) {
		if (ChatPlaceholderActive) {
			SendMessage(edit, WW_SETTEXTW, 0, reinterpret_cast<LPARAM>(L""));
			ChatPlaceholderActive = false;
			InvalidateRect(edit, nullptr, TRUE);
		}
	} else if (!ChatPlaceholderActive) {
		char buf[MAX_MESSAGE_LENGTH];
		buf[0] = '\0';
		SendMessage(edit, WW_GETTEXTA, sizeof(buf), reinterpret_cast<LPARAM>(buf));
		if (buf[0] == '\0') {
			SendMessage(edit, WW_SETTEXTW, 0, reinterpret_cast<LPARAM>(Chat_Edit_Placeholder()));
			ChatPlaceholderActive = true;
			InvalidateRect(edit, nullptr, TRUE);
		}
	}
}


/**
 *  Lets the other players know we are still alive while game logic is halted.
 *  This both keeps the connections (and any NAT mappings) warm and lets
 *  everyone detect players that have silently disappeared.
 */
void DesyncDialogClass::Send_Heartbeat()
{
	/**
	 *  Guard against being called while the world is in an inconsistent state
	 *  (e.g. a save load tearing down and rebuilding the player list under us).
	 */
	if (HouseClass::CurrentPlayer == nullptr || NodeNameType::Array.Count == 0) {
		return;
	}

	ExtGlobalPacketType packet {};
	packet.Command = EXT_NET_DESYNC_HEARTBEAT;
	Local_Player_Name(packet.Name, sizeof(packet.Name));
	packet.Heartbeat.HouseID = static_cast<char>(HouseClass::CurrentPlayer->ArrayIndex);
	packet.Heartbeat.IsHost = SessionClass::Instance.Am_I_Master() ? 1 : 0;

	Broadcast_Global_Packet(packet, 0);
}


/**
 *  Broadcasts the master's decision to continue without the desynced players.
 */
void DesyncDialogClass::Send_Continue()
{
	Debug::Log("DesyncDialog: broadcasting the decision to continue.\n");

	ExtGlobalPacketType packet {};
	packet.Command = EXT_NET_DESYNC_CONTINUE;
	Local_Player_Name(packet.Name, sizeof(packet.Name));

	Broadcast_Global_Packet(packet, 1);
}


/**
 *  Drops players we have not heard from in a long while (e.g. their game has
 *  crashed without sending a sign-off). This keeps the dialog from waiting on
 *  a dead master forever, and removes dead players from the player list so a
 *  subsequent save load reconciles them to the AI cleanly.
 */
void DesyncDialogClass::Check_Heartbeat_Timeouts()
{
	const auto now = std::chrono::steady_clock::now();

	auto& players = NodeNameType::Array;
	for (int i = players.Count - 1; i >= 1; i--) {

		NodeNameType* node = players[i];
		if (node == nullptr) {
			continue;
		}

		const int id = node->HouseIndex;
		if (id < 0 || id >= SessionExt::MaxPlayers) {
			continue;
		}

		if (now - LastHeartbeatFrom[id] > std::chrono::milliseconds(HEARTBEAT_TIMEOUT_MS)) {
			Debug::Log("DesyncDialog: no heartbeat from house %d for %d seconds, dropping them.\n", id, HEARTBEAT_TIMEOUT_MS / 1000);

			/**
			 *  A non-zero error makes Destroy_Connection remove the player
			 *  via a queued remove-player event rather than an immediate AI
			 *  takeover, and print a "connection lost" message.
			 */
			SessionClass::Destroy_Connection(id, 1);
			SessionExt::Update_Master_After_Player_Removal();
			Notify_Player_Left(id);
		}
	}
}


/**
 *  Broadcasts a sign-off so the other players drop our connection.
 */
void DesyncDialogClass::Send_Sign_Off()
{
	Debug::Log("DesyncDialog: signing off.\n");

	ExtGlobalPacketType packet {};
	packet.Command = NET_SIGN_OFF;
	Local_Player_Name(packet.Name, sizeof(packet.Name));

	/**
	 *  Send twice for good measure, since these are not acked.
	 */
	Broadcast_Global_Packet(packet, 0);
	Broadcast_Global_Packet(packet, 0);
}


/**
 *  Detects per-player desync for the current frame and, if any player has
 *  diverged, applies the outcome: with a save from before the desync, the
 *  dialog lets the game master take everyone back to it; without one, the
 *  players who disagree with the majority are dropped straight away.
 *
 *  Called from the Execute_DoList entry hook (which gates the engine's
 *  start-of-game CRC-skip window). Replaces the engine's stock
 *  message-box-and-quit: on "continue" the desynced players are dropped and
 *  their events kept out of the frame by the out-of-sync skip hook.
 */
DesyncActionType DesyncDialogClass::Check_And_Handle_Desync()
{
	if (!SessionClass::IsMultiplayer()) {
		return DESYNC_ACTION_PROCEED;
	}

	/**
	 *  Never re-enter while the dialog is already open. If the pump's Call_Back
	 *  ever re-ran the frame logic and reached Execute_DoList again, this would
	 *  otherwise recurse into Run() and overflow the stack.
	 */
	if (Is_Active()) {
		Debug::Log("DesyncDialog: re-entrant desync check ignored (dialog already open).\n");
		return DESYNC_ACTION_PROCEED;
	}

	// The others went back to a save this player couldn't load.
	if (QuitPending) {
		Send_Sign_Off();
		WWMessageBox::Instance.Process(
			StringTable::TryFetchString("GUI:DesyncReloadFailed",
				L"The game went out of sync, and this player couldn't go back to the save the others went back to."),
			StringTable::LoadString(GameStrings::TXT_OK), nullptr);
		return DESYNC_ACTION_QUIT;
	}

	// A load is already on its way; the frame is thrown away with the timeline.
	if (Has_Pending_Reload()) {
		return DESYNC_ACTION_SKIP_FRAME;
	}

	/**
	 *  Detect desync the same way the engine does: for each FRAMEINFO event
	 *  scheduled this frame, compare the frame's reported CRC against ours.
	 *  Every machine compares the same FRAMEINFOs, so grouping the players by
	 *  the CRC they reported gives every machine the same answer as to which
	 *  players agree, without any further messages.
	 */
	bool newly_desynced = false;
	EventClass* offending_event = nullptr;
	const unsigned int current_frame = static_cast<unsigned int>(Unsorted::CurrentFrame);

	/**
	 *  A FRAMEINFO carries the CRC of the frame its sender was on, the frame it
	 *  is executed on less the sender's MaxAhead, which can differ between
	 *  players for a while after the timing changes. So each player is compared
	 *  with our own CRC of that frame, as the engine does; players who disagree
	 *  with us are compared with each other only when they report the same frame.
	 */
	bool has_crc[SessionExt::MaxPlayers] = {};
	bool agrees[SessionExt::MaxPlayers] = {};
	int crc_frame[SessionExt::MaxPlayers] = {};
	unsigned int house_crc[SessionExt::MaxPlayers] = {};

	for (int i = 0; i < EventClass::DoList.Count; i++) {

		EventClass& event = EventClass::DoList[i];

		if (event.Type != EventType::FrameInfo || event.Frame != current_frame) {
			continue;
		}

		const int id = event.HouseIndex;
		if (id < 0 || id >= SessionExt::MaxPlayers || SessionExt::Is_Out_of_Sync(id)) {
			continue;
		}

		const int index = static_cast<int>((event.Frame - event.FrameInfo.Delay) & 0xFF);
		has_crc[id] = true;
		crc_frame[id] = index;
		house_crc[id] = event.FrameInfo.CRC;
		agrees[id] = EventClass::LatestFramesCRC[index] == event.FrameInfo.CRC;

		if (!agrees[id]) {
			newly_desynced = true;
			if (offending_event == nullptr) {
				offending_event = &event;
			}
		}
	}

	if (!newly_desynced) {
		return DESYNC_ACTION_PROCEED;
	}

	Debug::Log("DesyncDialog: desync detected on frame %u.\n", current_frame);
	for (int i = 0; i < EventClass::DoList.Count; i++) {
		const EventClass& event = EventClass::DoList[i];
		if (event.Type == EventType::FrameInfo && event.Frame == current_frame) {
			const int index = static_cast<int>((event.Frame - event.FrameInfo.Delay) & 0xFF);
			Debug::Log("DesyncDialog:   house %d delay %d: CRC %08X, ours %08X%s\n", event.HouseIndex,
				static_cast<int>(event.FrameInfo.Delay), event.FrameInfo.CRC, EventClass::LatestFramesCRC[index],
				SessionExt::Is_Out_of_Sync(event.HouseIndex) ? " (already out of sync)" : "");
		}
	}
	DesyncFrame = static_cast<int>(current_frame);

	/**
	 *  Out of sync again soon after going back to a save: that save leads to a
	 *  desync too, so the next one back is offered instead.
	 */
	if (LastReloadFrame >= 0) {
		const int window = (std::max)(Spawner::GetConfig()->AutoSaveInterval, 900);
		if (DesyncFrame <= LastReloadDesyncFrame + window && !Is_Failed_Save(LastReloadFrame)) {
			Debug::Log("DesyncDialog: out of sync again after loading the save from frame %d.\n", LastReloadFrame);
			FailedSaveFrames.push_back(LastReloadFrame);
		}
		LastReloadFrame = -1;
	}

	const int own_id = HouseClass::CurrentPlayer ? HouseClass::CurrentPlayer->ArrayIndex : -1;

	/**
	 *  The largest group of players that agree is the game that goes on. If
	 *  there is one, the players outside it are dropped by everyone in it, and
	 *  each of them drops out itself rather than playing on alone. If there is
	 *  no single largest group (a 1v1, or an even split), each group carries on
	 *  without the players it disagrees with.
	 */
	int own_group = 1;
	int largest_other = 0;
	for (int id = 0; id < SessionExt::MaxPlayers; id++) {
		if (!has_crc[id] || id == own_id) {
			continue;
		}

		if (agrees[id]) {
			own_group++;
			continue;
		}

		int count = 0;
		for (int other = 0; other < SessionExt::MaxPlayers; other++) {
			if (has_crc[other] && other != own_id && !agrees[other]
				&& crc_frame[other] == crc_frame[id] && house_crc[other] == house_crc[id]) {
				count++;
			}
		}
		largest_other = (std::max)(largest_other, count);

		// Whatever happens next, the engine must not compare its frames with ours.
		SessionExt::Mark_Player_As_Out_of_Sync(id);
	}

	const bool out_of_majority = largest_other > own_group;
	Debug::Log("DesyncDialog: %d players agree with this one, the largest other group is %d; this player is %s.\n",
		own_group, largest_other, out_of_majority ? "outside the majority"
			: own_group > largest_other ? "in the majority" : "in an even split");

	/**
	 *  Write the engine's sync-debug dump (SYNC*.TXT) for the offending event,
	 *  exactly as the stock out-of-sync handler does (Execute_DoList @0x64CC68).
	 *  Our entry hook intercepts before that handler runs, so reproduce it here
	 *  or desyncs become impossible to diagnose.
	 *
	 *  The dump draws a number from the scenario's random number generator to
	 *  print it ("My Random Number"). The stock handler quits right after, but
	 *  this game goes on, so the draw would become part of it, and a replay,
	 *  which writes no dump, would go its own way from here. Put the generator
	 *  back as it was.
	 */
	if (offending_event != nullptr) {
		const Randomizer random = ScenarioClass::Instance->Random;

		if (Game::EnableMPSyncDebug) {
			for (int slot = 0; slot < 256; slot++) {
				EventClass::Print_CRCs_All_Players(slot, offending_event);
			}
		} else {
			EventClass::Print_CRCs_Current_Player(offending_event);
		}

		ScenarioClass::Instance->Random = random;
	}

	/**
	 *  With a save to go back to, everyone - out of sync or not - waits for
	 *  the game master to choose. Without one, carry on without the players
	 *  who disagree with the majority.
	 */
	const DesyncDialogOutcomeType outcome = Has_Save_To_Offer() ? Run() : DESYNC_OUTCOME_CONTINUE;

	if (outcome == DESYNC_OUTCOME_CONTINUE && out_of_majority) {
		/**
		 *  The others agree with each other and not with us: they are dropping
		 *  us, so leave rather than play on alone.
		 */
		Send_Sign_Off();
		WWMessageBox::Instance.Process(
			StringTable::TryFetchString("GUI:DesyncYouWereRemoved",
				L"Your game went out of sync with the other players, and you have been removed from it."),
			StringTable::LoadString(GameStrings::TXT_OK), nullptr);
		return DESYNC_ACTION_QUIT;
	}

	switch (outcome) {

	case DESYNC_OUTCOME_LOAD:
		/**
		 *  Throw this frame away; the save is loaded at the top of the next
		 *  main loop, ahead of the network handshake a loaded game starts with.
		 */
		return DESYNC_ACTION_SKIP_FRAME;

	case DESYNC_OUTCOME_CONTINUE:
		/**
		 *  Continue without the out-of-sync players. Destroy_Connection queues
		 *  their removal; the out-of-sync skip hook keeps their events (and
		 *  FRAMEINFO) out of the executing DoList this frame so the game resumes
		 *  cleanly. Symmetric: the desynced players drop us the same way.
		 */
		for (int id = 0; id < SessionExt::MaxPlayers; id++) {
			if (SessionExt::Is_Out_of_Sync(id) && !PlayerRemoved[id]) {
				PlayerRemoved[id] = true;
				SessionClass::Destroy_Connection(id, -1);
				Print_Player_Removed(id);
			}
		}
		SessionExt::Update_Master_After_Player_Removal();
		return DESYNC_ACTION_PROCEED;

	case DESYNC_OUTCOME_QUIT:
	default:
		/**
		 *  Sign off so the others drop us, then let the caller stop the game
		 *  (the Execute_DoList hook returns failure to it).
		 */
		Send_Sign_Off();
		return DESYNC_ACTION_QUIT;
	}
}


/**
 *  Appends a chat message to the dialog's chat list.
 */
void DesyncDialogClass::Notify_Chat(const char* name, const char* text)
{
	if (!Is_Active()) {
		return;
	}

	char buf[256];
	std::snprintf(buf, std::size(buf), "%s: %s", name, text);
	Append_Chat_Line(buf);
}


/**
 *  Called when a player has left the game (signed off or timed out)
 *  while the dialog was open.
 */
void DesyncDialogClass::Notify_Player_Left(int house_id)
{
	if (!Is_Active()) {
		return;
	}

	if (house_id >= 0 && house_id < SessionExt::MaxPlayers) {
		PlayerLeft[house_id] = true;
	}

	if (house_id >= 0 && house_id < HouseClass::Array.Count && HouseClass::Array[house_id] != nullptr) {
		char name[64];
		Wide_To_Ansi(HouseClass::Array[house_id]->UIName, name, sizeof(name));
		char buf[128];
		std::snprintf(buf, std::size(buf), "%s has left the game.", name);
		Append_Chat_Line(buf);
	}

	Update_Player_List();

	/**
	 *  If the master is the one who left, SessionExt::Update_Master_After_Player_Removal
	 *  (run by our callers before this) has already promoted a replacement; if it
	 *  promoted us, switch to the host dialog.
	 */
	Morph_To_Host_Dialog_If_Needed();
}


/**
 *  Called when the master has decided to continue the game without
 *  the desynced players.
 */
void DesyncDialogClass::Notify_Continue()
{
	if (!Is_Active()) {
		return;
	}

	Debug::Log("DesyncDialog: the game master has chosen to continue.\n");
	ContinueReceived = true;
}


/**
 *  Called when a heartbeat has been received from another player.
 */
void DesyncDialogClass::Notify_Heartbeat(int house_id, bool is_host)
{
	if (!Is_Active()) {
		return;
	}

	if (house_id < 0 || house_id >= SessionExt::MaxPlayers) {
		return;
	}

	LastHeartbeatFrom[house_id] = std::chrono::steady_clock::now();

	/**
	 *  Self-heal the master fields in case we missed the announcement,
	 *  and move the host icon in the player list to the right row.
	 */
	if (is_host && SessionClass::Instance.MasterPlayerID() == -1) {
		SessionExt::Set_Master(house_id);
		Update_Player_List();
	}
}


/**
 *  Routes a received spawner global packet to the right notification.
 *  Returns true if the packet was one of ours and was consumed.
 *
 *  This is the receive side. It is ready to be called from the engine's global
 *  packet dispatch (Network_Call_Back, 0x48D1E0): commands the engine does not
 *  recognise fall through to its default case (~0x48DACE -> Process_Global_Packet),
 *  which is where this should be spliced in. The hook itself is left to be wired
 *  in once the exact splice point/registers are confirmed, e.g.:
 *
 *      DEFINE_HOOK(0x48DACE, NetworkCallBack_DesyncPacket, <size>)
 *      {
 *          GET(ExtGlobalPacketType*, packet, <reg for &GPacket>);
 *          GET(IPXAddressClass*, address, <reg for &GAddress>);
 *          if (DesyncDialog.Handle_Global_Packet(packet, address))
 *              return <address after the dispatch switch>;
 *          return 0;
 *      }
 *
 *  Player departures (sign-offs) are likewise routed by hooking the engine's
 *  Destroy_Connection (0x5DA750) to also call DesyncDialog.Notify_Player_Left(id).
 */
bool DesyncDialogClass::Handle_Global_Packet(const ExtGlobalPacketType* packet, const IPXAddressClass* address)
{
	(void)address;

	if (packet == nullptr) {
		return false;
	}

	switch (packet->Command) {
	case EXT_NET_HOST_ANNOUNCE:
		// Sent by the host at game start (before any dialog is open), so record
		// the master unconditionally; this is what makes MasterPlayerID
		// authoritative everywhere - including for the dialog's host icon.
		SessionExt::Set_Master(packet->Heartbeat.HouseID);
		return true;

	case EXT_NET_DESYNC_HEARTBEAT:
		Notify_Heartbeat(packet->Heartbeat.HouseID, packet->Heartbeat.IsHost != 0);
		return true;

	case EXT_NET_DESYNC_CONTINUE:
		Notify_Continue();
		return true;

	case EXT_NET_DESYNC_CHAT:
		Notify_Chat(packet->Name, packet->Chat.Text);
		return true;

	case EXT_NET_DESYNC_LOAD:
		Notify_Load(packet->Load.SaveFrame, packet->Load.DesyncFrame);
		return true;

	case EXT_NET_DESYNC_READY:
		Notify_Ready(packet->Heartbeat.HouseID);
		return true;

	case EXT_NET_DESYNC_GO:
		Notify_Go(packet->Load.SaveFrame);
		return true;

	default:
		return false;
	}
}


/**
 *  The window procedure for both dialog variants.
 */
BOOL CALLBACK DesyncDialogClass::Dialog_Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	/**
	 *  Let the owner-draw framework do all of its standard handling first: it
	 *  subclasses the controls and lays out the dialog on WM_INITDIALOG, paints
	 *  the dialog background, draws the owner-draw controls (WM_DRAWITEM),
	 *  colours the controls, and tears the dialog down on WM_DESTROY. If it
	 *  consumed the message, we are done. (This is exactly how the engine's own
	 *  in-game dialogs, e.g. the diplomacy dialog, are written.)
	 */
	if (LRESULT handled = UI::StandardWndProc(window, message, wparam, lparam)) {
		return static_cast<BOOL>(handled);
	}

	switch (message) {

		case WM_MOVING:
			// Game::OnWindowMoving clamps the drag rect (lParam) to the screen.
			Game::OnWindowMoving(reinterpret_cast<tagRECT*>(lparam));
			return TRUE;

		case WM_TIMER:

			/**
			 *  Heartbeats are sent from a timer rather than the pump loop,
			 *  so that they keep flowing while a nested dialog runs its own
			 *  message loop.
			 */
			if (wparam == HEARTBEAT_TIMER) {
				DesyncDialog.Send_Heartbeat();
			} else if (wparam == QUIT_ENABLE_TIMER) {
				EnableWindow(GetDlgItem(window, IDC_DESYNC_QUIT), TRUE);
				KillTimer(window, QUIT_ENABLE_TIMER);
			}
			break;

		case WW_EDIT_ENTERPRESSED:
			/**
			 *  The owner-draw chat edit reports Enter through this message
			 *  (lParam = the edit's HWND), not as an IDOK WM_COMMAND.
			 */
			if (reinterpret_cast<HWND>(lparam) == GetDlgItem(window, IDC_DESYNC_CHAT_EDIT)) {
				DesyncDialog.Send_Chat();
				return TRUE;
			}
			break;

		case WM_COMMAND:
			switch (LOWORD(wparam)) {

				case IDC_DESYNC_CHAT_SEND:
					DesyncDialog.Send_Chat();
					return TRUE;

				case IDC_DESYNC_CONTINUE:
				case IDC_DESYNC_QUIT:
				case IDC_DESYNC_LOAD:
					DesyncDialog.Decision = LOWORD(wparam);
					break;
			}
			break;
	}

	return FALSE;
}


/**
 *  Records a multiplayer save just made. Every player saves on the same frame,
 *  and the file is whichever SVGM_*.NET was written last (Phobos numbers them).
 */
void DesyncDialogClass::Note_Save_Made(int frame, bool saved)
{
	/**
	 *  A save that failed here is not offered, and if the game master picks it
	 *  this player leaves (see Notify_Go). That it succeeded is all that is
	 *  checked: a check on the file's age could fail on one machine and not on
	 *  another, and split a reload the same way.
	 */
	if (!saved) {
		Debug::Log("DesyncDialog: the save on frame %d failed.\n", frame);
		return;
	}

	char pattern[MAX_PATH];
	std::snprintf(pattern, std::size(pattern), "%s\\SVGM_*.NET", Spawner::GetConfig()->SavedGameDir);

	WIN32_FIND_DATAA found {};
	HANDLE search = FindFirstFileA(pattern, &found);
	if (search == INVALID_HANDLE_VALUE) {
		return;
	}

	std::string newest;
	FILETIME newest_time {};
	do {
		if (CompareFileTime(&found.ftLastWriteTime, &newest_time) > 0) {
			newest_time = found.ftLastWriteTime;
			newest = found.cFileName;
		}
	} while (FindNextFileA(search, &found));
	FindClose(search);

	if (newest.empty()) {
		return;
	}

	Saves.erase(std::remove_if(Saves.begin(), Saves.end(),
		[frame](const SaveEntry& save) { return save.Frame >= frame; }), Saves.end());
	Saves.push_back({ frame, newest });
	Debug::Log("DesyncDialog: noted the save %s made on frame %d.\n", newest.c_str(), frame);
}


/**
 *  Records the save the game was started from.
 */
void DesyncDialogClass::Note_Save_Loaded(const char* path, int frame)
{
	if (path == nullptr || path[0] == '\0') {
		return;
	}

	Saves.clear();
	Saves.push_back({ frame, path });
}


/**
 *  Whether there is a save from before the desync to offer loading.
 */
bool DesyncDialogClass::Has_Save_To_Offer() const
{
	return Preferred_Save_Index() >= 0;
}


/**
 *  The save to offer first: the newest one from before the desync that has not
 *  gone out of sync again after being loaded.
 */
int DesyncDialogClass::Preferred_Save_Index() const
{
	int newest = -1;
	for (int i = 0; i < static_cast<int>(Saves.size()); i++) {
		if (Saves[i].Frame < DesyncFrame && !Is_Failed_Save(Saves[i].Frame)) {
			newest = i;
		}
	}

	return newest;
}


/**
 *  Fills the host dialog's save list, newest first, with the preferred save
 *  selected.
 */
void DesyncDialogClass::Fill_Save_List()
{
	if (!Is_Active()) {
		return;
	}

	HWND list = GetDlgItem(Window, IDC_DESYNC_SAVE_LIST);
	if (list == nullptr) {
		return;
	}

	ListBox_ResetContent(list);

	const int preferred = Preferred_Save_Index();
	int selected_row = -1;

	for (int i = static_cast<int>(Saves.size()) - 1; i >= 0; i--) {
		if (Saves[i].Frame >= DesyncFrame) {
			continue;
		}

		// Game time at the game's frame rate, and how far back from the desync it is.
		const int seconds = Saves[i].Frame / 60;
		const int back = (DesyncFrame - Saves[i].Frame) / 60;
		wchar_t text[96];
		std::swprintf(text, std::size(text),
			StringTable::TryFetchString("GUI:DesyncSaveEntry", L"%02d:%02d game time (%d s back)"),
			seconds / 60, seconds % 60, back);

		const int row = static_cast<int>(SendMessage(list, WW_LB_ADDSTRINGW, 0, reinterpret_cast<LPARAM>(text)));
		if (row >= 0) {
			ListBox_SetItemData(list, row, static_cast<LPARAM>(Saves[i].Frame));
			if (i == preferred) {
				selected_row = row;
			}
		}
	}

	if (selected_row >= 0) {
		ListBox_SetCurSel(list, selected_row);
	}
}


/**
 *  The frame of the save selected in the host dialog, or of the preferred save
 *  if the list has no selection; -1 for none.
 */
int DesyncDialogClass::Selected_Save_Frame() const
{
	HWND list = Is_Active() ? GetDlgItem(Window, IDC_DESYNC_SAVE_LIST) : nullptr;
	if (list != nullptr) {
		const int row = ListBox_GetCurSel(list);
		if (row >= 0) {
			return static_cast<int>(ListBox_GetItemData(list, row));
		}
	}

	const int preferred = Preferred_Save_Index();
	return preferred >= 0 ? Saves[preferred].Frame : -1;
}


/**
 *  Broadcasts the master's decision to go back to a save.
 */
void DesyncDialogClass::Send_Load(int save_frame)
{
	Debug::Log("DesyncDialog: broadcasting the decision to load the save from frame %d.\n", save_frame);

	ExtGlobalPacketType packet {};
	packet.Command = EXT_NET_DESYNC_LOAD;
	Local_Player_Name(packet.Name, sizeof(packet.Name));
	packet.Load.SaveFrame = save_frame;
	packet.Load.DesyncFrame = DesyncFrame;

	Broadcast_Global_Packet(packet, 1);
}


/**
 *  Called when the game master has chosen to go back to a save. Taken even if
 *  this player's dialog is not open yet: it is loaded between frames all the
 *  same, and the handshake after the load brings everyone together.
 */
void DesyncDialogClass::Notify_Load(int save_frame, int desync_frame)
{
	Debug::Log("DesyncDialog: the game master chose the save from frame %d (desync on frame %d).\n",
		save_frame, desync_frame);

	const bool have_it = std::any_of(Saves.begin(), Saves.end(),
		[save_frame](const SaveEntry& save) { return save.Frame == save_frame; });

	if (!have_it) {
		Debug::Log("DesyncDialog: this player has no save from frame %d; leaving.\n", save_frame);
		QuitPending = true;
		return;
	}

	if (LoadDecidedFrame < 0) {
		LoadDecidedFrame = save_frame;
		LoadDecidedAt = std::chrono::steady_clock::now();
	}
}


/**
 *  Tells the game master that this player has nothing unacknowledged in flight.
 */
void DesyncDialogClass::Send_Ready()
{
	if (HouseClass::CurrentPlayer == nullptr) {
		return;
	}

	Debug::Log("DesyncDialog: ready to load.\n");

	ExtGlobalPacketType packet {};
	packet.Command = EXT_NET_DESYNC_READY;
	Local_Player_Name(packet.Name, sizeof(packet.Name));
	packet.Heartbeat.HouseID = static_cast<char>(HouseClass::CurrentPlayer->ArrayIndex);

	Broadcast_Global_Packet(packet, 1);
}


/**
 *  Tells everyone to load the save now.
 */
void DesyncDialogClass::Send_Go(int save_frame)
{
	Debug::Log("DesyncDialog: telling everyone to load the save from frame %d.\n", save_frame);

	ExtGlobalPacketType packet {};
	packet.Command = EXT_NET_DESYNC_GO;
	Local_Player_Name(packet.Name, sizeof(packet.Name));
	packet.Load.SaveFrame = save_frame;
	packet.Load.DesyncFrame = DesyncFrame;

	Broadcast_Global_Packet(packet, 1);
}


void DesyncDialogClass::Notify_Ready(int house_id)
{
	if (house_id >= 0 && house_id < SessionExt::MaxPlayers) {
		ReadyFrom[house_id] = true;
	}
}


/**
 *  The game master says to load. Taken even if this player never opened the
 *  dialog; it is loaded between frames all the same.
 */
void DesyncDialogClass::Notify_Go(int save_frame)
{
	Debug::Log("DesyncDialog: told to load the save from frame %d.\n", save_frame);

	const bool have_it = std::any_of(Saves.begin(), Saves.end(),
		[save_frame](const SaveEntry& save) { return save.Frame == save_frame; });

	if (have_it) {
		PendingReloadFrame = save_frame;
	} else {
		Debug::Log("DesyncDialog: this player has no save from frame %d; leaving.\n", save_frame);
		QuitPending = true;
	}
}


/**
 *  Whether everything this player has sent on its connections has been
 *  acknowledged, so that no packet of the current timeline can still arrive.
 */
bool DesyncDialogClass::Everything_Sent_Acknowledged() const
{
	for (int i = 0; i < static_cast<int>(IPXManagerClass::Instance.NumConnections); i++) {
		const auto* connection = IPXManagerClass::Instance.Connection[i];
		if (connection != nullptr && connection->Queue != nullptr && connection->Queue->Num_Send() > 0) {
			return false;
		}
	}

	return true;
}


/**
 *  Whether every other connected player has said it is ready to load.
 */
bool DesyncDialogClass::Everyone_Ready() const
{
	auto& players = NodeNameType::Array;
	for (int i = 1; i < players.Count; i++) {
		const NodeNameType* node = players[i];
		if (node == nullptr) {
			continue;
		}

		const int id = node->HouseIndex;
		if (id >= 0 && id < SessionExt::MaxPlayers && !ReadyFrom[id]) {
			return false;
		}
	}

	return true;
}


/**
 *  Loads the save the game master chose, between two frames, and sets the
 *  game up to start from it the way a game loaded from a save starts.
 */
void DesyncDialogClass::Perform_Pending_Reload()
{
	const int save_frame = PendingReloadFrame;
	PendingReloadFrame = -1;

	const auto save = std::find_if(Saves.begin(), Saves.end(),
		[save_frame](const SaveEntry& entry) { return entry.Frame == save_frame; });
	if (save == Saves.end()) {
		QuitPending = true;
		return;
	}

	const std::string path = save->Path;
	Debug::Log("DesyncDialog: loading %s (frame %d) on frame %d.\n", path.c_str(), save_frame, Unsorted::CurrentFrame);

	/**
	 *  Everything sent before the desync was acknowledged in the dialog (see
	 *  Run): a packet's ID is then known to the other side, which drops any
	 *  later copy of it, so no event from the timeline being left can turn up
	 *  after the load. The events still queued here belong to it too.
	 *
	 *  So do the packets received but not yet read: unacknowledged ones (a
	 *  FRAMEINFO with no commands in it) are not covered by the above, and one
	 *  read after the load would bring a CRC of the old timeline into the new
	 *  one. It is done before loading, as a faster player may already be
	 *  sending from the new timeline by the time this one has loaded.
	 */
	if (WinsockInterfaceClass::Instance != nullptr) {
		WinsockInterfaceClass::Instance->DiscardInBuffers();
	}
	for (int i = 0; i < static_cast<int>(IPXManagerClass::Instance.NumConnections); i++) {
		auto* const connection = IPXManagerClass::Instance.Connection[i];
		if (connection != nullptr && connection->Queue != nullptr) {
			connection->Queue->Init();
		}
	}

	EventClass::OutList.Init();
	EventClass::DoList.Init();
	EventClass::MegaMissionList.Init();

	Reload_Hooks::Before_Load();

	if (!LoadOptionsClass::LoadMission(path.c_str())) {
		Debug::Log("DesyncDialog: loading %s failed; leaving.\n", path.c_str());
		QuitPending = true;
		return;
	}

	Reload_Hooks::After_Load();

	/**
	 *  Queue_AI_Multiplayer treats the next frame as the first of a loaded
	 *  game: it resets the sync counters, CRC history and compressed-event
	 *  caches, and waits for everyone with a FRAMESYNC handshake.
	 */
	SessionClass::Instance.LoadGame = 1;

	std::fill(std::begin(SessionExt::IsOutOfSync), std::end(SessionExt::IsOutOfSync), false);
	std::fill(std::begin(PlayerRemoved), std::end(PlayerRemoved), false);
	LastReloadFrame = save_frame;
	LastReloadDesyncFrame = DesyncFrame;
	LoadDecidedFrame = -1;
	ReadySent = false;
	std::fill(std::begin(ReadyFrom), std::end(ReadyFrom), false);

	// Later saves belong to the timeline left behind.
	Saves.erase(std::remove_if(Saves.begin(), Saves.end(),
		[save_frame](const SaveEntry& entry) { return entry.Frame > save_frame; }), Saves.end());

	wchar_t message[160];
	std::swprintf(message, std::size(message),
		StringTable::TryFetchString("GUI:DesyncReloaded", L"The game went out of sync and has gone back to %02d:%02d."),
		save_frame / 60 / 60, save_frame / 60 % 60);
	MessageListClass::Instance.PrintMessage(message, RulesClass::Instance->MessageDelay,
		HouseClass::CurrentPlayer ? HouseClass::CurrentPlayer->ColorSchemeIndex : -1, false);
}


/**
 *  Whether the save from this frame went out of sync again after being loaded.
 */
bool DesyncDialogClass::Is_Failed_Save(int frame) const
{
	return std::find(FailedSaveFrames.begin(), FailedSaveFrames.end(), frame) != FailedSaveFrames.end();
}
