# PLAN — timing architecture v2

Written 2026-08-31 against `d9224e4`. `TIMING.md` describes what the code does now; this proposes
what it should become and the order to get there.

Marking, per `CLAUDE.md`: **[C]** is checkable in the source and cited. **[M]** needs the bench and
is written as a test with a pass condition, never as a conclusion. Nothing here is a bench finding.

---

## 0. The thesis

The system has three actuators, six error signals and eleven time-gates because it grew one fix at
a time. But the *physics* has only three kinds of error, and they do not overlap:

| Physical error | Correct actuator | Why nothing else |
|---|---|---|
| **Frequency** — the two DACs run at different rates (~0.33 ppm differential) | rate trim | a position correction against a rate error is a limit cycle by construction |
| **Displacement** — a step: a join, a refill, a hard resync, a timebase adoption | position (frames) | a rate actuator at τ = 120 s takes minutes to move a millisecond |
| **Reference offset** — my idea of the group's timeline is biased | the *deadline* | correcting it with either actuator makes the bias indistinguishable from a real error |

**The v2 architecture is that table, made literal.** One owner per row, one signal feeding each,
and an explicit arbiter deciding which row this chunk belongs to. Everything currently in the file
is either one of those three, or arbitration between two accounts of the same audio.

The reduction is not "delete layers". It is: **make the classification explicit, and let each
actuator own exactly one physical error.** Today the classification is implicit in a chain of
eleven latches, which is why it is hard to reason about and why a gain that only one board takes
can leak common-mode error into differential motion.

### The one thing that must happen first

**The reference does not yet agree with the wire.** The exchanged render phase under-measures a
real differential by ~8× (measured 2026-08-30, builds 84–86: rival-clean wire −1.5 ms against
≤0.2 ms in pairwise phases). Every inter-device control decision — the boost clamp,
`render_align`, the window step's sanity check, the unmute group-agreement gate — reads that
number. **No control change can be graded while the referee inside the device disagrees with the
wire**, and a plan that reorganises the controller first is reorganising against a broken
measurement.

Stage 1 is therefore not a refactor. It is, however, now much cheaper than it looked: reading the
fork **excludes the standing suspect** (the stamp is a genuine TX-done ISR capture) and a factor
of 2 turns out to be a deliberate design property being compared against the wrong reference. See
§4.

---

## 1. Target architecture

```
                 ┌─────────────────────────────────────────────┐
   tags ───────▶ │  ErrorView                                  │
   ledger ─────▶ │    { us, source, age_us, trusted }          │  one selector, one place
   group delta ▶ │    + class: RATE | STEP | BIAS | NONE       │
                 └──────────────────┬──────────────────────────┘
                                    │
                 ┌──────────────────┴──────────────────────────┐
                 │  Arbiter — one function, one switch         │
                 │  picks the actuator, records the decision   │
                 └───┬───────────────┬──────────────────┬──────┘
                     │               │                  │
              ┌──────▼─────┐  ┌──────▼──────┐   ┌───────▼───────┐
              │ RATE       │  │ POSITION    │   │ DEADLINE      │
              │ PI on      │  │ one in-flight│  │ slow bias,    │
              │ err_tag    │  │ ledger,      │  │ group-        │
              │ symmetric  │  │ serial       │  │ recentred     │
              │ gain only  │  │ step-&-verify│  │ NO rate kick  │
              └────────────┘  └─────────────┘   └───────────────┘
```

Five rules, and every one of them is a rule the current code breaks somewhere:

1. **One writer per actuator.** Rate already has one. Position gets one (today: four sites).
   Deadline gets one and loses its second delivery path (today: the align kick makes it two).
2. **One in-flight ledger**, frame-exact, shared by every position policy. Today the splice and
   the window step each keep their own and hard resync keeps none, so a correction can be applied
   twice because neither knew about the other's.
3. **Every gain is the same function of the error on every board.** Not "symmetric in form" —
   *identical in value for identical inputs*. A function of `|e|` is symmetric in form and still
   produces differential motion, because each board reads a slightly different `e` from the same
   common wander. Only functions of a genuinely shared quantity (`|gd|`, which is the same number
   on both boards) qualify.
4. **No evidence → tracking gain, never maximum gain.** Already the rule for the boost after
   `d9224e4`; generalise it. Absent, stale and sentinel are three states and none of them is zero.
5. **A timebase move is an input, not a disturbance.** When the consensus steps or the deadline
   source switches, the size of the move is *known*. Feed it forward into the position ledger
   instead of letting the loop discover it as error over the next several blocks.

Rule 5 is the one that buys most of the robustness the goal asks for, and it is new.

---

## 2. Where the goals are actually won

| Goal | The term that decides it | Stage |
|---|---|---|
| Easier to reason through | one selector, one arbiter, one horizon function, one decision log line | 2, 3, 4 |
| Robust to joining/leaving speakers | rule 5: feed the known timebase step forward; never let a source switch arrive as 29 ms of "error" | 5 |
| Robust to Wi-Fi disruption | hold-don't-revert (already right) + rule 4 generalised + a bounded, announced fallback | 5, 6 |
| Converges quickly | feed-forward, not gain: crystal seed, NVS integral, known-displacement steps | 6 |
| Tightest sync delta | a reference that agrees with the wire (Stage 1), then differential rate feed-forward gated on SF_d, then no per-board gain left standing | 1, 8b, 8c |
| Nothing silently asymmetric | rule 3, audited with the **peer's** decision logged beside our own | 8c |

Note what is *not* on this list: raising a gain. The residual is variance, not lag (measured:
consecutive-difference σ/σ = 1.32–1.43 against √2 for white noise), so more gain makes it worse.
Every convergence win below is feed-forward or a better measurement.

---

## 3. Stage 0 — two live defects (done, `638c714`)

Neither was a refactor; both were live defects found while reading. Recorded because one of them
invalidates earlier measurements.

**0a. `render_align_max: 0` does not disable render_align. [C]**
[L919](components/snapclient/snapcast_client.cpp#L919) only copies the YAML value into
`tune_align_max_us_` when it is `> 0`, and the member default is 500
([snapcast_client.h:1647](components/snapclient/snapcast_client.h#L1647)). The bench yaml sets
`0ms`, `HANDOFF.md` records render_align as disabled, and the comment at
[L6254](components/snapclient/snapcast_client.cpp#L6254) says "0 = off". It has been running at a
500 µs cap with `align_apply` true. Fix the seeding to store unconditionally.

Consequence: **every measurement taken while the operator believed align was off was taken with a
third controller live**, including the quiet-window numbers in `HANDOFF.md`. Those numbers are not
a baseline; re-baseline before grading anything against them.

**0b. `fast_splice_`'s docblock contradicts its code. [C]** The opening comment says the splice
"arms at `resync_splice_us` and immediately" inside the resync window; the code sets
`threshold = post_event ? 0 : cfg_threshold` and gates the whole body on `threshold > 0`
([L4934](components/snapclient/snapcast_client.cpp#L4934)), so the splice is *off* in the window.
The second comment three lines down says so correctly. Delete the stale sentence — a review
already read the wrong one and reported it as behaviour.

---

## 4. Stage 1 — the reference (closed)

**The reference is honest, and this stage no longer gates anything.** Measured over a clean
30-minute window on both probed boards (MLS44, `rival`+`pcm_coef` gated wire,
`scripts/bench/gdin-wire.py`, 2026-09-02):

    (n = 1398 A / 1370 B paired samples)              board A     board B
    median |wire|                                      80.5        80.2  us
    median |raw|                                       83.0        82.0  us
    median |raw| / median |wire|                        1.03        1.02
    median |gd|  / median |wire|                        0.51        0.51
    raw - wire: median / MAD                       +2.2 / 6.8  -2.2 / 6.9  us
    raw - wire: sd                                     58.4       129.9  us

The raw pairwise difference tracks the wire 1:1 in slope and magnitude, disagreeing by ~7 µs MAD
on an 80 µs signal. There is no gain error to explain.

### Four properties of the reference, each of which looks like a defect and is not

State these before touching anything that consumes the delta; each has already been proposed as a
bug at least once.

**`gd` is half the pairwise difference, by design.** `render_group_delta_us` includes our own
phase (`vals[0] = 0.0`) and `robust_mean` short-circuits to the plain mean for `n < 3`
([tsf_sync.cpp:794-802](components/clock_sync/tsf_sync.cpp#L794-L802)). With two publishing
speakers the delta is exactly half the A–B disagreement — correct for control, since each device
corrects half the gap and they meet in the middle. `update_group_diagnostics_` excludes self for
precisely this reason ([tsf_sync.cpp:1078](components/clock_sync/tsf_sync.cpp#L1078)). Measured
`|gd|/|wire|` = 0.51 on both boards, which is the expected value, not a shortfall.

**The observer publishes no phase, so `n` = 2 on this bench.** `publish_render_phase_` returns
early on `tsf_observer` ([L4378](components/snapclient/snapcast_client.cpp#L4378)), and
`recompute_group_delta_` returns early on `RENDER_PHASE_UNKNOWN` — so the observer contributes a
mapping but no phase, and emits no `GDIN` at all. That is its normal state, not a missing signal;
it publishes `PHASEIN` instead. Any consumer un-halving the delta must therefore use
`group_delta_n()` (phase contributors) and never `consensus_n()` (mapping contributors): on two
speakers plus an observer the latter gives `n/(n−1)` = 1.5 where 2 is correct.

**The stamp is a hardware capture, with nothing modelled in the path.**
`esp_timer_get_time()` is taken inside the I2S TX-done ISR `i2s_on_sent_cb` (`IRAM_ATTR`,
[i2s_audio_speaker.cpp:336](../esphome/esphome/components/i2s_audio/speaker/i2s_audio_speaker.cpp#L336)),
queued per completed DMA descriptor, paired 1:1 with its write record, and reduced by the
descriptor's trailing silence
([i2s_audio_speaker_standard.cpp:285](../esphome/esphome/components/i2s_audio/speaker/i2s_audio_speaker_standard.cpp#L285));
the mixer forwards it unchanged
([mixer_speaker.cpp:475](../esphome/esphome/components/mixer/speaker/mixer_speaker.cpp#L475)), and
`publish_render_phase_sample_`
([L4397-L4423](components/snapclient/snapcast_client.cpp#L4397-L4423)) only walks back `frames`,
converts through a fresh TSF sandwich, and subtracts the tag's own server time. No pivot, no EWMA,
no model. **"Make the render phase honest with a TX-done timestamp" is a proposal to build what
exists** — do not re-derive it.

**`GDIN` names the last *scanned* peer that paired**, i.e. `peer_[]` order, which is arrival
order rather than recency or proximity. With one phase-contributing peer that is the only peer;
with two it is an arbitrary choice among them.

### Pass condition met, both boards, steady-gated

`GDIN` now carries the emitting board's `steady=` flag and the grader drops non-steady samples.
On a clean 30-minute window (01:45–02:15, no gaps, no reboots):

    board                                       A            B
    blocks consistent with slope 1.0          6/6          6/6
    median |raw| / median |wire|             1.08         1.11
    residual raw-wire: median / MAD     +4.5 / 7.6   -4.4 / 7.4  us
    residual raw-wire: sd                    54.1         52.2   us
    samples dropped as non-steady            9.9%            -

**The tail asymmetry was transient samples, as predicted.** Ungated, board B's residual `sd` was
129.9 against A's 58.4; gated, it is 52.2 against 54.1 — the two boards are now indistinguishable,
and B's block that excluded slope 1.0 no longer does. The prediction was recorded before the
measurement and the number came back where it was expected, which is the only reason to trust the
explanation rather than merely fit it.

**One thing the gate revealed rather than removed:** the residual medians are equal and opposite,
`+4.5` µs on A against `−4.4` µs on B (and `+2.2`/`−2.2` in the earlier window). Each board reads
the other as ~4.5 µs later. That is a differential bias, not scatter — paired, signed and stable
across two windows — and it is small enough to have been invisible under the transient tail. It is
now the smallest live discrepancy on the bench, and the candidates are probe skew (the analyser's
own zero error, which `scripts/probe-cal.py` measures) and a real asymmetry in the pairing.

### Previously open, now explained

**The residual's tail is transient samples, by design.** Board B's `raw − wire` has `sd` 129.9
against board A's 58.4 on an identical MAD of ~6.9 — same typical agreement, twice the excursions
— and B's only block that excludes slope 1.0 is the one carrying 5 hard resyncs and a `raw` p2p of
4704 µs against ~500 µs elsewhere.

A board in transient stops *beaconing* its phase but keeps measuring and using it locally, on
purpose: `publish_render_phase_(!in_transient)` only sets the broadcast flag, and the comment at
[L4468](components/snapclient/snapcast_client.cpp#L4468) is explicit that the resync gate needs its
own delta while its window is open. So a board that is stepping its audio still computes a delta
from a phase that is moving — right for control, and not comparable to the wire.

**[M] Two things follow, in order:**

1. **`GDIN` must carry the emitting board's steady/transient state** (one flag; `in_transient` is
   already computed at [L4720](components/snapclient/snapcast_client.cpp#L4720)), and
   `gdin-wire.py` must gate on it. Grading the reference on samples the firmware itself does not
   treat as steady measures the transient, not the reference. Until that flag exists, read a
   window's verdict together with its hard-resync count.
2. **The two boards fail differently, and that is the next question.** Not *rate*: over the full
   logs they hard-resync equally (33 on A, 32 on B) — the 14-vs-5 that started this was one
   window, not a board property. What differs is the *shape*, on identical firmware and stream:

       resync spacing, median              A  9.4 s      B  2.4 s   (B bursts)
       resyncs preceded by a stall <=10 s  A  0/33       B  7/32
       OUT OF RANGE                        A   225       B   115
       DLLOOP err, median                  A  +219 us    B  +101 us

   So B's resyncs cluster and follow starvation, while A's follow nothing yet A carries twice the
   out-of-range count and double the standing loop error. Two different failure modes, both
   already instrumented — `DECIDE`'s `act=resync`, `PLAYER STALLED`, and the `OUT OF RANGE`
   census are the inputs, and no new logging is needed to pursue either.

**One statistical constraint on all of it:** use median/MAD, not `sd`. On this residual `sd`
overstates the disagreement by 8×, and the first figure computed from it was wrong by that factor.

---

## 5. Stage 2 — one decision line (prerequisite for every later stage)

Attribution is currently spread across `RSTEP`, `RSKIP`, `DLLOOP`, `FRAMEINV`, `TRIMDBG`,
`RALIGN`, `BOOSTBLIND`, `BOOSTHOLD` and the split `Sync:`/`SYNCX` report, and the ladder's
*refusals* are almost invisible — `gate_seen` exists for exactly one gate, because that lesson was
already paid for once.

Add `DECIDE`: **fixed field count, no variable-length tail** (the 256-byte ceiling), throttled to
≤ 2 Hz plus every non-idle decision, emitted from a single point after the ladder.

```
DECIDE src=tag|ledger|none cls=rate|step|bias|none gate=<first refusing gate>
       act=none|trim|splice|step|resync frames=%+d pend=%+d gd=%+d|unknown t=%lld
```

**Pass condition:** over a quiet 30-minute window every chunk is accounted for by exactly one
`act` and one `gate`, and the counts reconcile with `soft_dropped_frames`,
`soft_inserted_frames` and `hard_resyncs`.

**The frame sums are the pass condition, and they reconcile on both boards over a clean window.**
Throttled chunks are carried in `sk=`, so the census still accounts for every chunk. **Do not
reconcile against the `Sync:` line's `err samples` field**: it is `st.err_count`, which fires the
report *at* 128 and is therefore always 128 — an error-sample count, not a chunk census. It was
labelled `chunks` once, and that label alone bought a 4.2 % "mismatch" against DECIDE for the sole
reason that the two count different things. **If that reconciliation fails, `TIMING.md`'s
description of the ladder is wrong and this plan is built on a wrong map** — that is the point of
running it first.

Parser: extend `dl-window.py`. Verify the regex against a truncated line and an absent field
before flashing; never require a trailing field.

---

## 6. Stage 3 — one selector and one horizon (pure refactors, no behaviour change)

Two independent collapses, both shadow-verified before anything is swapped. They are one stage
because neither changes behaviour and both must be in place before the arbiter of Stage 4.

### 3a. One error selector

```c++
enum class ErrSource { Tag, Ledger, None };
enum class ErrClass  { Rate, Step, Bias, None };
struct ErrorView {
  int64_t  us;
  ErrSource src;
  int64_t  age_us;
  bool     trusted;      // subsumes tag_fault_until_us, DL_ERR_STALE_US, tags_fresh
  ErrClass cls;          // set by the arbiter, see Stage 4
};
ErrorView active_error(const ServoState &st) const;
```

Today `coarse_on_tags`, `tag_err_live`, `tags_fresh` and `now < tag_fault_until_us` are recomputed
independently at the hard-resync branch, the window step, the fast splice, the tag-fault judge,
the split repair's disarm ([L5747](components/snapclient/snapcast_client.cpp#L5747), which spells
the test out in full a third time) and the unmute gate — each with slightly different staleness
handling. **[C]**

**Verification:** run both selectors live, log a mismatch counter, change nothing. Bar: zero
mismatches over a session including an injected starvation and an `inject_split`. Only then swap
the consumers.

**Measured 2026-09-02 (`scripts/bench/errsel-bar.py`, shadow in `cee71d1`).** Bar met in letter:
`srcdiff = 0`, `valdiff = 0` across ~96 k cumulative chunks per board and through every phase.
Read the sharpness before believing it:

    phase                     arms exercised     srcdiff   events
    quiet, both boards        tag only                 0   -
    starvation 300 ms on A    ledger + tag             0   2 hard resyncs, 1 OOR
    split +1000 us on A       tag only                 0   -
    board B, every phase      tag only                 0   -

The two selectors can only differ OFF the tag path, so a phase that never leaves `live=tag`
proves nothing. Exactly one phase — the starvation on A — produced a real tag→ledger transition,
and the selectors agreed through it. That is a genuine pass over one board and ~882 chunks, and it
is thinner than "zero over a session" sounds.

**Two findings for the plan itself:**

1. **`inject_split` does not exercise the selector.** Tags stayed live on both boards throughout,
   so this half of the bar tested nothing about tag-vs-ledger. It perturbs the ledger, which is a
   different thing from making the tag path untrustworthy. To exercise the extra conditions
   directly, shrink `tag_stale_ms` via `servo_param` or starve for longer than
   `DL_ERR_STALE_US`; a 300 ms starvation only just reaches the transition.
2. **`active_error()`'s two extra conditions have never bound.** `dl_err_at_us != 0` and the
   `tune_tag_stale_ms_` accumulator gate changed no outcome, including across the one real
   transition. They are stricter than the live test and fail toward the ledger, so they are safe —
   but they are also **untested code in a load-bearing path**, which is the thing to decide about
   before Stage 4 consumes `ErrorView`. Keep them and record that they have never fired (so nobody
   later "simplifies" them believing they carry weight), or drop them and lose nothing measurable.
   **Do not swap the consumers while that is undecided.**

### 3b. One visibility horizon

"How long until a correction shows up in the measurement" is encoded **five ways**, four of which
are the same physical quantity from the same inputs with different clamps: `blank_ms` (500 ms),
`resync_blank_ms` (1200 ms), the per-chunk computed `ring + pipeline + 2·block`,
[travel_horizon_us_](components/snapclient/snapcast_client.cpp#L4366) (`ring + pipe + 2·block_n`,
clamped 1–5 s), and the flat `PHASE_TRANSIENT_US` (4 s). **[C]** This is `TIMING.md` §11's
"constants that encode a relationship" failure one level up: they are written as independent
literals and will drift apart the first time `block_n`, the ring depth or the codec changes.

```c++
enum class Horizon { TagBlank, CoarseBlank, PhaseTransient, SpliceInFlight };
int64_t visibility_horizon_us(Horizon purpose) const;   // ring + pipe + k·block, per-purpose k and clamp
```

All five encodings become one function differing only in `k` and clamp, each documented with what
it is waiting for.

**Status:** `visibility_horizon_us(clamp)` exists and three `travel_horizon_us_()` sites already
use it. That much is identical by construction — it is `clamp > 0 ? max(clamp, travel) : travel`
and those sites applied the same max inline — so it needs no shadow bar. `enum class Horizon` is
declared and unused; the rest of the collapse is outstanding.

**What reading the remaining sites turned up (2026-09-02): the tag blank is computed two different
ways, and nothing said so.**

    L1297   dl_blank_until_us_ = timestamp + visibility_horizon_us(blank_ms)   -> max(travel, blank)
    L1608   dl_blank_until_us_ = now + blank_ms                                -> flat, travel-blind
    L3213   dl_blank_until_us_ = now + blank_ms                                -> flat, travel-blind

Same field, same purpose, one site travel-aware and two not. **Unifying them lengthens two blanks,
which is a behaviour change and not the pure refactor this stage claims to be** — so it needs a
measurement, not a tidy-up, and it must not ride along with the enum.

**Two decisions to take deliberately, before any consumer moves:**

1. Do L1608/L3213 become travel-aware? If yes it is a control change and belongs in a build of its
   own with a before/after on the tag-blank behaviour, not in Stage 3.
2. Does `PHASE_TRANSIENT_US` become live or stay a flat 4 s? It is armed at five sites as
   `max(existing, now + PHASE_TRANSIENT_US)` and is the one encoding that cannot reproduce a live
   value by construction. **Write down which, and why, when it is decided.**

**Verification for whatever does move: shadow first.** Log old and new side by side for one session
and change nothing. Bar: the new function reproduces each old value within that value's own clamp
on ≥ 99 % of chunks, and every divergence is explained before any consumer is swapped.

---

## 7. Stage 4 — one position arbiter, one in-flight ledger

One function that, given the `ErrorView`, the gates and the pending-motion ledger, returns the
frames to move this chunk. Hard resync, window step, bang-bang and fast splice become four
*policies* inside it, not four sites.

**The in-flight ledger is the window step's**, which is the better of the two: frame-exact landing
markers tested against `played_frames_total_` with a two-block margin, and a sign guard so the
subtraction can never manufacture a wrong-way step. The splice's chunk-horizon estimate is
replaced by it. Hard resync records into it too — today it records into neither, which is why a
resync followed by a window step can double-count.

Serial step-and-verify becomes the rule for *all* position motion, not just in-window motion:
never step while a step is in flight.

**Bar:** `resync-test.py` post-hole convergence unchanged (< 100 µs within 10 s of a 300 ms
injection, held 5 s) and `converge-time.py` boot-to-lock unchanged, both n ≥ 4 per board. This is
where the WS3.1 invariant instrumentation stops being three copy-pasted blocks.

---

## 8. Stage 5 — membership and disruption as *inputs*

This is the robustness stage, and it is where v2 differs most from what exists.

### 5a. Feed the timebase move forward

Today a consensus adoption or a deadline-source switch moves the deadline under the audio and the
loop discovers it as error, several blocks later, through a signal it has just been told to
distrust. The worst measured case is a source switch with the two mappings **29 ms apart**: the
deadline and the published phase both stepped 29 ms, the delay loop correctly refused it for a
minute, and the coarse machinery walked the audio over audibly.

But the size of the move is **known at the instant it happens** — it is `new_offset − old_offset`.
Feed it directly into the position ledger as a pending displacement:

* the arbiter starts from the known step instead of rediscovering it;
* the tag blank and the kp-event re-arm stay (the *measurement* is still invalid across the move);
* `RENDER_PHASE_UNKNOWN` is published across the transient, as now.

**[M] Test:** `inject_split`-style hook that forces a source switch of a chosen size. Pass: audio
displacement ≤ 1 frame for a forced switch of 1 ms, and convergence within 10 s for 30 ms, against
today's baseline for both.

### 5b. Grade a join and a leave as first-class events

A membership change is currently measured only as collateral: `|median error|` 154 µs within 15 s
of one, against 93 µs elsewhere (p90 674 vs 286). Make it a graded test rather than a known cost.

**[M] Protocol:** with two boards settled and the analyser running, bring a third in and out on a
timer, `n ≥ 8` each way. Report wire p2p and time-to-return-inside-±10 µs. Bar for v2: **no
audible correction on either settled board**, and return inside 10 s. The mechanism that should
deliver it is 5a — a join changes the mean by a known amount and every device steps to the same
place at the same time, so with feed-forward the move is common-mode and the wire should barely
move.

### 5c. Bound and announce the fallback

The PI already holds rather than steering when the deadline is on the local Kalman fallback while
peers exist — correct, and the reasoning (the clock-offset estimator sits *inside* `err_tag`) is
sound. Two additions:

* **Bound the hold.** An indefinite hold at the learned crystal offset is right for minutes and
  wrong for hours. State the horizon explicitly and log the transition when it is passed.
* **Announce it.** A device on the fallback should publish `RENDER_PHASE_UNKNOWN` and a flag, so
  peers know its phase is not comparable rather than averaging it in. **[C]** Check whether it
  already does; if it does, say so in one place and delete the question.

### 5d. Rule 4, generalised

Audit every consumer of a possibly-absent signal for the absent/stale/zero distinction. The boost
now gets this right in three stages after `d9224e4` (fresh → held-as-bound within 30 s → tracking
gain). The window step's `gd` sanity check, the unmute group-agreement gate and `render_align`
each make the same decision independently. Each should read one helper with the same three-state
answer.

---

## 9. Stage 6 — convergence by feed-forward

Boot state is path-dependent today: NVS integral, NVS align bias, cold-start TSF crystal seed,
fast boot Ti, and different gain schedules on the restored vs cold paths. **[C]** That is four
sources of initial condition, and it means convergence times are only comparable between boards
with the same NVS history — which quietly invalidates most A/B comparisons of boot behaviour.

**Target:** one documented initial-condition rule.

* **Rate** initial condition: NVS integral if present and finite, else the TSF crystal seed, else
  zero. One expression, one log line naming which was used. (Both already exist; the branching is
  the problem, not the sources.)
* **Position** initial condition: the resync window, from the measured displacement, with
  step-and-verify. Already right — keep.
* **Bias** initial condition: NVS, refused outside the cap. Already right — keep.
* Delete the cold/restored gain-schedule split, or state in one place why it must stay and what it
  costs in comparability.

**Bar:** `converge-time.py`, n ≥ 4 per board, from *both* a cold NVS and a warm one, reported
separately. Today ~10–14 s post-injection including the 5 s hold; boot-to-lock should be stated
the same way and not regress.

---

## 10. Stage 7 — delete what the census says is dead

Every row is a **question answered from Stage 2's census over a week of logs**, not a decision.
Deleting an unfired path is free; deleting a rarely-fired one is not. Anything with a nonzero
census stays and gets a comment saying what fired it and when.

| Candidate | Question | Delete if |
|---|---|---|
| Bang-bang soft steer | does it fire at all on S3 with the rate lock healthy? **[C]** says it cannot — `trim_holds` is false only when the lock fails | zero firings; keep behind the no-rate-lock path only |
| Accounting-split repair | does it fire outside a TAGFAULT? It is *disarmed whenever tags are live*, and TAGFAULT *pre-arms* it — so it may run only in the state its own trigger cannot diagnose | no firings outside a TAGFAULT → fold into the fault handler |
| The align kick | it delivers a *position* error as a *rate* command, concurrently with the PI's own response to the same deadline move | see Stage 8 — it should not survive v2 in this form |
| `reanchor_after_relock_` | off by default already; does the forced repair measurably improve post-relock alignment on the wire? | no difference at n ≥ 8 relocks |
| Autotune | one-sided, default off, never validated | keep off; delete once Stage 8 lands a symmetric gain rule |
| `fill_corr` | measured, never applied, **by design** — applying it was the largest audible defect ever measured here | keep as a diagnostic, mark it so in exactly one place |
| `r_push` epoch machinery | 31–35 % out of range by its own counter | keep only if a consumer is planned |

### The ledger's remaining role

After Stages 3–7 the prediction is used for exactly five things: the tags-absent fallback, the
tag-fault judge, the stale bailout, the unmute anchor, and the splice horizon — the last as a
*length* in whole chunks, which is bias-immune by construction (a bias of a few ms is a fraction
of one chunk).

**[M] The question worth asking, and not before Stage 7:** can the tags-absent fallback be
replaced by *"hold the last trim, do not move position"* plus the existing stale bailout? That is
the honest response to having no measurement, and it would remove the ledger from the control path
entirely, leaving it as diagnosis — which collapses the tag/ledger arbitration that §0 identifies
as the largest structural cost in the file.

**Shadow it, do not cut it.** Log what the fallback *would* do across a month of real tag outages
before removing anything. And do not attempt it before Stage 7: the fallback is what several gates
fall back *to*, and a census taken before those gates change measures the old system.

---

## 11. Stage 8 — the deadline actuator, and the tightest delta

Two changes, both gated on Stage 1 having produced an honest reference.

### 8a. Retire the align kick

`render_align` writes two actuators: it moves the deadline, then delivers the same correction
again as a rate command capped at 1.5 ppm. The kick exists because the PI takes τ to walk the
audio to a moved deadline, which is a real problem — but the answer is a *position* move, not a
rate one. With Stage 4's arbiter and one in-flight ledger, a bias step of D µs is exactly a
pending displacement of D µs, delivered frame-exactly and verified, and the PI never sees it as
error at all.

**[M] Test, already half-built:** `servo_param align_bias_us` measures the deadline→wire gain and
`align_bias_kick_us` the kicked form. Compare both against a third, position-delivered form at the
same magnitude, wire-graded, n ≥ 6 steps each. Pass: position delivery reaches the same wire
displacement with fewer µs of overshoot and no trim excursion.

### 8b. Differential rate feed-forward — MEASURED, and the premise does not hold

**SF_d was run on 2026-09-02 (6.0 h, 147 937 rows, 7473 bins, p2p gate 60 µs — quote the gate,
the same quantity spans 3× across gate choices). Both of this stage's premises failed.**

    tau     SF(d_wire)   SF(trim_diff)   SF(achieved)   ratio d/trim
     5s         6.280         5.317          4.381         1.18
    10s         7.341         6.441          5.179         1.14
    30s         8.357         7.833          5.382         1.07
    60s         8.594         8.000          5.626         1.07

    sd: achieved(wire) 3.712   fs_diff 3.740   trim_diff 4.818 ppm

**1. The disturbance is broadband and downstream.** `SF(d)` moves +3 % between τ = 30 s and 60 s,
i.e. it has saturated: `d` carries essentially no structure at the timescales the fine loop works
on (τ = 120 s). Gain cannot reject what lies outside the loop's bandwidth, and `SF(achieved)` 5.38
against `SF(d)` 8.36 says the loop already removes ~35 % and no more is available that way. **A
τ/Ti sweep reads null. Do not run one.**

**2. Feed-forward does not address it either.** Feed-forward cancels a *predictable* term.
Broadband noise entering downstream of the command is not predictable, so the crystal
feed-forward this section was built around cannot cancel it. Both of the stage's two levers are
answered by one measurement, and the answer is neither.

**3. The sizing fact was wrong by 11×.** This stage was written around ~0.33 ppm of differential
rate noise (1 µs in ~3 ms). Measured: **3.71 ppm achieved, 4.82 ppm commanded** — 1 µs in 0.27 ms.

**What is left is the source.** `sf-d.py` names the candidates: rate-lock delivery, the I2S
driver, the `fs` estimator. And the commanded 4.82 ppm is the same quantity that produces the
visible sawtooth in the live plot — position is the integral of rate, the rate is held constant
between updates, so straight ramps with slope changes are what a rate actuator doing position work
draws. Measured 2026-09-02: `trim` sd 4.59 ppm on board A integrated over the offset's ~3 s
correlation time is 13.8 µs, against 15.1 µs of observed position wander — the commanded rate
noise accounts for essentially all of it. **Shrinking the commanded noise is now the stage, and it
is upstream of both original levers.**

Which one applies is a single measurement:

```
d = fs_diff − trim_diff       (per-capture, from the analyser CSV)
SF(d) FLAT across tau     → d is broadband: it enters DOWNSTREAM of the command, outside the
                            loop's bandwidth. A tau/Ti sweep reads null.
SF(d) GROWING with tau    → d is slow: the loop can see it, so tau/Ti is the lever.
```

**The rule above was stated inverted in this plan until 2026-09-02**, against `sf-d.py`'s own
docstring and against the definition of a structure function — SF grows with τ for a *slow*
signal and saturates for a broadband one. Read backwards it sends you to a gain sweep for a
disturbance the loop cannot see, at five membership changes a try. The tool is authoritative; this
paragraph now agrees with it.

**Run SF_d before writing any code for this stage.** The test is already written —
`scripts/bench/sf-d.py` — and it takes the achieved differential rate from the *wire slope*, not
from `fs_b − fs_a` (per-capture offset noise ~32 ns gives a 30 s slope ~1e-4 ppm, ~400× better
than the frequency columns). Do not rebuild it, and do not substitute a correlation test:
`corr(fs, trim) → −1` whatever the actuator does, because that is the feedback identity.

If it says feed-forward: the term to feed
forward is the *loop-derived* crystal (the integral, which is exactly that), not the TSF-derived
one — the two disagree by 2.6 ppm in the differential (R9.4), and the TSF-derived one is the
wrong number. The integral must keep running underneath as the safety net, because a crystal
drifts with temperature.

**Bar for the whole stage:** over ≥ 30 min and ≥ 6000 rival-clean samples — ≥ 6 disjoint
1000-sample blocks with median block-p2p ≤ 1 µs and worst ≤ 2 µs; ≥ 6 disjoint 5-minute blocks
with |mean| ≤ 0.2 µs and SE ≤ 0.1 µs **computed from block-means variance, not `sd/√n`**
(consecutive samples from one pipeline are not independent, so `sd/√n` is a lower bound, not the
standard error).

---

### 8c. The gain-symmetry audit

Rule 3 stated as a closing task, because it is the rule this project has broken most often and the
only one whose violations are invisible from a single board's log.

Enumerate **every term in the trim that is a function of anything not common to both boards**, and
for each state the differential motion it can generate for a given common-mode input:

| Term | Common or per-board? | Status |
|---|---|---|
| the error-magnitude knee (25 µs) | per-board — each board reads its own `e` | tolerated only because the `gd` bound clamps it; **the bound is the symmetry mechanism, not the knee** |
| the gd boost and its held-gd fallback | `|gd|` is the same number on both boards → symmetric | correct shape (`d9224e4`); verify the `n` fix from §4 |
| `align_kick` | per-board | retired by 8a |
| the resync-window floor τ | per-board — one board's window can be open while the peer's is shut | measured 2026-08-29 22:58 at 2–4 ppm differential, 2–3 µs/s of wire walk for 30 s |
| the cold/restored Ti split | per-board, depends on NVS history | Stage 6 collapses it |

**[M] The audit is not checkable from one board's log, and that is the point.** Log every boost
decision with **what the peer's corresponding decision was** — its `e`, its `gd`, its resulting
`kp`. Without that, "the gains were symmetric" is an assumption, not a measurement, and the
09:58:57 episode (A boosted ×12.5 to kp 0.104 while B stayed at 0.008 — ~6 ppm differential,
~120 µs on the wire, 40 s to unwind) is exactly what it looks like when the assumption is wrong.
The observer is the natural place to correlate this: it emits `PHASEIN`, the group-consensus
*inputs*, naming which peer moved, and the group delta's output cannot diagnose itself.

**Bar:** `structure-function.py` 1 s structure function no worse than 0.30 µs, and the 3-minute
medians within ±8 µs, over a quiet hour — the numbers `HANDOFF.md` records for build 30.

---

## 12. Order, and what may not be combined

```
0   live defects (align seeding, stale comment)      ── DONE 638c714
1   the reference                                    ── CLOSED. Open: B's residual tail is 2x A's
2   DECIDE line + parser                             ── frame sums reconcile; prereq for 4, 7, 8c
3a  one error selector          (pure refactor)      ── shadow, zero-mismatch bar
3b  one visibility horizon      (pure refactor)      ── shadow, >=99% reproduction bar
4   one position arbiter        (structure)          ── convergence bars unchanged
5   membership & disruption as inputs (behaviour)    ── the robustness goal
6   convergence by feed-forward (behaviour)          ── the speed goal
7   delete by census; then shadow the ledger fallback ── needs a week of 2's logs
8a  retire the align kick       (behaviour)          ── position delivery replaces a rate command
8b  differential rate feed-forward                   ── the tightness goal; SF_d decides its shape
8c  gain-symmetry audit                              ── closes rule 3; needs the peer's decision logged
```

**No stage changes control law and structure in the same build.** 3a and 3b are refactors and must
demonstrate identical behaviour before 5 and 6 change any. 7 comes after 5 and 6 because the
fallback paths are what several gates fall back *to*, and a census taken before those change
measures the old system. 8c comes last because it audits the gains that 5, 6 and 8b introduce, not
only the ones that exist today.

**Stage 1 is measured and does not block:** the reference tracks the wire 1:1 to ~7 µs MAD, so the
stages gated on it may proceed. The rule it was written to enforce still stands for what remains —
if board B's residual tail cannot be explained, cap the goal at what a reference with that tail can
deliver rather than proceeding and attributing the residual to the controller.

---

## 13. Bench protocol, for every stage

**After any flash, restart the analyser** (`bench-tmux.sh restart-analyzer`; `flash` does it for
you). Its running acquisition does not survive both I2S buses going down: it serves edge-free data
and reports "no BCLK edges" indefinitely, raising nothing, while a fresh one locks in two seconds.
Measured three times on 2026-09-02, once per OTA. A frozen `test.csv` after a flash is this, not a
dead bench.


Unchanged from what already works, restated so no stage skips it:

1. `scripts/bench/preflight.py` — refuses unless all boards are latency 0 on the same stream.
2. **One build, one `./reflash-speakers.sh`, then leave it alone.** A reflash is five consensus
   membership changes; thirteen in one session made the operator the dominant disturbance and most
   of the "events" chased were self-inflicted.
3. Verify the running build over the API (`device_info.compilation_time`). Never from "OTA
   successful" — a replug 40 s after an OTA reboot silently rolled both boards back once, and a
   persist gate then "did not work" for a whole build because the build was not running.
4. Grade from the CSV: `dod-grade.py` and `structure-function.py` (rival-gated), `dl-window.py` (byte-offset anchored,
   `--log-tail-mb 60`). Never from plots, never from `[HH:MM:SS]` greps — the logs span days and
   carry no date.
5. **Check `rival` before trusting any skew number.** MLS44 gives ~0.03; a run at 0.94 means
   whole-frame errors are masquerading as findings.
6. Headline tests per stage: `inject_split(+1000)` (audio must not move), `inject_starvation(300)`
   (< 100 µs within 10 s, held 5 s), a forced source switch (Stage 5a), a join/leave cycle
   (Stage 5b), boot-to-lock from both cold and warm NVS, and a quiet-hour structure function.
7. Prefer runtime `servo_param` A/B over a reflash wherever the change is a parameter. Two of the
   best results on this bench (knee 25 / τ_min 5) were found that way and only then compiled in.

---

## 14. What not to touch

* **The rate actuator and its dither.** Single owner, documented invariant, ~10 ns residual.
* **"Publish only your own raw line, never the consensus."** Feeding the mean back is positive
  feedback and the whole group can walk while every device agrees.
* **Deterministic adoption — stepped, not slewed.** The slew was tried; it made adoption
  path-dependent and cost 2.7× on sd. Stepping is safe *because* it is deterministic.
* **`fill_corr` remaining unapplied.** Applying it produced the 10 ms / 52 ms offsets.
* **`MEDIAN_WINDOW = 31`.** The residual is variance, not lag; shortening it has been tried and
  reverted.
* **The nominal prediction slope.** Predicting with the realised slope is arithmetically better
  and destabilised the loop in two minutes; the slope's insensitivity to trim is load-bearing.
* **Anything in Stage 7's table with a nonzero census.**
