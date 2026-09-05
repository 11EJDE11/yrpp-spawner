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

#include "ReplayStream.h"

#include <Vendor/miniz/miniz.h>

#include <algorithm>
#include <cstring>

namespace Replay
{
	namespace
	{
		// miniz encodes the compression level as a dictionary probe count in the low 12 bits of
		// the tdefl flags. 128 is what level 6 maps to.
		constexpr int DeflateProbes = 128;

		constexpr size_t OutputBufferSize = 32 * 1024;
		constexpr size_t InputBufferSize = 32 * 1024;

		tdefl_compressor* AsCompressor(void* p) { return static_cast<tdefl_compressor*>(p); }
		tinfl_decompressor* AsDecompressor(void* p) { return static_cast<tinfl_decompressor*>(p); }
	}

	void DeflateWriter::CompressorDeleter::operator()(void* p) const
	{
		delete AsCompressor(p);
	}

	void InflateReader::DecompressorDeleter::operator()(void* p) const
	{
		delete AsDecompressor(p);
	}

	DeflateWriter::~DeflateWriter()
	{
		this->Reset();
	}

	InflateReader::~InflateReader()
	{
		this->Reset();
	}

	bool DeflateWriter::Start(HANDLE file)
	{
		this->Reset();

		if (file == INVALID_HANDLE_VALUE)
			return false;

		std::unique_ptr<void, CompressorDeleter> compressor { new (std::nothrow) tdefl_compressor {} };
		if (!compressor)
			return false;

		// No TDEFL_WRITE_ZLIB_HEADER: a raw deflate stream, which the client can hand straight to
		// System.IO.Compression.DeflateStream.
		if (tdefl_init(AsCompressor(compressor.get()), nullptr, nullptr, DeflateProbes) != TDEFL_STATUS_OKAY)
			return false;

		this->OutputBuffer.resize(OutputBufferSize);
		this->Compressor = std::move(compressor);
		this->File = file;
		this->BytesWritten = 0;
		this->Failed = false;
		return true;
	}

	void DeflateWriter::Reset()
	{
		this->Compressor.reset();
		this->OutputBuffer.clear();
		this->OutputBuffer.shrink_to_fit();
		this->File = INVALID_HANDLE_VALUE;
		this->BytesWritten = 0;
		this->Failed = false;
	}

	bool DeflateWriter::WriteAll(const void* data, size_t size)
	{
		const auto* cursor = static_cast<const uint8_t*>(data);

		while (size > 0)
		{
			const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 0x10000000u));
			DWORD written = 0;
			if (!WriteFile(this->File, cursor, chunk, &written, nullptr) || written == 0)
				return false;

			cursor += written;
			size -= written;
			this->BytesWritten += written;
		}

		return true;
	}

	bool DeflateWriter::Pump(const void* data, size_t size, int flush)
	{
		if (this->Failed || !this->Compressor)
			return false;

		const auto* input = static_cast<const uint8_t*>(data);
		size_t inputLeft = size;

		for (;;)
		{
			size_t inputUsed = inputLeft;
			size_t outputUsed = this->OutputBuffer.size();

			const tdefl_status status = tdefl_compress(
				AsCompressor(this->Compressor.get()),
				input,
				&inputUsed,
				this->OutputBuffer.data(),
				&outputUsed,
				static_cast<tdefl_flush>(flush)
			);

			if (status < TDEFL_STATUS_OKAY)
			{
				this->Failed = true;
				return false;
			}

			if (outputUsed > 0 && !this->WriteAll(this->OutputBuffer.data(), outputUsed))
			{
				this->Failed = true;
				return false;
			}

			input += inputUsed;
			inputLeft -= inputUsed;

			if (status == TDEFL_STATUS_DONE)
				break;

			// All input consumed and the compressor stopped short of filling the output buffer, so
			// it has nothing left pending for this flush mode.
			if (inputLeft == 0 && outputUsed < this->OutputBuffer.size())
				break;
		}

		return true;
	}

	bool DeflateWriter::Write(const void* data, size_t size)
	{
		if (size == 0)
			return !this->Failed && this->Compressor != nullptr;

		return this->Pump(data, size, TDEFL_NO_FLUSH);
	}

	bool DeflateWriter::SyncFlush()
	{
		return this->Pump(nullptr, 0, TDEFL_SYNC_FLUSH);
	}

	bool DeflateWriter::Finish()
	{
		if (!this->Compressor)
			return false;

		const bool ok = this->Pump(nullptr, 0, TDEFL_FINISH);
		this->Compressor.reset();
		return ok;
	}

	bool InflateReader::Start(HANDLE file)
	{
		this->Reset();

		if (file == INVALID_HANDLE_VALUE)
			return false;

		std::unique_ptr<void, DecompressorDeleter> decompressor { new (std::nothrow) tinfl_decompressor {} };
		if (!decompressor)
			return false;

		tinfl_init(AsDecompressor(decompressor.get()));

		this->Window.assign(TINFL_LZ_DICT_SIZE, 0);
		this->InputBuffer.resize(InputBufferSize);
		this->Decompressor = std::move(decompressor);
		this->File = file;
		return true;
	}

	void InflateReader::Reset()
	{
		this->Decompressor.reset();
		this->Window.clear();
		this->Window.shrink_to_fit();
		this->InputBuffer.clear();
		this->InputBuffer.shrink_to_fit();
		this->File = INVALID_HANDLE_VALUE;
		this->WindowWritePos = 0;
		this->WindowReadPos = 0;
		this->WindowAvailable = 0;
		this->InputPos = 0;
		this->InputAvailable = 0;
		this->InputExhausted = false;
		this->Finished = false;
	}

	bool InflateReader::Refill()
	{
		if (this->Finished || !this->Decompressor)
			return false;

		for (;;)
		{
			if (this->InputAvailable == 0 && !this->InputExhausted)
			{
				DWORD read = 0;
				if (!ReadFile(this->File, this->InputBuffer.data(), static_cast<DWORD>(this->InputBuffer.size()), &read, nullptr))
					return false;

				this->InputPos = 0;
				this->InputAvailable = read;
				if (read == 0)
					this->InputExhausted = true;
			}

			size_t inputUsed = this->InputAvailable;
			// tinfl treats the window as a ring, so never ask it to write past the end of the
			// buffer.
			size_t outputUsed = TINFL_LZ_DICT_SIZE - this->WindowWritePos;

			const mz_uint32 flags = this->InputExhausted
				? 0u
				: static_cast<mz_uint32>(TINFL_FLAG_HAS_MORE_INPUT);

			const tinfl_status status = tinfl_decompress(
				AsDecompressor(this->Decompressor.get()),
				this->InputBuffer.data() + this->InputPos,
				&inputUsed,
				this->Window.data(),
				this->Window.data() + this->WindowWritePos,
				&outputUsed,
				flags
			);

			this->InputPos += inputUsed;
			this->InputAvailable -= inputUsed;

			if (outputUsed > 0)
			{
				this->WindowReadPos = this->WindowWritePos;
				this->WindowAvailable = outputUsed;
				this->WindowWritePos = (this->WindowWritePos + outputUsed) & (TINFL_LZ_DICT_SIZE - 1);

				if (status == TINFL_STATUS_DONE)
					this->Finished = true;

				return true;
			}

			if (status == TINFL_STATUS_DONE)
			{
				this->Finished = true;
				return false;
			}

			// Corrupt input, or the truncation a crashed recording leaves behind, lands here.
			if (status < TINFL_STATUS_DONE || this->InputExhausted)
			{
				this->Finished = true;
				return false;
			}
		}
	}

	bool InflateReader::Read(void* buffer, size_t size)
	{
		if (!this->Decompressor)
			return false;

		auto* cursor = static_cast<uint8_t*>(buffer);

		while (size > 0)
		{
			if (this->WindowAvailable == 0 && !this->Refill())
				return false;

			const size_t chunk = std::min<size_t>(size, this->WindowAvailable);
			std::memcpy(cursor, this->Window.data() + this->WindowReadPos, chunk);

			cursor += chunk;
			size -= chunk;
			this->WindowReadPos = (this->WindowReadPos + chunk) & (TINFL_LZ_DICT_SIZE - 1);
			this->WindowAvailable -= chunk;
		}

		return true;
	}
}
