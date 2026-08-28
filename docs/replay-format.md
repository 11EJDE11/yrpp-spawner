# Replay file format (`.yrrp`)

Defined in `src/Replay/ReplayFormat.h` and written by `src/Replay/ReplaySystem.cpp`; the hooks
that drive it live in `src/Replay/ReplaySystem.Hook.cpp`. The CnCNet client mirrors the header by hand in
`DXMainClient/Domain/ReplayGame.cs` — there is no compile-time link between the two, so **any
change here must be made in both repos in the same change**. A size mismatch does not error; it
silently misparses every field after the point of divergence.

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

An **additive** change keeps `Version` at 1. Older readers skip what they do not recognise:
`Reserved` words they do not know, header bytes past their own `sizeof`, and the length-prefixed
extension block. They keep playing the file.

An **incompatible** change bumps `REPLAY_VERSION`. Old readers then refuse the file, which is the
point of doing it. Leave `MIN_SUPPORTED_REPLAY_VERSION` where it is: a reader accepts the range
`MIN_SUPPORTED_REPLAY_VERSION`..`REPLAY_VERSION`, and that is what keeps every replay recorded
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
| 0 | 4 | `Magic` | `0x4A455259` |
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
shortening one array while lengthening another, leaves `sizeof` untouched and misparses everything
from the point of divergence onward. The per-field asserts are what catch that. None of them can
see `ReplayGame.cs` - the messages point at it, but keeping the two in step is still manual.

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

So recording is split across the call. `CaptureEventsForCurrentFrame`, at the event pump (0x64820E
for multiplayer, 0x647586 for campaign and skirmish), takes the *address* of every `DoList` entry
not yet consumed, without judging which of them this frame will reach. Once `Execute_DoList` has
returned (0x648234, and 0x6475B3 respectively) and before `Queue_AI` drops the spent entries,
`RecordCapturedEventsForCurrentFrame` copies out the ones the engine has now marked `IsExecuted`.

Two kinds of event are therefore absent from a frame that a naive capture would have put there: one
the engine rescheduled, which is recorded on the frame it finally runs on, and one it discarded as
`Packet received too late!`, which is never recorded because it never ran. Capturing before the call
recorded a rescheduled event *twice* - once where it was moved from, once where it ran - and
playback then executed an order the game never made.

`IsExecuted` is the marker to use here, and hooking `EventClass::Execute` (0x4C6CB0) instead - which
looks like the more direct way to record what ran - is a trap. `Execute_DoList` handles `FRAMEINFO`,
and `EXIT` and `OPTIONS` belonging to another player, inline and never calls `Execute` for them, so
that recording would lose every network timing sample and every remote quit. All of those paths do
reach `IsExecuted`, because all eleven of them end at the same place (`0x64CAA8`).

Events are written unexecuted, whichever side of execution they were copied from, so the same event
is always the same bytes on disk.

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
could step over it, so it means bumping `REPLAY_VERSION`.

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

### What the hash reads, and why full-map-reveal has to put a flag back

`Compute_Game_CRC` folds in every infantry, unit and building's coordinates and facings, the
`DisplayClass::Layer` and logic-queue objects' coordinates and types, that RNG draw - and, once per
house, the single byte at `HouseClass+0x241`, which is `MapIsClear`.

That last one is a trap for anything playback does to the local view. `MapClass::Reveal` (0x577D90,
`Clear_Shroud`) sets `house->MapIsClear = 1` as its very first statement, ahead of every guard in
it, and `MapClass::Reset_Shroud` (0x577AB0) clears it at 0x577ACA. So full-map-reveal playback
touches a hashed byte twice over: `MaintainFullMapReveal` calls Reveal, which sets the flag the
recording never had set, and the reshroud skip at 0x577AB8 bypasses the write that would have
cleared it again. Left alone, that makes the frame hash differ from the recording's from the first
frame onward, for a reason with nothing to do with the simulation - a permanent false positive on
the default playback settings.

Both sites therefore keep the flag at what the recorded simulation would have had: the reveal
snapshots it and puts it back, and the reshroud skip performs that one write by hand. This is safe
because nothing plays off the flag. `MapIsClear` is written only by the shroud reveal and reshroud
paths, and read by exactly three functions - `Compute_Game_CRC` (0x64DCCA), `HouseClass::Compute_CRC`
(0x502FFF) and `Print_CRCs_Current_Player` (0x64E250). All three are hashes or diagnostics; no
gameplay code reads it.

The rest of the playback options are clear of the hash: `MakeObserver` only sets
`HouseClass::Observer`, and the remaining shroud and gap-generator work that full-map-reveal skips
(0x6FB170, and the body of 0x577AB0) touches cell shroud counters, the radar and redraw flags -
never the RNG, and nothing else the hash reads. Anything added here that writes house state must be
checked against `HouseClass+0x241` specifically.

A hash costs 4 bytes plus, on frames that would otherwise have gone unrecorded, a 12-byte frame
header. The hashes themselves do not compress; the headers do.

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
| `ReplaySpectator` | `false` | Watch from an observer's seat rather than a player's. See below. |
| `ReplayShowChatAndBeacons` | `true` | Playback only; recording of this data is unconditional. |
| `ReplayPlaybackSpeed` | `-1` | Game speed index to pace playback at. `-1` falls back to `GameSpeed`. Does not affect the simulation, which stays pinned to `RecordedGameSpeed`. |
| `ReplayViewPlayer` | `-1` | Which player's screen to watch the recording from. See below. |

Playback speed is deliberately kept out of `OptionsClass Options.GameSpeed` (0xA8EB60): simulation
code reads that through `GetAnimSpeed`, so it stays pinned to `RecordedGameSpeed` for the whole of
playback. The in-game options dialog binds its speed slider to the same variable, so three hooks in
`ReplaySystem.Hook.cpp` (0x4E209E, 0x4E1E1B, 0x4E1EBA) give that dialog a view of the playback
speed instead. Recorded `GameSpeed` events are harvested for their requested speed and then dropped
rather than executed, so the engine never writes the pinned value.

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
which EVA lines play. A live multiplayer game already runs the identical simulation on every peer
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
