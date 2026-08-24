# TODO

## Blocking a usable public repo

- **No LICENSE.** The repo is publicly visible but not legally reusable, which for a
  component whose whole purpose is consumption via `github://` is the opposite of the
  intent. ESPHome core is GPLv3 and this links against it; MIT or Apache-2.0 are the
  usual choices for external components.
- **`example/snapclient-example.yaml` does not build against stock ESPHome.** It omits
  the forked speaker stack that provides `output_buffered_bytes`, so the documented
  minimal example fails for anyone following the README. Either add the fork to the
  example, or make the fork optional (see the starvation re-baseline item below, which
  could retire it entirely).

## Upstream ESPHome

- **Expose the speaker's queued frame count.** `speaker::Speaker` offers only
  `virtual bool has_buffered_data() const` — a bool, not a count — so the fill between
  our feedback point and the DAC is unobservable. That is the root of the silent-offset
  class: after a starvation the accounting can re-baseline against a wrong fill and
  settle playing 100–250 ms out while every sync metric reads ~0, because the error is
  measured against the same corrupted prediction. A `size_t buffered_frames()` would let
  both re-baseline paths use ground truth instead of inferring it, and would retire the
  six-component fork this repo currently carries.
- **`speaker::set_rate_adjustment(float ppm)`**, default no-op, implemented in the i2s
  speaker per-SoC. This is phase 4 of the shipped rate lock: the component currently
  pokes the S3's MCLK divider directly, and would prefer a speaker API with the
  register-poking helper as the fallback for older ESPHome. Satellite1's forked
  `I2SAudioSpeaker::sync_play()` is independent evidence of demand.
- **`speaker_source` spins forever on an unmixable announcement.** A format mismatch
  cannot be resolved by retrying, but that is what it does. Observed with an announcement
  pipeline at 48000/mono against a 44100/stereo stream: the mixer set
  `Error flag: Incompatible audio streams` once, then the player stayed in `ANNOUNCING`
  allocating a fresh 9600-byte ring ~16×/second — 1742 of them in under two minutes, flag
  never cleared, until reboot. Two defects: unbounded allocation on an unsatisfiable
  retry, and no terminal failure for the announcement.

## Investigate

- **Try dropping the starvation re-baseline entirely.** The accounting is exact from a
  clean start — `pushed − played` is the true queue as long as nothing discards — so the
  offset only appears when something does, and the re-baseline paths exist to recover
  from that. `timeout: never` was supposed to remove the teardown that discards, and
  measurement supports it: zero mixer stops across the fleet while starvations continued.
  If nothing tears down, nothing is discarded, no re-baseline is needed, and both
  `buffered_bytes()` and the fork can go.

  Attempted once and reverted, because the clamp's comments document observed
  100–250 ms offsets — but **those observations predate the `timeout: never` fix**. They
  are evidence from a world where teardowns happened. Cheap to re-test: keep the
  accounting, skip the starvation re-baseline, and watch `drift` in the sync report plus
  `sync-delta.py` across a few starvations. No divergence means the mechanism is
  redundant. Do this BEFORE investing further in the fork.

- **Stale deadline on stream resumption.** With `keepalive_hold: never` the pipeline is
  held across an idle, and the first chunk afterwards arrives with a deadline stale by
  roughly the idle duration (measured: 14044648 ms after 3.90 h, 24888016 ms after
  ~6.9 h — both matching the gap). It self-heals in ~2.5 s of chunk-dropping and the
  magnitude rule mutes correctly, but the mechanism is unexplained and needs the
  snapserver side to close. Two clients derived slightly different magnitudes from the
  same event, which suggests each computed it locally against a common bad input rather
  than being told the same number.

- **Extract the servo from `player_task_` into `audio_timing`.** That function is ~630
  lines and the largest single thing in the component; the timing primitives already
  moved out, so this is the remaining half. Blocked on nothing technical, but it is the
  most safety-critical code here — every constant in it is load-bearing and documented
  against a specific hardware failure — so it wants a quiet period and a soak, not a
  refactor alongside live debugging.

- **Residual serial-log gaps.** After fixing host-side buffering, a ~200 s gap still
  appeared in an otherwise clean 10 s cadence. `esp_wifi_statis_dump()` is the suspect —
  it emits a large multi-line block every tick and its output has never appeared in any
  capture — and it is now opt-in (`wifi_tools: diagnostics: dump_statistics`), so the
  next long capture with it off is the experiment.

## Features

- **Opus codec** — Snapcast uses raw (non-Ogg) Opus framing; needs a bespoke decode path
  (esp-audio-libs' decoder expects Ogg).
- **Runtime server retargeting** — `snapcast://host:port` URIs currently warn; needs
  thread-safe reconfiguration of the network task's target.
- **Crossfade on servo frame splices** — the last near-inaudible discontinuity class; a
  2–4 sample crossfade would zero it. Largely moot where `rate_lock` is enabled, since
  steady state stops splicing altogether.
- **Encoded-FLAC buffering** — buffer compressed chunks and decode just-in-time in the
  player task, halving PSRAM needs (Satellite1 validates feasibility). Only worth it if a
  PSRAM-less board variant matters.
