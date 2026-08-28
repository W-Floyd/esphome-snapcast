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

A single-integrator plant under proportional feedback gives first-order decay:

    trim = Kp * (D_hat - D_target)      =>   tau = 1/Kp seconds

`TRIM_KP_RUN` is 0.25 ppm/us today, i.e. tau = 4 s. **Do not reuse it.** Against a 3.35 s sample
interval that is marginal, and the sample interval is the dominant lag — see below.

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
* **`supports_render_tags()` and the freshness gate.** A signal that reports its own absence is the
  whole reason this is trustworthy; do not let the fallback hide it.

## Semantics that must be decided, not left open

* **Tag loss mid-flight.** The loop holds its last trim — NOT its last error — and reverts to the
  ledger path after a bounded number of missed updates. Decide the bound; do not leave it implicit.
* **Setpoint changes.** `buffer_ms` changes from the server, `static_delay` from config. Both step
  `D_target`. Step the setpoint and let the loop converge; do not splice to it.
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
