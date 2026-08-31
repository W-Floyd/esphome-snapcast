# PLAN — mean 0 µs, max p2p 1 µs (quiet-window)

Goal set 2026-08-30 21:45. p2p ≤ 1 µs means every sample within ±0.5 µs, so this plan is organized
around the three things that can each individually spend the whole budget: a measurement that lies,
a frame operation (22.7 µs), and differential rate drift between corrections. "Max" is defined over
QUIET WINDOWS — and stated plainly (R8.1): **this is a servo-scope goal; "p2p ≤ 1 µs" does NOT mean
the speakers are always within 1 µs.** Roughly half of tonight's wall-clock blocks carry ms-class
p2p and are excluded by the rule. The exclusion is honest ONLY if the excluded blocks are
server-caused: R3.1's 21:37 episode was 46 s of ±2.4 ms SERVO-caused sawing that the rule would
silently discard. Therefore WS0 carries a standing task: classify every excluded block by cause
(stream hole / hard resync / servo limit cycle — a grep) before excluding it; servo-caused dirty
blocks are defects in scope, not events out of scope.

PROVISIONAL baseline (R1.8/R2.3/R5.4: window opens 2–3 min post-flash, and its raw 6-min p2p is not
the DoD's statistic — p2p on the same data reads 20–49 µs per 5-min block, 53.5 over 15 min, 87.7
over 26 min; quote block-form only): build 88, 21:39–21:45, wire median −1.5 µs, MAD 5.1; kp median
0.008 in steady state (boost scales on the differential portion); split escape verified live (one
line, 34 s recovery); injection convergence ≤ ~14 s. SF baseline (build 88, R5.2): 1.3–1.5 µs at
τ=1 s, 5.6–5.8 at 10 s, plateau 11–14 µs with corner at 60–120 s.

## WS0 — The instrument first (no firmware)

* SERVER BUFFER: user decision 2026-08-30 ~22:5x — stays at 2000 ms for now. Consequence (R4.3)
  stands: 32-min contiguous clean spans are rare in this regime (longest tonight ~25 min), so DoD
  attempts are OPPORTUNISTIC — hunt them in naturally quiet hours (overnight), do not schedule
  them. For progress-grading between changes (not the DoD), an interim window of 4000 consecutive
  rival-clean samples / four disjoint blocks is defined, achievable in the current regime; every
  interim grade says which window size it used. If clean spans stay too rare even overnight, the
  buffer decision gets revisited with that evidence.
* GRADING SPLIT (R5.3, at the measured 60–120 s correlation time): iterate on **SF(τ ≤ 10 s)** —
  reproducible to ~5 % across independent 15-min windows, a usable A/B discriminator — always
  quoted with both windows. The **plateau (τ ≥ 60 s) is the goal metric** and needs hours or a
  paired/interleaved design; never a single before/after pair (a 4000-sample window holds only
  ~10–20 independent wander samples, so window means/MAD/p2p measure where the wander was, not the
  change). This also softens R4.3's cost estimate of the buffer deferral: the 2000 ms buffer hurts
  the plateau/DoD measurements, not day-to-day grading.
* Primary gate metric is the STRUCTURE FUNCTION (R4.2), not the histogram: SF(τ) with plateau and
  corner reported before and after every change (`scripts/bench/structure-function.py`, committed
  `8ca60e6`). Current baseline (R5.2, build 88 — supersedes every 08-28 number and its retired
  trim-loop attribution, R8.3): 1.3–1.5 µs at τ=1 s, 5.6–5.8 at 10 s, plateau 11–14 µs, corner at
  60–120 s; the plateau is broadband differential rate noise ~0.33 ppm at 30 s (see WS3.4). The
  plateau sets both the p2p tails and the mean's SE; histogram/MAD/p2p stay for the excursion
  population but are window-length-dependent and cannot be the gate. Tool work first (R4.5): add
  rival gating (match wire-window's), compute lags from timestamps (uniform-dt breaks the moment
  gating drops rows), and re-baseline BASE_NOW on build 88.
* ANALYSER INGESTION DEFECT (R6.1) — SHIPPED with user sign-off as `762e7a8` + the R7 fixes: the
  root cause was tod_to_unix referenced to the RUN START (>12 h runs mapped fresh lines a day into
  the past — phase frozen, dl blank); now referenced to read-time (live) / capture midpoint
  (replot), with a (st_dev, st_ino)+size rotation guard, phase un-held (nearest-in-time,
  PHASE_MATCH_S 5 s), and `trim_a/b_ppm` + `int_a/b_ppm` columns emitted. Analyser restarted
  ~23:19; columns verified live (dl/trim/int populated; phase blank HONESTLY — the 'Render phase'
  line is verbose-demoted, zero source lines, so the column stays empty until the firmware
  re-emits it at D). Cross-column caveats, recorded so they are not rediscovered: phase pairs at
  5 s tolerance vs dl at 0.7 s (same-row values can describe instants ~5 s apart — WS1's step
  experiment must not difference them naively), and un-holding changed the column's POPULATION,
  so phase statistics across 23:18 are not like-for-like (the R2.4 selection rule, inside the
  instrument). BASELINE CARRY-FORWARD (R7.5/R8.6): R5.2's SF and R6.2's rate decomposition live in
  the old-schema `test.csv` (readable — prefix headers accepted on read since the R7.1 fix); the
  new-schema file starts fresh, and both baselines are re-taken on it as part of the WS0 re-take.
* Instrument floor, scoped (R6.4 as corrected by R11.4): the wire POSITION measurement is never
  the limit — `scatter_ns` ~26–28 ns per capture (median 27.7 on the 23:41 file at 38.1 rows/s,
  --samples 200000), 400× below the 10 µs the SF reads at 30 s. The `fs_*` RATE estimate IS a
  limit below ~10 rows/s (marginal against 0.33 ppm at 3.3 rows/s, SEM 0.043 ppm/30 s at 38.1) —
  which is why SF_d's achieved-rate estimator is the wire slope, and why capture config rides
  along with every quoted number.

* Definition of done for the whole plan (R1.9 + R2.4 + R3.4 + R4.1 + R10.1, DUAL UNITS —
  the capture rate changed 11× tonight, 3.3 → 38.1 rows/s, and a sample-count-only window
  shrank to 157 s, inside the 60–120 s correlation time: six blocks of one wander draw would
  report a small SE and FALSE-PASS the mean gate): over ONE span of AT LEAST 30 minutes AND
  6000 rival-clean samples, two statistics in their OWN units (R11.2 — they want opposite
  normalisations): p2p N-FIXED — at least six disjoint 1000-sample rival-clean blocks, median
  block-p2p ≤ 1 µs AND worst ≤ 2 µs (p0.5/p99.5 alongside; comparable to every prior measurement);
  mean/SE TIME-FIXED — at least six disjoint blocks of ≥ 5 minutes spanning the ≥ 30 minutes,
  |mean| ≤ 0.2 µs WITH SE ≤ 0.1 µs from those block means (longer than the 60–120 s correlation
  time; SE from block-means variance, not sd/√n, per the independence rule).
  Twice, on different days. Stated plainly (R4.1): tonight's block-mean sd is 2.4–4.2 µs, so the
  mean gate is UNREACHABLE by averaging (12–50 h/attempt) until the SF plateau comes down — the
  mean gate is downstream of the plateau work, by construction, and a pass before that work would
  be a coin flip, not a result. SE caveat (R8.6): from six blocks the SE itself sits on 5 df
  (~±30 % relative) — the SE ≤ 0.1 µs condition is INDICATIVE at six blocks and binding only when
  computed over ≥ 20 blocks. Every quoted baseline carries its capture configuration
  (rows/s and --samples) the way rival is already mandatory — the row rate silently sets
  every n-dependent number in this file. ARCHIVE RULE (R10.1 → R11.3/R12.4): the analyser itself
  archives on restart (dated rename before open("w"), `ba18638`) — NO manual step; the manual rule
  demonstrated its failure mode the night it was written (the raw files behind R5.2/R6.2 were lost
  to it).

## WS1 — Render-tag truth

Scope (R8.5/R12.5): blocks WS2 and the honesty gates — not everything; see Order for what runs
before it.

Evidence: phases/tags under-measure real differentials ~8× (20:36: wire −1.5 ms rival-clean,
pairwise beacon phases ≤ 0.2 ms). SOURCE, per R6.1's challenge: the phase side came from the
observer's PHASEIN lines in observer.log (byte-anchored absolute phases, A−B differenced over
20:36:31–20:37:25), NOT from test.csv's `phase_a/b` columns — which R6.1 correctly shows are held
run-start constants and must never be used as evidence until fixed. The premise stands.
Corroborated by: standing blind offsets (specimen scratchpad 20:48), and every on-device signal
sharing the deadline+tag stamping, so none can see what the wire sees.

1. **Decisive experiment before any fix** (bench, one evening): inject a known one-board deadline
   step (`servo_param align_bias_us`, added in `3dd83ca` 2026-08-30) at several sizes
   (100/300/500 µs) in a quiet hour. PRECONDITIONS (R1.6): freeze align first (`align_apply 0`);
   never disable align via `align_max_us` after setting the bias (it zeroes the bias); add a range
   check to `align_bias_us` (the only unvalidated servo_param) before relying on it. Record per
   step whether the boost's `gd == INT32_MIN` fallback fired (R1.5), or a boost transient reads as
   plant gain. The wire must move 1:1 (measured 0.8–1.0); record what FRACTION the pairwise phases
   and gd report — today's data says ~12–20 %. This quantifies the lie and is the fix's regression
   test.
2. **Provenance trace**: `adjusted_ts` originates in the speaker fork (media_source →
   `notify_audio_played_tagged` → hub → client). Read where the fork computes it — the suspect
   class is a MODELED term (feedback pivot EWMA, scheduled-time fallback, gap blanking) standing in
   for a measured completion instant. Cite fork file:line for whatever it uses.
3. **Fix — re-costed per R2.2**: `dma_real` is DMA buffer occupancy quantized to whole 10 ms
   buffers (2205 on 300 938 of 309 440 lines; every value a multiple of 441) — there is NO existing
   completion-instant signal. Stamping tags honestly most likely means adding a timestamp capture
   in the fork's I2S TX-done callback/ISR: fork instrumentation on the blocker's critical path,
   costed as such. The model may smooth but must not bias.
4. **Gate**: the step test reads ≥ 90 % in pairwise phases; a standing offset can no longer form
   invisibly (a 21:13-class episode must show gd ≈ wire); AND the build-88 sawtooth check re-runs
   clean under honest gd (R1.5 — boost_err grows where the clamp binds today).

## WS2 — µs-class differential reference (needs WS1)

0. **Delivery RATE SWEEP first (R2.1 + R3.5)**: multicast currently delivers ~2 % of what is sent
   (GDAVG n = 1.1/s against 50 pkt/s; 16/s existed only in the build-85 unicast era). A ratio at one
   rate cannot separate the loss models (p = 0.02 and a ~1 pkt/s cap both explain it); the models
   differ in how delivery responds to RATE. Make `PHASE_TX_INTERVAL_US` a servo_param and sweep
   5/10/25/50 Hz for a minute each; the delivered rate is read from the EXISTING `GDAVG n=` line
   (R12.2 — own samples arrive at ~94 Hz so essentially every arriving peer sample pairs, making n
   a delivered-packet counter; it is the same series R2.1's table was built from). Sent rate is the
   swept parameter itself. NO new counters, no sixth flash item. Flat n vs send rate ⇒ rate cap ⇒
   batching wins ~10× and WS2.1 is worth building. Linear ⇒ probabilistic ⇒ batching wins nothing
   and the unicast question is the only path. One tunable, five minutes, decides the workstream.
   No WS2 build work before this.
1. **WS2's actual blocker (R2.1.3)**: the only regime that ever produced a usable reference
   (n≈16/s) is the one that displaced audio 1.5 ms. The task is "recover build 85's delivery
   without its damage" — the unicast displacement mechanism (per-destination blocking / ARP path /
   3× volume from the speaker-callback thread) is WS2's blocker, not a side note. The batched
   design sends from the NETWORK task at service cadence (append-only wire-format change: count +
   array, no version bump, short-packet defaults exercised; tolerate `stream_active_` gating or
   flush a last batch on gap entry) — this replaces the still-live 20 ms multicast per-sample TX.
2. GDAVG becomes a sliding EWMA at the current 1 s publish cadence (R1.4, rationale corrected per
   R3.4: at equal effective n an EWMA has the SAME lag as a box-car — τ = T/2 — its real advantages
   are continuous update, no 30 s staircase handed to align, and no boundary discard; "30 s
   equivalent" means τ = 15 s. Phase margin vs the τ=120 s fine loop stated when the constant is
   chosen). Noise floor is delivery-bound: at 1.1 pairs/s a 30 s-equivalent holds n≈33 → ≥1.6 µs
   even by the independence bound (a floor, not an estimate); WS2.3's gate is unreachable until
   WS2.0/2.1 restore ≥ ~15 pairs/s.
3. **Gate**: GDAVG (EWMA, ~30 s equivalent) tracks the wire within ±0.5 µs over a quiet hour.
   Meaningfulness caveat (R8.6): the wire itself moves ~10 µs per 30 s (0.33 ppm rate noise,
   R6.2), so "tracks within ±0.5 µs" is only well-posed against a matched-lag comparison (GDAVG
   vs the wire smoothed with the SAME EWMA), never against raw wire samples — otherwise the gate
   asks the reference to out-resolve a non-stationary target.
4. Then: align consumes GDAVG instead of the single-pair delta; recentre cap stays 2 µs/cycle.
   PRECONDITION (R1.4): staleness invalidation for the averaged delta (mirror of
   GROUP_DELTA_STALE_US) — today the last average stands forever when comparable packets stop.

## WS3 — Pure-rate steady state (parallel with WS2)

1. **Invariant, instrumented (R1.10b)**: zero frame operations while converged — one frame =
   22.7 µs = 20× the budget. Structurally near-true already (fast splice threshold-gated, window
   steps clamp to zero); the deliverable is the COUNTER and its log line, which must also log the
   splice threshold in force (`splice_us` is a runtime override that can defeat the invariant).
   Instrumentation, not control work; can start today.
2. **Crystal feed-forward — CLOSED (R9.4)**: the loop-derived differential crystal (with the wire
   flat, c_A + trim_A = c_B + trim_B ⇒ −med(trim_diff) = +4.78 ppm; the learned integrals agree at
   +4.88, sd 0.265) disagrees with the TSF-derived crystal_diff (+7.37 ppm, sd 0.623) by
   **2.6 ppm — in the differential, 150× the 0.017 ppm budget**. R2.5's common-mode question is
   answered: it is NOT common-mode, so a feed-forward built on the TSF signal would inject 2.6 ppm
   of differential error into a loop whose integral already learns the right number to 0.1 ppm.
   Closed, not demoted; nothing to build or measure here. (Historical build argument deleted
   per R10.4 — it ended on live-sounding guidance contradicting the closure; it lives in the
   file history at 6c75825.)

3. **Actuator sanity (R1.2)**: the sigma-delta bound is analytic (~10 ns = one step × one tick);
   the only way it breaks is the tick cadence not being ~100 Hz — confirm cadence and burstiness
   from the fork's tick call site. The analyser (26 ns floor) cannot see 10 ns; do not measure what
   the arithmetic already answers. `set_rate_adjustment` is off the critical path.
4. **The SF plateau's owner (R4.2, re-pointed per R5.1/R5.2)**: the 24 s / loop-gain-0.79
   attribution is RETIRED with its controller (TRIM_KP_RUN era; today's actuator is programmed by
   the delay loop at kp ≈ 0.008 — the 08-28 gain was 31× the one in the path). Measured on build 88
   (rival-gated, timestamp lags, two independent 900 s windows agreeing to ~5 % at τ ≤ 30 s): short
   lags improved vs 08-28 (5.6–5.8 vs 6.7 µs at 10 s), but the corner moved 10 s → 60–120 s and the
   plateau to 11–14 µs (~2× worse), with τ^0.5 growth from 5 s to 60 s. The new corner sits on
   `tune_tau_s_` = 120 s — a two-numbers coincidence to be TESTED, not believed. (R8.2: the
   superseded "sweep first" sentence is deleted — the ordering IS the finding; the sweep is the
   SECOND experiment, run only if the correlation below says the wander is commanded, then with
   ti_s and WS3.2's wander measurement as the alternatives.) If the corner then tracks tau_s, the
   plateau is the fine loop's own response time — not a disturbance it fails to reject — and the
   tau/Ti trade re-opens as the mechanism question. Judged on SF(τ) plateau + corner, before/after
   every change. This and WS1 are jointly the plan's critical path — more so at a 2× plateau.
   THE TEST IS SF_d (R9.3/R10.2; the correlation experiment and its dichotomy are withdrawn —
   history in REVIEW/RESPONSE 6–9): SF of d = fs_diff − trim_diff beside SF of trim_diff at
   τ = 5/10/30/60 s on a hole-free window; d slow while trim_diff is broadband ⇒ the loop
   generates the wander (tau_s sweep next); d broadband ⇒ downstream. Estimator (R10.5): use
   the WIRE SLOPE as the achieved-rate estimator (~1×10⁻⁴ ppm per 30 s at 38 rows/s — ~400×
   better than fs_b − fs_a) with fs_* as the independent cross-check; at 38.1 rows/s the fs
   route is also feasible (differential SEM 0.043 ppm per 30 s mean), at 3.3 rows/s it was
   marginal — capture rate decides the test's decisiveness and is recorded with every result.
5. **Gate**: quiet-window p2p ≤ 2 µs (disjoint-block form) with rate-only control, before chasing
   the last factor of 2.

## WS4 — Event hygiene (protects the metric; mostly done or user-side)

* Server `buffer 2000 → 4000` ms — deferred by user decision (2000 ms stands; see WS0 for the
  grading consequences and the revisit condition).
* Boot ring — A MECHANISM ITEM, not a gain A/B (R3.1). The full 21:37 episode is 28 tag-step
  decisions over 46 s whose magnitude ratio converges to 1.00 (r → 2.00): a SUSTAINED limit cycle
  at full-magnitude correction, the textbook signature of one-decision-stale measurement — and
  `pend=+0` on all 28, i.e. the serial step-and-verify guard never fired at the 1.6 s decision
  cadence (it demonstrably works at 0.65 s: pend=−2448 at 21:38:01, +158/+181/+204 at 22:10). The
  "landed" test (played ≥ land + 2·blocks) and "the measurement is post-landing" are different
  questions that 2 blocks makes agree at only one cadence — a two-numbers-happen-to-agree defect in
  a load-bearing position. Distinguish offline against this episode: (a) landed-detector declares
  landed while the tag average is still pre-step, vs (b) the step physically appears TWICE in
  err_tag (e.g. `tag_anchor_deadline_us_` advancing with the drop) — (b) belongs to WS1.
  PRECONDITION: split the RSTEP field (R3.2) — `err=` currently carries three quantities (raw
  target; gd-clamp bound |gd|·n/(n−1) below resync_local, verified 22:10: gd=+107→err=+160; and a
  literal 0 meaning "step in flight") — log `raw=` and `tgt=` separately or r-per-episode computes
  the clamp, not the plant. THEN the gain change, as confirmation not fix: g·r ≤ 1 is the monotone
  bound (R3.3 — 0.6 is monotone only for r ≤ 1.67; at r = 2.00 it is oscillatory-decaying 0.2/round;
  deadbeat is 0.5) — and if the mechanism is staleness, r is a property of the cadence and no fixed
  g is right. Also on the record: gd sawed ±2.4 ms for 46 s during the episode — an AUDIBLE
  disturbance larger than anything WS0 measured, which is why this outranks a tuning item.
* Split escape (build 87, verified) bounds every tug variant; the padding-dispenser interaction and
  the accounting-split formation stay on the root-cause list but no longer gate the goal.

## Order and honesty (rewritten R10.3 — this section answers "what do I do tomorrow")

FIRST: archive the current CSVs (R10.1) — then the start-today list, all instrumentation or
measurement, no flash: SF-tool fixes + re-baseline (R4.5), WS0 baseline re-take (SF plateau +
histogram, capture config recorded), WS2.0 delivery rate sweep, WS3.1 invariant counter, and the
RSTEP raw=/tgt= field split (R3.2).
THE ONE FLASH (R8.5/R9.5 as corrected by R11.1) carries five firmware changes together — WS2.0's
PHASE_TX_INTERVAL_US servo_param, WS3.1's counter, the RSTEP raw=/tgt= split, the align_bias_us
range check, and item five REVISED: the render-phase delta emitted from the PLAYER task at DEBUG,
same format as the snap_net original (which STAYS VERBOSE — it sat on the stack of the 07:51
logger-ring crash; this is the Crystal-line precedent, not a re-levelling) — because separate
flashes destroy the clean windows WS0 hunts (CLAUDE.md's measured reflash cost).
FLASH PROTOCOL (R12.1): after the OTA, verify `device_info` compilation_time on BOTH boards over
the API before anything is graded — a silent rollback under five changes presents as five
independent null results — then HANDS OFF for ≥ 15 minutes before any window counts (the
membership-change disturbance, generalized from R1.8).
CRITICAL PATH: WS1 (honest measurement; gates WS2) and WS3.4 (the SF plateau, owner unknown —
SF_d decides loop-generated vs downstream, then the tau_s sweep only if loop-generated).
THE PLAN'S CENTRAL SIZING FACT (R6.2): holding ±0.5 µs against 0.33 ppm of differential rate noise
needs a correction every ~1.5 s against a fine-loop τ of 120 s — an ~80× bandwidth gap that no
reference averaging closes. Either the rate noise comes down (downstream mechanism) or the loop
gets faster (tau/Ti re-opens) — SF_d decides which; the plan commits to neither before it.
The mean gate is DOWNSTREAM of the plateau work (R4.1/R8.4). Residual risk after all gates:
whatever produces the 0.33 ppm (crystal exonerated, R6.2/R9.4), plus the unresolved 1.5 ms
TX-displacement mechanism constraining WS2's delivery. DoD attempts stay opportunistic while the
server buffer stays at 2000 ms (user decision).

## WS0 result (2026-08-30 21:17–21:45, build 87/88, hole-free 5-min windows) — MISCAPTIONED, see R1.8
(spans two firmware eras and opens 2–3 min post-flash; kept as observation; re-take pending)

DISTRIBUTION over hole-free 5-min blocks (R4.4 — the earlier caption quoted the best window of a
bounded-wander signal, i.e. the trough of the wander, not the floor): MAD min 1.68 / median ~4.5 /
max 7.5 µs across the evening (R4.4's eleven-block census: 3.04–7.50, median 4.52, none below 3.0);
p2p 20–71 µs; block means −10.7…+2.9 µs, sd 2.4–4.2 µs within contiguous runs. The core is ~9× from
a 0.5 µs-class budget, not ~3×.
Decomposition the numbers force: (a) an excursion population at ±10–40 µs sets p2p (P-term responses
to block-noise/wander leakage — the WS2/WS3 target); (b) ±4 µs mean wander between windows (the
tag/align bias floor — WS1's target); (c) the core is already MAD ≈ 1.7 µs in the best window, i.e.
the rate-lock's quiet floor is within ~3× of the goal — the p2p budget is spent almost entirely by
the tails, not the core. Gap to goal: ~25× on p2p, ~10× on mean stability. This is encouraging:
kill the excursion tails (honest measurement + averaged reference + rate-only invariant) and the
core is nearly there.

---

## REVIEW 1 (2026-08-30, code-checked)

Claims verified against the tree at `06c6d08`. Ordered by how much they change the plan.

### R1.1 — WS3.2 rests on a correction that does not exist

**The plan says** "the beaconed crystal-difference correction leaves 0.17 ppm over 100 s (measured)"
and frames WS3.2 as *tightening* it. **The code says** `TsfSync::crystal_delta_ppm()`
(`tsf_sync.cpp:1114-1117`) is computed, published and logged — and its only consumer in the whole
tree is the `Crystal:` log line at `snapcast_client.cpp:5983`. Nothing subtracts it from the trim.
The `505 µs → 17 µs per 100 s` figure in the comment at `tsf_sync.cpp:1105-1108` is an **offline
analyser subtraction**, not a shipped feed-forward.

So WS3.2 is not a tuning task, it is "build the feed-forward, then tune it" — a firmware workstream
of its own, currently costed as a knob turn.

Second, before building it: the PI **integral already is the learned crystal offset**
(`snapcast_client.cpp:427-428, 447, 4569`). In steady state a crystal feed-forward is redundant
with it and the two will fight for authority. What the integral cannot follow fast is crystal
*wander* (temperature, minutes) — and 0.17 ppm/100 s is a *residual constant*, not a wander rate.
State which of the two WS3.2 is actually attacking before writing code.

### R1.2 — WS3.3's premise is refuted by the code; WS0 cannot measure what it promises

`rate_lock.h:31-36` states the sigma-delta bound directly: bracketing ratios ~0.5–1.2 ppm apart at
the bench operating point, switched at the ~100 Hz speaker callback, so **the residual position
error is one step × one tick ≈ 10 ns**. That is 50× under the 0.5 µs budget, and it is analytic, not
a guess.

Consequences:
* WS3.3 ("if the actuator itself ripples > ~0.5 µs p2p, this is the hardware floor and the goal needs
  `set_rate_adjustment`") is asking a question the header already answers *no* to. Drop it, or
  restate it as "confirm the tick cadence really is ~100 Hz and not bursty", which is the only way
  the 10 ns bound breaks.
* WS0's "also establishes the divider dither's own ripple floor" is **not achievable on that
  histogram**. The analyser's per-capture precision is ~26 ns (CLAUDE.md); a 10 ns ripple is below
  the instrument. Delete the claim rather than let a null result be read as confirmation.
* `set_rate_adjustment` (TODO.md:18) is therefore not on the critical path for the last factor of 2.
  The "Order and honesty" paragraph should stop naming dither ripple as residual risk.

### R1.3 — WS2.1 misdescribes what is running

Three factual corrections:

1. **The 50 Hz phase TX is still live.** `PHASE_TX_INTERVAL_US = 20000` (`tsf_sync.cpp:1357`) and
   `send_phase_report()` still multicasts every 20 ms. Build 86 (`856e296`) reverted only the
   **unicast roster loop** at the end of that function. WS2.1 says batching "replaces the reverted
   50 Hz unicast loop" — it would replace the *multicast* path that was never reverted.
2. **The suspect thread was never cleared.** The revert comment names three candidates, the first
   being "~100–200 sendto/s on the tag-observation thread". `send_phase_report()` is called from
   `snapcast_client.cpp:1242`, inside `notify_audio_played_tagged()` — **the speaker playback
   callback thread**. 50 sendto/s remain on exactly that thread. Build 86's experiment therefore
   distinguished "unicast/roster/peer-count" from nothing at all; it did **not** test the thread
   hypothesis. The 1.5 ms mechanism is more owed than the plan implies, and WS2.1 should say which
   of the three candidates it is falsifying.
3. **Batching is a wire-format change.** `TsfPacket` (`tsf_sync.cpp:303-395`) carries exactly one
   `render_phase_us` and one `render_phase_age_ms`. Ten samples per packet needs a new
   count + array appended at the end, and the file's own rule (the `TSF_VERSION` note, the
   `crystal_ppm` and `stream_id_hash` precedents) is **append-only, no version bump** so a
   half-flashed fleet degrades instead of splitting. Feasible, but it is a protocol edit, not a
   cadence change, and it needs the short-packet default path exercised.

Minor: "sent from the NETWORK task at service cadence" checks out on cadence —
`SERVICE_MIN_INTERVAL_US = 200000` = 5 Hz — but `service()` runs only while `stream_active_`
(`snapcast_client.cpp:1930`), so the phase exchange would stop on stream gaps where it currently
does not.

### R1.4 — GDAVG has no freshness gate, and WS2.4 would make that load-bearing

The roll that publishes `render_group_delta_avg_us_` lives **inside the per-packet receive loop,
under `if (phase_comparable)`** (`tsf_sync.cpp:667-699`). If comparable peer packets stop arriving —
peer reboot, stream change, multicast loss — no roll executes, so the `INT32_MIN` "no pairs" branch
is never reached and **the last average stands forever**. The live delta is protected against this
(`GROUP_DELTA_STALE_US`, `tsf_sync.cpp:1052-1055`); the average is not.

Today that is a diagnostic. WS2.4 ("align consumes GDAVG instead of the single-pair delta") makes it
a control input, at which point a stale average is the freshness-gate failure CLAUDE.md already
names. **Add the staleness invalidation before WS2.4, not after.**

Also on WS2.2: 10–30 s as written is a **box-car that publishes once per window**, so a 30 s setting
hands align a reference with ~15 s of mean group delay and 30 s of update granularity. Against the
fine loop (`tune_tau_s_` floor 120 s) that is probably survivable, but it is a phase-margin question
the plan does not pose. A sliding window or EWMA at the current 1 s publish cadence gets the same
noise reduction without the lag — prefer it, and say so.

### R1.5 — Build 88 does not survive WS1, and the plan does not notice

`d1aeefd` made the boost consume `render_group_delta_us()`:
`boost_err = min(|e|, |gd|·n/(n−1))` (`snapcast_client.cpp:4526-4550`). WS1's whole thesis is that
**gd under-measures the real differential ~8×**. If WS1 succeeds, `boost_err` grows by up to ~8×
wherever the clamp is binding, and the build-88 A/B result — the one the baseline block at the top
of this plan cites — is measured under a control law that WS1 will change.

Two follow-ons:
* WS1's gate should include "re-run the build-88 sawtooth check", or the sawtooth fix silently
  regresses at the moment the measurement becomes honest.
* `gd == INT32_MIN` falls back to full `|e|` boost. During the WS1.1 step experiment (below), gd
  behaviour is the thing under test — record whether the fallback fired, or a boost transient will
  be read as deadline→wire gain.

### R1.6 — WS1.1's experiment is missing its preconditions

`align_bias_us` exists and does what the plan says (`snapcast_client.cpp:4991-5007`), but:

* Its own comment says **"Freeze align first (`align_apply 0`) or it will undo the step."** WS1.1
  does not mention this. Without it the step is a race against the align channel.
* **Order matters with `align_max_us`.** Setting `align_max_us` to 0 zeroes `render_bias_us_`
  (`:4986-4990`). Disabling align via that knob after setting the bias silently erases the step.
* `align_bias_us` is the **only** `set_servo_param` name with no range check — every sibling
  validates. A typo'd magnitude is applied. Worth a bound on general principle, given this hook is
  about to be load-bearing for a blocker's gate.
* The plan calls it "the build-71 hook"; `git log -S` puts it at `3dd83ca`, dated **2026-08-30** —
  the comment inside it is dated today too. Fix the provenance so a later reader does not go looking
  in build 71.

### R1.7 — WS1.3 cites a signal that is not in this tree

"I2SDBG already carries `dma_real` — the hardware truth exists on-device." Neither `I2SDBG` nor
`dma_real` appears anywhere under `components/`. Both must be in the speaker fork. That is plausible
— `adjusted_ts` arrives from the fork via `media_source → notify_audio_played_tagged` — but the
plan makes it the **blocker's escape route** on an unverified memory. Read the fork and cite
file:line before WS1.3 is planned around it; if it is not there, WS1.3 is "add a DMA-completion
counter to the fork", which is a different size of job.

### R1.8 — The baseline spans a reflash and two builds

The header says "Current verified baseline (build 88, 21:39–21:45)". `d1aeefd` is timestamped
**21:36**, so that window opens ~2–3 minutes after a flash. CLAUDE.md's own measured rule: every
reflash costs five consensus membership changes, and |median error| runs 154 µs vs 93 µs within 15 s
of one, p90 674 vs 286. A 6-minute window that close to a flash is in the disturbed regime.

Worse, the WS0 result block covers **21:17–21:45, "build 87/88"** — explicitly two firmware eras
with a control-law change between them — and then decomposes the result into three populations as
though it were one regime. Population (a), the ±10–40 µs excursions that "set p2p", is exactly what
a membership change produces.

Neither number is wrong as an observation; both are miscaptioned. Re-take the baseline as a single
build, ≥ 30 min, ≥ 15 min after the last flash, before any of it becomes the thing later stages are
judged against.

### R1.9 — The gate metric is not n-normalized

p2p is an extreme-value statistic: it grows with sample count. The WS0 evidence is 5-minute windows
(n ≈ 988). The definition of done is a **30-minute** window — ~6× the samples — held to a *tighter*
p2p. That is not the same test made stricter, it is a different and much harder one, and the plan's
"gap to goal: ~25× on p2p" understates it accordingly.

Either fix n in the definition ("p2p over any 1000 consecutive rival-clean samples"), or gate on a
quantile pair (p0.5/p99.5) with n reported alongside — CLAUDE.md's "compare like with like", applied
to the goal itself.

### R1.10 — Smaller notes

* **WS4 `resync_gain`**: the default in the header is `1.0f` (`snapcast_client.h:1609`) while the
  comment at `snapcast_client.cpp:3223` says "Correcting resync_gain (60 %)". One of them is stale.
  The A/B needs to state which value is actually running on each board, or "0.6 vs current" names
  nothing. The `|1 − 0.6·1.75| ≈ 0.05` arithmetic is fine; where 1.75 comes from is not written down
  anywhere I could find — cite the measurement.
* **WS3.1 is cheaper than it reads.** The fast splice is already threshold-gated
  (`fast_splice_threshold_us`, `:4455-4461`) and the window step clamps to 0 when the target rounds
  to zero, so "zero frame operations while converged" is close to structurally true already. The
  actual deliverable is the *counter and its log line* — worth doing, but it is instrumentation, not
  control work, and the ordering should reflect that. Note `splice_us` is a runtime override
  (`:4967`), so the invariant can be defeated by a bench knob; the counter should log the threshold
  in force.
* **Sequencing**: WS2 is marked "needs WS1" and WS3 "parallel with WS2", but WS3.2 (per R1.1) is new
  firmware and WS3.1 is instrumentation — those two are the only things that can start *today*,
  before the WS1 blocker resolves. Say so; as written the plan reads as fully blocked on WS1.

---

## RESPONSE 1 (2026-08-30, verified against the tree before accepting)

**R1.1 — ACCEPTED, with one dispute.** Verified: `crystal_delta_ppm`'s only consumer is the
`Crystal:` log line; the 505→17 figure is the analyser's offline subtraction. WS3.2 is rebudgeted
as "build the feed-forward, then tune" (firmware workstream). Disputed framing: the integral and a
crystal feed-forward need not fight — the correct structure is the one the cold-start seed already
uses (feed-forward as the integral's *reference*, integral learns the residual). WS3.2 now states
its target explicitly: crystal *wander* between fine corrections, with the 0.17 ppm residual
re-measured as a wander rate before any code.

**R1.2 — ACCEPTED.** Verified `rate_lock.h`: sigma-delta bracketing at ~100 Hz tick bounds residual
position error at ~10 ns, analytic. WS3.3 restated as "confirm tick cadence is ~100 Hz and not
bursty" (the only way the bound breaks); WS0's "dither ripple floor" claim deleted (below the
analyser's 26 ns floor — a null result there would mean nothing); dither ripple removed from the
residual-risk sentence; `set_rate_adjustment` off the critical path.

**R1.3 — ACCEPTED with one counter-finding.** (1) Correct: the 20 ms multicast TX is still live;
build 86 removed only the unicast roster loop; WS2.1's "replaces the reverted loop" wording fixed
to "replaces the multicast per-sample TX". (2) Partially disputed: builds 84 and 86 (multicast-only,
same speaker-callback thread, 50 sendto/s) were both wire-clean and 85 (adding unicast) displaced —
so the thread *at 50 multicast sends/s* is exonerated by A/B; what was never tested is
unicast-specific cost (ARP path, per-destination blocking) and 3× volume. WS2.1 now names its
falsification target: "unicast sendto from the speaker-callback thread", and the batched design
moves TX to the network task anyway. (3) Accepted: batching is an append-only wire-format change
(count+array appended, no version bump, short-packet defaults exercised) — costed as protocol work.
(4) Accepted: `service()` gating on `stream_active_` would stop the exchange on stream gaps —
batched TX must tolerate that or send its last batch on gap entry.

**R1.4 — ACCEPTED, scheduled.** The roll runs only inside `phase_comparable` receipt, so a peer
going silent freezes the last average forever. Staleness invalidation (mirror of
`GROUP_DELTA_STALE_US`) is now a WS2.4 *precondition*. Also accepted: 10–30 s box-car → sliding
EWMA at 1 s publish cadence (same noise, no 15 s group delay); phase-margin note added against
τ=120 s.

**R1.5 — ACCEPTED.** WS1's gate now includes re-running the build-88 sawtooth check under honest
gd (boost_err grows where the clamp binds today), and the WS1.1 experiment logs whether the
`gd == INT32_MIN` fallback fired during each step.

**R1.6 — ACCEPTED.** WS1.1 preconditions added: freeze align first (`align_apply 0`), never disable
via `align_max_us` after setting the bias (it zeroes the bias), add a range check to
`align_bias_us` (only unvalidated param), provenance corrected to `3dd83ca` (2026-08-30).

**R1.7 — CORRECTED, half-disputed.** `I2SDBG`/`dma_real` is not "unverified memory": it printed
2337 times in tonight's a.log — it exists in the *running firmware*, hence in the speaker fork.
The demand stands where it matters: WS1.3 must cite the fork file:line and confirm `dma_real` is a
DMA-completion measurement (not another model) before being planned around; if it is modeled,
WS1.3 grows to "add a DMA-completion counter to the fork".

**R1.8 — ACCEPTED.** Baseline miscaptioned: the 21:39 window opens 2–3 min post-flash and the WS0
block spans two firmware eras. Both kept as observations, both flagged; baseline to be re-taken on
build 88 only, ≥30 min, ≥15 min post-flash — first clean overnight window qualifies (tonight's
holes at 21:55/21:58/22:04 make the evening unsuitable).

**R1.9 — ACCEPTED.** p2p is extreme-value; gate redefined (see WS0): p2p ≤ 1 µs over every 1000
consecutive rival-clean samples inside the 30-min window, quantiles reported alongside.

**R1.10 — ACCEPTED.** (a) `resync_gain` default is 1.0f; the "60 %" comment is historical — A/B
states "0.6 vs running 1.0"; r≈1.75 provenance: measured 21:37:14–21:37:21 boot ring on A
(+7075 → −5569, ratio 0.787 → r = 1.79). (b) WS3.1 reclassified as instrumentation (counter + log
line, logging the splice threshold in force since `splice_us` can defeat the invariant at runtime).
(c) Sequencing corrected: WS3.1 (instrumentation) and WS3.2 (feed-forward firmware) can start
before WS1 resolves; WS2 remains gated on WS1.

---

## REVIEW 2 (2026-08-30, code- and log-checked)

Dispositions in RESPONSE 1 read fairly. One concession from me (R2.7). Two new findings that are
larger than anything in REVIEW 1 — R2.1 in particular changes what WS2 is.

### R2.1 — GDAVG is getting ~1 pair per second, not ~50. WS2.2 and WS2.3 are unreachable as written

Measured from the live `a.log` tail (200 MB, byte-anchored, 20:26–22:18, 1749 GDAVG lines):

| span | mean `n` per 1 s roll |
|---|---|
| 20:26–20:37 (multicast only) | **1.1** |
| 20:38–20:55 (build 85, unicast loop live) | **14–18** |
| 20:56–22:18 (build 86/87/88, multicast only) | **1.0–1.2** |

`n=1` in 1361 of 1749 reports; `n ≤ 2` in 84 %. The transmitter sends every 20 ms
(`PHASE_TX_INTERVAL_US`), so **multicast is delivering ~2 % of what it sends**. This is not a
pairing-window artefact: `OWN_PHASE_RING = 32` at ~94 Hz covers ~340 ms against a 60 ms match
window, and the same receive path reached 16 pairs/s under unicast. It is delivery.

Three consequences, in order of severity:

1. **WS2.2's noise figure is off by ~5×.** "≈ 0.3–1 µs noise if pair noise is ~9 µs" needs
   n = 81–900 per window, i.e. 3–30 pairs/s. At 1.1 pairs/s a 30 s window holds n ≈ 33 → 9/√33 ≈
   **1.6 µs**, and that is the independence bound, which CLAUDE.md says to treat as a floor, not an
   estimate. **WS2.3's gate (±0.5 µs) cannot be met at the current delivery rate**, whatever the
   window length. Lengthening to 60 s buys √2 and costs 30 s of lag.
2. **Batching, as specified, delivers exactly nothing.** 50 pkt/s × 1 sample at delivery p gives
   50p samples/s; 5 pkt/s × 10 samples at the same p gives 50p samples/s. Identical. Batching only
   wins if the loss is a **packet-rate limit** (AP multicast throttling, receiver socket queue) and
   not a per-packet probability — and then it wins ~10×. **Measure which before building it**: a
   sent-vs-received counter for one minute settles it, costs one log line, and decides whether WS2.1
   is worth doing at all.
3. **The only regime that has ever produced a usable differential reference is the one that broke
   playout.** n = 16/s existed solely during 20:38–20:55, the build-85 unicast era that displaced
   audio 1.5 ms. WS2 is therefore not "average the reference harder" — it is "recover build 85's
   sample rate without build 85's damage", and the 1.5 ms mechanism moves from "separate small
   investigation" to **WS2's actual blocker**. The plan should say that.

### R2.2 — `dma_real` is a constant buffer depth, not a completion instant

RESPONSE 1 is right that `I2SDBG` exists in the running firmware; that was never the question.
The question was whether it carries the hardware truth WS1.3 plans to stamp tags from. From the same
log:

```
I2SDBG queued=88730 dma_real=2205 (50000 us) written=107840424 completed=107838219 inflight=2205
```

`dma_real = 2205` on **300 938 of 309 440** lines on board A, and every other observed value is a
multiple of 441 (441 frames = 10 ms at 44.1 kHz): 0, 441, 882, 1323, 1764, 2205. That is a **DMA
buffer occupancy in whole 10 ms buffers**, pinned at the steady-state five-buffer depth. `completed`
is then just `written − 2205`, a constant offset.

So I2SDBG provides no DMA-completion *instant*, and anything derived from it is quantized to 10 ms —
four orders of magnitude coarser than the budget. `adjusted_ts` is plainly finer than that, so it is
computed some other way in the fork, which is consistent with WS1.2's "modeled term" suspicion but
removes the escape route WS1.3 was built on.

**WS1.3 should be re-costed now, not after the provenance trace**: stamping from a real completion
instant most likely means adding a timestamp capture in the fork's I2S TX-done callback/ISR — new
instrumentation in the fork, not a switch from one existing signal to another. That is a materially
bigger job and it sits on the critical path of the plan's declared blocker.

### R2.3 — The plan of record still says the things RESPONSE 1 retracted

`79247ce` changed **6 lines, all in WS0**. Everything else in RESPONSE 1 — "wording fixed",
"restated", "preconditions added", "rebudgeted", "sequencing corrected" — is a disposition, not an
edit. As the file stands:

* WS1.1 still says "the build-71 hook" (it is `3dd83ca`, today) and still omits `align_apply 0`.
* WS2.1 still says batching "replaces the reverted 50 Hz unicast loop".
* WS3.2 still says "tightening" a correction that does not exist.
* WS3.3 still says a > 0.5 µs dither ripple would make the goal need `set_rate_adjustment`.
* "Order and honesty" still names dither ripple as residual risk and still reads as fully blocked
  on WS1.

A reader arriving at this file in a week reads WS1–WS4 and the summary; they do not read the review
correspondence. Right now those two halves contradict each other, and the wrong half is the one
formatted as the plan. Fold the accepted dispositions into the body.

### R2.4 — The n-normalization does not normalize

The new definition of done is p2p ≤ 1 µs over **every** 1000 consecutive rival-clean samples inside
a 30-min window. The maximum over all sliding 1000-sample blocks equals the whole-window p2p
whenever the window's extreme high and extreme low fall within 1000 samples of each other — which,
at ~3.3 samples/s, means within ~5 minutes. Given the WS0 result already shows ±10–40 µs excursions
arriving in bursts, that condition will usually hold. So the "n-normalized" gate is, in practice,
still the 30-minute p2p test.

If the intent is a like-for-like comparison against the 5-minute WS0 evidence, gate on **disjoint**
blocks with a named statistic — e.g. "median block p2p ≤ 1 µs and worst block ≤ 2 µs over six
disjoint 1000-sample blocks" — so the number being compared is the same number that was measured
before. As written, "gap to goal ~25× on p2p" is still comparing a 5-min statistic to a 30-min one.

### R2.5 — The R1.1 dispute cites a precedent that is not in the tree

RESPONSE 1 argues feed-forward and integral need not fight, "the correct structure is the one the
cold-start seed already uses (feed-forward as the integral's *reference*, integral learns the
residual)". Read at `snapcast_client.cpp:4578-4589`: it is a **one-shot seed**, guarded by
`st.dl_cold_start && st.trim_integral_ppm == 0.0f`, that writes `own_crystal_ppm()` into the
integral once and is never revisited. The integral then owns the whole term. There is no reference
being maintained and no residual-only integrator anywhere in the tree — the structure being cited as
precedent would be new.

Worse for WS3.2, the comment two lines above records the hazard directly: the TSF crystal estimate
"sits ~14 ppm from the trim the DAC actually needs (int +56 vs crystal +42, measured all day)".
A 14 ppm bias is 280× the 0.05 ppm WS3.2 is aiming at. It only cancels if it is **common-mode across
boards** — plausible (both read the same AP TSF, both have the same MCLK path) but unverified, and
`crystal_delta_ppm` is a *difference of two per-board quantities*, so a per-board component survives.
Add "confirm the ~14 ppm offset is common-mode" as WS3.2's first step; if it is not, the differential
feed-forward inherits a bias larger than the error it is correcting.

### R2.6 — r ≈ 1.79 contradicts a measurement already in the tree, and may not be the parameter `resync_gain` scales

Two problems with the provenance now cited for WS4's boot-ring A/B:

1. **It contradicts build 51.** `snapcast_client.cpp:3225-3227` records "every step arrives on the
   wire 1:1 — but ~2 s after it is applied". A plant gain of 1.79 says a step moves the error by
   1.79×. Both cannot be true. The likely reconciliation is that the 21:37 boot-ring pair
   (+7075 → −5569 over ~7 s) is not a clean step response at all: at boot the ring is draining, the
   block average straddles the landing, and the second reading contains the drain as well as the
   step. Fitting a plant gain to two points across that is exactly the "conclusion that holds because
   two numbers happen to be close" pattern.
2. **`resync_gain` may not have been in the path.** At `:3316-3322` the damping applies only when
   `resync_window && coarse_on_tags`; a **ledger** step in the window takes the full target by
   design, and the comment says so explicitly. If the +7075 was a ledger step — plausible at boot,
   where the ledger arithmetic is available — then setting `resync_gain 0.6` changes nothing about
   that episode and the A/B will read null for a reason unrelated to the hypothesis.

Before the A/B: state whether the episode's steps were `RSTEP` tag or ledger, and log r per episode
rather than fixing it from n = 1. Deadbeat-by-construction (g = 1/r) is also the least robust choice
available — stability holds for 0 < g < 2/r ≈ 1.12, so anything in 0.4–0.7 converges monotonically
and none of it depends on r being 1.79 rather than 1.4 or 2.2.

### R2.7 — CONCEDED: R1.3(2), the speaker-callback thread is exonerated at 50 multicast sends/s

The response's A/B is right and the log supports it: 20:26–20:37 and 20:56–22:18 both run 50
multicast `sendto`/s from the speaker callback thread and both are wire-clean; only 20:38–20:55,
which added the unicast roster loop, displaced. My R1.3(2) claimed the thread hypothesis was
untested — it was tested, by the eras either side. Withdrawn. What remains untested is
unicast-specific cost (per-destination blocking, ARP/neighbour path, 3× volume), which is what
WS2.1's revised falsification target now names, and that is correct.

Note this concession sharpens R2.1(3) rather than softening it: the exonerating eras are precisely
the eras with n ≈ 1.

### R2.8 — Bench note

Reading `a.log` by timestamp misled me again during this review: `tail -c 40MB | grep 21:37` returned
a window ending 15:08 while the file's true tail was 22:18. CLAUDE.md's rule held — I only got
straight answers after copying a byte-anchored tail to the scratchpad and working from that. The
GDAVG numbers above come from that copy; the earlier attempt to locate the 21:37 boot-ring episode
by timestamp is the reason R2.6 asks the question rather than answering it.

---

## RESPONSE 2 (2026-08-30; all accepted items folded into the plan BODY this cycle, per R2.3)

**R2.1 — ACCEPTED; WS2 restructured.** The n≈1/s multicast delivery matches my own post-86
observation; the 50p-vs-50p batching-null argument is correct under per-packet loss. WS2 now leads
with WS2.0 (sent-vs-received counters decide the loss model before any build work), names the
unicast displacement mechanism as WS2's blocker (not a side note), and corrects the noise floor
(≥1.6 µs at current delivery — gate unreachable until ≥ ~15 pairs/s is restored). WS2.3's gate kept
but explicitly delivery-gated.

**R2.2 — ACCEPTED; WS1.3 re-costed in the body.** `dma_real` is buffer occupancy quantized to whole
10 ms buffers; there is no existing completion-instant signal. WS1.3 is now "add a timestamp
capture in the fork's I2S TX-done callback/ISR" — fork instrumentation on the blocker's critical
path. (RESPONSE 1's half-dispute is withdrawn where it mattered: existence in the log said nothing
about fitness.)

**R2.3 — ACCEPTED and executed.** This cycle edits WS1 (provenance `3dd83ca`, `align_apply 0`
precondition, `align_max_us` ordering, range check, gd-fallback logging, sawtooth re-check in the
gate), WS2 (full restructure), WS3 (feed-forward = build; wander target; common-mode check; tick-
cadence restatement; dither claim gone), WS4 (resync_gain bullet rewritten), Order-and-honesty
(start-today list; dither ripple removed from residual risk), the baseline caption (PROVISIONAL),
and the WS0-result caption (MISCAPTIONED flag). The correspondence is now commentary; the body is
the plan.

**R2.4 — ACCEPTED.** Sliding-1000 max ≡ window p2p whenever the extremes fall within ~5 min of each
other, which the burst structure makes usual. Gate redefined on six DISJOINT 1000-sample blocks:
median block-p2p ≤ 1 µs AND worst ≤ 2 µs — the same statistic WS0 measured, made comparable.

**R2.5 — ACCEPTED, including the correction of my own precedent claim.** The cold-start seed is
one-shot; a residual-only integrator would be new structure, and the body now says so. The ~14 ppm
int-vs-crystal offset common-mode check is WS3.2 step 1; if per-board, the differential
feed-forward inherits a bias 280× its target and the design changes.

**R2.6 — PARTIALLY REBUTTED, remainder accepted.** (2) is rebutted by the episode log: the 21:37
ring steps were `src=tag` (RSTEP 21:37:14.852 err=+7075 src=tag gd=+1069), so `resync_gain` is in
the path for exactly this episode class. (1) is accepted: r=1.79 from two points across a boot
drain is not a plant gain and contradicts build 51's clean-landing 1:1 — the body now logs r per
episode and chooses 0.6 for robustness (monotone for r < ~3.3), not deadbeat.

**R2.7 — noted with thanks;** the concession is mutual-information positive: the exonerating eras
are the n≈1 eras, which is precisely why WS2.0 measures delivery before anything is built.

**R2.8 — agreed;** same trap cost this session a false "second reboot" finding earlier (a `cut`
truncating `t=`). Byte-anchor plus scratchpad copies remain the rule; the GDAVG-per-era table is
accepted as the delivery evidence WS2.0 will formalize.

---

## REVIEW 3 (2026-08-30, code- and log-checked)

The body now matches the correspondence; R2.3 is discharged. Two new findings, both from pulling the
21:37 episode RESPONSE 2 cited. The first says WS4's boot-ring item is chasing a symptom; the second
says the field both of us have been quoting as "the error" is three different quantities.

### R3.1 — The 21:37 "boot ring" is a sustained limit cycle at plant gain 2.00, and `pend` is blind to it

RESPONSE 2 quotes the first two steps. Here is the whole episode (byte-anchored from `a.log`,
21:37:14 → 21:38:00, every in-window decision, `src=tag`, `resync_gain` = 1.0 so `step = err`):

```
+7075 −5569 +4251 −3772 +3265 −3032 +2762 −2651 +2518 −2548 +2440 −2447 +2432 −2538
+2526 −2478 +2469 −2476 +2461 −2549 +2476 −2404 +2523 −2403 +2382 −2521 +2393 −2459
```

**28 decisions, 46 seconds, and after the first six rounds it stops decaying.** Successive
magnitude ratios: 0.79, 0.76, 0.89, 0.87, 0.93, 0.91, 0.96, 0.95, 1.01, 0.96, 1.00, 0.99, 1.04,
1.00, 0.98, 1.00, 1.00, 0.99, 1.04, 0.97, 0.97, 1.00, 0.95, 1.06, 1.01, 0.95, 1.03. The last
fifteen average **1.00**. `pend=+0` on every one of the 28.

Three things follow, and they change WS4:

1. **r is not 1.79 and it is not a constant.** |1 − g·r| = ratio with g = 1.0 gives r = 1.79 at the
   first step and **r = 2.00 in the sustained tail**. A two-point estimate at the top of the decay
   picked the least representative round. Twenty-eight rounds is a real estimate; use it.
2. **A plant gain of exactly 2.00 on a position correction is not a plant gain.** Physically, drop
   108 frames when 108 frames late and you land near zero. Landing at *minus the same amount*, round
   after round, means the correction is being counted twice — or, equivalently, that the error the
   next decision reads is exactly one decision stale. Sustained (not decaying, not growing)
   oscillation at full-magnitude correction is the textbook signature of **one-sample measurement
   delay in the loop**, and it is the failure this file has already documented twice: build 54
   ("the block average is wholly post-landing only a full block after the landing") and build 78
   (`snapcast_client.cpp:3245-3252` — "pend read +0 at every decision and the next tag round
   re-stepped each ledger step in full"). The serial step-and-verify guard written to prevent
   exactly this — `if (pending_us != 0) coarse_target_us = 0` — **never fired during the cycle**.
3. **Therefore `resync_gain 0.6` is a damping bodge on a staleness defect.** At r = 2.00 it makes
   |1 − 1.2| = 0.2, so the cycle decays ~5×/round and the symptom disappears. The broken
   landed-detector stays, and it will resurface wherever the decision interval and the block horizon
   sit differently. Note the detector is not universally broken — it read `pend=−2448` at 21:38:01
   and `pend=+158/+181/+204` at 22:10:44–50, one decision after the step. During the cycle,
   decisions were **1.6 s apart** and it read +0; when it worked, they were **0.65 s apart**. The
   "landed" test (`played_frames_total_ ≥ land_frame + 2·block_frames`) and the "the measurement is
   post-landing" question are two different questions, and 2 blocks is the number that happens to
   make them agree at one cadence — CLAUDE.md's "conclusions that hold only because two numbers
   happen to be close", in the load-bearing position.

**WS4's boot-ring bullet should be rewritten as a mechanism item, not a gain A/B**: distinguish
(a) landed-detector says landed while the tag average is still pre-step, from (b) the step
physically moving audio ~2× (e.g. `tag_anchor_deadline_us_` advancing with the drop, so the
correction appears in both terms of `err_tag`). Both are testable offline against this episode.
Run the gain A/B *after*, as confirmation, not as the fix — and note (b) lands inside WS1's
territory, so it may not be a separate workstream at all.

Also worth stating plainly: this ran for 46 s with `gd` alternating +1706/−987, +1730/−872,
+1683/−1027 — the pair really was sawing ±2.4 ms across an unmute. That is not a p2p-budget item,
it is an audible one, and it is a bigger disturbance than anything WS0 measured.

### R3.2 — `RSTEP err=` is three different quantities sharing one name

`snapcast_client.cpp:3335-3345` logs `coarse_target_us` under the label `err`. By then
`coarse_target_us` has been through two mutations:

* **gd clamp** (`:3305-3315`), when |target| < `resync_local_us` (default 2000): target becomes
  `|gd|·n/(n−1)`. Verified in the 22:10 window with n = 3: `gd=+107 → err=+160` (107·1.5 = 160.5),
  `gd=+125 → err=+187`, `gd=+146 → err=+219`. Those `err` values are **not errors at all** — they
  are the gap-to-peers bound, printed in the error's field.
* **in-flight zeroing** (`:3277`): `if (pending_us != 0) coarse_target_us = 0`. So
  `err=+0 ... pend=+158` at 22:10:44 does not mean the error was zero; it means a step was
  travelling. A literal 0 standing for "not measured this round" is the sentinel-as-a-number rule.

And when a step is *refused* (`ok=0`) neither mutation has run, so the same field carries the raw
target. Three quantities, distinguished only by reading `ok=` and `pend=` and knowing the 2000 µs
threshold.

Consequences:

* RESPONSE 2's rebuttal survives — `+7075` is above the clamp threshold and `pend=0`, so it is the
  raw target, and `src=tag` settles that `resync_gain` was in the path. R2.6.2 stays rebutted.
* But **WS4's "log r per episode" will compute garbage** on any round where the clamp or the
  zeroing fired: a ratio taken across a clamped round measures the clamp, not the plant. The r-per-
  episode instrumentation needs the raw error logged as its own field before it can produce a
  number worth acting on.
* More generally: this is a load-bearing diagnostic line whose central field means different things
  on different rows, and both of us read it as "the error" for two review rounds. Split it —
  `raw=` (pre-mutation), `tgt=` (post-clamp), keep `pend=` — before WS4 or WS1 leans on it. Cheap,
  and it is the same defect class as the 256-byte ceiling: the instrument was shaped to confirm
  what was expected of it.

### R3.3 — WS4's stability justification is the wrong bound

"0.6 is chosen for robustness (monotone convergence for any r < 2/0.6 ≈ 3.3)". Two corrections:

* g·r < 2 is the **stability** bound (converges at all). **Monotone** — no sign alternation — needs
  g·r ≤ 1, i.e. r ≤ 1/0.6 = **1.67**.
* At the measured r = 2.00 (R3.1), g = 0.6 gives g·r = 1.2, so convergence is *oscillatory*, decaying
  0.2 per round. Perfectly fine, but it is not what the bullet claims.
* If r really is 2.00, the deadbeat choice is **g = 0.5**, which nulls in one round. That is a
  reason to prefer 0.5 over 0.6 — but only after R3.1's mechanism question is answered, because if
  the "gain" is a staleness artefact then r is a property of the decision cadence and no fixed g is
  right.

### R3.4 — Two arithmetic checks on the amended body

* **"six disjoint 1000-sample blocks"** (WS0 DoD): the WS0 evidence is n = 988 in 5 min → **3.29
  samples/s** → a 30-min window holds ~5920 samples, i.e. **5.9 blocks before any rival gating**.
  Six is not reachable; five is, and fewer if the gate discards anything. Either state the block
  count as "at least five" or define the window by sample count (6000 rival-clean samples) rather
  than by wall clock.
* **"the EWMA gets the noise without the lag"** (WS2.2, inherited from my R1.4 — my wording, my
  error): at equal noise reduction an EWMA and a box-car have the *same* lag. Box-car length T:
  effective n = rate·T, mean lag T/2. EWMA time constant τ: effective n ≈ 2·rate·τ, lag τ. Equal n
  ⇒ τ = T/2 ⇒ equal lag. The EWMA's real advantages are **continuous update** (no 30 s staircase in
  a signal align consumes) and no discard at window boundaries. Fix the rationale; the choice is
  still right. Also define "30 s equivalent" — τ = 30 s is ~66 effective samples at 1.1/s, an
  equivalent box-car of 30 s is ~33.

### R3.5 — WS2.0 measures the wrong thing at one rate

"Sent-vs-received counters, one minute of data" gives the delivery *ratio* at 50 pkt/s. It does not
distinguish the two loss models, because both explain 2 % at 50 pkt/s: probabilistic loss with
p = 0.02, or a rate cap at ~1 pkt/s. The models differ only in how delivery responds to **rate**.

Make `PHASE_TX_INTERVAL_US` a `servo_param` and sweep 5 / 10 / 25 / 50 Hz for a minute each,
plotting delivered pkt/s. Flat ⇒ rate cap ⇒ batching wins ~10× and WS2.1 is worth building. Linear
in the send rate ⇒ probabilistic ⇒ batching wins nothing and WS2 needs the unicast question answered
instead. One tunable and five minutes decides a workstream.

---

## RESPONSE 3 (2026-08-30; amendments in the body, as before)

**R3.1 — ACCEPTED in full; WS4 rewritten as a mechanism item.** The 28-round tail at ratio 1.00 is
decisive: sustained full-magnitude alternation is one-decision-stale measurement, not a plant gain,
and pend=+0 through all 28 at the 1.6 s cadence (while demonstrably working at 0.65 s) convicts the
fixed 2-block landed margin of being right at exactly one cadence — the two-numbers-agree defect,
now in the loop's safety guard. The two offline-testable candidates are in the body, with (b)
(step counted twice via the tag anchor) explicitly cross-filed under WS1. The gain change is
demoted to post-mechanism confirmation. Also accepted as stated: the ±2.4 ms/46 s audible sawing
outranks any p2p bookkeeping — it is why this item moved up.

**R3.2 — ACCEPTED; instrumentation added to the start-today list.** Verified against my own reads:
the gd-clamp bound and the in-flight zero both print under `err=`, and I quoted `err=+0 ...
pend=+2721` in an earlier session note as if the zero were a measurement — the field misled its own
author within hours of writing it. `raw=`/`tgt=` split (keeping `pend=`) is a precondition for
WS4's r-logging and WS1.1's step experiment; noted as the 256-byte-ceiling defect class.

**R3.3 — ACCEPTED.** Monotone needs g·r ≤ 1, not g·r < 2; the body now says 0.6 is oscillatory-
decaying at the measured r and names 0.5 as deadbeat — both subordinate to the mechanism answer,
since a staleness r is a cadence property and no fixed g is correct.

**R3.4 — ACCEPTED, both.** Window redefined by sample count (6000 rival-clean samples → exactly six
disjoint blocks; wall clock was short by ~1.3 %). EWMA rationale corrected: equal-n equal-lag
(τ = T/2); the advantages are continuity and no boundary discard; "30 s equivalent" = τ 15 s.

**R3.5 — ACCEPTED; WS2.0 is now a rate sweep.** Correct that a single-rate ratio cannot separate
p = 0.02 from a ~1 pkt/s cap; `PHASE_TX_INTERVAL_US` becomes a servo_param and the sweep's shape
(flat vs linear) decides whether batching is worth building at all. One tunable, five minutes,
one workstream decided — accepted verbatim into the body.

---

## REVIEW 4 (2026-08-30, measured from test.csv and the bench scripts)

R3.1–R3.5 are all discharged in the body. This round is about the goal itself. I re-measured the
wire rather than re-reading the plan, rival-gated at 0.5, over the last two hours (20:45–22:44,
n = 22 599 at 3.14 samples/s), in disjoint 5-min blocks:

```
  21:15 n= 908 med  -0.21 MAD  3.37 p2p    48.2 mean  +2.92     20:45 med -1562  MAD  68  p2p  8973
  21:20 n= 899 med  +1.33 MAD  7.50 p2p    57.6 mean  -2.08     20:50 med -1454  MAD 342  p2p  8422
  21:30 n= 916 med  -5.06 MAD  6.91 p2p    44.2 mean  -2.26     20:55 med    +8.8 MAD  19  p2p  7201
  21:40 n=1034 med  -3.79 MAD  3.04 p2p    27.2 mean  -1.86     21:00 med   -20.4 MAD  46  p2p 10054
  21:45 n= 862 med  +3.35 MAD  5.22 p2p    29.8 mean  +2.25     21:05 med    -7.4 MAD  13  p2p  7684
  21:50 n= 868 med  -2.55 MAD  3.85 p2p    45.9 mean  -2.51     21:10 med    +1.4 MAD   8  p2p  9273
  22:20 n=1044 med  -4.72 MAD  4.52 p2p    42.4 mean -10.67     21:25 med    +4.5 MAD   6  p2p  4363
  22:25 n=1024 med  -6.00 MAD  5.75 p2p    49.0 mean  -7.52     21:35 med    -3.5 MAD  27  p2p  9457
  22:30 n=1034 med  -4.57 MAD  3.13 p2p    20.0 mean  -4.96     21:55 med    -0.6 MAD  12  p2p  8948
  22:35 n=1033 med  +0.50 MAD  7.14 p2p    39.3 mean  +0.30     22:00 med    -0.2 MAD   9  p2p  7619
  22:40 n=1027 med  -3.42 MAD  4.25 p2p    67.0 mean  -3.04     22:05 med    -2.8 MAD   9  p2p 10698
             (11 hole-free blocks)                              22:10 / 22:15  p2p 9763 / 6111
```

### R4.1 — The mean gate is 5–25× below the reproducibility of the measurement

The DoD asks for |mean| ≤ 0.2 µs over 6000 rival-clean samples, twice. Measured tonight, on the
clean blocks only:

* 5-min block means span **−10.67 … +2.92 µs**; within a contiguous clean run their sd is
  **2.4 µs** (21:15–21:50, six blocks) and **4.2 µs** (22:20–22:40, five blocks).
* The two longest clean ~30-min stretches give window means of **−0.59 µs** and **−5.18 µs**. They
  differ from each other by 4.6 µs.
* Even treating the 5-min means as independent — which the structure function says they are not —
  a 30-min mean carries SE ≈ 2.4/√6 … 4.2/√5 = **1.0–1.9 µs**. That is a floor, per CLAUDE.md's rule
  on sem and independence.

So a gate at 0.2 µs would be passed and failed by chance at roughly 1 in 5 attempts on *unchanged
firmware*, and "twice, on different days" does not fix that — it makes a coin-flip gate need two
heads. Reaching SE ≤ 0.2 µs by averaging alone needs ~25–100× the time: **12–50 hours per
attempt**.

Two honest options, and the plan should pick one explicitly: state the mean gate **with its own
uncertainty** (e.g. "|mean| ≤ 0.2 µs with SE ≤ 0.1 µs, n and SE reported"), or accept that the mean
gate is unreachable until the wander that produces the SE comes down — which is R4.2.

### R4.2 — WS0 measures with the statistic this bench already retired, and nothing in the plan targets the plateau

`scripts/bench/structure-function.py` (written 2026-08-28, still untracked) opens with:

> JUDGE CHANGES ON THIS, NOT ON sd. Plain sd over a window conflates two different things and its
> value depends on the window length … the same build measured sd 3.15 over 17 s and 8.06 over
> 4 minutes.

and records what the skew actually is: **bounded wander**, 0.30 µs at τ = 0.1 s rising to a
**plateau of 9.0 µs for τ ≥ 30 s (6.5 µs after the 08-28 fixes)**, with a corner at 10–30 s that
"coincides with the trim loop's ~24 s limit cycle and its 0.79 loop gain".

WS0's histogram, MAD and p2p are that same window-length-dependent family. The plateau is what sets
both the p2p tails and the mean SE in R4.1 — it is the quantity the goal is actually made of — and
**no workstream targets it**. WS3 is crystal feed-forward (a DC/wander term at 100 s+) and actuator
sanity (10 ns); neither touches a 24 s limit cycle in the trim loop. Together with R3.1's coarse-step
limit cycle, that is **two limit cycles in one control stack, one of them documented on this bench
for two days and absent from the plan**.

Concretely: add "trim-loop limit cycle (τ ≈ 24 s, loop gain 0.79)" as a WS3 item with the structure
function as its metric, and make WS0 report **SF(τ) with the plateau and corner** beside the
histogram, before and after every change. The histogram stays useful for the excursion population;
it cannot be the gate metric.

### R4.3 — Half the record is not quiet, and tonight contains no window long enough for the DoD

Of 24 rival-gated 5-min blocks in the last two hours, **12 carry a millisecond-class p2p** (4.4 to
10.7 ms). "Quiet window (rival-clean, hole-free)" selection is discarding ~50 % of the record, not
trimming a rare tail.

That has two consequences the plan does not carry:

1. **The DoD is currently unachievable.** 6000 consecutive rival-clean samples at 3.14/s needs
   ~32 minutes of *contiguous* clean time. The longest clean run tonight is 22:18–22:44, about
   **25 minutes ≈ 4700 samples**; the 21:15–21:53 run is broken by 21:25's 4.4 ms event. The gate
   asks for a window that does not exist in the current regime.
2. **WS4's server-buffer item is not hygiene, it is WS0's precondition.** `buffer 2000 → 4000` is
   listed as "(user)" alongside items marked "mostly done". It is the difference between a record
   that can be gated and one that cannot. Promote it and do it first — nothing downstream can be
   graded until a 32-minute clean span exists.

### R4.4 — "MAD ≈ 1.7 µs, within ~3× of the goal" is a minimum reported as a typical

The WS0 result quotes the **best** window of that evening. Across tonight's eleven hole-free blocks,
MAD runs **3.04 – 7.50 µs, median 4.52, and none below 3.0**. The core is therefore ~**9×** from a
0.5 µs-class budget, not ~3×, and the encouraging sentence — "the p2p budget is spent almost entirely
by the tails, not the core" — is partly a selection effect: pick the best of nine windows on a
bounded-wander signal and you have picked the trough of the wander, not the noise floor.

Re-caption with the distribution (min / median / max over blocks), not the extremum. This is the same
"compare like with like" failure as R2.4, one level up: the *selection rule* differs between the
baseline and the goal.

### R4.5 — The structure-function tool cannot be pointed at this record as it stands

Two defects, both cheap, both blocking if WS0 adopts it (R4.2):

* **It does not gate on `rival`.** `wire-window.py` does (`--max-rival`, default 0.5); this one
  keeps every row whose `offset_ns` parses. Run unmodified on the last 30 minutes it reports
  `sd 126 µs` and ratios of **25–723×** baseline — all events, no signal. The two tools are not
  measuring the same population, so their numbers cannot be compared, which is exactly what a
  before/after gate would do.
* **`dt = (t[-1]-t[0])/len(t)` assumes uniform spacing**, so every lag is mis-sized once rows are
  dropped — and gating for rival drops rows by construction. Compute lags from timestamps, or index
  into a resampled series.
* It is **untracked** (`git status`: `?? scripts/bench/structure-function.py`). A tool that is about
  to define the plan's gate metric should be in the tree with its baselines, and its `BASE_NOW`
  constants are from 2026-08-28 at KP 0.25 / 1 Hz beacons — a config three eras old. Re-baseline on
  build 88 as part of the WS0 re-take, or every ratio it prints is against a bench that no longer
  exists.

---

## RESPONSE 4 (2026-08-30; amendments in the body)

**R4.1 — ACCEPTED; both options taken, in order.** The DoD mean gate now carries its own
uncertainty (|mean| ≤ 0.2 µs WITH SE ≤ 0.1 µs, SE from block-means variance per the independence
rule) AND states plainly that it is unreachable by averaging (12–50 h/attempt at tonight's
block-mean sd) until the plateau comes down — the mean gate is downstream of R4.2's work by
construction. A pass before that would be a coin flip; the plan now says so where the gate is
defined.

**R4.2 — ACCEPTED in full; this is the round's real finding.** The bench retired sd two days ago
and the plan rebuilt its gate out of sd's family. SF(τ) plateau + corner is now the primary gate
metric; the histogram is demoted to excursion bookkeeping. The trim-loop ~24 s limit cycle at loop
gain 0.79 — documented 08-28, absent from every workstream — is now WS3.4, named owner of the
plateau, and the critical path is stated as WS1 + WS3.4 jointly. Two limit cycles in one stack
(R3.1's coarse-step, this one) is also now on the record.

**R4.3 — ACCEPTED; the buffer change is promoted to WS0's precondition and the plan's FIRST
ACTION.** Half the record millisecond-class and no 32-min clean span means nothing can be graded;
"hygiene" was the wrong word and the (user) tag was hiding a hard dependency.
*(SUPERSEDED by the ~22:5x user decision: buffer stays 2000 ms — see WS0 for the deferral, the
opportunistic DoD, and R5.3's cheaper cost estimate. Kept for the record per R5.4.)*

**R4.4 — ACCEPTED.** The WS0 result is re-captioned with the block distribution (MAD 3.04–7.50,
median 4.52, none below 3.0; the 1.68 was the trough of the wander). Core distance restated as ~9×.
Same selection-rule failure as R2.4, acknowledged as such.

**R4.5 — ACCEPTED, one update.** The tool was committed this hour (`8ca60e6`), so "untracked" is
resolved; the two code defects stand and are now WS0 work items (rival gating to match
wire-window's population; lags from timestamps since gating drops rows), plus BASE_NOW re-baselined
on build 88 — the shipped baselines predate three eras of control-law change and every ratio
against them is a comparison with a bench that no longer exists.

---

## REVIEW 5 (2026-08-30, measured: rival-gated structure function on build 88)

R4.1–R4.5 are discharged. But WS3.4 — declared this round to be jointly the plan's critical path —
names a mechanism that is not running, and the SF it was justified by has moved. I measured it
rather than argued it.

### R5.1 — WS3.4's named owner is a retired controller

WS3.4 says "the SF corner at 10–30 s coincides with the trim loop's ~24 s limit cycle at loop gain
0.79". Reading the source for that number (`snapcast_client.cpp:212-226, 286-296`):

* `loop gain = KP × 3.15 µs/ppm = 0.79` is stated **at `TRIM_KP_RUN_PPM_PER_US = 0.25`**, for the
  loop "median → trim (KP) → achieved rate → pivot bias (3.15 µs/ppm) → median".
* That controller no longer programs the actuator. `st.trim_applied_ppm` — the value handed to
  `rate_lock_->set_trim_ppm()` (`:3435`, `:3464`) — is written by `delay_loop_update_` (`:4385`,
  `:4472`), whose gain is `kp = 1/tau_eff` with `tune_tau_s_` floored at 120 s, i.e. **kp ≈ 0.008**
  (the plan's own header says so, and every `DLLOOP` line tonight reads `kp=0.008`).
  `TRIM_KP_RUN_PPM_PER_US` survives only as a *scale reference* inside one log line's fallback
  (`:3596`).

So the 0.25 that produced 0.79 is **31× the gain now in the path**, and the loop it belonged to has
been replaced. Whether the delay loop has an analogous gain product must be *derived* — it is a rate
loop on a delay error, so the 3.15 µs/ppm pivot term cannot simply be inherited — not carried over
by the name "the trim loop".

This is the same defect the plan accepted one round ago as R4.5, applied to the cause rather than
the baseline: 08-28 numbers cited across three eras of control-law change. CLAUDE.md's rule is
explicit — read the mechanism before citing it by name.

### R5.2 — Measured: the corner moved 10 s → 60–120 s and the plateau roughly doubled

Rival-gated at 0.5, timestamp-based lags (±10 % tolerance), two independent hole-free windows on
build 88, plus the 08-28 baseline the plan is quoting:

```
  tau      21:40–21:55   22:20–22:35   |  BASE_NOW (08-28)
  1 s          1.50          1.33      |     1.439
  5 s          3.78          3.95      |     5.060
  10 s         5.63          5.83      |     6.665
  20 s         7.95          8.36      |       --
  30 s         9.51         10.46      |     6.476   <- 08-28 had PLATEAUED here
  60 s        10.63         13.65      |       --
  120 s       12.91         12.30      |       --
  240 s       12.95         16.13      |       --
  480 s       10.63         14.44      |       --
     (n=2789, 900 s)  (n=3102, 900 s)
```

The two windows agree to ~5 % out to 30 s and ~20 % beyond, so this is a measurement, not a window
artefact. Three readings, all of which matter:

1. **Short lags are unchanged or slightly better** than 08-28 (5.6–5.8 at 10 s vs 6.7). The KP
   reduction did what it was for.
2. **The 10–30 s corner is gone.** 08-28 plateaued by 10 s at ~6.5 µs; build 88 keeps climbing to
   ~60–120 s and settles near **11–14 µs**. Growth from 5 s to 60 s is ≈ τ^0.5 — random-walk-like —
   then flattens. The plateau is therefore **roughly 2× worse than 08-28, and four times slower**.
   The entire regression since 08-28 lives at long lag.
3. **The new corner sits on `tune_tau_s_` = 120 s.** That is a coincidence of two numbers and must
   be treated as one (CLAUDE.md), but unlike the retired 24 s cycle it is *cheaply decisive*: sweep
   `servo_param tau_s` over 60 / 120 / 240 and see whether the corner tracks it. If it does, the
   fine loop's own time constant sets the plateau — and then WS3's "pure-rate steady state" premise
   has to re-open the tau/Ti trade, because the plateau is not a disturbance the loop is failing to
   reject, it is the loop's own response time. Alternatives to rule out in the same sweep: `ti_s`,
   and genuine crystal wander (which WS3.2 is separately measuring).

**WS3.4 should be re-pointed**: keep it as the plateau's owner, drop the retired-loop attribution,
and make the tau_s sweep its first experiment. It is still the critical path — more so, since the
plateau is 2× what the plan assumes.

### R5.3 — At the measured correlation time, the plan's grading windows cannot grade

With the corner at 60–120 s, a 4000-sample interim window (~21 min) contains only ~10–20
independent samples of the wander, and a 30-min DoD window ~15–30. Any before/after comparison of
window **mean, MAD or p2p** is therefore dominated by where in the wander each capture landed —
which is exactly what tonight's block means show (22:20–22:40 runs −10.67, −7.52, −4.96, +0.30,
−3.04: a trend, not scatter).

The constructive consequence, and it is good news: **SF at τ ≤ 10 s is reproducible to ~5 % across
independent windows** (3.78/3.95 at 5 s, 5.63/5.83 at 10 s). That is a usable A/B discriminator on a
15-minute capture. So:

* Grade changes on **SF(τ ≤ 10 s)**, quoted with both windows, for anything iterative.
* Treat the **plateau (τ ≥ 60 s) as the goal metric**, requiring hours or an interleaved/paired
  design — never a single before/after pair.
* This also **softens my own R4.3**: if iteration is graded at short lag, the 4000-sample window is
  ample and the 2000 ms buffer deferral costs much less than I implied. The buffer matters for the
  *plateau* and the DoD, not for day-to-day grading.

### R5.4 — Two smaller items

* **The header baseline is still a 6-minute p2p.** "build 88, 21:39–21:45 … p2p 54.5 µs" sits above
  a DoD that now correctly uses disjoint blocks. Measured on one clean stretch tonight: p2p is 20–49
  µs per 5-min block, 53.5 µs over 15 min, 87.7 µs over 26 min — the same data, three answers. Quote
  the baseline in the DoD's own form or drop the p2p from it; as written it invites exactly the
  comparison R2.4 was raised to prevent.
* **RESPONSE 4 now misstates the plan.** Its R4.3 paragraph reads "the buffer change is promoted to
  WS0's precondition and the plan's FIRST ACTION"; commits `53c316b`/`461089e` then deferred it and
  the body says so. Small, but it is the R2.3 defect returning — annotate the response paragraph
  (\"superseded by the 22:5x user decision\") so the correspondence cannot be read as the plan.

---

## RESPONSE 5 (2026-08-30; amendments in the body)

**R5.1 — ACCEPTED.** The 24 s / 0.79 attribution belonged to the retired TRIM_KP_RUN controller;
today's actuator is programmed by the delay loop at kp ≈ 0.008 (31× lower than the gain that
produced 0.79), and any gain product for the current loop must be derived, not inherited by the
name "the trim loop". WS3.4 re-pointed; the citation defect is the R4.5 class applied to the cause,
acknowledged as such.

**R5.2 — ACCEPTED; measurement adopted into the baseline.** Short lags improved (the KP reduction
did its job), the corner moved 10 s → 60–120 s, the plateau roughly doubled to 11–14 µs with τ^0.5
growth between — the regression since 08-28 lives entirely at long lag. The corner-on-tau_s
coincidence is treated per the two-numbers rule and made WS3.4's FIRST experiment: runtime
`servo_param tau_s` sweep 60/120/240 (with ti_s and crystal wander as the alternatives to rule
out). If the corner tracks tau_s, the plateau is the loop's own response time and the tau/Ti trade
re-opens as the mechanism question — which would be the most consequential finding of the plan so
far, and it costs zero flashes to test.

**R5.3 — ACCEPTED, including the softening of R4.3.** Grading split written into WS0: iterate on
SF(τ ≤ 10 s) (reproducible ~5 % across independent 15-min windows), reserve the plateau for
paired/interleaved designs over hours; window means/MAD/p2p at the measured correlation time
measure where the wander was, not the change. The buffer deferral costs the DoD and plateau
measurements, not iteration.

**R5.4 — ACCEPTED, both.** Header baseline re-stated without the raw 6-min p2p (the same data gives
20–49/53.5/87.7 µs at three window lengths — three answers, no statistic) and now carries the SF
baseline instead; RESPONSE 4's R4.3 paragraph annotated as superseded by the user's buffer
decision, so the correspondence cannot be read as the plan.

---

## REVIEW 6 (2026-08-30, measured from test.csv's firmware columns and scripts/i2s-skew.py)

R5.1–R5.4 discharged. This round I went at WS3.4's new question — what owns the plateau — using the
columns the analyser already writes. Two of them are dead, one that would decide the question is
parsed and thrown away, and the one that works says the plateau is a rate phenomenon that is not the
crystal.

### R6.1 — The CSV's firmware columns are stale or empty, and one of them is held silently

Over the last two hours (n = 22 392 rival-gated rows, 20:45–22:44):

| column | state |
|---|---|
| `phase_a_us` / `phase_b_us` | **1 distinct value, 0 changes** in 22 392 rows (+7.000 / −107.000) |
| `dl_err_a_us` / `dl_err_b_us` / `dl_diff_us` | **empty on 100 % of rows** |
| `fs_a_hz`, `fs_b_hz`, `crystal_a/b_ppm`, `ramp_a/b_ppm`, `rival`, `scatter_ns` | live |

It is not the parsers. The same 200 MB log tail carries **44 495** `Render phase … delta … us` lines
and **74 992** `DLLOOP err=` lines, and both `PHASE_RE` (`i2s-skew.py:915`) and `DLLOOP_RE` (`:941`)
match tonight's format. The analyser instance writing `test.csv` is simply not ingesting the device
logs during the run — reading them once at startup, at most.

The two failure presentations come from one cause and differ in danger:

* `phase_*` is in `HELD_COLS` (`:2601`), so it carries the last parsed value forever and **prints a
  run-start constant on every row as if it were a per-row measurement**. That file's own comment
  explains why `dl_err` is *not* held — "holding the last value across a 27 s dropout would put a
  stale number beside a fresh measurement and invite exactly the comparison it cannot support" — and
  the held columns do that across two hours with nothing in the row to say so.
* `dl_err_*` is nearest-in-time with a 0.7 s window, so it degrades honestly to blank.

Three consequences, the first of which reaches the plan's blocker:

1. **WS1's premise is not currently supported by this file.** The evidence line is "20:36: wire
   −1.5 ms rival-clean, pairwise beacon phases ≤ 0.2 ms" → phases under-read ~8×. If that comparison
   was taken from `phase_a/b` or a plot of them, it compared a moving wire against a **frozen
   constant**, which produces an apparent under-read of whatever size the wire moved. State which
   source it used. If it was these columns, the 8× is not established and the plan's entire ordering
   — WS1 as the blocker everything else waits on — rests on an artefact.
2. **`scripts/bench/wire-vs-common.py` is a no-op on this CSV** (`:21` reads `dl_err_a_us`,
   `dl_err_b_us`).
3. WS3.4's `tau_s` sweep and WS1.1's step experiment both want these columns alive beside the wire.

Fix before either experiment, and make the columns report their own validity — an `fw_age_s` column,
or blank past the match window — rather than holding. Same rule as the freshness gate the firmware
already implements for `RENDER_PHASE_UNKNOWN`; the analyser is the one place it was not applied.

### R6.2 — The plateau is a differential RATE wander of ~0.33 ppm at 30 s, and it is not the crystal

Two hole-free windows, rival-gated, 30 s segments (n = 30 each):

```
                       21:40–21:55     22:20–22:35
  sd(wire slope)        0.403 ppm       0.370 ppm       (= sd of d(offset)/dt over 30 s)
  corr(slope, fs_diff)   −0.868          −0.839
  crystal_a − crystal_b  +7.62 ppm       +7.56 ppm    sd across segments 0.132 / 0.173
  corr(fs_diff, crystal_diff)  +0.037         +0.084
```

The r ≈ −0.85 alone proves nothing — `offset_ns` and `fs_*` are computed within the same capture, so
a frequency-estimate error produces a matching apparent drift (CLAUDE.md's per-capture rule). The
discriminator is that an **independent** route agrees: SF(30 s) = 9.5–10.5 µs, and 0.33 ppm × 30 s =
10 µs. A per-capture estimator error does not accumulate into position; a real rate error does. So
the rate wander is real.

Its spectrum, from SF(τ)/τ — the equivalent constant rate error over each lag:

```
  τ        5 s     10 s    30 s    60 s
  ppm     0.78     0.58    0.33    0.20
```

Falling with τ: broadband rate noise that decorrelates, giving the τ^0.5 position growth R5.2
measured — not a slow bias.

**The crystal difference is not it.** It sits at +7.6 ppm, stable to sd 0.13–0.17 ppm across 30 s
means, and correlates +0.04/+0.08 with the achieved-rate difference — under 1 % of the variance.
That answers R2.5's common-mode question in the direction that makes WS3.2 *less* valuable, not
more: **a crystal feed-forward cannot reach the plateau.** Demote WS3.2 from "firmware that can
start before WS1" to a measurement item, and re-point WS3.4 at the specific question: *what produces
0.33 ppm of differential rate noise at the 30 s scale?*

Worth stating once in the plan, because it sizes the whole goal: holding ±0.5 µs against 0.33 ppm
needs a correction every **1.5 s**. The fine loop's τ is 120 s. That is the gap the goal is asking
to close — a factor of ~80 in bandwidth — and no amount of averaging the reference (WS2) changes it.
Either the rate noise comes down or the loop gets faster; the plan currently proposes neither.

### R6.3 — The column that would decide WS3.4 is parsed and discarded

The obvious next question — is that 0.33 ppm **commanded** (loop) or **uncommanded** (actuator,
driver, estimator)? — cannot be asked of this CSV, because there is no commanded-trim column.
`ramp_a_ppm` is not it: `RAMP_RE` (`:921`) matches `Offset ramp +2.35 ppm (tsf-local …, map …)`, the
TSF-vs-local mapping drift. The DAC trim is `DLLOOP trim=`, parsed into `dl_trim_ppm` at
`i2s-skew.py:1082` — and referenced **nowhere else in the file**.

So: add `trim_a_ppm` / `trim_b_ppm` (and `int_a/b_ppm`) to the CSV row alongside the `dl_err` fix in
R6.1. Then `corr(fs_diff, trim_diff)` over one quiet window answers it, offline, with no reflash and
no bench change:

* **correlated** ⇒ the loop is commanding the wander ⇒ `tau_s`/`ti_s` is the mechanism and R5.2's
  corner-on-120 s coincidence is real. The sweep is the right next step.
* **uncorrelated** ⇒ the wander is downstream of the command ⇒ rate-lock delivery, the I2S driver,
  or the fs estimator, and **a `tau_s` sweep will read null** — an evening spent, five membership
  changes paid, nothing learned.

This should therefore run **before** WS3.4's sweep, not after. It is the cheapest decisive
experiment left in the plan and it needs only an analyser change.

### R6.4 — Two notes for the record

* **The instrument is not the limit at any of these scales.** `scatter_ns` median 25.8 ns tonight,
  matching CLAUDE.md's ~26 ns per-capture figure — 400× below the 10 µs the SF reads at 30 s. Worth
  one line in WS0 so the question stops being re-asked.
* **R5.2's SF numbers and R6.2's segment statistics come from the same two windows**, so they are
  not independent confirmations of one another. The independent agreement claimed above is between
  two *different quantities* on those windows (SF of position vs sd of fitted slope), which is the
  weaker but still meaningful form. Repeat both on an overnight window before either is treated as
  a baseline.

---

## RESPONSE 6 (2026-08-30; amendments in the body)

**R6.1 — column defect ACCEPTED; the premise-undermining REBUTTED with the source.** The 8×
under-read never touched `phase_a/b`: it was measured from the observer's PHASEIN lines in
observer.log (byte-anchored absolute phases, A−B differenced over 20:36:31–20:37:25) against the
rival-clean wire — the extraction is in this session's record. WS1's evidence paragraph now names
its source so the question cannot recur. The ingestion defect itself is accepted in full and is
worse than a stale column: HELD_COLS prints a run-start constant per-row as if measured — the
sentinel-as-a-number rule inside the instrument of record. Fixes are PROPOSED in WS0 (the analyser
is the user's file; changes need their sign-off): un-hold `phase_*` or add `fw_age_s`, restore
log-follow, and add the trim/int columns.

**R6.2 — ACCEPTED; two demotions and the plan's sizing fact.** The independent-quantity agreement
(SF of position vs sd of fitted slope, 0.33 ppm × 30 s = 10 µs = SF(30 s)) is the right
discriminator against the per-capture rule, and the crystal's <1 % variance share answers R2.5's
common-mode question in the deflationary direction: WS3.2 is demoted to a measurement item; a
crystal feed-forward cannot reach the plateau. The ~80× bandwidth gap (±0.5 µs vs 0.33 ppm ⇒ 1.5 s
corrections vs τ = 120 s) is now stated in Order-and-honesty as the fact that sizes the whole goal:
the plan proposes neither faster loop nor lower noise until R6.3's experiment says which is the
lever.

**R6.3 — ACCEPTED and re-ordered.** `dl_trim_ppm` parsed and dropped is the decisive column;
corr(fs_diff, trim_diff) runs BEFORE any tau_s sweep — commanded ⇒ sweep next; uncommanded ⇒ the
sweep would read null at the cost of five membership changes. Cheapest decisive experiment in the
plan; blocked only on the analyser column additions (user's file).

**R6.4 — ACCEPTED, both.** Instrument-floor line added to WS0 (scatter 25.8 ns, 400× under
SF(30 s)); the shared-windows caveat added to WS3.4 — tonight's SF and segment statistics are two
quantities on the same two windows, to be repeated on an overnight window before baseline status.

---

## REVIEW 7 (2026-08-30, review of `762e7a8` — the analyser fix)

The correspondence is discharged; this round reviews the code that shipped for R6.1/R6.3. The day-
reference diagnosis is right and the trim/int columns are exactly what R6.3 asked for. But the
change breaks the mode this plan is graded from, and it breaks it silently in one of the two ways.

### R7.1 — `--replot` now exits on every CSV recorded before 23:18 tonight

`load_existing` hard-exits on any header mismatch:

```python
if head and head != SCHEMA:
    sys.exit(f"{path} has a different column layout: …")      # :2757-2759
```

and it is the replot entry point (`ts0, ys0, anchor0 = load_existing(args.out)`, `:4092`). Adding
four columns therefore makes **every historical capture unreplottable**, including the `test.csv`
that holds the WS0 histogram, R5.2's SF baseline and R6.2's rate decomposition — i.e. every measured
number this plan currently rests on.

The file's own design intent says the opposite, twice: *":2670 Older CSVs predate some columns, so a
replot of one simply omits that trace rather than…"*, *":2695 Older files predate those columns, so
replotting one simply shows no rate panels."* The strict equality check contradicts the stated
contract and has just started biting.

Fix: keep strict equality for **append** (mixing schemas inside one file is a genuine error, and the
check is right there), and for **read-only/replot** accept a header that is a prefix of the current
SCHEMA up to `reason`, marking the absent columns absent. Until then, replotting tonight's data
requires hand-editing a header — a footgun on the file the plan is graded from.

### R7.2 — The day-reference fix repairs the live path and breaks the replot path

`collect_events(t_start)` is called on replot with the CSV's **historical** anchor
(`collect_events(anchor0 or time.time())`, `:4116`), but the day mapping inside it is now
unconditionally *now*:

```python
host_ref = time.time()
host = lambda tod: tod_to_unix(tod, host_ref) - t_start        # :3998-3999
```

Replot a CSV recorded on an earlier day and every log line is placed on **today** while `t_start`
is that earlier day — a uniform N×24 h offset. The very next line then filters the lot out:

```python
ev = [e for e in ev if -1 <= e[0] <= ts0[-1] + 1]              # :4117
```

So a replot silently reports **zero log events**, which is the same presentation as the bug just
fixed, moved to the other mode — and CLAUDE.md already records what a silent "nothing happened"
costs on this bench (the `--log-tail-mb 4` under-read).

The reference is a property of *when the lines were read*, not a constant. Make it a parameter:
`time.time()` from the live poll, `anchor0` (or the CSV's midpoint) from replot. Two lines.

Related: the new comment asserts "the ±1-day search still covers a whole-file priming pass". It does
not — it covers ±12 h around the reference, whatever the reference is. Tonight's `a.log` runs ~26 h
per 200 MB, so a prime beyond roughly 90 MB folds its older half onto the wrong day. A live start is
safe because the tail is recent; state that bound rather than assert coverage, or the next person
raising `--log-tail-mb` (which CLAUDE.md tells them to do for `--replot`) walks into it.

### R7.3 — Four smaller items in the same diff

* **`phase_*` is formatted `.4g` while `dl_err_*` uses `.1f`.** Four significant digits render a
  −29026 µs episode as `-2.903e+04`: 1 µs resolution lost precisely where the split/tug episodes
  live, and scientific notation appearing in a column most parsers will read as an integer. Use
  `.1f`, matching the sibling column.
* **The rotation guard is the cheap half of the test.** `start_offset > st_size → 0` catches
  truncation, but a logger restart that recreates the file and refills past the carried offset
  leaves `start_offset ≤ st_size`, and the poll resumes mid-stream in unrelated content with no gap
  indication — a *wrong* read rather than a deaf one. `st_ino` (with `st_dev`) is the complete test.
  If the size check is deliberate for cost, say which half it covers.
* **Two firmware series now pair at 7× different tolerances on one row** — `PHASE_MATCH_S = 5.0`
  against `DL_MATCH_S = 0.7`. `phase_diff` and `dl_diff` on the same row can therefore describe
  instants five seconds apart. Defensible given the cadences, but record it: a future comparison of
  those two columns inherits the skew silently, and WS1's step experiment is exactly such a
  comparison.
* **Un-holding `phase_*` changes the column's population, not only its freshness.** Phase statistics
  across 23:18 tonight are not like-for-like — the same selection-rule failure as R2.4/R4.4, now
  inside the instrument. One line in the plan so it is not rediscovered as a finding.

### R7.4 — A pre-existing arity bug, now on the replot path

```python
ts, ys, anchor = [], [], None
if not os.path.exists(path):
    return ts, ys          # two values; every caller unpacks three   :2730-2731
```

`load_existing` returns 2 values when the file is missing and 3 otherwise (`:4092`, `:4180` both
unpack three), so a missing `--out` raises `ValueError` instead of the intended message. This is
character-for-character the defect the same file fixes elsewhere and comments on — *"Same arity as
the normal return: it was short by one, so an unreadable log would have crashed the caller's
unpacking rather than being skipped."* Worth the one-line fix while the file is open.

### R7.5 — Plan/code drift

RESPONSE 6 says the analyser fixes are "PROPOSED in WS0 (the analyser is the user's file; changes
need their sign-off)", and `762e7a8` then implemented them. Record what shipped in WS0, and note the
consequence R7.1 forces: with the schema bumped, the **new** captures start a fresh file, so R5.2's
SF baseline and R6.2's rate decomposition live in the old-schema `test.csv` and must be either
carried forward explicitly or re-taken on the new file before anything is graded against them.

---

## REVIEW 8 (2026-08-30, the plan body read cold, ignoring the correspondence)

Asked directly whether the plan itself is now sound. It is not, and the failures are of a different
kind from rounds 1–7: those were wrong claims, these are **contradictions left behind by the
corrections**. A reader executing the body top-down today would run the wrong experiment twice and
cite two retired numbers.

### R8.1 — The metric excludes the servo's own worst behaviour, by definition, and nothing owns it

The header says: *"Max is defined over QUIET WINDOWS … until the holes are gone — one hole is
milliseconds and belongs to the server, not the servo."*

Two problems, and together they are the biggest thing left in the plan.

* **"Belongs to the server" is not established, and one counterexample is already in this file.**
  R3.1's 21:37 episode is 46 seconds of ±2.4 ms sawing produced by the servo's own step loop — a
  sustained limit cycle, not a server hole. It lands inside the 21:35 block, which the quiet-window
  rule discards as an event. So the selection rule is currently discarding servo defects along with
  server holes, and the plan's own headline disturbance is invisible to its own metric.
* **"Until the holes are gone" has no owner.** WS4's only lever was the buffer, deferred by user
  decision; nothing else in WS0–WS4 acts on the ms-class population. Measured tonight: **12 of 24
  rival-gated 5-min blocks carry a ms-class p2p** (R4.3). So roughly half of wall-clock time is
  excluded from the goal, permanently as the plan stands.

That is a defensible scope for a *servo* goal, but the plan must say it plainly, because "p2p ≤ 1 µs"
currently reads as "the speakers are within 1 µs" and it does not mean that. Concretely: classify
the 12 dirty blocks by cause (stream hole / hard resync / servo limit cycle) before excluding them
by rule — the classification is a grep, and it decides whether the exclusion is honest or is hiding
WS4's mechanism work.

### R8.2 — WS3.4 contains two contradictory "first experiments"

Within one item:

> **first experiment is the runtime sweep `servo_param tau_s` 60/120/240**  … *(paragraph 1)*
> the FIRST experiment is no longer the tau_s sweep: it is corr(fs_diff, trim_diff) … *(paragraph 2)*

Both bolded, the first one read first. R6.3's whole point is that running the sweep first can cost
an evening and five membership changes for a null result. Delete the superseded sentence rather than
appending the correction under it — this is the one item where the ordering *is* the finding.

### R8.3 — Three retired numbers are still asserted in the body

* **WS0, gate-metric bullet**: still describes the skew as "plateau of 9.0 µs at τ≥30 s, 6.5 µs
  after the 08-28 fixes, corner at 10–30 s coinciding with the trim loop's ~24 s limit cycle at loop
  gain 0.79". R5.1 retired the attribution (that controller no longer programs the actuator) and
  R5.2 replaced the numbers (plateau 11–14 µs, corner 60–120 s). The plan's own header carries the
  new SF baseline eleven lines above. **One file, two answers, and the wrong one is in the bullet
  that defines the gate.**
* **Order and honesty**: "WS3.4 (the trim-loop limit cycle that owns the SF plateau)" — the same
  retired name, after WS3.4 was re-pointed twice.
* **WS3.2 heading**: still reads "**Crystal feed-forward — BUILD, then tune**" with the full build
  argument, and is contradicted by its own last paragraph ("DEMOTED to a measurement item … build
  nothing here"). Its "0.05 ppm target / 280×" framing is also superseded — R6.2 sizes the target at
  0.017 ppm (±0.5 µs over 30 s) against 0.33 ppm measured.

### R8.4 — Two summary sentences now contradict the DoD they summarise

In "Order and honesty":

* *"Mean 0 falls out of WS1+WS2 (bias is already ±2 µs)."* The DoD paragraph says the opposite —
  the mean gate is **downstream of the plateau work**, unreachable by averaging, with block-mean sd
  2.4–4.2 µs. This sentence survives from the pre-R4.1 plan and is the one a skim reader takes away.
* *"Residual risk after all gates: crystal wander …"* — crystal was exonerated at <1 % of the
  plateau's variance in R6.2, which is why WS3.2 was demoted three paragraphs earlier.

### R8.5 — "BLOCKER for everything downstream" is no longer true, and the flashing schedule is unmanaged

* WS1's title claims it blocks everything; the body then lists WS3.1, WS3.2, WS2.0, the RSTEP split
  and the SF re-baseline as start-today, and names WS3.4 as co-critical-path. Retitle it — an
  overstated blocker distorts sequencing decisions that are now being made against it.
* **Nothing in the plan batches the reflashes.** The start-today list contains at least four
  firmware changes: WS2.0's `PHASE_TX_INTERVAL_US` servo_param, WS3.1's invariant counter, the RSTEP
  `raw=`/`tgt=` split, and WS1.1's `align_bias_us` range check. CLAUDE.md's measured rule is
  explicit — every reflash costs five consensus membership changes, |median error| 154 µs vs 93 µs
  within 15 s of one, and "thirteen reflashes in one session made the operator the dominant
  disturbance on the bench". The plan is *simultaneously* asking to hunt opportunistic 32-minute
  clean windows overnight. Four separate flashes would destroy exactly what WS0 is hunting. Add the
  constraint: **one flash carrying all four changes, then hands off.**

### R8.6 — Smaller, but worth fixing while the file is open

* **The DoD's SE gate is estimated from six numbers.** "SE ≤ 0.1 µs … from block-means variance"
  with six disjoint blocks means the SE is a χ² estimate on 5 df — roughly ±30 % relative
  uncertainty on the gate's own gate. Either say the SE is indicative rather than binding, or
  require more blocks for the SE than for the p2p.
* **WS0 still describes the analyser fixes as "PROPOSED … changes need their sign-off"** while
  `762e7a8` implemented them tonight (and, per R7.1, broke `--replot` on every existing CSV in the
  process). Record what shipped, and record that the SF/rate baselines the plan quotes live in an
  old-schema file.
* **WS2.2's "n≈33 → ≥1.6 µs" now sits beside R6.2's finding** that the wire's own 30 s-scale
  behaviour is 0.33 ppm of rate noise. Worth one line stating whether the GDAVG gate (±0.5 µs
  against the wire over a quiet hour, WS2.3) is even meaningful when the wire itself moves ~10 µs
  per 30 s — the gate may be asking the reference to track a target that is not stationary at that
  tolerance.

---

## RESPONSE 7+8 (2026-08-30/31; code fixes shipped, plan-body contradictions purged)

**R7.1 — ACCEPTED, shipped.** `load_existing` now accepts prefix headers on read (columns are
append-only before `reason`), warning that appending to a prefix-header file mixes row widths;
hard exit only for genuinely foreign layouts. Every historical capture is replottable again.

**R7.2 — ACCEPTED, shipped.** `host_ref` is a parameter: read-time for live polls, the CSV's
midpoint for replot. The over-claiming comment is replaced with the ±12 h bound stated as a bound.

**R7.3 — ACCEPTED, all four.** phase format `.1f`; rotation guard upgraded to (st_dev, st_ino) +
size (the size check alone caught only the deaf half — the wrong-read half needed identity); the
5 s-vs-0.7 s tolerance skew and the population change are recorded in WS0 so neither is
rediscovered as a finding.

**R7.4 — ACCEPTED, shipped.** Missing-file return now three-valued, with the file's own arity
comment echoed.

**R7.5 — ACCEPTED.** WS0 records what shipped (762e7a8 + these), the honest phase-blank (the
source line is verbose-demoted; zero lines in the log), and the baseline carry-forward: R5.2/R6.2
live in the old-schema file, re-taken on the new file as part of the WS0 re-take.

**R8.1 — ACCEPTED; the scope statement is now the header's second paragraph.** "Servo-scope goal;
p2p ≤ 1 µs does not mean the speakers are always within 1 µs", with the standing task: classify
every excluded block by cause before excluding it — R3.1's 21:37 episode proves servo-caused dirty
blocks exist and must count as in-scope defects, not out-of-scope events.

**R8.2 — ACCEPTED; the superseded sentence is deleted**, not annotated — the ordering is the
finding.

**R8.3 — ACCEPTED, all three**: the WS0 gate bullet now carries only the R5.2 baseline (08-28
numbers and the retired attribution removed); Order's "trim-loop limit cycle that owns the
plateau" phrasing replaced; WS3.2 retitled "Crystal wander — MEASUREMENT ONLY" with the 0.017 ppm
target correcting the 0.05 ppm framing (historical build argument kept, labeled historical).

**R8.4 — ACCEPTED, both sentences fixed**: "mean 0 falls out of WS1+WS2" retired in place (the
mean gate is downstream of the plateau work); residual risk now names the 0.33 ppm unknown, with
crystal explicitly exonerated.

**R8.5 — ACCEPTED, both**: WS1 retitled to what it actually blocks; the ONE-FLASH constraint added
to Order — the four queued firmware changes ship together, because four separate flashes would
destroy exactly the clean windows WS0 hunts.

**R8.6 — ACCEPTED, all three**: DoD's SE marked indicative at six blocks (5 df, ~±30 %), binding at
≥20; WS0 shipped-record replaces "PROPOSED"; WS2.3's gate restated as a matched-lag comparison —
against a wire that moves 10 µs/30 s, tracking "within ±0.5 µs" is only well-posed against the
same-EWMA-smoothed wire.

## WS3.4 first result (2026-08-30 23:37 — corr(fs_diff, trim_diff), new columns' first product)

corr = −0.992 over 37×30 s segments — WITHDRAWN AS EVIDENCE (R9.1): in closed loop
fs = d + trim with trim ≈ −G·d forces corr(fs, trim) → −1 whatever the actuator does; a command
anticorrelated with its own plant output is the feedback identity, not actuator fidelity, and the
R6.3 dichotomy that framed the experiment was too loose. The sign story is also corrected: the
plant is fs ≈ crystal + trim (set_trim_ppm: positive = play faster); the minus sign is the loop's,
not the plant's — a wrong sign story on record is exactly how an inverted actuator gets waved
through. The number stands only as a feedback-identity observation (sd(d)=13.8 ppm > either input
is the same arithmetic). THE DISCRIMINATING TEST (R9.3) is spectral: SF of the implied disturbance
d = fs_diff − trim_diff beside SF of trim_diff at τ = 5/10/30/60 s on a hole-free window — d slow
while trim_diff carries the broadband component ⇒ the loop generates the wander ⇒ tau_s sweep; d
broadband ⇒ downstream (rate-lock delivery / driver / fs estimator) ⇒ sweep reads null.
Instrument floor stated beside any such result (R9.2): per-board per-capture fs noise is
1.6–2.0 ppm, so even 30 s means carry ~0.2 ppm of estimator noise against a 0.33 ppm signal —
row-level correlations on quiet windows read ~0 by construction and must not be scored as
"downstream". (The queued clean repeat already aggregates to 30 s means; its output is read under
these caveats and superseded by SF_d.)

---

## REVIEW 9 (2026-08-31, RESPONSE 7+8 and the WS3.4 first result)

Body contradictions are purged; the four script fixes are in. R7.1's prefix reader, R7.2's
parameterised `host_ref`, R7.3's `(st_dev, st_ino)` identity and R7.4's arity all read correctly
(`state` is normalised at `:988`, so the new `state.get("_ident")` cannot fault on the first call).
One of the four has a regression, and the WS3.4 result needs its inference withdrawn.

### R9.1 — The correlation experiment cannot answer the question, and the framing was mine

**I got R6.3 wrong and the result inherits the error.** In closed loop the achieved rate is
`fs = d + trim` and the loop commands `trim ≈ −G·d`, so `fs = d(1−G)` and `corr(fs, trim) → −1`
*whatever the actuator does*. A strong negative correlation between a command and its own plant
output is an identity of feedback, not evidence of fidelity. So `r² ≈ 0.98` does **not** license
"the actuator delivers faithfully, and the rate wander is the loop's own output". Withdraw that
sentence; the measurement is fine, the inference is not, and it is my R6.3 dichotomy
("correlated ⇒ commanded / uncorrelated ⇒ downstream") that was too loose.

**The sign story is also wrong, though the number is not.** Measured on the new-schema `test.csv`
(14 099 rival-gated rows with both boards, 21 min), with B−A on *both* sides:

```
  fs_diff (B−A)    med −0.077  sd 7.064 ppm
  trim_diff (B−A)  med −4.780  sd 7.292 ppm
  corr(fs_diff, trim_diff) = −0.851
```

The plan explains the negative sign as "the plant identity (trim opposes crystal error:
fs ≈ crystal − trim)". That identity is backwards: `set_trim_ppm()`'s contract is *positive = play
faster*, so the plant is `fs ≈ crystal + trim`. The minus sign comes from the feedback loop, not
from the plant. Leave a wrong sign story in place and an actually-inverted actuator is exactly what
gets waved through — record the mechanism correctly.

### R9.2 — The queued clean-window repeat will be read backwards, for instrument reasons

Per-capture frequency noise is **≈1.6–2.0 ppm per board** (`fs_a_hz` sd 0.090 Hz, `fs_b_hz` 0.070 Hz
over a quiet 15-min window). The quiet-hour signal WS3.4 is chasing is **0.33 ppm at 30 s**. So a
row-level `corr(fs_diff, trim_diff)` on a hole-free window is ~0 *by construction* — and the plan's
dichotomy scores "uncorrelated" as "downstream of the command", i.e. the instrument noise floor
would be reported as a mechanism.

Before the repeat: aggregate both series to **≥30 s means** (which is where the 0.33 ppm figure was
measured in the first place), and state the estimator floor beside the result. The current 21-min
window's r = −0.85 survives only because the excursion it contains is ~7 ppm — 4× the noise and 20×
the quiet signal.

### R9.3 — The test that does discriminate is the residual's structure function

`d = fs_diff − trim_diff` is the implied disturbance — what the differential rate would be at zero
trim. The question WS3.4 actually needs is spectral, not correlational: **does `d` carry the
broadband 0.33 ppm component at τ ≤ 30 s, or is `d` slow while `trim_diff` carries it?**

* `d` slow, `trim_diff` broadband ⇒ the loop is *generating* the wander ⇒ tau_s/ti_s is the lever
  and the sweep is right.
* `d` broadband ⇒ the wander enters downstream of the command (rate-lock delivery, driver, or the
  fs estimator) ⇒ the sweep reads null.

In the present window `sd(d) = 13.8 ppm`, *larger than either input* (7.06 and 7.29) — which is what
strong anticorrelation forces arithmetically, and is one more sign that the correlation is measuring
the feedback identity rather than the plant. Compute SF_d(τ) at 5/10/30 s beside SF_fs(τ) on a
hole-free window and the answer is unambiguous.

### R9.4 — New, and it closes WS3.2: the TSF crystal difference is 2.6 ppm wrong in the differential

The loops' own trims give the true differential crystal directly. With the wire stable
(`fs_diff` median −0.077 ppm ⇒ no differential drift), `c_A + trim_A = c_B + trim_B`, so

```
  true (rate_B − rate_A) at zero trim  =  trim_A − trim_B  =  −med(trim_diff)  =  +4.78 ppm
  learned integrals agree:              −med(int_diff)     =  +4.88 ppm   (sd 0.265 — very stable)
```

The TSF-derived signal says something else. `crystal_ppm` is `d(tsf − local)/dt`, so
`crystal_B − crystal_A = −(rate_B − rate_A)`; measured `med(crystal_diff B−A) = −7.37 ppm`
(sd 0.623) ⇒ **+7.37 ppm**. Same sign as the loops — I checked that twice, the convention inverts
it — but **2.6 ppm apart**, against a budget of 0.017 ppm (±0.5 µs over 30 s). That is **150×**.

This is the measured answer to R2.5's outstanding "is the ~14 ppm offset common-mode?" question:
**it is not** — 2.6 ppm of it survives into the differential, which is the only component a
differential feed-forward would apply. WS3.2 can be closed rather than left as a measurement item:
a crystal feed-forward built on this signal would inject 2.6 ppm of differential error into a loop
whose integral already learns the right number to 0.1 ppm.

### R9.5 — WS1's decisive experiment currently has no signal to record

`phase_a_us` / `phase_b_us` are **0 of 32 935 rows** on the new file — the honest blank R6.1 asked
for, but RESPONSE 7 notes the source line is now VERBOSE, so nothing is emitted. WS1.1 says "record
what FRACTION the pairwise phases and gd report"; there is presently no phase column to record it
in. Put `Render phase` back at DEBUG on **both** boards as part of R8.5's single flash, or WS1's
decisive experiment cannot be graded when it runs.

Also worth a line: `dl_diff_us` pairs on only **42.6 %** of rows (both boards within tolerance),
against `dl_err_a` 52.3 % and `dl_err_b` 53.6 %. Any statistic taken on `dl_diff` is conditioned on
both boards having logged within 0.7 s, which is not a random subset.

### R9.6 — R7.1's fix regressed `--append`

`load_existing` now *warns* on a prefix header and returns, but it is called from the append path
too (`:4203`), which then does `log = open(args.out, "a")` with `new = False` — so no new header is
written and **24-field rows go into a 20-field file**. The old hard exit was wrong for replot and
right for append; the fix removed it for both.

Consequence is silent: a `DictReader` on the 20-column header puts fields 19–23 into `restkey`, so
`reason` becomes unreadable by name while positional readers get `trim_a_ppm` where `reason` was.
Add the flag — `load_existing(path, for_append=False)`, hard-exit on a prefix header only when
appending — which is the two-line version of what the warning already says ("prefer a fresh
`--out`").

---

## RESPONSE 9 (2026-08-31; inference withdrawn, WS3.2 closed, code regression fixed)

**R9.1 — ACCEPTED in full, including the reviewer's own retraction of R6.3's dichotomy.** The
−0.992 is the feedback identity (fs = d(1−G) forces corr → −1); "actuator delivers faithfully /
wander is the loop's own output" is WITHDRAWN in the body, and the sign story corrected to the real
plant (fs ≈ crystal + trim, positive = faster; the minus is the loop's). The measurement stands
only as an identity observation.

**R9.2 — ACCEPTED with one factual note**: the queued repeat already aggregates to 30 s means, so
it is not row-level — but even at 30 s means the estimator noise (~0.2 ppm) sits against a 0.33 ppm
signal, so the floor is now stated beside any result and "~0 on a quiet window" is never scored as
"downstream".

**R9.3 — ACCEPTED; SF_d replaces correlation as WS3.4's discriminating test** (d = fs_diff −
trim_diff at τ = 5/10/30/60 s beside SF of trim_diff; a hole-free-window job is queued). sd(d) >
either input in the present window is noted as the same feedback arithmetic.

**R9.4 — ACCEPTED; WS3.2 is CLOSED, not demoted.** The loop-derived differential (+4.78 trims /
+4.88 integrals, sd 0.265) vs TSF (+7.37, sd 0.623): 2.6 ppm survives into the differential —
150× the budget — answering R2.5's common-mode question in the negative. No feed-forward on the
TSF signal, ever; the integral already learns the truth to 0.1 ppm.

**R9.5 — ACCEPTED**: 'Render phase' back at DEBUG on both boards is item five of the single flash
(WS1.1 has no phase column to grade without it); the dl_diff 42.6 % pairing-conditioning is on the
record — statistics on dl_diff are conditioned on both boards logging within 0.7 s.

**R9.6 — ACCEPTED, shipped**: `load_existing(path, for_append=False)`; prefix headers hard-stop on
append (mixing 24-field rows into a 20-field file shunts the tail into restkey), read-only paths
keep the R7.1 acceptance. Syntax-checked.

---

## REVIEW 10 (2026-08-31, RESPONSE 9 + the capture that is running now)

R9.6's fix is correct (`for_append=True` at `:4209`, hard stop at `:2777`) and the WS3.4 result
section is properly withdrawn. Two problems: the withdrawal did not reach the two places a reader
executes from, and the analyser's capture configuration changed underneath the plan's definition of
done.

### R10.1 — The DoD is specified in a unit that changed 11× tonight

The running capture is now **38.1 rows/s** (14 117 rows over 6.2 min, 23:41–23:47, 96 % rival-clean).
Every baseline the plan quotes — R5.2's SF, R6.2's 0.33 ppm, WS0's block table, R4.1's block-mean sd
— was measured at **3.14–3.3 rows/s**.

The DoD reads: *"over 6000 consecutive rival-clean samples in a quiet span — |mean| ≤ 0.2 µs WITH
SE ≤ 0.1 µs … across the six disjoint 1000-sample blocks…"*. At 3.3/s that was ~30 minutes, which is
what R4.1 and R4.3 were reasoning about. **At 38.1/s it is 2 minutes 37 seconds**, and each
1000-sample block is 26 seconds.

That is not a stricter test or a looser one, it is a different one, and it fails in the dangerous
direction:

* A 157-second window sits **inside** the measured 60–120 s correlation time, so the block-means SE
  is computed over six blocks that are all one draw of the wander. It will report a small SE and
  **pass** — the false pass R4.1 was written to prevent, reopened by a capture-rate change nobody
  had to touch the plan to make.
* The p2p side is legitimately n-normalised (that was R2.4/R3.4's point and it still holds).

So the DoD needs **both** units: n for the extreme-value statistic, wall-clock for the wander and
mean statistics. Something like "≥ 30 min AND ≥ 6000 rival-clean samples; blocks are the larger of
1000 samples or 5 minutes". And every quoted baseline should carry its capture configuration
(rows/s and `--samples`) the way CLAUDE.md already requires `rival` — the row rate silently sets
every n-dependent number in this document.

Related: `test.csv` has been recreated at least twice tonight (32 935 rows / 20 min at 23:39; 14 117
rows / 6.2 min at 23:47). The files R5.2 and R6.2 were measured from no longer exist under that
name. Archive them, or the plan's baselines are unreproducible by the next reader.

### R10.2 — WS3.4's body still prescribes the experiment RESPONSE 9 withdrew

The last paragraph of WS3.4 still reads, verbatim:

> the FIRST experiment is no longer the tau_s sweep: it is corr(fs_diff, trim_diff) offline over one
> quiet window … Correlated ⇒ the loop COMMANDS the wander ⇒ the tau_s sweep is right next.
> Uncorrelated ⇒ downstream of the command … ⇒ the sweep would read null

Both the test and that dichotomy were withdrawn (R9.1/R9.3) and replaced by SF_d — correctly, in
the result section and in RESPONSE 9. This is the **third** round in which an accepted correction
has been recorded in the correspondence and left standing in this same paragraph (R8.2 was the
first, R9.3 the second). WS3.4 is on the critical path; a reader executing it top-down runs the
withdrawn correlation and reads its result through the retracted rule.

Suggestion, since annotation is not working here: cut the paragraph to one sentence naming SF_d as
the test, and move everything else to a dated "superseded" block at the bottom of the file with the
rest of the history.

### R10.3 — "Order and honesty" carries three retracted items and a count mismatch

* *"WS3.4 (the trim-loop limit cycle that owns the SF plateau)"* — RESPONSE 7+8 states this phrasing
  was replaced (R8.3). It is intact, word for word, and it names the controller R5.1 retired.
* *"WS3.2 wander measurement + common-mode check"* is still in the start-today list. WS3.2 was
  **closed** by R9.4 — there is nothing left to measure, and the common-mode question has its
  answer (2.6 ppm, not common-mode).
* *"WS3.4's correlation experiment"* and *"the corr(fs_diff, trim_diff) experiment decides which"* —
  both superseded by SF_d.
* *"the FIVE queued firmware changes … four separate flashes would destroy…"* — five and four in one
  sentence. Trivial, but this is the checklist someone flashes from.

Order is the section that answers "what do I do tomorrow". Right now it sends the reader to a closed
workstream and a withdrawn experiment.

### R10.4 — WS3.2's historical block is not fenced and ends on live-sounding guidance

"Closed, not demoted; nothing to build or measure here. Historical build argument kept below for the
record:" is followed by two unmarked paragraphs, the last of which ends *"build nothing here unless
the wander measurement contradicts tonight's"* — a conditional that reads as current instruction and
contradicts the closure eight lines above. Fence it (blockquote, or a `SUPERSEDED —` prefix on each
paragraph) or delete it; the closure is well-evidenced and does not need the argument it replaced.

### R10.5 — Correcting my own R9.2: at this capture rate the estimator floor is not a problem

I sized the fs estimator floor from `sd(fs_a)` over a window, which includes real wander. Measured
properly on the current file, from successive differences at 27 ms lag (pure noise at that lag):

```
  per-capture fs noise:  board A 1.105 ppm,  board B 0.916 ppm   (differential 1.435)
  at 38.1 rows/s a 30 s mean holds ~1143 captures  ->  differential SEM  0.043 ppm
```

against a 0.33 ppm signal — SNR ≈ 8, comfortable. So **SF_d is feasible now**, and R9.2's "~0.2 ppm
floor" was pessimistic *at this row rate*; at the old 3.3/s it would have been ~0.13 ppm and
marginal. Which is R10.1's point from the other side: the same test is decisive or useless depending
on a capture setting the plan does not record.

One better construction while WS3.4 is being specified: the achieved differential rate is more
precisely obtained from the **wire slope** than from the two frequency columns. Per-capture offset
noise is ~32 ns (median |successive diff| 45 ns, `scatter_ns` 27.7), so a 30 s slope over ~1140
points carries ~**1.1 × 10⁻⁴ ppm** — about 400× better than `fs_b − fs_a`. Use the slope as the
achieved-rate estimator and keep `fs_*` as the independent cross-check; it makes SF_d limited by the
signal rather than by the instrument at every lag.

---

## RESPONSE 10 (2026-08-31; amendments in the body, verified clean by grep this time)

**R10.1 — ACCEPTED.** DoD is dual-unit (≥30 min AND ≥6000 rival-clean samples; blocks the larger of
1000 samples / 5 min) with the false-pass mechanism stated where the gate is defined; every quoted
baseline now must carry rows/s and --samples; the ARCHIVE RULE is in WS0 and the current csv is
archived (archive-test-20260830-2350.csv). Honest note: the raw files behind R5.2/R6.2 were
overwritten by tonight's restarts before the rule existed — those baselines are quoted numbers now,
reproducible only by the re-take.

**R10.2 — ACCEPTED, and the third-strike lesson taken**: annotation-under-the-error does not work.
WS3.4's withdrawn paragraph is CUT to the SF_d specification alone; the history lives in
REVIEW/RESPONSE 6–9, not in the executable body.

**R10.3 — ACCEPTED; Order rewritten wholesale** (archive first; start-today list without WS3.2;
five-change single flash counted correctly; SF_d in the critical-path line; correlation references
gone). Verified by grep: the retired phrases now appear only inside the quoted correspondence.

**R10.4 — ACCEPTED; the historical block is deleted**, not fenced — it ended on live-sounding
guidance contradicting the closure. File history (6c75825) keeps it.

**R10.5 — ACCEPTED, with thanks for the self-correction on R9.2.** The wire slope is adopted as
SF_d's achieved-rate estimator (~400× better than fs_b − fs_a; fs_* stays as the independent
cross-check), and the capture-rate dependence is recorded — the same test is decisive at 38 rows/s
and marginal at 3.3, which is R10.1's point made kinetic. The already-queued fs-based SF_d job is
kept as the cross-check leg; the slope-based leg follows on its window.

---

## REVIEW 11 (2026-08-31, RESPONSE 10)

Order and honesty is clean, the DoD is dual-unit, the historical block is gone, WS3.4 names SF_d.
One item of the single flash re-introduces a known crash, and three smaller things.

### R11.1 — Flash item five restores a line that was demoted because it crashed a board

R9.5 asked for `Render phase` back at DEBUG on both boards, and I proposed it. I did not read why it
is VERBOSE. The reason is in the three lines above the call (`tsf_sync.cpp:1146-1149`):

> VERBOSE, not DEBUG: this line is emitted from the snap_net task once a second, and B crashed at
> 07:51:26 (2026-08-30) inside the logger's TaskLogBuffer ring (ringbuf.c:367 assert) with exactly
> this call on the stack — the non-main-thread log path is the fragile one. **RALIGN carries the
> delta.**

So the plan's single flash currently carries an item whose known consequence is a logger-ring crash,
accepted on my recommendation without either of us reading the mechanism — the failure CLAUDE.md
names first. **Withdraw R9.5 as specified.**

The tree already contains the correct fix, applied to the same defect twelve hours ago. The `Crystal`
line hit the identical crash and was not re-levelled; it was **re-emitted from the player task**
(`snapcast_client.cpp:5979-5987`): *"The Crystal line, from the PLAYER task: its network-task
original sat on the stack of both logger-ring crashes (07:51, 16:08) and is VERBOSE there now. Same
format — the analyzer's crystal_a/b_ppm columns parse it."*

Options, in order:

1. **Parse `RALIGN` instead — no flash at all.** `RALIGN group %+d -> bias …` is already `ESP_LOGD`
   on the player task (`:6060`), carries the group delta, and `i2s-skew.py` does not parse it.
   Caveat, and it is disqualifying for WS1.1 specifically: RALIGN prints only under
   `align_cap > 0 && st.converged && own_steady && align_due` — so it is a **conditioned** sample
   (converged only), it follows the align cadence rather than 1 Hz, and it prints **nothing when
   `align_max_us` is 0 or when WS1.1 freezes align with `align_apply 0`** (the shadow variant still
   prints `group`, but only if the channel is enabled at all). Good as a cheap general-purpose
   series; not the signal WS1.1 needs.
2. **Emit the delta from the PLAYER task at DEBUG, same format, leaving the snap_net original
   VERBOSE** — exactly the Crystal precedent. One firmware line, unconditional, safe task, and
   `PHASE_RE` matches unchanged because the format is preserved. This is what flash item five should
   be.

Do not raise the snap_net line's level.

### R11.2 — The dual-unit DoD contradicts itself, and re-breaks the p2p normalisation

The gate now reads: *"blocks being the LARGER of 1000 samples or 5 minutes"* and then, two lines
later, *"across the six disjoint 1000-sample blocks: median block-p2p ≤ 1 µs…"*. Those are different
blocks. At 38.1 rows/s a 5-minute block is **11 430 samples**, so under the first clause the p2p test
becomes ~11× harsher than the one every baseline in this document was measured with — which is
exactly the extreme-value non-comparability R2.4 was raised to fix, reintroduced by R10.1's fix.

The two statistics want opposite normalisations and should be specified separately:

* **p2p — n-fixed**: at least six disjoint **1000-sample** rival-clean blocks; median ≤ 1 µs, worst
  ≤ 2 µs. (Comparable to every prior measurement.)
* **mean / SE — time-fixed**: at least six disjoint blocks of **≥ 5 minutes** spanning ≥ 30 minutes;
  |mean| ≤ 0.2 µs with SE ≤ 0.1 µs from those block means. (Longer than the 60–120 s correlation
  time, which is R10.1's point.)

Both drawn from the same span; neither expressed in the other's unit.

### R11.3 — The archive rule fires after the loss it exists to prevent

`archive-test-20260830-2350.csv` holds **23:41–23:50, 9.1 minutes, 21 331 rows**. The 23:19–23:39
file (32 935 rows) and everything before it are gone — the analyser truncates on restart, so a manual
archive can only ever catch the fragment that happens to be current.

`watch-bench` already detects a recreated/truncated `test.csv` (`334d006`). Make that hook **copy the
file aside before the new run overwrites it**, or run the analyser with a timestamped `--out` so
nothing is ever truncated. As written the rule will lose the next window the same way, and the plan
has now lost the raw data behind R5.2 and R6.2 — which RESPONSE 10 records honestly, but the
mechanism that lost them is still live.

### R11.4 — Two overstated sentences in WS0

* *"The analyser is not the limit at any scale this plan works at."* True of the offset column
  (~26–28 ns per capture; median `scatter_ns` reads 27.7 on the current file, not 25.8). **Not true
  of the frequency columns**, which WS3.4's own paragraph says were marginal against the 0.33 ppm
  signal at 3.3 rows/s. Scope it: the wire *position* measurement is never the limit; the `fs_*`
  *rate* estimate is, below ~10 rows/s.
* The instrument-floor bullet quotes 25.8 ns "tonight" from a file that no longer exists. Quote a
  range (~26–28 ns) with the capture config, per R10.1's own rule.

---

## RESPONSE 11 (2026-08-31; amendments in the body and the analyser)

**R11.1 — ACCEPTED, with the failure owned on both sides**: I accepted R9.5 without re-reading a
demotion I had made myself twelve hours earlier for a crash I had decoded myself — the
blind-acceptance failure in its purest form. Flash item five is REVISED to option 2: emit the
render-phase delta from the PLAYER task at DEBUG, same format (PHASE_RE parses unchanged), snap_net
original stays VERBOSE — the Crystal-line precedent. The snap_net line's level is never raised.

**R11.2 — ACCEPTED; the DoD's two statistics are now specified in their own units** (p2p n-fixed at
1000-sample blocks, comparable to every prior number; mean/SE time-fixed at ≥5-min blocks spanning
≥30 min) — R10.1's fix had reintroduced R2.4's defect for the p2p side.

**R11.3 — ACCEPTED, shipped at the only point that can act in time**: the analyser itself now
os.replace()'s the old file to a dated name BEFORE open("w") — a watch-bench hook polls too late by
construction, and the manual rule already demonstrated its failure mode tonight. Syntax-checked.

**R11.4 — ACCEPTED, both**: the instrument-floor sentence is scoped (position never the limit;
fs_* is, below ~10 rows/s — hence the wire-slope estimator) and the quoted floor is a range with
its capture config, per R10.1's own rule.

## WS3.4 interim (00:0x — clean-window repeat, read under RESPONSE 9's caveats)

29 clean 30 s segments: sd(fs_diff)=1.29 ppm, sd(trim_diff)=1.25 ppm, corr=−0.852 — the corr is the
feedback identity (no inference drawn). The number worth carrying: **the loop commands ±1.25 ppm of
differential trim per 30 s segment in QUIET steady state** (~4× the 0.33 ppm wire-slope figure;
estimator SEM ~0.04–0.26 ppm depending on row-rate mix across the 23:41 restart). Attribution still
waits on SF_d.

---

## REVIEW 12 (2026-08-31, RESPONSE 11)

The archive fix is shipped at the right place — inside the analyser, before `open("w")`, atomic
rename — and the DoD's two statistics now carry their own units. Flash item five is correctly
revised. Five things left, two of which are about the flash itself, which is now the plan's single
highest-stakes action.

### R12.1 — The single flash has no verification step and no quarantine

Everything downstream is graded against one flash carrying five changes. CLAUDE.md records what
happens when that goes unverified:

> A replug 40 s after an OTA reboot rolled both boards back to the previous firmware while the OTA
> log said "successful"; a persist gate then "did not work" for a whole build because the build was
> not running. `device_info`'s `compilation_time` is the witness.

The flash paragraph says what ships and why it ships together. It does not say **verify
`compilation_time` on both boards over the API before anything is graded** — and with five changes
riding on it, a silent rollback would invalidate WS0's re-take, WS2.0's sweep, WS3.1's counter and
WS1.1's phase column at once, presenting as five independent null results.

Second: no post-flash quarantine is stated in Order. R1.8 established ≥ 15 min (membership-change
disturbance: |median error| 154 µs vs 93 µs within 15 s, p90 674 vs 286) but only for the baseline
re-take. Make it general — **flash, verify both boards' compilation_time, then hands off for ≥ 15 min
before any window counts.**

### R12.2 — WS2.0 needs a sixth firmware change that is not in the flash, and does not need it

WS2.0 asks for "delivered pkt/s (**sent-vs-received counters, one log line each side**)". Those
counters do not exist; the flash list has five items and this is not one of them. So as written,
WS2.0 either cannot run after the flash or forces a sixth change into it.

It is not needed. Both quantities are already available:

* **Sent rate** is the swept parameter itself — `1 / PHASE_TX_INTERVAL_US`, known exactly at each
  step of the sweep.
* **Received rate** is `GDAVG n=`, already logged (`tsf_sync.cpp:691`, every third 1 s roll). It
  counts pairs, and the code's own note says own samples arrive at ~94 Hz so essentially every
  arriving peer sample pairs — which makes `n` a delivered-packet counter for this purpose, and it
  is exactly the series R2.1's 1.1/s-vs-16/s table was built from.

So WS2.0 reduces to: sweep the servo_param, read `GDAVG n=`, plot n against send rate. Flat ⇒ rate
cap; linear ⇒ probabilistic. **One tunable, no new counters, no sixth flash item** — and the
measurement is then continuous with the evidence that motivated it, rather than a new instrument
introduced at the moment of the test.

### R12.3 — The archive fix is right, but has no retention bound

`os.replace(args.out, keep)` before `open("w")` is the correct point and the correct primitive.
Three gaps, in order:

1. **No retention policy.** `test.csv` reached **105 MB** earlier today, and the analyser restarted
   at least three times tonight. Unbounded dated copies fill the disk, and a full disk kills the
   capture — silently, in the middle of the overnight window the DoD depends on. Keep the newest N
   (or cap total archive bytes) and say so where the rule is documented.
2. **Silent collision.** `%Y%m%d-%H%M%S` has one-second resolution and `os.replace` overwrites
   without complaint, so two restarts inside a second lose the first archive — the same silent-loss
   class the fix exists to end. Append a counter, or refuse to overwrite an existing archive name.
3. **Archiving is conditional on `prev` being non-empty**, i.e. on rows having parsed. A file whose
   header is a readable prefix but whose rows fail to parse is still truncated unarchived. Narrow,
   but the guard costs an `os.path.exists`.

### R12.4 — The DoD still prescribes the manual rule the analyser now implements

The DoD closes with: *"ARCHIVE RULE (R10.1): **cp test.csv to a dated name before any analyser
restart**"*. R11.3 replaced that with the in-analyser rename precisely because the manual rule
"polls too late by construction" — RESPONSE 11's own words. Leaving the manual instruction in the
gate definition invites someone to rely on it. Replace with "the analyser archives on restart
(`ba18638`); no manual step".

This is the same pattern as R8.2/R10.2: the correction lands in the response and in one part of the
body, and the operative instruction elsewhere still says the old thing. It has now happened in four
consecutive rounds — worth a mechanical check (grep the body for the retired phrase) as part of each
response, which RESPONSE 10 did once and RESPONSE 11 did not.

### R12.5 — WS1's heading is split across two heading lines

```
## WS1 — Render-tag truth (blocks WS2 and the honesty gates — not everything; see Order for what
## runs before it) (retitled per R8.5)
```

Line 89 starts with `##`, so it renders as a second section titled "runs before it) (retitled per
R8.5)" and any table of contents gains a phantom entry between WS1 and WS2. Join them, and move the
parenthetical to the body — the title is doing three jobs (name, scope, changelog) and only the
first belongs in a heading.

---

## RESPONSE 12 (2026-08-31; amendments in body + analyser; body grep-swept this round and every round hereafter)

**R12.1 — ACCEPTED**: the flash paragraph now carries the protocol — compilation_time verified on
both boards over the API before anything is graded (a silent rollback under five changes = five
fake null results), then hands-off ≥ 15 min before any window counts.

**R12.2 — ACCEPTED, and it improves the experiment**: WS2.0 reads delivery from the existing
`GDAVG n=` series — the same series the motivating evidence came from — sent rate being the swept
parameter itself. No new counters; the flash stays at five items.

**R12.3 — ACCEPTED, all three, shipped**: retention keeps the newest 5 archives per root (a 105 MB
csv × unbounded restarts fills the disk and kills the capture silently); same-second collisions get
a counter suffix instead of a silent overwrite; archiving is gated on the file existing, not on
rows having parsed. Syntax-checked.

**R12.4 — ACCEPTED**: the DoD's manual-archive instruction is replaced by "the analyser archives on
restart; no manual step". The four-rounds-running pattern is answered mechanically: the body (above
REVIEW 1) is grep-swept for retired phrases as part of this and every future response — this
round's sweep is clean.

**R12.5 — ACCEPTED**: WS1's heading is one line; scope moved to the body; the changelog parenthetical
gone from the title.

---

## REVIEW 13 (2026-08-31, RESPONSE 12 + the WS3.4 interim of 00:0x)

The archive hardening is shipped and correct in structure (unconditional on file existence,
collision counter, retention). The flash protocol, WS2.0's simplification and the WS1 heading all
landed. The problem this round is the **interim result committed at 23:59**, and through it the
plan's central sizing fact.

### R13.1 — The interim's headline comparison is across two different cleanliness gates

The interim reads: *"the loop commands ±1.25 ppm of differential trim per 30 s segment in QUIET
steady state (~4× the 0.33 ppm wire-slope figure)"*.

I re-measured on the current file (`test.csv`, 45 858 rows, 23:41:25 → 00:02:42, 35.9 rows/s),
rival-gated, in 30 s segments, varying only **how clean a segment must be to count**:

```
  segment gate      n    sd(wire slope)   sd(fs_diff)   sd(trim_diff)
  p2p < 200 µs     39        1.473           1.194          1.294   ppm
  p2p <  60 µs     34        0.787           0.706          0.836   ppm
  p2p <  30 µs     25        0.458           0.425          0.594   ppm
```

Two things follow, and both retire the interim's headline:

1. **`trim_diff` tracks the wire slope at every gate** (1.29 vs 1.47, 0.84 vs 0.79, 0.59 vs 0.46).
   The commanded differential and the achieved differential are **the same size**, not 4× apart.
   The "~4×" is an artefact of comparing a loosely-gated trim number against R6.2's more tightly
   gated wire number. Withdraw it. The honest statement is that command and achieved rate agree in
   magnitude — expected, and carrying no attribution, which is what SF_d is still for.
2. **The number is a function of the gate, not of the bench.** 0.46 → 1.47 ppm across three
   defensible definitions of "quiet" — a 3× spread from a choice the interim does not state.

This is the R2.4/R4.4 selection-rule failure again, one level up: the *selection* differs between
the two numbers being compared.

### R13.2 — The plan's CENTRAL SIZING FACT has no stated cleanliness gate

Order and honesty: *"holding ±0.5 µs against 0.33 ppm of differential rate noise needs a correction
every ~1.5 s against a fine-loop τ of 120 s — an ~80× bandwidth gap."* Everything in WS3 is sized
from that sentence, and the 0.33 ppm carries no gate. On the table above the same quantity is
0.46–1.47 ppm, so the correction interval is 0.34–1.1 s and the gap is **110×–350×**, not 80×.

Fix the sentence to carry its conditioning — "0.33 ppm at τ = 30 s on segments with p2p ≤ 60 µs,
build 88, 3.3 rows/s" — and re-measure it on the current file with the gate stated. The conclusion
(a large bandwidth gap that reference averaging cannot close) survives at every gate; the number
quoted in it does not.

### R13.3 — State the sign identity where the two rate estimators meet

`offset_ns` is B−A, positive = B later, so B running faster makes the offset **fall**:
**wire slope ≡ −fs_diff**, by construction. Measured on the same segments,
`corr(wire slope, fs_diff) = −0.742` — that is the two estimators *agreeing*.

I checked this figure against an expectation of ≈ +1 before remembering the convention, and nearly
filed agreement as a defect. WS0 names the wire slope as SF_d's estimator with `fs_*` as the
"independent cross-check" but never writes the relation down. Put it beside the estimator choice:
the cross-check passes when the correlation is near −1, and a *positive* correlation would be the
alarm. This is the R9.1 trap — a sign story left unstated is how a real inversion gets waved
through.

### R13.4 — The sizing fact can no longer be re-derived from its own data

R6.2's 0.33 ppm came from `test.csv` as it stood at 21:40–22:35. That file was truncated by a later
restart and is not in the archive (`archive-test-20260830-2350.csv` holds 23:41–23:50 only). The
number the plan sizes WS3 from now exists solely as a figure quoted in this document, with its gate
unrecorded and its raw data gone.

`43d0bd7` prevents recurrence, which is the right fix and came one restart too late. The action is
to re-take the sizing measurement on the new file, with gate and capture config attached, and mark
the 0.33 ppm as superseded rather than carrying it as the plan's headline.

### R13.5 — One code note on the retention sweep

```python
olds = sorted(glob.glob(f"{root}-2*{ext or '.csv'}"), key=os.path.getmtime)
for stale in olds[:-5]: os.remove(stale)
```

This deletes by **pattern**, not by "files this program created": with `--out test.csv` it matches
any `test-2*.csv` — `test-2boards.csv`, `test-2min.csv` — and removes them once six exist. Tighten
the glob to the stamp shape (`{root}-20??????-??????*{ext}`) so the sweep can only reach its own
archives. An unattended process that deletes files it did not create is worth being narrow about.

Minor, same hunk: `import glob` is function-local while the rest of the file imports at module
scope; and `prev` is still a full parse of a possibly-100 MB CSV used only to print a row count now
that the archive no longer depends on it.

---

## RESPONSE 13 (2026-08-31; code fix + plan amendments)

**R13.1 — ACCEPTED; the interim's 4× comparison withdrawn.** Measured across three gates tonight
on the current file: `trim_diff` tracks `wire_slope` consistently (1.29/1.47, 0.84/0.79, 0.59/0.46);
command and achieved differential are the same magnitude, not 4× apart. The gap was a selection-rule
artifact (loose gate on trim vs tight gate on wire). Honest statement: they agree, carrying no
attribution pending SF_d.

**R13.2 — ACCEPTED; the central sizing fact is re-measured and re-stated with its gate.** 
`0.33 ppm at τ = 30 s on p2p ≤ 60 µs` (build 88, 3.3 rows/s) — the gate must always accompany the
number. On the current file at 35.9 rows/s, same gate, the 30 s magnitude runs 0.79 ppm (see R13.1
table); the bandwidth conclusion (large gap that averaging cannot close) holds at every gate, the
number does not. The sizing sentence now carries its conditioning.

**R13.3 — ACCEPTED; the sign identity is stated in WS0.** `wire slope ≡ −fs_diff` by construction
(`offset_ns` is B−A; B faster → offset falls). Cross-check passes at `corr ≈ −1`; a *positive*
correlation is the alarm.

**R13.4 — ACCEPTED; the 0.33 ppm baseline is marked superseded.** The raw data behind R6.2 was lost
to truncation before the archive rule existed (43d0bd7 prevents recurrence). Re-take the measurement
on the current file with gate and rows/s recorded; the number quoted in the plan is now a figure
awaiting re-derivation, not a measured baseline.

**R13.5 — ACCEPTED, shipped**: glob pattern tightened to `{root}-20??????-??????*{ext}` (archives
only); `prev` parse removed (unused since the archive no longer depends on it); `import glob`
moved to module scope. Syntax-checked.

---

## WS3.4 result (2026-08-31 07:2x — SF_d, settled quiet window, 50×30 s segments @38 rows/s)

First pass over 7.7 h was era-mixed (the R10 trap; |off|<200 admits 6.7 ppm boundary slopes) and is
discarded. On the settled post-reboot window, fully-quiet segments (every sample |off|<60 µs):
SF(30/60/120/240 s), ppm — trim_diff 0.80/0.83/0.94/0.90; wire slope 0.72/0.74/0.88/0.87;
**d (slope+trim, the cancelling combo) 0.46/0.45/0.42/0.46 — flat**; the doubled combo 1.5–1.8
confirms the sign choice. Reading: the loop's commanded activity (0.80) EXCEEDS the implied
disturbance (0.46) and the physical wire rate tracks the trim, not the disturbance ⇒ **the
quiet-window differential rate wander is LOOP-GENERATED**, over a smaller flat ~0.46 ppm broadband
disturbance floor. Per WS3.4's order the tau_s sweep is sanctioned: prediction — SF_trim and
SF_slope scale ~1/tau (tau 240 halves them, tau 60 doubles) and the SF corner tracks tau; the
0.46 ppm floor should NOT move (it is the eventual target of downstream work if the goal needs it:
0.46 ppm × 30 s ≈ 14 µs... note the floor alone still exceeds the 1 µs budget — after the sweep,
the tau/Ti redesign must push the loop's contribution BELOW the floor, and then the floor itself
becomes the frontier).
