// Standalone tests: no game process or engine calls. See scripts/test_replay_checkpoints.bat.
#include "../src/Replay/ReplayKeyframeState.Serialization.cpp"
#include <Replay/ReplayFile.h>
#include <Replay/ReplayRecordedCheckpoint.h>
#include <Utilities/Debug.h>
#include <Vendor/miniz/miniz.h>
#include <cassert>
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

static void TestFile(const std::filesystem::path& directory, bool checkpoints)
{
	const auto path = directory / (checkpoints ? "embedded.yrrp" : "no-saves.yrrp");
	Replay::ReplayHeader header {};
	header.Magic = Replay::ReplayMagic;
	header.Version = Replay::ReplayVersion;
	header.HeaderSize = sizeof(header);
	{
		std::ofstream output(path, std::ios::binary);
		output.write(reinterpret_cast<const char*>(&header), sizeof(header));
	}
	// Enough data to exercise buffering and decompressor read-ahead.
	std::vector<unsigned char> events(512 * 1024);
	uint32_t random = 17;
	for (auto& value : events) { random = 1664525u * random + 1013904223u; value = static_cast<unsigned char>(random >> 24); }
	std::vector<unsigned char> archive { 0, 0, 0, 0 };
	Replay::File file;
	assert(file.OpenRecording(path.string().c_str()));
	assert(file.Write(events.data(), events.size()));
	assert(file.SyncFlush());
	assert(file.FinishRecording());
	if (checkpoints) assert(file.WriteCheckpointArchive(archive));
	assert(file.StampCleanShutdown(12345));
	file.Close();
	Replay::ReplayOpenFailure failure;
	assert(file.OpenPlayback(path.string().c_str(), failure));
	std::vector<unsigned char> decoded(events.size());
	assert(file.Read(decoded.data(), 17));
	std::vector<unsigned char> loaded;
	assert(file.ReadCheckpointArchive(loaded));
	assert(checkpoints ? loaded == archive : loaded.empty());
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
	}
	std::filesystem::remove(path);
}

static void TestRetention()
{
	std::vector<Replay::RecordedCheckpoint> records;
	Replay::TrimRecordedCheckpoints(records);
	for (int frame : { 10, 1000, 1001, 1002, 2000 })
	{
		Replay::RecordedCheckpoint record;
		record.Frame = frame;
		record.Compressed = { 0 };
		records.push_back(std::move(record));
		Replay::TrimRecordedCheckpoints(records);
	}
	assert(records.size() == 4 && records.front().Frame == 10 && records.back().Frame == 2000);
	assert(records[1].Frame == 1000 && records[2].Frame == 1002);
	for (int frame = 2001; frame <= 20000; ++frame)
	{
		Replay::RecordedCheckpoint record;
		record.Frame = frame;
		record.Compressed = { 0 };
		records.push_back(std::move(record));
		Replay::TrimRecordedCheckpoints(records);
		assert(records.size() == 4 && records.front().Frame == 10 && records.back().Frame == frame);
	}
	records.clear();
	for (int frame = 1; frame <= 3; ++frame)
	{
		Replay::RecordedCheckpoint record;
		record.Frame = frame;
		record.Compressed.resize(8 * 1024 * 1024);
		records.push_back(std::move(record));
	}
	Replay::TrimRecordedCheckpoints(records);
	assert(records.size() == 2 && records.front().Frame == 1 && records.back().Frame == 3);
	records.back().Compressed.push_back(0);
	Replay::TrimRecordedCheckpoints(records);
	assert(records.size() == 1 && records[0].Frame == 3);
	std::cout << "Checkpoint count, time coverage, and byte-budget checks passed\n";
}

int main(int argc, char** argv)
{
	assert(argc == 2);
	TestSnapshot();
	TestRetention();
	TestFile(argv[1], false);
	TestFile(argv[1], true);
	std::cout << "Recording without saves, archive round-trip, buffered read, rewind, and corrupt locator checks passed\n";
}
