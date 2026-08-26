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
        - First hardware reading: `mine +36.543 leader -7.835 delta +44.379 ppm`, arithmetic
          self-consistent, stable across samples. The mechanism works end to end.
        - **The prediction of ≈ −5.35 ppm FAILED, for a reason that was an assumption not a
          measurement:** −5.35 is the *a↔b* difference, and b's leader is not a. It is one of the
          unprobed boards, whose crystal reads −7.835 ppm. So nothing here contradicts the wire —
          but nothing here confirms it either, and it must not be written up as confirmation.
        - **VALIDATION STILL OWED.** It needs the two analyser-probed boards in a leader/follower
          relationship *with each other*. Leadership goes to the lowest MAC, so in a fleet of five
          that pair may never form: run only the two probed boards, or difference their raw
          published values rather than their deltas.
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
- **`TRIM_KP_RUN` is now 0.1, COMMITTED BUT UNMEASURED.** KP multiplies the differential median, and
  that input fell 6× across this work (sd 43.0 → 7.1 µs; differential trim sd 20.9 → 1.78 ppm).
  Straight linear scaling says 0.1 buys only 2–3 µs and costs 3.6× on reboot recovery
  (42 s → 150 s) — but the loop gain above is `KP × 3.15`, so 0.1 takes it from 0.79 to 0.32 and
  removes the 1/(1−G) ≈ 4.8× amplification as well.
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
          crossing. Seeing the drain start needs a rolling pre-trigger history, which is the next
          instrumentation step if this needs nailing down harder.
        - Also measured, and useful on its own: an ~100 ms excursion with a healthy ring resolves
          with no discards and no muting. The path only engages when the ring is gone.
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
