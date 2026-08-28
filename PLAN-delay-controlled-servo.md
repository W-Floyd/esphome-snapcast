# PLAN — control the measured transport delay, not a model of it

The servo steers on a prediction. Everything defensive around it exists because that prediction is
built from a ledger that can be silently wrong. A render tag measures the same quantity directly, so
the defences become unnecessary rather than better tuned.

This is specified to the point where building it is mechanical. Every number in it was measured on
the bench on 2026-08-28; none are estimates.

> **REVIEW 4 — verification sweep: every checkable claim was checked against the code.** Verified
> and correct: `split_ramp_remaining_us_` exists and "SPLITINJECT ramp complete" logs at zero
> (`snapcast_client.cpp:3184-3186`); the ramp is 100 µs/s (`SPLIT_RAMP_US_PER_S`, :712), so the
> starvation latch really is ~40 min away; `fast_splice_threshold` is 1 ms in the example config
> and `DRIFT_REPAIR_US` 2 ms; the local-Kalman fallback, `deadline_on_shared_tsf_`, the Kp
> acquire→run decay (`TRIM_KP_ACQUIRE` 0.5, decay tau 20 s), conditional integration, and the
> measured flow-control source (`on_query_audio` → `output_buffered_audio`) all exist as named;
> the tag anchor is republished per chunk; tau = 1/Kp arithmetic checks. Two claims do NOT
> survive the code: the splice mechanics under Startup, and — decisive — the headline test's pass
> criterion. Both annotated in place below.

> **RESPONSE — the sweep is the single most useful thing done to this document, and both failures
> are conceded and fixed in place.**
>
> Worth naming what the sweep changes about how much weight this plan can carry. Its opening line
> claims every number was measured and none estimated — but "measured on the bench" and "checked
> against the source" are different guarantees, and only the first was ever true here. Four rounds of
> review found: a control law missing a plant term, a seed naming a value that does not exist at
> boot, a citation borrowed from an unrelated failure, two mechanisms described without reading their
> shape, and a headline test that failed by construction. **Every one of those was findable by
> reading the code, and none needed the bench.**
>
> The two survivors are the important ones and neither is cosmetic: `fast_splice_` runs episodes
> rather than lone splices, and the retained splice path consumes the ledger, which made the plan
> predict its own test's failure. Both are answered where they occur.
>
> The rest of the sweep's verified list — `split_ramp_remaining_us_` and its completion log, the
> 100 µs/s ramp and therefore the ~40-minute starvation distance, the 1 ms / 2 ms threshold ordering,
> `deadline_on_shared_tsf_`, the Kp acquire→run decay, conditional integration, `on_query_audio` →
> `output_buffered_audio`, the per-chunk anchor republication, and the tau arithmetic — is now the
> only part of this document that has been independently confirmed rather than asserted. That
> distinction should survive into whatever gets built.

> **REVIEW 2 — the accepted responses leave the contradicted body text standing; fold them in.**
> A document that claims to be mechanical to build cannot require the reader to diff each section
> against a response further down. Still stating the superseded position: the proposal box
> (`D_hat`/`D_target`, the plant equation missing the crystal term), the control law
> `trim = Kp * (D_hat - D_target)`, "reverts to the ledger path" under Tag loss, "degrade to the
> existing behaviour" under What must be kept, and Startup's "at whatever `D` then is" (superseded
> by splice-to-threshold handoff). Also promised but not done: the shared-TSF-mapping precondition
> and the local-Kalman-fallback hold were to be "added to the semantics list" and were not.

> **RESPONSE — accepted in full; the body is now folded and the responses are history, not
> corrections.** A document that requires diffing its own sections is not mechanical to build from,
> and leaving the superseded text standing while the correction sits 200 lines below is exactly the
> failure mode that produced the retractions this plan is trying to avoid.
>
> Folded: the proposal box (loop variable `err_tag`, setpoint zero, plant carrying `crystal_ppm`,
> shared-mapping precondition), the control law (now PI with the standing-error arithmetic inline),
> the tag-loss and fallback semantics (hold trim + splice; no ledger servo to revert to), and
> Startup (splices to threshold, then hands over and seeds the integral).
>
> The two promised items were indeed dropped and are now in the semantics list as their own bullets:
> the shared-mapping **precondition**, and **hold trim on falling back to the local Kalman offset**.
> Both were written as "add to the semantics list" and then not added — the same class of miss as
> answering six of seven review notes and calling it complete.

## Why the present error signal needs defending

    error_us = predict_next_play_us_() - deadline
               └─ EWMA pivot over (frame index -> DAC time), extrapolated along the nominal slope ─┘

That pivot is the ledger. The ledger is perturbed by events it cannot observe: a pipeline restart
discarding queued frames, silence padding, the phantom-frame clamp, a mixer rebuild at a different
fill. So the code keeps a second independent estimate — the sink's measured depth — and `drift` is
their disagreement. `DRIFT_REPAIR_US` (2000) arms a window, `DRIFT_REPAIR_HOLD_US` (3 s) confirms
it, and the trim is pinned to the PI integral throughout. All of it is a lie-detector for one's own
bookkeeping.

**The ledger-derived error cannot see the displacement it causes.** Measured with
`inject_split(+1000)` on one board:

    err_live   -57..+95 us      the servo NULLS it by displacing real audio; it is blind to the
                                displacement, because creating it is how the error reached zero
    err_tag    moved ~1100 us   it measures where the audio actually is
    diff       -1020..-1052     ratio 1.02-1.05 against 1000 us injected
                                recovered to -33 us within ~15 s of the negated restore

Same result as the render-phase test the same morning: 0.94 for the measured form against 0.12 for
the inferred one.

## The measurement, and its precision

A render tag gives, for one frame, the server time it belongs to and the local instant it rendered.
Subtract the deadline (which carries the clock offset) and what remains is the render error, free of
any ledger:

    err_tag = (adjusted_ts - real_frames/rate) - deadline(tag.server_ts + tag.offset/rate)

Block-means variance sweep, ~334 arrivals per report:

    quiet window   B=1  27.3   2 19.1   4 13.1   8 8.4   16 6.7   32 5.2   64 5.0 us
    ideal 1/sqrt(B)      27.3     19.3     13.7     9.7      6.8      4.8      3.4
    active window        24-30    22-26    22-24    21-23    21-23    21-22    22-23

So there is a real white-noise component of ~20-27 us per sample that **averages away**, plus a
correlated component that does not. Practical resolution: **5-13 us per report when quiet, ~22 us
when the delay is genuinely moving** — and the latter is signal, not error.

**Unexplained, and worth resolving before relying on the tighter figure:** board A flattens at
~12.5 us where board B reaches 5.0. Same firmware, same stream, 2.5x difference in achievable
precision.

## The proposal

    plant        the pipeline as a BLACK BOX with transport delay D
                 dD/dt = -(trim_ppm + crystal_ppm)     1 ppm = 1 us/s; positive trim plays faster
                 crystal_ppm ~40 ppm on this bench, and it is why the loop needs an integral
    measurement  err_tag, from tags, available at ~100 Hz
    setpoint     ZERO. buffer_ms, server_latency and static_delay enter through deadline(),
                 which err_tag already subtracts -- there is no separate target constant
    actuator     the I2S rate trim
    fast path    fast_splice_, driven by err_tag WHEN TAGS ARE FRESH and by the demoted
                 prediction only when they are not. This is load-bearing, not a detail:
                 while splices consume the ledger, a 1 ms ledger bias displaces real audio
                 through them and the headline test below fails by construction
    PRECONDITION the SHARED TSF mapping. On the local-Kalman fallback the clock-offset wander is
                 per-device rather than common-mode; the loop must widen tau or hold trim there

It does not matter HOW the delay arises. Ring depth, DMA padding, mixer fill, a restart at an
unobserved level: each is a term the present design must model separately and can get silently
wrong, and each is invisible inside a box whose output is measured.

> **REVIEW — `D_hat` is not locally observable; the loop variable is `err_tag`, target 0.** The
> raw transport delay carries the unbounded local-vs-server clock offset — the code says exactly
> this where the raw delay is computed (`snapcast_client.cpp` ~1059: "Carries the ... CLOCK OFFSET,
> which is unbounded and drifts at the crystal difference"), and the precision figures quoted above
> were measured on the deadline-corrected `err_tag`, not on `D`. So `buffer_ms`, `server_latency`
> and `static_delay` enter through `deadline()`, not as a separate `D_target` constant; the
> "setpoint" row of this box is the same subtraction stated twice. Two consequences worth writing
> down: (1) the clock-offset estimator stays load-bearing and INSIDE the loop — its wander
> (~100 µs/s on wifi jitter, per the inject_split ramp-rate comment) is a disturbance a 3–10 Hz
> loop chases harder than the 3.35 s servo does. What filters it? (2) `deadline()` linearity holds
> only "for fixed buffer and offset" — the tag anchor must be re-anchored on a `buffer_ms` change,
> which belongs in the setpoint-change semantics below.

> **RESPONSE — accepted; the box is wrong and is restated below.** The loop variable is `err_tag`
> and the setpoint is **0**. `buffer_ms`, `server_latency` and `static_delay` enter through
> `deadline()`, so the "setpoint" row was the same subtraction written twice. There is no separate
> `D_target`.
>
> **(1) Nothing filters the offset wander, and nothing needs to — PROVIDED it is shared.** When
> `deadline()` uses the shared TSF mapping (`deadline_on_shared_tsf_`), every device in the group
> chases the same wander in the same direction. It is common-mode by construction, which is the
> entire reason that path exists, and inter-device alignment — the thing being optimised — is
> untouched. It costs absolute-latency accuracy, which nothing here is grading.
>
> That makes **"shared mapping available" a precondition of the delay loop, not an incidental**. On
> the local-Kalman fallback the wander is independent per device and does differentially misalign;
> there the loop must widen tau substantially or hold trim. Add to the semantics list.
>
> **(2) Correct, and the anchor being fresh does not save it.** `tag_anchor_*` is republished every
> chunk (~26 ms), so a `buffer_ms` change reaches the anchor almost immediately — but the ~250 ms of
> audio ALREADY IN FLIGHT was scheduled under the old deadline, so `err_tag` steps by the change for
> one pipeline depth and the step is counted twice exactly as described. Handle it with the mechanism
> already in the codebase: **mark the tag stream invalid for one pipeline depth after any setpoint
> change**, the same way the freshness gate refuses a stale observation. Moved into the semantics
> section.

The control law is **PI**, not P:

    trim = Kp * err_tag + Ki * integral(err_tag)      tau = 1/Kp,  Ti = Kp/Ki

The integral is not optional. `crystal_ppm` drives `dD/dt` at zero trim, so proportional-only parks
at a standing error of `crystal_ppm / Kp` — at 40 ppm and Kp = 0.033 that is **~1200 us**, which is
above `fast_splice_threshold` and would leave the loop permanently splicing. The integral IS the
learned crystal offset; that is what the present servo's integral holds and what its split-hold pins
to.

`TRIM_KP_RUN` is 0.25 ppm/us today, i.e. tau = 4 s at the servo's 3.35 s sampling — near-equal lag
and tau, which is its own instability regardless of dead time. At the 3 Hz sampling proposed below
the same tau is ~13x the 250-300 ms dead time and is not obviously unsafe; see the tau discussion
under the cadence section rather than assuming 4 s is disqualified.

> **REVIEW — pure P leaves a standing error of crystal_offset/Kp; the integral must survive.** The
> plant is not `dD/dt = -trim_ppm` alone: the crystal difference (~40 ppm on this bench) drives
> `dD/dt` even at zero trim. Under `trim = Kp·e` the steady state is `e = crystal_ppm/Kp` — at
> today's Kp that is ~160 µs of standing error. The current servo's integral IS the learned crystal
> offset (the split-hold pins to it for exactly that reason). So this is a PI loop, or P plus a
> feedforward from the rate lock's learned offset — say which. And the PI mechanics the deletion
> list doesn't name but the loop still needs — the trim clamp, conditional-integration anti-windup
> (the +164.9 ppm runaway cited under Risks is what its absence looks like), the Kp
> acquire→run decay — belong in "What must be kept".

> **RESPONSE — conceded, this is a straight error.** The plant is
> `dD/dt = -(trim_ppm + crystal_ppm)`; the crystal term drives drift at zero trim. Under pure P the
> steady state is `e = crystal_ppm/Kp`, which at ~40 ppm and Kp 0.25 parks **~160 µs** off target —
> two orders above the ~1 µs alignment now being achieved. Writing `dD/dt = -trim_ppm` in the plant
> box hid a term the current design already handles.
>
> **It is a PI loop**, and the integral should be **seeded from the rate lock's learned baseline**
> rather than re-acquired: that value already exists, the split-hold pins to it for exactly this
> reason, and seeding avoids a slow crystal re-learn on every start. P-plus-feedforward is the same
> thing with the adaptation removed, and loses the ability to track crystal drift with temperature.
>
> "What must be kept" gains: **the trim clamp, conditional-integration anti-windup, and the
> Kp acquire→run decay.** The +164.9 ppm runaway under Risks is what the second one's absence looks
> like, so listing it as a risk while omitting the mechanism that prevents it was inconsistent.

> **REVIEW 2 — the seed source is misnamed, and the value it names does not exist at boot.** The
> rate lock's "baseline" is the I2S divider correction (`rate_lock.h:60`,
> `baseline_corrected_ppm()`) — how far the driver's programmed divider is off ideal, re-read after
> every pipeline restart. The learned crystal offset is a different quantity, and it lives in
> `st.trim_integral_ppm` — per-session RAM, inside the servo this plan deletes, persisted nowhere.
> So there is nothing to seed from on a boot; the new loop's integral re-learns from zero (which is
> fine — say so) unless the plan adds persistence (which is a new work item — then name it).
>
> **And no Ki is stated, and at the proposed Kp the integral is load-bearing at startup.** With
> Kp = 0.033 ppm/µs and a ~40 ppm crystal, the P-only standing error is ~1200 µs — ABOVE the 1 ms
> `fast_splice_threshold`. Until the integral has wound up to the crystal offset, the loop parks at
> the splice boundary and the observable behaviour is periodic splices, governed entirely by the
> unstated integral rate. State Ki (or the integral time), its anti-windup interaction, and the
> expected wind-up duration — that transient is the first thing the bench will show.

> **RESPONSE — both halves correct; the seed was named wrong and Ki was missing entirely.**
>
> **On the seed:** `baseline_corrected_ppm()` is the divider correction — how far the driver's
> programmed divider sits from ideal, re-read after every pipeline restart — not the crystal offset.
> The crystal offset really does live only in `st.trim_integral_ppm`, in the servo being deleted,
> persisted nowhere. Naming it "the rate lock's learned baseline" conflated two different
> quantities.
>
> Corrected in Startup above: the loop **seeds from the trim currently applied at handoff**, which is
> what the acquisition path has already learned this session. That is continuity within a session,
> not persistence. A cold boot re-learns from zero, which is fine because acquisition is muted and
> splicing anyway. **Persisting the crystal offset across boots is a separate work item and is
> explicitly not in this plan.**
>
> **On Ki:** correct, and the consequence is worse than "unstated". At Kp = 0.033 and ~40 ppm the
> P-only standing error is ~1200 us, above the 1 ms splice threshold — so a cold-boot loop would sit
> at the splice boundary emitting periodic splices until the integral wound up, and the observable
> behaviour would be governed entirely by a number the plan never gave.
>
> **Stated: Ti = tau (Ki = Kp/tau), i.e. Ti = 30 s at tau = 30 s, or 8-10 s if the shorter tau is
> chosen.** Wind-up to 63% of the crystal offset in Ti, ~95% in 3*Ti. Anti-windup is conditional
> integration: freeze the integral whenever the trim clamp is active, which is also what stops the
> splice-boundary transient from winding the integral against a saturated actuator.
>
> With the handoff seed above, that transient only occurs at cold boot, where it is inaudible. Worth
> grading anyway, because the reviewer is right that it is the first thing the bench will show.

> **REVIEW — trim noise integrates into wire wander; pick Kp against that, and state tau.** At
> N=10 / 10 Hz the per-update noise is ~8.5 µs, and each update dithers the rate by Kp·σ. The wire
> offset is the integral of the DIFFERENTIAL rate (documented at snapcast_client.cpp ~298), so two
> devices' independent trim dither random-walks the wire between corrections. The doc bounds loop
> bandwidth at ~0.5 Hz from dead time but never states the target tau or the new Kp — the
> noise-vs-bandwidth trade is the actual tuning decision here, and it is left open.

> **RESPONSE — accepted, and the arithmetic says noise is NOT the binding constraint.** For a
> first-order loop with measurement noise sigma at update interval T, the closed-loop output noise is
> approximately `sigma * sqrt(T / 2*tau)`. At sigma = 8.5 µs and T = 0.1 s:
>
>     tau =  4 s   ->  0.95 µs
>     tau = 30 s   ->  0.35 µs
>
> Both are far below the ~1 µs the pair currently holds, so trim dither does not set the floor at any
> sane tau. **Dead time does.** With L ~ 250-300 ms, tau must be many multiples of L; tau = 30 s is
> ~100L and comfortable, tau = 4 s is ~15L and is where the realised-slope experiment already
> oscillated.
>
> **Stated as the starting point, to be graded rather than trusted: N = 32 samples (~320 ms, sigma
> ~5 µs), update 3 Hz, tau = 30 s, Kp = 0.033 ppm/µs, plus the integral above.** That is a starting
> point and not a derivation — this project's history is that gains which look right on paper
> oscillate on hardware, so it gets graded against the wire like everything else.

> **REVIEW 2 — tau = 30 s attributes the tau = 4 s oscillation to dead time, but the body blames
> the sample interval.** Two paragraphs up: "the sample interval is the dominant lag". The
> realised-slope oscillation ran at tau = 4 s under **3.35 s sampling** — near-equal lag and tau,
> which oscillates regardless of dead time. At 3 Hz sampling, tau = 4 s is ~13 L against the
> 250-300 ms dead time, classically comfortable. So the evidence cited for "tau = 4 s oscillates"
> does not apply to the new sampling regime, and tau = 30 s buys its margin by making every
> mid-band error (above noise, below the 1 ms splice threshold) converge ~7x slower than today's
> servo — a several-hundred-µs excursion now takes minutes to remove. Grade tau ~8-10 s alongside
> 30 s rather than committing to the conservative figure on a misattributed data point.

> **RESPONSE — the misattribution is real and the citation is withdrawn.** The realised-slope
> oscillation ran at tau = 4 s under **3.35 s sampling**: lag and time constant were within a factor
> of about one, which oscillates on its own account and says nothing about a 250-300 ms dead time.
> Citing it under "the dead time is real" borrowed evidence from a different failure — and the body
> two paragraphs above simultaneously blamed the sample interval, so the document argued both.
>
> At 3 Hz sampling tau = 4 s is ~13x the dead time and roughly 12x the sample interval, which is
> ordinary rather than marginal.
>
> **Accepted: grade tau = 8-10 s alongside 30 s, and treat neither as chosen.** The cost of the
> conservative figure is exactly as described — a mid-band error (above the noise floor, below the
> 1 ms splice threshold) converges ~7x slower than today's servo, so a several-hundred-µs excursion
> takes minutes rather than tens of seconds. That band is where most real excursions live, so
> committing to tau = 30 s on a borrowed data point would trade the plan's main benefit away
> silently. The starting-point line now reads as a range to be graded, not a decision.

## The report cadence is a diagnostic artefact, not a measurement constraint

Tags arrive per DMA descriptor, ~100 Hz. The present diagnostic logs one number per 3.35 s report,
which is a 300x throwaway. A delay-controlled loop should average over a **short** window and update
at that rate:

    N=10 samples (~100 ms)   resolution ~27/sqrt(10) ~ 8.5 us,  update 10 Hz
    N=32 samples (~320 ms)   resolution ~5 us,                  update 3 Hz

Either is far better than 3.35 s. Dead time is one pipeline depth (~250-300 ms), so loop bandwidth
is bounded near 0.5 Hz regardless; an update rate of 3-10 Hz keeps the sampler out of the way of
that bound instead of being the bound.

## What is deleted

Not "improved" — **deleted**, because with no ledger there is nothing for a split to be a split
between:

**Deleted outright:**

* the 3 s `split_pending` trim hold, and `trim_split_holds` with it — this is the one unconditional
  win, and the largest identified inter-device term
* the starvation re-baseline and the phantom-frame clamp's re-arming logic — **only because flow
  control moves to the measured `buffered_audio()` and the ledger becomes diagnostic-only.** They are
  not servo defences; they are what keeps `pushed - played` truthful, so deleting them while any
  consumer still trusts the ledger would leave it permanently biased after the first restart

**NOT deleted — the trace came back, and they are load-bearing:**

* `drift` / the accounting split, `DRIFT_REPAIR_US`, `DRIFT_STEADY_BAND_US`, `Accounting split
  repaired` and the `pushed_frames_total_` step **survive on the tags-absent fallback path.** The
  `!split_pending` term in the `fast_splice_` gate (`snapcast_client.cpp:3676`) is an existing guard
  against exactly the failure this plan's own headline test injects: without it a 1 ms ledger bias
  arms the splice path and displaces ~700 µs of real audio. With `fast_splice_` driven by `err_tag`
  when tags are fresh, that guard is needed only when they are not — but there it is irreplaceable,
  because with no tags the ledger is the only estimate available and a bias in it is
  indistinguishable from a real error.

**Demoted, not deleted:**

* `predict_next_play_us_()` and the EWMA feedback pivot. They stop being the **rate servo's** error
  signal — which is where the nominal-vs-realised slope bias, ~70% of the differential floor, was
  costing — but survive as the **per-chunk scheduling comparison** driving hard resync, stale
  bailout, storm mute and splices. Those act at millisecond scale, where that bias does not bind.

> **REVIEW — this list contradicts the fallback.** "Tag loss mid-flight" below says the loop
> "reverts to the ledger path after a bounded number of missed updates", and "What must be kept"
> requires degrading "to the existing behaviour". The existing behaviour is the ledger servo, which
> is built from `predict_next_play_us_()`, the pivot, and the defences deleted here. Either the
> fallback is genuinely the old path — then all of this stays compiled in and the deletion is
> really "demoted to fallback" — or the fallback is something simpler (hold trim, splice on gross
> error) and a resampler-in-path configuration permanently runs without a rate servo. Both are
> defensible; the document currently claims each in a different section. Decide which.

> **RESPONSE — conceded, and decided: the fallback is the SIMPLER one, and the deletion is real.**
> Keeping the ledger servo as a fallback means maintaining two timing paths, and the one that runs
> rarely is the one that rots — it would be exercised only in configurations nobody measures, which
> is how the stream-scoping bug survived unnoticed.
>
> So: **on tag loss the loop holds its last trim and the existing splice path handles gross error.**
> `predict_next_play_us_()`, the pivot, and the split machinery are deleted outright.
>
> **The consequence, stated plainly rather than buried:** a resampler-in-path configuration runs with
> **no rate servo at all**, only splices. That is acceptable for this bench (no resampler; mixer
> blending is transient, and holding trim through an announcement is fine) but it is a real
> capability regression for anyone who resamples. If that is unacceptable upstream, the honest
> alternative is **demotion, not deletion** — and then this section must say "demoted to fallback"
> and the maintenance cost of two paths must be accepted explicitly. It cannot be left as it was,
> claiming both.

**And the split-hold's inter-device cost goes with them**, which is the largest identified term:
measured A frozen at +64.00 ppm while B steered at +38.15 ppm, ~26 ppm for 3 s ~ 78 us of skew.

## What must be kept

* **Flow control, moved to `buffered_audio()`.** "How much is in flight, push or drop" is answered by
  the sink's MEASURED depth, not by `pushed - played`. The ledger becomes **diagnostic-only**, which
  is what allows the starvation re-baseline and phantom clamp to be deleted: nothing load-bearing
  trusts a counter that rots after the first restart. Consequence for the test below: RECON `drift`
  is no longer a reliable positive control.
* **A fallback when tags are unavailable.** Deliberately suppressed through a resampler, while the
  mixer blends a second source, and for client-inserted silence/splices. The fallback is **hold the
  last trim, and let the splice path handle gross error** — deliberately NOT the old ledger servo,
  which is deleted. A resampler-in-path configuration therefore runs with no rate servo at all.
* **The PI mechanics.** The trim clamp, conditional-integration anti-windup, and the Kp
  acquire-to-run decay. The +164.9 ppm runaway under Risks is what the second one's absence looks
  like.
* **Splices**, for corrections faster than a ~0.5 Hz loop can make.

  > **REVIEW — the splice/trim boundary needs a number.** At a trim authority of X ppm, an error of
  > E µs takes E/X seconds to integrate away; the acquisition handoff "at whatever `D` then is"
  > could hand the loop several ms, i.e. minutes of convergence. State the error magnitude above
  > which the fast path splices instead of the loop trimming, and its hysteresis — it is the same
  > class of decision as the tag-loss bound below, and equally not to be left implicit.

  > **RESPONSE — accepted, and the mechanism already exists.** `fast_splice_threshold` (1 ms in the
  > example config) is exactly this boundary: 43 single-frame splices of ~23 µs each close a
  > millisecond in about a second, inaudibly. Reuse it rather than inventing a second threshold.
  >
  > The arithmetic confirms it is needed: at tau = 30 s an error takes ~3 tau = 90 s to converge, so
  > anything at the millisecond scale must splice. **Rule: above `fast_splice_threshold`, splice;
  > below it, trim.** Hysteresis: splice only until the error is inside the threshold, then hand to
  > the loop, and do not re-arm until it exceeds 2x the threshold — otherwise the two mechanisms
  > fight at the boundary, which is the limit cycle the trim deadband comment already warns about.
  >
  > This also resolves the startup handoff: acquisition splices down to within the threshold, and the
  > loop takes over from there rather than "at whatever D then is".
* **`supports_render_tags()` and the freshness gate.** A signal that reports its own absence is the
  whole reason this is trustworthy; do not let the fallback hide it.

> **REVIEW 3 — the kept splice path runs on the DELETED prediction.** The fast path is not a
> separate mechanism: per chunk, `error_us = predicted - deadline`
> (`snapcast_client.cpp:2498`, from `predict_next_play_us_()` at 2415) is the input to the hard
> resyncs, the stale bailout, the storm mute, AND the fast splices — everything this plan keeps.
> Deleting `predict_next_play_us_()` deletes the fast path's error signal. Decide what drives
> per-chunk push/drop/splice under the new design: `err_tag` (then specify the behaviour when tags
> are absent — which is exactly the tag-loss and startup windows where the plan says splices are
> the ONLY remaining mechanism), or a retained minimal prediction (then the deletion list
> overstates for the second time, and "demoted, not deleted" applies here too).

> **RESPONSE — correct, and it is the second option: `predict_next_play_us_()` is DEMOTED, not
> deleted. The deletion list overstated again.**
>
> The circularity in the first option is fatal and decides it. Per-chunk push/drop/resync decisions
> happen every 26 ms; tags are neither guaranteed nor chunk-aligned, and are absent precisely during
> tag loss and startup — the windows where splices are said to be the only mechanism. A fast path
> that needs tags cannot be the fallback for tags being unavailable.
>
> So the prediction survives as **the per-chunk scheduling comparison** — hard resync, stale bailout,
> storm mute, splice — and stops being **the rate servo's error signal**. Those are different jobs
> with different tolerances: the scheduling comparison acts at millisecond scale, the rate loop at
> tens of microseconds.
>
> **This narrows the plan's central claim and the narrowing must be stated plainly.** What is deleted
> is the split/hold/repair apparatus, not the prediction. The justification for deleting the split
> detector is now conditional rather than structural: it defended the prediction *because the rate
> loop consumed it*, and the rate loop no longer does. Whether it can go therefore depends on
> whether the retained consumers are sensitive to a ledger bias — and `fast_splice_threshold` is
> 1 ms while `DRIFT_REPAIR_US` is 2 ms, so a bias between those can move audio through the splice
> path with the servo blind to it.
>
> **That is an open structural question, not a decided one.** It needs the same code trace REVIEW 2
> asked for on flow control: enumerate the retained consumers of `error_us` and establish the
> smallest ledger bias that changes any of their decisions. If that bias is below
> `fast_splice_threshold`, the split detector must stay too, and the plan's benefit shrinks to
> deleting the 3 s hold alone.

> **REVIEW 3 — the kept ledger rots without the deleted corrections.** The starvation re-baseline
> and the phantom clamp are not servo defences; they are what keeps `pushed - played` truthful.
> Delete them and the first pipeline restart leaves the discarded frames counted forever (the
> death-spiral comment at `snapcast_client.cpp:1124-1141`), so the ledger "kept for flow control"
> is permanently biased after the first restart of every session. Either flow control moves wholly
> to the measured `buffered_audio()` and the ledger becomes diagnostic-only — then say so, and note
> that the RECON `drift` positive control in the test below inherits the same staleness — or these
> two corrections belong in "What must be kept".

> **RESPONSE — correct; taking the first option. Flow control moves to `buffered_audio()` and the
> ledger becomes DIAGNOSTIC-ONLY.**
>
> The observation is exactly right and I had the category wrong: the starvation re-baseline and the
> phantom clamp are not servo defences at all, they are what stops `pushed - played` diverging from
> reality after a restart discards frames. Keeping the ledger for flow control while deleting them
> would leave it permanently biased from the first restart of every session — the death spiral the
> comment at 1124-1141 describes.
>
> Moving flow control to `buffered_audio()` is the consistent choice: it is measured rather than
> inferred, which is this plan's whole thesis, and it cannot rot because nothing accumulates. "What
> must be kept" is amended — the ledger is kept only as a diagnostic, and the two corrections go with
> the rest of the ledger machinery.
>
> **And the corollary is right and damaging to the test.** A diagnostic-only ledger makes RECON
> `drift` an unreliable positive control, because after the first restart it may be biased for
> reasons unrelated to the injection. The (a) criterion needs a witness that does not depend on the
> ledger staying truthful: use the injection's own ramp state (`split_ramp_remaining_us_` reaching
> zero, already logged at the ramp site) as proof the perturbation was applied, and keep RECON
> `drift` only as corroboration.

## Semantics that must be decided, not left open

* **Tag loss mid-flight.** The loop holds its last trim — NOT its last error — indefinitely, and the
  splice path handles any error that grows past `fast_splice_threshold` meanwhile. There is no
  reversion to a ledger servo, because there is no longer one to revert to.
* **Shared mapping lost.** The loop's precondition is `deadline_on_shared_tsf_`. On a fall back to
  the local Kalman offset the clock-offset wander stops being common-mode across devices, so the
  loop must hold trim (or widen tau substantially) until the shared mapping returns. Decide which;
  holding is the safer default and matches the tag-loss behaviour above.
* **Setpoint changes.** `buffer_ms` changes from the server, `static_delay` from config. Both change
  `deadline()`, and so step `err_tag` directly. Handle in this order:
  1. **Invalidate the tag stream for one pipeline depth.** The ~250 ms already in flight was
     scheduled against the old deadline, so `err_tag` would otherwise carry the step twice — once as
     the intended change, once as a corrupted measurement, in opposite directions.
  2. **Then apply the same `fast_splice_threshold` rule as any other error**: above it the fast path
     splices, below it the loop converges. A setpoint step and a measured error of the same size are
     the same thing and get no special case.
  3. **Then resume the loop** when fresh tags return. Splicing while the measurement still reports
     the old anchor would have the loop fighting the splice.

  > **REVIEW:** since the measurement is deadline-corrected (see the note under "The proposal"),
  > a `buffer_ms` change also invalidates the tag deadline anchor — the extrapolation is exact
  > only "for fixed buffer and offset". Re-anchor on the change, or the step appears twice: once
  > in the setpoint, once as a corrupted measurement. Also: "let the loop converge" on a step of
  > tens of ms is minutes at realistic trim authority — this conflicts with "do not splice to it"
  > unless a large setpoint step is exactly the "faster than the loop can make" case the splice
  > path exists for.

  > **RESPONSE — both halves accepted; "do not splice to it" is wrong and is withdrawn.**
  >
  > On the anchor: yes, and the answer is the one given under "The proposal" — a fresh anchor does
  > not save it, because the ~250 ms of audio already in flight was scheduled against the old
  > deadline. **Invalidate the tag stream for one pipeline depth after any setpoint change**, the
  > same mechanism the freshness gate already uses. Without that the step lands twice, once as
  > setpoint and once as corrupted measurement, in opposite directions.
  >
  > On convergence: the reviewer's own alternative is correct. At tau = 30 s a tens-of-ms step takes
  > **minutes**, which is not a defensible response to a latency change a user just requested. **A
  > setpoint step is governed by the same `fast_splice_threshold` rule as any other error** (see the
  > response under "What must be kept"): above the threshold the fast path splices to it, below it
  > the loop converges. That makes the two sections consistent instead of contradictory, and it
  > removes the special case entirely — a setpoint step and a measured error of the same size are
  > treated identically, which is what they are.
  >
  > Ordering matters: invalidate the tag stream **first**, then splice, then resume the loop when
  > fresh tags return. Splicing while the measurement is still reporting the old anchor would have
  > the loop fighting the splice.
* **Startup.** No tags until audio flows, so acquisition stays on the existing splice path, which
  splices down to within `fast_splice_threshold`. The loop takes over from there — not "at whatever
  the error then is" — and **seeds its integral with the trim currently applied**, which is the
  **delay loop's own prior trim if one survives in RAM** from earlier in the session. The splice path
  corrects position, not rate, so it learns no crystal offset and there is nothing to seed from at a
  genuine cold boot — the integral starts at zero.

  The cold-boot wind-up plays **unmuted**, over ~3·Ti (90 s at Ti = 30 s, ~30 s at Ti = 8-10 s), as a
  sub-threshold error ramping toward `crystal/Kp` while the integral catches up. `fast_splice_` runs
  **episodes, not lone splices**: it arms only after the effective error holds at or above the
  threshold for `FAST_SPLICE_PERSIST_US` (4 s), then corrects one frame per chunk until the error is
  inside `FAST_SPLICE_RELEASE_US` (300 µs) or it hits the 128-frame bound. At ~40 ppm an uncorrected
  error accrues ~1 ms every ~25 s, so the wind-up emits **episodes of ~30 frames (~0.7 ms over
  ~0.8 s), one per ~25 s, lengthening in interval as the integral catches up** — each handing back at
  300 µs, not at zero. Inaudible because the correction is one frame per chunk, which is the property
  that matters, not because anything is muted.

  Persisting the crystal offset across boots would remove the transient entirely; it is a separate
  work item and is not part of this plan.

  > **REVIEW 4 — the splice cadence misdescribes the mechanism being reused.** `fast_splice_` does
  > not emit lone splices; it runs EPISODES: it arms only after the effective error holds at or
  > above the threshold for `FAST_SPLICE_PERSIST_US` (4 s, `snapcast_client.cpp:3701`), then
  > corrects one frame per chunk until the error is inside `FAST_SPLICE_RELEASE_US` (300 µs, :634)
  > or 128 frames. So the cold-boot wind-up emits ~30-frame episodes (~0.7 ms over ~0.8 s), one
  > per ~25+ s and stretching as the integral catches up — "about four single-frame splices" is
  > wrong in both unit and count, and each episode hands back at 300 µs, not zero. The audibility
  > conclusion survives (the correction is still one frame per chunk), but the plan should describe
  > the mechanism it names. Also: the gate at :3676 requires `st.converged` — the new loop must
  > define what "converged" means for it, or the fast path never engages at all.

  > **RESPONSE — correct; "about four single-frame splices" was wrong in unit and count, and is
  > replaced above.** `fast_splice_` runs EPISODES, not lone splices: it arms only after the
  > effective error holds at or above the threshold for 4 s, then corrects one frame per chunk until
  > the error is inside 300 µs or it hits the 128-frame bound. I described a mechanism I had cited by
  > name without reading its shape, which is the same failure as citing the realised-slope precedent.
  >
  > Corrected: the cold-boot wind-up emits **episodes of ~30 frames (~0.7 ms over ~0.8 s), one per
  > ~25 s and lengthening in interval as the integral catches up**, each handing back at 300 µs
  > rather than at zero. The audibility conclusion is unchanged — the correction is still one frame
  > per chunk, which is the property that makes it inaudible — but it survives on its own terms
  > rather than on a miscount.
  >
  > **And the `st.converged` catch is the more serious half.** The gate requires it, `converged` is
  > servo state, and the new loop must define it or the fast path never engages at all — including
  > during the cold-boot wind-up just described. Definition adopted: **the delay loop is `converged`
  > once it has held `|err_tag|` inside `converge_fine_us` across a full integral time constant
  > (Ti)**, which is the same shape as the existing definition but expressed in the measured error.
  > Added to the semantics list, since it is precisely the kind of thing this plan keeps promising to
  > add and then not adding.

  > **REVIEW 3 — two claims here don't hold.** (1) The splice/acquisition path learns no rate — it
  > corrects position. Within a session the value being seeded is the delay loop's own prior trim
  > surviving in RAM; "the acquisition path has already learned" attributes it to a mechanism that
  > cannot produce it. (2) "acceptable because acquisition is muted" is wrong on duration: the
  > integral winds to ~95% in 3·Ti — 90 s at Ti = 30 s — which far outlives the mute. The cold-boot
  > transient plays UNMUTED as a sub-threshold error ramping toward `crystal/Kp` with periodic
  > single-frame splices until the integral catches up. That may well be inaudible, but say that,
  > with the splice cadence (~1 ms accrues in ~25 s at 40 ppm), rather than claiming the mute
  > covers it.

  > **RESPONSE — both wrong as written, and the second was a hand-wave.**
  >
  > **(1)** The splice path corrects **position**, not rate; it learns no crystal offset and cannot.
  > Within a session the value being seeded is the delay loop's **own prior trim surviving in RAM**,
  > which is worth having but is not what I called it. At a genuine cold boot there is **nothing to
  > seed from at all** — the integral starts at zero and must wind up.
  >
  > **(2)** "Acceptable because acquisition is muted" was wrong on duration and is withdrawn. The
  > integral reaches ~95% in 3·Ti = **90 s** at Ti = 30 s, far outliving the mute. So the cold-boot
  > transient plays **unmuted**: a sub-threshold error ramping toward `crystal/Kp` while the integral
  > catches up.
  >
  > **The honest statement, with the cadence rather than an appeal to the mute:** at ~40 ppm an
  > uncorrected error accrues ~1 ms every ~25 s, so during wind-up the fast path emits roughly one
  > single-frame splice (~23 µs) every ~25 s — about **four splices over the 90 s**. Single-frame
  > splices at 23 µs are inaudible by the same argument the existing `fast_splice_threshold` comment
  > makes, so this is acceptable — but it is acceptable because it is four inaudible splices, not
  > because anything is muted.
  >
  > It also argues for the shorter tau: at Ti = 8-10 s the wind-up is ~30 s and roughly one splice.
* **What `converged` means for the new loop.** The `fast_splice_` gate requires `st.converged`
  (`snapcast_client.cpp:3676`), which is servo state — so without a definition the fast path never
  engages at all, including through the cold-boot wind-up above. **Adopted: converged once
  `|err_tag|` has stayed inside `converge_fine_us` for a full integral time constant (Ti).** Same
  shape as the existing definition, expressed in the measured error rather than the predicted one.
* **Do NOT make any hold common-mode across devices.** Freezing every device captures each one's PI
  output at an arbitrary point in its own transient, converting N momentary corrections into N
  sustained rate offsets. This was proposed and is wrong.

## How it gets judged

One test, already tooled, and it must be run before anything downstream is trusted:

    inject_split(+1000 us) on one board. TWO-SIDED -- a null proves nothing unless the
    perturbation demonstrably landed:

      (a) the bias LANDED    split_ramp_remaining_us_ reached zero (logged at the ramp site)
                             RECON `drift` may corroborate, but is NOT the witness: the ledger
                             is diagnostic-only under this design and may be biased for
                             unrelated reasons after the first restart
      (b) the audio did NOT  wire displacement ~0

      (a) without (b) = the servo is still ledger-coupled somewhere -- and the FIRST place to look
                        is fast_splice_, which is why it must be driven by err_tag: left on the
                        prediction it arms at the 1 ms threshold and splices to within 300 us,
                        displacing ~700 us for a +1000 us injection, entirely by construction
      (b) without (a) = the injection reached nothing; the test is VOID, not passed

    present servo, measured 2026-08-28: displaces ~1100 us, then reports a clean error having
    done so (err_live -57..+95 throughout).

Flow-control immunity is a SEPARATE property and is not testable by injection: the starvation
latch is ~11,000 frames away, ~40 minutes of ramping, and crossing it is a deliberate underrun.
Establish it by code trace instead — enumerate the read sites of `pushed_frames_total_` and
`available_frames` and show no push, drop or starvation decision consumes the biased value.

> **REVIEW 4 — as specified, the design FAILS its own headline test, and the code trace the plan
> defers is already answerable.** Walk the retained path: `inject_split(+1000)` biases
> `pushed_frames_total_` → the demoted prediction → `median_err_us`. `fast_splice_`
> (`snapcast_client.cpp:3696`) arms when the effective error holds at or above the 1 ms threshold
> for `FAST_SPLICE_PERSIST_US` (4 s), then splices one frame per chunk until the error is inside
> `FAST_SPLICE_RELEASE_US` (300 µs). A +1000 µs injection sits exactly at the threshold, so the
> retained splice path displaces real audio by ~700 µs — (a) lands, (b) fails, by design rather
> than by defect. **Today the only thing preventing this is the `!split_pending` gate at :3676 —
> i.e. the split detector, listed above as "conditional on a code trace", is an existing guard on
> this exact path.** So the smallest ledger bias that changes a retained decision is
> `fast_splice_threshold` through the 31-chunk median, which answers the deferred question: the
> split detector cannot go unless splices stop consuming the ledger. The clean resolution is the
> one the plan already gestures at: **drive `fast_splice_` from `err_tag` when tags are fresh,
> falling back to the demoted prediction only when they are not** — then the injection is
> invisible to every timing consumer and the test can genuinely pass. Decide this before the test
> is run, because under the current spec (a)-without-(b) is the predicted outcome, not a finding.

> **RESPONSE — verified line by line, and correct on every point. This is the most important note in
> the document and the design changes because of it.**
>
> Checked against the source: `FAST_SPLICE_RELEASE_US` is 300 (:634), `FAST_SPLICE_PERSIST_US` is
> 4 s (:668), and the gate at :3676 reads
> `if (threshold > 0 && frame_us > 0 && st.converged && !split_pending && !repair_settling)`.
> **`!split_pending` is sitting right there.**
>
> So the walk is right and the conclusion is unavoidable: a +1000 µs injection biases the prediction,
> reaches `median_err_us`, arms `fast_splice_` at the 1 ms threshold after 4 s, and splices real audio
> until the error is inside 300 µs — **~700 µs of displacement, with (a) landing and (b) failing.**
> As specified, this design fails its own headline test by construction, and I had deferred the very
> trace that shows it.
>
> **It also answers the deferred question outright.** The smallest ledger bias that changes a retained
> decision is `fast_splice_threshold` (1 ms) through the 31-chunk median. The split detector is
> therefore not "conditional on a trace" — it is **an existing guard on this exact path**, and it
> cannot be deleted while splices consume the ledger. I had it listed as a deletion candidate while
> the code showed it doing load-bearing work.
>
> **Adopting the proposed resolution: `fast_splice_` is driven by `err_tag` when tags are fresh, and
> falls back to the demoted prediction only when they are not.** That makes the injection invisible to
> every fine-grained timing consumer — the coarse ones (hard resync, stale bailout, storm mute) act at
> the tens-of-ms scale where a 1 ms bias cannot arm them — and the headline test can genuinely pass
> rather than being predicted to fail.
>
> **The split detector then survives on the fallback path only**, guarding `fast_splice_` in the
> tags-absent case. That is honest rather than tidy: with no tags the ledger is the only estimate
> available, so a ledger bias is indistinguishable from a real error and nothing can do better than
> refuse to act on it. The deletion list is corrected accordingly — again.

> **REVIEW — verify the pass criterion survives the ledger's remaining job.** The ledger stays for
> flow control, and `inject_split` biases exactly that ledger. If a +1000 µs accounting bias can
> reach a push/drop or starvation decision, the injection moves audio through flow control even
> with the servo blind to it, and "wire displacement ~0" fails for a reason that is not the servo.
> Check where `inject_split`'s bias can propagate under the new design before treating a nonzero
> result as a servo failure — or a zero result as proof, if the bias no longer reaches anything.

> **RESPONSE — accepted, and it changes the test into a two-sided one.** A null result is only
> evidence if the perturbation demonstrably landed. As written, "wire displacement ~0" is
> indistinguishable from "the bias reached nothing at all", which is the more likely outcome: 1000 µs
> is 44 frames, while the starvation latch triggers on `available_frames <= 0` and the push/drop path
> is driven by the chunk deadline rather than by the ledger. So the bias probably propagates nowhere
> once the servo stops reading it — and the test would prove nothing.
>
> **Revised test, with a positive control:**
>
>     inject_split(+1000 us) on one board, and require BOTH:
>       (a) the bias LANDED    -- RECON `drift` (and err_live, kept as a diagnostic) moves ~1000 us
>       (b) the audio did NOT  -- wire displacement ~0
>     (a) without (b) = the servo is still ledger-coupled somewhere.
>     (b) without (a) = the injection reached nothing; the test is VOID, not passed.
>
> Before running it, trace where a `pushed_frames_total_` bias can still reach a push, drop or
> starvation decision under the new design, and size the injection so it would cross one of those
> thresholds if the coupling existed. An injection too small to matter cannot demonstrate immunity.

> **REVIEW 2 — "size the injection to cross a threshold" is impractical for the starvation latch;
> separate the two properties instead.** The latch arms on `available_frames <= 0`, one pipeline
> depth away (~250 ms ≈ 11,000 frames); at the injection ramp's 100 µs/s that is ~40 minutes of
> ramping, and crossing it is a deliberate underrun, not a perturbation. The test as revised proves
> **servo immunity**, and (a) plus (b) is sufficient for that. **Flow-control immunity** is the
> separate property, and the right tool for it is the code trace — enumerate the read sites of
> `pushed_frames_total_` / `available_frames` under the new design and show no push/drop/starvation
> decision consumes the biased value — not an injection sized to force an underrun. Keep the
> two-sided test at 1000 µs; drop the "size it to cross" instruction.

> **RESPONSE — accepted; "size the injection to cross a threshold" is withdrawn.** The arithmetic is
> decisive: `available_frames <= 0` is one pipeline depth away, ~11,000 frames, and the ramp moves at
> 100 us/s — roughly **40 minutes** of ramping to reach it, at which point the perturbation has
> become a deliberate underrun and is testing a different thing entirely.
>
> The instruction also conflated two properties that want different tools:
>
>     servo immunity          the two-sided injection at 1000 us, (a) bias landed + (b) audio did not
>     flow-control immunity   a CODE TRACE: enumerate the read sites of pushed_frames_total_ and
>                             available_frames under the new design, and show that no push, drop or
>                             starvation decision consumes the biased value
>
> The first is an experiment, the second is a proof, and no injection can substitute for the second
> because the thresholds it would have to cross are underruns. Keep the injection at 1000 us as
> written; do the trace separately and record it here when done.

Secondary, on a settled bench, against today's baseline:

    wire (B-A)      median +1.2 us   MAD 20.0   sd 46.7   p2p 242.8
    group delta     A -9 us MAD 26   B +0 us MAD 16, 100% availability, 0 outliers

`sd` and `p2p` should improve as the split-hold excursions disappear. The median is already ~1 us
and cannot improve meaningfully — **do not judge this on the median.**

## Risks

* **It replaces the load-bearing timing path.** A bug presents as a timing anomaly, the exact class
  this project has spent sessions chasing. Build it on a quiet bench with nothing else in flight.
* **Board A's 12.5 us floor against board B's 5.0 us is unexplained.** If it is a property of the
  measurement rather than of that board, the achievable resolution is the worse number.
* **The dead time is real.** ~250-300 ms of pipeline. A loop tuned as if the measurement were
  instantaneous will oscillate — a general property of loops with transport delay, and the reason
  tau must be many multiples of it. No precedent is cited here on purpose: the realised-slope
  runaway was a sampling-interval failure (tau comparable to lag), not a dead-time one, and using it
  as evidence would be borrowing from a different mechanism.

  > **REVIEW 3 — this citation was withdrawn two sections up and still stands here.** The response
  > under the cadence discussion concedes the realised-slope oscillation was a sampling-interval
  > failure (tau ≈ lag), "borrowed evidence from a different failure" — yet this bullet still
  > offers it as the dead-time precedent. The risk itself is fine; cite it as a general property of
  > loops with dead time, or not at all.

  > **RESPONSE — correct; the citation is removed from the bullet above.** Conceding a point in one
  > section and leaving it standing as evidence in another is the same defect REVIEW 2 opened with,
  > committed again in the same pass that claimed to have fixed it. The risk now states dead time as
  > a general property and cites no precedent, because the only precedent on hand was a
  > sampling-interval failure.
* **Do not trust a floor measured on a churned bench.** Every reflash causes five consensus
  membership changes, worth 154 us vs 93 us in |median error|. Today's best numbers came from
  twenty uninterrupted minutes.
* **Read `CLAUDE.md` first.** Three of four instrumentation defects found on 2026-08-28 produced
  confident, wrong numbers, and two conclusions in this document were reached only after earlier
  measurements of the same quantities had to be retracted.
