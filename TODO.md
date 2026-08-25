# TODO

## Upstream ESPHome

- **Expose the speaker's queued frame count.** `speaker::Speaker` offers only
  `virtual bool has_buffered_data() const` — a bool, not a count — so the fill between our
  feedback point and the DAC is unobservable. That is the root of the silent-offset class:
  the accounting can re-baseline against a wrong fill and settle playing 100–250 ms out
  while every sync metric reads ~0, because the error is measured against the same
  corrupted prediction. A `size_t buffered_frames()` would let the re-baseline paths use
  ground truth, and would retire the six-component fork this repo carries.
- **`speaker::set_rate_adjustment(float ppm)`**, default no-op, implemented in the i2s
  speaker per-SoC. `rate_lock` pokes the S3's MCLK divider directly and would prefer a
  speaker API, keeping the register-poking helper as the fallback for older ESPHome.
  Satellite1's forked `I2SAudioSpeaker::sync_play()` is independent evidence of demand.
- **`speaker_source` spins forever on an unmixable announcement.** A format mismatch
  cannot be resolved by retrying, but that is what it does: with an announcement pipeline
  at 48000/mono against a 44100/stereo stream, the mixer sets
  `Error flag: Incompatible audio streams` once, then the player stays in `ANNOUNCING`
  allocating a fresh 9600-byte ring ~16×/second — 1742 of them in under two minutes, flag
  never cleared, until reboot. Two defects: unbounded allocation on an unsatisfiable
  retry, and no terminal failure for the announcement.

## Investigate

- **Drop the starvation re-baseline.** It is a net harm as it stands. Anchoring `pushed` to
  the sink's reported fill uses a number that excludes whatever the mixer ring and I2S DMA
  hold at that instant, which mid-drain is substantial; the clamp in
  `notify_audio_played()` then permanently absorbs the shortfall. Measured consequence:
  `drift` steps to +51 ms and stays there while the device plays ~43 ms early, with both
  sync medians reading ~40 µs. The mechanism causes the offset it exists to prevent.

  The accounting is exact from a clean start — `pushed − played` is the true queue as long
  as nothing discards — so the question is whether anything still discards. `timeout:
  never` was meant to remove the teardown that does, and there are zero mixer stops across
  the fleet, yet `Pipeline drained (source starvation)` still fires. Whether that drain
  discards frames is the crux, and it is unmeasured. Test: skip the re-baseline, watch
  `drift` and `sync-delta.py` across several starvations; no divergence means it is
  redundant.

  This does not retire the fork. `on_query_buffered()` has three consumers — the
  re-baseline anchor, the `fill`/`drift` column, and the drift self-repair — and only the
  first would go. The self-repair is the one independent witness to the accounting, and it
  is what makes a split visible at all.

- **Stale deadline on stream resumption.** With `keepalive_hold: never` the pipeline is
  held across an idle, and the first chunk afterwards carries a deadline stale by roughly
  the idle duration (14044648 ms after 3.90 h; 24888016 ms after ~6.9 h). It self-heals in
  ~2.5 s of chunk-dropping and the magnitude rule mutes correctly, so it is bounded, but
  the mechanism is unexplained and closing it needs the snapserver side. Two clients
  derived slightly different magnitudes from one event, which suggests each computed it
  locally against a common bad input rather than being handed the same number.

- **Extract the servo into `clock_sync` — waiting on a consumer, not on tidiness.** The
  length complaint is closed: `player_task_` is 433 lines, its 31 locals are a `ServoState`
  struct, and the re-baseline, sync report and stale bailout are named methods. What is
  left is a real extraction, and two things argue against doing it speculatively.

  It has one consumer. `clock_sync`'s existing members are narrow primitives —
  `KalmanTimeFilter` takes RTT samples and knows nothing of their origin — where the servo
  is their *integration* with this component's I/O, so its interface would be designed from
  a single data point. And the playout feedback (`pushed_frames_total_`,
  `played_frames_total_`, the EWMA pivot terms) is written from the speaker callback thread
  under `playout_mutex_` and read by the player task: today one file's private invariant,
  extracted it becomes a cross-component guarantee, in the code TIMING.md §4 says the
  metrics structurally cannot audit.

  There is a second implementation of the same concept — `sendspin-cpp`'s 936-line
  `sync_task`, same `{frames_played, finish_timestamp}` feedback contract, same hard/soft
  correction split — with no rate steering, no median filter, no PI and no shared timebase.
  The MIT relicense means it *could* now take this code, which the GPL forbade. It has its
  own working implementation, though, so the trigger is somebody asking rather than the
  licence allowing. If it happens it belongs in `clock_sync`, not a new component.

  The portable half of this is `set_rate_adjustment` above: an API, so no licence or
  extraction question, and the only way rate steering reaches a non-S3 target at all.

- **Residual serial-log gaps.** A ~200 s gap appears in an otherwise clean 10 s cadence
  with host-side buffering ruled out. `esp_wifi_statis_dump()` is the suspect: it emits a
  large multi-line block every tick and its output has never appeared in any capture. It is
  opt-in (`wifi_tools: diagnostics: dump_statistics`, default off), so a long capture with
  it disabled is the experiment.

## Features

- **Runtime server retargeting** — `snapcast://host:port` URIs currently warn; needs
  thread-safe reconfiguration of the network task's target.
- **Crossfade on servo frame splices** — the last near-inaudible discontinuity class; a
  2–4 sample crossfade would zero it. Largely moot where `rate_lock` is enabled, since
  steady state stops splicing altogether.
- **Encoded-FLAC buffering** — buffer compressed chunks and decode just-in-time in the
  player task, halving PSRAM needs (Satellite1 validates feasibility). Only worth it if a
  PSRAM-less board variant matters.
