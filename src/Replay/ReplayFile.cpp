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

#include "ReplayFile.h"

#include <Utilities/Debug.h>

#include <algorithm>
#include <cstddef>

namespace Replay
{
	namespace
	{
		// Forced commits stall the game thread, so they run less often than sync flushes.
		constexpr uint64_t DiskFlushIntervalBytes = 50ull * 1024 * 1024;

		bool WriteRawToHandle(HANDLE file, const void* data, size_t size)
		{
			if (file == INVALID_HANDLE_VALUE)
				return false;

			DWORD bytesWritten = 0;
			return WriteFile(file, data, static_cast<DWORD>(size), &bytesWritten, nullptr)
				&& bytesWritten == static_cast<DWORD>(size);
		}

		bool ReadRawFromHandle(HANDLE file, void* buffer, size_t size)
		{
			if (file == INVALID_HANDLE_VALUE)
				return false;

			DWORD bytesRead = 0;
			return ReadFile(file, buffer, static_cast<DWORD>(size), &bytesRead, nullptr)
				&& bytesRead == static_cast<DWORD>(size);
		}

		// Split out from IsReplayHeaderValid so the reason survives as far as the message the player sees.
		ReplayOpenFailure ClassifyReplayHeader(const ReplayHeader& header)
		{
			if (header.Magic != ReplayMagic)
				return ReplayOpenFailure::NotAReplay;

			if (!IsReplayVersionSupported(header.Version))
				return ReplayOpenFailure::UnsupportedVersion;

			// A shorter header than this build's is missing fields it reads; a longer one is fine, and is
			// the whole point of HeaderSize.
			if (header.HeaderSize < sizeof(ReplayHeader))
				return ReplayOpenFailure::Malformed;

			if (header.UniqueIDCounter < 0
				|| header.RandomNext1 < 0 || header.RandomNext1 >= 250
				|| header.RandomNext2 < 0 || header.RandomNext2 >= 250
				|| header.SpawnIniSize > MaxEmbeddedFileBytes
				|| header.SpawnMapSize > MaxEmbeddedFileBytes
				|| !IsReplayGameSpeedIndexValid(header.RecordedGameSpeed))
			{
				return ReplayOpenFailure::Malformed;
			}

			return ReplayOpenFailure::None;
		}

		bool ReadReplayHeaderFromHandle(HANDLE file, ReplayHeader& outHeader, ReplayOpenFailure& outFailure)
		{
			outHeader = {};

			if (!ReadRawFromHandle(file, &outHeader, sizeof(outHeader)))
			{
				outFailure = ReplayOpenFailure::Unreadable;
				return false;
			}

			outFailure = ClassifyReplayHeader(outHeader);
			return outFailure == ReplayOpenFailure::None;
		}

		bool ReadReplayHeaderFromHandle(HANDLE file, ReplayHeader& outHeader)
		{
			ReplayOpenFailure failure = ReplayOpenFailure::None;
			return ReadReplayHeaderFromHandle(file, outHeader, failure);
		}

	}

	const char* DescribeReplayOpenFailure(ReplayOpenFailure failure)
	{
		switch (failure)
		{
		case ReplayOpenFailure::NotAReplay:
			return "the file is not a replay";
		case ReplayOpenFailure::UnsupportedVersion:
			return "the replay was recorded in a newer format than this version of the game can read";
		case ReplayOpenFailure::Malformed:
			return "the replay header is damaged";
		case ReplayOpenFailure::Unreadable:
		default:
			return "the replay could not be read";
		}
	}

	bool ReadReplayHeaderFromPath(const char* replayPath, ReplayHeader& outHeader)
	{
		HANDLE file = CreateFileA(
			replayPath,
			GENERIC_READ,
			FILE_SHARE_READ,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr
		);

		if (file == INVALID_HANDLE_VALUE)
			return false;

		const bool ok = ReadReplayHeaderFromHandle(file, outHeader);
		CloseHandle(file);
		return ok;
	}

	File::~File()
	{
		this->Close();
	}

	void File::Close()
	{
		this->Writer.Reset();
		this->Reader.Reset();
		this->PlaybackStreamOffset = 0;
		this->BytesAtLastDiskFlush = 0;

		if (this->Handle != INVALID_HANDLE_VALUE)
		{
			FlushFileBuffers(this->Handle);
			CloseHandle(this->Handle);
			this->Handle = INVALID_HANDLE_VALUE;
		}
	}

	bool File::OpenRecording(const char* outputPath)
	{
		this->Close();

		this->Handle = CreateFileA(
			outputPath,
			GENERIC_WRITE,
			FILE_SHARE_READ,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr
		);

		if (this->Handle == INVALID_HANDLE_VALUE)
		{
			// WriteInitialReplayFile has just written this file. Recreating it would start the deflate
			// stream at offset 0 and produce a headerless file no reader can parse.
			Debug::Log("[Replay] Could not reopen the replay file for recording: %s\n", outputPath);
			return false;
		}

		SetFilePointer(this->Handle, 0, nullptr, FILE_END);

		// Everything from here on - and nothing before it - is deflated.
		if (!this->Writer.Start(this->Handle))
		{
			Debug::Log("[Replay] Failed to start the compressed replay stream.\n");
			this->Close();
			return false;
		}

		this->BytesAtLastDiskFlush = 0;
		return true;
	}

	bool File::OpenPlayback(const char* replayPath, ReplayOpenFailure& outFailure)
	{
		this->Close();
		outFailure = ReplayOpenFailure::None;

		this->Handle = CreateFileA(
			replayPath,
			GENERIC_READ,
			FILE_SHARE_READ,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr
		);

		if (this->Handle == INVALID_HANDLE_VALUE)
		{
			outFailure = ReplayOpenFailure::Unreadable;
			return false;
		}

		ReplayHeader header {};
		if (!ReadReplayHeaderFromHandle(this->Handle, header, outFailure))
		{
			this->Close();
			return false;
		}

		// The sizes come straight off disk, so check them against the file before seeking.
		const uint64_t payloadSize = static_cast<uint64_t>(header.SpawnIniSize) + header.SpawnMapSize;
		const uint64_t streamOffset = static_cast<uint64_t>(header.HeaderSize) + payloadSize;

		LARGE_INTEGER fileSize {};
		if (!GetFileSizeEx(this->Handle, &fileSize)
			|| streamOffset > static_cast<uint64_t>(fileSize.QuadPart))
		{
			Debug::Log("[Replay] Replay declares embedded file sizes that do not fit the file.\n");
			outFailure = ReplayOpenFailure::Malformed;
			this->Close();
			return false;
		}

		LARGE_INTEGER streamStart {};
		streamStart.QuadPart = static_cast<LONGLONG>(streamOffset);

		if (!SetFilePointerEx(this->Handle, streamStart, nullptr, FILE_BEGIN))
		{
			outFailure = ReplayOpenFailure::Unreadable;
			this->Close();
			return false;
		}

		// The frame stream is always deflated; the header and the two INIs above it never are.
		if (!this->Reader.Start(this->Handle))
		{
			Debug::Log("[Replay] Failed to start reading the compressed replay stream.\n");
			outFailure = ReplayOpenFailure::Unreadable;
			this->Close();
			return false;
		}

		// Kept so a seek can restart the decompressor here. See ReplaySeek.h.
		this->PlaybackStreamOffset = streamOffset;
		return true;
	}

	bool File::Write(const void* data, size_t size)
	{
		if (this->Writer.IsActive())
			return this->Writer.Write(data, size);

		return WriteRawToHandle(this->Handle, data, size);
	}

	bool File::Read(void* buffer, size_t size)
	{
		if (this->Reader.IsActive())
			return this->Reader.Read(buffer, size);

		return ReadRawFromHandle(this->Handle, buffer, size);
	}

	bool File::RestartPlaybackStream()
	{
		if (!this->IsOpen())
			return false;

		LARGE_INTEGER streamStart {};
		streamStart.QuadPart = static_cast<LONGLONG>(this->PlaybackStreamOffset);
		return SetFilePointerEx(this->Handle, streamStart, nullptr, FILE_BEGIN)
			&& this->Reader.Start(this->Handle);
	}

	bool File::SyncFlush()
	{
		if (!this->Writer.IsActive())
			return true;

		if (!this->Writer.SyncFlush())
			return false;

		// Committing to the disk is far more expensive, so it stays on a rare byte-count schedule.
		const uint64_t written = this->Writer.CompressedBytesWritten();
		if (written - this->BytesAtLastDiskFlush >= DiskFlushIntervalBytes)
		{
			FlushFileBuffers(this->Handle);
			this->BytesAtLastDiskFlush = written;
		}

		return true;
	}

	bool File::FinishRecording()
	{
		return !this->Writer.IsActive() || this->Writer.Finish();
	}

	bool File::StampCleanShutdown(int lastWrittenFrame)
	{
		if (this->Handle == INVALID_HANDLE_VALUE)
			return false;

		static_assert(offsetof(ReplayHeader, Flags) == offsetof(ReplayHeader, TotalFrames) + sizeof(uint32_t),
			"TotalFrames and Flags are stamped in one write and have to stay adjacent");

		const LONG headerOffset = static_cast<LONG>(offsetof(ReplayHeader, TotalFrames));
		if (SetFilePointer(this->Handle, headerOffset, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
			return false;

		const uint32_t tail[2] = {
			static_cast<uint32_t>(std::max(0, lastWrittenFrame)),
			ReplayHeaderFlag_CleanShutdown
		};

		DWORD bytesWritten = 0;
		const bool ok = WriteFile(this->Handle, tail, sizeof(tail), &bytesWritten, nullptr) != FALSE
			&& bytesWritten == sizeof(tail);

		SetFilePointer(this->Handle, 0, nullptr, FILE_END);
		return ok;
	}
}
