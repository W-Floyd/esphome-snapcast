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
  analyser. Differential TRIM already predicts differential rate at corr −0.778, so the information
  exists; integrating those snapshots explains only 13–19% because the report samples one value per
  3.3 s of a continuously moving quantity. That is aliasing, not physics. **The cheap first step is
  to log the time-MEAN applied trim per report window instead of the end-of-window snapshot.**
  Measuring it from the credit stream was tried and failed on jitter — see the pivot item.
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
- **`TRIM_KP_RUN` stays at 0.25 for now, but the case for 0.1 is stronger than it looks.** KP
  multiplies the differential median, and that input fell 6× across this work (sd 43.0 → 7.1 µs;
  differential trim sd 20.9 → 1.78 ppm). Straight linear scaling says 0.1 buys only 2–3 µs and costs
  3.6× on reboot recovery (42 s → 150 s) — but the loop gain above is `KP × 3.15`, so 0.1 takes it
  from 0.79 to 0.32 and removes the 1/(1−G) ≈ 4.8× amplification as well. Worth measuring rather
  than reasoning about, and worth doing BEFORE the pivot work since it is a one-line experiment.
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
  structural; rejected by the median, so harmless, but unexplained. Since the in-flight fix it reads
  −52245/−49343 — the same spike plus the 10 ms `inflight` those samples carry — and the terms are
  self-consistent (`sum == meas`, `r_mix == 0`), so it is not a conservation failure. Every
  occurrence has `xfer=50000` (its maximum, equal to `dma`), `inflight=10000`, `queued=70000`,
  `dma=50000`, `age≈33 ms`: a full transfer buffer, i.e. the sink briefly not accepting. Start from
  why `xfer` is railed at exactly one DMA buffer whenever this fires.
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
