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
  unknown constant.** The differential trim sits **−5.25 ppm** from the true differential rate
  (−5.246 and −5.272 on the two runs, so stable rather than scatter). That is the **crystal
  difference** — each board's trim converges to cancel its *own* crystal error, so the differential
  carries the difference between two crystals, and nothing on the device knows it. Being a rate it
  integrates forever: **527 µs per 100 s** against a 13–15 µs floor. The trim offset integral
  explains **1%** where the `fs` columns explain **96–99%**.
- **Calibrating the constant away would still not be enough:** residual 0.70–0.75 ppm, ≈70 µs per
  100 s.
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

## Step 2 — `TRIM_KP_RUN = 0.1`, measured

One line. The machinery already adapts: KI tracks `kp²/4`, and the bumpless-transfer block handles
the acquire→run switch at the new gain. Loop gain drops 0.79 → 0.32, removing a 1/(1−G) ≈ 4.8×
amplification, so the win should beat the linear 2–3 µs estimate.

Measure three things, not one: differential median (predicted ~3× better than linear), wire sd on
a quiet 100 s+ run, and reboot recovery time (the priced cost: 42 s → up to ~150 s). Run **after**
step 1 lands so the better instrument sees the experiment. Fold in the free rider: re-check board
a's `split +22 µs` (stale since the in-flight fix changed what `meas` contains).

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
- **Beacon compatibility:** `TsfPacket` is packed and versioned. Adding a field changes the size,
  so bump the version byte or accept both lengths during the mixed-fleet window.

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

## State

- Three boards flashed with `172b274` (two SuperMinis + one M5Stamp S3); wire at ~2 µs static,
  sd 3.3–4.7 µs.
- `snapclient-esphome` main at `172b274`, ~29 ahead of origin; `../esphome` on
  `speaker-render-latency`. Nothing pushed (see step 0).
- `2cfd294` committed but never executed (step 5).
