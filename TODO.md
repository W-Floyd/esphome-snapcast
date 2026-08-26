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

Current floor: MAD 1–3 µs steady state, p2p ~15 µs, reboot recovery ~42 s. The static term was
the offset filter's tracking lag and is now fed forward (see `OFFSET_RATE_*` in `tsf_sync.cpp`).
What is left is a static offset planted at every accounting re-anchor — see the first item, which
is now the largest error in the chain by an order of magnitude.

- **The feedback pivot** advances in 50 ms DMA granules (`inflight=2205`) with each board at a
  different sub-granule phase. The remaining half of the differential noise; untouched.
- **The −42 ms split spike.** Recurs at −42223..−42246 µs on both boards, to within 20 µs, so it
  is structural. Rejected by the median so it is harmless, but unexplained. Suspect a stale or
  partial depth snapshot that `accounted_at_()` then differences against. New clue: every
  occurrence caught with the residuals logged had `xfer=50000` — its maximum, equal to `dma` — and
  `r_mix=441` frames (10 ms), which is nowhere near the 42 ms and so does not explain it. Start
  from why `xfer` is railed at exactly one DMA buffer whenever this fires. Since the in-flight fix
  it reads −52245/−49343 — the same spike plus the 10 ms `inflight` those samples carry — and the
  terms are self-consistent there (`sum == meas`, `r_mix == 0`), so it is not a conservation
  failure. Every occurrence has `xfer=50000`, `inflight=10000`, `queued=70000`, `dma=50000` and
  `age≈33 ms`: a full transfer buffer, i.e. the sink briefly not accepting.
- **Board a carries `split +22 µs`** where b sits at −1. Constant across every window measured.
- **The re-baseline anchor is now reproducible on demand and measured at chunk resolution.** Fire
  `inject_starvation(ms)` (an API action in `snapclient-base.yaml`; there is a helper at
  `scripts/`-adjacent scratch or four lines of `aioesphomeapi`, plaintext API, no noise PSK) and the
  seed arms an 80-chunk `EARLY[n] seed` burst at ~26 ms. Two injections, 900 ms and 2500 ms:
    - **The residual is exact and constant.** `acct − meas` sat at **−51747 µs = 2282.0 frames** from
      the first sample after the seed and never moved by more than 1 µs across 3.3 s. That is
      **one DMA buffer (2205 frames, 50000 µs) plus exactly 77 frames (1747 µs)**. Both integers.
      Unexplained, and the interesting part: `debt` was 0 for that seed and every conservation
      residual was 0, so it is neither the padding path nor a stage losing audio.
    - **The seed can anchor audio that does not exist.** `latency=50000 own=0 dma=0 debt=2205
      seed=2205` — no real audio anywhere in the chain, 2205 frames anchored. That is the padding
      path working as designed (seed on `latency`, repay `debt` once the padding drains), but see
      the next point for when it repays.
    - **The repayment lands while the pipeline is still refilling.** `PAYDBG debt=2205 repay=2205
      acct_after=66530 lat=126530 own=126530 resid=-60000`: after repaying, the accounting is 60 ms
      below the chain where it would have been 10 ms below without repaying. `pad_now` reaching 0
      is not sufficient evidence that the seeded padding is what drained — by then the real audio
      behind it has arrived. A repayment keyed on the seeded padding having been *credited* rather
      than on the current padding being empty would not have this problem.
  Ruled out along the way, with the sign as the argument: `own` and `latency` are computed
  correctly by the sink (`queued + dma_real` vs `queued + dma_span`), and `held` is the DMA SPAN,
  not silence — so there is no field inconsistency, and `debt=0` really does mean the DMA held real
  audio. Earlier note that ~30 ms of audio "does not reach the DAC" is superseded: the sink never
  stopped (`I2SDBG` continuous, `written`/`completed` advancing) and `srcrx` stayed cumulative
  across the seed, so neither ring was discarded.
- **Two candidate mechanisms for the per-start offset are now dead**, both recorded at their sites
  in the fork (`449574cc5`): `playback_delay` was ZERO on all 18 starts measured, and padded
  silence does not displace (two boards differing by 877 ms of accumulated padding sat 133 µs
  apart, so the sink's per-descriptor real-frame bookkeeping handles it). `pad` is now published
  through `AudioDepth` and printed in RECON, so both stay cheap to re-check.
- **Test prospectively whether an offset only ever appears after a re-baseline or repair.** The
  boots that needed neither landed at −5, +7.8 and +7.7 µs; the events that planted 30–133 µs all
  involved one. Not yet conclusive: one lone replug settled at −56 µs with nothing logged, and b's
  log history before 02:57 was lost when its stream was restarted, so the correlation could not be
  checked against the earlier samples. Needs a few more events with both logs continuous.
- **Re-measure the per-boot absolute offset now that the depth report is complete.** It was ±30 µs
  and invisible on-device (differential median −2.0 ±3 µs against +30 µs on the wire) before the
  mixer fix; the boot after it settled at −0.5 to −9 µs with 8–23 µs p2p, which is at or below the
  analyser's own floor. One boot is not a distribution: reboot one board 4–5 times and record where
  each lands before deciding whether anything is left here.
- **Every accounting re-anchor plants a static wire offset of tens of µs.** Now the dominant
  term, and only visible on the analyser. Measured in one session at MAD 1–3 µs of noise, so the
  residual is resolved to 1 part in 40 — far better SNR than the 8.5 ms this was chased at:
    - three boards restarted together: **+0.9 µs**
    - board b alone, twice: **+89 µs**, then **+115 µs** (26 µs apart, so not frame-quantised)
    - both boards together: **+26 µs**, then b's split repair fired and it went to **+85 µs**
  The chain on that last one is now understood and was a CASCADE: the mixer's incomplete depth
  report showed +25509 µs, the repair fired on it at 02:24:46, and subtracting those 1125 frames
  from `pushed` is what created the −25488 µs split that the second repair answered 14 s later.
  Two repairs, the second undoing the first, neither needed — the frames were only in flight.
  `MIX_RESIDUAL_MATCH_US` stops it at the head; the two gate-era boots landed at −15 and +30 µs
  against +85 to +115 µs before it.
  Instrument the seed and the first credits at the instant they happen.
- **Leave `TRIM_KP_RUN` at 0.25.** Re-measured after the feed-forward and the trade has inverted.
  KP multiplies the DIFFERENTIAL MEDIAN, and that input has fallen 6× since the gain was chosen
  (sd 43.0 → 6.7 µs; differential trim sd 20.9 → 1.7 ppm), so the 35 µs the setting was said to
  cost is now most of a 15.7 µs p2p budget. 0.1 would buy ~2–3 µs of the 5.1 µs sd and cost 3.6×
  on reboot recovery (42 s → 150 s). Revisit only if the recovery trough goes away.
- **Stale deadline on stream resumption.** With `keepalive_hold: never`, the first chunk after a
  long idle carries a deadline stale by roughly the idle duration. Self-heals in ~2.5 s and the
  magnitude rule mutes correctly, so it is bounded. Closing it needs the snapserver side.
- **Why did lateness spiral to 4.9 s** before the stale bailout fired? Probably self-inflicted by
  an accounting error since reverted, but unconfirmed.

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
- **Two boards following the same leader can be differenced.** `delta(b) − delta(a)` from the
  `Render phase` line is the cheapest on-device stand-in for the analyser, and the medians need
  ~70 samples each before they mean anything (single samples carry ±100 µs).
