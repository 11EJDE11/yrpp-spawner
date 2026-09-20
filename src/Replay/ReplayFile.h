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

#include <filesystem>
#include <string>
#include <vector>

class SpawnerConfig;

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

	// Paths in spawn.ini are UTF-8 like the rest of it. Widen them for the W file APIs rather
	// than passing them to the A ones, which would read them in the system code page.
	std::wstring Utf8ToWide(const char* text);

	// Read the uncompressed header without opening a playback stream.
	bool ReadReplayHeaderFromPath(const char* replayPath, ReplayHeader& outHeader);

	// Recording setup reads the current engine state and embeds the launch INIs.
	const char* GetRecordingOutputPath(const SpawnerConfig* pConfig);
	bool WriteInitialReplayFile(const SpawnerConfig* pConfig);

	// A save to embed as a checkpoint: the game's own .SAV and the sidecar captured with it, both on disk.
	struct CheckpointSource
	{
		int32_t Frame = 0;
		std::filesystem::path Save;
		std::filesystem::path Sidecar;
	};

	// Owns the file handle and compression state. The header and embedded files are
	// uncompressed; frame bytes use one deflate stream beginning just after them.
	// An optional checkpoint archive follows the completed deflate stream.
	class File
	{
	public:
		File() = default;
		~File();
		File(const File&) = delete;
		File& operator=(const File&) = delete;

		// Append to the header/INIs written by WriteInitialReplayFile.
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

		// Optional independent checkpoint archive after the finished frame deflate stream, streamed
		// from the sources' files so no save is ever held in memory. Sources must be in ascending frame
		// order. One that cannot be read, or that would take the archive past maxBytes, is left out
		// and the rest still written. Returns how many were written; the header only points at the
		// archive when that is at least one.
		int WriteCheckpointArchive(const std::vector<CheckpointSource>& sources, uint32_t maxBytes, int probes);
		bool ReadCheckpointArchive(std::vector<unsigned char>& bytes);

		// Optional statistics section, appended after the checkpoint archive. Nothing in the game
		// reads it back; it is for readers outside the game.
		bool WriteStatisticsSection(const std::vector<unsigned char>& bytes);

		// Call only after writing the end marker and finishing compression successfully.
		bool StampCleanShutdown(int lastWrittenFrame);

	private:
		// Appends a section at EOF, then stamps its offset and size into the header fields at
		// offsetField (uint64) and offsetField + 8 (uint32) - only once all of it is on disk.
		bool AppendSection(const std::vector<unsigned char>& bytes, uint32_t maxBytes, LONGLONG offsetField);
		bool StampSection(LONGLONG offsetField, uint64_t sectionOffset, uint32_t sectionSize);

		// Appends one archive entry at EOF: its index fields, then the payload deflated from the two files.
		bool AppendCheckpoint(const CheckpointSource& source, int probes);
		bool WriteAt(uint64_t offset, const void* data, size_t size);
		bool TruncateTo(uint64_t offset);
		bool EndOffset(uint64_t& offset);

		HANDLE Handle = INVALID_HANDLE_VALUE;
		DeflateWriter Writer;
		InflateReader Reader;
		uint64_t PlaybackStreamOffset = 0;
		// Copied from the header by OpenPlayback.
		uint64_t CheckpointArchiveOffset = 0;
		uint32_t CheckpointArchiveSize = 0;
		uint64_t BytesAtLastDiskFlush = 0;
	};
}
