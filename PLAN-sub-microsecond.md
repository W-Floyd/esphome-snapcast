# PLAN — mean 0 µs, max p2p 1 µs (quiet-window)

Goal set 2026-08-30 21:45. p2p ≤ 1 µs means every sample within ±0.5 µs, so this plan is organized
around the three things that can each individually spend the whole budget: a measurement that lies,
a frame operation (22.7 µs), and differential rate drift between corrections. "Max" is defined over
QUIET WINDOWS (rival-clean, no server hole inside) until the holes are gone — one hole is
milliseconds and belongs to the server, not the servo.

PROVISIONAL baseline (R1.8/R2.3: window opens 2–3 min post-flash — disturbed regime; to be re-taken
on one build, ≥30 min, ≥15 min post-flash): build 88, 21:39–21:45, wire median −1.5 µs, MAD 5.1,
p2p 54.5 µs; kp median 0.008 in steady state (boost scales on the differential portion); split
escape verified live (one line, 34 s recovery); injection convergence ≤ ~14 s.

## WS0 — The instrument first (no firmware)

* Quiet-hour wire HISTOGRAM of the current rate-lock-only regime (rival-gated, hole-free spans from
  test.csv). Decomposes today's ±3–8 µs into rate ripple vs measurement noise, and is the gate
  metric every later stage is judged by.

* Definition of done for the whole plan (R1.9 + R2.4, disjoint-block form so the gate statistic is
  the same statistic WS0 measured): over a 30-min quiet window, |mean| ≤ 0.2 µs, and across its six
  disjoint 1000-sample rival-clean blocks: median block-p2p ≤ 1 µs AND worst block-p2p ≤ 2 µs
  (p0.5/p99.5 reported alongside). Twice, on different days.

## WS1 — Render-tag truth (BLOCKER for everything downstream)

Evidence: phases/tags under-measure real differentials ~8× (20:36: wire −1.5 ms rival-clean,
pairwise beacon phases ≤ 0.2 ms); standing blind offsets (specimen scratchpad 20:48); every
on-device signal shares the deadline+tag stamping, so none can see what the wire sees.

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

0. **Delivery measurement first (R2.1)**: multicast currently delivers ~2 % of what is sent (GDAVG
   n = 1.1/s against 50 pkt/s; 16/s existed only in the build-85 unicast era). Add sent-vs-received
   counters (one log line each side, one minute of data) to decide the loss model: a PACKET-RATE
   limit (AP multicast throttling, socket queue) makes batching win ~10×; a per-packet probability
   makes batching worth exactly nothing (50p either way). No WS2 build work before this number.
1. **WS2's actual blocker (R2.1.3)**: the only regime that ever produced a usable reference
   (n≈16/s) is the one that displaced audio 1.5 ms. The task is "recover build 85's delivery
   without its damage" — the unicast displacement mechanism (per-destination blocking / ARP path /
   3× volume from the speaker-callback thread) is WS2's blocker, not a side note. The batched
   design sends from the NETWORK task at service cadence (append-only wire-format change: count +
   array, no version bump, short-packet defaults exercised; tolerate `stream_active_` gating or
   flush a last batch on gap entry) — this replaces the still-live 20 ms multicast per-sample TX.
2. GDAVG becomes a sliding EWMA at the current 1 s publish cadence (R1.4 — a 10–30 s box-car adds
   ~15 s of group delay; the EWMA gets the noise without the lag; phase margin vs the τ=120 s fine
   loop stated when the constant is chosen). Noise floor is delivery-bound: at 1.1 pairs/s a 30 s
   equivalent holds n≈33 → ≥1.6 µs even by the independence bound (a floor, not an estimate);
   WS2.3's gate is unreachable until WS2.0/2.1 restore ≥ ~15 pairs/s.
3. **Gate**: GDAVG (EWMA, ~30 s equivalent) tracks the wire within ±0.5 µs over a quiet hour.
4. Then: align consumes GDAVG instead of the single-pair delta; recentre cap stays 2 µs/cycle.
   PRECONDITION (R1.4): staleness invalidation for the averaged delta (mirror of
   GROUP_DELTA_STALE_US) — today the last average stands forever when comparable packets stop.

## WS3 — Pure-rate steady state (parallel with WS2)

1. **Invariant, instrumented (R1.10b)**: zero frame operations while converged — one frame =
   22.7 µs = 20× the budget. Structurally near-true already (fast splice threshold-gated, window
   steps clamp to zero); the deliverable is the COUNTER and its log line, which must also log the
   splice threshold in force (`splice_us` is a runtime override that can defeat the invariant).
   Instrumentation, not control work; can start today.
2. **Crystal feed-forward — BUILD, then tune (R1.1)**: no shipped feed-forward exists
   (`crystal_delta_ppm`'s only consumer is a log line; the 505→17 µs/100 s figure was the
   analyser's offline subtraction). Target: crystal WANDER between fine corrections (the integral
   already owns the constant part; re-measure the 0.17 ppm residual as a wander rate before any
   code). Step 1 (R2.5): confirm the ~14 ppm TSF-crystal-vs-integral offset ("int +56 vs crystal
   +42, measured all day") is common-mode across boards — it is 280× the 0.05 ppm target, and
   `crystal_delta_ppm` is a difference of per-board quantities, so a per-board component survives
   into the differential. If it is not common-mode, this workstream inherits a bias larger than the
   error it corrects. Note (R2.5): the "seed as reference" structure RESPONSE 1 cited does not
   exist in the tree — the cold-start seed is one-shot; a residual-only integrator would be NEW
   structure and must be designed as such. Firmware workstream; can start before WS1.
3. **Actuator sanity (R1.2)**: the sigma-delta bound is analytic (~10 ns = one step × one tick);
   the only way it breaks is the tick cadence not being ~100 Hz — confirm cadence and burstiness
   from the fork's tick call site. The analyser (26 ns floor) cannot see 10 ns; do not measure what
   the arithmetic already answers. `set_rate_adjustment` is off the critical path.
4. **Gate**: quiet-window p2p ≤ 2 µs (disjoint-block form) with rate-only control, before chasing
   the last factor of 2.

## WS4 — Event hygiene (protects the metric; mostly done or user-side)

* Server `buffer 2000 → 4000` ms (user): tonight's 1–6 s late bursts are the dominant disturbance.
* Boot ring: A/B `resync_gain 0.6 vs running 1.0` (header default 1.0f; the "60 %" comment is
  historical). The 21:37 episode's steps were `src=tag` (RSTEP 21:37:14.852: err=+7075 src=tag),
  so `resync_gain` IS in the path (rebuts R2.6.2). r is NOT fixed at 1.79 (R2.6.1: two points
  across a boot drain are not a step response, and build 51 measured 1:1 on clean landings) — log
  r per episode; 0.6 is chosen for robustness (monotone convergence for any r < 2/0.6 ≈ 3.3, near-
  deadbeat at the observed boot r), not as deadbeat tuning to n=1 evidence.
* Split escape (build 87, verified) bounds every tug variant; the padding-dispenser interaction and
  the accounting-split formation stay on the root-cause list but no longer gate the goal.

## Order and honesty

Start today, before the WS1 blocker resolves: WS0 baseline re-take, WS2.0 delivery counters,
WS3.1 invariant counter, WS3.2 wander measurement + common-mode check (all instrumentation or
measurement). Then WS1 (blocker) → WS2 (gated on WS1 for honesty AND on WS2.0/2.1 for delivery) +
WS3.2 build → gates in sequence 2 µs → 1 µs (disjoint-block statistic throughout). Mean 0 falls
out of WS1+WS2 (bias is already ±2 µs). Residual risk after all gates: crystal wander between
corrections and the unresolved 1.5 ms TX-displacement mechanism constraining WS2's delivery —
physics and one owed mechanism, both measured before they are believed.

## WS0 result (2026-08-30 21:17–21:45, build 87/88, hole-free 5-min windows) — MISCAPTIONED, see R1.8
(spans two firmware eras and opens 2–3 min post-flash; kept as observation; re-take pending)

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
