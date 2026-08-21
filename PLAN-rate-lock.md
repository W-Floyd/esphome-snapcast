# Plan: Hardware rate lock (v2 sync) — steering the I2S clock on ESP32-S3

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
- Verified against IDF 5.5.5 headers (`hal/esp32s3/include/hal/i2s_ll.h`):
  - `I2S_LL_GET_HW(port)` — hardware access **by port number**, no channel handle
    needed, so no fork of ESPHome's i2s speaker is required for a PoC.
  - `i2s_ll_tx_set_mclk(hw, hal_utils_clk_div_t{integer, numerator, denominator})` —
    runtime divider programming (the driver itself uses it live).
- Resolution: denominators up to 511 give adjacent rational ratios well under 1 ppm
  apart (Stern–Brocot density) — far finer than the ±200 ppm steering range needed
  and finer than the APLL's own steps.

## Design

### Phase 1 — PoC: `i2s_clock_trim` helper (this repo, S3-only)

A small non-owning helper (not a Component platform yet) inside the snapclient
component, guarded by `USE_SNAPCLIENT_RATE_LOCK` + SOC check:

- Config on the hub: `rate_lock: {i2s_port: 0}` (option absent = feature off,
  splice servo unchanged).
- At first trim: **read back** the divider registers the i2s driver programmed
  (robust to whatever rate/mclk_multiple the speaker chose), derive the base ratio.
- `set_trim_ppm(float ppm)`: compute target ratio `base × (1 − ppm·1e-6)`, find the
  best rational `N + b/a` (continued-fraction / Stern–Brocot search, a ≤ 511), write
  via `i2s_ll_tx_set_mclk()`. Called from the player task; the write is a couple of
  register stores.
- Safety: clamp to ±500 ppm; no-op with a warning if the read-back divider looks
  uninitialized (speaker not started yet).

Risk check in this phase: momentary MCLK phase disturbance on divider writes —
validate audibly with a sine and by scoping BCK if needed. PCM5102A/MAX98357A derive
from BCK with tolerant clocking; expected inaudible.

### Phase 2 — Servo integration

- Player task: when rate lock is active, the servo's output becomes a trim command
  instead of a splice:
  - PI on the median error: `ppm = Kp·median + Ki·∫median` with anti-windup;
    conservative start `Kp ≈ 0.05 ppm/µs`, `Ki` an order below, both re-derived from
    the loop's measured lag before enabling by default (lesson from the warble
    incident: gain × measurement lag caused oscillation — model first, then tune).
  - Alternatively phase-1-simple: bang-bang ±20 ppm with the existing hysteresis
    band (direct port of the reference's `adjust_apll(dir)` law) — start here,
    PI only if residual hunt is measurable.
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
| LL API churn across IDF majors | Version-guard includes; feature is opt-in; splice servo always present as fallback |
| Divider write glitches audio | Phase 1 validates in isolation before servo wiring; writes are rare (bang-bang direction changes) |
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
