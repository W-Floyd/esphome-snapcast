# Plan: Hardware rate lock (v2 sync) — steering the I2S clock on ESP32-S3

> **Status (2026-08-21):** Phases 1–2 implemented (`rate_lock.{h,cpp}` + servo
> integration), opt-in via `rate_lock:` on the hub, enabled in the S3 example
> config. The interface is chip-agnostic; only the S3 backend exists (other SoCs
> compile the stub and keep the splice servo). Awaiting phase 3 hardware
> validation — the fractional-only-write glitch question is still open.

## Goal

Replace steady-state frame splicing with continuous hardware sample-rate steering,
achieving the reference firmware's APLL-grade rate lock on chips without an APLL:
zero waveform discontinuities in steady state, sync bounded only by the time-sync
estimate (~±100–300 µs on wifi) instead of frame granularity (~±23 µs splices).

**Non-goals:** improving absolute inter-device accuracy below the wifi time-sync
floor (needs PTP-class hardware timestamping); sub-frame *positioning* (a fractional
resampler — different feature); ESP32-classic APLL support (possible later via the
same interface, different LL backend).

## Background

- Current sync (working, shipped): median + bang-bang servo trims 1 frame/chunk with
  sample stuffing; splices are near-inaudible but nonzero, and correction capacity is
  ~875 ppm.
- The reference esp32 snapclient steers the APLL while playing (`adjust_apll(±1)`),
  holding lock with zero splices. The S3 has no APLL — its I2S MCLK comes from a
  fixed 160 MHz PLL through a **fractional-N divider**: `MCLK = 160 MHz / (N + b/a)`.
- Verified against IDF 5.5.5 sources (`hal/esp32s3/include/hal/i2s_ll.h`,
  `esp_driver_i2s/i2s_std.c`):
  - `I2S_LL_GET_HW(port)` — hardware access **by port number**, no channel handle
    needed, so no fork of ESPHome's i2s speaker is required for a PoC.
  - `i2s_ll_tx_set_mclk()` exists but is **not usable as-is on a running channel**:
    it delegates to `i2s_ll_tx_set_raw_clk_div()`, whose "double division issue"
    workaround first programs `div_num = 2` (MCLK ≈ 80 MHz, ~6.5× overspeed) before
    the target — a full-speed clock burst on every call, not a phase blip. The
    PCM5102A's clock-error detector may mute/relock on it.
  - **No precedent for live writes**: `i2s_channel_reconfig_std_clock()` requires
    the channel disabled (`i2s_std.c:381`) — IDF never reprograms this divider
    while running. Glitch-free live steering must be proven, not assumed.
  - Likely mitigation: write only the fractional fields
    (`tx_clkm_div_conf.{x,y,z,yn1}`) directly — a ±ppm trim never changes the
    integer part, and the workaround exists for `div_num` changes. Whether
    fractional-only writes dodge the double-division erratum is the central
    phase-1 question.
  - No LL getter for readback: read `tx_clkm_conf.tx_clkm_div_num` +
    `tx_clkm_div_conf` and invert the encoding — `a = (x+1)·z + y`,
    `b = yn1 ? a−z : z`. All-zeros fractional fields = pure integer divider,
    doubling as the "uninitialized" sentinel.
- Resolution: the x/y/z fields are 9-bit, so denominators up to 511 are valid;
  Farey spacing near the 48 kHz ratio (N ≈ 13.02) gives worst-case steps of
  ~0.15 ppm — far finer than the ±200 ppm steering range needed and finer than
  the APLL's own steps.
- BCK/WS divide down from MCLK, so a fractional MCLK trim steers the actual
  sample rate with no change to the BCK divider.

## Design

### Phase 1 — PoC: `i2s_clock_trim` helper (this repo, S3-only)

A small non-owning helper (not a Component platform yet) inside the snapclient
component, guarded by `USE_SNAPCLIENT_RATE_LOCK` + SOC check:

- Config on the hub: `rate_lock: {i2s_port: 0}` (option absent = feature off,
  splice servo unchanged).
- At first trim: **read back** the divider registers the i2s driver programmed
  (robust to whatever rate/mclk_multiple the speaker chose), invert the x/y/z/yn1
  encoding to `N + b/a`, derive the base ratio.
- `set_trim_ppm(float ppm)`: compute target ratio `base × (1 − ppm·1e-6)`, find the
  best rational `N + b/a` (continued-fraction / Stern–Brocot search, a ≤ 511), write
  the **fractional fields only** (`tx_clkm_div_conf.{x,y,z,yn1}`), never through
  `i2s_ll_tx_set_mclk()` (its double-division workaround bursts MCLK to ~80 MHz on
  every call). Reject any trim whose best rational changes the integer part (can't
  happen within ±500 ppm at sane base ratios; assert anyway). Called from the
  player task.
- Safety: clamp to ±500 ppm; no-op with a warning if the read-back divider looks
  uninitialized — all-zeros fractional fields (pure integer divider) or a zero
  `div_num` (speaker not started yet).

**Central phase-1 question:** are fractional-only field writes glitch-free on a
running channel? IDF never reprograms this divider live (`reconfig_std_clock`
requires the channel disabled), so there is no precedent either way — the
double-division erratum workaround targets `div_num` changes, which we never make.
Validate audibly with a sine (PCM5102A's clock-error detector would mute/relock on
a real glitch — easy to hear) and by scoping BCK if needed. If fractional-only
writes still glitch, fall back to rate-limiting writes and/or gating them into
inter-chunk silence; if that fails too, the feature dies here cheaply.

Note: fractional-divider jitter is not a new concern — the driver already uses a
fractional ratio (12.288 MHz from 160 MHz); trimming adds no new jitter class.

### Phase 2 — Servo integration

- Player task: when rate lock is active, the servo's output becomes a trim command
  instead of a splice:
  - **Hardware result (2026-08-21):** the bang-bang/stepping law limit-cycled at
    ±250 ppm / ±3 ms, structurally: queue depth integrates a rate mismatch, so the
    plant is already an integrator and a stepping trim double-integrates. Observed
    amplitude matched the double-integrator prediction `√(2·e₀·slew)` and ~40 s
    period. Not a gain problem — no step size damps it.
  - **Adopted: continuous PI on the median error**, no deadband gating (trims are
    inaudible; hysteresis gating re-creates the limit cycle):
    `trim = Kp·median + Ki·∫median`, error in µs, trim in ppm (plant slope:
    1 ppm = 1 µs/s). `Ki = Kp²/4` (critically damped) absorbs the crystal offset.
    Integrator clamped to ±500 ppm (anti-windup) and persists across
    resyncs/flushes/rate changes — it's a property of the crystal, not the stream.
  - **Gain is set by disturbance tracking, not settling** (second hardware
    lesson): the clock-offset estimate wanders ~100 µs/s with wifi jitter, and an
    integrator plant trails a ramp by `rate/Kp`. `Kp = 0.1` measured ±1–2 ms
    excursions — pure tracking lag, not instability. `Kp = 0.5 ppm/µs` bounds it
    to ~200 µs with ~50° phase margin against the ~0.85 s measurement lag. The
    splice servo never showed this because 1 frame/chunk ≈ 875 ppm of authority
    chased the wander invisibly.
- Splices remain for: hard resync (big jumps), catch-up beyond ±clamp, and as the
  automatic fallback when rate lock is unavailable (non-S3, option off, port
  read-back failed).
- Mute-until-synced, flush re-baseline, and all existing recovery logic unchanged.
- Diagnostics: add `trim=+X.X ppm` to the periodic sync report.

### Phase 3 — Validation

- QEMU cannot validate this (no I2S device); the splice path stays default-off…
  i.e. rate lock is **opt-in** until hardware-proven.
- Hardware: A/B on the stereo pair — sync report should show `corrected -0/+0`
  steady-state with a stable small trim value (crystal offset, ±<100 ppm); listening
  test on sine tones (splice clicks audible there if any remain).
- Abuse tests: stream start/stop, announcement ducking, wifi storm recovery, sample
  rate change (44.1↔48 stream), speaker stop/start (divider re-read after pipeline
  restart — registers may be reprogrammed by the driver; re-baseline on flush gap).
- Soak: overnight, watching trim drift (should wander slowly with temperature) and
  correction counters (should stay ~0).

### Phase 4 — Upstream

- Propose `speaker::Speaker::set_rate_adjustment(float ppm)` (default no-op) +
  implementation in the i2s speaker using the same LL calls, per-SoC guarded.
  Sendspin benefits identically — good PR narrative.
- When merged, the snapclient hub prefers the speaker API over the port-poking
  helper; the helper stays as fallback for older ESPHome.

## Risks

| Risk | Mitigation |
|---|---|
| LL API churn across IDF majors | Version-guard includes; feature is opt-in; splice servo always present as fallback. We write register fields directly (not the LL setter), so the coupling is to the S3 register layout — stable — not the LL API |
| Divider write glitches audio (**the** open question — no IDF precedent for live writes) | Fractional-fields-only writes, never `i2s_ll_tx_set_mclk()` and its ~80 MHz burst workaround; phase 1 validates in isolation before servo wiring; fallbacks: rate-limit writes, gate into silence, or abandon cheaply |
| Driver reprograms divider (rate change, channel restart) | Re-read base ratio on stream format change and after flush-gap re-baseline |
| Control-loop oscillation (again) | Start with the reference's proven bang-bang law; PI only after measuring loop lag; trim clamp ±500 ppm bounds worst case |
| Announcement pipeline shares the DAC clock | ±100 ppm = 0.01% pitch shift on announcements — imperceptible; documented |
| Non-S3 chips | SOC-guard; ESP32-classic can later get an APLL backend behind the same interface |

## Estimates

- Phase 1: ~half a day (math + register plumbing + audible check)
- Phase 2: ~half a day (servo wiring + diagnostics)
- Phase 3: hardware time, mostly elapsed soak
- Phase 4: PR-sized, independent timeline

## Decision point

Ship phases 1–2 opt-in (`rate_lock:` absent by default). Promote to default only
after the pair passes phase 3 soak with zero regressions against the splice servo.
