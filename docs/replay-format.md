# Replay file format (`.yrrp`)

Defined in `src/Replay/ReplayFormat.h` and written by `src/Replay/ReplaySystem.cpp`; the hooks
that drive it live in `src/Replay/ReplaySystem.Hook.cpp`. The CnCNet client mirrors the header by hand in
`DXMainClient/Domain/ReplayGame.cs` — there is no compile-time link between the two, so **any
change here must be made in both repos in the same change**. A size mismatch does not error; it
silently mis-parses every field after the point of divergence.

`GetReplayFPSFromGameSpeed` is duplicated the same way, as `ReplayGame.GetFramesPerSecond`. The
client uses it to turn `TotalFrames` into a displayed duration, so the two have to agree.

The client does not hide a replay it cannot read. A file with the right magic and a version outside
its supported range is listed greyed out, with the Watch button disabled and the details pane saying
the game needs updating. Dropping it from the list instead - which is what happens to anything
without the magic number - would look to the player like the update had deleted their recordings.

## Layout

```
[ReplayHeader]        HeaderSize bytes (1452 today), #pragma pack(1)
[spawn.ini]           SpawnIniSize bytes, verbatim text (sanitized, see below)
[spawnmap.ini]        SpawnMapSize bytes, verbatim text
--- everything past this point is one raw deflate stream ---
[frame records]       repeated, one per simulated frame, monotonic
[end-of-stream]       FrameRecordHeader with FrameNumber == -1
```

The header and the two embedded INIs are never compressed, so a reader can pull them out with a
plain seek and read. That is what the client does, and why it needs no decompressor.

Readers seek to the embedded spawn.ini by the header's own `HeaderSize`, never by their compiled
`sizeof(ReplayHeader)`. The two are equal for a file this build wrote and differ the moment a later
build appends a header field, which is the whole point of the field.

## Changing the format

Three things exist so that a change does not invalidate every replay already recorded, and all
three only work because the *reading* side of them shipped before anything used them. They cost
nothing until then.

| Want to add | Use | Version |
|---|---|---|
| A header field | `Reserved`, or append and grow `HeaderSize` | stays |
| Per-frame data | an `Extensions` block | stays |
| Anything that changes the meaning or position of an existing field | — | bump |

An **additive** change keeps `Version` at 1. Older readers skip what they do not recognize:
`Reserved` words they do not know, header bytes past their own `sizeof`, and the length-prefixed
extension block. They keep playing the file.

An **incompatible** change bumps `ReplayVersion`. Old readers then refuse the file, which is the
point of doing it. Leave `MinSupportedReplayVersion` where it is: a reader accepts the range
`MinSupportedReplayVersion`..`ReplayVersion`, and that is what keeps every replay recorded
before the break readable afterwards.

The first twelve bytes - `Magic`, `Version`, `HeaderSize` - are the only part of the file whose
meaning is fixed for all time. Nothing may be inserted ahead of them, and no reader may touch
anything after them until it has checked the version.

None of this has anything to do with whether a replay will play back *correctly*. A version 1 file
that this build parses perfectly will still diverge if the rules, the engine, Ares, Phobos or the
mixes moved underneath it; that is what the client's per-file `[ReplayFileHashes]` check is for, and
the two questions are answered separately and independently.

## ReplayHeader

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | `Magic` | `0x50525259` (`YRRP` in file order) |
| 4 | 4 | `Version` | `1` |
| 8 | 4 | `HeaderSize` | bytes from the start of the file to the embedded spawn.ini; what readers seek by |
| 12 | 260 | `MapName` | `spawnmap.ini [Basic] Name`, else spawn.ini `UIMapName`, else `ScenarioClass::FileName` |
| 272 | 4 | `SpawnerVersion{Major,Minor,Revision,Patch}` | four `uint8`; from spawn.ini `SpawnerVersion` if present, else the compiled-in `VERSION_*` |
| 276 | 64 | `GameClientVersion` | from spawn.ini `GameClientVersion` |
| 340 | 4 | `GameMode` | `SessionClass::GameMode` — Campaign 0, LAN 3, Internet 4, Skirmish 5 |
| 344 | 4 | `UniqueIDCounter` | `ScenarioClass::UniqueID` at recording start |
| 348 | 4 | `Seed` | `Game::Seed` |
| 352 | 4 | `RandomNext1` | |
| 356 | 4 | `RandomNext2` | |
| 360 | 1000 | `RandomizerTable[250]` | full RNG table snapshot |
| 1360 | 4 | `SpawnIniSize` | bytes of embedded spawn.ini |
| 1364 | 4 | `SpawnMapSize` | bytes of embedded spawnmap.ini |
| 1368 | 4 | `RecordedGameSpeed` | 0–6; validated on load |
| 1372 | 8 | `RecordedUnixTime` | `time()` at recording start |
| 1380 | 4 | `TotalFrames` | last frame that carried a record |
| 1384 | 4 | `Flags` | bit 0 = `CleanShutdown`; see below |
| 1388 | 64 | `Reserved[16]` | zeroed on write; space for header fields added without moving anything. A reader that meets a value it does not understand here ignores it — that is what makes claiming one additive |

Both version fields are written and read. `SpawnerVersion` identifies the DLL that produced the
file and the client shows it in the replay's details; nothing gates on it, and it is the first thing
worth having when someone reports a replay that will not play. No client writes the spawn.ini
`SpawnerVersion` key, so in practice the spawner's own compiled-in version is always what lands
here — which is the value you want anyway.

`TotalFrames` counts the last frame a record was *written* for. Every simulated frame carries a
record now that each one records a hash (see [Frame records](#frame-records)), so the two are the
same in practice. It is only ever used to show a duration.

`Flags` bit 0 is set by `StopReplaySystem` when it stamps `TotalFrames`, so a recording that died
with the process leaves both fields zero. That flag - not `TotalFrames > 0` - is what tells a
truncated recording apart from one that was quit on frame 0.

`IsReplayHeaderValid` checks `Magic`, that `Version` is inside the supported range, that
`HeaderSize` is at least this build's own header, and `RecordedGameSpeed <= 6`. A *larger*
`HeaderSize` is accepted on purpose: everything this build reads is still at the offsets above, and
the surplus is skipped. Playback also checks that `HeaderSize + SpawnIniSize + SpawnMapSize` fits
the file before seeking past them. There is no checksum.

`ClassifyReplayHeader` splits those checks apart so the reason survives into the message the player
sees. Playback failure is fatal — `StartScenario` has already skipped `CreateConnections`, so there
is nothing to fall back on — which makes that message the last thing anyone gets, and an unsupported
version says so rather than being folded into a generic read error.

`ReplayFormat.h` static-asserts `sizeof(ReplayHeader) == 1452`, `sizeof(FrameRecordHeader) == 12`
and `sizeof(SideChannelRecord) == 329`, **and** the individual `offsetof` of every header field the
client hardcodes. Size alone does not pin a layout: swapping two fields of the same width, or
shortening one array while lengthening another, leaves `sizeof` untouched and mis-parses everything
from the point of divergence onward. The per-field asserts are what catch that. None of them can
see `ReplayGame.cs` - the messages point at it, but keeping the two in step is still manual.

## Compression

The frame records are a single raw deflate stream (RFC 1951, no zlib wrapper), produced by
upstream miniz 3.0.2, vendored byte-identical in `src/Vendor/miniz/` and configured through the
`MINIZ_NO_*` defines on its `ClCompile` entry in `Spawner.vcxproj` (deflate and inflate only). It is
driven by `Replay::DeflateWriter` / `Replay::InflateReader` in `src/Replay/ReplayStream.{h,cpp}`.
See `src/Vendor/miniz/README.md` for provenance and how to verify it.

A truncated stream is detected by clearing `TINFL_FLAG_HAS_MORE_INPUT` once the file runs out and
treating the resulting `TINFL_STATUS_FAILED_CANNOT_MAKE_PROGRESS` as end of data. Do not reinstate
the pre-1.16 behavior of leaving the flag set: older miniz zero-padded the input and decoded
plausible-looking garbage past the end of a cut-short recording.

The stream is sync-flushed every 60 recorded frames. A sync flush ends the current deflate block
without resetting the dictionary, so everything written before it decodes on its own. That is what
keeps a recording a crash cut short usable: the reader inflates up to the last bytes that reached
the disk and then reports a short stream. At most one second of play is lost, and the header's
`CleanShutdown` flag stays clear, which is how a reader knows the recording is incomplete.

Independent per-block compression bounded to the same one second of loss needs 16 KB blocks, which
compress about 8% worse than the single stream because each block restarts the dictionary.

`TotalFrames` and `Flags` are stamped together by `StopReplaySystem()`, which seeks back to
`offsetof(ReplayHeader, TotalFrames)` just before the file is closed. A game that crashes or is killed never reaches that, so both
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

## Campaigns: one mission per recording

**A campaign recording covers the mission that was launched, and nothing after it.** Finish a
mission and the game moves straight on to the next one; that next mission is not recorded, and the
replay already on disk is the one that was just played.

The chain is the reason. `Do_Win` (0x685670) reads `[Basic] NextScenario` / `AltNextScenario` out of
the mission's own map, hands it to `Map_Select_Advance`, and calls `Start_Scenario` itself - all
without `Main_Game`'s frame loop exiting, and without the client or spawn.ini ever hearing about it.
A replay file, though, is built around the `spawn.ini` and `spawnmap.ini` the client wrote *before
the game started*, and those describe the launched mission only: its scenario name, its map, its
player. Recording the second mission would either need a second file the client knows nothing about,
or would leave the second mission's events sitting behind the first mission's scenario - which is
what used to happen, and plays back as the first mission's map driven by events that mean nothing on
it.

So `FinishRecordingAtMissionEnd` closes the recording out from the `Do_Win` hook - stamping
`TotalFrames` and `CleanShutdown` at the moment the mission ended, so quitting at the score screen
still leaves a complete file - and latches recording off for the rest of the launch
(`RecordingFinishedForSession`). `ReplaySystem::OnGameStartReset`, which runs once per game started
out of `Select_Game`, clears the latch.

Losing or restarting a mission is not the same case and is not hooked: both re-enter the *same*
scenario, whose spawn.ini is still the right one, so the retry records over the previous attempt and
the file describes the attempt the player actually finished with.

### Playback ends when the mission does

The same chain is why **watching** a campaign replay ends the game when the mission does. The score
screen still shows, and `Continue` is the way out of the replay - but `Continue` normally means
`Start_Scenario` for the next mission, a mission the replay has no events for and whose houses do
not match the ones playback set up. There is no Exit button to offer instead: the screen assumes a
campaign always continues. The victory and defeat movies are skipped; the score screen is the
mission's result and worth watching, minutes of full-screen video are not.

Three hooks handle it, all gated on `ReplayFile` being set:

| Hook | Site | Effect |
|---|---|---|
| `DoWin_SkipWinMovieDuringPlayback` | `loc_685915` | jumps to `0x68593A`, past `Play_Movie(WinMovie)` |
| `DoWin_EndGameAfterScoreScreen` | `loc_685965` | jumps to `0x685B2D` once the score screen is done |
| `DoLose_EndGameAfterPlayback` | `loc_686060` | jumps to `0x6863C3`, past the defeat movie and the replay prompt |

`loc_685965` is where the two score-screen branches meet - the one that presented it and the one a
map with `[Basic] SkipScore` takes past it - so playback leaves whether or not the mission wanted a
score screen. A campaign defeat has no score screen at all, which is why `Do_Lose` leaves at the
movie.

The two exit addresses are the functions' own tails, the ones vanilla takes for a one-time-only
mission and for a declined replay prompt. Both clear `GameActive`, which is what `Aux_Loop` returns,
so `Main_Game`'s loop breaks, `Spawner::StartGame` returns false on the next `Select_Game` call, and
the process exits back to the client. `StopReplaySystem` still runs on the way out, so the
divergence summary is still logged.

The gate is the `ReplayFile` key rather than `ReplayState.Playback`: a session launched to watch a
replay must not chain into another scenario even if playback itself stopped early on a read error.

## Frame records

`FrameRecordHeader` (12 bytes):

```
int32  FrameNumber          // -1 = end of stream
int32  EventCountThisFrame
uint32 Flags                // 1 = TacticalPos, 2 = Selection, 4 = SideChannel, 8 = GameCRC,
                            // 16 = Extensions
```

Then, in order, whichever blocks the flags select:

- **TacticalPos** — `Point2D` (two `int32`).
- **Selection** — `int32 SelectedObjectCount`, then that many `uint32` unique IDs. Readers
  reject counts above 4096.
- **SideChannel** — `int32 count` (max 64), then that many `SideChannelRecord` (329 bytes each).
- **GameCRC** — `uint32`, the engine's own state hash for the frame. See
  [Desync detection](#desync-detection).
- **Extensions** — `uint32 length` (max 1 MiB), then that many opaque bytes. Nothing writes one
  yet; see below.
- **Events** — `EventCountThisFrame` × `sizeof(EventClass)` (111 bytes, static-asserted in
  `YRpp/EventClass.h`). The events that *executed* on this frame, which is not the same as the
  events stamped with it - see below.

### Which events a frame holds

Recording an event on the frame its `Frame` field names would be wrong, because `Execute_DoList`
(0x64C380) rewrites that field before it executes anything: any event sitting between
`NewMaxAheadFrame1` and `NewMaxAheadFrame2` is moved to the latter and left in the queue unexecuted,
to run on that later frame instead. The engine logs each one as `DoList: Moving event from frame %d
to frame %d`.

So the frame an event belongs to is decided by the engine, and recording takes it from the one
instruction that decides it. `ExecuteDoList_RecordConsumedEvent` hooks `0x64CAC0`, where
`Execute_DoList` sets `IsExec` on a queue entry - the single point every path through the function
converges on. `EAX` there holds the engine's own scaled slot index, so the entry is at
`DoList.List + EAX * 3` (`sizeof(EventClass)` is 111, and the instruction addresses the flag as
`[eax + eax*2 + &DoList.List.IsExec]`). Each event is copied out as it is consumed, in the order the
engine consumed it. The two post-execute sites - `0x648234` for multiplayer, `0x6475B3` for campaign
and skirmish - then write the frame's collected events once the frame is known to be complete.

`MEGAMISSION` - what an ordinary unit order is - is the one type recorded elsewhere, by
`ExecuteDoList_RecordAcceptedMegaMission` at `0x64C75D`. It never runs in place: `Execute_DoList`
copies it into a second queue (`MegaMissionList`, `EventClass::MegaMissionList` in YRpp), marks the
original consumed, and runs the copy later, after `Queue_256_Sort` has reordered the batch. Reaching
the mark therefore does not mean the engine kept it - when that queue is already full at 256 entries
the copy is skipped and `0x64C743` jumps straight to the mark, so the order never happens. Recording
from the accept path instead keeps a dropped order out of the file; recording it would make playback
carry out an order the recorded game never did.

Three kinds of event are therefore absent from a frame that a naive capture would have put there:
one the engine rescheduled, which is recorded on the frame it finally runs on; one it discarded as
`Packet received too late!`, which is never recorded because it never ran; and a `MEGAMISSION`
dropped for want of room in `MegaMissionList`.

**Do not record by comparing `IsExec` before and after `Execute_DoList`.** That approach - take the
address of every unconsumed entry at the event pump, then copy out the ones marked afterwards -
looks equivalent and is not, because on RA2 the flag does not mean "ran". A `MEGAMISSION`, which is
what an ordinary unit order is, never executes in place: `Execute_DoList` copies it into a second
queue (`DoList_256`), marks the original consumed immediately, and runs the copy later, after
`Queue_256_Sort` has reordered the batch. The marked original stays in `DoList` until `Clean_DoList`
reaches it, so any later frame's scan sees it already-marked and skips it - and the event is lost
from the recording entirely. Measured on one recording, four of the five frames that carried an
event wrote nothing at all. Playback then runs a game where an order was never given, and diverges
one frame later.

Tiberian Sun has no `DoList_256` and executes every due event inline, so a design checked against
that source reads as correct. It is not; check `Execute_DoList` in the RA2 binary.

Hooking `EventClass::Execute` (0x4C6CB0) is the other trap, in the opposite direction:
`Execute_DoList` handles `FRAMEINFO`, and `EXIT` and `OPTIONS` belonging to another player, inline
and never calls `Execute` for them, so that recording would lose every network timing sample and
every remote quit. `0x64CAC0` is reached by all of them.

Events are written unexecuted, so the same event is always the same bytes on disk.

Blocks are stored bare, so a reader that meets a flag it does not know cannot find the end of that
block - the length is written down nowhere - and `ReadNextPlaybackFrameRecord` rejects the record.

They are written in the order listed above, which is *not* the numeric order of their flag bits:
`ObjectCensus` (bit 5) and `GameSpeed` (bit 6) are written ahead of `Extensions` (bit 4), because
the extension block has to stay physically last. Read them in the written order, not by walking the
flag bits.

The extension block is the exception, and the only way per-frame data gets added later without
invalidating recordings made before it. It carries its own length, so a reader skips it whole
without knowing anything about what is inside. It is always the last block in the record: anything
appended after it would be unreachable to a reader that skipped it. Nothing writes one today —
shipping the *skip* now, ahead of any use, is what buys the freedom later.

Adding a per-frame block that is *not* carried inside the extension block is an incompatible
change. It would have to take a lower flag bit to stay in flag order, and no reader in the field
could step over it, so it means bumping `ReplayVersion`.

Every simulated frame carries a record, because every frame records a hash. Frame numbers are
still only guaranteed monotonic rather than contiguous: a reader must not assume it can index them.

`SideChannelRecord` carries chat, beacons and taunts, which travel over the game's global channel
rather than the deterministic event queue and so are reproduced directly rather than re-injected.
`Type` is `1 = ChatMessage, 2 = BeaconPlace, 3 = BeaconDelete, 4 = BeaconText, 5 = Taunt`; `Aux` is
overloaded per type (colour scheme index for chat, beacon slot 0–2 for beacons, raw command byte
for taunts).

The three do not reach the recording player the same way, and that is what decides how much of
each a replay can hold.

Chat is filtered by the *sender*. `Message_Input` (0x55E420) picks a scope from the key that opened
the prompt - `0x0D`, `0x08` and `0x5C` set the scope global at 0xABCE18 to 1, 2 and 3 - and the
send loop at 0x55EF48 then walks `HouseClass::Array` and keeps only the houses that scope selects:
those whose `ChatMask` byte (0xA8D108) is set, mutual allies, or everyone. Each surviving house
gets its own `Send_Global_Message`, addressed to the connection whose `Connection_Name` matches
that house's `UIName`. Taunts do the same through `ChatMask` alone
(`TauntCommandClass::Process`, 0x536350).

Beacons do not. `BeaconManagerClass::Place` (0x430BA0) sends to every player node with no ally
check at all; the ally test on the receiving side only gates the radar ping and the EVA line, and
the `BeaconClass` is created in `Beacons[house][slot]` either way. Visibility is decided at draw
time by `Beacon_canbeseen` (0x4308B0), against `PlayerPtr`.

So a recording holds every beacon placed in the game, but only the chat and taunts the recording
player sent or was addressed to receive. A message between two other players never touched the
recording machine and is absent from the file rather than filtered on read.

Replays get shared, so these records are treated as untrusted on read.
`SanitizeSideChannelRecord` terminates `SenderName` and `Text` - the arrays are copied verbatim
off disk and need not be - and range-checks `House` against `BeaconManagerClass::Beacons[8][3]`
and `Aux` against the beacon slot count, `ColorScheme::Array`, or the 16 taunt commands. Records
that fail are still read, to keep the stream aligned, and then dropped.

## Desync detection

Playback is the recorded simulation re-run from a file instead of from the wire, so it can drift
off the recording exactly the way two network peers desync - a difference in game files, a rules
change, an engine change between spawner versions. Before this, that showed up only as a replay
that quietly stopped matching what happened, with nothing saying so.

The engine already computes the hash needed to catch it. `Compute_Game_CRC` (0x64DAB0) folds every
infantry, unit and building's coordinates and facings, every house's tally byte, the display layers
and the logic queue into `GameCRC` (0xAC51FC), and `Queue_AI_Multiplayer` calls it once per frame
and stores the result into `CRC[Frame & 0xFF]` for the network sync check. The replay system reads
that same value and never hashes anything itself.

`Queue_AI_Multiplayer_ReplayGameCRC` (0x647689) sits on the instruction straight after that call.
Recording stores the hash on the frame's pending capture; playback compares it against what the
recording wrote for the same frame. Both modes read it at that one site, which is what makes the
two values comparable: it is the same instant in the frame, after `LogicClass::AI` and before
`Execute_DoList` (0x64822F) applies the frame's events.

That site, and not the event pump at 0x64820E, because 0x64820E does not run every frame. With
`PacketProtocol` 2 the function services the connection and returns at 0x647F63 on every frame that
is not a multiple of `FrameSendRate`, long before reaching it. The hash site is ahead of that
return, so it is reached on every frame in both modes.

On a mismatch, playback logs the frame and both hashes and prints *"Replay playback has diverged
from the recording."* in-game, then **keeps playing**. A diverged replay is wrong, not broken, and
stopping would take away the only view of what actually went wrong.

The in-game message is shown once, on the first mismatch, but the comparison keeps running to the
end of playback. Whether a mismatch persists or clears up on the next frame is the difference
between the simulation having actually come apart and something being off for a moment in a value
that only the hash reads, and stopping at the first one throws that distinction away. The first ten
divergences are logged with their frame and both hashes, a return to matching is logged with the
range it covered, and a total is written when playback ends.

Nothing in this path may call `Compute_Game_CRC` itself: its last act is to draw from
`ScenarioClass::Random` **and fold the drawn number into the hash**, which makes it part of the
simulation. An extra call would pull the RNG out of step and cause the very divergence it is there
to report.

### What the hash reads

`Compute_Game_CRC` folds in every infantry, unit and building's coordinates and facings, the
`DisplayClass::Layer` and logic-queue objects' coordinates and types, that RNG draw - and, once per
house, the single byte at `HouseClass+0x241`, which is `MapIsClear`.

That last one is a trap for anything playback does to the local view, because `MapClass::Reveal`
(0x577D90, `Clear_Shroud`) sets `house->MapIsClear = 1` as its very first statement, ahead of every
guard in it, and `MapClass::Reset_Shroud` (0x577AB0) clears it at 0x577ACA. The viewer's reveal
never calls either function (see "Showing the whole map" below), so the flag is left where the
recorded simulation put it. Nothing plays off it in any case: `MapIsClear` is read by exactly three
functions - `Compute_Game_CRC` (0x64DCCA), `HouseClass::Compute_CRC` (0x502FFF) and
`Print_CRCs_Current_Player` (0x64E250) - all hashes or diagnostics.

`MakeObserver` is clear of the hash too; it only sets `HouseClass::Observer`. Anything added here
that writes house state must be checked against `HouseClass+0x241` specifically.

A hash costs 4 bytes plus, on frames that would otherwise have gone unrecorded, a 12-byte frame
header. The hashes themselves do not compress; the headers do.

## Relevant spawn.ini keys

All in `[Settings]`. Recording and playback are mutually exclusive — a non-empty `ReplayFile` wins.

| Key | Default | Meaning |
|---|---|---|
| `EnableReplayRecording` | `false` | Record this game. |
| `ReplayFileOut` | `replay.yrrp` | Where the recording is written. Relative to the game directory; missing directories are created. |
| `ReplayFile` | *(empty)* | Non-empty switches the session to playback of that file. |
| `ReplayShroudEnabled` | `false` | Keep the recording player's shroud instead of revealing the map. |
| `ReplayLockedViewport` | `true` | Pin the camera to the recorded position. |
| `ReplaySelectUnits` | `true` | Reproduce the recorded unit selection. |
| `ReplaySpectator` | `false` | Watch from an observer's seat rather than a player's. See below. |
| `ReplayShowChatAndBeacons` | `true` | Playback only; recording of this data is unconditional. |
| `ReplayPlaybackSpeed` | `-1` | Game speed index to pace playback at. `-1` falls back to `GameSpeed`. Does not affect the simulation, which stays pinned to `RecordedGameSpeed`. |
| `ReplayViewPlayer` | `-1` | Which player's screen to watch the recording from. See below. |
| `ReplayControlBar` | `true` | Draw the on-screen playback controls during playback. See below. |
| `ReplayKeyframeInterval` | `750` | Frames between playback keyframes, which is what makes seeking backwards possible. `0` takes none. See below. |

Playback speed is deliberately kept out of `OptionsClass Options.GameSpeed` (0xA8EB60): simulation
code reads that through `GetAnimSpeed`, so it stays pinned to `RecordedGameSpeed` for the whole of
playback. The in-game options dialog binds its speed slider to the same variable, so three hooks in
`ReplaySystem.Hook.cpp` (0x4E209E, 0x4E1E1B, 0x4E1EBA) give that dialog a view of the playback
speed instead. Recorded `GameSpeed` events are harvested for their requested speed and then dropped
rather than executed, so the engine never writes the pinned value.

## Seeking, and the playback controls

Playback is a deterministic recurrence: the state at frame N follows from the state at frame 0 and
the events up to N. Seeking forward is therefore just running the simulation without drawing it,
but seeking backwards needs an earlier state to restart from, and there is no way to run a frame
in reverse.

Those states are **not** in the replay file - they would dwarf the events many times over, and the
file is meant to be small enough to hand around. Instead playback drops one every
`ReplayKeyframeInterval` frames as it watches, using the engine's own savegame format
(`ScenarioClass::SaveGame` 0x67CEF0). That only makes the part of the replay already watched cheap
to rewind into, which is the part a viewer wants to rewind into. Nothing about the `.yrrp` layout
changes, and a replay recorded by a build without any of this plays back identically.

A seek runs in two halves, in `src/Replay/ReplaySeek.cpp`:

1. **Land on a keyframe.** Skipped entirely when the target is ahead of where playback already is.
   Otherwise the newest keyframe at or before the target is loaded through
   `LoadOptionsClass::LoadMission` (0x559D60), the engine's own in-game load.
2. **Run to the target.** Frames go by with the pacing off (`ApplyPlaybackFramePacing` returns
   early), the drawing mostly off (`MainLoop_SkipRenderWhileSeeking` at 0x55D8F2 keeps one frame
   in 61 so it reads as progress rather than as a hang) and the sound off (`VocAllowed` at
   0x8464AC, which every `VocClass::Play` path tests, is cleared for the duration).

Keyframes live in `<SavedGameDir>\Replay Keyframes`, are written when playback starts and are
deleted when it ends. They are savegames, so they cost a few MB each; a 30 minute recording at the
default interval leaves a few dozen of them while it is being watched.

Two things a load has to be protected from. The scenario-start hooks (0x685659 and 0x686B6A) would
otherwise reopen the replay from frame 0 and re-seed the RNG over the top of the state just
restored, so both stand down while `ReplaySystem::Seek::IsLoadInProgress()` holds. And the frame
stream is deflated, which has no random access: `RepositionPlaybackStreamToFrame` restarts the
decompressor at the top and walks forward, re-applying the sticky state - viewport, game speed -
carried by the sparse records it steps over. Inflating the whole stream is far cheaper than the
simulation the seek is about to run anyway.

Because a seek replays hundreds of frames in a second or two, recorded chat and taunts are dropped
while one runs - they are momentary, and replaying them all would land the lot on screen at once
with their lifetimes starting from the seek. Beacons are state that outlives the frame that placed
them, so those are applied either way.

### The control bar

`ReplayControlBar` draws a media-player panel along the bottom of the tactical view
(`src/Replay/ReplayOverlay.cpp`): jump to start, step back a frame, slower, play/pause, faster,
step forward a frame, jump to end; a clock; the speed as a multiple of the recorded speed; and a
seek bar that can be clicked or dragged, ticked with the frames a backwards seek can still reach.

It is drawn from `GScreenClass::Render` at 0x4F4583 - after the sidebar, the message list and the
tooltips, before the mouse cursor - so nothing in the game covers it and the cursor stays on top
of it. Input is taken at 0x5BDF13, the point `MouseClass::AI` hands off to the rest of the input
chain. While the pointer is over the panel, or holding the seek handle wherever it has been
dragged to, that whole chain is skipped: a click on a button is not also a click on the map behind
it, and the bottom screen edge under the panel does not scroll the view. Keyboard input is
unaffected, because `Main_Loop` runs `Keyboard_Process` on the same key afterwards.

Every button also has a keyboard command in the `Replay` category of the in-game keyboard options
(`ReplayStepFrame`, `ReplaySeekBack`, `ReplaySeekForward`, `ReplayToggleControlBar`, alongside the
existing pause and speed ones), so the bar can be hidden and the replay driven entirely from the
keyboard.

With `ReplayKeyframeInterval = 0` no keyframes are taken: seeking forward still works, seeking
backwards does not, and the two buttons that need it are drawn greyed out.

`ReplayPlaybackSpeed` only sets the speed playback *starts* at; from there the hotkeys below own it.

Playback also overrides `Seed`, `GameSpeed`, `Protocol`, `FrameSendRate` and `MaxAhead` from the
header, skips `CreateConnections()`, and suppresses the statistics packet. `Spawner/Statistics.cpp`
does that last one by jumping to `0x64820E`, which is the address of the replay system's own
event-pump hook - moving that hook means moving that jump target with it.

The spawner does **not** extract the embedded spawn.ini/spawnmap.ini for playback — it seeks past
them. The client is responsible for writing both files out before launching.

## Playback controls

Three `CommandClass` commands, registered from `Init_Commands` (0x533066) and implemented in
`Replay/ReplayControls.{h,cpp}`. They show up in the in-game keyboard options under a `Replay`
category and are bound from there, or by hand in `KEYBOARDMD.INI`:

```ini
[Hotkey]
ReplayTogglePause=48    ; '0'
ReplaySpeedUp=187       ; VK_OEM_PLUS, the '='/'+' key
ReplaySpeedDown=189     ; VK_OEM_MINUS, the '-' key
```

They ship unbound on purpose. `Init_Commands` runs once per process rather than once per session, so
a default would apply to normal games too, and every plausible key is already spoken for by vanilla
(`0` is `TeamSelect_10`).

The hook sits at the same address Phobos registers its own commands from, so the two agree on where
in `CommandClass::Array` third-party commands land.

Their names and descriptions come from the string table, each with an English fallback, so a client
package can translate them by adding these labels to its CSF. Without them the fallbacks are used
and nothing breaks:

| Label | Fallback |
|---|---|
| `TXT_REPLAY_CATEGORY` | Replay |
| `TXT_REPLAY_PAUSE` | Replay: Pause/Resume |
| `TXT_REPLAY_PAUSE_DESC` | Freezes replay playback. The map can still be scrolled while paused. |
| `TXT_REPLAY_SPEED_UP` | Replay: Speed Up |
| `TXT_REPLAY_SPEED_UP_DESC` | Plays the replay back faster, past the speed a live game allows. |
| `TXT_REPLAY_SLOW_DOWN` | Replay: Slow Down |
| `TXT_REPLAY_SLOW_DOWN_DESC` | Plays the replay back slower. |

### Speed

Playback paces off its own target frame rate rather than a game-speed index, because the ladder runs
past what any index can express:

    10  12  15  20  30  45  60  90  120  180  240  300  500  1000  2000 FPS

The first seven rungs are exactly the rates the game-speed slider produces (index 6 down to index
0), so the slider and the hotkeys agree wherever they overlap; above 60 FPS the slider reports its
fastest position, so opening the options dialog and leaving it alone cannot silently slow playback
back down.

Nothing caps the engine at 60 FPS — `GetReplayFPSFromGameSpeed` simply topped out there.
`Main_Loop` turns `Game::Network::RequestedFPS` into two timers: `FrameTimer.DelayTime =
60 / RequestedFPS` in 60Hz ticks (0x55D501) and `NFTTimer.Accumulated = 1000 / RequestedFPS` in
milliseconds (0x55D522). Past 60 FPS the first floors to zero and stops capping anything, and the
second — the one `Sync_Delay` actually waits on, because a replayable session is never campaign or
skirmish — carries the pacing the rest of the way. A speed change lands on the next iteration, since
those timers are set before the hook that applies it (0x55D878).

That millisecond division is what sets the spacing at the top of the ladder. `MSTimerClass` has no
resolution below 1 ms, so above 240 FPS only rungs that divide to a different millisecond do
anything — 300 → 3 ms, 500 → 2 ms, 1000 → 1 ms — and anything past 1000 divides to 0, which
`Sync_Delay` treats as "expired, do not wait" and skips its wait loop entirely. 2000 is that
uncapped rung. It does not deliver 2000 FPS: every iteration still simulates and draws a full frame,
so what you get is whatever the machine manages, typically a few hundred. Adding rungs between these
would cost a keypress and change nothing.

Playback speed does not reach the simulation. The only place `Game::Network::RequestedFPS` feeds
anything deterministic is `HouseClass::Begin_Production` (0x4FA661), which sets a starting
`FactoryClass::Production.Value` of `min(53, 54 * (RequestedFPS * MaxAhead / 60) / Time_To_Build)` —
the head start compensating for the frames a live game takes to get a produce event executed. That
write is gated on a per-category `HouseClass` flag (0x53D0-0x53D8) whose only setter is
`SidebarClass::StripClass::SelectClass::Action` (0x6AB4E3, 0x6AB737), a local sidebar cameo click on
`PlayerPtr`. Playback injects recorded produce events straight into `EventClass::DoList` and nobody
clicks the sidebar, so the flag is never set and the branch is never taken — `RequestedFPS` and
`MaxAhead` both stay out of the simulation no matter what speed the viewer picks.

### Pause

Pausing has to hold the simulation while the rest of `Main_Loop` carries on, so that the viewer can
still scroll, drag-select, open the options dialog and press the unpause key. The engine's own pause
(`Scen->someloopcount_62C`, honoured at 0x55D821) cannot do that — it is campaign/skirmish only,
locks user input and never reaches `Keyboard_Process`, so nothing could unpause it. Standing in for
the whole function the way Phobos's frame-step mode does at 0x55D360 loses the dialog and scroll
handling with it.

Instead three hooks cut exactly the three things that advance the game out of an otherwise normal
iteration:

| Hook | Instruction | Skips |
|---|---|---|
| 0x55DC99 → 0x55DCA3 | `mov ecx, offset Logic` | `LogicClass::AI` (0x55DC9E) |
| 0x55DE3A → 0x55DE45 | `mov ProcessingFrames, ecx` | `Queue_AI` (0x55DE40) |
| 0x55DE73 → 0x55DE9A | `mov edx, Frame` | `++Frame` (0x55DE7E) |

Each hooks a position-independent instruction rather than the relative `call` next to it, so the
not-paused path can return 0 and re-execute the stolen bytes from Syringe's trampoline safely.

Skipping `LogicClass::AI` takes panning with it, so the last hook puts it back — as a pair, not a
single call. `Tactical::AI` (0x6D2540) commits `_DesiredTacticalCoord` — where the arrow-key
`Scroll_Map` calls (0x55DD32-0x55DD96) and mouse edge scrolling reached through
`GScreenClass::Input` → `ScrollClass::Scroll_AI` both record the viewer's scrolling — into the
`_TacticalCoord` that gets drawn, and `LogicClass::AI` is its only per-frame caller (0x55B667).
`GScreenClass::Render` is then what consumes the redraw state that commit leaves behind: it clears
`GScreenClass::Bitfield` as it goes (0x4F44A8) and takes its scroll-blit delta off the coordinate
pair `Tactical::AI` just wrote. Both sequences the engine gets a paused frame out of run the two
back to back in that order — its own pause branch (0x55D835-0x55D854) and the extra pass
`Sync_Delay` makes (0x55E253-0x55E271) — which is why the hook sits at 0x55DE73, after `Main_Loop`'s
own `Render` has come and gone, rather than at the skip itself.

That `Sync_Delay` pass is also the whole reason this looked speed-dependent: it is what kept paused
panning alive without any help at all, and `Sync_Delay` only makes it while more than 10 ms of the
frame budget is left. `NFTTimer.Accumulated = 1000 / RequestedFPS` is 16 ms at 60 FPS against 8 ms
at 120, so it quietly stops happening above 60.

A paused frame reaches `Tactical::AI` with `LastAIFrame == Frame` already, so all that runs is the
commit — not the waypoint animation or the `SpecialFlag` camera lerp ahead of it.

The three move together, and `RestoreFrameState` is held with them (0x55D878). Leaving `Queue_AI` in
would replay the current frame's events once per paused iteration; leaving the frame counter in
would walk `Frame` past the record `RestoreFrameState` is holding, which it reports as a frame
mismatch and stops playback over; and running `RestoreFrameState` again would clear
`ExpectedEventsThisFrame` for a frame whose events are still sitting unread in the file, so every
later frame would parse the wrong bytes. Held together, a paused iteration touches no replay state
at all.

Everything ahead of those three still runs — input, keyboard commands, rendering, arrow-key and edge
scrolling, the message list — and `Sync_Delay` still paces the loop, so a paused replay does not spin
the CPU.

## Showing the whole map

`ReplayShroudEnabled = false`, and spectating, both ask playback to reveal the map. Doing that the
obvious way - calling `MapClass::Reveal` on the local house - **breaks campaign playback**, and the
reason is worth stating plainly because it is not obvious: *in a campaign the local player's shroud
is simulation state.*

Two separate mechanisms make it so.

1. `HouseClass::Visionary` (`HouseClass+0x240`), which `Clear_Shroud` sets, is read at the top of
   four trigger actions, each of which returns immediately while it is set:
   `TActionClass::Reveal_Around_Waypoint` (0x6E0FE0), `Reveal_Zone_Of_Waypoint` (0x6E11C0),
   `Reveal_All_Map` (0x6E1330) and `Reshroud_Map_At` (0x6E1A70).
2. Those actions exist to call `MapClass::Sight_From` (0x5678E0), and revealing a cell is what
   discovers the objects standing on it:
   `Sight_From` -> `DisplayClass::Map_Cell2` (0x4A9CA0) -> `MapClass::reveal_check` (0x5865F0) ->
   `TechnoClass::Revealed` (0x6F4960) -> `TagClass::RaiseEvent(TEVENT_DISCOVERED, techno)`.
   `Map_Cell2` only reaches `reveal_check` when the cell's visibility actually *changed*, so a
   pre-revealed map produces no discovery callbacks at all.

`TechnoClass::Revealed` is simulation code: besides that tag spring it assigns `MISSION_HUNT` to an
AI unit sitting on `MISSION_AMBUSH`. And the spring is not merely a reordering - a trigger whose
event is already satisfiable fires the moment anything springs its tag, so a discovery can fire a
trigger *in the middle of another trigger's action list*. Allied Mission 1 does exactly this: the
`Create Thugs` action list enables `Change House 5` (elapsed-time-0) and then reveals waypoint 68,
and the GI discovered by that reveal springs `Change House 5` on the same frame. With the map
already revealed it waits for the next global tag pass instead, one frame later, and the campaign
drifts from there.

So the viewer must not touch the shroud the simulation sees - and the cleanest way to honour that
is to never write to it at all. The reveal is a lie told at draw time: every cell keeps exactly the
shroud the simulation gave it, and five hooks make the *drawing* behave as though the map were
clear. Nothing has to be restored, so no ordering between the render and the logic has to hold, and
a playback that stops mid-frame cannot leave a revealed map behind.

The shroud only ever becomes visible through two places, both of them pure render code:

- `CellClass::Cell_Shadow_Update` (0x4801F0) is the only caller of `CellClass::Draw_Shroud`
  (0x47EFE0) and `CellClass::Draw_Fog` (0x47F250), and is itself only reached from
  `Tactical::Render_Shroud` (0x6D3660) and `Render_Shroud2` (0x6D71E0), both only from
  `Tactical::Render`. Nothing else paints the black. The two hooks at 0x48022B and 0x480249 skip the
  shroud blit and the fog blit respectively, and deliberately leave the `Tactical::Cell_Shadow`
  (0x6D8700) calls above them in place, so the cell's `Visibility` and `Foggedness` caches are
  refreshed exactly as they would be in an untouched game. That matters: those two fields are inputs
  to the `changed?` test in `Map_Cell`, so writing them - as an earlier attempt did - is itself a
  way to lose a discovery.
- `CellClass::Is_Shrouded` (0x487950) is the cull each object draw performs on itself. All nine
  callers are `AnimClass::Draw_It`, `BuildingClass::Draw_It`, `Draw_Door_And_Turret`,
  `InfantryClass::Draw_It`, `Tactical::Draw_Pixel_Effects`, `TechnoClass::Draw_Extras`,
  `Techno_Draw_Object` and `UnitClass::Draw_Object` - every one a draw routine - so the hook at
  0x487950 shows every object on the revealed map.

Terrain needs no hook: `Tactical::Render_Tiles`, `Render_Layers`, `Render_Buildings`,
`Render_Terrain_Objects` and `Render_Cell` read no cell shroud state at all. Unexplored ground is
drawn like any other and then covered by the shroud shape, so not painting the shape is the whole
job.

Do **not** be tempted to hook `MapClass::Is_Mapped` (0x586360), the function
`CellClass::Is_Shrouded` wraps. It has around twenty-five other callers and a good half of them are
simulation code - `TechnoClass::Fire_At`, `AircraftClass::Fire_At`, `FootClass::Per_Cell_Process`,
`Tiberium_Check`, `Weed_Check`, `JumpjetLocomotionClass::Process`, `AircraftClass::Can_Enter_Cell`
and `DisplayClass::Passes_Proximity_Check2` among them. The `CellClass` wrapper is the chokepoint
that is draw-only; the `MapClass` one is not.

### The minimap

Each minimap pixel is blacked out by the same `MapClass::Is_Mapped` test, in
`RadarClass_draw_655C50` (0x655C50) - which is also why only player-controlled units show through
shroud there. The routine resolves the cell into two stack locals, `[esp+0x44]` (shrouded) and
`[esp+0x13]` (covered by a gap generator), and reads neither until 0x655D87. The hook there clears
both during a revealing playback, so the pixel is copied from the terrain image instead of blacked
out and enemy blips are let through.

Being a per-pixel lie rather than a cell edit, this needs no cache management of its own. The
minimap is a cached surface that `RadarClass::Draw_It` only repaints where something called
`Push_Cell`, but the full repaint it gets from `Read_Scenario_INI` (`RadarClass::Update_Map`
0x657CE0, called at 0x687C84) already runs with the hook live - `StartReplayPlayback` happens
earlier, in `Clear_Scenario` - and every later engine-driven full repaint (`Reset_Shroud`,
`Create_Gap`, `Delete_Gap`) goes through the same hooked routine.

Blips are gated once more, separately. `TechnoClass::Radar_Tracking_AI` (0x70D990) only calls
`Radar_Track_Object` when the object's virtual `Is_Visible_On_Radar` says so, and that test is
shroud-based - see `BuildingClass::Is_Visible_On_Radar` (0x457020). It runs inside the logic frame,
so the hook at 0x70DA48 forces the answer during full-map playback, for anything not flagged
`Invisible` or `RadarInvisible`.

Note `MapClass::Is_Mapped` is named backwards in the IDB: it returns `(AltFlags & Mapped) == 0`, so
it is true when a cell is *shrouded*.

### Clicking on what the reveal shows

`ScrollClass::Coords_To_Click_Info` (0x692300) resolves the cursor into a cell, then writes a
`shrouded` flag and a `fogged` flag for its caller and only picks an object when both are clear:

```
*shrouded = MapClass::Is_Mapped(coord) && MainWindow;
*fogged   = <building IsFogged, else MapClass::Is_Location_Fogged(coord)>;
if (!*shrouded && !*fogged) *object = Tactical::Select_At(point);
```

The hook at 0x6924FC clears both flags during a revealing playback and then re-implements the two
tests, so the viewer can click and hover anything the reveal has drawn. It composes with the
`Is_Shrouded` hook rather than duplicating it: `Tactical::Select_At` (0x6DA380) picks out of
`TacticalSelectables`, which is filled during the draw pass, so an object only becomes clickable
because it was drawn in the first place.

Letting local clicks through cannot reach the simulation. `RemoveReplayGameplayEventsFromDoList`
strips every locally generated gameplay event from `EventClass::DoList` before it executes, so a
click can change what this machine has selected and nothing else. Cloaked units stay unpickable
either way - `Select_At` and `Coords_To_Click_Info` both test `CellClass::SensedByHouses`, which the
reveal does not touch, and revealing the map is not the same as detecting cloak.

Two things the reveal deliberately does not change. Buildings that the recording player left under
fog still draw from their `FoggedObjectClass` snapshot rather than live, because `Tactical::
Draw_Buildings` skips a building whose `IsFogged` flag is set and that flag is written by logic, not
by drawing - it is the remembered building, which is also what the state-swapping version showed.
And the cursor's *action* over unexplored ground still comes from the shroud-based tests in
`FootClass::What_Action` and friends; playback discards the resulting orders regardless, so only the
cursor glyph is affected.

## Watching as a spectator

`ReplaySpectator` hands the local screen to the engine's own observer - the same
`HouseClass::Observer` a spectator in a live match gets - so the whole presentation follows without
a hook per symptom: the observer sidebar with every player's score, the full map, cloaked and
disguised units visible, pips on everything, and no "you have been defeated" message. All of it is
presentation. The recording player's house is left exactly as the recording has it, so the
simulation is unchanged and the frame hashes still match.

Two things about the timing, both in `ReplaySystem::ApplyPlaybackSpectator`:

- It runs **after** the scenario has loaded. The observer sidebar's player list is built during the
  load and leaves out whichever house is the observer, so setting this earlier would drop the
  player being watched out of their own scoreboard.
- `Game::ObserverMode`, which the loading screen reads, is set **before** the scenario starts, next
  to where a real observer's slot sets it.

It is applied from `Spawner::StartGame` rather than from inside `Spawner::StartScenario`, which is
what it took to make it work for a skirmish replay: `StartScenario` returns from a different branch
for each session type, and the spectator setup used to sit in the multiplayer one. It has to happen
after house assignment either way, since that clears the observer itself.

EVA is silenced while spectating (the `Speak` hook). Those lines - "construction complete", "our
base is under attack" - are addressed to the player who recorded the game, and were the loudest
remaining sign that playback was still sitting in that player's seat.

A campaign replay can be spectated too. `Misc/Observers.cpp` otherwise refuses the observer sidebar
outright for a campaign - a mission has no observers - so that one hook now makes an exception for a
replay being watched from an observer's seat, and the scoreboard lists the mission's AI houses,
which are all it has to list. The rest of the campaign-gated observer hooks are loading-screen
cosmetics and are left alone.

The shroud setting has nothing to act on while spectating: an observer sees the whole map by
definition, and `PlaybackWantsFullMapReveal` reveals it whether or not `ReplayShroudEnabled` is set.
The client greys the shroud box and the "Watch as" drop-down out when Spectator view is ticked, and
launches with no perspective, rather than leaving controls that do nothing.

## Watching from another player

`ReplayViewPlayer` names a spawn.ini player slot by index: `0` is `[Settings]`, which is always the
player who made the recording, and `N` is `[OtherN]`. `-1` and `0` both mean the recording player,
which is the default. Anything that is not a human slot - an AI, an empty slot, an out of range
index, or any value at all in a campaign recording - falls back to the recording player and logs
why. The client's dropdown writes the index; nothing else about the spawn.ini changes.

`ReplaySystem::ApplyPlaybackViewPlayer` resolves the slot to a house and points
`HouseClass::CurrentPlayer` (`PlayerPtr`, 0xA83D4C) at it, moving `IsInPlayerControl` across with
it. Slot to house goes slot -> `NodeNameType` -> `NodeNameType::HouseIndex`, which `Assign_Houses`
stamps: `StartScenario` creates one node per human slot in slot order, and `Assign_Houses` orders
`HouseClass::Array` by player colour. That ordering is a pure function of the spawn.ini, so it is
the same on every peer - which is also why the recorded events' house indices mean the same thing
here as they did on the recording machine.

This does not put the simulation at risk of drifting off the recording. `PlayerPtr` is the engine's
*local* viewpoint: it decides what is rendered, what the shroud covers (`MapClass::Sight_From`
resolves a sighting house to `PlayerPtr` before clearing any cell), whose sidebar is built, and
which EVA lines play. The one caveat is the shroud, which is not purely local in a campaign - see
"Showing the whole map" - but `ReplayViewPlayer` only ever resolves to another *human* slot, so a
campaign recording, which has one, always falls back to the recording player. A live multiplayer game already runs the identical simulation on every peer
with `PlayerPtr` pointing at a different house, so anything that leaked from it into the simulation
would desync ordinary games. The recorded event stream is house indexed and is replayed unchanged.

Timing is the part that is not free to choose. The call sits in `Spawner::AssignHouses`, straight
after `ScenarioClass::AssignHouses` (0x687F10) - which is both what creates the houses and what
stamps `HouseIndex` - and before `Read_Scenario_INI` reads the map's objects. Every piece of state
the engine derives from the local player at object creation time (ownership flags, shroud, the
sidebar's buildables, the initial camera position from `DisplayClass::Compute_Start_Pos`) is then
computed once, for the right house. It is also ahead of the observer handling in the same function,
so `MakeObserver()` - which only acts on the current player - sees the house being watched from.

`ReplayLockedViewport` and `ReplaySelectUnits` are forced off whenever the view player is not the
recording player. Both reproduce the recording player's own screen, and neither means anything from
someone else's seat: the camera would be pinned to a base you are not watching, and the selection
would be units the house you are watching does not own and mostly cannot see.

`Game::PlayerColor` and `Game::ObserverMode` follow the view slot rather than slot 0, so chat
colouring and the observer loading screen match who is being watched.

Beacons follow the viewpoint on their own, without anything here having to help: they are recorded
for every house, and `Beacon_canbeseen` re-tests each one against the new `PlayerPtr`, so another
seat shows that player's allies' beacons and hides their enemies'.

Chat and taunts cannot follow it, and no amount of work on this side would change that - the sender
decided who received them, so a message between two other players is not in the file to begin with.
Playback shows what the recording player sent and received, whoever is being watched. See
[Frame records](#frame-records).
