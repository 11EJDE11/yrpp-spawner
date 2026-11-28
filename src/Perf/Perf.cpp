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

#include "Perf.h"

#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <TlHelp32.h>
#include <cstring>

namespace
{
	// Calls back with each hook a module's .syhks sections list: address, length and name, one after
	// another (Ares pads each entry to 16 bytes, Phobos and this DLL do not, so the words are read one
	// at a time and an entry is taken where the three of them make sense together).
	template <typename Callback>
	void ForEachListedHook(HMODULE module, Callback&& callback)
	{
		auto const base = reinterpret_cast<const BYTE*>(module);
		auto const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return;

		auto const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
		const DWORD size = nt->OptionalHeader.SizeOfImage;
		auto section = IMAGE_FIRST_SECTION(nt);
		for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
		{
			if (memcmp(section->Name, ".syhks", 6) != 0 || section->VirtualAddress + section->Misc.VirtualSize > size)
				continue;

			auto const words = reinterpret_cast<const DWORD*>(base + section->VirtualAddress);
			const DWORD count = section->Misc.VirtualSize / 4;
			for (DWORD at = 0; at + 2 < count; ++at)
			{
				const DWORD address = words[at], length = words[at + 1];
				auto const name = reinterpret_cast<const char*>(words[at + 2]);
				if (address < 0x401000 || address >= 0x10000000 || length == 0 || length > 0x40
					|| reinterpret_cast<const BYTE*>(name) < base || reinterpret_cast<const BYTE*>(name) >= base + size
					|| !memchr(name, 0, base + size - reinterpret_cast<const BYTE*>(name)))
				{
					continue;
				}

				callback(address, length, name);
				at += 2;
			}
		}
	}
}

bool Perf::OnlyKnownHooks(DWORD start, DWORD end, const KnownHook* known, size_t count, const char* what)
{
	const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	if (snapshot == INVALID_HANDLE_VALUE)
	{
		Debug::Log("[Perf] %s: the loaded DLLs could not be listed; leaving it alone.\n", what);
		return false;
	}

	bool ok = true;
	MODULEENTRY32W entry { sizeof(entry) };
	for (BOOL more = Module32FirstW(snapshot, &entry); more && ok; more = Module32NextW(snapshot, &entry))
	{
		ForEachListedHook(entry.hModule, [&](DWORD address, DWORD length, const char* name)
		{
			if (!ok || address < start || address >= end)
				return;

			for (size_t i = 0; i < count; ++i)
			{
				if (known[i].Address == address && known[i].Length == length
					&& _wcsicmp(known[i].Module, entry.szModule) == 0 && strcmp(known[i].Name, name) == 0)
				{
					return;
				}
			}

			Debug::Log("[Perf] %s: %ls hooks %08X (%s), which is not one that is known; leaving it alone.\n",
				what, entry.szModule, address, name);
			ok = false;
		});
	}

	CloseHandle(snapshot);
	return ok;
}

bool Perf::HookListed(const wchar_t* module, DWORD address, DWORD length, const char* name)
{
	const HMODULE handle = GetModuleHandleW(module);
	if (!handle)
		return false;

	bool listed = false;
	ForEachListedHook(handle, [&](DWORD at, DWORD bytes, const char* listedName)
	{
		listed |= at == address && bytes == length && strcmp(listedName, name) == 0;
	});

	return listed;
}

// After Syringe has written every hook, so the MapRevealer takeover replaces Ares' jumps and the
// others can see the hooks that are in the functions they take over.
DEFINE_HOOK(0x6BD7CB, WinMain_PerfPatches, 0x5)
{
	Perf::MapRevealer::Apply();
	Perf::EmptyCells::Apply();
	Perf::Weapons::Apply();
	Perf::Detach::Check();
	return 0;
}
