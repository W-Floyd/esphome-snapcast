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
  speaker API, keeping `i2s_rate_lock` as the fallback for older ESPHome.
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

- **`fill` and `pipeline` disagree by the source ring's contents.** With a mixer in the
  chain the measured latency exceeds the accounted queue by exactly what the SourceSpeaker's
  own ring holds, which oscillates 20-104 ms as chunks arrive and the mixer drains them.
  Reproducible in `tests/qemu-mixer-test.yaml`; on hardware it shows as `fill` collapsing to
  a 52-54 ms floor in ~20% of samples, the constant term there being the i2s DMA rather than
  a 500 ms virtual ring.

  Decomposed in QEMU: sink ~500 ms and steady, mixer transfer buffer 0-8 ms, source ring
  20-104 ms. `pipeline` tracks sink + transfer; `fill` tracks all three.

  Which side is wrong is NOT yet established. The obvious suspicion -- that `played` is
  credited when frames leave the source ring rather than when they render -- was checked and
  is false: `pending_playback_frames_` is incremented where the mixer mixes and drained by
  the output speaker's own callback, so the accounting should include the ring. Settling it
  needs `pushed`, `played` and the measured terms logged side by side from the client, which
  the mixer harness now makes a local experiment rather than a reflash.

  Until it is settled the repair is gated on steadiness, so the disagreement is reported and
  not acted on.

- **Stale deadline on stream resumption.** With `keepalive_hold: never` the pipeline is
  held across an idle, and the first chunk afterwards carries a deadline stale by roughly
  the idle duration (14044648 ms after 3.90 h; 24888016 ms after ~6.9 h). It self-heals in
  ~2.5 s of chunk-dropping and the magnitude rule mutes correctly, so it is bounded, but
  the mechanism is unexplained and closing it needs the snapserver side. Two clients
  derived slightly different magnitudes from one event, which suggests each computed it
  locally against a common bad input rather than being handed the same number.

- **Extract the servo — waiting on a consumer, not on tidiness.** `player_task_` is 433
  lines, its state is a `ServoState` struct, and the re-baseline, sync report and stale
  bailout are named methods, so what remains is a real extraction rather than a tidy-up.
  Two things argue against doing it speculatively.

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
  MIT permits it to take this code, but it has a working implementation of its own, so
  the trigger is somebody asking rather than the licence allowing.

  It would need somewhere to live. `clock_sync` is deliberately audio-free — a clock
  filter and TSF distribution, neither of which knows what is being played — and the
  servo is the opposite: chunks, sample rates, frame splices, silence insertion. So it
  would be a third component alongside `i2s_rate_lock`, not a member of either.

  The portable half of this is `set_rate_adjustment` above: an API, so no licence or
  extraction question, and the only way rate steering reaches a non-S3 target at all.

- **Residual serial-log gaps.** A ~200 s gap appears in an otherwise clean 10 s cadence
  with host-side buffering ruled out. `esp_wifi_statis_dump()` is the suspect: it emits a
  large multi-line block every tick and its output has never appeared in any capture. It is
  opt-in (`wifi_tools: diagnostics: dump_statistics`, default off), so a long capture with
  it disabled is the experiment.

- **Schema-to-README drift check.** The README's options table and the example's
  commented defaults are hand-maintained against `CONFIG_SCHEMA`, and nothing detects a
  new option, a changed default, or a commented value that is not the default. An AST
  walk over every `cv.Optional` checks all three in ~40 lines: key set, default values,
  and the `unset` marker for options that have none. Generating the DESCRIPTIONS is not
  worth it — seven options carry no source comment, so the README's prose is better text
  than anything derivable.

## Housekeeping

- **Cut a tagged release.** The README tells people to pin `github://…@<tag>` and there
  are no tags, so the only thing to pin is a SHA. A tag would also anchor the MIT cutover,
  which NOTICE.md currently dates but does not tie to a release.

## Features

- **Runtime server retargeting** — `snapcast://host:port` URIs currently warn; needs
  thread-safe reconfiguration of the network task's target.
- **Crossfade on servo frame splices** — the last near-inaudible discontinuity class; a
  2–4 sample crossfade would zero it. Largely moot where `rate_lock` is enabled, since
  steady state stops splicing altogether.
- **Encoded-FLAC buffering** — buffer compressed chunks and decode just-in-time in the
  player task, halving PSRAM needs (Satellite1 validates feasibility). Only worth it if a
  PSRAM-less board variant matters.
