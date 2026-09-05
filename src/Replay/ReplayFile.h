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

#include "ReplayFormat.h"
#include "ReplayStream.h"

namespace Replay
{
	// Header/open failures stay distinct so the caller can show the appropriate message.
	enum class ReplayOpenFailure
	{
		None,
		// The file could not be opened, or ended inside the header.
		Unreadable,
		// No magic number: not a replay at all.
		NotAReplay,
		// The header does not describe a valid file structure.
		Malformed
	};

	const char* DescribeReplayOpenFailure(ReplayOpenFailure failure);

	// Read the uncompressed header without opening a playback stream.
	bool ReadReplayHeaderFromPath(const char* replayPath, ReplayHeader& outHeader);

	// Owns the file handle and compression state. The header and embedded files are
	// uncompressed; frame bytes use one deflate stream beginning just after them.
	class File
	{
	public:
		File() = default;
		~File();
		File(const File&) = delete;
		File& operator=(const File&) = delete;

		// Append to the header/INIs already written at the start of the file.
		bool OpenRecording(const char* outputPath);
		bool OpenPlayback(const char* replayPath, ReplayOpenFailure& outFailure);
		bool IsOpen() const { return this->Handle != INVALID_HANDLE_VALUE; }
		void Close();

		// Frame encoding/decoding belongs to the caller.
		bool Write(const void* data, size_t size);
		bool Read(void* buffer, size_t size);
		bool RestartPlaybackStream();

		// The caller chooses the frame cadence; forced disk commits use a byte threshold.
		bool SyncFlush();
		bool FinishRecording();

		// Call only after writing the end marker and finishing compression successfully.
		bool StampCleanShutdown(int lastWrittenFrame);

	private:
		HANDLE Handle = INVALID_HANDLE_VALUE;
		DeflateWriter Writer;
		InflateReader Reader;
		uint64_t PlaybackStreamOffset = 0;
		uint64_t BytesAtLastDiskFlush = 0;
	};
}
