# Replay file format (`.yrrp`)

Written by `src/Replay/ReplaySystem.cpp`. The CnCNet client mirrors the header by hand in
`DXMainClient/Domain/ReplayGame.cs` — there is no compile-time link between the two, so **any
change here must be made in both repos in the same change**. A size mismatch does not error; it
silently misparses every field after the point of divergence.

## Layout

```
[ReplayHeader]        1416 bytes, #pragma pack(1)
[spawn.ini]           SpawnIniSize bytes, verbatim text (sanitized, see below)
[spawnmap.ini]        SpawnMapSize bytes, verbatim text
[frame records]       repeated, sparse and monotonic
[end-of-stream]       FrameRecordHeader with FrameNumber == -1
```

## ReplayHeader

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | `Magic` | `0x4A455259` |
| 4 | 4 | `Version` | `1` |
| 8 | 260 | `MapName` | `spawnmap.ini [Basic] Name`, else spawn.ini `UIMapName`, else `ScenarioClass::FileName` |
| 268 | 4 | `StartFrame` | always 0; there is no seek support |
| 272 | 4 | `ProducerVersion{Major,Minor,Revision,Patch}` | four `uint8`; from spawn.ini `SpawnerVersion` if present, else the compiled-in `VERSION_*` |
| 276 | 32 | `GameVersionString` | hardcoded `"1.006"` |
| 308 | 64 | `GameClientVersion` | from spawn.ini `GameClientVersion` |
| 372 | 4 | `GameMode` | `SessionClass::GameMode` — Campaign 0, LAN 3, Internet 4, Skirmish 5 |
| 376 | 4 | `UniqueIDCounter` | `ScenarioClass::UniqueID` at recording start |
| 380 | 4 | `Seed` | `Game::Seed` |
| 384 | 4 | `RandomNext1` | |
| 388 | 4 | `RandomNext2` | |
| 392 | 1000 | `RandomizerTable[250]` | full RNG table snapshot |
| 1392 | 4 | `SpawnIniSize` | bytes of embedded spawn.ini |
| 1396 | 4 | `SpawnMapSize` | bytes of embedded spawnmap.ini |
| 1400 | 4 | `RecordedGameSpeed` | 0–6; validated on load |
| 1404 | 8 | `RecordedUnixTime` | `time()` at recording start |
| 1412 | 4 | `TotalFrames` | last recorded frame; **0 means the recording was truncated** |

`IsReplayHeaderValid` checks `Magic`, `Version == 1` and `RecordedGameSpeed <= 6`. There is no
checksum and no compression.

`TotalFrames` is stamped by `StopReplaySystem()` seeking back to offset 1412 just before the file
is closed. A game that crashes or is killed never reaches that, so the field stays 0 — that is the
only signal a reader has that the frame stream is incomplete.

## Embedded spawn.ini

`SanitizeSpawnIniForReplay` rewrites the value of every `Ip`, `IPv6` and `LanIP` key to `0.0.0.0`,
preserving line endings, and touches nothing else. Player names, sides, colours, teams, game
options and the client's `[ReplayFileHashes]` section are all present verbatim, so a reader can
reconstruct the full lobby state from this block alone.

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
- **Selection** — `int32 SelectedObjectCount`, `uint32 SelectedObjectCRC` (plain sum of unique
  IDs), then that many `uint32` unique IDs. Readers reject counts above 4096.
- **SideChannel** — `int32 count` (max 64), then that many `SideChannelRecord` (329 bytes each).
- **Events** — `EventCountThisFrame` × `sizeof(EventClass)` (111 bytes, static-asserted in
  `YRpp/EventClass.h`).

Frames are only written when something changed, so frame numbers are sparse but monotonic.

`SideChannelRecord` carries chat, beacons and taunts, which travel over the game's global channel
rather than the deterministic event queue and so are reproduced directly rather than re-injected.
`Type` is `1 = ChatMessage, 2 = BeaconPlace, 3 = BeaconDelete, 4 = BeaconText, 5 = Taunt`; `Aux` is
overloaded per type (colour scheme index for chat, beacon slot 0–2 for beacons, raw command byte
for taunts).

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
| `ReplaySpectator` | `false` | Watch as an observer. Suppressed when `IsSinglePlayer`. |
| `ReplayShowChatAndBeacons` | `true` | Playback only; recording of this data is unconditional. |

Playback also overrides `Seed`, `GameSpeed`, `Protocol`, `FrameSendRate` and `MaxAhead` from the
header, skips `CreateConnections()`, and suppresses the statistics packet.

The spawner does **not** extract the embedded spawn.ini/spawnmap.ini for playback — it seeks past
them. The client is responsible for writing both files out before launching.
