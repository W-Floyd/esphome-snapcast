# TODO

Worklist only. The reasoning, measurements and dead ends live in the commit messages and in
comments at the relevant code — several fixes carry a `REVERTED:` or `DISPROVEN:` note saying
what was tried and what it measured. Read those before re-attempting anything in the Sync
section; most of the obvious approaches have already failed on hardware.

## Upstream ESPHome

- **Land the buffered-audio API.** Three stacked PRs against `dev` in kahrendt's #16317 style
  (an outside contributor cannot base a PR on a branch in their own fork). Retires the
  six-component fork. `render_latency()` / `buffered_audio()` as durations, published as seqlock
  snapshots carrying the instant they describe (`audio::AudioDepth`). PRs #18753/#18754/#18755
  were opened and closed; resubmit fresh rather than force-pushing.
- **Send the `frames_to_microseconds` overflow fix separately and first.** Frame it as a latent
  overflow on a public helper with no documented ceiling — no current upstream caller reaches
  4295 frames. gtest is written and lands under `tests/components/audio/`, which had no tests.
- **`speaker::set_rate_adjustment(float ppm)`**, default no-op, per-SoC in the i2s speaker.
  `i2s_rate_lock` stays as the fallback for older ESPHome, and this is the only way rate
  steering reaches a non-S3 target.
- **Carry the mixer's in-flight fix to the `render-latency` branch** before upstream submission.
  Landed on `speaker-render-latency` as `07f80de5e` (`dbg_inflight_us`: the mixer counts frames
  handed to the sink, the sink already publishes frames accepted, and the difference is the audio
  between them — the term the composite depth omitted, worth up to one sink publish interval).
  That branch is the flashing one; `render-latency` is the submission one and does not have it.
- **`mixer` does not yield when the sink stops draining.** Fixed on the fork (`74d32d9dd`) as a
  defensive yield; not proposed upstream. Needs a reproduction that actually stops a sink and
  measures CPU before it is worth filing.
- **`speaker_source` spins forever on an unmixable announcement.** Unbounded allocation on an
  unsatisfiable retry (1742 rings in two minutes), and no terminal failure for the announcement.

## Sync

**WHAT PLANTS IT IS STILL UNKNOWN — five hypotheses now dead.** n=14 cycles with per-resync
metrics (2026-08-27, MLS stimulus, analyser at sd 5.8 µs):

- **Snapshot staleness: DEAD.** r = +0.03 against signed delta, +0.32 against |delta|, against
  a critical 0.532. Independent confirmation of the `DISPROVEN` note already in
  `snapcast_client.cpp`, which was established at the 3.7–13 ms seed scale; this extends it to
  the ~130 µs hard-resync scale. That note warns the reasoning "looks compelling and is wrong",
  and it was proposed here anyway before the code was read — read it first.
- **Disturbance size: a two-level effect, not a correlation.** Cycles at 18 chunk-drops gave
  |delta| 73 ± 45 µs (n=11); three cycles at 79 drops gave 378 µs. Pooled, `drops` correlates
  with |delta| at r = 0.874 and looks like a mechanism. WITHIN the 18-drop subset every
  predictor collapses — rsyncs +0.40, sink_swing +0.56, age_max −0.08, none significant. The
  pooled correlation is three outliers wearing the shape of a trend.
- **Within a fixed disturbance, nothing measured predicts the displacement.** It is 73 ± 45 µs
  and apparently random.

The leading untested candidate is PADDING, and the prediction is already written in the code at
`dbg_padded_frames`: "Two devices differing by N frames of padding should sit N * (1e6 / rate)
µs apart on a logic analyser, which is the prediction to test." 130 µs is 5.7 frames. Padding is
the only mechanism that moves the output while leaving every internal metric self-consistent,
which is the measured signature: acoustic offset stable to sd 5.8 µs while both boards' own
reported error wanders ±60 µs. `scripts/padding-test.py` tests it; it needs the PADDISP log line,
because `pad=` is the last field of RECON and the logger truncates it mid-number.

**A HARD RESYNC PLANTS A RANDOM DISPLACEMENT OF ~130 µs, AND IT DOES NOT NEED A RE-LOCK.**
Measured 2026-08-27 on an MLS stimulus (sd 5.8 µs, zero frame errors — the first instrument
this was measurable on), n=7 cycles of a 500 ms `server_latency` step out and back:

    |delta| mean 130 µs, median 130, range -142..+203     signed mean +50, sd 134
    control, no step applied (n=3):  |delta| mean 7.8 µs
    ratio: 17x

Three things it is NOT:

- Not the re-lock. Six cycles muted and re-locked (mean |delta| 121 µs); the seventh corrected
  "staying unmuted" with no lock at all and moved 188 µs. The displacement rides on the HARD
  RESYNC, not on the mute/converge/unmute path that was the working hypothesis all afternoon.
- Not the anchor. r = -0.641 over 6 locks, and cycles 1, 3, 4 all carried `anchor 22` and
  landed +125.8, -7.7, +115.2. Consistent with the earlier refutation, now on a good instrument.
- Not deterministic, not bistable, and not a per-board constant. End states +49, -74, -78,
  +46, +214, +64, +257 — spread 334 µs, no preferred value. A two-state reading survived four
  cycles and died on the fifth.

So a static per-device calibration cannot work: the quantity is re-drawn on every resync. What
would work is either making the resync's landing point deterministic, or measuring and
correcting the residual after each one.

**RETRACTED — "the firmware cannot see a sub-millisecond pipeline difference, by construction".**
That was recorded here on 2026-08-27 and is WRONG. It rested on one quiet window (12:58–13:04)
in which the pipeline happened to be sitting at its configured maxima, so every field read as a
round number identical on both boards:

    xfer 20000   own 89728   sink_audio 120000   sink_lat 120000   down_audio 140000
    B - A:    0        0            0                 0                 0

Sampled again while the buffers were not saturated, the same fields are measured at microsecond
resolution, take hundreds of distinct values, and DO differ between the boards:

    sink_lat   A 117732   B 114694   B-A -3038 µs   (24 distinct values)
    xfer       A  14785   B  13311   B-A -1474 µs   (15 distinct)
    own        A  88980   B  88481   B-A  -499 µs   (268 distinct)

Only `down_audio`, `pending`, `delay` and `inflight` are round and identical, and those are
configured buffer TARGETS rather than measurements. So the accounting is not blind to
sub-millisecond differences, and a per-board latency residual has not been ruled out as
observable — it remains untested. Generalising "by construction" from a single saturated
window was the error; the lesson is that these depth fields are only meaningful when the
pipeline is not pinned at its limits.

What the code DOES say, at the starvation clamp in `snapcast_client.cpp`, is that the framework
"restarts it with an unpredictable buffer fill level between this feedback point and the DAC --
invisible to the accounting, so playback would settle audibly offset with clean-looking sync
reports". That is a real mechanism for the ~100-250 ms case it was written about, and a
re-baseline handles it. Whether a sub-millisecond residual of it survives is still open.

Consequences worth keeping in mind before proposing a fix:

- The sub-frame part is NOT the problem. The servo holds it to ~2 µs. The whole error is a
  constant displacement chosen when the pipeline starts.
- `server_latency` cannot correct it: it is integer MILLISECONDS (`Client.SetLatency`).
- `inject_split` cannot either: it perturbs the accounting, which provokes the self-repair, and
  the repair is corrective — its net steady-state effect is ~0.
- Two hypotheses were tested and REFUTED on 2026-08-27. The accounting split difference matched
  the skew exactly at one instant (+90 vs +295 against 205 µs measured) but is uncorrelated
  across a capture (r = +0.08 while the split swings ±2500 µs). And anchor-at-unmute does not
  predict the planted offset: board A re-locked carrying `anchor 19976 us` and the skew moved
  50 µs.

**THE WIRE OFFSET IS THE INTEGRAL OF THE DIFFERENTIAL RATE, AND NOTHING ELSE.** Measured with the
analyser's `fs_a_hz`/`fs_b_hz` columns over three quiet runs of 92–499 s:

    integral of (fs_b − fs_a) vs the measured offset
      corr −0.997 / −0.999 / −1.000     slope −0.996 / −0.983 / −1.008
      offset sd 4.7 / 3.3 / 24.2 µs  ->  residual sd 0.38 / 0.14 / 0.31 µs   (99–100% explained)

Slope −1.0 with a sub-µs residual, so there is no second term to look for. This retires a whole
class of hypothesis: every "static offset" chased in this file — the per-boot values, the ones no
on-device field could see, the ones that re-planted at every event — was accumulated rate difference
that stopped accumulating. It explains the invisibility directly, since each device compares itself
against its own prediction and a rate difference integrated in the past leaves no trace in any
present-tense field. **Read this before proposing a mechanism for a static offset.**

Current floor, measured within clean segments after the last revert: **static ~2 µs, sd 3.3–4.7 µs,
p2p ~10 µs**, reboot recovery ~42 s. Simultaneous restarts land within ±10 µs; a lone restart or a
recovery event lands anywhere in ±130 µs, and that spread is the integral above, not a separate
defect.

- **THE ONE MISSING MEASUREMENT: achieved rate, referenced outside the servo loop.** It would do two
  jobs — de-trend the prediction (see the pivot item) and, published in the beacon beside
  `drift_ppm`, let each device integrate the difference and know its own relative offset without an
  analyser. **The trim cannot do this job, and that is now measured rather than assumed.** The cheap
  first step is done: the report publishes the time-MEAN applied trim per window (`Trim window:`,
  with the window's audio time and its covered fraction), and `i2s-skew.py --replot --annotate a.log
  b.log` scores any candidate rate reference against the analyser's own rate columns.
    - **The aliasing was real and the fix works — at the RATE level.** Window-mean differential trim
      tracks the analyser's differential achieved rate at **corr +0.976..+0.979** while the rate is
      still moving, against the **−0.778** recorded for end-of-window snapshots. Measured on 96 and
      127 windows over 231 and 425 s. Sampling was genuinely throwing away most of the signal. (In
      steady state the correlation falls to +0.773 for the ordinary reason that there is barely any
      variation left to correlate: the true differential rate's sd is 1.465 ppm there.)
    - **It still fails as an OFFSET reference, for a different and more fundamental reason: an
      unknown constant.** The differential trim sits **−5.25..−5.40 ppm** from the true differential
      rate (−5.246, −5.272, −5.400 on three runs including one in steady state, so it is stable and
      not scatter). That is the CRYSTAL DIFFERENCE: each board's trim cancels its own crystal error,
      so the differential trim carries their difference, and nothing on the device knows it. It is a
      rate, so it integrates linearly and forever — **~540 µs per 100 s**, against a steady-state
      offset floor of **6.96 µs sd**. Hence the offset integral from trim explains **1–6%** where
      the analyser's own rate columns explain **96–99%** (steady state: corr −1.000, slope −1.003,
      residual 0.15 µs).
    - **THE CONSTANT IS ALREADY AVAILABLE ON-DEVICE, in `tsf-local`.** The `Offset ramp` line
      publishes each board's `tsf-local` — the rate of (TSF − local clock), measured against the
      radio timebase and therefore OUTSIDE the audio servo loop. Its differential is **−5.347 ppm
      (sd 0.559)**, matching the trim's constant to within the measurement. Subtracting it, scored
      against the analyser on 119 quiet windows:
        - trim alone: constant **−5.050 ± 0.163 ppm** → **505 µs per 100 s**
        - trim − differential `tsf-local`: constant **+0.170 ± 0.165 ppm** → **17 µs per 100 s**
      So the crystal difference is fully accounted for and the remaining constant is statistically
      indistinguishable from zero — a 30× cut in the unbounded term, from data the device already
      has. (Caveat: consecutive 3.3 s windows are autocorrelated, so ±0.165 is optimistic; the
      constant is "consistent with zero", not "known to 0.165 ppm".)
    - **What still blocks it is the RESIDUAL, not the constant.** After the constant is removed the
      per-window residual is **0.708–0.916 ppm**, i.e. **~71–92 µs per 100 s** against a 6.96 µs
      floor — still **10–13×** too coarse. So `trim − tsf-local` is a much better rate reference than
      trim alone but is still not good enough on its own, and the remaining work is reducing that
      residual rather than chasing the offset. This is what the least-squares achieved-rate
      measurement against server time is for.
    - Consequence for the beacon: the field to publish is **not** the trim. It is either the
      achieved rate itself, or trim and `tsf-local` together — the second is nearly free, since
      `drift_ppm` already travels and `tsf-local` is already computed.
    - **WITHDRAWN: a "blocker" recorded here claiming the reference is only maintained while
      leading.** That was wrong, and the error is instructive. There are TWO fields for the same
      physical quantity: `tsf_rate_ppm_`, measured on the network task inside `broadcast_` and so
      genuinely leader-only, and `offset_rate_ppm_`, measured on the player task from the samples
      the offset filter already takes, in every role. The header says so explicitly at
      `offset_rate_ppm_`. The log line labelled `tsf-local` prints `offset_rate_ppm_`, so every
      measurement taken from those logs — including the −5.347 ppm differential — was always the
      all-roles field and was never stale.
      What actually happened: a new log line was added for `tsf_rate_ppm_`, the wrong field of
      the two, and its leader-only behaviour was then read as a property of the reference rather
      than of the line. Two boards' log counts (79 vs 0) confirmed the line's behaviour and were
      taken as confirming a claim about the quantity. **Before concluding a quantity is
      unavailable, check which of the same-named fields the evidence came from.**
    - **BUILT AND RUNNING, NUMBER NOT YET VALIDATED: the crystal difference is computed
      on-device.** `crystal_ppm` is appended to `TsfPacket` (no version bump — appending cannot be
      misread, and a bump costs a half-flashed fleet its shared timebase in both directions; the
      receiver accepts both lengths), sourced from `offset_rate_ppm_` via a cross-thread mirror.
      A follower differences it against the leader's and logs `Crystal: mine … leader … delta …
      ppm`, exposed as `TsfSync::crystal_delta_ppm()`. Both sides measure against the same AP TSF,
      so the AP's own crystal cancels. **Diagnostics only — nothing steers on it**, and nothing
      should until the 0.708 ppm residual after correction (~71 µs per 100 s against a ~7 µs
      floor) is understood.
        - **VALIDATED AGAINST THE WIRE.** With both probed boards following the SAME leader, their
          published rates difference to the pair's crystal difference:

              on-device (a.mine  - b.mine ) = +5.425 +- 0.128 ppm   (9 paired settled samples)
              cross-check (a.delta - b.delta) = +5.421 ppm          (matches to 0.004, as it must)
              logic analyser, same pair       = +5.25 .. +5.40 ppm
              on-device minus wire midpoint   = +0.100 ppm  ->  10 us per 100 s

          So the constant that made the trim useless as a rate reference is now measurable on the
          device to within the wire's own spread: **535 µs per 100 s of integrated error becomes
          ~10 µs**, against a ~7 µs floor. The 0.100 ppm offset is inside one sd of the sample
          spread, so the honest claim is "agrees within measurement precision", not "0.1 ppm off".
        - Two conditions on using it, both measured. **It needs ~40 s of settling**: at 18.6 s
          uptime the estimate read 4 ppm from its settled value, which integrates to 400 µs per
          100 s — worse than the error it removes. Samples above were filtered to uptime > 60 s.
          And it is **reproducible across a power cycle to 0.002 ppm** (+36.808 against +36.810),
          so once settled it is as stable as a hardware constant should be.
        - A first reading of `delta +44.379 ppm` looked like a failed prediction of −5.35, but the
          leader was an unprobed board at −7.835 ppm, not board a. The delta against *whoever
          leads* is not the pair quantity; differencing two peers' raw rates is, which is why
          publishing the raw value rather than only the delta was the right primitive.
        - **What this does NOT fix:** the residual. After the constant is removed the trim still
          tracks the true rate only to 0.708 ppm per window, ~71 µs per 100 s, ~10× the floor.
          The constant is solved; the residual is the remaining blocker.
    - **TOPOLOGY LIMITATION, and it bites the actual use case: followers never beacon.** All three
      `broadcast_` call sites are leader-gated (`role == Role::LEADER`, the leader cadence block,
      and the moment of assuming leadership). So a follower only ever sees the LEADER's
      `crystal_ppm`, and **two followers cannot see each other's** — which is the normal case for a
      stereo pair in a group of five. `mine − leader` is the pair difference only when the leader
      happens to be the partner.
        - Publishing the raw rate rather than only the delta was the right call for this reason:
          the raw value is the composable primitive, and any two peers' raw values difference to
          their crystal difference. The delta-vs-leader is a convenience, not the quantity a
          stereo pair needs.
        - The natural fix is to let a follower publish `crystal_ppm` too, very infrequently —
          it is a slowly-varying hardware constant, so once per 30 s is ample and the airtime is
          negligible. Not attempted yet; it changes who transmits, which is a topology change and
          wants its own measurement.
    - **Do not "fix" this by de-meaning.** Tried on the data: subtracting the series mean drops the
      analyser's own fs check from 96% to 2%, because the true differential rate has a real nonzero
      mean (+0.61 ppm on one run, a genuine 177 µs ramp over 290 s) and de-meaning destroys exactly
      the term the offset is made of. The constant has to be *known*, not removed.
    - Also measured in passing: the trim moves ~1.30× further than the realised differential rate
      (fit slope +0.771), and the realised trim differs from the PI's demand by sd 0.5–0.65 ppm with
      a 4 ppm p2p — more than the documented 0.15 ppm quantisation step, though common-mode enough
      that the differential barely notices (sd 5.89 realised vs 5.74 demanded).
    - **So the reference must be measured against SERVER TIME from the plant.** That is the next
      item and it is now the only surviving route, not merely the preferred one. Measuring it from
      the credit stream was tried and failed on jitter — see the pivot item.
- **The feedback pivot is the dominant differential term, and the loop around it is why the residual
  oscillates rather than looking like noise.** Closed form: an EWMA at α lags a ramp by
  `c = (1−α)/α = 63` steps, and `S·2205 = 50000 µs` exactly, so

      predicted − ideal = c·(S·2205 − Δt) = c · 50000 · δ = 3.15 µs per ppm of δ

  where δ is the ACHIEVED rate against nominal. Measured with `fs_a_hz`/`fs_b_hz`:
    - δ = **+38 ppm** on both boards → **+120 µs of COMMON absolute latency**, scaling with the
      pivot's smoothing. Inaudible for imaging, real for lip-sync.
    - differential rate: mean +0.024 ppm but **sd 1.644 ppm**, p2p 12.3 — the rates agree on average
      and never at an instant, because the trim wanders (differential trim sd 1.78 ppm, same thing).
    - so differential pivot bias = 3.15 × 1.644 = **5.18 µs** of the 7.14 µs differential median
      floor. About 70%.
    - **loop gain:** median → trim (`TRIM_KP_RUN` 0.25) → achieved rate → pivot bias (3.15 µs/ppm) →
      median = `0.79`. Just under unity, hence the smooth ~20 s oscillation, and hence lowering KP
      buys more than its own factor.
  Smoothing it is the WRONG fix: smoothing scales the bias with `c`. **Three corrections have been
  tried on hardware and all three failed.** The mechanism is not in doubt; the reference is.
    1. **Deviation of the applied trim from a slow mean** — much worse. The mean carries the
       acquisition transient: the trim rails to hundreds of ppm while acquiring, so at a 110 s time
       constant the mean sat at +611 to +748 ppm for minutes, pinning the deviation at its ±50 ppm
       clamp and injecting ~45 µs of prediction bias that then decayed slowly. Medians of 250–460 µs,
       +3.1 ppm residual rate difference, wire ramping past +540 µs and still climbing at 40 s.
    2. **Seeding that mean at convergence** does not rescue it — the trim still travels from hundreds
       of ppm down to ~50 ppm AFTER converging, and that settling shares the 10–40 s band with the
       oscillation the mean must preserve. No time constant separates them. (Not flashed; reasoned.)
    3. **Achieved rate measured from the credit stream** — much worse, for two independent reasons,
       both worth keeping:
         - **The credit timestamps carry ~300 µs of jitter**, not the ~20 µs assumed. A 30 s
           two-endpoint baseline read +66.8, +48.6, +45.6, +55.2, +57.6, +43.7 ppm — spread ±10 ppm —
           so even after a 1/4 EWMA the residual is ~3.4 s × 5 ppm ≈ 17 µs, THREE TIMES the 5.2 µs it
           removes. Only a least-squares fit over all ~600 credits in the window divides that down.
         - **A multiplicative scale on the span is unsafe at any rate accuracy.** It multiplies
           `pushed − fb_mean_frames`, and a re-baseline leaves those counters inconsistent — `r_push`
           has been measured at −7958592 frames, a span of −180 s, which at 50 ppm injects 9 ms.
           Observed: the pair sat at +5753 µs, then jumped to −6094 µs, each position held with a
           within-window MAD near 1 µs. At scale exactly 1.0 a corrupt span costs nothing; with a
           scale it costs milliseconds. Bound the span, or apply an absolute µs correction from a
           span known to be sane.
  Also do not retry the note, since withdrawn, that the pivot CANCELS between devices: that used the
  0.018 ppm MEAN differential rate where the pivot multiplies the instantaneous value.
  **Surviving direction:** avoid the extrapolation entirely — compare in FRAMES rather than time,
  which removes the nominal-rate assumption the bias comes from at its root.
- **`TRIM_KP_RUN` stays at 0.25. 0.1 was tried, measured, and REVERTED.** The loop-gain argument
  for lowering it is still sound — gain is `KP × 3.15`, so 0.79 against 0.32, removing a
  1/(1−G) ≈ 4.8× amplification on top of the linear factor. The cost side was underpriced.
    - Measured at 0.1 on a recovery: **trough +548 µs, tau 80 s, settled 295 s**, against ~42 s at
      0.25 and worse than the ~135 s expected.
    - **The deciding term, which nobody had priced: the offset the recovery LEAVES BEHIND.** It
      landed at **−155 µs**, outside the ±130 µs band recorded at 0.25.
    - That follows directly from the finding at the top of this section and should have been
      predicted. The offset is the integral of the differential rate, so a recovery freezes
      wherever the integral has reached when the servo nulls the rate. Lower gain nulls it more
      slowly → the integral runs longer → **the planted offset is larger.** So KP trades
      steady-state noise against recovery time *and* against the size of the static offset every
      event leaves — and that last term is the whole problem this work exists to solve.
    - Revisit only once the anchor stops planting an offset: with little to converge from, the
      integration window shrinks and this cost goes with it.
    - **Baseline to beat, at KP = 0.25 in steady state over 180 s:** wire offset **sd 6.20 µs**,
      differential achieved rate **sd 1.515 ppm**, on-device differential median **MAD 6.00 µs**.
      The analyser's own rate columns reproduced the offset at corr −0.999 / slope −0.992 /
      residual 0.33 µs over that window, so the instrument is trustworthy at this floor.
    - **Judge it on the wire and on the differential MAD, never on sd of the differential median** —
      network events put that at 209 µs against a MAD of 6, which is the medians-not-sd rule in the
      one place it is easiest to forget.
    - The 0.1 measurement has NOT been taken: every window attempted so far graded a post-flash
      transient (medians −298…+239 µs, trim railed to +376 ppm). Recovery settling is now measurable
      rather than eyeballed — `i2s-skew.py` reports tau and time-to-settle per recovery — but note
      that a large excursion recovers under `TRIM_KP_ACQUIRE` while muted, so coarse re-lock speed is
      governed by the acquire gain and `KP_RUN` governs only the fine settling after unmute. The
      quoted 42 s / 150 s figures do not say which they measured.
- **BUILT 2026-08-26 (`5b751f9`): the decay schedule is in.** KP = RUN + (ACQUIRE − RUN)·e^(−age/τ)
  once converged, τ = 20 s, snapped to RUN at 3τ; ACQUIRE unchanged while muted. `age` is time since
  the last disturbance event, and the events are boot, hard resync (late and early), re-baseline,
  split repair, and a TSF role change (checked per chunk, not at report cadence — a 3.3 s delay
  would spend most of the decay before the schedule noticed). `kp=` is published on the `Trim
  window` line, appended at the end so the existing parser still matches.
    - **Endpoints deliberately unchanged (0.5 → 0.25)** so the first measurement grades the
      *schedule* alone. Lowering the RUN endpoint toward 0.1 is the follow-up this is meant to make
      affordable — separate change, separate measurement, and it is the one with the real upside.
    - **How to grade it.** Null test first: steady state must be UNCHANGED, because the schedule
      differs from the old fixed switch only after an event — wire offset sd 6.20 µs, differential
      achieved rate sd 1.515 ppm, on-device differential median MAD 6.00 µs at KP = 0.25 over 180 s.
      Then the target: the LANDING OFFSET after a lone restart, previously ±130 µs, plus tau and
      time-to-settle from `i2s-skew.py`'s per-recovery report. Judge on the wire and the MAD, never
      on sd of the differential median.
    - **The residual feedback path, so it is checked rather than assumed:** a hard resync is an
      event and resyncs are error-triggered. The separation is 50 ms against single-digit-µs
      steady-state error, and the measured triggers are supply outages. If resync RATE ever starts
      tracking KP, this schedule is the first suspect.
- **Dynamic KP is the right answer, but ONLY scheduled on something outside the loop.** The
  0.25-vs-0.1 bake-off is choosing a compromise between two things a schedule would give you both
  of, so it is worth less than fixing the schedule. Two forms have already failed on hardware
  (recorded at the PI block): **scheduling the gain on `|median error|` with hysteresis
  limit-cycled**, and a **one-way latch on sustained smallness** did not cycle but was
  history-dependent and did not address the starvation-recovery case that justified it.
    - **Why the `|error|` schedule cannot work.** The loop *trails a ramp by* `rate/KP` (measured
      ±1–2 ms at KP = 0.1 against ~200 µs at KP = 0.5), and differential trim is `KP ×` differential
      median to within a few percent in every session measured. So the steady-state error is
      **inversely proportional to the very gain being scheduled**: raising KP shrinks the error the
      schedule watches, which lowers KP, which grows the error back through the threshold. Hysteresis
      does not remove that, it only sets the period — which is what "limit-cycled" means. The plant
      being an integrator is why the related bang-bang trim also limit-cycled *structurally*.
    - **Why the CURRENT switch is safe, and what the rule is.** KP is already dynamic (0.5 acquiring,
      0.1 converged); it survives because it keys off `st.converged`, a LATCH set by a full in-band
      median window and cleared only by a resync/mute event. It is scheduled on a discrete REGIME,
      not on the continuous variable the gain controls, so no path runs from the gain back to the
      scheduling input. Hence the code's "keep this switch boring".
    - **The form worth building:** decay KP from ACQUIRE toward RUN over a time constant after the
      last disturbance EVENT (boot, re-baseline, leadership change, hard resync) instead of stepping
      once at convergence. Open-loop in the error, so a cycle is structurally impossible, and it
      gives fast recovery right after an event with a low steady-state gain. The earlier
      "history-dependent" objection does not apply: history is only dangerous when the history IS the
      controlled variable, and a timer since a discrete observable event is not.
- **Events plant an offset because the two servos hunt independently through recovery.** Not a
  separate defect — the integral above — but the events are what make it large, and they are all in
  the logs. Landing values measured in one session:
    - three boards restarted together **+0.9 µs**; both boards together **−5, +7.7, +26 µs**
    - board b alone **+89, +115, −56, +127 µs**
    - after b's repair cascade **+85 µs**; after an injected starvation + repair **−133 µs**
  The cascade is understood: the mixer's incomplete depth report showed +25509 µs, the repair fired
  on it, and subtracting those 1125 frames from `pushed` created the −25488 µs split that a second
  repair answered 14 s later. Neither was needed. `MIX_RESIDUAL_MATCH_US` stops it at the head, and
  is now a regression guard rather than a live defence since the mixer reports `dbg_inflight_us`.
- **NEXT: is the anchor's per-device `latency` error the source of the planted offset?** This is the
  root-cause candidate for the whole static-offset problem, and the code already names it: *"the
  reason a reboot starts 620 us out at all is the re-baseline anchor planting an offset. Fix that
  and there is little left to converge from, and KP stops mattering."*
    - Mechanism: the seed sets `pushed = played + latency` from **this device's own** measured
      pipeline latency. Any per-device error in that latency becomes a PERMANENT phase offset,
      because the servo then measures against the very prediction it anchored and reads ~0 while
      the audio sits that far off. Unobservable on-device by construction.
    - The recorded landing values already fit it: three boards restarted **together** landed at
      **+0.9 µs**, both together at −5/+7.7/+26 µs, but **board b alone** at +89/+115/−56/+127 µs.
      Simultaneous restarts have correlated pipeline states so their latency errors cancel; a lone
      restart's does not.
    - **FIRST RESULT: seeds DO step the wire, but the step is not explained by the anchored
      latency.** Eight seeds fired with `inject_starvation`, three measurable on the wire:

          latency_ms  age_ms   step_us   floor_us
             220.0     17.0     +17.6      0.05
             230.0     13.1     -13.1      0.05
             233.2     17.9    +222.3      0.11

      Every step is >100× the noise floor, so the anchor plants a real discontinuity — that half
      of the hypothesis holds, and it is the half that distinguishes it from "the servo integrated
      to a new resting point".
    - **But the correlation test as designed was worthless, and that is my error.** `latency` is
      near-constant across all three seeds (220–233 ms) while the step swings +17.6 → −13.1 →
      +222.3 µs, so it *could not* have tracked. Snapshot `age` is near-constant too (13–18 ms).
      On reflection the design was wrong from the start: the planted offset should be the *error*
      in `latency`, and a near-constant `latency` says nothing about its error. **A future test
      needs an independent estimate of that error, not the value.**
    - **THE MECHANISM, measured end to end, and it identifies the −52 ms spike.** `SEEDDRAIN` now
      measures the anchor's error on-device: the seed asserts the resident audio takes `latency` µs
      to drain, and the playout feedback (ground truth from the speaker callback, no `pushed` in the
      path) says how long it really took. Then the RECON line and the repair complete the chain:

          11:48:29  SEEDANCHOR latency=234399 age=25146 frames=10336
          11:48:29  SEEDDRAIN  anchored=234399 actual=227016 err=-7383
          11:48:31  RECON drift=-51747   held steady
          11:48:34  RECON drift=-51747
          11:48:37  Accounting split repaired: accounted queue ran -51747 us for 3 s; playback late
          11:49:01  RECON drift=-1       reconciled

      So the seed leaves the accounting **≈51.7 ms short of measured — one DMA buffer (50 ms)** —
      the split repair catches it after `DRIFT_REPAIR_HOLD_US` (3 s), and **for those 3 s the servo
      steers real audio against a prediction 52 ms wrong.** The repair then fixes the ACCOUNTING,
      but the audio has already moved: that is a concrete mechanism for a planted offset that no
      on-device metric reports afterwards, because once repaired every residual reads 0.
    - **REFUTED: the spike is NOT caused by seeds.** I guessed the −52 ms split spike WAS this
      post-seed artifact and checked it. Counted across both logs: **337 spike episodes against 47
      seeds**, and only **2–6% of episodes have a seed within 60 s**. The converse holds weakly —
      13/33 and 10/14 seeds *are* followed by a spike — so a seed is one trigger among several, and
      the post-seed instance I saw was simply one of many. The spike's own cause remains open, and
      "it appeared right after a seed" was a coincidence of timing in a sample of one.
    - **BUT the better lead survives, and it is not about seeds at all: the split REPAIR fires 23
      times** (13 + 10 across the two logs) on sustained splits from **4.7 ms to 57 ms**
      (−51747 ×2, +57075, +29976, +24671, +9977, +7097, +7096, +4716 …). Every firing means the
      accounting was that far off **for `DRIFT_REPAIR_HOLD_US` = 3 s**, during which the servo
      steered real audio against a prediction wrong by that much — and then the repair corrects the
      accounting, which makes the audio displacement invisible to every on-device metric
      afterwards. That is a candidate mechanism for planted static offsets that fires far more often
      than seeds do, and it does not depend on the seed hypothesis being right.
    - **QUANTIFIED, n=5, and the size is set by the TRIM DURING THE HOLD — not by the split.**
      Four provoked points (ramped splits of ±2500 µs) plus the natural one:

          repaired_us   step_us   floor_us   note
              +2471      +329.6      1.05
              -2427      -732.9     15.69    floor elevated by the previous point's aftermath
              +2539      +311.1      5.99
              -2495      -347.4      2.66
             +20000      +491.0      0.71    natural

      The two clean positive points agree to 18 µs (+329.6, +311.1) and the clean negative one is
      −347.4, so **a ±2500 µs split plants ≈330 µs regardless of sign.** The model that explains
      the sublinearity:

          split   KP*split   clamped@1000   x 3 s hold   measured   ratio
           2500       625            625         1875       330      0.18
          20000      5000           1000         3000       491      0.16

      Same efficiency both times. So the step is **the trim the servo applies during
      `DRIFT_REPAIR_HOLD_US`, saturated by the ±1000 ppm clamp** — which is why an 8× larger split
      yields only 1.5× the step. The ~17% is presumably the ~0.85 s measurement lag plus trim slew
      eating most of the 3 s window.
    - **FIX 1 LANDED AND MEASURED: hold the trim through the confirmation window.** While
      `drift_excess_since_us` times a split toward `DRIFT_REPAIR_HOLD_US`, the trim is held instead
      of recomputed from a median measured against a prediction the code is about to declare wrong.
      Held rather than zeroed, since the current trim is the converged crystal cancellation.

          +-2500 us split      steps                    mean magnitude
          before               +329.6 +311.1 -347.4     329 us
          with trim hold       +187.3 -256.9            222 us     (~33% cut)

      Both after-floors clean (7.60, 5.61 us), and `Trim held`/`Trim released` confirmed engaging
      for 3.3 s. A 42% figure quoted from the first point alone was optimistic; n=2 gives 33%.
    - **FIX 2 MEASURED AND REVERTED: clearing the median window made it WORSE.** n=2, both floors
      clean:

          stage                steps                    mean magnitude
          baseline             +329.6 +311.1 -347.4     329 us
          + trim hold          +187.3 -256.9            222 us    <- best
          + median clear       +375.0 -357.7            366 us    <- worse than baseline

      **Why, and it is a rule already written in this file.** With `err_window_filled = 0` the
      median falls back to `error_us`, a SINGLE RAW SAMPLE. I argued that was "honest because it
      uses the corrected prediction" — honest about the prediction, and terrible as a measurement.
      The raw error carries hundreds of µs of network noise where the median carries tens (see
      *"sd is the wrong summary here… a differential of MAD 12 µs reported sd 129 µs"* in the method
      notes), and at KP = 0.25 a single 1000 µs noise sample commands 250 ppm of trim. So the servo
      stopped steering on stale-but-smooth data and started steering hard on one noisy sample, right
      at the moment it is most sensitive. The median window exists to prevent exactly that.
      Reverted; the stale-median account of the residual is unsupported and the residual's cause is
      open.
    - **SO THE RESIDUAL 222 µs IS UNEXPLAINED, and that is the current state.** The reasoning that
      motivated fix 2 — that the servo chases the prediction step the repair itself makes, using
      `st.err_window` samples taken against the old prediction — sounded right and matched the
      convention at `clear_playout_history_`, but the measurement rejected it. Whatever accounts
      for the remaining displacement is open. Do not re-attempt the median clear; if the stale
      window is still suspected, the way to test it is to let the window REFILL before the PI acts
      again (hold the trim for a further window after the repair) rather than to empty it.
    - **Consequence, and it is a design tension worth stating:** the 3 s hold exists to avoid acting
      on a spike, but during those 3 s the servo steers real audio against a prediction the code is
      *about to declare wrong*. The damage is done before the correction lands, and it is permanent
      because nothing in the system has position feedback. Shortening the hold, or freezing the trim
      once a split is suspected, would cut the displacement roughly proportionally.
    - Note the step scales with KP, so it pulls the opposite way to the landing offset: lower KP
      shrinks the repair's displacement while *enlarging* the planted offset after a recovery. Any
      future KP change should be judged on both.
    - The first quiet natural repair (+20000 µs → **+491.3 µs**, floor 0.71) is what opened this
      thread; the n=5 table above supersedes it as the quantification. Kept because it is the only
      NATURAL point and so the only one free of any injection artefact — and it sits on the model's
      line, which is part of why the model is credible.
    - **Also settled by a NATURAL event: the empty ring precedes everything.** The 11:57:40 burst on
      board b, at chunk resolution:

          RSYNC[0] err= 99748 ring=26 drops=0    <- ring already empty, before any discard
          RSYNC[1] err= 73627 ring=26 drops=1    <- one drop closed 26121 us = exactly one chunk
          RSYNC[2] err= 97504 ring=26 drops=2    <- then it grows
          RSYNC[7] err=853062 ring=26 drops=7

      The ring is at 26 ms at chunk ZERO, and the first discard closes exactly one chunk (1:1, as
      designed). So discarding works and is the recovery; the error runs away because with the ring
      empty there is nothing left to discard. Confirms the injected-burst finding on a natural
      event, and puts the remaining question squarely on the SUPPLY: why does the ring empty?
    - **`SEEDDRAIN` reads err ≈ −7 ms when playback is continuous** (−6778 and −7383 µs on
      `frames` 11801 and 10336 — consistent), which is a *different and smaller* quantity than the
      51.7 ms split. Both are real; do not conflate them.
    - **Known limitation of `SEEDDRAIN`:** on a dry pipeline (`frames = 2205`, one DMA buffer) it
      read +844 ms and +1017 ms. That is not anchor error — with the pipeline dry the DAC has
      nothing, so the resident 50 ms genuinely does not render until refill. The instrument
      conflates "the anchor was wrong" with "playback was stalled", and only answers the intended
      question when playback runs continuously through the drain.
    - **Two contaminations to avoid next time.** Injections were spaced 22 s while recovery at
      KP = 0.25 takes ~42 s, so every seed but the first landed mid-recovery from the previous one
      — the "before" level was never settled, and fit-and-extrapolate handles a ramp but not
      curvature. Space them beyond 90 s. And there is a **selection effect that removes the most
      extreme cases**: the five seeds with `latency = 50000` (one DMA buffer, i.e. the pipeline
      fully dry) produced no wire measurement at all, because with the audio stopped the analyser
      loses PCM lock and the offset goes NaN either side of the seed. The deepest starvations are
      exactly the ones the wire cannot see.
    - If confirmed, the fix needs a group reference that does not consume the accounting, and the
      obvious candidate is blocked: `render_phase_us` is *supposed* to be it but is measured blind
      (−15.5 ±8.5 µs against +85 µs on the wire, wrong sign, ~12σ) because it consumes
      `(pushed − played)` — the same term the bad anchor corrupts. The enabling change would be to
      carry each chunk's server timestamp through to the playout feedback, so the played frame's
      server time is known directly rather than derived from `pushed`. Validate any such rework
      against the analyser BEFORE steering on it; the current one failed exactly that check.
- **`inject_split(us)` provokes the self-repair without the chaos.** API action in
  `snapclient-base.yaml`, alongside `inject_starvation`. Perturbs only the playout accounting and
  leaves the audio alone, RAMPED in at ~100 µs/s so the servo absorbs it as ordinary drift.
    - **It must ramp, not step.** A stepped version measured its own disturbance instead of the
      repair's: +10 ms injected produced a −3.9 ms excursion and left the wire's fit floor at
      822–1204 µs against the few hundred µs being measured. Nature ramps (the accounting diverges
      over seconds, so the servo tracks it out and only the REPAIR steps); a step hands the servo a
      large instantaneous error and contributes a second, larger step of its own.
    - **The accounting has no unit finer than a frame** (22.7 µs at 44.1 kHz), so a sub-frame
      per-chunk budget truncates to zero and the ramp silently never moves. Fixed with a carry that
      spends whole frames every few chunks; rates below one frame per chunk (868 µs/s) are only
      reachable that way. The bug was visible solely because the request line prints the running
      remainder.
    - **Space points on a QUIET-WIRE GATE, not a fixed sleep.** Each repair plants a few hundred µs
      that then takes ~42 s to stop moving, so 110 s spacing left the next point's floor at 15.69 µs
      against 1.05 µs for a settled one. `scratchpad/waitquiet.py` polls until the 30 s MAD is
      ≤3 µs; a settled wire measures 0.71 µs.
- **The re-baseline anchor plants a fixed offset, reproducibly, and can be studied on demand.** Fire
  `inject_starvation(ms)` (API action in `snapclient-base.yaml`; four lines of `aioesphomeapi`,
  plaintext API, no noise PSK) and the seed arms an 80-chunk `EARLY[n] seed` burst at ~26 ms.
    - **The residual is constant within an event and varies between events**: −51747 µs (2282 frames)
      on one injection, −201225 µs (8874 frames) on another, each held to ±1 µs across 3.3 s. `debt`
      was 0 and every conservation residual 0 in both, so it is neither the padding path nor a stage
      losing audio. (An earlier note called the first value "one DMA buffer plus exactly 77 frames"
      and structural; two samples show that was a coincidence. Withdrawn.)
    - **The seed can anchor audio that does not exist**: `latency=50000 own=0 dma=0 debt=2205
      seed=2205`. That is the padding path as designed — seed on `latency`, repay `debt` when the
      padding drains. Hard to reproduce: it needs the DMA dry of real audio at the instant the seed
      runs, and four injections since produced `debt=0` because audio had refilled by then. The one
      occurrence came from a three-seed cascade.
    - **The repayment trigger was changed and has NEVER EXECUTED.** It now repays the whole debt on a
      deadline set at the seed (`seed instant + latency then`), because the seeded silence sits behind
      the real audio in the resident descriptors and the DAC plays at real time, so no query is
      needed. The old trigger fired on current padding reading empty, which the rolling DMA window
      makes wrong in both directions; measured before the change, repaying 2205 frames left the
      accounting 60 ms below the chain where not repaying would have left 10 ms. The "never below
      played" clamp is retained, so the catastrophic mode is still guarded. **Treat as a proposal
      until a starvation leaves the DMA dry.**
  Ruled out along the way, with the sign as the argument: nothing is losing audio (the sink never
  stopped — `I2SDBG` continuous with `written`/`completed` advancing — and `srcrx` stayed cumulative
  across the seed, so neither ring was discarded), and the sink computes `own` vs `latency` correctly
  (`queued + dma_real` vs `queued + dma_span`), with `held` being the DMA SPAN and not silence.
- **Two mechanisms for the per-start offset are dead**, recorded at their sites in the fork
  (`449574cc5`): `playback_delay` was ZERO on all 18 starts measured, and padded silence does not
  displace audio — two boards differing by 877 ms of accumulated padding sat 133 µs apart, so the
  sink's per-descriptor real-frame bookkeeping handles it. `pad` is published through `AudioDepth`
  and printed in RECON, so both stay cheap to re-check.
- **The −42 ms split spike.** Recurs at −42223..−42246 µs on both boards to within 20 µs, so it is
  structural; rejected by the median, so harmless *to the steering servo*, but unexplained. Since
  the in-flight fix it reads −52245/−49343 — the same spike plus the 10 ms `inflight` those samples
  carry — and the terms are self-consistent (`sum == meas`, `r_mix == 0`), so it is not a
  conservation failure. Every occurrence has `xfer=50000` (its maximum, equal to `dma`),
  `inflight=10000`, `queued=70000`, `dma=50000`, `age≈33 ms`: a full transfer buffer, i.e. the sink
  briefly not accepting. Still worth knowing why `xfer` is railed at exactly one DMA buffer whenever
  this fires — but note the "harmless" was doing less work than it looked: the median rejects it for
  steering, while the hard-resync path tests the RAW instantaneous error, so nothing shielded that
  path from a ~50 ms single-sample reading against a 50 ms threshold. See the item below.
- **REVERTED, and the diagnosis is NOT established. Do not re-attempt without the evidence below.**
  A discard budget on the late hard-resync path was written, flashed, and reverted within minutes
  because it caused a WORSE failure than the one it targeted: board B refused to discard, its error
  ran away 666 → 2657 → 8076 ms, the stale bailout forced a reconnect, and the speaker never
  restarted — `queued=0 dma_real=0 written==completed inflight=0`, frozen for over a minute, where
  the unbounded behaviour it replaced recovers in ~22 s. Reverted in `4c13250`.
    - **What the attempt got wrong.** Unbounded discarding is LOAD-BEARING: it is how the client
      consumes a genuine multi-second backlog. The fallback left behind (the aggressive soft band at
      frames/32, ~3% catch-up) cannot close seconds of lateness, so capping the discard turned a
      recoverable backlog into a runaway. The budget was also set once at episode open and never
      re-armed, so a growing real backlog could never buy the discards it needed.
    - **The original interpretation is now in doubt.** The 09:46 cascade below was read as
      self-inflicted — the correction draining the ring. But if the client was genuinely falling
      behind, draining IS the recovery and the 22 s re-lock is correct behaviour, not a defect.
      Nothing measured so far distinguishes "spurious 50 ms reading" from "real backlog starting at
      50 ms", and that distinction is the whole question.
    - **ANSWERED by the per-chunk trace, and it refutes the original diagnosis. The empty ring
      comes FIRST.** Nine bursts captured on hardware, and the split is total:
        - ring full at burst start (1697–1828 ms): **6 bursts, 0 discards every time**, initial
          error ~100 ms, all closed without the discard path being the mechanism at all
        - ring already at **26 ms** — one chunk, i.e. empty — at burst start: **3 bursts, 79 / 32 /
          4 discards**, initial error +150 ms to +1741 ms
      So discarding is not what empties the ring: by the time the error crosses the 50 ms
      threshold the ring is *already* empty in every case where discarding then happens. The
      causality runs source starvation → late playout → resync → discards, which makes the
      discards the RECOVERY. That is exactly why capping them made things worse, and it retires
      the "the correction drained the ring" reading of the 09:46 cascade.
      **So the trigger, not the response, is what wants attention: why does the ring empty?**
        - Caveat on the evidence: the burst is armed by the threshold crossing, so it shows the
          ring at that moment, not before the excursion began. "Already empty" means empty by the
          crossing. Seeing the drain start needs a rolling pre-trigger history.
        - **BUILT AND FLASHED to all five boards (2026-08-26), awaiting an event.** `RPRE` lines:
          the last 80 chunks before an arm — `dt`, `err`, `med`, `ring`, `drops` per chunk — kept
          in a 1.3 KB ring on the client, replayed six-per-line and one line per chunk when the
          burst arms, so the history adds ~25% to a burst that already runs at the flood rate.
          Recording is frozen during the replay; the frozen span is what the live burst covers.
          `i2s-skew.py` expands them into the existing per-chunk rows with NEGATIVE sequence
          numbers, so an episode reads −80…+79 continuously, and reports the drain separately
          from the discard verdict (which is still computed on the armed chunks only).
        - **FIRST RESULTS, 13:19–13:23 on board a — THE SUPPLY STOPS.** Three arms captured, and
          the two that drained read the same way:

              t= 68.7s  ring 1697 -> 26 ms over 80 chunks (2044 ms)  supply ratio 0.18
              t=279.2s  ring 1671 -> 26 ms over 74 chunks (1939 ms)  supply ratio 0.15
              t=298.9s  ring 1776 -> 1750 ms, no drain at all        supply ratio 0.99

          In both drains `err` stayed inside ±9 µs and `med` inside ±9 µs the whole way down —
          the servo was perfect until the moment the ring ran out — and the ring fell by almost
          exactly one chunk per chunk. The third arm is the already-known case: a ~180 ms
          excursion with the ring FULL and supply at real time, i.e. a prediction/deadline
          excursion with no starvation in it.
        - **CAVEAT ON THESE TWO: they are not a quiet-period sample.** Both landed minutes after
          `reflash.sh` OTA'd five boards over the same radio, so contention is a live explanation
          for a 2 s supply outage. Needs a capture with nothing else on the channel before
          "the network stops for 2 s" is a property of normal running.
        - **`dt` DOES NOT MEASURE ARRIVALS — do not read it that way.** It is the servo loop's
          cadence, and the loop runs off the backlog: through a total supply outage it kept a
          textbook ~26 ms cadence with no gaps, because it was working through ~65 buffered
          chunks. The first version of the analyser called that "supply intact" off a total
          outage. The discriminator that works is the ring's SLOPE — playout is real time, so
          `supplied = span + (ring_end − ring_start)`, and the ratio against `span` is the
          supply rate with the loop's own pacing cancelled out.
        - **Reading the verdict**: ratio &lt; 0.35 → supply stalled and the servo is a victim;
          0.35–0.85 → arriving below real time; ≈1 with the ring falling → the loss is DOWNSTREAM
          of the ring, which would point at the mixer/speaker path rather than the network. If
          `ring` is already flat at its floor at seq −80, raise `RESYNC_PRE_CHUNKS`.
        - **PARKED (2026-08-26): the ~2 s supply outage is most likely snapserver, not us.**
          The chain's trigger is now located and it is upstream of this firmware, so chasing it
          further is not the next slot's work. If it is ever picked back up, the trace stops at
          the ring and says nothing about radio vs server vs socket vs decode; instrumenting
          arrivals at `emit_pcm_` would split those, and is the same shape of change as this one.
        - Also measured, and useful on its own: an ~100 ms excursion with a healthy ring resolves
          with no discards and no muting. The path only engages when the ring is gone.
- **CLEAN INJECTION UNDER THE KP SCHEDULE (2026-08-26, n=1), AND IT GRADES THE MODEL, NOT THE
  SCHEDULE.** `inject_split(+2500)` on a settled baseline (pre −16.2 µs, MAD 5.98):

        trim at the hold  +199 ppm
        wire step         −101.5 µs   (relaxing at +4 µs/min at age 185 s, so ~ −100 landing)
        model prediction  199 ppm x 3 s x 0.17 = 101 µs

    - The recorded model reproduces the step to 0.5 µs. So this is NOT evidence that the schedule
      shrank the repair's displacement: it is the same model with a SMALLER TRIM AT THE HOLD than
      the baseline pair (+187.3 / −256.9 µs, which imply ~625 ppm, i.e. the servo had reached the
      full 2500 µs error before the hold latched). Comparing steps without comparing the trim at
      the hold compares two different disturbances.
    - **STRUCTURAL FINDING, and it says the KP schedule CANNOT help here.** The displacement is
      generated DURING the 3 s hold, from the trim already commanded when the hold latched. The
      schedule re-arms KP *at the repair* — after the damage is done. It can only affect the
      post-repair settling, so no KP schedule of any shape reduces this term.
    - **What would: hold the INTEGRAL, not the whole trim.** `trim = p_term + integral`, and the
      p_term is the servo's response to an error the code is about to declare bogus, while the
      integral is the converged crystal cancellation the hold exists to preserve. Freezing the sum
      preserves the suspect term for 3 s at full size — 199 of the 199 ppm here is p_term, since a
      converged integral runs ~40–50 ppm. Holding I-only keeps the clock at the right rate (the
      stated reason for not zeroing) while removing exactly the term that displaces audio. Predicted
      step: ~0.17 x 50 ppm x 3 s ≈ 25 µs, i.e. a 4x cut, and it is a three-line change at the hold.

- **PADDING DOES NOT DISPLACE AUDIO — the prediction recorded here is REFUTED (2026-08-27).**
  `dbg_padded_frames` carried a standing prediction: "two devices differing by N frames of padding
  should sit N x (1e6/rate) us apart on a logic analyser". Tested by starving board b while a ran
  clean:

        dpad  a = +0 frames   b = +11551 frames (262 ms)
        predicted d(b-a) = +261927 us
        measured  d(b-a) =      +5.7 us

  262 ms of padding produced no displacement. It is fully absorbed by the starvation re-baseline
  and the `padding_debt_frames` repayment -- which is what that code is for, and it works. In
  steady state no padding accrues at all (three 2-min windows, dpad = 0 on both boards, offset
  wandering +-5 us, i.e. analyser noise). **Do not re-open padding as an offset mechanism.**
- **THE HANDOFF IS ALIGNED; THE RESIDUAL MAY BE THE INSTRUMENT.** `raw-sync` (device feedback, no
  probes, no prediction model) reads a-b = -0.8, -2.7, +2.6, -1.8, +2.2 us across five independent
  windows. The analyser reads 0-70 us over the same period. Everything below the handoff is
  therefore either a real per-board pipeline difference or the ANALYSER'S OWN ZERO ERROR -- and the
  5 ms calibration proved SCALE (1%), which a step test can do, but says nothing about a constant
  bias between two probes.
    - **The deciding experiment is a PROBE SWAP**: exchange the two logic-analyser channels between
      boards and re-measure. Sign flips -> the difference is real and in the boards. Sign holds ->
      it is instrumental, the boards are aligned to ~3 us, and the "static offsets" of this size
      reported today were measurement error.
    - Until that is done, treat sub-25 us analyser readings as unverified.

- **2026-08-27 MORNING: THREE THINGS VALIDATED ON HARDWARE, ONE STILL OPEN.**
    - **`r_push` re-basing: VALIDATED, 50 windows.** After a reconnect the RAW counter is invalid
      **100%** of the time (`bad_raw=96 (100.0%)` in 39 windows, 95 and 97 in others) while the
      per-epoch rebased value is **0%** invalid in every single window. That is the prediction made
      when the fix was committed, confirmed. Step 4's input is now sound where it was stable garbage
      a third of the time.
    - **The unmute anchor gate holds.** Ten re-locks logged `anchor` of 0, −1, −1, −1, 91, −1, −1,
      0, −1, −1 µs, all inside the 100 µs threshold — against the 158 µs that walked through the old
      2000 µs gate and planted 215 µs on the wire. Caveat: the eight forced reconnects did NOT mute
      (zero `Muting:` lines), so the gate was not stressed by them; the evidence is the earlier locks.
    - **THE WIRE IS AT +2.1 µs (MAD 3.0) after eight forced reconnects** — inside the <10 µs goal.
      For scale, yesterday's events planted 170 µs to 1.4 ms.
    - **The wedge is now rare, not fixed.** 4 wedges in 4 reconnects BEFORE the `emit_pcm_`
      reserve-space fix; 1 in 11 after. That is either a rarer surviving race, or the mutex probe
      added to hunt it perturbs the timing enough to mask it — a try_lock plus a barrier on every
      loop iteration is a classic way to hide a race, and the two cannot be told apart from here.
      **Do not record the wedge as fixed.**
    - **What the wedge is NOT** (each measured, so none of these get retried): the STOPPED handler
      discarding a pending START (`bits=0x002002`, bit 0 clear), heap exhaustion (175 KB free), the
      depth seqlock (bounded by construction, 4 tries), `is_connected()` (lock-free atomic), the
      logger (other tasks log throughout), and PCM stranded without records (`records=112` waiting).
      In the one captured wedge the player went `iters=+2565` then `iters=+0` — it spins, then
      blocks, with work available.

- **MATCHED A/B OF THE FORCED RE-ANCHOR, n=3 per arm: SUGGESTIVE, NOT SIGNIFICANT.** Six lone
  restarts of board b, flag alternated per trial so the arms interleave (conditions drift here —
  outages, wedges — and blocking the arms would confound the change with the afternoon). Landing
  LEVEL is the statistic, not the step: the step depends on where the board happened to sit, and
  the claim is "it ends up on alignment".

        OFF  |post| = 19.8, 91.3, 513.2 us   median  91.3
        ON   |post| = 23.3, 32.2, 145.1 us   median  32.2   (48.9 pooled with the earlier -1.8)

  Median 3x lower and the worst case 3.5x lower, **but the ranges overlap and the exact rank-sum
  gives p = 0.50** — at n=3 vs 3 the best achievable is 0.05, so this cannot reach significance
  however it comes out. It is evidence the feature does not HURT and a hint that it helps.
    - **The control arm does plant**, which the first two trials had cast doubt on: 513 µs on one
      clean OTA restart. So the disturbance is real but highly variable, which is exactly why n=3
      cannot resolve it.
    - **To settle it:** ~8 trials per arm (about 2.5 h unattended, the script is written and
      restores the flag), or a disturbance that plants reliably — an outage-driven reconnect via
      `inject_starvation`, remembering it puts the wire's fit floor at 162–1291 µs against 0.71 µs
      quiet, which may swamp the effect being measured.

- **FORCED RE-ANCHOR, FIRST GRADE ON A LONE RESTART (n=1): landed at −1.8 µs.**
  `reanchor_after_reconnect: true`, board b OTA'd alone so the restart was lone rather than
  simultaneous:

        pre +51.2 -> post -1.8 us     LANDING -53.0 us   (post MAD 5.21)
        16:45:57  Sync locked (median 78 us), unmuting
        16:46:07  Re-anchoring after re-lock: forcing one repair cycle (+2500 us bias)
        16:46:31  Accounting split repaired: accounted queue ran +2561 us

  Against **+145.7 µs** on the same test this morning without it, and 1.3–1.4 ms for today's
  outage-planted offsets. The device landed essentially ON alignment rather than tens or hundreds
  of µs away. n=1, so this establishes that the cycle fires on a lone restart and that the landing
  can be inside the ±50 µs residual — not that it always is. Repeat before believing the size.

- **CONFIRMED n=12, 2026-08-26: THE SPLIT REPAIR IS A CORRECTIVE MECHANISM. It removes ~2/3 of
  whatever standing offset the device carries, per firing.** This inverts the framing everything
  above was written under.

        pre_us    post_us   step_us     (injected +2500 us each, board b, quiet-gated)
        -1377.0   -509.8    +867.2      63% of the standing error removed
         -509.8    -34.7    +475.1      93%
          -34.7    -82.8     -48.1      already aligned: residual scatter only
          -82.9    -33.8     +49.0
          -33.8    +11.7     +45.5
          +11.8    -54.6     -66.4

    - **post = +0.33 x pre − 6.4.** That is the test that matters: 0 would mean the repair lands on
      a fixed point, +1 would mean it displaces from wherever it started and carries the old error
      forward. It is neither -- it removes a FRACTION (~2/3) per firing, which is why two repairs
      took the board from −1377 µs to −35 µs. Post levels: median −44.7 µs, MAD 24.5.
    - **DO NOT use the step-vs-pre regression** that the first run reported at −1.25. `step = post −
      pre`, so noise in `pre` forces that slope negative on its own. It is confounded by
      construction. Regress POST on PRE.
    - **The residual displacement, measured where it can be seen (device already aligned), is
      ±50 µs**, not the 222 µs on record. Those earlier points were taken near alignment too, so
      they are samples of this same scatter -- one clean point establishes existence, not size, and
      three of them with varying sign is what scatter looks like.
    - **Consequence: outage-planted offsets are recoverable, and the mechanism already exists.** The
      repair re-anchors the accounting to MEASURED latency, which is exactly what a pipeline restart
      leaves wrong. Twice today an outage planted ~1.3-1.4 ms; both times injected repairs pulled it
      back to tens of µs. The change this argues for is to force that re-anchor after a
      reconnect/pipeline restart rather than trusting the fresh anchor and waiting for a natural
      split to be detected. Not built -- this is the proposal, and it is the biggest lever measured.
    - **Environment note from the same run:** two of fourteen attempted points were eaten by natural
      supply outages (one reset the split window so the injection was never repaired; one wedged
      board a). Any campaign here needs a per-point timeout and a wall-clock validity gate -- the
      analyser emits nan while a board is silent, and windowing on the last VALID sample will
      happily grade pre-outage data as "quiet now".

- **NEW LEAD, 2026-08-26: A FORCED REPAIR UNDID AN OUTAGE-PLANTED OFFSET, EXACTLY.** Measured on
  board b, n=1, and it points the opposite way to everything recorded above about repairs.
    - 13:51:53 a natural supply outage (`RPRE` supply ratio 0.17, no OTA running) ran the full
      chain: 39 drops → `Stream 4823 ms late for 3 s: reconnecting` → disconnect → `speaker_mixer:
      Stopped` → reconnect with a fresh ring buffer and mixer restart → `Sync locked (median
      249 µs)`. The wire went from **+180 µs to −1307 µs and stayed there**.
    - Unmuted, the on-device median then walked **811 → 1090 → 755 → 440 → 304 → 224 → 181 µs**
      over ~30 s. That is the servo steering REAL AUDIO against the fresh anchor, audibly and
      permanently, while every on-device field reads like a healthy convergence. `Playout depth
      −5419 µs vs leader` at reconnect and `−11905 µs` 30 s later is the same thing seen from the
      other side. **The unmute gate let it out at 249 µs and the error then GREW to 1090 µs** —
      the mixer had only just restarted and its depth was still moving.
    - 13:54:36 `inject_split(+2500)`; repair fired 13:55:00. The wire stepped **+1329.8 µs** and
      landed at **+23.2 µs against the +24.6 µs baseline measured before the replug** — 1.4 µs from
      where it started, i.e. the repair UNDID the whole planted offset.
    - **The mechanism is NOT established and the recorded model does not fit.** The repair measured
      `+2562 us`, essentially just the injection, so the 1.3 ms was *not* in the split it saw. And
      step = trim × hold × 0.16–0.18 predicts ~80 µs here (trim was +148 ppm), not 1330. Landing
      within 1.4 µs of the old baseline is not what random displacement looks like, but n=1 and one
      clean point establishes existence, not size.
    - **Why it matters:** if a forced repair can recover an outage-planted offset, that is a lever
      on the single largest term measured — an outage plants ~1.3 ms where the steady-state floor is
      7 µs. The obvious follow-up is whether a repair provoked deliberately after a RECONNECT does
      this reliably, which is testable with the same hook.
    - **Do not generalise from this to natural repairs yet.** The recorded 23-per-session repairs
      plant 222 µs; this one recovered 1330 µs. Both cannot be the same mechanism, and the
      difference may be that this device was carrying a bad anchor from a pipeline restart while the
      earlier measurements were of repairs on a healthy one.

    - **THE WEDGE IS A SEPARATE, PRE-EXISTING DEFECT, and NOT caused by the discard cap.** I said it
      was and reverted on that basis. The revert was still right — the cap removed the recovery path
      — but the wedge fires without it: `speaker_mixer: Stopped` followed by permanent silence
      occurred **five times** on one board (10:14:10, 10:32:55, 10:36:15, 10:49:56, 11:06:02), and
      **three of those were after the cap was reverted at ~10:15**. Requires a replug to clear.
      Sequence, captured per-chunk:

          11:06:00  RSYNC[22..26] err 2873342 -> 3606874 us, ring=26, drops=22..26   runaway
          11:06:01  mixer DEPTH own=0 down_audio=0 total_audio=0                     all empty
          11:06:01  Stream ended / Disconnected from server
          11:06:02  speaker_mixer: Stopped        <- speaker stopped, `written` freezes here
          11:06:04  Connected / Stream started / State changed to PLAYING
                    ... no mixer "Starting", no further player-task line, ever

      So the reconnect succeeds and the media player reports PLAYING while the mixer task stays
      deallocated and the player task blocks writing into it. `dma_real=0` confirms the I2S channel
      is not running rather than merely starved.
      **A SECOND VARIANT, observed 12:19–12:22: the pipeline never STARTS after a boot.** Same end
      state, different entry. b's last `I2SDBG` was at 12:19:25 before the OTA reboot; it then
      booted, connected at 12:19:46, reported `State changed to PLAYING`, logged `Boot seems
      successful`, and from 12:20 produced **zero** lines from `snap_player`, `snap_net`,
      `speaker_task` or `mixer` — only `wifi_diag` from the main loop. So the main loop is alive
      while every audio task is dead from boot, and no `Stopped` appears anywhere in it. That
      points AWAY from the STOPPED handler below and toward task startup, or a lock taken before
      the tasks run. A fix aimed only at the stop/restart path would miss this variant entirely.
      Both need a replug.
      **Suspect, not confirmed:** `mixer_speaker.cpp:466-471` handles `MIXER_TASK_STATE_STOPPED` by
      calling `task_.deallocate()` and then `xEventGroupClearBits(event_group_,
      MIXER_TASK_ALL_BITS)` — clearing *all* bits, which would discard a pending
      `MIXER_TASK_COMMAND_START`. That would explain a start request being lost across a stop. What
      is not yet established is whether a START was issued and lost, or never issued because the
      player task was already blocked; the two are distinguishable and no "Starting" line appears
      either way. **Do not fix on this inference alone** — instrument which of the two it is first.
      Note the runaway itself is upstream of the wedge and is the empty-ring trigger below, so this
      is the second defect in the chain, not the first.
- **The measured cascade this all started from**, for whoever picks it up. On board A:
    - healthy at 09:46:53.410 (median −38 µs, ring **1724 ms**), `Hard resync 50 ms late` at .688
    - **pipeline completely dry** 0.7 s later: `queued=0 dma_real=0 written==completed inflight=0`
    - ring down to **26 ms**, **37 hard resyncs**, median 1.06 s, then
      `Pipeline drained (source starvation); re-baselining playout`, then **22 s muted**
    - 66 chunks discarded; the error grew 50 → 490 → 811 ms as it went
  Two readings remain open, and the failed attempt above is what makes the choice matter. Either the
  drain was self-inflicted (a spurious 50 ms reading, discards that could not close it, and the
  emptying pipeline degrading the prediction further because the playout feedback the pivot smooths
  stops arriving), or the client was genuinely falling behind and the drain plus re-lock was the
  correct recovery. The growing error is consistent with BOTH — self-inflicted or real — which is
  exactly why capping the response blindly made things worse. Settle it with the per-chunk trace
  described above before touching this path again.
- **Board a carries `split +22 µs`** where b sits at −1. Constant across every window measured, but
  not re-checked since the in-flight fix changed what `meas` contains.
- **Stale deadline on stream resumption.** With `keepalive_hold: never`, the first chunk after a long
  idle carries a deadline stale by roughly the idle duration. Self-heals in ~2.5 s and the magnitude
  rule mutes correctly, so it is bounded. Closing it needs the snapserver side.
- **Why did lateness spiral to 4.9 s** before the stale bailout fired? Probably self-inflicted by an
  accounting error since reverted, but unconfirmed. One natural occurrence was captured at 2.4 s and
  recovered through the starvation re-baseline.

## Housekeeping

- **Cut a tagged release.** The README tells people to pin `github://…@<tag>` and there are no
  tags. Would also anchor the MIT cutover that NOTICE.md dates but does not tie to a release.
- **Schema-to-README drift check.** An AST walk over every `cv.Optional` checks the key set,
  default values and the `unset` marker in ~40 lines. Do not generate the descriptions — seven
  options carry no source comment and the README's prose is better.
- **Residual serial-log gaps.** A ~200 s gap in an otherwise clean 10 s cadence, host buffering
  ruled out. `esp_wifi_statis_dump()` is the suspect; a long capture with
  `wifi_tools: diagnostics: dump_statistics` off is the experiment.
- **Extract the servo** only when a second consumer asks. It has one today, and extracting would
  turn a file-private invariant (the playout feedback, written from the speaker callback thread
  under `playout_mutex_`) into a cross-component guarantee. Would need a third component
  alongside `i2s_rate_lock`, since `clock_sync` is deliberately audio-free.

## Features

- **Runtime server retargeting** — `snapcast://host:port` URIs currently warn; needs thread-safe
  reconfiguration of the network task's target.
- **Crossfade on servo frame splices** — the last near-inaudible discontinuity class; 2–4 samples
  would zero it. Largely moot where `rate_lock` is enabled, since steady state stops splicing.
- **Encoded-FLAC buffering** — buffer compressed chunks and decode just-in-time in the player
  task, halving PSRAM needs. Only worth it if a PSRAM-less board variant matters.

## Method notes

Earned expensively; ignoring these cost hours.

- **Log the terms at one instant or not at all.** Sampling stages from their own tasks put them
  up to 0.4 s apart — ±17640 frames of noise against an 882-frame signal — and produced a column
  that looked like data.
- **Throttle diagnostics by time, never by iteration count.** A task loop with no guaranteed
  cadence will flood the log hard enough to stall an OTA.
- **`sd` is the wrong summary here.** Network events dominate it: a differential of MAD 12 µs
  reported sd 129 µs. Use medians and MAD.
- **A mean carries a rare fixed-size spike into the answer**; a median rejects it. That is what
  made the accounting split readable at 1 µs where the raw field showed 42 ms of artefact.
- **De-trend before measuring noise.** A field dominated by a systematic ramp reads the ramp, not
  the noise, and will not detect a change in the noise at all.
- **Watch the report's length.** Adding fields silently truncated `trim` and `tsf` off the end of
  the sync line, which reads as missing data rather than as a formatting problem.
- **On-device metrics have lied repeatedly** — the depth delta compares occupancy rather than
  timing, and the render phase cancels the very error it measures. The logic analyser
  (`scripts/i2s-skew.py`) is the only instrument that has not. The render phase's blindness is
  now MEASURED, not just suspected: with both boards following the same leader and their deltas
  differenced, it read −15.5 ±8.5 µs against +85 µs on the wire. Wrong sign, ~12σ out. It
  consumes `(pushed − played)`, so an accounting error and the audio offset it causes cancel —
  which is exactly the class of defect it was built to find. Do not use it for absolute offset.
- **The mean and the sd answer different questions, and picking the wrong one costs a day.** The
  pivot term was declared disproven off a differential rate whose MEAN was 0.018 ppm — true, and
  irrelevant, because the bias multiplies the instantaneous value and its sd was 1.64 ppm. Before
  concluding that a term cancels, check which moment of it the mechanism actually multiplies.
- **Before capping a correction, prove the correction is the problem — a cap on a load-bearing
  recovery path is worse than the thing it fixes.** Cost one flash and a minute of dead audio. The
  late hard-resync discard looked like an unbounded correction chasing a bad reading, so it was
  budgeted. It is also the ONLY mechanism that consumes a genuine multi-second backlog: capped, a
  real backlog ran away (666 → 2657 → 8076 ms) into a stale-bailout reconnect that left the speaker
  stopped indefinitely, where the uncapped version recovers in ~22 s. The tell that should have
  stopped the attempt: the *same observable* (a growing error during discards) is produced both by
  a spurious trigger the discards cannot fix AND by a real backlog they are fixing too slowly, so
  the evidence in hand never distinguished the two. **When one observable is consistent with both
  the defect and correct operation, no change to the response is justified yet — instrument to
  separate them first.** Here: does each discard reduce the next chunk's error ~1:1, or not?
- **The repair cascade remains the real instance of the unbounded-correction pattern:** a repair
  fired on an incomplete depth report, and its own subtraction created the opposite split that a
  second repair answered 14 s later. That one was confirmed by `r_mix` matching the split to 1 µs
  before anything was changed — which is the standard the hard-resync attempt above did not meet.
- **A guard on the median does not protect a path that reads the raw error.** The −52 ms split spike
  was filed as harmless because the median rejects it. It is harmless to the steering servo and was
  never harmless to the hard-resync path, which deliberately tests the instantaneous error so it can
  react to genuine steps. When a known artifact is dismissed as "rejected by the median", check
  which consumers actually go through that median.
- **Search this file for a rule that contradicts the change BEFORE flashing it.** Three changes in
  one session had flaws already documented here. The discard cap ignored that unbounded discarding
  is the recovery path. The stepped `inject_split` ignored that the servo reacts to steps and not to
  slow drift. Clearing the median window at the repair ignored *"sd is the wrong summary here"* — it
  made the median fall back to a single raw sample, and steering on one noisy sample at KP = 0.25
  commands 250 ppm of trim, which took the repair's displacement from 222 µs to 366 µs, worse than
  doing nothing. Each was argued from mechanism, each was plausible, and each was refuted by a note
  already in this repository. Reading for the contradiction is cheaper than a flash cycle.
- **Do not quote a result from one point.** A 42% improvement quoted off the first measurement
  became 33% at n=2; the "205 ms I2S stall", the "200 ms host log delay" and "the −52 ms spike is
  post-seed" were all n=1 and all wrong. One point with a clean floor establishes that an effect
  EXISTS; it does not size it.
- **Anything derived from the controller's output is inside the loop.** Three attempts to correct the
  pivot bias from the applied trim failed for this reason: a slow mean lags acquisition, an
  instantaneous value carries full loop gain, and the absolute value carries the crystal offset. A
  reference has to be measured from the plant, or the comparison restructured so it is not needed.
- **Never read a slope across a data gap.** A boot leaves ~25 s with no audio to correlate
  (`pcm_coef` 0, offset NaN), and joining the endpoints either side produced a convincing 40 ppm
  ramp that nothing in the audio did. `scripts/i2s-skew.py` now breaks the line, shades the gap and
  fits the slope over the longest contiguous segment only — but the habit generalises to every
  instrument here.
- **Credit timestamps carry ~300 µs of jitter.** Measured, not assumed: a 30 s two-endpoint baseline
  resolves only ±10 ppm. Anything derived from differencing two credit instants needs a fit over
  many of them.
- **A device reboots without saying so.** `safe_mode: Boot seems successful` only prints after a
  POWER CYCLE, so a clean OTA reboot never logs it; waiting for that line cost 25 minutes of a
  session. The reliable signal is `I2SDBG written=` resetting, since it is cumulative since boot.
  `scripts/flash.sh -p` can also print "OTA successful" for one board and never reach "All devices
  flashed successfully" — check for that line.
- **Two boards following the same leader can be differenced.** `delta(b) − delta(a)` from the
  `Render phase` line is the cheapest on-device stand-in for the analyser, and the medians need
  ~70 samples each before they mean anything (single samples carry ±100 µs).
