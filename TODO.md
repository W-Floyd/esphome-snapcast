# TODO

## Upstream ESPHome

- **Land the buffered-audio API upstream.** `speaker::Speaker` offers only
  `virtual bool has_buffered_data() const` — a bool, not a quantity — so the fill between
  our feedback point and the DAC is unobservable, which is the root of the silent-offset
  class: the accounting re-baselines against a wrong fill and settles playing 100–250 ms
  out while every sync metric reads ~0, the error being measured against a prediction
  built from the same corrupted accounting. Landing this retires the six-component fork.

  Implemented on the fork as two virtuals, because there are two questions and they differ
  by a real quantity:
  `render_latency()` (when will audio handed over now be heard — counts DMA silence
  padding) and `buffered_audio()` (how much of the caller's own audio is left — does not).
  Durations, not bytes or frames, since neither composes across a mixer's channel widening
  or a resampler's rate change. Both published as atomic snapshots by the owning task,
  because `RingBufferAudioSource` is single-consumer-thread by contract.

  PRs #18753/#18754/#18755 were opened and closed. Review found ten defects across them —
  unit mixing between input and output formats, SPDIF never publishing, a stopped speaker
  reporting "cannot" instead of zero, a documented thread-safety violation, an unreachable
  branch justified by a measurement that belonged to something else — all since fixed, and
  the design changed enough that resubmission should be fresh rather than a force-push.

  No longer blocked: the `fill` vs `pipeline` question below is settled, and it settles in
  the API's favour — the published numbers are right at the instant they are published, and
  the 20–100 ms disagreement is the consumer differencing a stale snapshot against a live
  accumulator. It does add one requirement, now implemented on the fork: every reading carries
  the instant it describes (`audio::AudioDepth`, published through a seqlock), and a composing
  stage reports the oldest instant in its total. Submit as three stacked PRs targeting `dev` in
  kahrendt's #16317 style — an outside contributor cannot base a PR on a branch in their own
  fork.

  Send the `frames_to_microseconds` overflow fix SEPARATELY and first (see below). The field
  numbers that make the upstream argument were corrupted by it, so the evidence in these PRs
  is only defensible once it has landed — and bundling an unrelated arithmetic fix into a
  feature PR invites the "why is this here" friction that closed the last round.
- **`speaker::set_rate_adjustment(float ppm)`**, default no-op, implemented in the i2s
  speaker per-SoC. `rate_lock` pokes the S3's MCLK divider directly and would prefer a
  speaker API, keeping `i2s_rate_lock` as the fallback for older ESPHome.
  Satellite1's forked `I2SAudioSpeaker::sync_play()` is independent evidence of demand.
- **`mixer` does not yield when the sink stops draining.** The task loop guards the no-data
  case with `delay(TASK_DELAY_MS)` but not the no-space case: `transfer_data_to_sink()` only
  blocks while the sink is ACCEPTING, so a stopped sink refuses at once, nothing can be mixed
  into a full output buffer, and the loop spins. `MIXER_TASK_PRIORITY` is 10 against the
  ESP-IDF main task at 1, so it can starve the main loop. Fixed on the fork (`74d32d9dd`) as a
  defensive yield, NOT proposed upstream: the failure it was written to explain turned out to be
  concurrent log sessions of mine, not starvation, so the spin is visible by inspection but has
  never been observed to bite. A report needs a test that actually stops a sink and measures
  CPU.

- **`speaker_source` spins forever on an unmixable announcement.** A format mismatch
  cannot be resolved by retrying, but that is what it does: with an announcement pipeline
  at 48000/mono against a 44100/stereo stream, the mixer sets
  `Error flag: Incompatible audio streams` once, then the player stays in `ANNOUNCING`
  allocating a fresh 9600-byte ring ~16×/second — 1742 of them in under two minutes, flag
  never cleared, until reboot. Two defects: unbounded allocation on an unsatisfiable
  retry, and no terminal failure for the announcement.

## Investigate

- **RESOLVED: the starvation re-baseline stays, and its residue is now repaired.** Tested by
  removing it, using `inject_starvation()` to fire the event on one device on demand rather
  than waiting for a group-wide one. It cannot go: without it the same injected starvation
  left the accounting at -204 ms with nothing able to fix it, and the pair swung +-20 ms for
  about twenty seconds before the servo walked it out. With it: re-locked in 9 s, back to
  within a frame on a logic analyser.

  What it DOES plant is a per-device accounting error, and that was the "silent offset" class
  all along. Measured against a logic analyser, the offset a device renders at is `-drift`:

  | `drift` | wire |
  |---|---|
  | -8526 us | 8.51 ms |
  | -64672 us | 67 ms |
  | -68549 us | 73 ms |
  | -194060 us | 198 ms |
  | -198435 us | 191 ms |

  Five for five within a few percent, and every one NEGATIVE — which is why they persisted:
  the self-repair armed on `fill_drift_us >= DRIFT_REPAIR_US`, positive only, so the one sign
  the anchor plants was unrepairable by construction. Now sign-symmetric and keyed on a
  windowed median (see the skew section), with the thresholds that follow from a 1 us floor:
  `DRIFT_REPAIR_US` 20 ms -> 2 ms, hold 10 s -> 3 s, band 10 ms -> 2 ms. The 8.5 ms case was
  BELOW the old threshold and so invisible even to a sign-symmetric test.

  Five explanations for the planted error were eliminated and are recorded in the source, so
  they are not worth retrying: divider quantisation (`want` tracks `applied` to 0.24 ppm);
  the re-baseline cascade alone (fixed, offsets persisted); snapshot staleness (tested with the
  correct published term — age does not imply drainage, upstream keeps refilling); and seed #2
  as culprit (it is COMPENSATING for a seed #1 whose anchor expires — refusing it made the pair
  198 ms apart against 10 ms).

  Still open: the anchor plants the error in the first place. It captures an instant that stops
  being true — `own=0` when measured, ~244 ms 1.4 s later — and the accounting can only learn
  that by being re-anchored. Preventing it needs an anchor that stays valid, not another way to
  suppress a seed.

  The original argument is kept below, because its reasoning about WHERE the harm came from
  is still the clearest statement of the mechanism.

  The case against it was that it anchored
  `pushed` to a number excluding the mixer ring and I2S DMA, so the clamp in
  `notify_audio_played()` permanently absorbed the shortfall — `drift` stepping to +51 ms
  and staying there while the device played ~43 ms early, both medians reading ~40 µs.

  That premise no longer holds: the anchor now uses `on_query_latency()`, which reports the
  whole chain including the DMA span, and does so deliberately — it anchors a PREDICTION of
  when the next pushed frame renders, and padding sits ahead of that frame. So the specific
  harm is gone and the open question is narrower: is the re-baseline still needed at all?

  The accounting is exact from a clean start, so this reduces to whether anything discards.
  `timeout: never` was meant to remove the teardown that does, and there are zero mixer
  stops across the fleet, yet `Pipeline drained (source starvation)` still fires. Test as
  before: skip the re-baseline, watch `drift` and `sync-delta.py` across several
  starvations. Now unblocked: the disagreement below is settled and `drift` reads 0 or -1 us
  on healthy hardware, so a single sample is usable rather than needing a median over many.
  Note the one caveat there: do NOT age the anchor for the snapshot's staleness — see below
  for the dropout that caused.

  This does not retire the fork either way. The sink query has three consumers — the
  re-baseline anchor, the `fill`/`drift` column, and the self-repair — and only the first
  would go.

- **`fill` and `pipeline` disagreed for FOUR reasons. All fixed and verified on hardware;
  `drift` now reads 0 to -1 us.** Three were in `fill` — a stale-snapshot sampling artefact, a
  32-bit overflow in `frames_to_microseconds()`, and a mixer summing two terms either side of a
  hand-off — and each was hiding the next, so they only became visible in that order. The fourth
  was not in `fill` at all: a real accounting split at pipeline start that the repair could not
  act on because of a defect in its arming test. `pipeline` — the accounting — was never the
  wrong side, and still is not.

  Three independent lines of evidence, all pointing the same way.

  *Shape.* `pipeline` is smooth and `fill` is noise: lag-1 autocorrelation 0.84/0.90 for
  `pipeline` against 0.29/0.45 for `fill`, and a mean report-to-report step of 13.5/13.3 ms
  for `pipeline` against 45.6/33.3 ms for `fill` at comparable standard deviations
  (46-55 ms both). A latched accounting error is the opposite signature — both smooth, one
  offset.

  *Quantisation.* Drift takes recurring EXACT values -26100, -26101, -26122 and -26123 us,
  plus integer combinations of those with 20000 (-46123, -54150, -74150, -80273, -97370,
  -100273). 1152 frames at 44100 is 26.122 ms, and the 128-chunk report cadence measures
  26.16 ms/chunk independently. The disagreement is a whole number of chunks.

  *Non-steadiness.* Mean drift wanders over hours, in opposite directions on the two
  clients: -46.7 -> -6.4 ms on one, +4.7 -> -34.6 ms on the other. A real split holds — the
  original sat at +50.7 ms for 18 minutes.

  Mechanism: the mixer task publishes `own + xfer + sink` as one store per iteration
  (`TASK_DELAY_MS` = 25 ms), computed BEFORE that iteration's consume and transfer, and the
  player task reads it at an arbitrary phase. Between publish and read the player pushes in
  whole 26.12 ms chunks while the DAC drains continuously, so
  `drift = pushed_since - drained_since`: chunk-quantised, negative-mean, barely
  autocorrelated. Exactly what is measured. The published number is right at the instant it
  is published; differencing it against a live accumulator is what is wrong.

  The accounting side is also ruled out structurally, which the earlier note only half
  checked. Every bias path runs the OTHER way: the clamp in `notify_audio_played()` only
  reduces `played`, `playback_delay_frames_` only withholds credit, and the
  `pending_playback_frames_` / `ps.pending_frames` resets discard it. So `pushed - played`
  can only OVER-state the queue, and `pipeline < fill` cannot come from there.

  Step 2 of the old plan was a no-op and is dropped. `mix_announce` is wired to no pipeline
  in `qemu-mixer-test.yaml`, so it never runs, never joins `speakers_with_data`, and the
  mixer is already on its single-source copy path. Cross-source over-crediting is impossible
  regardless: `atomic_subtract_clamped` caps each source's credit at its own contribution,
  the mixer never writes silence to the sink (it `continue`s), and `virtual_speaker` fires
  its callback only for frames actually consumed, never for underrun padding.

  There was a SECOND defect underneath, and it was the bigger one — found only once the
  sampling artefact above was removed and `drift` got sharp enough to show it.
  `AudioStreamInfo::frames_to_microseconds()` computed `frames * 1000000` in uint32
  arithmetic, which wraps at 4295 frames: 97.37 ms at 44.1 kHz, 89.5 ms at 48 kHz. The
  result comes back short by exactly `2^32 / sample_rate` per wrap — 97391.5 us at
  44.1 kHz — so any stage holding more than ~97 ms under-reported its depth. The example's
  i2s ring is `buffer_duration: 100ms` = 4410 frames, over the threshold by design.

  That is upstream code, present on `upstream/dev` today (verified live at b579751bdf) and
  predating all the fork work — introduced by #8164. It is LATENT there: all three upstream
  callers pass sub-DMA-buffer counts (`SPDIF_BLOCK_SAMPLES`, `frames_zeroed`,
  `silence_frames`), so nothing in pristine ESPHome reaches 4295. It becomes reachable the
  moment a caller passes a buffer occupancy, which is exactly what the depth work does.
  Fixed with a 64-bit intermediate, along with the same defect in `ms_to_frames`,
  `ms_to_samples`, `ms_to_bytes` and `frames_to_milliseconds_with_remainder` — those need
  tens of seconds in one call, so they are latent-latent and labelled as such.

  This retires an earlier claim in this entry, which was wrong: the collapse of `fill` to a
  "52-54 ms DMA floor" was never the DMA span and never physical. A true 150 ms losing one
  wrap reports as 52.6 ms. It was this overflow all along.

  It also explains `a leads b`. The overflow made `fill` read ~97 ms low, so `drift` read
  ~97 ms high, so the repair fired on a split that did not exist and subtracted ~107 ms from
  a's accounted queue. A constant overflow is perfectly steady, so the steadiness gate could
  not distinguish it from the real thing — the gate was working, its input was not.

  Both fixed and verified on hardware. Every depth reading now carries the instant it
  describes (`audio::AudioDepth`), published through a seqlock (`audio::DepthPublisher`) so a
  duration can never be paired with another publish's timestamp. A composing stage reports the
  OLDEST instant in its total, because that is the term the drain has to be measured from: the
  i2s speaker stamps its own sample point, the mixer carries up the sink's, and the resampler
  and router pass theirs through. `virtual_speaker` reads live and stamps now, so the QEMU
  chain has exactly one stale stage to attribute to.

  The consumer side is the other half, and without it the timestamp would be decoration. The
  client keeps short histories of `pushed` and `played` against the instants they took effect
  (`accounted_at_()`) and differences the reading against what the accounting said AT
  `as_of_us` rather than now. Two rings, not one: a push is stamped when it happens while a
  playback credit is stamped with the DAC time the audio rendered, which is earlier than the
  callback reporting it, so interleaving them would not be monotone. Any reset — output
  reactivation, either re-baseline, a repair — clears them.

  A reading older than the history is REFUSED rather than compared: the report prints
  `drift stale` and the repair does not arm. Printing `+0` for "could not compare" is how a
  broken instrument reads as a healthy device.

  Do NOT age the starvation re-baseline anchor. Tried, and it caused a dropout. That anchor
  takes a LATENCY, whose dominant term is the i2s DMA span, and the always-fill model holds
  that span permanently full — it does not decay with the snapshot's age the way a draining
  queue does. Aging it under-anchored the accounting to 43 ms where the honest reading was 60,
  the prediction ran early, the device rendered late, and it did not recover: hard resyncs at
  350, 2297 and 3581 ms late within four seconds, ending in `not catching up: reconnecting`
  — a message with zero occurrences before that build. The queue ahead of the DMA would be
  fair to age, but this path runs BECAUSE the pipeline drained, so that term is empty.

  Then a THIRD defect, found only because the first two were fixed and `drift` got sharp
  enough to show it. The reported depth dipped ~30 ms on 14-21% of samples while the accounted
  queue did not move at all — the mixer summed its transfer buffer, read AFTER handing audio to
  the sink, with the sink's snapshot, published BEFORE it, so the audio just moved was counted
  in neither. Fixed by reading both on the same side of the hand-off; free space still reads
  after, since that one wants the post-transfer figure. That dip dwelt for several samples at a
  time, which is long enough to look steady, and a self-repair took it for a real split.

  What remained after that was a constant `+20000 us`, and it turned out NOT to be an artefact
  at all — which four successive theories of mine assumed it was, on the strength of it being a
  suspiciously round number. Ruled out by measurement, in order: QEMU with a live-reporting sink
  reads 0/-1/-22 us, so it is not the mixer or the client; `played` tracks the i2s speaker's
  completed-frame count to within one DMA buffer, so no credit is lost; `dma_real_frames` equals
  written-minus-completed exactly, so the sink's internal accounting is honest; and `sum == meas`
  in 217 of 217 samples, so the four stages add up to precisely what is reported.

  It is a REAL latched split, established at pipeline start, and it varies per start: 20000 and
  40000 us on one client, 20000, 25510 and 30000 on the other, across three starts each. Mostly
  whole DMA buffers, not always. That is the signature of whatever was in flight when the source
  first contributed, not of any constant in the code.

  The defect was the repair GATE, not the measurement. It required every sample to clear
  DRIFT_REPAIR_US and cleared the hold window on any that did not, so a drift alternating between
  19999 and 20000 could never arm — the value most in need of repair was the one value that could
  not get it. Arming and holding are now separate tests (see `ce05a75`). Verified across four
  repairs on two clients, each returning the drift to 0 or -1 us.

  Result on hardware, both clients: `drift` reads 0 or -1 us for runs of ~70 consecutive reports,
  against ±26 ms of quantised noise before. `fill` reads 201-249 ms where it used to read 133-146
  with collapses to 53. Zero starvations, zero re-baselines, zero `drift stale`, zero
  `not catching up`, `-0/+0` frame corrections on every report, median error in the tens of us.

  Still to do:

  1. PR the `frames_to_microseconds` overflow to upstream ON ITS OWN, before or alongside the
     depth series. Frame it honestly as a latent overflow on a public helper with no
     documented ceiling — NOT as a live user-facing bug, because no current upstream caller
     can reach it. Overselling it would repeat exactly what review objected to last time. The
     gtest is written and lands with it under `tests/components/audio/`, which had no tests.
  2. Find why a pipeline start leaves 2-4 DMA buffers of the client's audio uncredited. The
     mechanism to examine is `playback_delay_frames_`: on a stream cycle the SourceSpeaker
     restarts and resets `has_contributed_` while the mixer task keeps running and retains
     `frames_in_pipeline_`, so the delay is captured from that leftover. Reading the code the
     ordering looks correct — the delay is taken BEFORE the first mix is added to
     `pending_playback_frames_` — so either that reading is wrong or the leftover is stale.
     Instrument the value AT THE MOMENT IT IS SET rather than reasoning about it; every attempt
     to reason ahead of the data in this entry was wrong.
  3. Leave the accounting alone. It is the trustworthy side, and none of this changed it.

  Method note, earned the hard way. Log the terms at ONE instant or not at all: an attempt to
  attribute the offset by logging each stage from its own task sampled them up to 0.4 s apart,
  which at 44.1 kHz is ±17640 frames of noise against an 882-frame signal, and produced a column
  of pure noise that looked like data. The reconciliation only worked once the decomposition was
  carried inside the depth snapshot itself, under the same seqlock. Two other traps: throttle a
  diagnostic BY TIME, never by iteration count, because a task loop with no guaranteed cadence
  will spin and flood the log hard enough to stall an OTA; and when an experiment ages a
  timestamp, age the VALUE it describes too, or it measures a mismatch you invented rather than
  the one you are hunting.

- **A stale-bailout reconnect could leave a speaker permanently silent — fixed, cause of the
  trigger still open.** Observed once: lateness ran 1504 -> 4544 -> 4893 ms in ~4 s, tripping
  `STALE_BAILOUT_US`. The network task recovered fully (reconnected, codec negotiated, TSF
  leadership yielded) and the player task never processed another chunk. The mixer logged
  `Stopped` once and never started; the media player reported `PLAYING` throughout; the speaker
  sat at `queued=0 dma_real=0 inflight=0` for 3.5 minutes.

  The re-arm watchdog exists for exactly this and never fired, because it was gated on
  `get_state() != IDLE` — which assumes IDLE is the only way to end up not routing. Now keyed on
  the invariant instead: chunks arriving and `output_active()` false, with PAUSED still excluded
  by state (a pause is a deliberate not-routing). The log line prints the state it fired in, so
  the next occurrence identifies the stuck state rather than leaving it to be inferred.

  Not addressed: why lateness spiralled to 4.9 s. That run had the accounting left at
  `drift=-194060` by a since-reverted experiment, so the trigger was probably self-inflicted —
  but the failure to recover from it was not.

- **Inter-device skew: the floor is now sd 4.6 us, and the chain is characterised.** Measured
  on a logic analyser decoding both boards' I2S (`scripts/i2s-skew.py`). Started at ~100 us
  excursions; the dominant chain is:

  ```
  common-mode disturbance   MAD 122 us   (both boards, corr 0.98)
        |  each board's servo corrects it
  differential in response  MAD  12 us
        |  x KP, integrated over a report
  wire skew                 sd  4.6 us
  ```

  Common-mode cancels between boards and is harmless; only the DIFFERENTIAL reaches the wire.
  That distinction is the single most useful idea here, and it is why several plausible fixes
  made things worse.

  What was fixed: the rate-lock gain split (`TRIM_KP_ACQUIRE` 0.5 / `TRIM_KP_RUN` 0.25 —
  recovery ~150 s -> 42 s, measured 3.6x); the shared-offset filter smoothed 4x
  (`OFFSET_EWMA_ALPHA` 1/64 -> 1/256, sd 9.3 -> 4.6 us, exactly the sqrt(4) predicted); and a
  leadership handover stepping the offset filter by its accumulated lag (~500 us per handover).

  SIX changes failed, all recorded in the source with their measurements. Do not retry without
  reading them: a slew limit on the trim (limit-cycled, +-1.5 ms), a gain schedule on |median|
  (limit-cycled, the threshold sat inside the steady-state distribution), snapshot aging twice,
  a drained-pipeline guard on the seed, and an alpha-beta offset filter (removed the lag it was
  designed to remove and made static skew -50 -> -567 us).

  The alpha-beta failure carries the general lesson: for inter-device sync the goal is not for
  each device's error to be ZERO but for the two to be EQUAL. An EWMA's lag is deterministic so
  it largely cancels in the difference; two independently converged rate estimates do not.

  Remaining levers, in order:

  0. **`TRIM_KP_RUN` = 0.25 is a comfort setting, not a fix.** It buys recovery speed with
     steady-state noise (15 us at 0.1, 35 us at 0.25, 45-100 us at 0.5 — the relation is linear
     and held across all three). A reboot only starts ~790 us out because the anchor plants an
     offset; fix that and this should go back down.
  1. **The static ~50 us offset.** About 32 us of it is EWMA lag difference — the boards'
     local-vs-server drifts differ (-52.4 vs -47.6 ppm) and lag is `rate * tau`, so 4.8 ppm x
     6.7 s. Predicted and confirmed twice (8 us at tau 1.7 s / observed -14.7; 32 us at tau
     6.7 s / observed -50.3). The fix is a SHARED rate estimate published in the beacon like
     `drift_ppm` already is, so it cancels in the difference. Not a tuning change.
  2. **The feedback pivot**, which advances in 50 ms DMA granules (`inflight=2205`) with each
     board at a different sub-granule phase. The other half of the differential, and untouched.
  3. **The -42 ms split spike.** Recurs at -42223..-42246 us on both boards, to within 20 us,
     so it is structural rather than noise. Rejected by the median, still unexplained. Likely a
     stale or partial depth snapshot that `accounted_at_()` then differences against.
  4. **Board a carries a persistent `split +22 us`** where b sits at -1. Constant across every
     window measured, so real.

- **Instruments, and which ones lie.** Every real gain tonight followed a measurement getting
  better, and three instruments were actively misleading:

  - **Pipeline DEPTH delta** (`pipeline_us` over TSF) compares buffer OCCUPANCY, which
    legitimately differs between devices rendering in sync. It read -13.4 ms for a 3.7 ms
    offset and +206 ms for one the wire put at 191 ms. Fit to raise an alarm, unfit to size
    anything. `PIPELINE_DIVERGE_US` is now 5 ms (was 100 ms, sized against an artefact that the
    window-mean fix had already removed) and it caught two self-inflicted regressions within
    6 s each.
  - **Render phase** (`render_phase_us`, added then documented as blind) compares WHEN a known
    frame renders, which is the right quantity — but it consumes `pushed - played`, and the
    servo steers audio to match that same accounting, so the two errors cancel exactly:
    `render_phase = true_phase - d`. It read +10 us for a 191 ms offset. Any future instrument
    must avoid consuming the accounting on both sides.
  - **`tbspan`** was the raw spread of `deadline - server_ts`, dominated by the -50 ppm drift
    (~165 us per report). It read ~170 us regardless and could not detect the 4x filter
    change. De-trended to `tbjit` (spread of consecutive differences) it reads 1-4 us in steady
    state and found the handover step at 525 us within one report.

  Two statistics lessons: `sd` is the wrong summary for anything here — network events dominate
  it, and a differential of MAD 12 us reported sd 129 us — and a `mean` over a window carries a
  rare fixed-size spike straight into the answer where a median rejects it.

- **Stale deadline on stream resumption.** With `keepalive_hold: never` the pipeline is
  held across an idle, and the first chunk afterwards carries a deadline stale by roughly
  the idle duration (14044648 ms after 3.90 h; 24888016 ms after ~6.9 h). It self-heals in
  ~2.5 s of chunk-dropping and the magnitude rule mutes correctly, so it is bounded, but
  the mechanism is unexplained and closing it needs the snapserver side. Two clients
  derived slightly different magnitudes from one event, which suggests each computed it
  locally against a common bad input rather than being handed the same number.

- **Extract the servo — waiting on a consumer, not on tidiness.** `player_task_` is 433
  lines, its state is a `ServoState` struct, and the re-baseline, sync report and stale
  bailout are named methods, so what remains is a real extraction rather than a tidy-up.
  Two things argue against doing it speculatively.

  It has one consumer. `clock_sync`'s existing members are narrow primitives —
  `KalmanTimeFilter` takes RTT samples and knows nothing of their origin — where the servo
  is their *integration* with this component's I/O, so its interface would be designed from
  a single data point. And the playout feedback (`pushed_frames_total_`,
  `played_frames_total_`, the EWMA pivot terms) is written from the speaker callback thread
  under `playout_mutex_` and read by the player task: today one file's private invariant,
  extracted it becomes a cross-component guarantee, in the code TIMING.md §4 says the
  metrics structurally cannot audit.

  There is a second implementation of the same concept — `sendspin-cpp`'s 936-line
  `sync_task`, same `{frames_played, finish_timestamp}` feedback contract, same hard/soft
  correction split — with no rate steering, no median filter, no PI and no shared timebase.
  MIT permits it to take this code, but it has a working implementation of its own, so
  the trigger is somebody asking rather than the licence allowing.

  It would need somewhere to live. `clock_sync` is deliberately audio-free — a clock
  filter and TSF distribution, neither of which knows what is being played — and the
  servo is the opposite: chunks, sample rates, frame splices, silence insertion. So it
  would be a third component alongside `i2s_rate_lock`, not a member of either.

  The portable half of this is `set_rate_adjustment` above: an API, so no licence or
  extraction question, and the only way rate steering reaches a non-S3 target at all.

- **Residual serial-log gaps.** A ~200 s gap appears in an otherwise clean 10 s cadence
  with host-side buffering ruled out. `esp_wifi_statis_dump()` is the suspect: it emits a
  large multi-line block every tick and its output has never appeared in any capture. It is
  opt-in (`wifi_tools: diagnostics: dump_statistics`, default off), so a long capture with
  it disabled is the experiment.

- **Schema-to-README drift check.** The README's options table and the example's
  commented defaults are hand-maintained against `CONFIG_SCHEMA`, and nothing detects a
  new option, a changed default, or a commented value that is not the default. An AST
  walk over every `cv.Optional` checks all three in ~40 lines: key set, default values,
  and the `unset` marker for options that have none. Generating the DESCRIPTIONS is not
  worth it — seven options carry no source comment, so the README's prose is better text
  than anything derivable.

## Housekeeping

- **Cut a tagged release.** The README tells people to pin `github://…@<tag>` and there
  are no tags, so the only thing to pin is a SHA. A tag would also anchor the MIT cutover,
  which NOTICE.md currently dates but does not tie to a release.

## Features

- **Runtime server retargeting** — `snapcast://host:port` URIs currently warn; needs
  thread-safe reconfiguration of the network task's target.
- **Crossfade on servo frame splices** — the last near-inaudible discontinuity class; a
  2–4 sample crossfade would zero it. Largely moot where `rate_lock` is enabled, since
  steady state stops splicing altogether.
- **Encoded-FLAC buffering** — buffer compressed chunks and decode just-in-time in the
  player task, halving PSRAM needs (Satellite1 validates feasibility). Only worth it if a
  PSRAM-less board variant matters.
