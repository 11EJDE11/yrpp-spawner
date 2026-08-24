# Replay file format (`.yrrp`)

Defined in `src/Replay/ReplayFormat.h` and written by `src/Replay/ReplaySystem.cpp`; the hooks
that drive it live in `src/Replay/ReplaySystem.Hook.cpp`. The CnCNet client mirrors the header by hand in
`DXMainClient/Domain/ReplayGame.cs` — there is no compile-time link between the two, so **any
change here must be made in both repos in the same change**. A size mismatch does not error; it
silently misparses every field after the point of divergence.

`GetReplayFPSFromGameSpeed` is duplicated the same way, as `ReplayGame.GetFramesPerSecond`. The
client uses it to turn `TotalFrames` into a displayed duration, so the two have to agree.

## Layout

```
[ReplayHeader]        1384 bytes, #pragma pack(1)
[spawn.ini]           SpawnIniSize bytes, verbatim text (sanitized, see below)
[spawnmap.ini]        SpawnMapSize bytes, verbatim text
--- everything past this point is one raw deflate stream ---
[frame records]       repeated, sparse and monotonic
[end-of-stream]       FrameRecordHeader with FrameNumber == -1
```

The header and the two embedded INIs are never compressed, so a reader can pull them out with a
plain seek and read. That is what the client does, and why it needs no decompressor.

## ReplayHeader

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | `Magic` | `0x4A455259` |
| 4 | 4 | `Version` | `1` |
| 8 | 260 | `MapName` | `spawnmap.ini [Basic] Name`, else spawn.ini `UIMapName`, else `ScenarioClass::FileName` |
| 268 | 4 | `SpawnerVersion{Major,Minor,Revision,Patch}` | four `uint8`; from spawn.ini `SpawnerVersion` if present, else the compiled-in `VERSION_*` |
| 272 | 64 | `GameClientVersion` | from spawn.ini `GameClientVersion` |
| 336 | 4 | `GameMode` | `SessionClass::GameMode` — Campaign 0, LAN 3, Internet 4, Skirmish 5 |
| 340 | 4 | `UniqueIDCounter` | `ScenarioClass::UniqueID` at recording start |
| 344 | 4 | `Seed` | `Game::Seed` |
| 348 | 4 | `RandomNext1` | |
| 352 | 4 | `RandomNext2` | |
| 356 | 1000 | `RandomizerTable[250]` | full RNG table snapshot |
| 1356 | 4 | `SpawnIniSize` | bytes of embedded spawn.ini |
| 1360 | 4 | `SpawnMapSize` | bytes of embedded spawnmap.ini |
| 1364 | 4 | `RecordedGameSpeed` | 0–6; validated on load |
| 1368 | 8 | `RecordedUnixTime` | `time()` at recording start |
| 1376 | 4 | `TotalFrames` | last frame that carried a record |
| 1380 | 4 | `Flags` | bit 0 = `CleanShutdown`; see below |

`TotalFrames` counts the last frame a record was *written* for, not the last frame simulated.
Frames that changed nothing are not recorded, so a game whose final seconds were idle under-reports
its length slightly. It is only ever used to show a duration.

`Flags` bit 0 is set by `StopReplaySystem` when it stamps `TotalFrames`, so a recording that died
with the process leaves both fields zero. That flag - not `TotalFrames > 0` - is what tells a
truncated recording apart from one that was quit on frame 0.

`IsReplayHeaderValid` checks `Magic`, `Version == 1` and `RecordedGameSpeed <= 6`. Playback also
checks that `SpawnIniSize + SpawnMapSize` actually fits the file before seeking past them. There is
no checksum.

`ReplaySystem.cpp` static-asserts `sizeof(ReplayHeader) == 1384`, `sizeof(FrameRecordHeader) == 12`
and `sizeof(SideChannelRecord) == 329`, so a layout change here fails the build rather than
silently misparsing on the client side. The asserts cannot see `ReplayGame.cs` - the message
points at it, but keeping the two in step is still manual.

## Compression

The frame records are a single raw deflate stream (RFC 1951, no zlib wrapper), produced by
upstream miniz 3.0.2, vendored byte-identical in `src/Replay/miniz.{c,h}` and configured through the
`MINIZ_NO_*` defines on its `ClCompile` entry in `Spawner.vcxproj` (deflate and inflate only). It is
driven by `Replay::DeflateWriter` / `Replay::InflateReader` in `src/Replay/ReplayStream.{h,cpp}`.

A truncated stream is detected by clearing `TINFL_FLAG_HAS_MORE_INPUT` once the file runs out and
treating the resulting `TINFL_STATUS_FAILED_CANNOT_MAKE_PROGRESS` as end of data. Do not reinstate
the pre-1.16 behaviour of leaving the flag set: older miniz zero-padded the input and decoded
plausible-looking garbage past the end of a cut-short recording.

The stream is sync-flushed every 60 recorded frames. A sync flush ends the current deflate block
without resetting the dictionary, so everything written before it decodes on its own. That is what
keeps a recording a crash cut short usable: the reader inflates up to the last bytes that reached
the disk and then reports a short stream. At most one second of play is lost, and the header's
`CleanShutdown` flag stays clear, which is how a reader knows the recording is incomplete.

Independent per-block compression was measured against this and is strictly worse - to bound the
loss to the same one second it would need 16 KB blocks, which compress to 9.3x against the single
stream's 10.1x. On a 3.7 MB, 8.5 minute recording the stream costs about 2.6 us per frame to
compress and the whole file inflates in ~5 ms.

`TotalFrames` and `Flags` are stamped together by `StopReplaySystem()`, which seeks back to offset
1376 just before the file is closed. A game that crashes or is killed never reaches that, so both
stay 0 — the `CleanShutdown` flag is the only signal a reader has that the frame stream is
complete.

## Embedded spawn.ini

`SanitizeSpawnIniForReplay` rewrites the value of every `Ip`, `IPv6` and `LanIP` key to `0.0.0.0`,
preserving line endings, and touches nothing else. It matches on key name alone and is deliberately
section-unaware, so a new section carrying an address is covered without anyone remembering to
update it. Ports are not blanked.

Player names, sides, colours, teams, game options and the client's `[ReplayFileHashes]` section are
all present verbatim, so a reader can reconstruct the full lobby state from this block alone.

Sanitization runs *before* `SpawnIniSize` is computed, so the size always describes what was
actually written.

## Frame records

`FrameRecordHeader` (12 bytes):

```
int32  FrameNumber          // -1 = end of stream
int32  EventCountThisFrame
uint32 Flags                // 1 = TacticalPos, 2 = Selection, 4 = SideChannel
```

Then, in order, whichever blocks the flags select:

- **TacticalPos** — `Point2D` (two `int32`).
- **Selection** — `int32 SelectedObjectCount`, then that many `uint32` unique IDs. Readers
  reject counts above 4096.
- **SideChannel** — `int32 count` (max 64), then that many `SideChannelRecord` (329 bytes each).
- **Events** — `EventCountThisFrame` × `sizeof(EventClass)` (111 bytes, static-asserted in
  `YRpp/EventClass.h`).

Frames are only written when something changed, so frame numbers are sparse but monotonic.

`SideChannelRecord` carries chat, beacons and taunts, which travel over the game's global channel
rather than the deterministic event queue and so are reproduced directly rather than re-injected.
`Type` is `1 = ChatMessage, 2 = BeaconPlace, 3 = BeaconDelete, 4 = BeaconText, 5 = Taunt`; `Aux` is
overloaded per type (colour scheme index for chat, beacon slot 0–2 for beacons, raw command byte
for taunts).

Replays get shared, so these records are treated as untrusted on read.
`SanitizeSideChannelRecord` terminates `SenderName` and `Text` - the arrays are copied verbatim
off disk and need not be - and range-checks `House` against `BeaconManagerClass::Beacons[8][3]`
and `Aux` against the beacon slot count, `ColorScheme::Array`, or the 16 taunt commands. Records
that fail are still read, to keep the stream aligned, and then dropped.

## Relevant spawn.ini keys

All in `[Settings]`. Recording and playback are mutually exclusive — a non-empty `ReplayFile` wins.

| Key | Default | Meaning |
|---|---|---|
| `EnableReplayRecording` | `false` | Record this game. |
| `ReplayFileOut` | `replay.dat` | Where the recording is written. Relative to the game directory; missing directories are created. |
| `ReplayFile` | *(empty)* | Non-empty switches the session to playback of that file. |
| `ReplayShroudEnabled` | `false` | Keep the recording player's shroud instead of revealing the map. |
| `ReplayLockedViewport` | `true` | Pin the camera to the recorded position. |
| `ReplaySelectUnits` | `true` | Reproduce the recorded unit selection. |
| `ReplaySpectator` | `false` | Watch as an observer. Suppressed when `IsCampaign`. |
| `ReplayShowChatAndBeacons` | `true` | Playback only; recording of this data is unconditional. |
| `ReplayPlaybackSpeed` | `-1` | Game speed index to pace playback at. `-1` falls back to `GameSpeed`. Does not affect the simulation, which stays pinned to `RecordedGameSpeed`. |

Playback speed is deliberately kept out of `OptionsClass Options.GameSpeed` (0xA8EB60): simulation
code reads that through `GetAnimSpeed`, so it stays pinned to `RecordedGameSpeed` for the whole of
playback. The in-game options dialog binds its speed slider to the same variable, so three hooks in
`ReplaySystem.Hook.cpp` (0x4E209E, 0x4E1E1B, 0x4E1EBA) give that dialog a view of the playback
speed instead. Recorded `GameSpeed` events are harvested for their requested speed and then dropped
rather than executed, so the engine never writes the pinned value.

Playback also overrides `Seed`, `GameSpeed`, `Protocol`, `FrameSendRate` and `MaxAhead` from the
header, skips `CreateConnections()`, and suppresses the statistics packet. `Spawner/Statistics.cpp`
does that last one by jumping to `0x64820E`, which is the address of the replay system's own
event-pump hook - moving that hook means moving that jump target with it.

The spawner does **not** extract the embedded spawn.ini/spawnmap.ini for playback — it seeks past
them. The client is responsible for writing both files out before launching.
