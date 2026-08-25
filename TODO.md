# TODO

## Upstream ESPHome

- **Land the buffered-audio API upstream.** `speaker::Speaker` offers only
  `virtual bool has_buffered_data() const` — a bool, not a quantity — so the fill between
  our feedback point and the DAC is unobservable, which is the root of the silent-offset
  class: the accounting re-baselines against a wrong fill and settles playing 100–250 ms
  out while every sync metric reads ~0, the error being measured against a prediction
  built from the same corrupted accounting. Landing this retires the six-component fork.

  Implemented on the fork as two virtuals, because there are two questions and they differ
  by a real quantity:
  `render_latency()` (when will audio handed over now be heard — counts DMA silence
  padding) and `buffered_audio()` (how much of the caller's own audio is left — does not).
  Durations, not bytes or frames, since neither composes across a mixer's channel widening
  or a resampler's rate change. Both published as atomic snapshots by the owning task,
  because `RingBufferAudioSource` is single-consumer-thread by contract.

  PRs #18753/#18754/#18755 were opened and closed. Review found ten defects across them —
  unit mixing between input and output formats, SPDIF never publishing, a stopped speaker
  reporting "cannot" instead of zero, a documented thread-safety violation, an unreachable
  branch justified by a measurement that belonged to something else — all since fixed, and
  the design changed enough that resubmission should be fresh rather than a force-push.

  Blocked on the `fill` vs `pipeline` question below: the field numbers that make the
  upstream argument are the ones currently in doubt, and resubmitting while the only
  consumer disagrees with the API by 20–100 ms would be arguing from evidence that no
  longer holds. Submit as three stacked PRs targeting `dev` in kahrendt's #16317 style —
  an outside contributor cannot base a PR on a branch in their own fork.
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

- **Re-evaluate the starvation re-baseline.** The case against it was that it anchored
  `pushed` to a number excluding the mixer ring and I2S DMA, so the clamp in
  `notify_audio_played()` permanently absorbed the shortfall — `drift` stepping to +51 ms
  and staying there while the device played ~43 ms early, both medians reading ~40 µs.

  That premise no longer holds: the anchor now uses `on_query_latency()`, which reports the
  whole chain including the DMA span, and does so deliberately — it anchors a PREDICTION of
  when the next pushed frame renders, and padding sits ahead of that frame. So the specific
  harm is gone and the open question is narrower: is the re-baseline still needed at all?

  The accounting is exact from a clean start, so this reduces to whether anything discards.
  `timeout: never` was meant to remove the teardown that does, and there are zero mixer
  stops across the fleet, yet `Pipeline drained (source starvation)` still fires. Test as
  before: skip the re-baseline, watch `drift` and `sync-delta.py` across several
  starvations. Worth doing only after the disagreement below is settled, since `drift` is
  the instrument and it is currently untrustworthy.

  This does not retire the fork either way. The sink query has three consumers — the
  re-baseline anchor, the `fill`/`drift` column, and the self-repair — and only the first
  would go.

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

  Plan:

  1. Log `pushed`, `played`, `own`, `xfer` and `sink` on one line per sync report in the
     mixer harness. Everything so far is inferred from differences between two of them;
     this attributes the disagreement directly. One QEMU run, no reflash.
  2. Cheap discriminator, do it first: rebuild the harness with a single source speaker.
     The sink callback subtracts `new_frames` — every frame the SINK played — from this
     source's `pending_playback_frames_`, so any output not sourced from this speaker
     (the other source, or mixer silence while this ring is dry) over-credits `played`.
     If the disagreement vanishes with one source, that is the mechanism, and the fix is
     to credit a source only for frames it contributed.
  3. If it survives, follow the sign. `pipeline < fill` means `played` too high or
     `pushed` too low, and step 1 says which moves. Rule `playback_delay_frames_` in or
     out: it permanently under-credits `played` at first contribution, which pushes the
     opposite way, so if it is involved something else is too.
  4. Fix the guilty side, not the convenient one. If the accounting is wrong it is a
     `mixer` fix upstream; do not adjust `buffered_audio()` to match, which would encode
     the bug in the API.
  5. Done when `drift` holds near zero through a mixer in QEMU. Then re-enable the repair
     at a sane threshold, confirm it stays quiet, and only then flash hardware.

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
