# Plan: inter-device sync, next session

Forward plan distilled from the 2026-08-26/27 session (`85db712`..`172b274`). `TODO.md` holds the
full worklist and the dead ends; this file is the ordered path through it, with the implementation
detail checked against the code. The measurements backing each step live in `TODO.md` and the
commit messages.

**The organising fact:** the wire offset is the integral of the differential rate, and nothing
else — slope −1.0, sub-µs residual, 99–100% explained over three quiet runs. Do not propose
accounting mechanisms for static offsets; ask what made the rates differ and for how long.

Current floor: static ~2 µs, sd 3.3–4.7 µs, p2p ~10 µs, reboot recovery ~42 s. Dominant known
term: the feedback pivot, 3.15 µs/ppm of instantaneous rate error, ~70% of the 7.1 µs differential
median floor, inside a loop of gain `TRIM_KP_RUN × 3.15 = 0.79`.

---

## Step 0 — push both repos — ✅ ALREADY DONE

The handoff's "nothing pushed" was stale. Both repos are fully pushed and were at the time:
`origin/main...HEAD` is 0/0, and `origin/speaker-render-latency...HEAD` is 0/0. Nothing to do.
(`render-latency` does still lack `07f80de5e`, so step 6 stands.)

## Step 1 — log the time-mean applied trim per report window — ✅ LANDED (`ee447c5`)

The on-device snapshots explain only 13–19% of the offset because the report samples one value per
3.3 s of a continuously moving quantity — aliasing, not physics. The report now publishes the
window's time-mean instead.

Built, compiled clean, and flashed. Three details that decided whether the number is usable:

- Accumulated **after the whole servo chain**, not inside the PI block. The hard-resync and
  aggressive-catch-up branches never enter the PI, yet audio keeps being clocked out under
  whatever trim was last programmed; accumulating inside the PI would drop that time out of the
  integral silently. `st.trim_applied_ppm` is the right term because it already tracks what is
  actually programmed, including the nominal-rate fallback — so the fallback contributes a real
  0 ppm rather than a hole. (This supersedes the plan's original "accumulate `0 × dt` in the
  hold-at-nominal branch": the same effect, at a site that also covers the event branches.)
- The window's **audio time is published beside the mean**, because the integral needs mean ×
  duration and a window is not reliably one report interval. Its shortfall against the wall clock
  is also what lets the analysis detect a starvation and refuse to integrate across it.
- **Covered time** is tracked separately and printed as a percentage, so "compared, and it agreed"
  cannot look like "there was nothing to compare".

On its own line (the Sync line is at the 256-byte ceiling) and in every build, not just under
timing diagnostics.

**Success criterion — MEASURED, and the answer is a split decision (`19bca3f`).** Two runs, 96 and
127 windows over 231 and 425 s, scored against the analyser's own rate columns:

- **The aliasing was real and the fix works, at the rate level.** Window-mean differential trim
  tracks the true differential achieved rate at **corr +0.976..+0.979**, against **−0.778** for the
  end-of-window snapshot. Sampling was throwing away most of the signal.
- **The trim still cannot be the offset reference, for a more fundamental reason than aliasing: an
  unknown constant.** The differential trim sits **−5.25..−5.40 ppm** from the true differential
  rate (−5.246, −5.272, −5.400 across three runs including one in steady state, so stable rather
  than scatter). That is the **crystal difference** — each board's trim converges to cancel its
  *own* crystal error, so the differential carries the difference between two crystals, and nothing
  on the device knows it. Being a rate it integrates forever: **~540 µs per 100 s** against a
  steady-state floor of **6.96 µs sd**. The trim offset integral explains **1–6%** where the `fs`
  columns explain **96–99%**.
- **The constant is now SOLVED on-device and validated against the wire.** Each device publishes
  its own clock rate against the radio timebase in the beacon (`crystal_ppm`); two peers'
  values difference to their crystal difference. Measured with both probed boards following one
  leader: **+5.425 ± 0.128 ppm on-device against +5.25…+5.40 ppm on the analyser**, a 0.100 ppm
  agreement — **535 µs per 100 s of integrated error becomes ~10 µs**, at a ~7 µs floor. Needs
  ~40 s of settling after boot (at 18.6 s it was 4 ppm out) and is reproducible across a power
  cycle to 0.002 ppm.
- **The RESIDUAL is what still blocks the route.** After the constant is removed the trim tracks
  the true rate only to **0.708 ppm** per window, ≈**71 µs per 100 s**, still ~10× the floor. So
  the trim plus the crystal correction is a much better reference than the trim alone and remains
  insufficient on its own. Reducing that residual is the surviving work, and it is what the
  least-squares achieved-rate measurement against server time is for.
- **De-meaning is not the fix.** It drops the analyser's own check from 96% to 2%, because the true
  differential rate has a real nonzero mean (+0.61 ppm on one run = a genuine 177 µs ramp over
  290 s) and de-meaning deletes exactly the term the offset is made of. The constant must be
  *known*, not removed. A high correlation with a constant offset is the trap; correlation says
  nothing about an integral.

The same work makes the **central finding reproducible on demand** rather than remembered: the
`fs`-column check reproduces it on every replot (measured: 19.5 s → corr −1.000/slope −0.994/0.03 µs;
425 s → corr −0.999/slope −0.994/0.50 µs, 96%). Two latent script bugs were fixed to get there
(`0239695`): the replot path double-counted every `--annotate` log, and `--simulate` had always
crashed at the end of a run, which had quietly disabled the only no-hardware regression check.

## Step 2 — `TRIM_KP_RUN = 0.1` — ✅ MEASURED AND REVERTED (`4393039`)

Tried, measured, reverted; back at 0.25. The loop-gain argument (0.79 → 0.32, removing a
1/(1−G) ≈ 4.8× amplification) is still sound. The cost side was underpriced:

- **tau 80 s, settled 295 s** against ~42 s at 0.25 — worse than the ~135 s expected
- **landed at −155 µs**, outside the ±130 µs band recorded at 0.25

The second term is the one nobody had priced, and it follows directly from the organising fact
above: the offset *is* the integral of the differential rate, so a recovery freezes wherever the
integral has reached when the servo nulls the rate. Lower gain nulls slower → the integral runs
longer → **the planted offset is larger.** KP trades steady-state noise against recovery time
*and* against the size of the static offset every event leaves — which is the problem this whole
plan exists to solve. Revisit only once the anchor stops planting an offset.

## Step 2b — THE OFFSET-PLANTING MECHANISM (new, and now the live thread)

Everything above converges here: a static offset is planted by an event and then persists because
no device can see its own absolute relative position. Today's instrumentation opened this up.

**Instruments built and validated** (all diagnostics-only, nothing steers on them):
`SEEDANCHOR` / `SEEDDRAIN` (the anchor's latency error, measured from playout feedback with no
prediction in the path), per-chunk `RSYNC` bursts, wire-step detection at seeds *and* at split
repairs (fit-and-extrapolate both sides, so a ramp cannot masquerade as a step), and `crystal_ppm`
in the beacon.

**Established:**
- **Discards are the RECOVERY, not the cause.** Nine bursts: 6 with a full ring → *zero* discards;
  3 with the ring already at 26 ms → 79/32/4 discards. The empty ring comes first.
- **A re-baseline does step the wire** (3/3 seeds, >100× the noise floor).
- **`SEEDDRAIN` reads ≈ −7 ms** with playback continuous (−6778, −7383 µs — consistent).

**Refuted, by counting:** the −52 ms split spike is *not* post-seed — 337 spike episodes against
47 seeds, only 2–6% with a seed within 60 s.

**RESOLVED, and it is the session's main result: the split repair displaces real audio, and the
size is set by the trim applied during the hold.** It fires 23 times per session on splits of
4.7–57 ms. Each firing spends `DRIFT_REPAIR_HOLD_US` = 3 s steering real audio against a prediction
the code is *about to declare wrong*, then fixes the accounting — after which the displacement is
invisible to every on-device metric, because every residual reads 0.

    stage                  +-2500 us split -> step        mean      n
    baseline               +329.6 +311.1 -347.4           329 us    3
    + trim hold  (KEPT)    +187.3 -256.9                  222 us    2
    + median clear (REVERTED) +375.0 -357.7               366 us    2

The model, which held across an 8x range of split sizes: step = trim x hold, saturated by the
+-1000 ppm clamp, at 0.16–0.18 efficiency (2500 -> 625 ppm -> 330 µs; 20000 -> clamped 1000 ppm ->
491 µs). That is why a much larger split yields only a slightly larger step, and it is what
identified the TRIM rather than the split as the thing doing the damage.

**`TRIM_KP_RUN` interacts with this in the opposite direction** to the landing offset: the step
scales with KP, so a lower gain shrinks the repair's displacement while *enlarging* the offset a
recovery leaves. Judge any future KP change on both — the last one was judged on neither.

**Still open:** the 222 µs that survives the trim hold. The stale-median explanation was tested and
refuted (see `TODO.md`), so its cause is unknown. If the stale window is still suspected, test it by
letting the median REFILL before the PI acts again, not by emptying it.

**Method for collecting more:** `inject_split(us)` provokes a repair on demand — ramped, never
stepped — and points must be spaced on a quiet-wire gate rather than a fixed sleep. Both constraints
were learned the hard way and are recorded in `TODO.md`. **Do not use `inject_starvation` for this**;
it puts the fit floor at 162–1291 µs against 0.71 µs quiet.

## Step 3 — achieved rate against server time, published in the beacon

The keystone, and after step 1's measurement the **only surviving route**, not merely the preferred
one. An outside-the-loop rate reference de-trends the prediction (step 4) and, published beside
`drift_ppm`, lets each device integrate the difference and know its own relative offset without an
analyser. The three failed corrections all derived the reference from the controller's output; this
one comes from the plant.

**The accuracy spec, derived from step 1 rather than guessed.** The offset is the integral of the
rate, so a constant error `ε` ppm costs `ε` µs per second of run — the trim's −5.25 ppm constant is
what makes it useless. To keep the integrated error inside the ~13 µs floor over a 300 s run needs
the *constant* known to better than **0.04 ppm**, and over 100 s to **0.13 ppm**. That is the number
the design has to hit, and it is what makes the least-squares fit below mandatory rather than
tidy: a two-endpoint baseline on ~300 µs-jitter timestamps resolves only ±10 ppm, i.e. 250× too
coarse. It also rules out any scheme whose zero point is a servo output, since those carry the
crystal offset by construction.

- **Source:** fit `played_frames_total_` (playout feedback, `notify_audio_played`) against server
  time via the clock_sync mapping. The feedback arrives at ~50 ms cadence — ~600 points per 30 s
  window, which is what the fit needs.
- **Least-squares over the whole window** (running sums, incremental). Never a two-endpoint
  baseline: credit-adjacent timestamps carry ~300 µs of jitter and a 30 s baseline resolves only
  ±10 ppm.
- **Reset the fit on any re-baseline or seed** (the existing `fb_samples_ = 0` sites are the
  arming points). The fit must never straddle a counter discontinuity — `r_push` has been measured
  at −180 s of span, and a corrupt span is milliseconds of error the moment anything scales it.
- **Publish as an absolute µs-free quantity** (ppm vs nominal), diagnostics-only like
  `pipeline_us` — never feeds the timebase.
- **Beacon compatibility — solved, with a working precedent.** `crystal_ppm` was appended to
  `TsfPacket` this session: append at the END, do NOT bump the version (a bump costs a half-flashed
  fleet its shared timebase in BOTH directions, which the note at `TSF_VERSION` explains), and make
  the receiver accept both lengths, defaulting the missing field to NaN. Copy that pattern.
- **Note the topology limit before designing the consumer:** followers never beacon (all three
  `broadcast_` sites are leader-gated), so a follower sees only the LEADER's published value and two
  followers cannot see each other's — which is the normal case for a stereo pair. Publish the RAW
  rate, since raw values from any two peers difference to what the pair needs; a delta-vs-leader does
  not.

## Step 4 — the pivot, frames-based

Only after step 3, and only via the surviving direction: compare in frames rather than time,
removing the nominal-rate assumption at its root instead of correcting for it. The bias lives in
the `nominal_slope × (pushed − fb_mean_frames)` extrapolation in `predict_next_play_us_`.

Per the session's closing lesson — the arithmetic was right three times and the robustness wrong
three times — start by **bounding the inputs** (`r_push` consistency across re-baselines), not by
refining the estimate. Do not retry: deviation-from-slow-mean, seeded means, credit-stream rates,
or any multiplicative scale on an unbounded span. All measured, all failed, details in `TODO.md`.

## Step 5 — exercise the padding-debt repayment (`2cfd294`)

Committed, never executed: every seed since has had `debt=0` because audio refills before the seed
runs; the one real occurrence took a three-seed cascade. Treat as a proposal until it runs.

The reproduction is the actual work: an `inject_starvation` variant long enough to drain the
~50 ms DMA span (or a deliberate cascade), so the seed lands on a dry DMA and `debt > 0`.
Otherwise this item idles at the bottom of the list forever. The "never below played" clamp is
retained, so the catastrophic mode stays guarded while experimenting.

## Step 6 — carry `07f80de5e` to `render-latency`

Mechanical cherry-pick in `../esphome`: the mixer in-flight fix landed on `speaker-render-latency`
(the flashing branch) but not `render-latency` (the submission branch). It gates the whole
upstream stack — resubmit the three PRs fresh, overflow fix separately and first — so pull this
forward if upstream submission has any calendar pressure.

---

## Standing constraints

- **Trust only `scripts/i2s-skew.py` on the wire.** The render phase is measured blind to absolute
  offset (wrong sign, ~12σ out); the depth delta compares occupancy, not timing. On-device, use
  the conservation residuals, `tbjit`, and the differential median between boards on one leader.
- Detect reboots by `I2SDBG written=` resetting; `Boot seems successful` only prints after a power
  cycle. Check `flash.sh -p` reached "All devices flashed successfully", not just one board's "OTA
  successful". Append to log files — restarting an `esphome logs` stream truncates them.
- Medians and MAD, never sd, for anything network-adjacent. De-trend before measuring noise. Never
  read a slope across a data gap. Sample all terms of an identity at one instant or not at all.
- Open diagnostics not on the path: the −42 ms spike with `xfer` railed at exactly one DMA buffer
  (start from why it rails), and the ~200 s serial-log gaps (`dump_statistics: off` is the
  experiment).

## Two open defects, neither fixed, both deliberately left alone

- **The mixer-lifecycle wedge.** `speaker_mixer: Stopped` on a server disconnect, then the reconnect
  succeeds and reports PLAYING while the mixer task stays deallocated and the player task blocks
  writing into it — `dma_real=0`, permanent silence, needs a replug. Fired **five times** on one
  board (10:14, 10:32, 10:36, 10:49, 11:06), three of them after the discard cap was reverted, so
  it is pre-existing and not that change. Suspect: `mixer_speaker.cpp:466-471` clears
  `MIXER_TASK_ALL_BITS` in the STOPPED handler, which would discard a pending
  `MIXER_TASK_COMMAND_START`. **Unconfirmed** — nobody has established whether a START was issued
  and lost or never issued because the player task was already blocked. Instrument before fixing.
- **The wedge has a SECOND variant: the pipeline never starts after a boot.** Same end state,
  different entry — the main loop and `wifi_diag` live while every audio task is dead from boot, and
  no `Stopped` appears at all. That points away from the STOPPED handler and toward task startup, so
  a fix aimed only at stop/restart would miss it. Both variants need a replug.
- **Why the ring empties in the first place.** Upstream of the resync runaway and of the wedge, and
  still unexplained. This is the first defect in the chain; the others are consequences.
  ANSWERED 2026-08-26 and PARKED: the `RSYNC` burst now replays an 80-chunk pre-trigger history
  (`RPRE`) when it arms. Two drains captured, both reading supply at 0.15-0.18x real time for ~2 s
  with the servo error inside +-9 us the whole way down -- the ring empties because nothing
  arrives. That is upstream of this firmware and most likely snapserver, so the chain's trigger is
  identified and not our next slot's work. Caveat: both landed minutes after an OTA of five boards
  over the same radio, so contention is not excluded.
- **A silent board is not always a wedge.** Both boards once went quiet together with no audio-task
  lines; the cause was `stream 'Spotify': status='idle'` on the server. Query
  `Server.GetStatus` on `192.168.1.2:1780` before diagnosing firmware — and note the tell: a real
  wedge leaves the net task and main loop running, so if those are alive AND the stream never
  started, look upstream.

## State

- All three boards flashed at `360ee13` (trim hold in, median clear reverted) and playing;
  steady-state medians 7–37 µs at KP = 0.25. The M5Stamp intermittently fails to resolve over mDNS
  at flash time — re-run `reflash.sh` and check the success count, since it is a group member and an
  old-firmware leader would not publish `crystal_ppm`.
- `snapclient-esphome` main pushed and clean; `../esphome` on `speaker-render-latency`, and
  `render-latency` still lacks `07f80de5e` (step 6).
- `2cfd294` committed but never executed (step 5).
- **Schema note:** `i2s-skew.csv` gained `phase_*`, `ramp_*` then `crystal_*` columns today. The
  layout check refuses a mismatch rather than misreading, so a capture running across a schema
  change needs restarting.

## Method notes earned today, the expensive way

- **Three control changes were written on plausible stories and two had to be reverted.** The
  discard cap made things strictly worse by capping a load-bearing recovery path; `TRIM_KP_RUN = 0.1`
  cost more than it bought. Both were argued from mechanism and neither was measured first.
- **"X appeared immediately after Y" failed three times today** — the 205 ms I2S stall (a log
  artifact), the 200 ms host log delay (n=1, from one mangled line), and the −52 ms spike as
  post-seed (2–6% of episodes). Each time the fix was to count how often X appears *without* Y.
  Do that first, not after writing it up as understood.
- **When one observable is consistent with both the defect and correct operation, no change to the
  response is justified yet.** A growing error during discards is produced both by a spurious
  trigger and by a real backlog being fixed too slowly. Instrument to separate them.
- **Check which of two same-named fields the evidence came from.** A whole "blocker" was recorded
  against the rate reference because a log line was added for `tsf_rate_ppm_` (leader-only) instead
  of `offset_rate_ppm_` (all roles) — the header states the difference explicitly.
- **Search the repo for a rule that contradicts the change before flashing it.** Three changes this
  session had flaws already written down here: the discard cap (unbounded discarding IS the recovery
  path), the stepped split injection (the servo reacts to steps, not slow drift), and clearing the
  median window (single samples are not measurements — it made things worse than doing nothing).
- **One clean point establishes that an effect exists; it does not size it.** A 42% improvement
  quoted from the first measurement became 33% at n=2.
- **An offset fitted across a gap is as wrong as a slope across a gap.** Anchoring device time with
  one median over a log spanning reboots put the axis 2000 s away, and it failed by reporting
  "0 paired windows", which reads as thin data rather than a broken axis.
