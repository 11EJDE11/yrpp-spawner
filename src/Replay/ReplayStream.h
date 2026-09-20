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

// Raw deflate (RFC 1951) streaming over a Win32 file handle, used for the replay frame stream.
// No game or YRpp dependency.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Replay
{
	// Recording. One deflate stream punctuated by sync flushes, so a crash mid-recording loses
	// only what was written since the last one.
	class DeflateWriter
	{
	public:
		DeflateWriter() = default;
		~DeflateWriter();

		DeflateWriter(const DeflateWriter&) = delete;
		DeflateWriter& operator=(const DeflateWriter&) = delete;

		// Begins a stream at the handle's current position. The handle stays owned by the caller.
		bool Start(HANDLE file);

		bool IsActive() const { return this->Compressor != nullptr; }

		bool Write(const void* data, size_t size);

		// Ends the current deflate block. Everything written so far is on disk afterwards.
		bool SyncFlush();

		// Terminates the stream. Must be called before seeking the handle elsewhere.
		bool Finish();

		void Reset();

		uint64_t CompressedBytesWritten() const { return this->BytesWritten; }

	private:
		bool Pump(const void* data, size_t size, int flush);
		bool WriteAll(const void* data, size_t size);

		struct CompressorDeleter { void operator()(void* p) const; };

		std::unique_ptr<void, CompressorDeleter> Compressor;
		std::vector<uint8_t> OutputBuffer;
		HANDLE File = INVALID_HANDLE_VALUE;
		uint64_t BytesWritten = 0;
		bool Failed = false;
	};

	// Playback. Reads are served out of a 32 KiB dictionary window, refilled from the file on
	// demand.
	class InflateReader
	{
	public:
		InflateReader() = default;
		~InflateReader();

		InflateReader(const InflateReader&) = delete;
		InflateReader& operator=(const InflateReader&) = delete;

		// Begins reading a stream at the handle's current position. The handle stays owned by the
		// caller.
		bool Start(HANDLE file);

		bool IsActive() const { return this->Decompressor != nullptr; }

		// False when the stream ends, is truncated - the usual shape of a crashed recording - or
		// is corrupt. Callers cannot tell those apart and do not need to: all three stop playback.
		bool Read(void* buffer, size_t size);

		void Reset();

	private:
		bool Refill();

		struct DecompressorDeleter { void operator()(void* p) const; };

		std::unique_ptr<void, DecompressorDeleter> Decompressor;
		std::vector<uint8_t> Window;      // TINFL_LZ_DICT_SIZE, used as a ring buffer
		std::vector<uint8_t> InputBuffer;
		HANDLE File = INVALID_HANDLE_VALUE;
		size_t WindowWritePos = 0;        // where the next decompressed bytes land
		size_t WindowReadPos = 0;         // start of the bytes not yet handed out
		size_t WindowAvailable = 0;       // bytes not yet handed out
		size_t InputPos = 0;
		size_t InputAvailable = 0;
		bool InputExhausted = false;
		bool Finished = false;
	};
}
