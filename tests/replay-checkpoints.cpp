// Standalone tests: no game process or engine calls. See scripts/test_replay_checkpoints.bat.
#include "../src/Replay/ReplayKeyframeState.Serialization.cpp"
#include <Replay/ReplayFile.h>
#include <Replay/ReplayRecordedCheckpoint.h>
#include <Utilities/Debug.h>
#include <Vendor/miniz/miniz.h>
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

void Debug::Log(const char*, ...) { }
namespace ReplaySystem::KeyframeState
{
	Snapshot::Snapshot() : Data(std::make_unique<Detail::SnapshotData>()) { }
	Snapshot::~Snapshot() = default;
}

using namespace ReplaySystem::KeyframeState;
using namespace Replay::CheckpointCodec;

static std::vector<unsigned char> Encode(Detail::SnapshotData& data)
{
	Writer writer;
	Detail::Visit(writer, data);
	assert(writer.Good);
	return writer.Bytes;
}

static void TestSnapshot()
{
	Detail::SnapshotData data;
	data.ScenarioUniqueID = 1000042;
	data.Technos.push_back({ 42, true });
	data.HouseRepairs.push_back({ 43, true, 123, 456 });
	data.Planning.Nodes.resize(1);
	data.Planning.Nodes[0].Members.resize(1);
	data.Planning.Nodes[0].Branches.resize(1);
	data.Planning.Tokens.resize(1);
	data.Planning.Tokens[0].Nodes = { 0 };
	data.Planning.PendingEvents.resize(1);
	data.Planning.PendingEvents[0][30] = 53;
	data.Planning.ManagerNodeLists[2] = { 0 };
	data.Planning.ActiveRouteOwners = { 42 };
	data.AresParticles.Captured = true;
	data.AresParticles.Systems.resize(1);
	data.AresParticles.Systems[0].MovementData.resize(1);
	data.AresParticles.Systems[0].DrawData.resize(1);
	data.AresParticles.Systems[0].DrawData[0].Bytes[20] = 123;
	data.Tiberium.Captured = true;
	data.Tiberium.Queues.resize(1);
	for (auto& queue : data.Tiberium.Queues[0])
	{
		queue.Present = true;
		queue.Heap.push_back({ { 12, 34 }, 0.625f });
		queue.CellFlagCount = 9;
		queue.CellFlagBits = { 255, 1 };
	}
	data.LoadResetTimers.SpawnManagers.resize(1);
	data.LoadResetTimers.Bullets.resize(1);
	data.SlaveManagers.resize(1);
	data.SlaveManagers[0].Controls.resize(1);
	data.CellPassability = { 0, 1, 254, 255 };
	data.CellSubzones.push_back({ { 1, 2, 3, 4 }, 5, 6 });
	data.SubzoneGraph.Levels[0].resize(1);
	data.SubzoneGraph.EntryCounts[0] = 1;
	data.SubzoneGraph.Levels[0][0].Connections.push_back({ 0, 1 });
	data.LocomotorResetStates.resize(1);
	data.LocomotorResetStates[0].Bytes[0] = 42;
	for (auto& order : data.Orders) order = { 42, 43, 44 };
	for (auto& order : data.LayerOrders) order = { 44, 42 };
	data.Beacons[7][2].Present = true;
	data.Beacons[7][2].X = 1024;
	data.Beacons[7][2].Y = 2048;
	data.Beacons[7][2].Text[0] = L'B';
	const auto expected = Encode(data);
	Snapshot snapshot;
	assert(snapshot.Deserialize(expected));
	std::vector<unsigned char> roundtrip;
	assert(snapshot.Serialize(roundtrip) && expected == roundtrip);

	for (size_t size = 0; size < expected.size(); ++size)
	{
		const std::vector<unsigned char> truncated(expected.begin(), expected.begin() + size);
		assert(!snapshot.Deserialize(truncated));
	}
	auto bad = expected;
	bad.push_back(0);
	assert(!snapshot.Deserialize(bad));
	bad = expected;
	const size_t technoCount = 4 + sizeof(Randomizer);
	std::memset(bad.data() + technoCount, 255, 4);
	assert(!snapshot.Deserialize(bad));
	bad = expected; bad[technoCount + 8] = 2; // invalid bool
	assert(!snapshot.Deserialize(bad));
	data.Random[offsetof(Randomizer, Next1)] = 250;
	assert(!snapshot.Deserialize(Encode(data)));
	data.Random[offsetof(Randomizer, Next1)] = 0;
	data.Tiberium.Queues[0][0].CellFlagBits.clear();
	assert(!snapshot.Deserialize(Encode(data)));
	// Failed decodes leave the previously valid snapshot intact.
	assert(snapshot.Serialize(roundtrip) && expected == roundtrip);

	size_t compressedSize = 0;
	void* compressed = tdefl_compress_mem_to_heap(expected.data(), expected.size(), &compressedSize, 128);
	assert(compressed);
	std::vector<unsigned char> inflated(expected.size());
	assert(tinfl_decompress_mem_to_mem(inflated.data(), inflated.size(), compressed, compressedSize, 0) == expected.size());
	assert(inflated == expected);
	assert(tinfl_decompress_mem_to_mem(inflated.data(), inflated.size() - 1, compressed, compressedSize, 0) == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED);
	mz_free(compressed);
	std::cout << "Snapshot round-trip, truncation, bounds, validation, and miniz checks passed\n";
}

static void WriteBytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

static std::vector<unsigned char> Noise(size_t size, uint32_t seed)
{
	std::vector<unsigned char> bytes(size);
	for (auto& value : bytes) { seed = 1664525u * seed + 1013904223u; value = static_cast<unsigned char>(seed >> 24); }
	return bytes;
}

static void WriteStreamOnly(const std::filesystem::path& path)
{
	Replay::ReplayHeader header {};
	header.Magic = Replay::ReplayMagic;
	header.Version = Replay::ReplayVersion;
	header.HeaderSize = sizeof(header);
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output.write(reinterpret_cast<const char*>(&header), sizeof(header));
	}
}

static void TestFile(const std::filesystem::path& directory, bool checkpoints)
{
	const auto path = directory / (checkpoints ? "embedded.yrrp" : "no-saves.yrrp");
	// Enough data to exercise buffering and decompressor read-ahead.
	const auto events = Noise(512 * 1024, 17);
	WriteStreamOnly(path);

	// Three staged saves, the middle one missing its sidecar: it is left out and the others still written.
	std::vector<Replay::CheckpointSource> sources;
	std::vector<std::pair<std::vector<unsigned char>, std::vector<unsigned char>>> payloads;
	for (int i = 0; i < 3; ++i)
	{
		Replay::CheckpointSource source;
		source.Frame = 100 * (i + 1);
		source.Save = directory / ("staged" + std::to_string(i) + ".sav");
		source.Sidecar = directory / ("staged" + std::to_string(i) + ".sidecar");
		// Compressible, and larger than the writer's read buffer.
		std::vector<unsigned char> save(200 * 1024 + i);
		for (size_t j = 0; j < save.size(); ++j) save[j] = static_cast<unsigned char>((j / 7) ^ i);
		const auto sidecar = Noise(3000 + i, 99 + i);
		WriteBytes(source.Save, save);
		std::error_code error;
		std::filesystem::remove(source.Sidecar, error);
		if (i != 1) WriteBytes(source.Sidecar, sidecar);
		sources.push_back(source);
		payloads.emplace_back(save, sidecar);
	}

	Replay::File file;
	assert(file.OpenRecording(path.string().c_str()));
	assert(file.Write(events.data(), events.size()));
	assert(file.SyncFlush());
	assert(file.FinishRecording());
	if (checkpoints)
		assert(file.WriteCheckpointArchive(sources, Replay::MaxRecordedCheckpointBytes, Replay::RecordedCheckpointProbes) == 2);
	assert(file.StampCleanShutdown(12345));
	file.Close();
	Replay::ReplayOpenFailure failure;
	assert(file.OpenPlayback(path.string().c_str(), failure));
	std::vector<unsigned char> decoded(events.size());
	assert(file.Read(decoded.data(), 17));
	std::vector<unsigned char> loaded;
	assert(file.ReadCheckpointArchive(loaded));
	if (checkpoints)
	{
		// Decoded the way ImportRecordedCheckpoints does.
		Reader reader { loaded };
		uint32_t count = 0;
		reader.Scalar(count);
		assert(reader.Good && count == 2);
		for (const int source : { 0, 2 })
		{
			Replay::RecordedCheckpoint record;
			Fields(reader, record.Frame, record.RawSize, record.CRC, record.Compressed);
			assert(reader.Good && record.Frame == sources[source].Frame);
			std::vector<unsigned char> raw(record.RawSize);
			assert(tinfl_decompress_mem_to_mem(raw.data(), raw.size(), record.Compressed.data(),
				record.Compressed.size(), 0) == raw.size());
			assert(mz_crc32(0, raw.data(), raw.size()) == record.CRC);
			Reader payload { raw };
			std::vector<unsigned char> save, sidecar;
			Fields(payload, save, sidecar);
			assert(payload.Good && payload.Position == raw.size());
			assert(save == payloads[source].first && sidecar == payloads[source].second);
		}
		assert(reader.Position == loaded.size());
	}
	else
	{
		assert(loaded.empty());
	}
	assert(file.Read(decoded.data() + 17, decoded.size() - 17));
	assert(events == decoded);
	assert(file.RestartPlaybackStream());
	assert(file.Read(decoded.data(), decoded.size()));
	assert(events == decoded);
	file.Close();
	if (checkpoints)
	{
		// Invalid optional archive offset must not make the event stream unreadable.
		std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
		output.seekp(offsetof(Replay::ReplayHeader, CheckpointArchiveOffset));
		uint64_t invalid = UINT64_MAX;
		output.write(reinterpret_cast<const char*>(&invalid), sizeof(invalid));
		output.close();
		assert(file.OpenPlayback(path.string().c_str(), failure));
		assert(!file.ReadCheckpointArchive(loaded) && loaded.empty());
		assert(file.Read(decoded.data(), decoded.size()) && decoded == events);
		file.Close();

		// Over the byte budget: every entry is rolled back, the header is not pointed at an archive,
		// and the file ends where the stream did.
		WriteStreamOnly(path);
		assert(file.OpenRecording(path.string().c_str()));
		assert(file.Write(events.data(), events.size()));
		assert(file.FinishRecording());
		file.Close();
		const auto streamEnd = std::filesystem::file_size(path);
		assert(file.OpenRecording(path.string().c_str()));
		assert(file.WriteCheckpointArchive(sources, 64, Replay::RecordedCheckpointProbes) == 0);
		file.Close();
		assert(std::filesystem::file_size(path) == streamEnd);
		assert(file.OpenPlayback(path.string().c_str(), failure));
		assert(file.ReadCheckpointArchive(loaded) && loaded.empty());
		file.Close();
	}
	std::filesystem::remove(path);

	for (const auto& source : sources)
	{
		std::error_code error;
		std::filesystem::remove(source.Save, error);
		std::filesystem::remove(source.Sidecar, error);
	}
}

static void TestCheckpointChoice()
{
	// Thinning: never the newest; otherwise the smallest gap, measured from frame 0 for the first.
	assert(Replay::ChooseStagedCheckpointToEvict({ 7200, 14400 }) == 0);
	assert(Replay::ChooseStagedCheckpointToEvict({ 100, 5000, 5100, 9000 }) == 2);
	assert(Replay::ChooseStagedCheckpointToEvict({ 100, 9000, 9100 }) == 0);

	// Autosaves every 7200 frames for a long game, thinned as they arrive, stay spread over the game.
	std::vector<int32_t> staged;
	for (int32_t frame = 7200; frame <= 7200 * 27; frame += 7200)
	{
		staged.push_back(frame);
		while (staged.size() > Replay::MaxStagedCheckpoints)
			staged.erase(staged.begin() + Replay::ChooseStagedCheckpointToEvict(staged));
		assert(staged.back() == frame && std::is_sorted(staged.begin(), staged.end()));
	}
	assert(staged.size() == Replay::MaxStagedCheckpoints);
	for (size_t i = 1; i < staged.size(); ++i)
		assert(staged[i] - staged[i - 1] <= 7200 * 27 / 4);

	// Picks land near 37.5/57.5/72.5/87.5% of the recording, never past its end.
	const int32_t lastFrame = 198430;
	const auto chosen = Replay::ChooseRecordedCheckpoints(staged, lastFrame);
	assert(chosen.size() == Replay::MaxRecordedCheckpoints && std::is_sorted(chosen.begin(), chosen.end()));
	for (size_t i = 0; i < chosen.size(); ++i)
	{
		const int64_t target = static_cast<int64_t>(lastFrame) * Replay::RecordedCheckpointTargetsPerMille[i] / 1000;
		assert(std::llabs(staged[chosen[i]] - target) <= lastFrame / 8);
	}

	// A save every 1000 frames: the nearest to each target exactly.
	std::vector<int32_t> even;
	for (int32_t frame = 1000; frame <= 40000; frame += 1000) even.push_back(frame);
	const auto exact = Replay::ChooseRecordedCheckpoints(even, 40000);
	assert(exact.size() == 4 && even[exact[0]] == 15000 && even[exact[1]] == 23000
		&& even[exact[2]] == 29000 && even[exact[3]] == 35000);

	// Fewer saves than targets: all of them, each once. A save after the last frame: never.
	assert((Replay::ChooseRecordedCheckpoints({ 500, 900 }, 1000) == std::vector<size_t> { 0, 1 }));
	assert((Replay::ChooseRecordedCheckpoints({ 500, 1500 }, 1000) == std::vector<size_t> { 0 }));
	assert(Replay::ChooseRecordedCheckpoints({ 1500 }, 1000).empty());
	assert(Replay::ChooseRecordedCheckpoints({}, 1000).empty());

	std::cout << "Checkpoint staging and placement checks passed\n";
}

int main(int argc, char** argv)
{
	assert(argc == 2);
	TestSnapshot();
	TestCheckpointChoice();
	TestFile(argv[1], false);
	TestFile(argv[1], true);
	std::cout << "Recording without saves, archive round-trip, buffered read, rewind, and corrupt locator checks passed\n";
}
