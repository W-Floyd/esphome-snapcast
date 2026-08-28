# PLAN — control the measured transport delay, not a model of it

The servo steers on a prediction. Everything defensive around it exists because that prediction is
built from a ledger that can be silently wrong. A render tag measures the same quantity directly, so
the defences become unnecessary rather than better tuned.

This is specified to the point where building it is mechanical. Every number in it was measured on
the bench on 2026-08-28; none are estimates.

## Why the present error signal needs defending

    error_us = predict_next_play_us_() - deadline
               └─ EWMA pivot over (frame index -> DAC time), extrapolated along the nominal slope ─┘

That pivot is the ledger. The ledger is perturbed by events it cannot observe: a pipeline restart
discarding queued frames, silence padding, the phantom-frame clamp, a mixer rebuild at a different
fill. So the code keeps a second independent estimate — the sink's measured depth — and `drift` is
their disagreement. `DRIFT_REPAIR_US` (2000) arms a window, `DRIFT_REPAIR_HOLD_US` (3 s) confirms
it, and the trim is pinned to the PI integral throughout. All of it is a lie-detector for one's own
bookkeeping.

**The ledger-derived error cannot see the displacement it causes.** Measured with
`inject_split(+1000)` on one board:

    err_live   -57..+95 us      the servo NULLS it by displacing real audio; it is blind to the
                                displacement, because creating it is how the error reached zero
    err_tag    moved ~1100 us   it measures where the audio actually is
    diff       -1020..-1052     ratio 1.02-1.05 against 1000 us injected
                                recovered to -33 us within ~15 s of the negated restore

Same result as the render-phase test the same morning: 0.94 for the measured form against 0.12 for
the inferred one.

## The measurement, and its precision

A render tag gives, for one frame, the server time it belongs to and the local instant it rendered.
Subtract the deadline (which carries the clock offset) and what remains is the render error, free of
any ledger:

    err_tag = (adjusted_ts - real_frames/rate) - deadline(tag.server_ts + tag.offset/rate)

Block-means variance sweep, ~334 arrivals per report:

    quiet window   B=1  27.3   2 19.1   4 13.1   8 8.4   16 6.7   32 5.2   64 5.0 us
    ideal 1/sqrt(B)      27.3     19.3     13.7     9.7      6.8      4.8      3.4
    active window        24-30    22-26    22-24    21-23    21-23    21-22    22-23

So there is a real white-noise component of ~20-27 us per sample that **averages away**, plus a
correlated component that does not. Practical resolution: **5-13 us per report when quiet, ~22 us
when the delay is genuinely moving** — and the latter is signal, not error.

**Unexplained, and worth resolving before relying on the tighter figure:** board A flattens at
~12.5 us where board B reaches 5.0. Same firmware, same stream, 2.5x difference in achievable
precision.

## The proposal

    plant        the pipeline as a BLACK BOX with transport delay D
                 dD/dt = -trim_ppm   (1 ppm = 1 us/s; positive trim plays faster, D shrinks)
    measurement  D_hat from tags, available at ~100 Hz
    setpoint     D_target = buffer_ms - server_latency - static_delay   (all known constants)
    actuator     the I2S rate trim
    fast path    splices, unchanged, for corrections faster than the loop can make

It does not matter HOW the delay arises. Ring depth, DMA padding, mixer fill, a restart at an
unobserved level: each is a term the present design must model separately and can get silently
wrong, and each is invisible inside a box whose output is measured.

> **REVIEW — `D_hat` is not locally observable; the loop variable is `err_tag`, target 0.** The
> raw transport delay carries the unbounded local-vs-server clock offset — the code says exactly
> this where the raw delay is computed (`snapcast_client.cpp` ~1059: "Carries the ... CLOCK OFFSET,
> which is unbounded and drifts at the crystal difference"), and the precision figures quoted above
> were measured on the deadline-corrected `err_tag`, not on `D`. So `buffer_ms`, `server_latency`
> and `static_delay` enter through `deadline()`, not as a separate `D_target` constant; the
> "setpoint" row of this box is the same subtraction stated twice. Two consequences worth writing
> down: (1) the clock-offset estimator stays load-bearing and INSIDE the loop — its wander
> (~100 µs/s on wifi jitter, per the inject_split ramp-rate comment) is a disturbance a 3–10 Hz
> loop chases harder than the 3.35 s servo does. What filters it? (2) `deadline()` linearity holds
> only "for fixed buffer and offset" — the tag anchor must be re-anchored on a `buffer_ms` change,
> which belongs in the setpoint-change semantics below.

> **RESPONSE — accepted; the box is wrong and is restated below.** The loop variable is `err_tag`
> and the setpoint is **0**. `buffer_ms`, `server_latency` and `static_delay` enter through
> `deadline()`, so the "setpoint" row was the same subtraction written twice. There is no separate
> `D_target`.
>
> **(1) Nothing filters the offset wander, and nothing needs to — PROVIDED it is shared.** When
> `deadline()` uses the shared TSF mapping (`deadline_on_shared_tsf_`), every device in the group
> chases the same wander in the same direction. It is common-mode by construction, which is the
> entire reason that path exists, and inter-device alignment — the thing being optimised — is
> untouched. It costs absolute-latency accuracy, which nothing here is grading.
>
> That makes **"shared mapping available" a precondition of the delay loop, not an incidental**. On
> the local-Kalman fallback the wander is independent per device and does differentially misalign;
> there the loop must widen tau substantially or hold trim. Add to the semantics list.
>
> **(2) Correct, and the anchor being fresh does not save it.** `tag_anchor_*` is republished every
> chunk (~26 ms), so a `buffer_ms` change reaches the anchor almost immediately — but the ~250 ms of
> audio ALREADY IN FLIGHT was scheduled under the old deadline, so `err_tag` steps by the change for
> one pipeline depth and the step is counted twice exactly as described. Handle it with the mechanism
> already in the codebase: **mark the tag stream invalid for one pipeline depth after any setpoint
> change**, the same way the freshness gate refuses a stale observation. Moved into the semantics
> section.

A single-integrator plant under proportional feedback gives first-order decay:

    trim = Kp * (D_hat - D_target)      =>   tau = 1/Kp seconds

`TRIM_KP_RUN` is 0.25 ppm/us today, i.e. tau = 4 s. **Do not reuse it.** Against a 3.35 s sample
interval that is marginal, and the sample interval is the dominant lag — see below.

> **REVIEW — pure P leaves a standing error of crystal_offset/Kp; the integral must survive.** The
> plant is not `dD/dt = -trim_ppm` alone: the crystal difference (~40 ppm on this bench) drives
> `dD/dt` even at zero trim. Under `trim = Kp·e` the steady state is `e = crystal_ppm/Kp` — at
> today's Kp that is ~160 µs of standing error. The current servo's integral IS the learned crystal
> offset (the split-hold pins to it for exactly that reason). So this is a PI loop, or P plus a
> feedforward from the rate lock's learned offset — say which. And the PI mechanics the deletion
> list doesn't name but the loop still needs — the trim clamp, conditional-integration anti-windup
> (the +164.9 ppm runaway cited under Risks is what its absence looks like), the Kp
> acquire→run decay — belong in "What must be kept".

> **RESPONSE — conceded, this is a straight error.** The plant is
> `dD/dt = -(trim_ppm + crystal_ppm)`; the crystal term drives drift at zero trim. Under pure P the
> steady state is `e = crystal_ppm/Kp`, which at ~40 ppm and Kp 0.25 parks **~160 µs** off target —
> two orders above the ~1 µs alignment now being achieved. Writing `dD/dt = -trim_ppm` in the plant
> box hid a term the current design already handles.
>
> **It is a PI loop**, and the integral should be **seeded from the rate lock's learned baseline**
> rather than re-acquired: that value already exists, the split-hold pins to it for exactly this
> reason, and seeding avoids a slow crystal re-learn on every start. P-plus-feedforward is the same
> thing with the adaptation removed, and loses the ability to track crystal drift with temperature.
>
> "What must be kept" gains: **the trim clamp, conditional-integration anti-windup, and the
> Kp acquire→run decay.** The +164.9 ppm runaway under Risks is what the second one's absence looks
> like, so listing it as a risk while omitting the mechanism that prevents it was inconsistent.

> **REVIEW — trim noise integrates into wire wander; pick Kp against that, and state tau.** At
> N=10 / 10 Hz the per-update noise is ~8.5 µs, and each update dithers the rate by Kp·σ. The wire
> offset is the integral of the DIFFERENTIAL rate (documented at snapcast_client.cpp ~298), so two
> devices' independent trim dither random-walks the wire between corrections. The doc bounds loop
> bandwidth at ~0.5 Hz from dead time but never states the target tau or the new Kp — the
> noise-vs-bandwidth trade is the actual tuning decision here, and it is left open.

> **RESPONSE — accepted, and the arithmetic says noise is NOT the binding constraint.** For a
> first-order loop with measurement noise sigma at update interval T, the closed-loop output noise is
> approximately `sigma * sqrt(T / 2*tau)`. At sigma = 8.5 µs and T = 0.1 s:
>
>     tau =  4 s   ->  0.95 µs
>     tau = 30 s   ->  0.35 µs
>
> Both are far below the ~1 µs the pair currently holds, so trim dither does not set the floor at any
> sane tau. **Dead time does.** With L ~ 250-300 ms, tau must be many multiples of L; tau = 30 s is
> ~100L and comfortable, tau = 4 s is ~15L and is where the realised-slope experiment already
> oscillated.
>
> **Stated as the starting point, to be graded rather than trusted: N = 32 samples (~320 ms, sigma
> ~5 µs), update 3 Hz, tau = 30 s, Kp = 0.033 ppm/µs, plus the integral above.** That is a starting
> point and not a derivation — this project's history is that gains which look right on paper
> oscillate on hardware, so it gets graded against the wire like everything else.

## The report cadence is a diagnostic artefact, not a measurement constraint

Tags arrive per DMA descriptor, ~100 Hz. The present diagnostic logs one number per 3.35 s report,
which is a 300x throwaway. A delay-controlled loop should average over a **short** window and update
at that rate:

    N=10 samples (~100 ms)   resolution ~27/sqrt(10) ~ 8.5 us,  update 10 Hz
    N=32 samples (~320 ms)   resolution ~5 us,                  update 3 Hz

Either is far better than 3.35 s. Dead time is one pipeline depth (~250-300 ms), so loop bandwidth
is bounded near 0.5 Hz regardless; an update rate of 3-10 Hz keeps the sampler out of the way of
that bound instead of being the bound.

## What is deleted

Not "improved" — **deleted**, because with no ledger there is nothing for a split to be a split
between:

* `drift` / the accounting split, `DRIFT_REPAIR_US`, `DRIFT_STEADY_BAND_US`
* the 3 s `split_pending` trim hold, and `trim_split_holds` with it
* `Accounting split repaired` and the `pushed_frames_total_` step
* the starvation re-baseline and the phantom-frame clamp's re-arming logic
* `predict_next_play_us_()` and the EWMA feedback pivot, with the nominal-vs-realised slope
  bias documented there as ~70% of the differential floor

> **REVIEW — this list contradicts the fallback.** "Tag loss mid-flight" below says the loop
> "reverts to the ledger path after a bounded number of missed updates", and "What must be kept"
> requires degrading "to the existing behaviour". The existing behaviour is the ledger servo, which
> is built from `predict_next_play_us_()`, the pivot, and the defences deleted here. Either the
> fallback is genuinely the old path — then all of this stays compiled in and the deletion is
> really "demoted to fallback" — or the fallback is something simpler (hold trim, splice on gross
> error) and a resampler-in-path configuration permanently runs without a rate servo. Both are
> defensible; the document currently claims each in a different section. Decide which.

> **RESPONSE — conceded, and decided: the fallback is the SIMPLER one, and the deletion is real.**
> Keeping the ledger servo as a fallback means maintaining two timing paths, and the one that runs
> rarely is the one that rots — it would be exercised only in configurations nobody measures, which
> is how the stream-scoping bug survived unnoticed.
>
> So: **on tag loss the loop holds its last trim and the existing splice path handles gross error.**
> `predict_next_play_us_()`, the pivot, and the split machinery are deleted outright.
>
> **The consequence, stated plainly rather than buried:** a resampler-in-path configuration runs with
> **no rate servo at all**, only splices. That is acceptable for this bench (no resampler; mixer
> blending is transient, and holding trim through an announcement is fine) but it is a real
> capability regression for anyone who resamples. If that is unacceptable upstream, the honest
> alternative is **demotion, not deletion** — and then this section must say "demoted to fallback"
> and the maintenance cost of two paths must be accepted explicitly. It cannot be left as it was,
> claiming both.

**And the split-hold's inter-device cost goes with them**, which is the largest identified term:
measured A frozen at +64.00 ppm while B steered at +38.15 ppm, ~26 ppm for 3 s ~ 78 us of skew.

## What must be kept

* **The ledger, for FLOW CONTROL.** `pushed - played` also answers "how much is in flight, push or
  drop". A tag says where audio is, not how much is queued. It stays; it just stops being a timing
  reference. `buffered_audio()` reports the same thing measured, and is the better source.
* **A fallback when tags are unavailable.** Deliberately suppressed through a resampler, while the
  mixer blends a second source, and for client-inserted silence/splices. The loop must degrade to
  the existing behaviour, not to nothing.
* **Splices**, for corrections faster than a ~0.5 Hz loop can make.

  > **REVIEW — the splice/trim boundary needs a number.** At a trim authority of X ppm, an error of
  > E µs takes E/X seconds to integrate away; the acquisition handoff "at whatever `D` then is"
  > could hand the loop several ms, i.e. minutes of convergence. State the error magnitude above
  > which the fast path splices instead of the loop trimming, and its hysteresis — it is the same
  > class of decision as the tag-loss bound below, and equally not to be left implicit.

  > **RESPONSE — accepted, and the mechanism already exists.** `fast_splice_threshold` (1 ms in the
  > example config) is exactly this boundary: 43 single-frame splices of ~23 µs each close a
  > millisecond in about a second, inaudibly. Reuse it rather than inventing a second threshold.
  >
  > The arithmetic confirms it is needed: at tau = 30 s an error takes ~3 tau = 90 s to converge, so
  > anything at the millisecond scale must splice. **Rule: above `fast_splice_threshold`, splice;
  > below it, trim.** Hysteresis: splice only until the error is inside the threshold, then hand to
  > the loop, and do not re-arm until it exceeds 2x the threshold — otherwise the two mechanisms
  > fight at the boundary, which is the limit cycle the trim deadband comment already warns about.
  >
  > This also resolves the startup handoff: acquisition splices down to within the threshold, and the
  > loop takes over from there rather than "at whatever D then is".
* **`supports_render_tags()` and the freshness gate.** A signal that reports its own absence is the
  whole reason this is trustworthy; do not let the fallback hide it.

## Semantics that must be decided, not left open

* **Tag loss mid-flight.** The loop holds its last trim — NOT its last error — and reverts to the
  ledger path after a bounded number of missed updates. Decide the bound; do not leave it implicit.
* **Setpoint changes.** `buffer_ms` changes from the server, `static_delay` from config. Both step
  `D_target`. Step the setpoint and let the loop converge; do not splice to it.

  > **REVIEW:** since the measurement is deadline-corrected (see the note under "The proposal"),
  > a `buffer_ms` change also invalidates the tag deadline anchor — the extrapolation is exact
  > only "for fixed buffer and offset". Re-anchor on the change, or the step appears twice: once
  > in the setpoint, once as a corrupted measurement. Also: "let the loop converge" on a step of
  > tens of ms is minutes at realistic trim authority — this conflicts with "do not splice to it"
  > unless a large setpoint step is exactly the "faster than the loop can make" case the splice
  > path exists for.
* **Startup.** No tags until audio flows, so acquisition stays on the existing splice path. The
  handoff to the delay loop happens on the first fresh tag, at whatever `D` then is.
* **Do NOT make any hold common-mode across devices.** Freezing every device captures each one's PI
  output at an arbitrary point in its own transient, converting N momentary corrections into N
  sustained rate offsets. This was proposed and is wrong.

## How it gets judged

One test, already tooled, and it must be run before anything downstream is trusted:

    inject_split(+1000 us) on one board.
    A delay-controlled servo should NOT MOVE THE AUDIO AT ALL: the ledger is not in its loop,
    so a ledger bias is not an error it can see.
      present servo:  displaces ~1100 us, then reports a clean error having done so
      this design:    wire displacement ~0    <- pass

> **REVIEW — verify the pass criterion survives the ledger's remaining job.** The ledger stays for
> flow control, and `inject_split` biases exactly that ledger. If a +1000 µs accounting bias can
> reach a push/drop or starvation decision, the injection moves audio through flow control even
> with the servo blind to it, and "wire displacement ~0" fails for a reason that is not the servo.
> Check where `inject_split`'s bias can propagate under the new design before treating a nonzero
> result as a servo failure — or a zero result as proof, if the bias no longer reaches anything.

> **RESPONSE — accepted, and it changes the test into a two-sided one.** A null result is only
> evidence if the perturbation demonstrably landed. As written, "wire displacement ~0" is
> indistinguishable from "the bias reached nothing at all", which is the more likely outcome: 1000 µs
> is 44 frames, while the starvation latch triggers on `available_frames <= 0` and the push/drop path
> is driven by the chunk deadline rather than by the ledger. So the bias probably propagates nowhere
> once the servo stops reading it — and the test would prove nothing.
>
> **Revised test, with a positive control:**
>
>     inject_split(+1000 us) on one board, and require BOTH:
>       (a) the bias LANDED    -- RECON `drift` (and err_live, kept as a diagnostic) moves ~1000 us
>       (b) the audio did NOT  -- wire displacement ~0
>     (a) without (b) = the servo is still ledger-coupled somewhere.
>     (b) without (a) = the injection reached nothing; the test is VOID, not passed.
>
> Before running it, trace where a `pushed_frames_total_` bias can still reach a push, drop or
> starvation decision under the new design, and size the injection so it would cross one of those
> thresholds if the coupling existed. An injection too small to matter cannot demonstrate immunity.

Secondary, on a settled bench, against today's baseline:

    wire (B-A)      median +1.2 us   MAD 20.0   sd 46.7   p2p 242.8
    group delta     A -9 us MAD 26   B +0 us MAD 16, 100% availability, 0 outliers

`sd` and `p2p` should improve as the split-hold excursions disappear. The median is already ~1 us
and cannot improve meaningfully — **do not judge this on the median.**

## Risks

* **It replaces the load-bearing timing path.** A bug presents as a timing anomaly, the exact class
  this project has spent sessions chasing. Build it on a quiet bench with nothing else in flight.
* **Board A's 12.5 us floor against board B's 5.0 us is unexplained.** If it is a property of the
  measurement rather than of that board, the achievable resolution is the worse number.
* **The dead time is real.** ~250-300 ms of pipeline. A loop tuned as if the measurement were
  instantaneous will oscillate. `predict_next_play_us_()` records precisely this failure from the
  realised-slope experiment: trim ran away to +164.9 ppm and medians oscillated within two minutes.
* **Do not trust a floor measured on a churned bench.** Every reflash causes five consensus
  membership changes, worth 154 us vs 93 us in |median error|. Today's best numbers came from
  twenty uninterrupted minutes.
* **Read `CLAUDE.md` first.** Three of four instrumentation defects found on 2026-08-28 produced
  confident, wrong numbers, and two conclusions in this document were reached only after earlier
  measurements of the same quantities had to be retracted.
