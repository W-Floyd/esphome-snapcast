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
  metric every later stage is judged by. Also establishes the divider dither's own ripple floor.
* Definition of done for the whole plan: a 30-min quiet window with |mean| ≤ 0.2 µs and p2p ≤ 1 µs
  on this histogram, twice, on different days.

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
