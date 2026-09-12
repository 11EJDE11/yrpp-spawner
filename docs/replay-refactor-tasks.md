# Replay refactoring tasks

Work on one task at a time. Stop after each task for user review and game testing;
start the next only after the user gives the go-ahead. Preserve behavior and the
replay format throughout these extractions unless the user requests a change.

- [x] 1. Commands — built, tested, and accepted by the user. Move the shared registration helper and engine
  registration hook to `src/Commands`, with replay commands using the replay APIs.
  Preserve command names, translations, registration order, allocator, and hook address.
  Check: build; keyboard options lists all nine commands; existing bindings work;
  pause/resume, speed, single step, seek, control bar, viewport, and selection toggles.
- [x] 2. Keyframe sidecars — built, tested, and accepted by the user. Extract snapshot ownership and capture/restore code from
  `ReplaySeek`, grouping substantial planning, map, and Ares particle components.
  Preserve post-save capture and the distinct restore phases around session resume.
  Check: build; repeated forward/backward seeks, paused seeks, spectator seeks,
  replay CRC comparisons, keyframe eviction, and exit cleanup.
- [x] 3. Replay file container — built, tested, and accepted by the user. Extract headers, metadata, embedded INI handling,
  and file opening/finalization from `ReplaySystem`.
  Check: build; record/playback, existing replay compatibility, clean shutdown,
  incomplete recordings, and invalid header handling.
- [ ] 4. Frame encoding/decoding — implemented; awaiting user review and game testing. Separate frame serialization and parsing from
  simulation coordination, with explicit state ownership and validation boundaries.
  Check: build; sparse frames, gameplay events, selection triggers, speed changes,
  truncated input, stream repositioning, and CRC comparisons.
- [ ] 5. Side-channel ownership (replaces original Task 6) — recommended next; not started.
  Group chat/beacon/taunt capture, pending queue and scratch-buffer ownership, per-frame draining,
  validation, playback, and dedicated beacon/taunt hooks in ReplaySideChannels.
  ReplaySystem chooses when to drain and apply; the codec keeps the byte layout and calls the
  supplied validator. Keep the existing public recording API for InGameChat callers.
  Preserve frame assignment, ordering, overflow dropping, visibility settings, and seek behavior:
  chat/taunt effects are suppressed while seeking, while beacon state is reconstructed.
  Check: builds; local/remote chat, beacon placement/deletion/text, taunts, visibility settings,
  overflow handling, seeking, and cleanup between sessions.
- [ ] 6. Controls and pacing ownership (narrows original Task 7) — recommended after Task 5 review.
  Put the playback FPS, pause/single-step/resume state, deadline, and pacing implementation in
  the existing ReplayControls module. Add explicit start/stop and timing-reset entry points;
  seek completion and lifecycle callers should not write the pacing deadline directly.
  Keep simulation game speed distinct from the viewer's playback rate. Preserve frame-start
  ordering and leave the shared pause/observer simulation hooks together.
  Include small dependency cleanup: remove unused GetPlaybackTargetFPS, keep the clock helper
  private, and remove ReplayOverlay's unnecessary internal-state include/using directive.
  IsReplayHeaderValid also has no production callers and duplicates the real file validation;
  remove it after checking the temporary harness references rather than maintain two validators.
  Check: builds; speed ladder/options slider, pause/resume, single step, seek arrival, repeated
  playback sessions, and recorded game-speed changes.

Original Task 5 (broad viewer-state extraction) is deferred, not automatically queued after these.
Do not create another general viewer module just to shorten ReplaySystem. Reassess after the two
changes above; the intended stopping point is a ReplaySystem that coordinates lifecycle and frames.

Task 1: user confirmed the build and game test passed and authorized Task 2.
Task 2: Release and Release-CnCNetYR Win32 builds passed. Compared the moved repair code, snapshot fields, collection ordering, and capture/restore phases against the original source; only the expected ownership and parameter changes were found. The user confirmed game testing passed and accepted Task 2. Added concise reviewer comments for the sidecar lifecycle and each repair; verified these source changes are comments only.

Task 3: extracted ReplayFile.h/.cpp and ReplayFile.Metadata.cpp. Release and Release-CnCNetYR Win32 builds passed. A temporary standalone check linked to the real file/compression objects passed header and embedded-byte preservation, clean finalization, decompression and rewind, incomplete sync-flushed playback, malformed/truncated headers, and missing-file handling. Compared moved metadata and header validation routines against the original source; replay format and compression implementation are unchanged. The user confirmed game testing passed and accepted Task 3. Do not start Task 4 until authorized.

Task 3 follow-up: user requested removing INI sanitization and version handling so the client owns them. Removed address rewriting, the spawn.ini version parser/override, and playback version gating. Header layout and compiled DLL version metadata remain unchanged. Release and Release-CnCNetYR Win32 builds passed with zero warnings/errors. File regression checks passed, including acceptance of different version values and continued rejection of malformed/truncated input. Task 4 remains awaiting authorization.

Task 3 header follow-up: removed the four DLL-version bytes and the client's decoder/property/detail text. The header is now 1124 bytes; updated all client offsets in xna-cncnet-client/DXMainClient/Domain/ReplayGame.cs and the format documentation. The user explicitly permits breaking header changes during private development, with no version bump or backward compatibility; update both repositories together. DescribeReplayOpenFailure remains used by StartReplayPlayback for fatal open-error messages. Release and Release-CnCNetYR Win32 spawner builds passed; the WindowsDXRelease net8.0-windows client build passed with existing warnings. File checks and a C++-written fixture read by the actual client ReplayGame parser passed. Task 4 remains awaiting authorization.

Task 3 follow-ups: user confirmed the header cleanup and client changes are tested and working, then authorized Task 4.

Task 4: extracted ReplayFrameCodec.h/.cpp with capture/decoded-record types, FrameWriter change tracking, FrameReader ordering state, block serialization/parsing, and gameplay-event byte I/O. Engine capture, playback effects, and side-channel validation remain in ReplaySystem, with validation passed explicitly to the reader. Release and Release-CnCNetYR Win32 builds passed with zero warnings/errors. Compared moved writer/reader code against the pre-extraction source. A temporary standalone harness produced byte-identical encoded files and matched original decoding across 917 cases covering sparse frames, CRCs, selection/triggers, speed, side channels, gameplay events, stream rewind, all-block truncation, extension skipping, malformed counts/flags, and frame ordering. The replay format is unchanged; no client changes were needed. Do not start Task 5 until authorized.

Review after Task 4 (review only; no source changes):

- Side channels have a cohesive boundary already visible in the code: pending queue/draining,
  SanitizeSideChannelRecord, ApplySideChannelEvent, public capture functions, and four dedicated
  hooks. Moving ownership as well as functions removes dependencies from ReplaySystem and gives
  the codec validator a natural home without adding a general callback framework.
- Timing state currently spans ReplayControls, ReplayRuntimeState, lifecycle setup/reset, and a
  direct deadline write in Seek::EndSeek. Consolidating this existing responsibility is useful;
  introducing a new scheduler/service hierarchy is not.
- Viewer behavior is not all presentation. SelectedByPlayer springs are simulation inputs;
  observer hiding/restoration brackets LogicClass::AI and Queue_AI; keyframe load restores the
  viewer camera while stream scanning rebuilds recorded camera state. A broad viewer split would
  need a more careful boundary and could obscure ordering. Leave these coordination points visible.
- ReplayOverlay already uses the public replay/controls/seek APIs and does not access ReplayState.
  Its internal-header dependency is unnecessary. GetPlaybackTargetFPS has no callers, and
  PlaybackClockMilliseconds is used only by pacing. These are small cleanup opportunities.
- Keep one shared lifecycle/frame coordinator. Do not replace every ReplayState field with an
  accessor or split every hook region into another file. Further extraction should remove a real
  ownership problem, not satisfy the original task count.

This review supersedes the original Tasks 5–7. Both recommended tasks remain unstarted and require
the user's go-ahead, with review and game testing between them. Task 4's existing test status is
unchanged by this review.