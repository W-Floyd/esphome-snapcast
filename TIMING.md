# Timing architecture

How a chunk gets from a snapserver timestamp to a speaker cone at the same instant on every
device, and which quantities are measurable.

**This document describes the code as built at `bc80f18` (2026-09-02).** Where a claim is
checkable in the source it is cited; where it rests on a bench measurement it says so, with the
date. This file does not propose anything, and it carries no history — git does that.

The short version:

> One **engine** owns both actuators. It closes on a **measured** render error (`err_tag`), steers
> **rate** continuously and reaches for **position** only when rate provably cannot, and it is
> told the buffer depth so it can refuse to act on a shortage.

Two actuators (rate, position), one decision, three horizons. The previous architecture — a PI
with boost/knee/τ/Ti, and a four-branch per-chunk ladder — was deleted in `dee1ddd` and `50b01d3`;
`render_align` survives as a third controller but is off by default.

---

## 1. The clock chain

| Clock | Source | Shared? |
|---|---|---|
| **Server time** | snapserver's monotonic clock; every chunk carries a timestamp | Yes — identical on every client for the same audio |
| **AP TSF** | the AP's 802.11 timing counter, via `esp_wifi_get_tsf_time()` | Yes — all clients on one BSS read the same counter |
| **Local time** | this device's `esp_timer` | No |
| **DAC clock** | the I2S bit clock, from a PLL through a fractional divider | No |

Render server time *T* at the same physical instant everywhere: express the deadline in a shared
clock, then steer the DAC clock so audio lands on it.

### Why TSF rather than each device's own estimate

Each client Kalman-filters NTP-style exchanges into an offset estimate that wanders ±100–300 µs,
**uncorrelated between devices** — precisely the error that moves a stereo image, since only
*relative* timing is audible.

TSF sidesteps it. Every member multicasts its **own raw** TSF→server mapping once a second
(`BEACON_INTERVAL_US = 1 s`) and adopts the robustly weighted **mean** of everyone's, its own
included, so the group computes deadlines from one shared line and the mapping's own error is
common-mode and cancels. What remains per-device is local TSF read noise.

**Leaderless, by consensus averaging.** No election. Averaging beats inheriting: noise falls as
√N, nothing is handed over so there is no reference discontinuity, and a device rebooting shifts
the mean slightly instead of collapsing the timebase. Measured 2026-08-28, three devices: median
−3.75 µs, sd 4.32, MAD 2.19, zero churn.

**Three invariants:**

1. A member publishes only its **own raw** estimate, never the consensus. Feeding the adopted
   mean back is positive feedback — the whole group can walk while every device agrees.
2. The adopted mapping is **stepped, not slewed**, and stepping is safe *because* the consensus is
   deterministic: every device holding the same estimate set computes the same mean and steps to
   the same place. A slew makes adoption depend on each device's own history, destroying exactly
   that cancellation — measured at 2.7× on sd. The device's **own published line** is still
   slew-limited toward its live Kalman estimate, which is a different thing: it low-passes this
   device's jitter before anyone else averages it.
3. **The anchor is a function of the SET, not of the device** (`6dcdc77`). `ref_tsf` is the
   freshest `tsf_base` among contributors, self included. It used to prefer our own base, which
   made the adopted mapping device-dependent: `robust_mean` is nonlinear, its weights depend on
   the spread between lines, and that spread moves with the reference when drifts differ. Measured
   in `tests/timebase` group 9 with this bench's crystals and beacons 500 ms apart: **43.4 µs of
   spread between the mappings three devices adopt, from the reference alone, with identical
   sets** — and exactly 0 once the anchor comes from the set. A plain mean would not have cared;
   the design's note that the estimator is "gauge-invariant" is true of the mean and false of the
   reweighted mean.

**Published drift is bounded** (`6dcdc77`). `drift_ppm = tsf_rate − kalman_drift`. The tsf term was
always rejected past ±100 ppm ("larger = TSF discontinuity"); the Kalman term had no check at all.
The Kalman's significance gate is inverted in practice — a real +43 ppm crystal drift never clears
`drift² > 4·drift_cov` (0 of 5000 clean samples), while a 30 ms offset step reaches +322 ppm and a
180 ms step +1460 ppm, which do. **So the filter published drift only when the drift was wrong.**
Observed on the wire: −1158, +1462, +5059, +10930, +61641 and −179896 ppm, always from whichever
board had most recently rebooted, adopted with no rejection, dragging peers ~1.6 ms/s until two
boards sat 250 ms apart. Now bounded at both ends — published as **0 rather than clamped**, since
a clamped garbage line is still a wrong line, and rejected peers are *absent* rather than voting a
zero.

The one residual path-dependence: devices differ only while they hold different estimate *sets*,
bounded by `PEER_MAP_STALE_US`.

### The second exchange: render phase

Separate from the timebase, and easy to confuse with it. Each device publishes its **render
phase** — the TSF instant at which it renders server audio time zero — and receives peers'. The
pairwise difference is `render_group_delta_us()`, "am I early or late relative to the group".

* Phases are **sampled per tagged chunk (~94 Hz)** and exchanged in phase-only multicast packets.
  This is *not* the 1 Hz mapping beacon.
* Pairing requires the two phases to have been sampled within `PHASE_PAIR_WINDOW_US = 300 ms` — a
  phase is an absolute TSF-vs-server offset that drifts continuously, so differencing a fresh peer
  phase against a stale local one injects drift × staleness.
* Publishing is **gated on freshness and on transient**: a device that has just stepped its
  position publishes `RENDER_PHASE_UNKNOWN` rather than a phase that does not yet describe where
  its audio will be (`PHASE_TRANSIENT_US = 4 s`).

**The group delta on a pair is half the pairwise disagreement, by construction.** Our own phase is
included and `robust_mean` short-circuits to the plain mean for `n < 3`, so with two publishing
speakers `gd = −(A−B)/2`. Deliberate — each device corrects half the gap and they meet in the
middle — but **`|gd|` must never be compared against a wire differential without the factor.**

The observer publishes no phase (`tsf_observer` gates `send_phase_report` and
`publish_render_phase_`), so the phase group on the bench is `n = 2` always. `consensus_n()` is a
different count — contributors with a valid fresh **mapping**, which the observer does beacon — so
on the bench `consensus_n = 3` while the phase group is 2.

**`gd` is a staircase, not a smooth signal.** Measured 2026-09-02: it changes on essentially every
beacon (1153 changes in 1394 s, median interval 1.10 s), median step 8 µs, p90 61 µs. It is fresh
on ~33 % of decisions with a mean age of 236 ms. Anything steering on it is steering on a 1 Hz
quantised input.

**The reference is honest**, measured 2026-09-02. `GDIN` logs the pairing inputs — `raw`, the
un-halved pairwise difference — and `scripts/bench/gdin-wire.py` regresses it against the
rival-clean wire. Over a clean 30-minute window, both boards, non-steady samples gated out:

    median |raw| / median |wire|            1.03   1.08   (two windows)
    median |gd|  / median |wire|            0.51   0.51
    raw − wire: median / MAD          +2.2 / 6.8   +4.5 / 7.6  us
    slope, per 5-min block            6/6 consistent with 1.0 ± 0.15, both boards

So `raw` tracks the wire 1:1 and `gd` is half of it. Read the `steady=` flag before believing any
of it: ungated, transient samples inflate the residual sd from ~54 µs to ~130 µs.

**One live discrepancy:** the `raw − wire` medians are equal and opposite between boards
(`+4.5` / `−4.4` µs). Paired, signed and stable across windows, so a differential bias rather than
scatter; candidates are the analyser's own zero error and a real pairing asymmetry.

### Reading TSF: the sandwich

`esp_wifi_get_tsf_time()` is bracketed by two `esp_timer` reads, midpoint paired with the TSF
value, bracket width reported per sample. Width is **~42–50 µs, highly consistent** (7 µs spread) —
not jitter but the deterministic cost of the call, so the midpoint bias is common to identical
devices and only the few-µs *variation* matters. One device reads 83 µs median with excursions to
122 µs; best-of-N sampling hid that entirely, so retry count is not a free parameter.

---

## 2. The pipeline

Five buffers between network and pin.

```
snapserver ──network──▶ PCM ring buffer          (client, ~1.7 s)
                          │  player task pushes decoded frames
                        SourceSpeaker ring        (mixer input)
                          │  mixer task
                        output transfer buffer    (50 ms, TRANSFER_BUFFER_DURATION_MS)
                        i2s_audio ring            (buffer_duration, 100 ms)
                        I2S DMA descriptors       (5 × ~8 ms ≈ 40 ms)
                          ▼
                        pin ──▶ DAC ──▶ amp ──▶ driver ──▶ air
```

A chunk is **one codec block**, not a duration of our choosing: FLAC gives 1152 frames, measured at
26.2 ms at 44.1 kHz. It follows the encoder, so it changes with codec and rate.

### Two accounts of the same audio

**The ledger.** `accounted = pushed − played`, two counters with no self-correcting term, so a
frame miscounted once stays miscounted. Against it the client queries what the pipeline actually
holds (`Speaker::buffered_bytes`, which returns `false` when a platform cannot report — distinct
from reporting zero). With all stages reported, `accounted` and observed `fill` agree within ~10 ms.

**The render tags.** Each buffer carries a `RenderTag` (server timestamp + frame offset); when it
completes, `notify_audio_played_tagged` computes
`err_tag = first_frame_local − deadline(that frame's server time)`. Untagged audio — silence,
splices, repeated frames, announcement blends — is skipped by design: there is nothing to measure.

**The tag stamp is a hardware capture, not a model.** `esp_timer_get_time()` is read inside the
I2S TX-done ISR (`IRAM_ATTR`) and queued per completed DMA descriptor. The task pairs each event
1:1 with its write record and subtracts the descriptor's trailing silence, so it is the instant
*that descriptor's real audio finished*, not a pivot EWMA. The mixer forwards the stamp unchanged
to the source that tagged the audio, and only that one.

`publish_render_phase_sample_` derives the phase from that stamp alone: `render_tsf −
render_server`, freshness-gated at `RENDER_TAG_MAX_AGE_US = 100 ms`, else `RENDER_PHASE_UNKNOWN`.
**No modelled quantity enters the render phase.**

These are two independent routes to the same physical quantity, deliberately: a loop closed on a
prediction cannot see an error in the prediction. Evidence they are independent: `inject_split`
moves `err_tag` by 1.02–1.05× the injected truth while the ledger-derived error, servo-nulled,
cannot see it at all.

One constraint inherited from the fork: `RenderTag` distance arithmetic is in frames of one stream,
so **a stage that changes the frame count (a resampler) cannot carry tags**. A resampler in the path
means no tags, which means no measured error — `tags=0` is a configuration answer, not a fault.

### The deadline, and what "absent" means

`chunk_deadline()` ([chunk_deadline.h](components/snapclient/chunk_deadline.h)) is a pure function
and **reports its own validity**:

```cpp
deadline = server_ts + buffer − offset + bias      // only when an offset source exists
```

The offset comes from the shared TSF mapping when one is held, else the local Kalman. When neither
exists it returns **false**, and the caller discards the chunk.

It used to answer `offset = 0` in that case, which does not mean "the clocks agree" — it means the
server timestamp is compared against the local clock with **no domain conversion**, so the deadline
is wrong by the whole difference between the two domains and that error **grows with uptime**.
Measured on the bench observer 2026-09-02: medians of −162 083 994 757 µs (**−45 h**, equal to its
own TSF base) and **64 118 frames corrected in a single report**. The window is every reconnect,
because `connect_socket_()` resets the Kalman. It stayed invisible for months because the shared
mapping is tried first and normally answers — board a reconnected *more* often than the observer
and never saw it. That is the trap: the masking fallback disappears exactly when the timebase is
already in trouble.

Confirmed live after the fix: `NOTB` fired 9 times in 15 minutes, the two longest windows 3019 and
2696 chunks (~1 minute each) that would previously have been scheduled against a zero offset.

---

## 3. The error signals

| # | Signal | Where | Cadence | Consumers |
|---|---|---|---|---|
| 1 | `error_us = predicted − deadline` (**ledger**) | `predict_next_play_us_` | per chunk (~26 ms) | hard resync, stale bailout, resync trace |
| 2 | `err_tag` — per-arrival **measured** render error | `notify_audio_played_tagged` | per tagged DMA completion (~94 Hz) | accumulates into #3 |
| 3 | `e` — time-filtered mean of #2 | `delay_measure_` → `Engine::step` | per observation | **the engine**: the integral, the jump detector |
| 4 | `e_diff` — the group delta, separately filtered | `Engine::step` | per observation, `gd` refreshing at ~1 Hz | **the engine**: P, and the position gate |
| 5 | accounting split — `accounted` vs measured `fill` | per report | 33-sample median | split repair, unmute anchor |
| 6 | `buffer_us` — audio queued ahead of the DAC | `Observation` | per observation | starvation guard, correction affordability |

`e_position = e_diff` whenever the group supplies a differential, else `e_filtered`. **Position
acts on the differential**, because only the differential is audible; the common part is left to
the integral.

`predict_next_play_us_` uses the **nominal** slope, not the realised one. Predicting with
`nominal/(1+applied_ppm)` was arithmetically better and destabilised the loop within two minutes on
hardware: the nominal slope's *insensitivity to trim* is load-bearing, keeping the controller's
output out of its own error signal.

---

## 4. The engine

`Engine::step(now, Observation, GroupEvidence) -> Command`
([timing_engine.cpp](components/snapclient/timing_engine.cpp)). One function, one decision, no
per-chunk ladder. It knows nothing about chunks, codecs or sample rates: every constant is in µs,
ppm, or derived from `Profile`.

### Three horizons, because the two actuators do not share a delay

```
measurement_lag_us   render instant -> reported error.  RATE's dead time (the observation cadence)
position_delay_us    a delivered frame correction -> the DAC.  POSITION's dead time (ring + pipe)
filter_lag_us        = err_tau_horizons * (position_delay + measurement_lag)
rate_horizon_us      = measurement_lag + filter_lag          -- measured 2.0 s on this bench
compensation_us      = position_delay + measurement_lag
settle_us            = compensation + filter_lag
```

**Rate acts at the DAC**: changing the I2S clock shifts the render instant of everything already
buffered, so the ring is not in its loop. **Position acts at the ring input**: a dropped frame must
drain the whole ring before anyone can see it. Sizing rate from position's delay costs a factor of
6 in position error — measured at median |e| 60 µs against 9 µs.

Note the error filter's time constant is keyed to **`compensation_us`**, which the position delay
dominates, so rate's knowledge is smoothed on the timescale of an actuator it does not use. That
coupling is real; a sweep of a split filter found it costs almost nothing (6 → 4 µs of median skew
for an 8× faster rate filter), so it stays shared. `Profile::rate_filter_lag_us` exists to re-test
it, defaulting to 0 = shared.

### The order of decisions

1. **No observation** → hold at the crystal, `Why::NoEvidence`, count it in `suppressed`.
2. **Compensation** for a correction still in flight: subtract the pending displacement from the
   raw observation, and terminate the compensation **on evidence** — whichever of "landed" or "not
   landed" the observation is closer to — rather than on a computed deadline.
3. **Starved** (`buffer_us < buffer_floor_us`) → hold everything. Below one measurement lag of
   audio there is not enough queued for the render instant to be deadline-driven; the growing
   "error" is a data shortage reported in microseconds, and correcting it with position removes
   yet more audio. Measured spiral: ring 1724 ms → 26 ms, then error climbing 3 ms per report while
   the loop dropped 5584 frames a time.
4. **Filter** the error and its own noise (`sigma_e`, from consecutive differences, floored at one
   frame), and the differential separately.
5. **Jump detection**, scaled to noise: `max(max_move, 4σ)`, confirmed over
   `JUMP_CONFIRM_SAMPLES = 3` same-signed innovations. A measurement spike does not survive its own
   sample; a re-anchor or an unannounced resync does.
6. **Position or not.** Detailed below.
7. **The crystal integral**, then **P**, then the **slew limit**.

### Position: only what rate cannot do

```cpp
gate      = frame_us + 2 * gate_sigma            // sized from the noise of the signal position tests
need_ppm  = e_position / rate_horizon_s          // what would remove it within RATE's own horizon
can_fix   = !crystal_spent && |need_ppm| <= rate_authority_ppm
frames    = (|e_position| >= gate && !can_fix) ? e_position / frame_us : 0
```

Three things make this the whole of the position logic:

* **`need_ppm` is judged over rate's horizon, not position's.** Over the ring, a 907 µs step needs
  only 91 ppm, passes as "within reach", and settles 290 µs the other side.
* **`crystal_spent`** asks whether rate can *hold* the fix, not merely reach it. If the integral is
  already railed in the needed direction, P corrects transiently and the error returns. Measured in
  test 14: `need=92` against `auth=100` read as "rate can fix it", **zero frames spent, error
  peaked at 162 ms**.
* **A drop is spent from the buffer**, so a correction is capped at half of it.

`rate_authority_ppm = 150` (`b15fce8`). Sized from the ceiling being *hit*, not from wander: three
excursions in one settled 15-minute window with `need` 114–131 ppm against `auth` 100 and
`avoidable=0`, each followed by an 11-frame step — 249.4 µs, which the analyser recorded as
+242.9/+252.1/−259.6 µs. A position step on one board *is* a step in the pair's difference. The old
100 came from a harness sweep that predates the simulator modelling beacons, a second board or
phase noise. It stays below `CRYSTAL_LIMIT_PPM = 200` deliberately: at the clamp, P could accept an
error only a fully-railed integral could hold.

### Rate: an integral that learns the plant, and a P that answers now

```
crystal += wn^2 * e_bounded * dt_int      wn = 1/(CRYSTAL_DELAY_MARGIN * rate_horizon), clamped ±200
p_term   = clamp(Kp * e_position, ±authority)     Kp = rate_noise_budget / sigma_e
rate     = crystal + p_term,  then slew-limited by authority * dt/horizon
```

* **`Kp = budget / sigma_e`** — the gain is set by the *noise*, not the error, and
  `budget = 1e6 * target_position_us / rate_horizon` is the position-noise budget
  (`sigma_rate <= sigma_position / horizon`). With `target = 20 µs` and `sigma_e` at its one-frame
  floor, `Kp ≈ 0.45 ppm/µs`, i.e. a loop time constant of ~2.2 s.
* **`sigma_e` is floored at one frame**, which caps `Kp` at the gain where P stays *linear* across
  the range position cannot cover. Below that floor P becomes a relay: at a quarter-frame floor it
  saturated past 5.5 µs and the loop bang-banged at ±10–20 µs on a 20–40 s period with zero frame
  corrections.
* **Each error component is clamped separately** — `clamp(e_common) + clamp(e_diff)` — because
  clamping the sum saturates on whichever is larger, and a 40 ppm common drift then discards the
  few microseconds of differential riding on it. Measured in `tests/group`: common drift alone 1 µs
  of skew, a differential alone 16 µs, the two together **90 µs** before the split.
* **`dt` is bounded** by the rate horizon. During a storm observations stop for seconds, and the
  integral was applying `wn² · bound · dt` in one step as though it had been watching: single steps
  of −93.8 ppm in one second, ending railed.
* **The command is slew-limited** (`2e28a71`). Removing an earlier boolean gate left a snap in its
  place; a continuous actuator's command should be continuous.

**What the integral cannot do.** It cannot tell "my oscillator runs fast" from "I am behind because
the audio did not arrive" — both are a persistent one-signed error. A hold extending past the
starvation was tried and **reverted** (`bc80f18`): it does stop wind-up, and it equally stops the
integral learning a real common drift, costing 15–25 ppm wherever one exists. Measured with the
correct target (`plant + common`, since holding the error still requires cancelling both). **The
bench observation that motivated it is unexplained**: board a wound a hand-zeroed crystal to
+192 ppm in twelve minutes while starved, against a true +46, and the simulator does not reproduce
that magnitude.

---

## 5. The third actuator: `render_align`

A separate controller with its own gain, deadband, reject threshold, step cap, group re-centring
term and NVS persistence. It acts on the group delta and moves the **deadline**
(`render_bias_us_`), applied only on the shared-TSF path — without a shared mapping two devices'
phases are not comparable. It also injects an "align kick", delivering the bias delta as a direct
rate command rather than waiting for the loop to walk there, so one controller writes two actuators.

**It is off by default** (`render_align_max: 0ms`) and off on the bench. The nine `align_*` servo
params remain reachable if it is enabled. It is the obvious next deletion: the engine now owns both
actuators it duplicates.

---

## 6. The resync window

After an event the error is a **known displacement**, not wander. `post_event_until_us` opens a
window (`resync_win_s = 60 s`) in which the tag stream is blanked for `resync_blank_ms = 1200 ms`
— longer than the ordinary `blank_ms = 500` — because a step's own visibility horizon is ring
(~1.7 s ahead) + pipeline (~250 ms) + one block, and every shorter blank judged the next step
against blocks that predated the previous one.

Both knobs are still live control. The step-and-verify ladder that used to run inside the window
was deleted with the rest of the ladder; the engine's coarse path subsumes it with one gate derived
from the frame quantum and the filter's own uncertainty.

---

## 7. What is measurable, and the blind spots

`error = predicted − deadline` is computed from `predicted`. **If `predicted` is wrong, the loop
steers real audio to the wrong time and reports zero error.** Measured consequence: devices 10 ms
and 52 ms out of alignment, plainly audible, while every on-device metric read clean.

Closing the rate loop on `err_tag` removes that blind spot *for the engine*. It does not remove it
from the paths still reading the prediction. The render phase is derived from the same tag
stamping, so a fault there would be common to both — which is why the wire regression in §1 matters.

| Instrument | Sees | Blind to |
|---|---|---|
| `ENGINE` line | the decision: `err`, `act`, `why`, `frames`, `rate`, `xtal`, `sigma`, `sup` | anything upstream of the observation |
| `STEPDBG` | `frames`/`ef`/`gate`/`need`/`auth`/`avoidable` — why a step was or was not taken | only emitted when a step is considered |
| `err_tag` / `e` | measured render error of tagged audio | anything untagged; a fault in the tag stamping |
| `render_group_delta` | this device's phase vs the peer mean | a fault shared by the whole group. Tracks the wire 1:1; `gd` is half the pairwise difference |
| **`ARRGAP`** | **chunk ARRIVAL gaps off the socket** — the audio SUPPLY | everything after arrival |
| `tbjit` | the deadline's own per-chunk movement, drift removed | — |
| `pipeline` / `fill` / `drift` | accumulator vs observed pipeline content | anything downstream of the reported stages |
| **logic analyser** (`scripts/i2s-skew.py`) | **the actual wire**, ~26 ns per-capture precision | nothing that matters; this is the referee |

**`ARRGAP` and `tbjit` together separate supply from timing**, and that distinction has already
overturned one diagnosis. 2026-09-02: `buffered` collapsing 26 → 1750 ms with `tbjit` steady at
**2 µs** proved the timebase was fine and the audio supply was not — after hours of the symptom
being read as a sync fault. `ARRGAP` reports a fixed-field histogram (`n`, `mean`, `max`, and
counts in ≤30/≤60/≤120/>120 ms buckets) from the network task, 141 bytes worst case so it cannot be
truncated, and clears its accumulator on reconnect so the disconnect gap is not counted as a stall.

Reading it: chunks carry ~26 ms of audio, so a `mean` near 26 000 is break-even. Board a starved at
mean 61 000–99 570 with `n` down to a fifth and gaps to 2.6 s.

### The analyser is the referee

`scripts/i2s-skew.py` on MLS44 stimulus, `--samples 200000`. On music the analyser cannot resolve
the problem at all (adjacent frames correlate at ~0.997); on MLS the runner-up correlation is ~0.03.
**Check `rival` before trusting any skew number** — a run at 0.94 means whole-frame errors are
masquerading as findings. Per-capture precision is ~26 ns. Grade from `test.csv`, never from plots.

---

## 8. Error budget and current state

| Term | Contribution | Notes |
|---|---|---|
| TSF read noise | ~±3.5 µs per device | variation within a ~42 µs deterministic bracket |
| Published mapping error | ~0 | common-mode by construction — what TSF buys |
| Consensus anchor | **0** | was 43.4 µs of device-dependence before `6dcdc77` |
| Frame quantisation | ~0 with rate lock | 22.7 µs per frame when position acts |
| DAC/amp/driver | fixed | identical hardware, cancels between devices |
| **Speaker placement** | **29 µs per cm** | dominates everything below ~100 µs; trim with `static_delay` |

**Current wire behaviour**, settled, rival-gated, 2026-09-02 21:00: median **−2.8 µs**, sd 18.2,
p05/p95 **−27.8 / +30.7**, zero frame corrections, zero `NOTB`, zero disturbances.

**Post-flash reacquisition is now near-immediate.** Measured in 30 s buckets from an analyser
restart: `nan` count zero from the first bucket, median inside ±10 µs throughout, no convergence
ramp. The ~10-minute figure quoted earlier came from a board unwinding a poisoned timebase and a
broken deadline, both since fixed — a broken-state recovery time, not a settle rule.

**The residual is ~98 % random walk, not a limit cycle.** Autocorrelation over two independent
settled windows (818 s and 885 s) gives the same peaks — 12.00, 23.75, 35.25 s, a 1:2:3 harmonic
series on ~11.8 s — but **r ≈ 0.13, so the cycle carries ~1.7 % of the variance**, and the trace
decorrelates in 2.25 s. Three separate methods agree on the period; it is real and it is small.
Tuning loop gains against it is optimising the wrong term.

What sets that period is the **error filter**, not the crystal integral: `tests/group` 3b sweeps
`filter_lag` 1500 → 187 ms and the period tracks it 23.9 → 8.1 s, roughly `period ~ sqrt(lag)`.
This also explains why `CRYSTAL_DELAY_MARGIN` moved the period the *wrong* way. Three earlier
candidates were proposed and killed by test: the align bias (disabled), the crystal integral
(period moved wrongly with K), and `gd` steps (no correlation against a 200-draw null model).

### The simulator's fidelity gap

`tests/group` runs the **real** `timing_engine.cpp` with the loop closed, and still disagrees with
the bench by roughly **2×**, now seen four independent ways:

| | sim | bench |
|---|---|---|
| ring amplitude | 113.7 µs | 63.4 µs |
| ring period | 21.6 s | 11.5 s |
| period vs filter lag | 23.9 s at 1500 ms | ~11.8 s |
| differential reaching the step threshold | never | 230–260 µs, three times in 15 min |

Consistent enough to be one scale error rather than four coincidences. Closing it is what would
make sweeps *predictive* rather than merely rank-ordered. Until then, use the sim for **ranking**
options and the bench for magnitudes.

---

## 9. Robustness: what happens when the group changes

| Event | Handling | Cost |
|---|---|---|
| **Peer joins** | its raw line enters the mean; every device steps deterministically to the same new mean | `|median error|` 154 µs within 15 s of a membership change vs 93 µs elsewhere (p90 674 vs 286) |
| **Peer leaves** | expires after `MAPPING_EXPIRY_US = 5 s`; the mean moves | same |
| **Peer publishes implausible drift** | rejected at intake, not averaged; it rejoins the moment it publishes something physical | none — this is the fix for the 250 ms divergence of 2026-09-02 |
| **All peers gone** | Kalman fallback; the deadline source switches | a step of the two mappings' disagreement — measured at **29 ms** once |
| **Neither mapping nor Kalman** | `chunk_deadline` returns false; the chunk is discarded, `NOTB` logs the window | audio dropped rather than placed against an unconverted clock domain |
| **Wi-Fi disruption, tags keep flowing** | mapping expires → fallback → the engine holds | trim frozen at the learned crystal offset |
| **Wi-Fi disruption, delivery stops** | ring drains → starvation guard holds → resync window on refill | audible gap; `ARRGAP` is the instrument that names this correctly |
| **Local reboot** | crystal restored from NVS; TSF crystal seed if cold | boards with different NVS histories converge differently |

**Every reflash costs five consensus membership changes.** Fourteen reflashes in one session made
the operator the dominant disturbance on the bench, and wound one board's crystal from a
hand-zeroed 0 to +192 ppm. Batch changes; flash once; then leave it alone. Note this is a reason
not to flash *often* — it is no longer a reason to wait ten minutes after each one.

---

## 10. Failure modes worth knowing

**Absent is not zero.** Five defects in one session shared this shape: a missing offset reported as
0 (§2, −45 h deadlines), a missing bound reported as unbounded (§1, −179 896 ppm published), a
missing anchor taken from whatever this device had (§1, 43 µs), a never-written knob reporting its
range minimum as though it were the setting (four tunables misreported, `timing_target_us` by 20×),
and a sentinel printed as a number. The codebase already had the rule written down for
`RENDER_PHASE_UNKNOWN`; it had not been carried everywhere.

**Correcting a measurement artefact displaces real audio.** The accounted-vs-observed residual was
applied as a timing correction. Once all stages were reported it became measurement offset, and
applying it manufactured relative offsets of 10 ms and 52 ms — the largest audible defect ever
measured here, introduced while trying to fix that defect. Now reported but **not applied**.

**Assuming a fill instead of measuring it.** A starvation re-baseline zeroed the accounting assuming
an empty pipeline. It usually is, but the assumption went unchecked and cost three diagnoses.

**Per-beacon allowances masquerading as rates.** The mapping slew limit was applied per beacon,
coupling tracking speed to the beacon interval. Now a rate, scaled by the measured interval.

**Constants that encode a relationship** drift apart silently when written as literals. The
starvation floor was keyed to `measurement_lag_us`; that constant was later redefined from ~250 ms
to the observation cadence (~47 ms) and the threshold silently fell 5×, so the guard stopped firing
while the ring drained 65 times in one session.

**A field name that is a substring of another field.** `ef=` matches inside `tms@ref=`; `gate=` and
`kp=` each appear in two unrelated lines with different meanings. Three analyses were built on
contaminated greps in one session. Parse whole records, not bare fields.

**The ear outperformed every instrument.** Across five episodes a listener identified which device
was offset, and its persistence, before any metric agreed — because the metrics were measured
against a corrupted reference.

---

## 11. Configuration reference

### YAML

| Setting | Default | Effect on timing |
|---|---|---|
| `sync_deadband` | 128 µs | unmute band (×2) and `render_align`'s own-steady gate. The engine has no deadband on rate |
| `converge_fine` | 2 ms | coarse→fine handoff for the legacy paths |
| `hard_resync_threshold` | 50 ms | beyond this, whole chunks dropped or silence inserted; also the TSF mapping plausibility bound |
| `render_align_max` | **0 ms (off)** | deadline-bias cap; §5 |
| `rate_lock` | opt-in, S3 only | hardware clock steering; splices remain the fallback |
| `tsf_sync` | opt-in | shared timebase; falls back to per-device Kalman |
| `tsf_observer` | false | suppresses phase publication, enables `PHASEIN` |
| `static_delay` | 0 | per-device trim — the right tool for placement asymmetry |
| speaker `buffer_duration` | 100 ms | i2s ring depth; bounds recovery cushion |
| speaker/mixer `timeout` | **`never`** | at the 500 ms default a delivery hiccup tears the pipeline down and rebuilds it at a different fill |

### Runtime (`servo_param` / frontend numbers, no reflash)

**Live control:** `timing_target_us` 20 (prices the rate gain, `Kp = budget/sigma_e`) ·
`rate_authority_ppm` 150 (§4) · `crystal_ppm` (the learned offset; writable so a poisoned estimate
can be zeroed without a serial NVS erase, and it does **not** restore at boot) · `tag_stale_ms`
1000 · `blank_ms` 500 · `gap_blank_ms` 50 · `resync_win_s` 60 · `resync_blank_ms` 1200 · `persist`.

**Experiment knobs** (`532f770`), each added because a constant blocked a measurement:
`err_tau_horizons` 1.0 (the error filter's length; what sets the oscillation period) ·
`rate_filter_lag_us` 0 (split filter, 0 = shared) · `split_ramp_us_per_s` 100 (`inject_split`'s
ramp; at 100 it demands exactly 100 ppm, which the integral absorbs before authority is stressed,
so a probe of authority must raise it).

**`render_align`**, off by default: `align_max_us` · `align_gain` · `align_deadband_us` ·
`align_reject_us` · `align_step_us` · `align_recentre_us` · `align_apply` · `align_bias_us` ·
`align_bias_kick_us`.

**Deleted** (`532f770`), each a knob whose control had already gone: `tau_s`, `ti_s`, `knee_us`,
`tau_min_s`, `block_n`, `boost_floor_us`, `guards`, `timing_engine`, `autotune`, `resync_splice_us`.
A frontend knob wired to nothing invites tuning it.

**A knob reports what the firmware is running** (`c1b3cab`). When NVS holds no value the entity
asks the firmware rather than publishing its range minimum — which it used to do, so
`timing_target_us` read 1 against a compiled 20 and an entire analysis was built on the displayed
number.

Bench hooks: `inject_split`, `inject_starvation`.

### Diagnostic lines

| Line | Carries | Read it for |
|---|---|---|
| `ENGINE` | `err`/`act`/`why`/`frames`/`rate`/`xtal`/`sigma`/`sup` | the decision. `why=1` is `NoEvidence`; `sup` climbing means the engine is holding with nothing to act on |
| `STEPDBG` | `frames`/`ef`/`gate`/`need`/`auth`/`avoidable` | why a step happened, and whether rate could have answered instead |
| `ARRGAP` | `n`/`mean`/`max`/`le30`/`le60`/`le120`/`gt120` | the audio SUPPLY, from the network task. Break-even mean is ~26 000 |
| `GDIN` | `raw`/`gd`/`n`/`gap`/`drift`/`extrap`/`steady` | the group delta's *inputs*. `steady=0` means the sample is not comparable to the wire |
| `CONSIN` | one short line per contributor, plus a summary | who moved the timebase. Fires only on `solo` or `step`, so silence is health |
| `MAPDIV` | how far a peer's adopted mapping is from ours | set divergence, which is not common-mode and lands on the wire |
| `DRIFTREJ` | a rejected implausible drift, ours or a peer's | the drift bound doing its job |
| `NOTB` | a window with no timebase, and the chunk count on recovery | how long the deadline was unavailable |
| `SYNCX` / `Sync:` | error summary, corrections, `tbjit` | `tbjit` is the deadline's own movement — the fastest way to exonerate the timebase |

---

## 12. What this does not solve

Sync is not delivery. Every dropout observed had the same signature: lateness growing ~1 s per
second of wall clock with healthy RSSI, meaning data stopped arriving. This architecture shortens
*recovery*; it cannot prevent the event. `ARRGAP` is the instrument that tells the two apart.

On the reference fleet the cause was airtime, not the clients: twenty stations on one 2.4 GHz
channel, several negotiating 1–6 Mbps, where under DCF a slow station consumes airtime far out of
proportion to the data it carries. Per-station **PHY rates** are the decisive evidence, not RSSI —
a cell can look fine on RSSI and still stall audio. The router's packet counters do **not** update
reliably, so they cannot attribute airtime to a station: a zero delta means "not counted", not
"idle", and a nonzero one may be an accumulation released at an arbitrary moment. Rank with PHY
rates and say plainly that it prices traffic which may not be happening.
