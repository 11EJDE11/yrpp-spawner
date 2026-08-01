# Netcode packet-loss test plan

Eight 3-minute runs. Every run writes `netlog_<NetTestName>_<pid>.txt` into the game
directory. Collect one file per player per run.

## Before you start

**Every run needs these two lines**, regardless of which fix is under test:

```ini
[Settings]
NetTelemetry=true
NetTestName=<see table>
```

`NetTelemetry` only observes — it changes no behaviour — but without it there is no
log. `NetTestName` names the output file so runs do not get confused.

Note the log is written by the spawner itself, not through `debug.log`.
`Debug::Log` routes to `WWDebugString` at `0x4068E0`, which is an **empty stub** in
the retail executable, so anything sent there is discarded.

## The matrix

Set every listed key explicitly. Anything not listed keeps its default, which is
*on* for the four measured fixes — so a run that omits a key is not a clean test.

| # | `NetTestName` | Keys to set |
|---|---|---|
| 1 | `baseline` | `FrameAwareGate=false` `FastRetransmit=false` `RetransmitBackoff=false` `PacketRedundancy=false` `StallCounterFix=false` `RenderThrottle=false` `SendClockDrift=false` |
| 2 | `stallfix` | baseline, but `StallCounterFix=true` |
| 3 | `gate` | baseline, but `FrameAwareGate=true` |
| 4 | `retransmit` | baseline, but `FastRetransmit=true` `RetransmitBackoff=true` |
| 5 | `redundancy` | baseline, but `PacketRedundancy=true` |
| 6 | `render` | baseline, but `RenderThrottle=true` |
| 7 | `sendclock` | baseline, but `SendClockDrift=true` |
| 8 | `all` | all seven `=true` |

`LadderBidirectional` is deliberately absent. The latency ladder only runs when
`[Settings] Protocol=0`, and the client writes `Protocol=2` by default — so unless
you force protocol 0, that fix is inert and testing it would measure nothing. If you
do want it, add a ninth run with `Protocol=0` for both baseline and fix.

## Reading a result

Each log has a configuration banner, a `[NetTest] RESULT` block every 30 s, and a
final block at match end. Use the **last** block.

```
[NetTest] frames=5412 over 180000ms => 30.06 frames/sec      <- the headline number
[NetTest] blocked 61200ms (34% of run): commands 58100ms (94%) maxahead 3100ms (6%)
[NetTest] framegate: saves=1840 blocks=212 late=0
[NetTest] retransmit: cleanRTT=4ticks redundant_datagrams=9210 send_queue_drops=0
[NetTest] queues: peak DoList=310 (throttle 8192, fatal 16384) peak OutList=6
[NetTest] sendclock: drift=0 peak=48 | stall_counter_writes_suppressed=14903
```

- **frames/sec** is the comparison metric. Everything else explains it.
- **commands %** vs **maxahead %** says which stall the run was dominated by.
  Under induced loss it should be overwhelmingly commands; if it is not, the loss
  is not reaching the game.
- **`late=` must be 0 in every run.** Anything else is a determinism bug, not a
  performance result — stop and send me that log.
- **`peak DoList`** matters only for run 7. If it approaches 4000 the drift
  backstop is engaging; if it approaches 8192 tell me and lower
  `SendClockMaxDrift`.
- **`stall_counter_writes_suppressed`** shows how often the engine was corrupting
  its own frame-rate negotiation. Expect a large number under loss in run 2.

## Method notes

- Same map, same starting position, roughly the same command volume per run.
  Command volume matters more than it looks: `MEGAMISSION` over a large selection
  becomes one DoList entry *per unit*.
- Apply Clumsy to the same player each time, 20% in and out.
- Let each run reach at least 60 s so the 1800-frame warm-up guard on
  `RenderThrottle` has expired, otherwise run 6 measures nothing.
- Collect logs from **both** players. The lossy client and the healthy client see
  very different pictures, and the interesting question is what the *healthy* one
  paid for the other's link.
