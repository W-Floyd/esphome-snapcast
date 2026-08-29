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

**DELAY-CONTROLLED SERVO IS LIVE (build 14, `29ca74f`, 2026-08-29) — see `HANDOFF.md` and
`PLAN-delay-controlled-servo.md`.** Wire went from sd 46.7 / p2p 243 µs (2026-08-28 morning) to
sd 8.9 / p2p 45 over 11.5 quiet minutes with a zero event census on both boards. Open items, by
audible impact:

- **Server-side delivery pauses** (all three boards' rings dip at the same instants; 120–297/hour
  on 2026-08-28 afternoon, 8–20/hour that night). Investigate the Pi host / process-stream loop /
  AP. Not fixable on the client.
- **Speaker-task feedback stalls** (60–1500 ms, fleet-wide). Blanked in firmware; root cause open.
- **Consensus steps around OTA/replug** (operator-induced, 60–100 ms common-mode); boot-time
  mapping flapping; **play-before-time-sync early-side wedge** (no bailout exists on the early side).
- **Slow differential feed-forward** on exchanged `err_tag` (the measured right signal: r 0.88,
  bias 2.5 µs vs the wire) to remove the ~+4 µs standing offset; dead-time compensation per board
  for the ±5 µs wander residual; tau 30 re-test once integrals are within ~1 ppm.
- **`block_n` 32 vs 64** on a quiet span (expected wash; the noise floor flattens at B=32).
- **HA `number`/`switch` entities** on the `servo_param` tunables; parse `DLLOOP` into the
  analyser CSV (`dl_err_a_us`, `dl_err_b_us`, overlay `errA−errB` on the wire skew).
- Persist API-set tunables across reboot? Currently deliberate NOT (safety); revisit when they are
  entities.

**30 Hz BEACONS: MEASURED, DOES NOT HURT, AND IT RETIRES THE RADIO-CONTENTION HYPOTHESIS
(2026-08-28, `f93eb9d`).** Three constants, since two are hard ceilings: `BEACON_INTERVAL_US`
1 s -> 33.3 ms, `SERVICE_MIN_INTERVAL_US` 200 ms -> 20 ms (service() returns early inside it, so
beacons would otherwise cap at 5 Hz and the experiment would silently not happen), and
`CONSENSUS_INTERVAL_US` 500 ms -> 100 ms.

VERIFIED LIVE BY MEASUREMENT, not by the flash succeeding: the observer's PHASEIN peer ages fell
from ~1000 ms to 46-111 ms. That implies an EFFECTIVE ~10-20 Hz rather than 30 -- worth knowing,
and probably service() call frequency (it runs per arriving message, ~38/s) or packet loss rather
than the constant.

    sd, matched instrument, correction off
    pre-KP,  1 Hz      6.326 us  (1.5 h window)
    post-KP, 1 Hz      1.694 us  (45 s window)
    post-KP, 30 Hz     2.319 us  (n=336, p2p 7.154)

**SO THE 15x FOLLOWER-BEACON REGRESSION WAS NOT RADIO CONTENTION.** That was the one hypothesis
never excluded when `broadcast_()`'s trailing `adopt_()` was identified as the cause (`27ad3de`).
30x the beacon traffic leaves sd in the same band, so contention is not a material term and the
`adopt_()` explanation stands.

WHAT IT DID NOT BUY, as predicted: nothing measurable for the timebase. The consensus averages
LINES carrying drift_ppm, so a stale peer line is extrapolated rather than stale-valued and 1 s of
staleness costs ~1 us, against a measured 137-920 us spread between the devices' own raw estimates.
The rate was never the limiting term.

STILL UNTESTED, and the reason to keep it: render-phase PAIRING. `PHASE_PAIR_WINDOW_US` is 300 ms
against a phase written once per sync report, and only ~25% of arrivals used to pair. 10-20x the
arrivals should transform that, which is the signal `render_align` needs -- but `render_align` is
blocked on ledger-independence, so there is nothing yet to consume it.

**A STRUCTURAL DIFFERENTIAL RAMP, worth recording as a candidate for part of the standing offset:**

    A: Offset ramp +3.20 ppm (tsf-local +43.02, map +39.82)
    B: Offset ramp -3.10 ppm (tsf-local +37.00, map +40.10)

`map` is COMMON (~40 ppm, the shared consensus drift, as it must be) while `tsf-local` differs by
6 ppm -- the crystal difference. So each device's offset filter sees a ramp differing by 6.3 ppm
BY CONSTRUCTION. Through the documented lag mechanism that is 6.3 ppm x tau 6.7 s ~= 42 us, against
an observed -48.6 us at the time.

DO NOT READ THAT AS THE ANSWER, tempting as the match is: a differential EWMA lag is a FIXED POINT,
so the offset would return to ~-42 us after every event. It does not -- it went
+89 -> +136 -> +85 -> -183 -> -77 -> -49 us across the morning, which is a random walk. The
event-planted mechanism still explains the standing offset better and the magnitude agreement is
most likely coincidence.

**BENCH CONDITION, and it currently blocks all precision work:** roughly one multi-board excursion
every 5 minutes since ~06:19, against a 1.5 h event-free run at ~05:00 that produced the baseline.
The affected subset VARIES (09:47 A+B, 09:57 A only, 10:07 A+observer, B's stalls at 08:00/09:05/
09:47), so it is not one weak board, and it coincides with Spotify-stream clients being affected in
parallel -- i.e. server or network side, not firmware. Consequence: `TRIM_KP_RUN` 0.125 is still
UNGRADED after three 5-minute windows aborted on events, and its known cost is real (per-board
median 1-217 us against +-30-60 at 0.25). Do not stack further parameter changes until the event
rate returns to the overnight level; nothing can be resolved at this one.

**MEASURED: THE RATCHET IS GONE (2026-08-28). 8 forced starvations, post-fix:**

    plants: +13.4  -105.0  -6.0  +89.6  -44.8  +75.3  -45.5  +62.6   us
    median +3.69   mean +4.96 (+-24 SE)   |median| 54.07   POSITIVE 4/8

Against the pre-fix record of **10/10 positive at +15.7 us per resync** (p = 0.001 one-sided,
accumulating +157.3 us over 10 resyncs). The sign split is the test and it is unambiguous: signs are
balanced, so a resync-planted offset now RANDOM-WALKS (growing as sqrt(N)) where it used to RATCHET
(growing as N). Over a session that is the difference between unbounded drift and a bounded wander.

CAVEATS, both real. The SIGN evidence is strong (4/8 against a prior 10/10); the MEAN is +4.96 +- 24
us, consistent with zero but at n=8 unable to exclude the old +15.7 either. Two windows were noisy
-- window 1 at `sd 100.57 corr_min 0.8133` is genuinely poor, so its +13.4 is the weakest point in
the set. And the SPREAD is unchanged (|median| 54 us), exactly as predicted: the fix removes the
bias, not the variance, because the consumer still reads the snapshot ~20 ms stale and samples the
sawtooth at random phase.

**FIXED IN THE FORK (`56601e6bc6`), VERIFIED LIVE.**
`held=` now reports **49600** where it was invariably **50000** across all 52 prior seeds, so the
mechanism is confirmed and the fix does what it claims. The remaining term is STALENESS, and the
same seed line quantifies it: `SEEDANCHOR ... age=20483` -- the consumer reads the snapshot 20.5 ms
old, which is TWO descriptor periods, so it samples the sawtooth at effectively random phase.

    before   span a constant 50000  -> systematic +5 ms bias, one-sided (hence 3.7-13 ms, hence a ratchet)
    after    span = instantaneous remaining, read at random phase -> ZERO-MEAN +-5 ms

So the fix converts a systematic bias into zero-mean noise. That kills the ratchet and does NOT
remove the spread. n=1 seed so far; per this file's own rule, one point establishes the effect
exists and does not size it.

IT ALSO EXPLAINS "SNAPSHOT AGE DOES NOT IMPLY DRAINAGE" PROPERLY, which was left as a puzzle above:
the span is a SAWTOOTH -- it drains within a descriptor and is refilled at each boundary -- so over
20 ms of staleness it is back near full, and subtracting elapsed time over-corrects. All three
ageing attempts failed for that reason, not because the split between draining and non-draining
terms was wrong.

FOLLOW-UP WORTH CONSIDERING, a contract question for `AudioDepth` rather than a bug: for a reader
this stale, publishing the sawtooth MEAN (`capacity - buffer/2`, i.e. 45000) gives zero bias AND
zero variance from this term, where the instantaneous value gives zero bias and +-5 ms.

**ROOT CAUSE OF THE OFFSET PLANTING, CONFIRMED QUANTITATIVELY (2026-08-28). The render latency
reports DMA CAPACITY where it must report REMAINING TIME, and the error is bounded by one
descriptor = 10 ms.**

Two independent confirmations of the same number:

    from the logs    every dry seed reports latency=50000 EXACTLY, zero variance
                     (13 such seeds on A, 4 on B; every other value is 50000 + queued)
    from the source  DMA_BUFFER_DURATION_MS 10 x DMA_BUFFERS_COUNT 5 = 50 ms span,
                     one descriptor = 10 ms

    predicted planted offset   uniform (0, 10] ms, mean 5 ms
    observed planted offsets   3.7-13 ms

`dma_resident_bytes_` is written exactly twice -- set to `total_dma_bytes` at task start, zeroed on
stop -- and NEVER decremented as descriptors complete. So the published `dma_span_us` is the full
50 ms whatever the true remaining wait is, and the seed (`pushed = played + latency`) converts the
over-statement directly into a planted accounting offset.

A STALE COMMENT THAT PROBABLY CAUSED THIS TO BE MISPRICED: the note at the seed says "a fully dry
pipeline anchors at one DMA buffer". It does not -- it anchors at all five (50000 us, never 10000),
which the seed log shows unambiguously. Priced at one buffer the error looks like 10 ms of ceiling;
priced correctly it is 10 ms of FREE PHASE on a 50 ms floor.

THE FIX: derive the span from remaining rather than capacity. The bookkeeping already exists in the
same loop -- `dbg_written_real`/`dbg_completed_real` maintain `dma_real_frames` as their difference,
which IS live -- so the change is to stop reading the one-time capacity store. Upstream-shaped;
belongs with the buffered-audio API work at the top of this file.

Original framing kept below, because the reasoning that got here is the reusable part.

**LEAD ON THE OFFSET PLANTING (2026-08-28), code-supported. The first hypothesis on this thread
that is not a subtraction.**

`I2SAudioSpeakerBase::dma_resident_bytes_` (esphome fork, i2s_audio speaker) is written exactly
twice: set to `total_dma_bytes` once at task start, and zeroed on stop. **It is never decremented as
descriptors complete.** So `dma_span_us` in the published render latency is a CONSTANT -- the full
DMA span -- while the true wait for a frame handed over now is that span MINUS however much of the
currently-playing descriptor has already been clocked out.

    reported latency = queued_us + FULL dma span        (constant)
    true latency     = queued_us + span - (already-played part of current descriptor)
    over-statement   = bounded by ONE DESCRIPTOR
    observed plants  = 3.7-13 ms   (whole span measured at 50 ms via I2SDBG dma_real=2205)

WHY THIS FITS WHERE THE OTHERS DID NOT:

  * MAGNITUDE. 3.7-13 ms is one descriptor's worth of a 50 ms span.
  * IT IS DIFFERENTIAL, which is the property that matters and which nothing else explained. Each
    board's phase within the descriptor cycle at seed time is independent, so two identical boards
    plant DIFFERENT offsets. An absolute error in the latency report would be common-mode across
    the group and therefore inaudible -- see the reframing below.
  * IT EXPLAINS WHY ALL THREE AGEING ATTEMPTS FAILED. The fault is not snapshot staleness; it is
    that one term of the snapshot is a CONSTANT where it should be a live remaining-time. No
    subtraction of elapsed time can correct a term that never moves. That also retires the
    "SNAPSHOT AGE DOES NOT IMPLY DRAINAGE" puzzle: the span genuinely does not drain, and the
    published value is genuinely wrong anyway, for an unrelated reason.
  * The fork agrees something is unexplained there: the diagnostic block immediately above carries
    "TEMPORARY DIAGNOSTIC ... Remove once the offset is explained."

THE REFRAMING, which is worth more than the hypothesis: every attempt so far has asked "why is the
latency report wrong?", i.e. hunted an ABSOLUTE error. An absolute error is common-mode and
inaudible. The quantity that matters is the DIFFERENCE between two identical boards, so the only
admissible mechanisms are ones with a per-device degree of freedom. Descriptor phase is one;
snapshot staleness, padding and drain semantics are not, which is why they all failed.

CANDIDATE FIX: decrement `dma_resident_bytes_` as descriptors complete, or derive the span from the
descriptor read position, so it reports REMAINING TIME rather than CAPACITY. Upstream-shaped, and it
belongs with the buffered-audio API work at the top of this file.

TEST BEFORE FIXING: `SEEDANCHOR` already logs `latency=` and `age=`. Add the sub-descriptor position
at seed time and check whether the planted offset correlates with it. If it does, this is it; if the
planted offset is instead constant per board, the phase is not free and this is wrong too.

ALREADY DEAD on this thread, so it is not re-proposed: snapshot ageing (three variants, all
measured worse), refusing a seed onto a non-drained pipeline (measured far worse), and DMA silence
padding (an in-code note at PADDISP: "pad= is a diagnostic, not a displacement term ... Whatever
plants the hundreds-of-us offsets, it is not this", because the repayment path removes it).

ESPHOME-SIDE READ, COMPLETED. One real bug, two candidates exonerated with numbers:

    render_latency reporting CAPACITY not remaining   REAL BUG, fixed 56601e6bc6 (+5 ms at seeds)
    played_ts ISR stamps                              CLEAN: MAD 1.00 us over n>=13695 steps,
                                                      esp_timer_get_time() is the first statement
                                                      of an IRAM_ATTR ISR, zero-mean, and the
                                                      consumer median-filters it
    queued_us quantisation                            CLEAN: <1 us, since the ring is written in
                                                      whole frames so bytes_to_frames loses nothing
                                                      and only frames_to_microseconds rounds

So the RESIDUAL (tens of us, distinct from the ms-scale seed error) is NOT an esphome measurement
problem. The seed error is milliseconds and the residual is tens of us, so something removes most of
it -- the accounting cross-check whose output is `RECON drift=` (22 us in a sampled line, against an
88 us standing offset). The residual is that repair loop's FLOOR, and it is local code, not the
fork. That is the next place to dig.

Also worth noting the played_ts measurement method, since it is reusable: completion events are
clocked by I2S and therefore exactly one buffer period apart, so the deviation of consecutive
`played_ts` from that period IS the ISR-entry jitter, measurable from the existing RAW lines with no
instrumentation. Caveat on the tail: RAW is ~38/s against callbacks at 100/s, so filtering to
single-buffer steps takes a biased subsample -- MAD is trustworthy, the tail shape is not.

WHY THIS IS THE ONE LEVER WORTH PULLING: three separate threads terminate here.
  1. `TRIM_KP_RUN` cannot be lowered to kill the ~77 ppm common-mode rate oscillation (12.5 us p2p
     on the wire) until re-baselines stop planting offsets -- the note at that constant says so
     explicitly, and 0.1 was tried and reverted because a slower null lets the differential-rate
     integral run longer and LANDS A BIGGER offset (-155 us against a +-130 us band).
  2. `render_align` cannot correct what a re-baseline plants, because `render_phase` is
     structurally blind to a repaired ledger bias (measured: ratio 0.003 on a ledger perturbation,
     1.0000 on an external one).
  3. The resync ratchet (+15.7 us per resync before stream scoping, +1.6 after) is the same
     planting.
Fix the planting and all three improve; fix any of the three directly and none of them do.

**LEADERLESS TIMEBASE IS IMPLEMENTED AND UNMEASURED (2026-08-28).** `PLAN-leaderless.md`, all
five steps: every device beacons its own raw server↔TSF line, everyone adopts the robustly
weighted MEAN of all of them (its own included), nobody ever publishes the consensus, the
adopted mapping is slewed rather than stepped, and `Role`/takeover/`last_rx_us_`/
`LEAD_COOLDOWN_US`/`always_healthy`/`set_playout_healthy` are deleted. `render_delta_us()`
(leader-relative) is gone; depth and crystal deltas are against the peer mean.

**RESOLVED (2026-08-28): removing the adoption slew recovered the sd. Final numbers first.**

    build                          devices   median      sd     MAD    notes
    leader-based (old baseline)       2       +4.5 us    3.6      -    six handovers / 17 min
    leaderless, SLEWED adoption       3       +0.47     8.06    4.03   0 re-anchors
    leaderless, SLEWED adoption       3       -1.79     9.50    5.48   reproduces
    leaderless, SLEWED adoption       2      -25.39     9.72    6.19   group size NOT the cause
    leaderless, DETERMINISTIC         3       -3.75     4.32    2.19   0 steps reported

sd 9.50 -> 4.32 by deleting the slew: within 20% of the leader baseline, with THREE devices where
the baseline had two, and with zero churn. See `75c01f4` and the note at `update_consensus_()`.
The 20% that remains is unattributed -- candidates are the third device, the beacon rate (every
device now publishes every second where only the leader did), and transient set-disagreement from
lost beacons.

The history below is kept because the wrong turn is the instructive part: the slew was step 4 of
the plan, the plan predicted "if sd worsens, step 3 or 4 is wrong", and it took a two-device
control to rule out the group-size explanation before the slew became the obvious suspect.

FIRST MEASUREMENT (2026-08-28, one clean 4-minute window, correction OFF, preflight verified,
n=10726). **Failed the sd bar; the churn goal met outright.**

    era                              median      sd     MAD    notes
    best sustained, leader-based      +4.5 µs    3.6      -     two-device group
    leader-based, all fixes in        +3.0 µs   16.9      -     settling
    LEADERLESS, three-device group    +0.47 µs   8.06    4.03   0 re-anchors all night
    LEADERLESS, second window         -1.79 µs   9.50    5.48   reproduces (n=11139)

    sd within any 30 s slice: 3.49 - 10.48, typically 3.5-6  (i.e. near baseline)

The 4-minute sd is inflated by the MEDIAN ITSELF WANDERING +-4.4 µs on a ~30 s timescale, not by
broadband noise -- hence MAD 4.03 against sd 8.06. Under a leader that wander was common-mode
(one publisher, one mapping) and cancelled; under consensus each device computes and slews toward
its own mean, so the two adopted mappings can differ slightly and that difference is free to move.

**NOT CONFOUNDED BY GROUP SIZE -- TESTED.** The observer was taken out of the TSF consensus and the
pair re-measured over a matched 4-minute window:

    2-device, leader-based (baseline)   sd 3.6
    3-device, leaderless               sd 8.06, 9.50
    2-device, leaderless               sd 9.72   MAD 6.19   n=5748   median -25.39 us

Group size is not the explanation. **The consensus genuinely costs ~2.7x on sd**, and by the plan's
own judging rule that means "step 3 or 4 is wrong".

IT IS STEP 4, THE ADOPTION SLEW, AND THE MECHANISM IS STRUCTURAL RATHER THAN A BUG. With a leader
every device computed deadlines from ONE IDENTICAL PUBLISHED LINE, so the mapping's own error was
EXACTLY common-mode and cancelled perfectly between devices -- which is why the leader design held
3.6 us while each device's Kalman wandered +-100-300 us. Under consensus each device computes its
own mean and SLEWS TOWARD IT ALONG ITS OWN PATH: same inputs, different history, so the adopted
mappings are only approximately equal. The live spread between two devices' raw estimates is
40-934 us, and whatever fraction of that fails to cancel lands directly on the wire.

So leaderless traded EXACT COMMON-MODE for freedom from churn. The churn win is real and complete
(zero re-anchors across the whole night); the cost is that the mapping is no longer bit-identical.

THE FIX INVERTS STEP 4 RATHER THAN TUNING IT. A common-mode STEP is harmless -- the plan itself
argues group-wide drift is inaudible -- while a differential SLEW is not. So make the adopted
mapping a PURE DETERMINISTIC FUNCTION of the live estimate set, with no per-device slew history:
devices holding the same set then agree exactly, and a membership change steps everyone
simultaneously and identically, which cancels. The slew was protecting against a step, but the step
was never the problem; the PATH-DEPENDENCE was. Open question for that design: devices do not hold
identical sets at the same instant (a beacon lost by one and not the other), so "same set" needs
either a shared epoch to evaluate at or tolerance of brief disagreement.

NEXT-BEST SUSPECT if the above does not recover the sd: the beacon rate. Every device now publishes
a mapping every second where only the leader used to, and the 15x follower-beacon regression was
attributed to `broadcast_()`'s trailing `adopt_()` -- which may have had a real radio-time
component as well. Halving the rate is the cheap test.

Corroboration from the same window: the median reads -25.39 us, still carrying the residual the
forced resyncs planted (-2.92 before, -27.95 after, -25.39 here). The ratchet is real and nothing
removes it.

Corroboration that the mean position itself is sound: `raw-sync.py` over the same window puts
A - B at **-2.2 ± 2.1 µs, not significant**, agreeing with the wire's +0.47 µs median.

If sd worsens, the two suspects are in that order: the consensus being fed back (it is not — our
beacon reads `pub_*`, never `map_*`; check that first anyway), and the beacon rate, since every
device now sends a mapping every second where only the leader used to. See the retired
follower-beacon diagnosis below, which explains why that is expected to be safe now.

Entity/log surface changed with it: text sensor `tsf_role` → `tsf_state`, publishing
`Consensus`/`Solo`/`Inactive`; the sync report prints `tsf=consensus(n2, 1.0s, depth … render …)`;
new `Consensus over N estimate(s): spread …` every 10 s and `Timebase re-anchor: …` on a snap.

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

**THE RATCHET WAS THE CROSS-STREAM COMPARISON, AND STREAM SCOPING REMOVED IT.** Same campaign
re-run 2026-08-27 after flashing stream-scoped TSF leadership, with the pair verified in-contract
(both reporting `TSF stream scope: 'MLS44' (a6ed0c3d)`, B elected leader between them, and both
logging `Ignoring TSF packets for a different stream`):

    PRE  (leader on another stream)  n=10  mean +15.7  sd 16.1   positives 10/10  (p = 0.002)
    PRE, excluding two outliers      n= 8  mean  +8.5  sd  3.3   positives  8/8
    POST (stream-scoped)             n= 7  mean  +1.6  sd  6.0   positives  5/7   (chance)

    accumulated: PRE +157.3 µs over 10 resyncs;  POST +10.9 µs over 7

So the residual fell 8.5 → 1.6 µs, the sign bias vanished, and the standing error stopped
climbing (it held at ~+225 µs across the post-flash run against −28 → +192 before). The
"systematic ratchet" recorded above was an artefact of comparing render phases across two
snapcast streams — self-inflicted by splitting the probed pair onto their own stream — not a
firmware bias. That is worth knowing precisely because it was significant at p = 0.002 and
completely wrong.

CONSEQUENCE: `render_delta` is now a good correction signal in-contract (residual 1.6 ± 6.0 µs
against displacements of 20–235 µs), which is what `render_align_max` needs to be worth
enabling. Two of nine cycles were contaminated and excluded — one by a B reseed, one by the A
stall below.

**BOARD A STALLED MID-CAMPAIGN, CAUGHT LIVE.** After a mute at 19:04:00 the player task stopped
iterating entirely while work piled up:

    ring=0 -> 78 KB -> 520 KB     records=0 -> 16 -> 112     iters=+73334 -> +50 -> +0
    phase=idle(record queue), output_active=1, heap free 176 KB (not exhaustion)

Records available and the task not iterating is a lost wakeup or a blocked mutex, not
starvation. It recovered only on restart. This is the known wedge (~1-in-11 on reconnects) that
`PLAYER STALLED` was added to catch, and this is the fullest capture of it so far. NOT excluded:
the boards had just been flashed with the TSF change, so a contribution from it is unproven
either way — the stalled task is the player, and the change touches the net task and the
deadline.

**~~THE ACCOUNTING ERROR RATCHETS: ~8.5 µs PER RESYNC, ALWAYS THE SAME DIRECTION.~~ (superseded
by the above — kept because it was significant at p=0.002 and still wrong.)** n=10 forced
resyncs, 2026-08-27, MLS stimulus:

    d_err: +2.9 +9.7 +7.4 +31.0 +58.6 +14.5 +7.4 +7.2 +12.0 +6.6   -- ALL POSITIVE
    excluding two outliers: +8.5 ± 3.3 µs per resync
    standing error over the campaign: −28.1 → +191.8 µs

Ten of ten with the same sign is p ≈ 0.002 by chance, so this is a systematic bias the resync
path injects, not noise the devices fail to track. It never self-corrects, so it accumulates
without bound: ~85 µs per 10 resyncs, ~850 µs per 100.

That is the defect, and it is a different shape from everything chased before it. The question
is no longer "what plants a random displacement" — the displacement IS tracked by the render
delta, with a residual that is constant in µs rather than proportional. The question is what
adds a fixed positive quantity to the accounting on every resync.

CAVEAT: measured with the leader on a different snapcast stream, i.e. outside render_delta's own
contract. Stream-scoped leadership (committed) enforces it; re-measure before trusting the
magnitude. The prediction to test is that the residual falls below 9 µs once the group shares a
stream — and if the ratchet survives that, it is real.

**RUNBOOK — the accounting error is the target, and it is measurable.** The devices' render
phase is derived from `(pushed - played)`, so it inherits the very error it would need to
correct (the code says so at that site). The defect is therefore:

    accounting_error = measured_skew (logic analyser)  -  (render_phase_B - render_phase_A)

Measured 2026-08-27 over 54318 paired rows: truth −27.2 µs (MAD 1.7), belief −1.0 µs (MAD 15.0),
error −25.9 µs. The devices believe they are aligned while 27 µs apart, and reality is NINE
TIMES more stable than their estimate of it. Everything else chased this month has been a proxy
for this number.

To continue:

    python3 scripts/accounting-error.py a.log b.log test.csv --tail 600000 --out acct-err.csv
    python3 scripts/skew-regress.py --minutes 240        # differenced corrs + placebo control

Regress the error against the published accounting terms (`pushed`, `played`, `clamped`, `pad`,
`queued`, `xfer`, `own`). A term that tracks it is the bug; if none do, the error is in
something the firmware does not publish, which says what to instrument next. Exclude rows where
either board has just reseeded — the absolute phase pair is meaningless there and those
excursions are ~500 ms, not µs (`|believed| < 2000` dropped 4%).

Bench state: both boards in their own snapserver group `4eb19e5e` on the `MLS44` stream (an
MLS stimulus, added at runtime via `Stream.AddStream`, file source, loops); other speakers
untouched on Spotify; `scratchpad/group-orig.json` restores the original grouping. MLS matters:
on music the analyser cannot resolve this at all. `timing_diagnostics: true` is still set in
`example/snapclient-base.yaml`.

**FOLLOWER BEACONING FIXED; THE REMAINING BLOCKER IS THAT `render_group_delta_us` IS WRONG.**
Sending follower phase reports through `broadcast_()` was the destabiliser — it does a 45-81 µs
TSF sandwich read, mutates leader-only rate state, and unicasts to every peer. Replaced with
`broadcast_phase_only_()` (no TSF read, no rate state, multicast only, quarter rate), `a0f624c`:

    control, no follower beacons        sd  5.24 µs
    follower beacons via broadcast_()   sd 81.6 µs
    follower beacons, phase-only        sd  4.62 µs   <- fixed

With that fixed, the correction was measured on a stable base for the first time and is NOT
harmful — settled, it is BETTER than off:

    correction OFF, phase-only beacons  sd 4.62 µs
    correction ON,  settled             sd 1.84 µs   (median +107)

But it does NOT null the offset: it crawled 126 → 105 µs at 0.08 µs/s and stalled. The reason
is that THE SIGNAL IS WRONG. `render_group_delta_us` compared against the analyser:

    A group   skew B-A   predicted (-2 x group)
      +48      +109.5      -96      <- sign INVERTED
      +49      +106.3      -98
      +20      +113.3      -40
     -266      +106.2     +532      <- jumped 286 µs in 10 s while true skew moved 20

Regressed properly against the analyser rather than read off a few samples — the signal is not
merely sign-inverted, it is largely DISCONNECTED from reality:

    board A: n=42  r=-0.153  slope=-0.10      expected slope -2.0
    board B: n=29  r=-0.688  slope=-0.09      (group should equal -skew/2)

The slope is ~20x too small and on A the correlation is near zero. So the loop was being fed
mostly noise, which is why it crawled and stalled, and why it appeared to converge while the
arithmetic said it should not.

WHERE TO START: instrument the inputs, not the output — log this device's own `render_phase_us`,
each peer phase in the table with its age, and the computed median, on one line. The candidates
are a stale or re-seeded peer entry surviving in the table (`record_peer_phase_` accepts entries
up to PHASE_STALE_US = 15 s old and cannot see a peer counter reset), and the median being taken
over ABSOLUTE phases whose own doc warns "absolute value is meaningless... only differences
between devices mean anything". Do not touch the controller: its gain, deadband and rate were
tuned against measured loop dynamics and it is provably benign (sd 1.84 vs 4.62 with it off).

**~~CORRECTION TO THE BELOW: THE DESTABILISER IS FOLLOWER BEACONING, NOT THE CORRECTION.~~**
`render_align_max: 0ms` gates only the CORRECTION; follower beaconing runs regardless. So the
"OFF" arm below was not the same firmware as the original baseline, and every number in that
table is confounded. Controlled properly by flashing `3c4356c` (stream scoping, NO follower
beacons):

    21:22  no follower beacons, no correction    sd  5.4 µs   <- original baseline
    22:33  follower beacons,    no correction    sd 81.6 µs   <- what was called "OFF"
    22:49  no follower beacons, no correction    sd  5.24 µs  <- control, reproduces baseline

The control reproduces the baseline exactly, so publishing follower beacons costs a factor of
~15 in skew stability all by itself. **The correction has therefore never been evaluated** — it
was measured only ever on top of a destabilised system, and the conclusion below that it is
"10x worse than off" does not follow from the data.

**MECHANISM FOUND (2026-08-28), and it was not radio time.** `broadcast_()` ended with
`adopt_()` — "the leader plays from its own published mapping so everyone quantizes alike",
correct for a leader. A FOLLOWER running the same function therefore overwrote the shared
mapping with its own private Kalman line every two seconds, so the group stopped sharing a
timebase at all. That is a far better explanation of a 15× degradation than a few hundred µs of
transmit, and it retires the "contention or transmit perturbation" hypothesis above. Verified
against `352a9f7`: `service()` called `broadcast_()` on the follower path, and `broadcast_()`
line 863 adopted what it had just sent.

CONSEQUENCE FOR THE LEADERLESS BUILD: every device now beacons a mapping every second, which is
the thing that measured 15× worse — but nothing adopts from `broadcast_()` any more. Adoption
happens in exactly one place, `update_consensus_()`, from the average. If skew stability
regresses anyway, that is the first place to look and the beacon rate is the first thing to
halve.

**~~`render_align` DOES NOT WORK YET — 10x WORSE THAN OFF~~ (CONFOUNDED — see above).** Left
disabled (`render_align_max: 0ms`).**
Measured 2026-08-27, same analyser, four configurations:

    era                              sd(d fs_a)  sd(d fs_b)   skew sd
    correction OFF (baseline)           0.0333      0.0313       5.4 µs
    ON, leader-referenced, gain 0.25    1.8041      0.0740    1150.1
    ON, group-median,      gain 0.25    0.0789      0.0651     117.6
    ON, group-median, gain 0.05 + 1/3   0.1003      0.0428      58.0

Three real bugs were found and fixed, each with a measured improvement (1150 -> 118 -> 58), and
all are committed — but the result is still an order of magnitude worse than not correcting at
all, so the feature stays off:

1. **Leader-referenced correction.** Leadership changed SIX TIMES IN SEVENTEEN MINUTES on a
   two-device group (it requires a healthy device; every resync disqualifies the incumbent), so
   the reference moved constantly. Fixed by correcting toward the group median. `352a9f7`
2. **Followers published no phase**, so there was no group to take a median of. Adding follower
   beacons WEDGED BOTH BOARDS first: they fell through into the election block and refreshed
   `last_rx_us_` — "last valid packet from another LEADER", which takeover uses to detect
   silence — so no device could ever take over, the group lost its leader and mapping, and both
   players stalled with only `wifi_diag` logging. Fixed by handling follower beacons BEFORE
   election. `352a9f7`
3. **Gain set by feel.** Sustained limit cycle, period 26.2 s (7.8 reports), 207 µs p2p. Period
   ≈ 4× loop delay gives ~6.5 s ≈ 2 reports of delay, so gain 0.25 was at or above ultimate.
   Retuned to 0.05 and one correction per three reports. `ac08968`

**THE OPEN THREAD:** the remaining instability is on the LEADER, which does not correct at all
(`sd(d fs_a)` 0.10 against a 0.033 baseline, while the correcting follower sits at 0.043). If a
follower's correction destabilises the leader, the coupling is not through the code path built
here, and it is not yet understood. That is where to start.

**THE ON-DEVICE DIFFERENTIAL OFFSET LOOKS INVERTED — r = -0.98 AGAINST THE WIRE.** Measured
2026-08-28 on the leaderless build, correction OFF, one clean 4-minute window (n=10726 wire
samples, preflight verified, MLS resolving at peak 1.0000 / runner-up 0.027 throughout).

Method, because the result depends entirely on it. The on-device series is NOT `render_group_delta`
and NOT the render-phase log line: both update once per sync report (~3.4 s), whose per-sample
noise (sd 13.3 us) exceeds the +-4 us signal, and pairing two independent 3.4 s series inside the
300 ms window yields 5 usable points in four minutes. Instead the per-chunk `RAW` lines are used
directly, ~38/s, and paired on **`s_ts`** -- the server's timestamp for a chunk, the same number on
every device for the same audio, so there is no sampling-instant problem to bound. Both series are
then re-bucketed on ABSOLUTE WALL TIME (the first attempt bucketed each from its own first sample,
leaving the grids ~10 s apart, a third of a bucket).

    30 s bucket medians, both B - A, us
    wire       -2.72  -3.33  +1.96  +7.25  +3.87  +6.11  +0.31  +0.10  -5.55
    on-device  +5.50  +7.50  -0.50  -3.50  -0.50  -2.50  +2.50  +2.50  +7.50

    r = -0.980  (n=9)    wire sd 4.35 us   on-device sd 4.13 us   SE ~1 us/bucket
    r = -0.975 / -0.989 / -0.967 / -0.973 / -0.945  at 10 / 15 / 20 / 45 / 60 s buckets

Same amplitude, opposite sign. THE ARITHMETIC SAYS IT SHOULD BE +1: if B renders a server frame
late by d, its `(pushed - played)` is larger by d*rate, so `server_time = s_ts - (pushed-played)/rate`
is smaller by d, and `phase = shared_tsf - server_time` is larger by d. So either that derivation
or one of the instruments is wrong. No mechanism is proposed here.

RULED OUT — a/b channel swap, which fits r = -1 at equal amplitude and was the leading suspect.
The analyser's own built-in check (`--replot --annotate`, documented at its docstring: "a swap
shows up as slope +1 instead of -1") reads **slope -0.622, corr -0.791**, and its constant offset
**-5.245 ppm** matches what the boards publish about themselves (A `mine +44.453`, B `mine +39.346`
-> B-A = -5.1 ppm) in both sign and magnitude. A swap would have put the wire at +5.2 against the
devices' -5.1. Skew and rate come from the same channel assignment, so the skew sign is right too.
The agreeing MEANS are NOT evidence either way (+0.89 wire vs +2.05 on-device, against a
bucket-median sd of 4.3) and were briefly cited as such -- they are not.

THIS IS THE SAME THREAD as "the group delta is disconnected from reality, not merely sign-inverted"
(`6f468bf`) and the regression that read slope -0.10 where the arithmetic demanded -2.0. The same
`--replot` run now reads that check as:

    trim mean (offset integral, bw - aw): expected slope -1.0
      span 258.7 s  n=255  corr +0.068  slope +0.001  resid 7.91 us  expl 0%

0% explained, against a RATE reference on the same run that works (corr -0.791, constant matching
the published crystal delta to 0.1 ppm). An inverted offset measurement is a coherent account of
why: the correlation cancels in the integral. Rate reference good, offset reference carrying no
information, and a candidate reason for it.

CONSEQUENCE: **do not enable `render_align`** until this is settled. It steers on `-group_delta`,
so an inverted input drives the pair apart at double rate -- which is also what the 1150 / 118 / 58
table above measured, and would mean those numbers were never about gain.

**REPLICATED** on an independent window (01:33:40+), a RESTARTED analyser process and a healthy
disk, preflight verified:

    bucket     wire      on-device   (both B - A, us)
    93:30     +4.91       +3.35
    94:00    -10.34      +12.35
    94:30     +9.07       -8.65
    95:00    -10.00      +11.35
    95:30     +3.13       -0.65
    96:00     -4.50      +10.35
    96:30     -0.30       +0.35

    r = -0.921  (n=7)   wire sd 7.46 us   on-device sd 7.74 us

(An intervening attempt, 01:14-01:18, is contaminated and is neither confirmation nor refutation:
analyser NaN rows from a USB capture dropout, wire swinging +-1.5 ms, and B logging
`RECON drift=-52223` at 01:12:11 with `pad=255666` against A's `pad=51598`.)

WHAT IS ESTABLISHED IS THE AC TERM, NOT THE DC TERM -- and the distinction decides the fix. The
standing offset, which is the only thing `render_align` exists to remove, is NOT resolved:

    window 1   raw-sync B-A  +2.2 +- 2.1 us (not significant)   wire median +0.47
    window 3   raw-sync B-A  -4.5 +- 2.3 us (not significant)   wire median -1.79

Both agree in sign with the wire, both are explicitly not significant, and they disagree with each
other. SO DO NOT SIMPLY FLIP THE SIGN: that fixes the fluctuation and may invert a DC term that is
currently correct.

MECHANISM: TWO CANDIDATES DEAD, both killed cheaply, neither by argument.

1. **In-flight sample-stuffing entering `(pushed - played)`.** Precedent was good -- the sync report
   already subtracts corrections in flight "so it cannot overshoot an error it has already fixed",
   and `render_phase` has no such subtraction. DEAD: across window 3 both boards report
   `corrected -0/+0 frames` on every one of 84 and 86 reports. `rate_lock` steers instead, so there
   is no splicing to contaminate anything.
2. **Differential trim.** Each board's trim swings ~90 ppm p2p within a 4-minute window (A
   +10.5..+99.7, B +10.8..+93.8) on the same ~30 s timescale as the wander. DEAD on the analyser's
   own integral check: the swings are near-common-mode (`d_sd_ppm 2.631`), and 2.6 ppm integrated
   over 30 s is ~78 us against the 7.9 us observed, correlating at `+0.068` / `expl 0%`. It
   predicts an order of magnitude too much and explains none of it.

KNOWN-DISPLACEMENT TESTS RUN, AND THE SIGN IS STILL OPEN. Two attempts, both instructive, neither
decisive.

1. **Server-side latency, +5 ms on B.** Snapserver accepted and read back `latency=5` twice. The
   wire went 100% `nan` -- NOT because nothing moved, but because `frame_lag`'s continuity guard
   rejects a lag jump that cannot physically happen between captures, which is what a step is. So
   server-side latency steps are unmeasurable on this analyser unless ramped.

2. **`inject_split(+1000)` on B**, which ramps at `SPLIT_RAMP_US_PER_S` = 100 us/s and therefore
   keeps the continuity guard satisfied. The wire tracked it cleanly:

        BASELINE   wire  -19.9 us     on-device  -2.5 us
        STEP       wire +996.9 us     on-device  +0.9 us
        SHIFT      wire +1016.9 us    on-device  +3.4 us     ratio +0.003

   The device saw **0.3%** of a displacement the wire resolved to within 2%.

WHY THAT IS NOT PROOF OF BLINDNESS, and why it does not settle the sign: `inject_split` moves the
device's own DEADLINE, so the audio and the device's model of the audio move TOGETHER -- and that
coherent pair is exactly the quantity `render_phase` differences out. This displacement is very
likely invisible to it BY CONSTRUCTION, which makes the test uninformative rather than damning.

RETRACTED, same reason: the conclusion above that the 5 ms latency step "never reached the client"
because B's render phase did not move. If render phase cannot see a model-coherent displacement,
a step that DID apply looks identical to one that did not. That claim was unsupported.

WHAT IT DOES ESTABLISH: `render_phase` documents itself as having "the servo, the prediction model
and the pipeline depth all outside the measurement". A model-coherent 1 ms displacement producing
0.3% says that contract is not met -- it inherits the same blind spot TIMING.md names for the sync
median ("an accounting offset shifts prediction and audio together and reads as zero error"),
which is the specific flaw it was built to avoid.

AND IT RECONCILES THE PICTURE. The n=9 campaign above found `render_delta` tracking
RESYNC-PLANTED displacements at ~94% with an 8.7 +- 3.4 us residual. So the instrument sees
EXTERNALLY planted offsets and is blind to model-coherent ones. Which means the +-4 us wander it
reports at r = -0.98 is not displacement at all -- it is a different quantity that happens to
anti-correlate with the wire's wander, and that is the thing to identify.

**RUN, AND THE INSTRUMENT IS VINDICATED: ratio -1.0000 on a known 500 ms displacement.**
+500 ms server latency on B, which forces a mute and a hard resync (B logged `Hard resync 499 ms`,
so latency DOES reach the client). The wire cannot measure this state at all -- at 500 ms the two
boards share no audio in the capture window -- but the ground truth is known from the server
setting, so the wire is not needed:

    baseline   3458 paired chunks   B - A median        +3.2 us
    step       3442 paired chunks   B - A median   -499987.6 us
    shift                              -499990.8 us against a known 500000
    ratio                                  -1.0000   (9 us in 500000, 0.002%)

Sign negative, i.e. B renders EARLIER for +500 ms of latency, which is what
`server_ts + bufferMs - serverLatency` demands. So the DC sign is right as well as the gain.

    perturbation            class                                    on-device response
    inject_split(+1000)     deadline shift (audio + model together)        0.3%
    latency +500 ms         externally planted                          100.00%

The instrument sees what it was built to see and is blind to what it was built to difference out.
Both behaviours are CORRECT, and the 0.3% was invisible-by-construction as suspected, not a fault.

RETRACTED: "render_align was steering on an inverted signal throughout its tuning, so the
1150 -> 118 -> 58 us table was never about gain." That does not follow. `render_align` exists to
remove resync-planted residuals -- the externally-planted class -- and for that class the signal
is accurate to 0.002% with the correct sign. The gain interpretation of that table stands.

**~~THE ANSWER: AN ADDITIVE ERROR FLOOR OF ~10-30 us~~ (WRONG -- see THE ACTUAL ANSWER below.**
A floor of 10-30 us predicts `inject_split(+1000)` reading ~970-1030 us, i.e. ~100%. It read
3.4 us. A floor cannot produce a gain of 0.003 at 1000 us, so this explained the 25 us point and
failed on the 1000 us point. Kept because the arithmetic that kills it is the useful part.)** The same campaign
restored latency to 0, forcing a second resync, and measured the residual the pair of resyncs left:

    BASELINE      wire  -2.92 us    on-device +3.21 us
    POST-RESTORE  wire -27.95 us    on-device +6.44 us
    RESIDUAL      wire -25.03 us    on-device +3.22 us     ratio -0.129

    displacement    on-device ratio
    500 ms             -1.0000
    25 us              -0.129

Scale-dependent response with a constant offset is an ADDITIVE FLOOR: ~10-30 us is 0.002% of
500 ms and over 100% of 25 us. And -0.129 is essentially the "slope -0.10 where the arithmetic
demands -2.0" recorded above, now with a mechanism instead of a mystery.

The note at `render_delta`'s use site described this floor and did not name it as one: "the residual
is roughly CONSTANT, not proportional: the displacement ranged 17-180 us while the render delta
missed it by 8.7 +- 3.4 us each time". That is the floor, measured, and the reason "percent tracked"
looked erratic -- percent is the wrong statistic against a constant error.

IT EXPLAINS EVERY OBSERVATION AT ONCE, and retires the inversion as a separate phenomenon:
  * the r = -0.92..-0.98 wander anti-correlation -- the +-4 us wander lies ENTIRELY INSIDE the
    floor, so what the instrument reports there is its own error, not displacement. No inversion
    needs explaining.
  * ratio -0.129 at 25 us -- signal and floor are the same size, so the sign is arbitrary.
  * ratio -1.0000 at 500 ms -- signal swamps the floor.
  * `expl 0%` on the offset integral -- that integral is built from small offsets, all sub-floor.
  * **`render_align` failing "10x worse than off"** -- it operates in the 20-200 us regime, which
    is exactly where the floor is comparable to the signal. No sign error required.

CONSEQUENCE: the floor has to be identified and removed before `render_align` can work at all.
Nothing about gain, reference choice or median-vs-mean matters until it is, and all three were
tuned on top of it.

**THE ACTUAL ANSWER: `render_phase` IS STRUCTURALLY BLIND TO A REPAIRED ACCOUNTING BIAS -- which is
the exact class of fault `render_align` exists to remove.**

Read what `inject_split` does before interpreting any of the above. It is one line:

    this->pushed_frames_total_ += shift;

and the re-anchor path that shares it says so: "Biases the ACCOUNTING only, leaving the audio
alone." So it never displaced the audio. It planted a LEDGER bias, and the servo then repaired it
by moving the audio -- "the repair is the only step on the wire", which is the design.

That reframes all three points as CLASS-dependent, not scale-dependent:

    perturbation        what it perturbs                          ledger?   ratio
    inject_split        frame ledger; servo repairs by moving audio  yes     0.003
    resync residual     re-baseline of the ledger, same shape        yes     0.13
    server latency      deadline input only                          no      1.000

MECHANISM: `phase` consumes `(pushed - played)`. After the servo repairs a ledger bias, the bias
and the audio displacement are equal and opposite INSIDE that formula, so they cancel and the phase
reads ~zero while the audio is genuinely displaced. The blindness is structural and permanent, not
a gain, a sign, or a floor.

This is the failure mode already recorded at PIPELINE_DIVERGE_US -- "each was planted by a
re-baseline anchoring to a per-device instantaneous measurement, and each was invisible to every
other metric on the device by construction" -- now with the reason.

CONSEQUENCE: **`render_align` cannot work as designed.** Its signal cannot see the class of offset
it exists to correct, on any gain, with any reference, mean or median. Every measurement in the
1150 -> 118 -> 58 us table was taken on a signal that was structurally blind to the fault, so none
of them are evidence about gain either way. `render_align_max` stays 0 not pending tuning but
pending a different signal.

**THE SIGNAL render_align NEEDS: THE SINK MUST ECHO AN IDENTITY, NOT REPORT A QUANTITY.**
Specified 2026-08-28, with the two cheaper alternatives measured and eliminated first.

THE UNIFYING DIAGNOSIS, which both of tonight's real bugs share: the API returns the right number
of the WRONG KIND.

    DMA span            returned CAPACITY        needed REMAINING     (fixed, fork 56601e6bc6)
    output callback     returns  QUANTITY        needs  IDENTITY      (this)

`CallbackManager<void(uint32_t, int64_t)>` -- frames and a timestamp. Never *which* audio. So
`render_phase` has to infer the mapping via `s_ts - (pushed - played)/rate`, and a device cannot
detect that its own running counter is biased by consulting that counter. After the servo repairs a
bias, the bias and the resulting audio displacement are equal and opposite inside that subtraction
and cancel exactly. No gain, reference, mean-vs-median or filter choice fixes an identity problem
with arithmetic on quantities.

THE FIX: let the caller attach an opaque tag to audio it hands over, and have the sink return that
tag when THAT audio completes.

    caller: push(chunk, tag = server_ts of its first real frame)
    sink:   on completion -> callback(frames, adjusted_ts, tag)
    caller: "the audio I tagged S rendered at local time T" -> convert T to TSF

A CAPTURED pair, not an inferred one. `pushed`/`played` never enter it.

CHEAP TO CARRY: `write_records_queue_` already holds one record per descriptor and is already
maintained in lockstep with completion events (there is an `ERR_LOCKSTEP_DESYNC` bit guarding that
invariant), so extending `uint32_t` -> `{real_frames, tag}` rides existing structure. And
`adjusted_ts` already does the hard part, subtracting trailing silence to find when the real audio
finished.

TWO DESIGN WRINKLES, both real: a descriptor can span chunks, so the tag wants to be
(server_ts, offset) of its first real frame rather than a bare chunk id; and THE MIXER MIXES
MULTIPLE SOURCES, so a single tag per descriptor is ambiguous whenever more than one source is
active. On this bench only one is, but the design has to say something -- probably "tag only while
a single source is active, otherwise report unsupported".

FALSIFIABLE TEST, using tooling that already exists: with tags, an `inject_split` ledger bias
should read at ratio ~= 1.0 where it currently reads 0.003. That is the same experiment already run
tonight, so the pass/fail is unambiguous.

TWO CHEAPER OPTIONS MEASURED AND ELIMINATED (2026-08-28), which is what justifies the API change
rather than assuming it:

  * **Fix the ledger from the depth cross-check** instead of bypassing it. `RECON drift` is the
    ledger-error signal and needs no new API. DEAD ON RESOLUTION: with the coherence gate live the
    floor is median +22 us / MAD 23 (A) and +44 / MAD 45 (B). Correcting a tens-of-us error with a
    23-45 us MAD signal injects noise of the same order. Worth noting separately that the per-board
    MEDIANS differ systematically (+22 vs +44), and that ~22 us differential is a candidate for a
    SYSTEMATIC component of the standing offset, unlike the random walk.
  * **`min(r_push)` as a bias estimator.** `r_push = our pushed - the SOURCE's received count`, and
    the source's counter is independent of ours, so at the instant nothing is in flight their
    difference IS the bias. DEAD ON QUANTISATION: pushes happen a chunk at a time, so r_push takes
    values k*1152 + 128 frames and the per-block floor has sd 512 frames (11.6 ms). It cannot
    resolve one chunk, let alone 44 frames. Also note r_push is only meaningful WITHIN one session
    -- `dbg_pushed` and `dbg_src_received` have different epochs, so across a reconnect the
    difference reads in the tens of millions of frames.

NOT IMPLEMENTED. It is a cross-component API change (speaker base, i2s speaker, mixer passthrough,
snapclient consumer) in an upstream-bound tree, and it should not be stacked on top of three
already-unmeasured changes (TRIM_KP_RUN 0.125, 30 Hz beacons, reanchor off) on an event-prone bench.
Build it when the bench is quiet and those three are graded.

Older framing, kept because the reasoning is the reusable part:
WHAT A WORKING SIGNAL WOULD NEED: independence from the local running frame counter. The device
cannot detect that its own ledger is wrong by consulting that ledger. Options, unexplored:
  * A PER-CHUNK ledger -- record each chunk's server timestamp against the frame index at push
    time, so "played frame N" maps to a server time that was CAPTURED rather than inferred from a
    running difference. A bias in the running counter then cannot corrupt the mapping. Whether the
    bias corrupts the index itself needs thinking through.
  * Accept that only an external reference sees this class, and stop trying to correct it
    on-device. That is the honest reading of "the group is the only reference we have" -- except
    the group comparison is ALSO built from each device's own ledger, which is why it inherits the
    blindness.

DEAD CANDIDATE, recorded so it is not re-proposed: a systematic one-frame (22.7 us) accounting
difference between boards. It was the floor hypothesis's mechanism and the floor hypothesis is
refuted above.

TRAP, LIVE, COST REAL TIME TONIGHT: **`inject_split(0)` IS NOT A RESTORE.** The field is documented
"REQUEST in us, consumed once by the player task; 0 when none" -- 0 means no request pending, so
zero does nothing and the board is left permanently displaced. A campaign must reverse with the
NEGATED value (`inject_split(-1000)`) and then verify on the wire that it came back. Mine did not,
and B sat +1017 us out until it was reversed by hand.

**MEDIAN-OF-THREE IS DISCONTINUOUS — the reason the observer made the correction WORSE.** With
two devices the median IS the mean: smooth, and each device moves half the gap. With three it is
the MIDDLE VALUE, which hops whenever the ordering changes — two phases close together and a
third crossing between them steps the correction target with no real movement behind it.
Measured 2026-08-28 with the observer in the group:

    group delta seen by the speakers   +14 -31 +63 +81 +13 +11 +96 -33   (swinging ±96 µs)
    group delta seen by the observer   median +3 µs, sd 12               (same instant, clean)
    skew, correction ON                sd 13-28 µs
    skew, correction OFF               sd 8.48 µs

So the fix is to average rather than take a median for small groups, or to difference against the
mean of PEERS excluding self. The median was chosen for outlier robustness, which matters at
larger group sizes and actively hurts at three.

**DONE (2026-08-28), unmeasured.** Both the group delta and the new timebase consensus use one
reweighting pass around the MEAN instead: `w = 1/(1 + (d/(2·scale))²)` with `scale = max(MAD,
floor)`. Continuous in every input, so no reordering can step it, while a value far outside the
pack still contributes almost nothing; with two devices it degenerates to exactly their mean.
`render_align_max` is still `0ms` — re-measure before enabling.

TWO REAL BUGS WERE FIXED GETTING HERE, both worth keeping:
- Phases were differenced across a ~3.3 s staleness gap, so the signal measured relative DRIFT
  (~165 µs at 50 ppm) rather than skew. Now paired within 300 ms. `b49ae48`
- A failed pairing WIPED a good delta, and only ~25% of beacon arrivals pair, so the consumer
  essentially never saw a value — zero corrections ran. Now a miss keeps the last valid delta
  until it is genuinely stale. `c0648fb`

**CORE AFFINITY WAS COSTING REAL JITTER.** Every task in the audio path was created with
`xTaskCreate`, which on ESP-IDF is `tskNO_AFFINITY` — the scheduler places them on either core
and may migrate them. In this build wifi AND the ESPHome main loop are both on CPU0
(`CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0`, `CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0`), and the wifi
driver runs at priority 23 — above every task in the chain, including `speaker_task` at 19.

Pinning `snap_player` (prio 8) and `snap_net` (prio 5) to CPU1, matched 4-minute windows:

    unpinned     n=4949  median +5.62 µs  sd 6.24  p95 +19.3
    pinned CPU1  n=4871  median +4.47 µs  sd 3.58  p95 +11.0

sd down 1.7x, p95 down 1.8x, from a one-line change. `bb94f20`

STILL UNPINNED, and the obvious next experiment: `speaker_task` (prio 19, feeds the I2S DMA)
and `mixer` (prio 10), both in the esphome fork. The speaker task is the most timing-critical in
the chain and can still be scheduled onto CPU0 beneath a priority-23 radio.

Task map for reference:

    speaker_task  19  unpinned   (fork)      mixer         10  unpinned  (fork)
    snap_player    8  CPU1                   snap_net       5  CPU1
    ESPHome loop   1  CPU0                   wifi driver   23  CPU0
    lwIP TCP/IP   18  unpinned

**THE PAIR IS ALIGNED TO SUB-MICROSECOND WHEN QUIET.** Per-frame skew (`--dump-skew`, ~44100
rows/s) across a forced resync, 2026-08-27:

    baseline, 20 s before:   median -24.1 µs   sd 0.1 µs
    settled stretches after: sd 0.2 – 0.7 µs

So the <1 µs target is ALREADY MET between resyncs, and the sd ~5.8 µs quoted from the
per-capture CSV is capture-to-capture MEASUREMENT noise, not device jitter. Any future work
should quote the per-frame figure; the per-capture one understates the hardware by ~50x.

**THE DISPLACEMENT IS NOT A DISCRETE STEP.** The same trace shows it is the residual of a
ramp-and-collapse, not an insertion of N frames at one instant:

    +13 s   +1658 µs  settled at a new position
    +24 s   ramp begins, ~45 µs/s measured at 20 ms resolution (+0.8..+1.0 µs per 20 ms)
    +47 s   +4618 µs  peak
    +48 s   collapse when the latency offset is removed
    +69 s   -122 µs   settles ~100 µs from where it started

That retrospectively explains why every quantum-based hypothesis failed — padding, clamping,
frame counts all predict a step of N × 22.68 µs at one instant, and no such step exists. A
1391 µs "jump" at +57 s spans a 0.5 s gap where the analyser rejected, so it is NOT evidence of
an instantaneous event; at 20 ms resolution either side, the motion is smooth.

**PADDING: TESTED AND REFUTED (2026-08-27).** n=4 forced resyncs, MLS stimulus. Board B accrued
18369 frames (417 ms) of padding while the net skew moved 18.6 µs; slope +0.0008 against a
predicted +1.0, r = +0.11. Padding does not displace the output because the repayment path
(`padding_debt_frames` / `padding_repay_at_us`) already takes it back out — the prediction in
the code was written as though padding were unaccounted, and that accounting sits a few hundred
lines above it. `pad=` is a diagnostic, not a displacement term. Recorded at the call site.

The candidate WAS padding, and the prediction was already written in the code at
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
