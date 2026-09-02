# Timing architecture

How a chunk gets from a snapserver timestamp to a speaker cone at the same instant on every
device, and which quantities are measurable.

**This document describes the code as built at `4f792e0` (2026-09-02).** Where a claim is
checkable in the source it is cited; where it rests on a bench measurement it says so, with the
date. `PLAN-timing-v2.md` proposes how to reduce what is described here; this file does not
propose anything, and it carries no history — git does that.

The short version:

> The loop is closed on a **measured** render error (`err_tag`), not on a prediction. The
> prediction survives as a *scheduling* comparison — hard resync, stale bailout, the splice
> fallback when tags are absent — and the two coexist, which is where most of the remaining
> complexity lives.

Three actuators (rate, position, deadline), six error signals, eleven time-gates.

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
(`BEACON_INTERVAL_US = 1 s`, [tsf_sync.cpp:113](components/clock_sync/tsf_sync.cpp#L113)) and
adopts the robustly weighted **mean** of everyone's, its own included, so the group computes
deadlines from one shared line and the mapping's own error is common-mode and cancels. What
remains per-device is local TSF read noise.

**Leaderless, by consensus averaging.** No election. Averaging beats inheriting: noise falls as
√N, nothing is handed over so there is no reference discontinuity, and a device rebooting shifts
the mean slightly instead of collapsing the timebase. Leadership had been changing six times in
seventeen minutes on a two-device group. Measured 2026-08-28, three devices: median −3.75 µs,
sd 4.32, MAD 2.19, zero churn.

**Two invariants:**

1. A member publishes only its **own raw** estimate, never the consensus. Feeding the adopted
   mean back is positive feedback — the whole group can walk while every device agrees
   ([tsf_sync.h:50](components/clock_sync/tsf_sync.h#L50)).
2. The adopted mapping is **stepped, not slewed**, and stepping is safe *because* the consensus
   is deterministic: every device holding the same estimate set computes the same mean and steps
   to the same place at the same time, so the move is common-mode. A slew makes adoption depend
   on each device's own history, which destroys exactly that cancellation — measured at 2.7× on
   sd ([tsf_sync.h:54-58](components/clock_sync/tsf_sync.h#L54-L58),
   [tsf_sync.cpp:237](components/clock_sync/tsf_sync.cpp#L237)). The device's **own published
   line** is still slew-limited toward its live Kalman estimate, which is a different thing:
   it low-passes this device's jitter before anyone else averages it
   ([tsf_sync.cpp:201](components/clock_sync/tsf_sync.cpp#L201)).

The one residual path-dependence: devices differ only while they hold different estimate *sets*,
bounded by the beacon interval.

### The second exchange: render phase

Separate from the timebase, and easy to confuse with it. Each device publishes its **render
phase** — the TSF instant at which it renders server audio time zero — and receives peers'. The
pairwise difference is `render_group_delta_us()`, "am I early or late relative to the group".

* Phases are **sampled per tagged chunk (~94 Hz)** and exchanged in phase-only multicast packets
  at `phase_tx_hz` ([tsf_sync.h:233](components/clock_sync/tsf_sync.h#L233)). This is *not* the
  1 Hz mapping beacon.
* Pairing requires the two phases to have been sampled within `PHASE_PAIR_WINDOW_US = 300 ms`
  ([tsf_sync.h:375](components/clock_sync/tsf_sync.h#L375)) — a phase is an absolute TSF-vs-server
  offset that drifts continuously, so differencing a fresh peer phase against a stale local one
  injects drift × staleness (~165 µs at 3.3 s and 50 ppm).
* Publishing is **gated on freshness and on transient**: a device that has just stepped its
  position publishes `RENDER_PHASE_UNKNOWN` rather than a phase that does not yet describe where
  its audio will be ([L4396](components/snapclient/snapcast_client.cpp#L4396),
  `PHASE_TRANSIENT_US = 4 s`).

**The group delta on a pair is half the pairwise disagreement, by construction. [C]** Our own
phase is included in the group (`vals[0] = 0.0`), and `robust_mean` short-circuits to the plain
mean for `n < 3` ([tsf_sync.cpp:794-802](components/clock_sync/tsf_sync.cpp#L794-L802)), so with
two publishing speakers `gd = −(A−B)/2`. That is deliberate — each device corrects half the gap
and they meet in the middle instead of one chasing the other — but it means **`|gd|` must never be
compared directly against a wire differential without the factor.** The neighbouring
`update_group_diagnostics_` excludes self for exactly this reason and says so
([tsf_sync.cpp:1078](components/clock_sync/tsf_sync.cpp#L1078)); the control-path delta does not.

The observer publishes no phase at all (`publish_render_phase_` returns early when
`tsf_observer` is set, [L4378](components/snapclient/snapcast_client.cpp#L4378)), so the phase
group on the bench is `n = 2` always.

> **`consensus_n()` is not that `n`, and the boost bound must not use it.** `consensus_n` counts
> contributors with a valid, fresh **mapping**
> ([tsf_sync.cpp:876-888](components/clock_sync/tsf_sync.cpp#L876-L888)), and the observer beacons
> a mapping — `observer-supermini.yaml` inherits `tsf_sync: true` from the base package — so on the
> bench `consensus_n = 3` while the phase group is 2. The boost bound un-halves `gd` with the
> **phase** count from `group_delta_n()`; using `consensus_n` gives 1.5 where 2 is correct, a
> silent 25 % over-tight bound on any group containing a mapping-only member.

**The reference is honest, measured 2026-09-02.** `GDIN` logs the pairing inputs — `raw`, the
un-halved pairwise difference before self-inclusion — and `scripts/bench/gdin-wire.py` regresses it
against the rival-clean wire. Over a clean 30-minute window, both probed boards, non-steady samples
gated out by `GDIN`'s `steady=` flag:

    median |raw| / median |wire|            1.03   1.08   (two windows)
    median |gd|  / median |wire|            0.51   0.51
    raw − wire: median / MAD          +2.2 / 6.8   +4.5 / 7.6  us
    slope, per 5-min block            6/6 consistent with 1.0 ± 0.15, both boards

So `raw` tracks the wire 1:1 in slope and magnitude, and `gd` is half of it — the correct value for
two phase contributors. Read the `steady=` flag before believing any of it: a board in transient
keeps measuring its phase locally on purpose ([L4468](components/snapclient/snapcast_client.cpp#L4468)),
and ungated those samples inflate the residual `sd` from ~54 µs to ~130 µs while leaving the MAD
unchanged.

**One live discrepancy at this scale:** the `raw − wire` medians are equal and opposite between
boards (`+4.5` / `−4.4` µs, and `+2.2` / `−2.2` in an earlier window). Each board reads the other
as ~4.5 µs later. Paired, signed and stable across windows, so it is a differential bias rather
than scatter; candidates are the analyser's own zero error (`scripts/probe-cal.py` measures it) and
a real pairing asymmetry.

### Reading TSF: the sandwich

`esp_wifi_get_tsf_time()` is bracketed by two `esp_timer` reads, midpoint paired with the TSF
value, bracket width reported per sample.

Width is **~42–50 µs, highly consistent** (7 µs spread) — not jitter but the deterministic cost of
the call. A threshold below ~42 µs is unachievable and only burns retries; because the width is
consistent the latch point is consistent, making the midpoint bias common to identical devices.
Only the *variation*, a few µs, matters. One device reads 83 µs median with excursions to 122 µs;
best-of-N sampling hid that entirely, so retry count is not a free parameter.

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

A chunk is **one codec block**, not a duration of our choosing: FLAC gives 1152 frames, measured
at 26.2 ms at 44.1 kHz. It follows the encoder, so it changes with codec and rate — and several
loop constants are expressed per chunk.

### Two accounts of the same audio

**The ledger.** `accounted = pushed − played`, two counters with no self-correcting term, so a
frame miscounted once stays miscounted. Against it the client queries what the pipeline actually
holds (`Speaker::buffered_bytes`, which returns `false` when a platform cannot report — distinct
from reporting zero; `i2s_audio` reports ring plus the full DMA span including silence padding;
`mixer` reports its source queue plus the task-local transfer buffer). With all stages reported,
`accounted` and observed `fill` agree within ~10 ms.

**The render tags.** Each buffer of audio carries a `RenderTag` (server timestamp + frame offset);
when it completes, `notify_audio_played_tagged` computes
`err_tag = first_frame_local − deadline(that frame's server time)`
([L1190-L1225](components/snapclient/snapcast_client.cpp#L1190-L1225)). Untagged audio — silence,
splices, repeated frames, announcement blends — is skipped by design: there is nothing to measure.

**The tag stamp is a hardware capture, not a model. [C]** Verified in the fork
(`speaker-render-latency`, `dd14d50`): `esp_timer_get_time()` is read inside the I2S TX-done ISR
`i2s_on_sent_cb` (`IRAM_ATTR`,
[i2s_audio_speaker.cpp:336](../esphome/esphome/components/i2s_audio/speaker/i2s_audio_speaker.cpp#L336))
and queued per completed DMA descriptor. The task pairs each event 1:1 with its write record and
subtracts the descriptor's trailing silence —
`adjusted_ts = write_timestamp − frames_to_microseconds(silence_frames)`
([i2s_audio_speaker_standard.cpp:285](../esphome/esphome/components/i2s_audio/speaker/i2s_audio_speaker_standard.cpp#L285))
— so it is the instant *that descriptor's real audio finished*, not a pivot EWMA. Untagged
descriptors report nothing rather than a fabricated tag. The mixer forwards the stamp **unchanged**
to the source that tagged the audio, and only that one
([mixer_speaker.cpp:475](../esphome/esphome/components/mixer/speaker/mixer_speaker.cpp#L475)).

`publish_render_phase_sample_` then derives the phase from that stamp alone
([L4397-L4423](components/snapclient/snapcast_client.cpp#L4397-L4423)):
`render_tsf − render_server`, where `render_tsf` walks back `frames` from `adjusted_ts` and
converts through a fresh TSF sandwich, and `render_server` is the tag's own server time. Freshness
gate `RENDER_TAG_MAX_AGE_US = 100 ms`; outside it, `RENDER_PHASE_UNKNOWN`. **No modelled quantity
enters the render phase.**

These are two independent routes to the same physical quantity, and that is deliberate: a loop
closed on a prediction cannot see an error in the prediction. Evidence they are independent:
`inject_split` moves `err_tag` by 1.02–1.05× the injected truth while the ledger-derived error,
servo-nulled, cannot see it at all ([L1216](components/snapclient/snapcast_client.cpp#L1216)).

One constraint inherited from the fork: `RenderTag` distance arithmetic is in frames of one
stream, so **a stage that changes the frame count (a resampler) cannot carry tags**
([audio.h:54](../esphome/esphome/components/audio/audio.h#L54)). A resampler in the path means no
tags, which means no measured error — `tags=0` is a configuration answer, not a fault.

The cost of keeping both is arbitration, and that cost is the dominant structural complexity in
the file — every consumer of "the error" carries its own selector, staleness rule and fallback.

---

## 3. The six error signals

| # | Signal | Where | Cadence | Consumers |
|---|---|---|---|---|
| 1 | `error_us = predicted − deadline` (**ledger**) | [L2808](components/snapclient/snapcast_client.cpp#L2808), [predict_next_play_us_](components/snapclient/snapcast_client.cpp#L6397) | per chunk (~26 ms) | hard resync, window step, stale bailout, resync trace, tag-fault judge |
| 2 | `median_err_us` — 31-sample median of #1 | per chunk | per chunk | bang-bang steer, PI gate, unmute gate, splice fallback |
| 3 | `err_tag` — per-arrival **measured** render error | [notify_audio_played_tagged](components/snapclient/snapcast_client.cpp#L1190) | per tagged DMA completion (~94 Hz) | accumulates into #4 |
| 4 | `dl_err_us` — block mean of #3 over `block_n = 64` | [delay_loop_update_](components/snapclient/snapcast_client.cpp#L4426) | ~1.5 Hz (≈0.65 s) | **the PI**, coarse decisions, fast splice, window step |
| 5 | `render_group_delta_us` — my phase vs peer mean | [tsf_sync.h:163](components/clock_sync/tsf_sync.h#L163) | per block / per report | `render_align`, the PI's boost clamp, window-step sanity, unmute |
| 6 | accounting split — `accounted` vs measured `fill` | [L5642](components/snapclient/snapcast_client.cpp#L5642) | 33-sample median per report | split repair, unmute anchor, splice hold |

`predict_next_play_us_` uses the **nominal** slope, not the realised one. Predicting with
`nominal/(1+applied_ppm)` was arithmetically better and destabilised the loop within two minutes
on hardware (trim +50 → +165 ppm, median oscillating): the nominal slope's *insensitivity to
trim* is load-bearing, keeping the controller's output out of its own error signal
([L6398-L6420](components/snapclient/snapcast_client.cpp#L6398-L6420)).

---

## 4. The three actuators

### (a) Rate — the I2S fractional divider

**One writer**, `rate_lock_->set_trim_ppm()`, programmed every chunk
([L3501](components/snapclient/snapcast_client.cpp#L3501)) — that line *is* the hold. MCLK =
SRC / (N + b/a), the fraction in one 32-bit register so it swaps atomically. The integer part is
never written live (IDF's "double division" workaround bursts MCLK ~6.5× and is unusable on a
running channel). Sigma-delta dither between bracketing ratios at the speaker callback cadence;
residual ~10 ns.

**The ~0.15 ppm figure is the spacing AT THE BASELINE, not a bound.** Measured on the host
(`tests/rate_lock/run.sh`): the representable fractions crowd near high-denominator targets and
thin out near simple ones, so the bracket is 0.15 ppm around 76/441 but reaches **23 ppm** where
the fraction passes 1/6 (inside the servo's own ±500 ppm) and **27.7 ppm** near 1/5 (inside the
±5000 ppm clamp). That costs *ripple*, not accuracy: the dithered mean is the requested rate to
**0.0003 ppm**, and the worst bracket puts 0.28 µs of position ripple per 10 ms tick.

Trims are *relative to a baseline*, so a wrong baseline is a DC offset the servo must cancel out
of its own authority. For 44.1 kHz × 256 from 160 MHz the ideal divider is `6250/441 = 14 + 76/441`
— exactly representable, so where the driver picks a worse approximation we recompute. After the
first trim the register holds *our* value; re-reading it as a baseline would reinterpret the
servo's learned offset as driver error, so the code compares against the last value it wrote.

**This actuator is clean** — single owner, documented invariant, measured residual ~10 ns. It is
the one part of the system not implicated in anything below.

Five sites assign `st.trim_applied_ppm`: PI output ([L4775](components/snapclient/snapcast_client.cpp#L4775)),
the align kick ([L4792](components/snapclient/snapcast_client.cpp#L4792)), in-range hold with
decaying P ([L4475](components/snapclient/snapcast_client.cpp#L4475)), out-of-range integral-only
hold ([L4562](components/snapclient/snapcast_client.cpp#L4562)), and the muted out-of-band branch
([L3531](components/snapclient/snapcast_client.cpp#L3531)).

### (b) Position — frames added or dropped

The per-chunk ladder is a **single `if / else if` chain**, so at most one of these acts on a given
chunk. This is worth stating plainly because it is easy to misread the file as four concurrent
correctors:

| Site | Line | Signal | Arming |
|---|---|---|---|
| Hard resync, late | [L3163](components/snapclient/snapcast_client.cpp#L3163) | #4 if tags live else #1 | `> hard_resync_threshold`; drops whole chunks and `continue`s |
| Hard resync, early | [L3197](components/snapclient/snapcast_client.cpp#L3197) | same | `< −hard_resync_threshold`; inserts silence |
| Coarse window step | [L3230](components/snapclient/snapcast_client.cpp#L3230)–[L3430](components/snapclient/snapcast_client.cpp#L3430) | #4 or #1, sanity-checked against #5 | resync window only, serial step-and-verify, frame-exact landing |
| Bang-bang steer **or** fast splice | [L3536](components/snapclient/snapcast_client.cpp#L3536) / [L3560](components/snapclient/snapcast_client.cpp#L3560) | #2 / (#4 if fresh else #2) | the two halves of one `if/else` on `!trim_holds && !coarse_on_tags` |

**The bang-bang steer is unreachable on S3 with the rate lock healthy.** `trim_holds` is the
return of `set_trim_ppm`, so the branch condition is false whenever the lock is programmed, and
when `coarse_on_tags` is true it is skipped outright. It survives as the no-rate-lock fallback.

**The fast splice is off inside the resync window.** `threshold = post_event ? 0 : cfg_threshold`
and the whole body is gated on `threshold > 0`
([L4934](components/snapclient/snapcast_client.cpp#L4934)) — inside the window the coarse
step-and-verify owns position, so the two never act on the same block error. The function's own
opening comment still describes the older "arms at `resync_splice_us`" behaviour and is stale;
believe the code.

Outside the window the splice needs `|err| ≥ threshold` held for `FAST_SPLICE_PERSIST_US = 4 s`
to arm, releases inside `min(300 µs, threshold/2)`, and is bounded at 128 frames. Default
threshold is **0 — the splice is off unless configured**; the bench yaml sets `1ms`.

**Two in-flight accountings, no shared arbiter.** The splice subtracts `splice_hist` over a
horizon in chunks; the window step subtracts `win_step_us` with frame-exact landing markers
(a step has landed when `played_frames_total_` passes the push index it was applied at, plus a
two-block margin). Neither knows about the other's pending corrections; hard resync records into
neither.

### (c) Deadline — `render_align`

A third controller with its own gain, deadband, reject threshold, step cap, group re-centring
term and NVS persistence ([L6213-L6330](components/snapclient/snapcast_client.cpp#L6213-L6330)).
It acts on signal #5 and moves the deadline (`render_bias_us_`), applied only on the shared-TSF
path — without a shared mapping two devices' phases are not comparable.

It then **also** injects an "align kick": the bias delta is delivered as a direct rate command
(`ALIGN_KICK_MAX_PPM = 1.5`) rather than waiting τ for the PI to walk the audio there
([L4776-L4792](components/snapclient/snapcast_client.cpp#L4776-L4792)). So one controller writes
two actuators. The sign was established **by measurement, not derivation**: a positive bias makes
this board play earlier, so `bias -= delta × gain`; an earlier build flipped it from a polluted
run and made early boards earlier.

> **`render_align_max: 0ms` does disable it**, but only since the seeding was fixed to store
> unconditionally ([L919](components/snapclient/snapcast_client.cpp#L919), `>= 0`). Before that the
> YAML value was copied only when `> 0` and the member default of 500 stood, so a config asking for
> "off" ran render_align at a 500 µs cap. **Any measurement taken before that fix was taken with a
> third controller live and is not a baseline.**

---

## 5. The delay loop (the actual control loop)

`delay_loop_update_` ([L4426](components/snapclient/snapcast_client.cpp#L4426)), at most one PI
step per completed block of `block_n = 64` tag arrivals (≈0.65 s).

```
e        = mean(err_tag over the block)
τ_eff    = τ / boost,   boost = clamp(boost_err / knee, 1, τ/τ_min)
Kp       = 1 / τ_eff
Ki       = Kp / Ti                          (Ti NOT boosted — see below)
integral += Ki · e · dt                     (conditional, anti-windup)
trim      = clamp(Kp·e + integral, ±clamp)
trim     += align_kick                      (≤ 1.5 ppm)
```

| Term | Default | Why |
|---|---|---|
| `τ` | 120 s | floor; the boost stiffens it |
| `Ti` | 600 s | `Ti = τ` (i.e. `Ki = Kp²`) swung the integral ~57 ppm p-p chasing common-mode wander |
| `knee` | 25 µs | A/B 2026-08-30 18:42: 30–50 s tails at flat τ died in ~4 s |
| `τ_min` | 5 s | boost floor |
| `clamp` | `clamp(0.5 · converge_fine, 500, 2000)` ppm | derived: the output must be able to express the P term at the handoff, or the top of the fine band is saturated by construction |

**The boost clamps on the differential, not the magnitude.** Boosting on `|e|` is symmetric and
still moves a pair apart: common timeline wander ramps both boards' `e` together, each board's
slightly different local reading gets multiplied by the gain, and `Kp·(e_A − e_B)` becomes 2–5 ppm
of differential trim. The differential evidence is the group delta (`|gd|` is the same number on
both boards, so the schedule stays symmetric), so the boost runs on `min(|e|, |gd|·n/(n−1))`
([L4646](components/snapclient/snapcast_client.cpp#L4646)).

**But the boost amplifies its own input noise, and that sets the steady-state floor.** Measured
2026-09-02 over 232 co-timed seconds:

    err signal, each board              sd 251 us,  corr(A,B) = +0.949   (95 % common-mode)
    differential part (err_A − err_B)   sd 80.3 us
    kp · sd(err_diff)                   1.12 ppm    predicted differential rate command
    measured sd(trim_A − trim_B)        4.77 ppm    -> effective 4.2x
    true differential wander, wire      ~15 us sd

The boards' estimate of their differential is ~5× noisier than the differential it estimates, and
the loop cannot tell a real residual from noise on the estimate of one, so it pays boosted gain for
both. **Position is the integral of rate and the rate is held constant between updates**, so
4.77 ppm draws straight ramps whose slope changes each update: 13.8 µs predicted against 15.1 µs of
observed position wander over the offset's ~3 s correlation time. That is the sawtooth visible in
the live plot — structural to a rate actuator doing position work, not a plotting artefact and not
a fault.

`boost_floor_us` (default 0 = off) subtracts a floor from the differential evidence before it earns
any boost: above the floor the boost is unchanged so a real step still converges at full speed,
below it the loop reverts to tracking gain. The deadband is on the **evidence**, not the error —
position error is still corrected at tracking gain always. `guards` bit `0x10` ablates the boost
entirely, for the arm that says what it is buying.

**Unknown `gd` is not the same as being alone** (`d9224e4`). The old fallback boosted on `|e|`
whenever `gd == INT32_MIN`, regardless of peer count — so a board with a healthy pair that merely
dropped `gd` for one decision got full boost on a *common* error. Measured 09:58:57: A boosted
×12.5 to Kp 0.104 and moved +57 → +88 ppm in one block while B, whose `gd` was valid, stayed at
Kp 0.008 — ~6 ppm differential, ~120 µs on the wire, 40 s to ramp back. `gd` goes unknown on
~2.7 % of decisions on both boards and it is **bursty**. Now three stages:

1. fresh `gd` → bound the boost with it;
2. `gd` stale but within `BOOST_GD_HOLD_US = 30 s` → use the held value as a *magnitude bound*
   (a delta tens of seconds old still answers "differential or common", which is the only
   question asked of it);
3. older, or never computed, with peers present → **tracking gain**. The correct response to no
   evidence is not maximum gain.

Full boost survives only at `consensus_n ≤ 1` — genuinely alone, nothing to disturb.

**No per-board gain anywhere else.** The rule, from 2026-08-29 22:58: *any gain only one board has
converts common-mode error into differential motion.* A resync-window boost on one board (Kp 0.05
inside its window vs 0.008 outside on the peer) turned the same +30…+130 µs common wander into
2–4 ppm of differential trim and walked the wire at 2–3 µs/s for 30 s.

**Hold, never revert.** Three exit paths hold rather than releasing the actuator:

* tags stale → hold the integral and **decay P toward it over τ**. Holding the integral alone
  dropped P instantly, and with P ≈ 25 ppm of legitimate response to wander that the peer kept
  applying, every ~1 s mapping flap became a differential rate step and ~130 µs of wire skew.
* deadline on the local Kalman fallback while peers exist → hold. The clock-offset estimator sits
  *inside* `err_tag`, so on the shared mapping its wander is common-mode and harmless; on the
  fallback it is per-device and steering on it misaligns.
* `|e| ≥ splice_threshold` (out of range) → hold the **integral only**. Out of range is by
  definition mid-transient, so the last demanded trim carries a P term computed against an error
  the fast path is about to remove. If the integral has drifted > 20 ppm from its own 300 s EMA,
  snap to the EMA.

**`dl_oor` is asserted at a single point that always executes** and cleared only on the one path
reaching a good in-range block — CLAUDE.md's accumulator rule, applied verbatim after setting it
in the out-of-range branch alone latched it through every other early return.

### Boot

Four sources of initial condition, and they do not compose:

* **NVS integral** — the 300 s EMA of the integral is persisted and restored. The integral is the
  learned crystal offset; re-seeding it from the applied trim was measured re-engaging at
  "+0.00 ppm" after a mute cycle, accruing 1 ms in ~18 s (the audible flutter).
* **NVS align bias**, refused if outside the current cap.
* **Cold-start TSF crystal seed** — a fresh board with no NVS integral would wind ~56 ppm through
  Ki over 10+ minutes at Ti 600. The TSF crystal estimate is the same hardware property measured
  against the radio within seconds of boot and sits ~14 ppm from what the DAC needs.
* **Fast boot Ti** (`DL_TI_BOOT_S = 20 s` for the first 180 s, cold start only).

Convergence-time measurements are therefore only comparable between boards with the same NVS
history.

---

## 6. The resync window

After an event the error is a **known displacement**, not wander, and rate control at τ = 120 s is
the wrong instrument for it. `post_event_until_us` opens a window (`resync_win_s = 60 s`) in which
position corrections do the work by **serial step-and-verify**:

* one step at a time — never step while a step is in flight, tested frame-exactly against
  `played_frames_total_` with a two-block margin (build 79/83; the earlier `err − pend` arithmetic
  was unstable at boot, and instantaneous ring+pipe+block under-read while the ring was drained
  post-hole, so every step was re-stepped in full);
* `resync_gain = 1.0` of the measured error per clean block;
* below `resync_local_us = 2000 µs` a step additionally **needs the group delta to agree** —
  common timebase steps reach ±400 µs on both boards simultaneously, and only starvation-class
  errors are local by construction;
* the window closes after `resync_close_s = 5 s` inside `resync_splice_us = 100 µs`, and reopens
  on a block error past `resync_reopen_us = 400 µs`.

Measured (builds 77–81, 2026-08-30): 300 ms injections converge in **10–14 s including the 5 s
hold**, i.e. < 100 µs at +5…+9 s, with zero TAGFAULTs. The remaining variance was the ledger's
first step landing on mid-refill readings; build 78 made it wait for two consecutive readings
within 20 % (500 µs floor), landing every first step at +1.9 s.

---

## 7. The gate lattice

Eleven independently maintained time-latches:

`post_event_until_us` · `tag_fault_until_us` · `phase_transient_until_us` · `dl_blank_until_us_`
(five write sites) · `coarse_act_us` + `blank_us` · `resync_step_at_us` · `last_repair_us` +
`FAST_SPLICE_REPAIR_HOLDOFF_US` (30 s) · `drift_excess_since_us` + `DRIFT_REPAIR_HOLD_US` (3 s) ·
`fast_splice_seen_us` + `FAST_SPLICE_PERSIST_US` (4 s) · feedback-gap blank (`gap_blank_ms`) ·
`unmute_anchor_wait_us`.

Plus the booleans: `converged`, `dl_active`, `dl_oor`, `tags_fresh`, `coarse_on_tags`,
`tag_err_live`, `trim_holds`, `rate_lock_ok`, `steer_dir`, `fast_splice_active`,
`deadline_on_shared_tsf`, `boost_blind`.

**The "visibility horizon" — how long until a correction shows up in the measurement — is encoded
five ways**: `blank_ms` (500 ms), `resync_blank_ms` (1200 ms), the per-chunk computed
`ring + pipeline + 2·block`, `travel_horizon_us_` (`ring + pipe + 2·block_n`, clamped 1–5 s), and
the flat `PHASE_TRANSIENT_US` (4 s). Four of the five are the same physical quantity from the same
inputs with different clamps — §11's "constants that encode a relationship" failure, one level up.

`visibility_horizon_us(clamp)` is the single source the `travel_horizon` sites now derive from
(`clamp > 0 ? max(clamp, travel) : travel`, identical by construction to the inline max each site
applied). The rest is not consolidated, and one inconsistency is worth knowing: `dl_blank_until_us_`
is set travel-aware at [L1297](components/snapclient/snapcast_client.cpp#L1297) and **flat** at
[L1608](components/snapclient/snapcast_client.cpp#L1608) and
[L3213](components/snapclient/snapcast_client.cpp#L3213) — same field, same purpose, two formulas.
Unifying them lengthens two blanks, so it is a control change needing a measurement, not a tidy-up.

---

## 8. What is measurable, and the blind spots

`error = predicted − deadline` is computed from `predicted`. **If `predicted` is wrong, the loop
steers real audio to the wrong time and reports zero error.** Measured consequence: devices 10 ms
and 52 ms out of alignment, plainly audible, while every on-device metric read clean.

Closing the rate loop on `err_tag` removes that blind spot *for the rate loop*. It does not remove
it from the paths still reading the prediction. The render phase is derived from the same tag
stamping, so a fault there would be common to both — which is why §1's wire regression matters:
it is the one check the on-device signals cannot perform on themselves.

| Instrument | Sees | Blind to |
|---|---|---|
| `median` in the sync report | tracking error against *this device's* prediction | any error in the prediction itself |
| `dl_err` / `err_tag` | measured render error of tagged audio | anything untagged; a fault in the tag stamping |
| `render_group_delta` | this device's phase vs the peer mean | a fault shared by the whole group. Tracks the wire 1:1 (§1); `gd` is half the pairwise difference by construction |
| `pipeline` / `fill` / `drift` | accumulator vs observed pipeline content | anything downstream of the reported stages |
| **`raw-sync.py`** | inter-device rendering from raw observations | anything past the I2S pin |
| **logic analyser** (`scripts/i2s-skew.py`) | **the actual wire**, ~26 ns per-capture precision | nothing that matters; this is the referee |

### raw-sync.py

One `RAW` line per chunk containing **only direct observations** — no servo state, no prediction:

```
RAW s_ts=… pushed=… played=… played_ts=… tsf=… tsf_local=… sw=… rate=…

server_time_of_last_rendered_frame = s_ts − (pushed − played) × 1e6 / rate
tsf_time_of_that_frame             = played_ts + (tsf − tsf_local)
```

Robust fitting is essential — every starvation re-baselines the accounting, stepping
`pushed − played`; plain least squares let a handful of steps tilt the line (residuals
440–1220 µs), iterative 2.5σ rejection gives 181–385 µs. The rate offsets are a free validation:
server-vs-TSF rate is physically common to all devices, and after rejection four independent fits
agree at −16.7 to −18.5 ppm.

### The analyser is the referee

`scripts/i2s-skew.py` on MLS44 stimulus, `--samples 200000`. On music the analyser cannot resolve
the problem at all (adjacent frames correlate at ~0.997); on MLS the runner-up correlation is
~0.03. **Check `rival` before trusting any skew number** — a run at 0.94 means whole-frame errors
are masquerading as findings. Per-capture precision is ~26 ns, so every µs it shows is board
behaviour. Grade from `test.csv` with `scripts/bench/dod-grade.py` (the definition-of-done statistics) and
`scripts/bench/structure-function.py`, never from plots.

---

## 9. Error budget and current state

Measured at 44.1 kHz on the two-speaker bench:

| Term | Contribution | Notes |
|---|---|---|
| TSF read noise | ~±3.5 µs per device | variation within a ~42 µs deterministic bracket |
| Published mapping error | ~0 | common-mode by construction — what TSF buys |
| **Differential rate noise** | **3.7 ppm achieved, 4.8 ppm commanded** | measured 2026-09-02, 6.0 h, p2p gate 60 µs. 1 µs accrues in 0.27 ms. `kp · sd(err_diff)` alone predicts 1.1 ppm; the boost multiplies it ~4.2× (§5) |
| Frame quantisation | ~0 with rate lock | 22.7 µs per frame on the splice fallback |
| DAC/amp/driver | fixed | identical hardware, cancels between devices |
| **Speaker placement** | **29 µs per cm** | dominates everything below ~100 µs; trim with `static_delay` |

Current wire behaviour (quiet window, 2026-09-02): detrended position sd ~15 µs, residual MAD
7.4–7.6 µs against the boards' own reference. Post-reboot convergence to < 10 µs takes ~3.5 min.

**The residual is variance, not lag.** Consecutive-difference σ divided by σ came out 1.32–1.43
against √2 ≈ 1.41 for white noise. So raising gain makes it worse and averaging harder makes it
better at no tracking cost. `MEDIAN_WINDOW` is 31 for that reason.

**SF_d, run 2026-09-02 (6.0 h, 147 937 rows, 7473 bins, p2p gate 60 µs — quote the gate, the same
quantity spans 3× across gate choices):**

     tau     SF(d_wire)   SF(trim_diff)   SF(achieved)   ratio d/trim
      5s         6.280         5.317          4.381         1.18
     30s         8.357         7.833          5.382         1.07
     60s         8.594         8.000          5.626         1.07

`SF(d)` moves +3 % between τ = 30 s and 60 s, i.e. it has saturated: the disturbance is **broadband
and enters downstream of the command**, outside the fine loop's τ of 120 s. `SF(achieved)` 5.38
against `SF(d)` 8.36 says the loop already removes ~35 % and no more is available by gain. So a
τ/Ti sweep reads null, and feed-forward cannot help either — it cancels predictable terms and
broadband noise is not one.

**What is left is the source, and §5 identifies it:** the commanded 4.8 ppm is the boost paying
gain for noise on its own input. Read `SF(d)` growing as "slow, the loop can see it" and flat as
"broadband, it cannot" — the structure function grows with τ for a correlated signal and saturates
for white noise.

---

## 10. Robustness: what happens when the group changes

| Event | Handling | Cost |
|---|---|---|
| **Peer joins** | its raw line enters the mean; every device steps deterministically to the same new mean | measured: `|median error|` 154 µs within 15 s of a membership change vs 93 µs elsewhere (p90 674 vs 286) |
| **Peer leaves** | expires after `MAPPING_EXPIRY_US = 5 s`; the mean moves | same |
| **All peers gone** | `consensus_n < 2` → Kalman fallback; the deadline source switches | a step of the two mappings' disagreement — measured at **29 ms** once. Raises `deadline_source_switched_`, which the player loop converts into a kp-event re-arm plus a tag blank |
| **Wi-Fi disruption, tags keep flowing** | mapping expires → fallback → PI **holds** (it will not steer on a per-device estimate while peers exist) | trim frozen at the learned crystal offset; error accrues at the residual rate only |
| **Wi-Fi disruption, delivery stops** | ring drains → starvation re-baseline → hard resync on refill → resync window opens | audible gap; 10–14 s to reconverge |
| **Reconnect / session epoch change** | `mark_kp_event_`, tag blank, `reanchor_armed` | off by default (`reanchor_after_reconnect: false`) |
| **Local reboot** | NVS integral + align bias restored; TSF crystal seed if cold | boards with different NVS histories converge differently |

**Every reflash costs five consensus membership changes.** Thirteen reflashes in one session made
the operator the dominant disturbance on the bench. Batch changes; flash once; then leave it alone.

---

## 11. Failure modes worth knowing

**Correcting a measurement artefact displaces real audio.** The accounted-vs-observed residual was
applied as a timing correction. While stages went unreported that residual was real unmeasured
audio; once all were reported it became measurement offset. Applying it then manufactured relative
offsets of 10 ms and 52 ms — the largest audible defect ever measured here, introduced while
trying to fix that defect. Now measured and reported but **not applied** (`fill_corr`).

**Assuming a fill instead of measuring it.** A starvation re-baseline zeroed the accounting
assuming an empty pipeline. It usually is — DAC feedback gaps of 660–760 ms confirm a genuine
drain — but the assumption went unchecked and cost three diagnoses.

**Per-beacon allowances masquerading as rates.** The mapping slew limit was applied per beacon,
silently coupling tracking speed to the beacon interval and starving tracking when a broadcast was
late. Now a rate, scaled by the measured interval.

**Constants that encode a relationship.** The trim clamp is `Kp × converge_fine`; the muted steer
size is a *slew rate*, not a frame count (8 frames is ~6800 ppm at 44.1 kHz, proportionally less
as rate rises); the splice horizon is a pipeline depth in chunks. Written as literals, these drift
apart silently.

**A unicast phase loop physically displaced the audio.** Build 85 sent ~100–200 `sendto/s` from
the tag-observation thread and moved the bucketed wire from +2 µs to a stable **−1460 µs** at
exactly its boot; it survived a reboot and cleared the moment build 86 disabled the loop.
Mechanism still owed. Anything transmitting from the audio-observation path is suspect.

**The ear outperformed every instrument.** Across five episodes a listener identified which device
was offset, and its persistence, before any metric agreed — because the metrics were measured
against a corrupted reference.

---

## 12. Configuration reference

### YAML

| Setting | Default | Effect on timing |
|---|---|---|
| `sync_deadband` | 128 µs | steer engage threshold; also the unmute band (×2) and `render_align`'s own-steady gate. The rate PI has no deadband |
| `converge_fine` | 2 ms | coarse→fine handoff; the derived trim clamp scales from it |
| `hard_resync_threshold` | 50 ms | beyond this, whole chunks dropped or silence inserted; also the TSF mapping plausibility bound |
| `fast_splice_threshold` | **0 (off)** | standing offset at which position correction engages while converged. Bench: `1ms` |
| `render_align_max` | 0 ms | deadline-bias cap. **See §4c — a YAML 0 does not currently disable it** |
| `reanchor_after_reconnect` | false | forced repair cycle after a relock |
| `rate_lock` | opt-in, S3 only | hardware clock steering; splices remain the fallback |
| `tsf_sync` | opt-in | shared timebase; falls back to per-device Kalman |
| `tsf_observer` | false | enables the `PHASEIN` group-input log |
| `static_delay` | 0 | per-device trim — the right tool for placement asymmetry |
| speaker `buffer_duration` | 100 ms | i2s ring depth; bounds recovery cushion |
| speaker/mixer `timeout` | **`never`** | at the 500 ms default a delivery hiccup tears the pipeline down and rebuilds it at a different fill |

Cross-field validation enforces `2 × sync_deadband < converge_fine < hard_resync_threshold`.
Below the lower bound the coarse splices limit-cycle outside the band the unmute gate requires,
and a client can stay muted indefinitely — this has happened.

### Runtime (`servo_param`, no reflash)

`tau_s` 120 · `ti_s` 600 · `block_n` 64 · `knee_us` 25 · `tau_min_s` 5 · `splice_us` −1 (use
config) · `tag_stale_ms` 1000 · `blank_ms` 500 · `gap_blank_ms` 50 · `align_max_us` 500 ·
`align_gain` 0.3 · `align_deadband_us` 1 · `align_reject_us` 500 · `align_step_us` 20 ·
`align_recentre_us` 2 · `align_apply` true · `resync_win_s` 60 · `resync_gain` 1.0 ·
`resync_reopen_us` 400 · `resync_splice_us` 100 · `resync_close_s` 5 · `resync_local_us` 2000 ·
`resync_blank_ms` 1200 · `phase_tx_hz` · `autotune` false · `persist` true ·
`boost_floor_us` 0 (§5: noise floor on the differential evidence that earns the gd boost) ·
`guards` 0 (ablation mask; bits disable one mechanism each — `0x01` tag blank, `0x02` gap blank,
`0x04` the two-consistent-readings wait, `0x08` the pending-trim splice hold, `0x10` the gd boost.
Every change logs a `GUARDS` line, because a window graded without knowing which mechanisms were
live is ungradeable).

Bench hooks: `inject_split`, `inject_starvation`, `align_bias_us` / `align_bias_kick_us`.

### Diagnostic lines

| Line | Carries | Read it for |
|---|---|---|
| `DECIDE` | one per chunk: `src`/`gate`/`act`/`frames`/`pend`/`gd`/`sk` | the servo ladder's resolution. `sk=` counts chunks the idle throttle suppressed, so the census still accounts for every chunk while the line rate stays ~3/s |
| `GDIN` | `raw`/`gd`/`n`/`gap`/`drift`/`extrap`/`steady` | the group delta's *inputs*. `steady=0` means the board was in transient and the sample is not comparable to the wire |
| `ERRSEL` | `n`/`srcdiff`/`valdiff`/`live`/`new`/`worst` | `active_error()` running as a shadow beside the live selector; zero mismatches over ~96 k chunks per board including a starvation-driven tag→ledger transition |
| `PHASEIN` | the consensus inputs, naming which peer moved | observer only — the group delta's output cannot diagnose itself |
| `Sync:` | error summary, `corrected -D/+I frames`, `hard resyncs`, `err samples` | the aggregate. The last field is `err_count`, which fires the report *at* 128 and is therefore always 128 — it is not a chunk census |

---

## 13. What this does not solve

Sync is not delivery. Every dropout observed had the same signature: lateness growing ~1 s per
second of wall clock with healthy RSSI, meaning data stopped arriving. This architecture shortens
*recovery*; it cannot prevent the event.

On the reference fleet the cause was airtime, not the clients: fourteen stations on one 2.4 GHz
channel at 81 % utilisation, including two BLE proxies transmitting at 1–6 Mbps, where under DCF a
slow station consumes airtime far out of proportion to the data it carries.

From 2026-08-30 ~18:53 the server's holes changed class — bursts of 964 ms to 6.3 s of lateness
every few minutes. Under those, every refill plants a tag/ledger split and the window's tag steps
fight an unattributed per-chunk drop actor in a ~10 s limit cycle. That is server-side
(snapserver buffer 2000 → 4000, or the Pi), not a client timing defect.
