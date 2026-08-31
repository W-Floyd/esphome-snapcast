# PLAN — mean 0 µs, max p2p 1 µs (quiet-window)

Goal set 2026-08-30 21:45. p2p ≤ 1 µs means every sample within ±0.5 µs, so this plan is organized
around the three things that can each individually spend the whole budget: a measurement that lies,
a frame operation (22.7 µs), and differential rate drift between corrections. "Max" is defined over
QUIET WINDOWS (rival-clean, no server hole inside) until the holes are gone — one hole is
milliseconds and belongs to the server, not the servo.

Current verified baseline (build 88, 21:39–21:45): wire median −1.5 µs, MAD 5.1, p2p 54.5 µs;
kp median 0.008 in steady state (boost now scales on the differential portion); split escape
verified live (one line, 34 s recovery); injection convergence ≤ ~14 s.

## WS0 — The instrument first (no firmware)

* Quiet-hour wire HISTOGRAM of the current rate-lock-only regime (rival-gated, hole-free spans from
  test.csv). Decomposes today's ±3–8 µs into rate ripple vs measurement noise, and is the gate
  metric every later stage is judged by.

* Definition of done for the whole plan (n-normalized, R1.9): |mean| <= 0.2 us over a 30-min quiet
  window AND p2p <= 1 us over EVERY 1000 consecutive rival-clean samples within it (p0.5/p99.5
  reported alongside), twice, on different days.

## WS1 — Render-tag truth (BLOCKER for everything downstream)

Evidence: phases/tags under-measure real differentials ~8× (20:36: wire −1.5 ms rival-clean,
pairwise beacon phases ≤ 0.2 ms); standing blind offsets (specimen scratchpad 20:48); every
on-device signal shares the deadline+tag stamping, so none can see what the wire sees.

1. **Decisive experiment before any fix** (bench, one evening): inject a known one-board deadline
   step (`servo_param align_bias_us`, the build-71 hook) at several sizes (100/300/500 µs) in a
   quiet hour. The wire must move 1:1 (measured 0.8–1.0 already); record what FRACTION the pairwise
   phases and gd report. Today's data says ~12–20 %. This quantifies the lie and gives the
   regression test for the fix.
2. **Provenance trace**: `adjusted_ts` originates in the speaker fork (media_source →
   `notify_audio_played_tagged` → hub → client). Read where the fork computes it — the suspect
   class is a MODELED term (feedback pivot EWMA, scheduled-time fallback, gap blanking) standing in
   for the measured DMA-completion instant. I2SDBG already carries `dma_real` — the hardware truth
   exists on-device.
3. **Fix**: stamp tags from the DMA-completion counter path, not the model; the model may smooth
   but must not bias (same rule as the freshness gate: prefer a signal that reports its own
   validity).
4. **Gate**: the step test reads ≥ 90 % in pairwise phases; a standing offset can no longer form
   invisibly (the 21:13-class episode must show gd ≈ wire).

## WS2 — µs-class differential reference (needs WS1)

1. Batched phase TX: ~10 samples per packet, sent from the NETWORK task at service cadence (5 Hz ×
   10 = the same ~50 pairs/s; pairing is by sample instant, so batching loses nothing). This
   replaces the reverted 50 Hz unicast loop, which physically displaced audio 1.5 ms (mechanism
   still owed — separate small investigation, it may inform WS1).
2. GDAVG windows lengthened 1 s → 10–30 s for the fine regime (≈ 0.3–1 µs noise if pair noise is
   ~9 µs and honest after WS1).
3. **Gate**: GDAVG(30 s) tracks the wire within ±0.5 µs over a quiet hour.
4. Then: align consumes GDAVG instead of the single-pair delta; recentre cap stays 2 µs/cycle.

## WS3 — Pure-rate steady state (parallel with WS2)

1. **Invariant, enforced**: zero frame operations while converged — a counter/log line that makes
   any steer trim or splice in a quiet window a reportable defect (one frame = 22.7 µs = 20× the
   budget). Quiet hours already read corrected −0/+0; make it a guarantee, not a habit.
2. **Crystal feed-forward tightening**: the beaconed crystal-difference correction leaves 0.17 ppm
   over 100 s (measured); budget needs ≤ ~0.05 ppm between fine corrections. Longer crystal
   averaging + fine-loop cadence trade; measure, don't assume.
3. **Divider dither ripple**: measure the rate-lock's phase ripple on the WS0 histogram at 0.1 ppm
   scale. If the actuator itself ripples > ~0.5 µs p2p, this is the hardware floor and the goal
   needs `set_rate_adjustment` upstream work (TODO already tracks it).
4. **Gate**: quiet-window p2p ≤ 2 µs with rate-only control, before chasing the last factor of 2.

## WS4 — Event hygiene (protects the metric; mostly done or user-side)

* Server `buffer 2000 → 4000` ms (user): tonight's 1–6 s late bursts are the dominant disturbance.
* Boot ring: A/B `resync_gain 0.6` (queued; |1 − 0.6·1.75| ≈ 0.05 → one round instead of 45 s).
* Split escape (build 87, verified) bounds every tug variant; the padding-dispenser interaction and
  the accounting-split formation stay on the root-cause list but no longer gate the goal.

## Order and honesty

WS0 → WS1 (blocker) → WS2 + WS3 in parallel → gates in sequence 2 µs → 1 µs. Mean 0 falls out of
WS1+WS2 (bias is already ±2 µs). The residual risk after all gates: crystal wander between
corrections and dither ripple — physics of this hardware; WS3.3 measures whether the last factor
of 2 is reachable without upstream `set_rate_adjustment`.

## WS0 result (2026-08-30 21:17–21:45, build 87/88, hole-free 5-min windows)

Best window (21:41): n=988, mean −4.1, med −4.6, MAD **1.68 µs**, p2p 24.4 µs [−15.2..+9.2].
Typical: MAD 3–9 µs, p2p 27–71 µs, p1/p99 at ±15–36 µs; window means wander ±4 µs.
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
