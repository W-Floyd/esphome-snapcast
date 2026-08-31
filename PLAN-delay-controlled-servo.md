# PLAN — control the measured transport delay, not a model of it

> **CURRENT STATE (2026-08-29, build 14 `29ca74f`) — read this, skip the log below unless you
> need the evidence.** Live on all boards: PI on `err_tag`, tau 10 s, Ti 120 s, block 32, flat Kp;
> integral persisted (300 s EMA) and restored at boot; out-of-range → hold integral (snap to EMA if
> > 20 ppm off); tag-loss/mapping-flap → hold integral + P decaying over tau; tag stream blanked on
> setpoint change, re-anchor, source switch and feedback gaps > 50 ms; `fast_splice_`, hard resync,
> storm mute and aggressive catch-up all decide on `err_tag` while tags are live, the ledger
> prediction only as the fallback; the split repair disarmed while tags are live; runtime tunables
> over `servo_param`; one-sided autotune available, off. Wire: sd 46.7 / p2p 243 µs → sd 8.9 /
> p2p 45 (11.5 quiet minutes, zero events). Remaining audible events are server-side delivery
> pauses. Open items are listed in `HANDOFF.md`.
>
> **IMPLEMENTED 2026-08-28** in `snapcast_client.{h,cpp}`; see `delay_loop_update_` and the
> constants at `DL_BLOCK_N`. Decisions taken where the plan left a range, and one deviation it
> forces on its own logic:
>
> * **tau = 10 s** (`DL_KP_RUN_PPM_PER_US` 0.1, Ti = tau, Ki = Kp²) as the starting point — the
>   plan's three independent arguments all favoured the shorter figure. Grade against 30 s on the
>   wire before trusting it.
> * **The starvation re-baseline and phantom clamp are KEPT**, against the "deleted outright"
>   bullet. Its own caveat is why: they may go "only because ... the ledger becomes
>   diagnostic-only", but the DEMOTED prediction (hard resync, stale bailout, storm mute, splice
>   fallback) still consumes the ledger, so it is not diagnostic-only and deleting its
>   truth-keepers reintroduces the post-restart death spiral the comment at
>   `notify_audio_played()` records. This is the same argument REVIEW 3 made for flow control,
>   applied to the consumer the deletion list itself retains. Nothing else about them changed.
> * **`converged` stays median-based** (the existing latch, cleared by mute/hard events) rather
>   than the adopted err_tag definition: it is the UNMUTE audibility gate, driven by the retained
>   scheduling comparison, and an err_tag-based latch would deadlock unmute in exactly the
>   tags-absent configurations the fallback exists for. The property the redefinition protected —
>   a ledger bias must not move audio through the splice path — is held instead where it lives:
>   `fast_splice_` consumes err_tag while fresh, and the split-pending guard survives on the
>   prediction fallback.
> * **The split detector and repair are kept in full** (the trace's answer): their one remaining
>   job is guarding the splice fallback. The 3 s split-pending TRIM hold and `trim_split_holds`
>   are deleted — the rate loop no longer reads the prediction, so there is nothing to hold
>   against.
> * The splice in-flight horizon is derived per chunk as pipeline-depth-chunks + half the
>   averaging block (from the ledger depth, used only as a window LENGTH in whole chunks — a
>   multi-ms bias is sub-quantum there); the prediction fallback keeps the historical 15.
> * Tag-stream blanking (one pipeline depth, `DL_SETPOINT_BLANK_US`) fires on setpoint changes
>   AND on every `mark_kp_event_` (hard resync, timebase re-anchor) — those displace audio or
>   step the mapping, so in-flight tags describe the old placement.
> * The proposed "re-arm only above 2x the threshold" splice hysteresis was NOT added: it predates
>   the round that read `fast_splice_`'s real shape, which already carries bench-tested
>   anti-chatter (4 s persistence to arm, release at 300 µs, repair holdoff). Reusing the
>   mechanism means reusing its hysteresis; only the in-flight horizon changed.
>
> **RUN 2026-08-28 ~18:29, board B (f04d74): the two-sided `inject_split(+1000)` test PASSED.**
> (a) the bias landed — SYNCX `drift` stepped −1 → **+1020 µs** (45 frames) and stood; (b) the
> audio did not move — B's DLLOOP err stayed inside its ±150 µs settle wobble through ramp,
> 90 s of standing bias, and the negated restore (drift back to +23 µs); the group render delta
> stayed at its baseline; no fast splice engaged. The old servo displaced ~1100 µs on this exact
> injection the same morning. Also observed live: board A carried a NATURAL sawtooth split
> (drift alternating −49 ms ↔ −1 µs, the mixer-ring artifact, correctly refused by the
> steadiness band) while its measured error stayed double-digit µs — ledger immunity on a
> disturbance nobody injected.
>
> **Found on first flash and fixed: the trim half of the threshold rule was not implemented.**
> "Above `fast_splice_threshold`, splice; below it, trim" — the first build's PI ran at ANY error
> magnitude. Measured consequences: engaged at err +15081 µs on a boot and slammed to the
> +1000 ppm rail, and wound the integral to −994 ppm chasing a delivery stall's displaced audio
> (B, 18:35, buffer 1750→574 ms, ~2900 frames dropped by the coarse path with the loop live under
> it). Second build holds the trim and skips seeding while |err_tag| ≥ the splice threshold —
> the fast path owns that regime; the loop keeps publishing its error for it.
>
> Also captured (pre-existing, not delay-loop): B's late-side stale bailout reconnected mid-stall,
> the new session started playback before first time sync, and there is **no early-side bailout**
> — it wedged inserting silence at err ≈ 10.96×10¹² µs (a collapsed clock mapping) for 5+ minutes,
> feeding a ~17 ms-spread estimate into the group consensus, until manually rebooted. In
> `b.log` around 18:36–18:41 for a post-mortem.
>
> **Found by digging into the recurring wire events (19:12–19:20): SPEAKER-CALLBACK STALLS
> corrupt every local render timestamp, and no existing gate catches them.** The task stalls
> 60–1500 ms (measured fleet-wide: A 709/1499 ms, B 92–439 ms, observer 210–1110 ms — systemic
> and pre-existing); the DAC keeps draining DMA, and on resume the queued completions are stamped
> with a late "now". Both the measured and inferred render phases then spike by ~the stall length
> (they share those stamps — +141/+263 ms published into the group at the 19:12/19:13 events,
> `group=` polluted to −85 ms), and the same stamps feed err_tag. The tag-age gate passes them
> because both sides of the age are the same late clock; TSF was proven clean (574 consecutive
> sandwiches within ±12 µs across a spike). Fixed in build 4: a feedback gap > 50 ms (norm ~10 ms)
> blanks the tag stream for one pipeline depth and refuses the phase publish for observations
> rendered inside the blank. **Open: what starves the speaker task for up to 1.5 s** — that is a
> scheduler/driver question, and it also bounds how well ANY render-timestamp instrument can work.
>
> **Found 19:41 (build 4): a DEADLINE SOURCE SWITCH is an unannounced timebase step.** A fell off
> the shared mapping, its local Kalman disagreed by ~29 ms, and the switch stepped the deadline
> and the published phase by that amount with no `Timebase step` log, no epoch bump, no blanking
> — not a consensus move, so nothing reported it. The loop refused it correctly (out-of-range
> hold, integral kept, a full minute at err −29 ms) and the coarse machinery walked the audio
> over audibly (~26 ms inserted, +3.5 ms overshoot, splice-back) — the second "flutter". Build 5
> flags the toggle in `chunk_deadline_us_` and the player loop treats it as a re-anchor (gain
> re-arm + tag blank). **Open, clock_sync-side:** the audible cost of a switch is bounded by the
> local-vs-shared disagreement — steering the idle local Kalman toward the adopted shared mapping
> would make fallbacks seamless; and why A lost the shared mapping at 19:40 at all (beacon/TSF
> availability) is the same wifi-health question as the speaker stalls.
>
> **Found 19:47-19:51 (build 5): an UNCONVERGED DEAD ZONE between the splice threshold and
> converge_fine.** Post-boot under Never-Mute, A sat at err +1.0→+2.0 ms creeping +20 µs/s for
> 4.5 minutes with nothing correcting: the loop out-of-range-holds (correct), `fast_splice_` was
> gated on `st.converged`, the coarse steer fallback lives in the !trim_holds branch (skipped —
> the rate lock's gate passes on `|median| <= converge_fine`), and the aggressive branch needs
> 10 ms. Build 6 lifts the converged gate for the MEASURED error only — it cannot fight the muted
> coarse convergence, which acts on the prediction; the prediction fallback keeps the gate.
>
> **Corrected attribution:** the 19:41 −29 ms event was NOT a mapping/source step — consensus ran
> n=3 spread <1 ms throughout, and the ledger error stayed small while only err_tag and the
> published phase (the two tag-identity consumers) read −29 ms. That is a sustained ~one-chunk
> TAG-IDENTITY error through the media-source/mixer path, self-recovering, drifting at crystal
> rate while it stood. **Open, top of the list:** find it — evidence pinned in `a.log`
> 19:41:22–19:42:18. (Build 5's source-switch announcement stays: the boot-time flapping is real,
> and B's post-boot log shows it firing 4x in 3 min.)
>
> **Builds 7–8 (iteration speed + tuning):** `reflash-speakers.sh` (one build, two OTAs);
> `servo_param(name,value)` API action + `scripts/servo-param.py` for runtime tuning of tau_s,
> block_n, splice_us, gate timings, and the `autotune` master toggle (lag-1-autocorrelation
> adapter on tau, 64-block windows, bounds [5,60] s, WARN-logged, never persisted);
> `scripts/bench/dl-window.py` for one-command window grading; NVS persistence of the integral
> (first-save gate fixed in build 8). Build 8 also: **flat Kp = 1/tau** — the acquire→run schedule
> scaled Ki = Kp² by 4x on every re-arm and source flaps re-armed every ~25 s, winding A's
> integral to +134 ppm; source switches now blank only; and the tag-stale / mapping-lost holds
> program the INTEGRAL, not the last trim (measured holding +192..+236 ppm of boot P term).
>
> **Autotune v1 run 20:26–20:28 (build 8), stopped after 45 s.** Two defects: the r1 estimator
> reported +1.07/+1.04/+1.02 (mixed n and n−1 normalisations over running sums — impossible values,
> instrument first); and the speed-up rule fired identically on both boards within a second
> (10→8.5→7.2→6.1 s) because a standing mean with r1≈1 is exactly what tracking the common-mode
> timebase wander looks like — loop lag and a moving target are indistinguishable on one device.
> Build 9: proper demeaned lag-1 r1 over a stored 64-sample window; **one-sided adaptation**
> (ringing → slow down only; the operator's tau is the floor); persistence first-save gated on
> 60 s of continuous engagement (build 8 re-saved +0.63 within a second of engaging). A 254 ms
> delivery stall on B at 20:27:23 during the run was unrelated (radio class); both loops closed
> the aftermath cleanly at the shortened tau.
>
> **Top clock_sync item, found 20:29–20:36: recurring COMMON-MODE TIMEBASE STEPS of 60–100 ms.**
> Both speakers hard-resynced by the same amount within 0.5 s of each other three times in seven
> minutes (−60 @20:29:45, −65/−62 @20:33:44, +100/+50 @20:36:06), alternating sign; the observer
> logged `Consensus over 1 estimate(s)` at 20:29:44 — the speakers' estimates dropping out of
> and back into the set moves the deterministic mean by tens of ms. Alignment survives (common
> mode) but under Never-Mute every step is an audible skip on every speaker. Not the delay loop:
> it held through each. **Correction, 20:51: every one of these steps lines up with OPERATOR
> activity** — OTA uploads/reboots (20:29:37 build → 20:29:45 step; 20:33:46 reboot → 20:33:44
> step; 20:51:15–18 triple step during the build-10 upload, with the observer logging n=3→1→3 and
> spread 2643 µs) or the replugs (20:36:06). An OTA in progress stalls a speaker's time-sync/beacon
> path, its estimate drops out of the set, and the deterministic mean jumps. CLAUDE.md's warning
> verbatim: the operator was the dominant disturbance. Whether the steps occur on a QUIET bench is
> unmeasured — the 6-minute build-9 window had none. Also: build 9's OTA was silently ROLLED BACK by a replug 40 s after the
> reboot (both boards reported build 8's compile time via API device_info) — verify the running
> build over the API after every flash; restart the logger process instead of replugging.
>
> **Build 9 live (verified via API), 20:41+: autotune v2 holds correctly** — r1 = +0.93..0.95 (valid
> now), both boards flag the identical common-mode window and neither acts. Persistence wrote
> +50.79 (A) but **+98.73 on B while its integral read +46 a minute later**: the ~45 s common-mode
> excursions swing the instantaneous integral by tens of ppm, so a settle gate still samples
> peaks. Build 10 persists a 300 s EMA of the integral instead.
>
> **RESULT — build 9, tau 10 s, autotune v2 armed, 20:44–20:50 (6 min, zero events of any kind
> on either board):** loop error A/B median −41/−45 MAD 152/151 p5..p95 ±375 µs — that is the
> common-mode timebase wander, and A–B r = **+0.997**. What lands between devices: **differential
> error median +2 MAD 10 µs, p5 −23 / p95 +26** (n=326); group render delta A −5 MAD 14, B −7
> MAD 15. Against the morning baseline (wire MAD 20.0 / p2p 243; group delta MAD 26 / 16) the
> on-device differential MAD is half the wire MAD and the group deltas are tighter on both boards.
> Wire confirmation over the same kind of quiet span is the remaining grade.
>
> **WIRE RESULT, build 10, live analyzer window: n=1303 mean −10.9 µs sd 42.9 p2p 134 µs** (morning
> baseline sd 46.7 / p2p 242.8 — peak-to-peak nearly halved). Remaining visible artefact: two ~27 ppm
> rate steps on B lasting a few seconds, each a `deadline on local fallback` hold — holding the
> INTEGRAL drops P instantly while the peer keeps applying P to the same common-mode error, so a
> ~1 s mapping flap costs ~130 µs of differential skew. Build 11: in-range holds carry P and decay
> it toward the integral over tau (short flaps step nothing; long outages still end at crystal);
> out-of-range keeps integral-only.
>
> **RESOLVED AS BENCH INFRASTRUCTURE (21:05): the "A stalls" are GROUP-WIDE delivery pauses from
> the server/network side, present ALL DAY.** At every A stall the observer's and B's rings dipped
> at the same instant (B to 208 ms at 20:36:07; observer to 992 ms at 20:58:12); A simply empties
> first. `no chunk records for 3 s, ring holds 0` census per hour today (A / B): 14h 120/128,
> **15h 263/297**, 18h 12/21, 19h 20/18, 20h 17/13, 21h 8/10 — tonight is a quieter-than-average
> day for it. The snapserver (192.168.1.2) is not this Mac. Not the persist, not the mapping, not
> A's radio (RSSI −30, no WiFi events). Under Never-Mute each pause is an audible skip on every
> board. Investigate the server host / AP; the client cannot fix a source that stops sending.
> Superseded suspect kept below for the record:
>
> **Open suspect, A only: receive stalls 1.5–2.5 min after boot, ending in the stale bailout**
> (20:36:08→13 and 20:58:14→21, lateness growing ~1 s per 2 s, RSSI −30 dBm, no WiFi events, B
> unaffected). Both followed A's first NVS persist of the boot (by 38 s and 4.7 s); A's eight
> earlier boots tonight, none of which persisted early, had no such stall — but one persist
> (20:30:43) had no stall either. Mechanism if real: NVS commit → flash erase/compaction stalls
> the receive path. Natural test: A's next persist is due ~21:08 (10-min gate); a stall within
> a minute of it is strong evidence. `servo_param persist 0` (build 12) turns saves off for a
> controlled test.
>
> **WIRE, build 11, 21:0x: n=1300 mean −0.19 µs sd 3.51 µs p2p 11.8 µs** — from the morning's
> sd 46.7 / p2p 243. Residual: a ±5 µs cyclical differential = the two loops' mismatched
> tracking of the ±375–600 µs / ~60 s common-mode wander (dead time differs per board by ±30 ms,
> visible as `pipeline 247→211` annotations). Levers: longer tau (tracks less of the wander —
> **tau 30 set over the API at 21:09:27 as an A/B**), a smoother consensus mapping (clock_sync),
> or slow feed-forward from the group render delta (deliberately still off).
>
> **Tuning defect found 21:08: Ti = tau (Ki = Kp²) lets the integral swing ~57 ppm p-p chasing the
> wander** (A: +103.6 → +114.4 in 2 s at err +600 µs; crystal is +57). A delivery-pause hold then
> froze it at +114 and the board ran ~50 ppm fast, err to −5.2 ms, splices dragged it back. Ti was
> tied to tau for cold-boot wind-up speed, which NVS persistence made moot. Build 12 (flashed
> ~21:12): `ti_s` tunable, default 120 s (Ki = Kp/Ti → ~2 ppm swing); out-of-range with the
> integral > 20 ppm from its own 300 s average snaps it to the average (A sat at +94 vs +57 for
> minutes with the hold faithfully keeping the wrong value); `persist` on/off knob. tau 30 re-applied
> after the reboot to continue the A/B on the differential residual.
>
> **tau = 30 A/B (21:13–21:17, build 12, Ti 120): WORSE — differential err median +425 MAD 161 µs,
> r = 0.75** (vs +2 / 10 / 0.997 at tau 10). Mechanism: the loop's standing error is
> integral_error / Kp; at Kp 0.033 a ~15 ppm integral misestimate parks ~450 µs (A: median +438
> with integral +50 against a ~+65 crystal), and Ti = 120 s pulls it in only slowly. tau 30 is
> viable only once the integral is within ~1 ppm; reverted to tau 10 at 21:17 with Ti kept at
> 120 s. Confounded A/B (two parameters changed) — noted so it is not repeated. **Wire confirms
> (from `test.csv` via `scripts/bench/wire-window.py`, rival-gated): tau 30 span 21:14:30–21:18:50
> mean +373 MAD 148 sd 202 p2p 1172 µs; after the revert, 21:24–21:25 mean +29 MAD 5.5 sd 7.3
> p2p 21 µs and still draining.** The analyzer CSV is now graded directly, not read off plots.
>
> **How well does the device SEE the wire skew? (21:22–21:27:30, settled, wire averaged per 1 s
> device sample, n=284):** the loop-error differential errA−errB vs the analyzer: **r = +0.88,
> slope 0.87, means +31.3 vs +33.8 µs (bias ~2.5 µs), residual sd 12.8 µs** — an essentially
> unbiased estimate with ~13 µs of per-second noise (→ ~2 µs after a minute of averaging). The
> render-phase difference (phase_b−phase_a, what the group exchange carries) is far worse: sd 46 µs
> settled, r −0.55, bias +70 µs, and +938 µs bias / 786 µs sd during excursions (stall stamps).
> Implication: a slow differential feed-forward should be built on exchanged err_tag, not on the
> render phases, and only after minutes of median filtering. Realistic floor: standing offset →
> ~2 µs; the ±5 µs wander residual (dead-time asymmetry) is the next term. **Analyzer precision,
> measured, not quoted:** `scatter_ns` (median |per-frame skew − within-capture fit|) is 26 ns
> median / 40 ns p90 over the settled span — the wire resolves each capture's B−A offset to tens
> of ns, so every µs in sd 3.5 / p2p 11.8 is real board behaviour. (The "0.71 µs fit floor" in
> the yaml comment is an older, different quantity; retracted as a limit.)
>
> **Build 12 / tau 10 / Ti 120, settled (21:28–21:33): WIRE mean +5.7 MAD 3.9 sd 5.3 p2p 26.6 µs;
> on-device differential median +11 MAD 13, r 0.992.** A ~+6 µs standing offset remains (slow-Ti
> residual). The ±5 ppm `fs_b − fs_a` hash the operator saw "near zero" coincides with the steepest
> COMMON rate ramp (~120 ppm/min), not with the zero crossing: unsynchronised 0.33 s blocks sample
> a fast ramp at different phases, so the two P terms differ by a few ppm block-to-block; ±5 ppm ×
> 0.3 s ≈ ±1.5 µs, which is the skew wiggle seen. Inaudible. `block_n 64` set at 21:33:50 as an
> A/B on the dither (halves update rate, doubles averaging; splice horizon follows).
>
> **Ten settled minutes, build 12 / tau 10 / Ti 120, WIRE 21:27–21:37 (n=11096, no rows
> rival-gated): mean +3.7 median +3.6 MAD 3.8 sd 5.2 p2p 30.4 µs.** Standing offset draining
> (+5.7 → +3.7 over the span). Against the morning baseline (sd 46.7 / p2p 243) that is 9x on sd
> and 8x on peak-to-peak over a window long enough to include two server-side delivery pauses.
>
> **The "wild flutter" and the 19:41 −29 ms event have one cause: the ACCOUNTING SPLIT REPAIR
> still fires with tags live.** A 21:37:52 `Accounting split repaired: ran −29026 us`, 21:38:15
> `+29024 us` — the mixer-ring drift sawtooth briefly held steady, passed the steadiness band, and
> the repair stepped the ledger; the demoted prediction jumped and the hard-resync path dropped
> 1404 frames of REAL audio (31.8 ms), err_tag read the true −32 ms for 24 s, the sawtooth flipped,
> a second repair stepped back and 1512 frames were inserted. Skip, stutter, "fixed itself". The
> delay loop was immune throughout (integral +56.61 held). Both earlier attributions of the 19:41
> event (mapping step, then tag mis-identity) were wrong — it was this: same ±29 ms signature.
> Build 13: with tags live the repair window is disarmed (ledger diagnostic-only, as the plan
> always said); the repair survives only on the tags-absent fallback.
>
> **block_n 64 A/B (21:35–21:43): NO VERDICT** — the span contains the −29/+29 ms repair pair and a
> delivery stall (1100/8890 rows rival-gated, sd 928 µs); the differential-rate dither read MAD
> 1.49 vs 0.83 ppm but is dominated by those events. block_n stays 32 (the reboot into build 13
> reset it). Rerun on a quiet span before deciding.
>
> **Build 13 outcome (21:42–21:56): the disarmed repair exposed the other half of the problem.**
> A's unrepaired −29 ms split biased the PREDICTION, the common wander carried it over the 50 ms
> threshold, and the hard-resync path dropped 50 ms of real audio three times (50/53/50 ms at
> 21:47:48, :50, 21:51:13) that err_tag then spliced back — 253 out-of-range blocks, 39 splice
> episodes, wire sd 272 / p2p 9.7 ms with 58% of rows rival-gated. B (clean ledger) had zero
> events. REVIEW 3 called this: "the kept ledger rots without the deleted corrections". Repaired,
> the prediction moves audio; unrepaired, it is biased. Build 14: hard resync, storm mute and the
> aggressive catch-up decide on err_tag while tags are live (prediction only as the fallback), with
> a re-measure guard (no repeat until the tags post-date the last action by one blank interval).
>
> **Build 14, WIRE 22:13–22:24:30 (11.5 min, n=11156, ZERO rows rival-gated): mean +5.5 median +4.0
> MAD 5.3 sd 8.9 p2p 45.2 µs.** No hard resyncs, no repairs, no splice episodes on either board
> through the span — the first window since the morning with nothing in it but the loop and the
> common-mode wander. Comparable to build 12's quiet 10 min (sd 5.2 / p2p 30.4); against the
> morning baseline sd 46.7 / p2p 243. On-device over the same span: A median −3 MAD 102, B −2
> MAD 110 (common mode, r = 0.995), **differential err median +2 MAD 9 p5 −20 p95 +27 µs, and an
> event census of exactly zero on both boards** — no resyncs, splices, out-of-range holds, stale
> tags, mutes or repairs.
>
> **2026-08-29 11:20, found from "why no correlation": B sat 8–10 ms EARLY for 5+ minutes and
> nothing could close it.** Two prediction consumers I had left alone were still moving audio:
> the rate-lock fine-band GATE and the unconverged STEER fallback both read the ledger median
> (+2019 µs, biased) while err_tag read −8.6 ms — the gate stayed shut (B never "converged" after
> an unobserved reboot at 22:46), so the steer dropped −136 frames per report on the ledger while
> the tag-driven fast path inserted +120: a tug of war. And between the fast splice's 128-frame
> bound (~2.9 ms, "measurement fault" — a rule written for the prediction) and the 10 ms aggressive
> threshold, nothing corrects a REAL error. Build 15: gate, steer, unmute latch and the catch-up
> threshold all read the measured error while tags are live; the steer fallback is off while tags
> are live; catch-up threshold on tags = the splice bound. B rebooted by API to clear it meanwhile.
> B's 22:46 reboot has no log (tails were dead) — cause unknown.
>
> **Build 16 (2026-08-29 ~11:40): boot settle.** After a boot the restored integral can be ~20 ppm
> stale (both boards read +36 this morning against +55 persisted last night — common, so the
> timebase's drift estimate or temperature, not one crystal), which at Kp 0.1 parks 200 µs that
> Ti = 120 s takes ~5 min to absorb. Ti is 20 s for the first 180 s of esp_timer time (boot-scoped,
> so mapping flaps later never re-trigger it), and the EMA is saved on shutdown (hub `on_shutdown`
> → `persist_now`) so an OTA/restart restores the value at the moment of reboot rather than one up
> to 10 min old. `internal_temperature` sensor added to the base config at 30 s so the analyser's
> temperature lane can be correlated with the persisted crystal offset. Also this morning: B's
> reconnect-after-stall recovered in ~20 s under build 15 (last night the same precondition wedged
> for five minutes on the early side).
>
> **Build 16 first minutes (11:39–11:42):** both loops' `err` swing in phase (±350 µs, ~60 s
> period — the common-mode timebase wander), and with Ti 20 s the integrals follow it (A 30→67→41
> ppm, B 29→61→32; trim +14…+85 ppm) instead of averaging through it. The wire B−A drifted
> −230→+50 µs as the difference of two loops chasing the same wander from different starting
> points; the 11:42:52 persist (+50.5) landed on a crest (true ≈ +40). **Decision for the next
> batch:** keep shutdown-persist, shorten `DL_TI_BOOT_WINDOW_US` to 60 s. Flutter in the same span
> was three delivery events, none the loop: B-only ring dry 11:40:14 (276 ms), server-wide +7 ms
> step 11:40:44 (observer's own phase moved +7.1 ms), A-only ring dry 11:41:15 (873/1366 ms).
> **Hypothesis (not yet a finding):** every starvation this morning fell 1–7 min after an OTA
> reboot (11:28→B 11:32–35; 11:38→B 11:40, A 11:41) — a post-boot starvation window.
>
> **Census (all of yesterday evening + this morning, stall clusters >60 s apart vs the preceding
> "Boot seems successful"):** A 14 stalls, 8 within 300 s of a boot; B 15, 7 within 300 s. The
> boards spend well under 10% of their time inside the first 5 min after a boot, so the post-boot
> window carries roughly half the stalls at ~8× the base rate. The observer (no DAC, no OTA
> churn) stalls too, at 1000–8000 s — the server-wide class. Two classes, then: a **post-boot
> per-board receive stall** (RSSI fine, WiFi "signal good" at the moment of the bailout, TCP
> simply stops delivering for 3+ s; 11:44:44 A escalated to a 4.6 s bailout + clean 3 s
> reconnect) and the **server-wide pause**. Neither is the servo's; the servo's job is to make
> them cheap, which build 15/16 does (splice-out in 3–4 s, reconnect in 3 s).
>
> **Per-board stall class, traced one hop further (11:41–11:52):** A alone stalled at 11:41:15,
> 11:44:44 and 11:49:50 (the last two escalated to a 4 s bailout + 3 s reconnect); B clean since
> 11:40. During each stall the `snap_net` task kept logging (TSF/UDP alive) while TCP chunk
> records stopped dead — ring 81 ms → 0 in one second, no gradual decline. Server side
> (`debian-hp-z440`, snapserver in docker): no error until A's own reset; `ss -tni` shows A's
> session rtt 52 ± 23 ms with retransmits, B's 40 ms. So the packets leave the server and die on
> A's WiFi hop; RSSI is fine both ways, and the SuperMini boots at `output_power: 8.5dB` — a
> marginal *uplink* (ACKs) is exactly what turns one lost retransmit into an RTO back-off of
> 0.25→0.5→1→2 s = a 3.75 s hole. **Experiment 11:52:01:** A's "WiFi TX Power" raised to 14 dBm
> over the API (driver readback 14.00), B left at boot power as the control. Pass = A's stalls
> stop while B's rate is unchanged; then the same for B and a config change.
>
> **11:53:15 — inconclusive for TX power, informative otherwise:** A *and the observer* went dry
> in the same second (A → bailout at 11:53:22), B did not. All three boards sit on the same AP
> (`70:58:A4:1E:1E:09`, channel 1, 2.4 GHz). Yesterday's census has the same shape: many stalls
> hit A and B in the same second, several hit one of them. A loss burst on channel 1 that each
> client survives or not by its own margin fits every observation; a per-board TX-power story
> fits only some. The servo cannot fix this, but the *buffer* can: the client holds ~1.7 s ahead of
> playout (SYNCX `buffered 1671–1750 ms` = server `buffer` 2000 ms minus the ~300 ms pipeline), and an RTO back-off hole is 0.25+0.5+1+2 = 3.75 s,
> so every such hole beats the buffer by construction. Server buffer 4000–5000 ms would absorb
> one back-off cycle outright — the single cheapest lever on the table.
>
> **Not a finding (checked):** the integral sits above the TSF mapping's ppm — A +56 vs +40,
> B +52 vs +42 over 11:46–11:55. Last night 23:10–23:40 it was the same (A +58 vs +40, B +51 vs
> +40), so it is a steady per-board offset between "server-time mapping slope" and "I2S trim that
> holds tags on the deadline" — the DAC clock's own error against the crystal — not a build-16
> effect. Retracts the "wasn't there last night" remark made from a point sample of `Crystal:
> mine` (a different quantity).
>
> **Steady-state anatomy (12:0x, goal < 1 µs swing).** Wire A−B over the stall-free 11:54–12:00
> (n=6357, rival-gated): median +3.4 µs, robust sd 3.3 µs; robust sd of the *change* over a lag
> is 0.11 µs @0.1 s, 0.63 @1 s, 2.4 @10 s, 4.6 @60 s, 6.1 @120 s — grows as √τ, a random walk:
> the boards' **rate** difference is noisy and position integrates it. Within a 10-s block the
> wire is 0.78 µs; the swing is all slow. On-device the P term is the source: per update (0.33 s)
> `err` changes by 18 µs median and `trim` by 1.75 ppm = Kp·Δerr; err's own robust sd is 129 µs
> (the common wander, which cancels between boards) — the *uncorrelated* part of the 18 µs per
> update is what walks the wire. Levers, all runtime: tau 10 → 30 (P noise 3×), block_n 32 → 64
> (√2), then the exchanged-err differential term. **12:04:53: tau_s=30 on both boards**
> (SERVOPARAM on A and B); graded 12:05:30–12:16 with `scratchpad/wire-sf.py`.
>
> **The floor under the P noise: divider quantization (12:1x).** With tau 30 the wire turned into
> a sawtooth, p2p ~10 µs: the analyser's `fs_b` sits on two discrete levels 0.16 Hz (3.6 ppm)
> apart in ~3 s squares while B's requested trim glides 45→58 ppm. The achievable MCLK ratios
> 14 + b/a (a ≤ 511) form a Farey sequence spaced **0.5–1.2 ppm** across B's range (+47..+56):
> B's TRIMDBG `applied` values (+54.90, +53.19, +52.07, +51.19, +50.69, +50.16) are exactly that
> set (54.927, 53.219, 52.099, 51.215, 50.721, 50.188). The lock picked the single nearest ratio,
> so each board carried an uncorrelated error of up to half a step for the seconds between
> crossings — ~1 ppm × 3 s ≈ 3 µs per leg, the sawtooth. (A first sweep reported a 4.2 ppm gap at
> +59.9; that was a scripting artefact — the neighbours there are 0.32–0.38 ppm apart. Retracted.) **Build 17 adds sigma-delta dithering:** `set_trim_ppm`
> publishes the two bracketing ratios and a duty; `RateLock::tick()`, called from
> `notify_audio_played` at the DMA cadence (~100 Hz), switches between them with a first-order
> accumulator. Mean rate = request; residual ≤ one step × one tick ≈ 10 ns. Same single atomic
> 32-bit register store as before. `read_baseline_` recognises either bracket value as ours.
>
> **Build 17 landed 12:17:10** (both compiled 17:16:28Z). Shutdown-persist verified exact (A
> +56.80 saved/restored, B +51.79). DIETEMP flowing (A 63.6 °C, B 65.1 °C). Boot fast-Ti judged
> harmful now that the restore is exact: it integrated the settling transient behind the
> 1000→2000 ms setpoint change (err +500..+950 µs) into +13 ppm on A in 8 s → window set to 0 for
> build 18. The tau-30 window (12:05–12:16) was voided by a group-wide −19 ms deadline step at
> 12:10:36 (A −19.5, B −18.8, observer −4.7/+2.5 ms after its own deadline-source switch at
> 12:09:50) — infrastructure, parked per the steady-state focus.
>
> **Build 17 first minutes:** dither confirmed live — TRIMDBG `applied` is now continuous
> (+57.25, +48.06, +42.51 …) instead of the Farey set, zero rate-lock warnings. The reflash reset
> the session tunables, so tau was 10 again and the P term thrashed (A +37→+114 ppm, B −33→+66)
> through the post-boot fallback flaps; tau 30 re-issued 12:20:32 and made the compiled default
> for build 18 (with the boot fast-Ti window 0). Build-17 wire grade: 12:22–12:33 via wire-sf.
>
> **Where the remaining wander comes from (12:22–12:33, build 17, tau 30, dither live).** The
> wire's change does not follow the common wander (r +0.10 → not gain/phase mismatch). The
> on-device `err_a − err_b` averaged over 1/5/20/60 s has robust sd 14.8 / 10.0 / 7.1 / 11.7 µs —
> white noise would fall to ~2 by 60 s — so each board carries a **slow (tens of seconds) ~7 µs
> error of its own** against its deadline, which the loop tracks onto the wire (r(wire, err_a −
> err_b) = +0.77). It is not the server-time mapping alone: the observer's `PHASEIN` A−B render
> phase (TSF-timestamped, mapping-free) has the same 7.4 µs and does NOT track the wire (r +0.30,
> wrong sign; residual 15 µs). So the render-phase / tag measurement itself has ~7 µs of slow
> per-board error. Consequence: the loop should not follow errors on the 20–60 s scale at all —
> the true disturbance (crystal vs temperature) moves ~0.1 ppm/min, far slower. **Next test
> (runtime, no flash): tau 120, Ti 600, block_n 64** — attenuates the 20–60 s measurement wander
> ~4× and stops the 8-ppm rate swings spent chasing mapping noise. Cost: the ±350 µs common wander
> passes to absolute playout, which is common to every board and inaudible.
>
> **Build 17 graded (12:22–12:33, tau 30, dither, n=12,598):** median −3.5 µs, robust sd 4.9 µs;
> change over 1/10/30/60/120 s = 0.38 / 2.2 / 3.9 / 7.1 / 7.7 µs vs build 16 (tau 10) 0.63 / 2.4 /
> 3.6 / 4.6 / 6.1. Fast noise halved (P term + dither), slow wander unchanged or worse — as the
> slow per-board measurement error predicts: tau 30 still follows a 7 µs error that moves over
> 20–60 s.
>
> **INCIDENT 12:38–13:17 — a tag/ledger split the servo cannot escape.** (The bench Mac slept
> ~12:38–12:55 and again ~12:56–13:11, so the logs have holes; the boards did not.) When logging
> resumed A held `err_tag` = −19.4 ms and B +91..+97 ms, constant for 40 minutes. `SHADOW` shows the
> disagreement exactly: A err_tag −19435 vs err_live +1698 (diff −21133 = RECON drift 20000);
> B err_tag +90997 vs err_live −21472 (diff +112469 = RECON drift −112449). `RENDERTAG measured`
> vs `inferred` differ by the same 20 / 110 ms. So the tag identity path and the frame ledger
> disagree by an accounting split — and since build 13 the split repair is disarmed while tags
> are live, while since build 14 every coarse correction acts on err_tag. Result: B hard-resynced
> every ~20 s ("KP re-armed (hard resync (late))" ×N) and err_tag never moved; A's fast splice hit
> its bound with −19476 still standing and gave up ("measurement fault") — over and over. The
> analyser saw no shared audio (>17 ms apart). **Corrections that do not move the measurement
> they act on must invalidate that measurement**, not repeat. Fix to design: when a coarse
> correction on err_tag is followed (after the re-measure guard) by |err_tag| unchanged within a
> fraction of the correction, declare the tag path faulted — drop tag identity (re-anchor tags),
> fall back to the ledger error for coarse decisions, re-arm the split repair. Both boards
> restarted over the API at 13:17 to clear it.
>
> **Implemented for build 18 (tag fault):** the first err_tag block that post-dates a tag-driven
> coarse correction judges it — |err_tag| still ≥ 75 % of the value acted on is a miss; three
> consecutive misses set `tag_fault_until_us` = now + 60 s and log `TAGFAULT`. While faulted, the
> coarse selection, the measured-error splice gate and the split-repair disarm all treat tags as
> stale, so the ledger drives coarse decisions and the accounting repair can run. The rate loop
> itself keeps its out-of-range hold (which covered both observed cases) — a sub-millisecond tag
> fault would still bias the integral slowly; open item. Mechanism of the split itself still
> unknown: `push_chunk_` tags each slice with `rec.server_ts_us` + offset assuming the ring bytes it
> reads are that record's, and `discard_ring_bytes_` keeps ring and record queue aligned on the
> resync paths; the observed biases (20.000 ms on A, 112.45 ms on B, equal to RECON drift) grew with
> B's tag-driven resyncs, so a ring/record desync on one of those paths is the suspect.
>
> **Trigger caught live (A, 13:21, six minutes after a clean restart):** `no chunk records for 3 s,
> ring holds 0` → hard resync 1464 ms late → **98 hard resyncs over 128 chunks** (a chunk-drop storm
> on an EMPTY ring) → `RECON drift` jumps from −52245 to **+44988** and from then on `SHADOW diff` is a
> constant −45 ms (err_tag −43…−47 ms, err_live within ±2.6 ms). Mechanism (to verify in code): the
> late-resync path drops the record AND calls `discard_ring_bytes_(rec.bytes)`; with the ring empty
> that read blocks in 100 ms ticks until bytes arrive — the decoder's bytes for LATER records — and
> eats them. Ring and record queue are then misaligned by the total mis-discarded bytes, every
> subsequent slice is tagged with the wrong `rec.server_ts_us`, and err_tag carries a constant bias
> equal to the misalignment — which is exactly what the ledger's "drift" measures. Same signature on
> B at 12:34 (stall → resync → +39 ms). The tag-fault fallback (build 18) breaks the deadlock; the
> root fix is to never discard ring bytes that are not this record's — build 19.
>
> **Build 18 landed 13:25:09** (tag fault, boot fast-Ti off). Runtime tau 120 / Ti 600 / block 64
> applied 13:27:05; graded 13:33–13:45. Post-flash the wire closed +330 µs as a pure first-order
> approach (tau 120 → 4–6 min, no overshoot — correct for ζ ≈ 1, too slow to live with).
> **Build 19: error-proportional gain** (operator's suggestion over a two-state schedule): Kp =
> (1/tau)·max(1, |err|/75 µs), Ti divided by the same factor, effective tau floored at 20 s. Inside
> the knee the slow tunables run exactly; at 450 µs the loop is already 6× stiffer. Continuous in
> the error — no state, no hysteresis, no gain step. Replaces the event-driven schedule the flat
> gain removed in build 7 without its flaw: it keys on the error, not on a timer.
>
> **Cycle time is now the metric (operator, 13:31).** `scripts/bench/converge-time.py --a-off --b-off
> --wire-from` measures, from the reboot line: first DLLOOP, engage, first time |err| ≤ 75 µs holds
> 20 s, and on the wire first time |A−B| ≤ 20 µs holds 20 s. **Baseline build 18 (tau 120 flat):**
> engaged +14 s (A) / +22 s (B); neither board nor the wire converged within 450–490 s. Build 19
> (error-proportional gain) flashed 13:3x against that.
>
> **Build 19 first boot (13:32:41):** the error-proportional gain engaged as designed — B at err
> +995 µs ran kp 0.050 (tau 20), back to 0.010 inside the knee within ~2 min; B |err| ≤ 90 µs by
> 13:35. The wire closed −390 → 0 at +17 ppm in ~35 s… and parked at **+311 µs for a minute**
> (13:34:00–13:34:40) with B's own error at −90 µs. So the plateau is not the loop: it is the two
> boards disagreeing about WHERE zero is. Consensus adopts the mean of the live estimate set
> (`adopted +0 us from target` always — by design, so mappings are identical when the sets are);
> spreads were 200–700 µs, so two boards holding different sets (a beacon lost by one, up to
> PEER_MAP_STALE_US) differ by O(spread/n) ≈ 100–250 µs — the right magnitude. A's log tail died
> with the OTA (replug 13:35:45 restored it; build 19 confirmed still running), so the
> decomposition wire = err_A − err_B + Δdeadline could not be done on this boot; it is being
> done on A's second boot at 13:44.
>
> **Build 19 second look (A's replug boot):** the on-device `err a−b` tracked the wire, so this
> boot's slow tail was the loop's own: inside the 75 µs knee the gain is back at tau 120 and the
> −150 µs undershoot (integral wound while boosted) unwinds at that rate. **Build 20:** knee and tau
> floor become runtime tunables (`knee_us`, default 25; `tau_min_s`, default 20; `tau_s` bound
> raised to 600) so the acquisition shape is iterated over the API. Knee 25 → tau ≈ 30 s at 100
> µs, tau 120 only inside the per-block noise.
 Also in build 20: **Ti is no longer boosted.** Ki = kp/Ti
> already rises with kp; dividing Ti too made Ki ∝ boost² and wound the (correct, restored)
> integral during the position catch-up — the −150 µs undershoot and the long tail. A boot error
> is position, not rate. Build-19 cycle time to beat: wire inside 20 µs held 20 s at **+242 s**
> from A's 13:35:45 boot.
>
> **Build 20 landed 13:45:26** (both 18:44:45Z, tails alive). 13:44 decomposition of build 19's
> replug boot: err_A ≈ err_B (−96/−98 … −169/−168) with the wire at −3.5 µs → deadlines agree; the
> earlier +311 µs shelf was pre-consensus disagreement during A's log gap. The ~10 µs stair-steps
> were the integrals (A 57.3 → 59.7 → 57.3 ppm inside a minute) chasing the wander at Ki ∝ boost²
> — removed in build 20. Cycle time graded at 13:53 from the 13:45:26 flash.
>
> **Build 20 (13:45) diverged from zero after crossing it — bumpless transfer.** B's integral went
> 54.2 → 61.5 ppm in four blocks (13:45:49–53) as err crossed +232 → −189 µs: not Ki (0.005
> ppm/block) but the bumpless transfer `I += (kp_old − kp_new)·e`, written for API tau changes and
> now firing every block because kp is error-proportional — a hidden integrator of Δkp·e. The
> wire then ran away from zero as the loop chased its own integral. **Build 21:** the transfer is
> keyed on the tuned 1/tau only; the proportional boost steps the output on purpose.
>
> **Cycle-time ledger (wire |A−B| ≤ 20 µs held 20 s, from the reboot line):** build 18 (tau 120
> flat) > 450 s; build 19 (boost, Ti boosted) 242 s; build 20 (knee 25, Ti unboosted) never inside
> its 248 s of life — diverged after crossing zero (bumpless transfer); build 21 (transfer keyed on
> tuned kp) flashed 13:49:34, back at +16 µs by +100 s, integrals flat — graded at +8 min.
>
> **Build 21 graded (from the 13:49:34 reboot):** A inside 75 µs from +150 s, B from +261 s,
> integrals flat (A 56.2–56.6, B 54.5→53.9) — the bumpless fix holds. But the wire never held
> inside 20 µs in 466 s: median **+61 µs**, robust sd 21, 1-s change 1.4 µs (build 17: 0.38). Two
> lessons. (1) A magnitude knee cannot separate "far from setpoint" from "the common wander": err
> carries the ±350 µs common wander, so with knee 25 the loops sit at tau 30–60 in steady state and
> the P noise is back. Knee above the wander (~200+) restores the slow steady state but gives back
> the slow tail below it. Magnitude alone cannot have both. (2) The +61 µs offset with both loops
> at their setpoints is the two boards' deadlines disagreeing — the estimate-set path-dependence.
> **Both point at the same structure:** a fast channel on the DIFFERENTIAL (my render phase −
> group render phase, already exchanged over TSF and already tag-derived) and the slow channel on
> err_tag. That is `render_align`, parked in the code as "blocked on ledger-independence" — which
> the tag path now provides.
>
> **Can the exchanged render phase drive a differential channel today? No.** 13:52–13:58, the
> on-device TSF render-group delta (SYNCX `render N us`, tag-derived, published per ~3.3 s report,
> paired within 300 ms): A robust sd 39 µs, r with the wire −0.23; B carries 7 ms outliers. The
> wire itself wandered ±20 µs. The pairing is the problem, as the 30 Hz note predicted: two boards
> sample their phase at different instants while the common wander moves at ~40 µs/s, so a 300 ms
> pairing window alone is ±12 µs, and the 3.3 s cadence makes pairs rare. To use it: publish a
> LINE (phase + slope, extrapolable like the timebase estimates) or raise the exchange rate — i.e.
> the beacon-rate experiment, now with a consumer. Knee moved to 150 µs over the API at 13:58:41
> (above the common wander) for the steady-state grade 14:01–14:11.
>
> **Build 22 (fleet flash, all three boards):** the observer publishes no render phase (its +9.5 ms
> value was half of the speakers' group mean); `render_align` cap/gain/deadband become
> `servo_param align_max_us / align_gain / align_deadband_us` so the inter-device channel can be
> switched on and tuned over the API. With the observer excluded, the exchanged render delta
> correlates with the wire (A +0.75, B −0.60, ~10 µs per 3.3 s sample) — enough for a slow
> channel on the standing offset (+47 µs at 13:53–13:58) and the slow drift.
>
> **Build 22 landed 14:05:28 (speakers 19:01:51Z; observer reflashed).** Render deltas on B after
> the fix: −22…0 µs, no more +9.5 ms half. Cycle time from the reboot: wire inside 20 µs held from
> **+209 s** (build 19: 242; build 18: >450), boards inside 75 µs at +241/+242 s (knee back at the
> compiled 25 after the reflash). 14:13:58: `knee_us 150` and `align_max_us 2000` (gain 0.05,
> deadband 20) on both speakers — the inter-device channel is live for the first time with a clean
> group phase; graded 14:16–14:27.
>
> **14:15:34–14:21:04 the bench Mac slept again** (all three log tails gap; analyser process
> survived, CSV current). The align channel's first window was voided and re-armed 14:23–14:35
> (`align_deadband_us` 3 since 14:15:10). First RALIGN steps seen on A: group −62 → bias −19, −17,
> −15, −12 (+3 µs per due report = gain 0.05 × 60). B stalled 14:11:48 (three late resyncs) →
> clean bailout/reconnect 14:11:53, SHADOW diff −32 µs afterwards — no tag/ledger split, so the
> 13:21-class desync is not a certainty after every storm. Both loops healthy post-sleep.
>
> **render_align's first live run (14:21–14:24) ran the wrong way.** A: delta −60 held while bias
> stepped −19 → −10 and the wire fell −69 → −94 µs at 0.3–0.4 ppm — exactly gain × delta per due
> report. The wire had A LATE; the code read negative delta as EARLY. B mirrored it. Then one bad
> pair (+1717 µs) moved A's bias 41 µs in one step and the wire jumped +110 µs. Disabled 14:24:49.
> **Build 23:** step = +delta × gain (sign measured, not assumed), pairs beyond `align_reject_us`
> (500) ignored, step capped at `align_step_us` (5 µs per ~10 s report = 0.5 ppm max).
>
> **Build 23 (14:26:47): the tag fault fired and was not enough.** 14:29:21 A starved → hard resync
> 330 ms → SHADOW diff −15,039 µs → `TAGFAULT … err_tag −12592 (ledger −807)` at 14:29:34 — the
> detector works. But distrust only stops the thrash: A then sat at err_tag −15.9 ms (holding),
> ledger −0.6 ms, wire 3.4 ms off, indefinitely — three measurements, none agreeing, nothing able
> to rebuild the tag path. **Build 24:** TAGFAULT also requests a reconnect (the late-stream
> bailout's flag): the session teardown rebuilds the pipeline and its tag tracks, and every
> observed bailout has come back with tags and ledger agreeing within tens of µs. ~3 s gap vs an
> open-ended desync. The root (why a chunk-drop storm on an empty ring desyncs tags from audio)
> is still open; the reconnect bounds its cost.
>
> **What actually repaired A (14:33:41):** not a reconnect — the accounting-split repair, re-armed
> by TAGFAULT: "accounted queue ran +14987 us against measured latency for 3 s", ledger corrected,
> coarse path moved the audio, and one report later SHADOW diff was −55 µs with err_tag +2.5 ms
> → fast splice → done. So the LEDGER was the side that had slipped by 15 ms (RECON drift 14988
> the whole time) and the tags were right; the "misses" happened because the tag-driven catch-up's
> frame drops were being undone by the split (the ledger, not the audio, absorbed them). It took
> four minutes only because each 60-s fault window expired and three misses had to re-accumulate
> before the repair got its 3-s window (TAGFAULT ×3: 14:29:34, 14:30:37, 14:31:41, 14:32:44).
> **Build 25 plan:** on TAGFAULT run the repair at once (pre-arm `drift_excess_since_us`) and hold
> the fault 180 s. Build 24 (reconnect on TAGFAULT) stays as the backstop if the repair does not
> close the split. Bench watchdog (`scripts/bench/watch-bench.py`) now runs as a persistent
> Monitor: TAGFAULT / stalls / bailouts / |err| > 5 ms / split > 5 ms / wire > 200 µs / analyser
> no-correlation / log gaps, one line per event with a 5-min cooldown.
>
> **Build 24 boot (14:35:24): wire −5 µs by +76 s, ±1 µs by +116 s** — cycle-time ledger now
> 18: >450 s · 19: 242 · 22: 209 · 24: ~90–115 s. The boot's own 15 s (first DLLOOP +12, engage
> +14…19) is now a large fraction; the remainder is the boosted P closing a few hundred µs.
>
> **Build 25 (queued behind the build-24 align grade):** (1) TAGFAULT requires tag/ledger
> DISAGREEMENT (> 3 ms) — B's 14:37:54 fault fired with err_tag +47827 / ledger +47490, a healthy
> tag path 47 ms late after a starvation, and build 24 reconnected for nothing. (2) On a genuine
> fault: pre-arm the split repair (the thing that actually fixed A at 14:33:41), fault window
> 180 s, reconnect only on a second fault without a repair. (3) Cold start: no NVS integral →
> seed from the TSF own-crystal estimate at first engage (~14 ppm from the DAC's trim; a fresh
> board otherwise winds 56 ppm through Ki for 10+ min) and run the fast boot Ti for 180 s — only
> when cold; a restored board never sees it.
>
> **Build 24 graded:** wire inside 20 µs held from **+74 s** after the reboot (ledger: 18 >450 ·
> 19 242 · 22 209 · 24 74). The align run 14:39–14:49 is void as a steady-state number — it holds
> A's 14:45 and B's 14:47 starvations, one false TAGFAULT reconnect and one late-stream bailout —
> but its 1-s change of 0.30 µs (knee 150 → tau 120 inside the wander) is the quietest fast noise
> yet; the slow terms (median −39, robust sd 27) are the events. Three starvations in the 12 min
> after the 14:35 boot (B 14:37, A 14:45, B 14:47): the post-boot cluster again.
>
> **15:07–15:12: a dead session nobody noticed.** The Mac slept 14:50–15:11; when it woke, both
> boards showed "PLAYER STALLED: no chunk completed for 245 s", rings empty, while MLS44 was
> `playing` on the server and the server listed both speakers *disconnected* ("Removing inactive
> sessions" 15:07:08). `recv_exact_` waits on a silent socket forever — it keeps sending Time
> requests into it and nothing ever returns false — and the late-stream bailout cannot fire
> without chunks to be late. A client that has lost its server without a FIN starves until reboot.
> **Build 26:** `last_rx_us_` tracks the last byte received; 15 s of total silence on a connected
> session (Time replies come every 1 s streaming, every few s idle) → WARN + reconnect. The
> watchdog's STALLED/NOCORR lines were what surfaced it.
>
> **Build 25 boot (15:12:43): wire inside 20 µs held from +67 s** (ledger 18 >450 · 19 242 · 22
> 209 · 24 74 · 25 67). Align (sign flipped, step 5, deadband 3) on since 15:15:47: A's bias walks
> −32 → −39 against a group delta of −43…−61 while the wire sits at −44 µs, robust sd 8.6, not
> closing over 5 min — the bias is moving and the wire is not following. Suspects: both boards
> receiving same-sign deltas (then both advance and the difference stands still), or A's deadline
> off the shared-TSF path where the bias is applied. B's serial is wedged post-OTA, so its side is
> read from the observer's PHASEIN.
>
> **Build 26 boot (15:22:53): wire inside 20 µs held from +46 s** (ledger 18 >450 · 19 242 · 22
> 209 · 24 74 · 25 67 · 26 46). The Mac slept again 15:26:42–15:43:00; during it the server logged
> A disconnect/reconnect 15:28:09→12 and B 15:31:21→24 — client-initiated, 3 s apart, i.e. the
> dead-session detector or the bailout doing its job while nobody watched (the a.log gap hides
> which). Runtime params (knee 150, align on) survived, no reboot. Align graded 15:43:30–15:54.
> B's serial is still wedged post-OTA (logger restarted, no output) — needs a replug.
>
> **Align run 15:25:56–15:56 (sign flipped): still wrong, and self-reinforcing.** A's bias walked
> −159 → −164 → −339 µs while its group delta grew −124 → −305; two outlier pairs (−22.9 ms,
> −35.9 ms) were correctly rejected. Wire at −605 µs by 15:57 — but the Mac slept 15:43:52–15:56:30
> and the server logged both speakers dropping and reconnecting inside the window, so the wire
> cannot arbitrate the sign from this run. Align disabled, both speakers restarted 15:57 to clear
> the standing biases (nothing else could — fixed in build 27: `align_max_us 0` zeroes the bias).
> **Build 27 demotes align to SHADOW** (`align_apply`, default 0): the step is computed and
> logged, nothing moves, and the sign is settled by regressing the logged would-be step against
> the wire before anything is applied. CLAUDE.md said shadow first; this is the cost of not.
>
> **Build 27 (fleet, 16:02:53) and the first clean 30-minute window (16:05:54–16:35:55, shadow
> align, knee 150):** n=23,563, median −33 µs, robust sd 16.8, change 0.32 / 2.4 / 6.7 / 9.7 /
> 11.3 µs over 1/10/30/60/120 s; **zero TAGFAULT, zero server-silence events, no reconnects**;
> the observer's 16:15 starvation was absorbed silently. Cycle time: wire inside 20 µs from +4 s
> (both boards rebooted together into the same state — not a fair number), A inside 75 µs at
> +142 s. **Align sign, settled by shadow:** A's group delta (median −36) vs wire B−A (median −35),
> r = +0.72 over 248 pairs — negative delta = A late = wire negative, so the build-23 convention
> (step = +delta·gain, advance when late) is RIGHT; the 15:56 runaway had two session drops
> inside it and cannot be read. Next: apply with a tiny cap (align_max_us 60, step 2) and watch
> the wire for ten minutes — bounded harm, decisive result.
>
> **16:38:16 server-wide starvation (A + observer) → genuine TAGFAULTs (A −3.5 ms vs +1.8; observer
> −33.7 vs +1.5) → the build-27 repair-first path did NOT repair:** RECON drift matched the split
> (A +4988, observer +34989 — the ledger had slipped again) and the residual gate did not refuse,
> but the repair's drift median is over DRIFT_WINDOW samples at the 20-s RECON cadence — minutes
> — so the second-fault backstop fired first at 16:41:30/36 (reconnect). Three minutes of desync,
> the applied-align test (cap 60) voided. **Build 28:** a genuine TAGFAULT reconnects at once; the
> repair pre-arm stays as a bonus. The disagreement rule makes the false-positive reconnects of
> build 24 impossible, so the immediate reconnect is now safe.
>
> **Build 28 fleet flash (16:43): B came up 58–61 ms off A (observer PHASEIN) and stayed there
> 3+ minutes with no reconnect** — no TAGFAULT (which needs tag/ledger DISagreement: a plain 60 ms
> lateness with both agreeing is not a fault) and apparently no hard resync either, although 60 ms
> exceeds the 50 ms threshold; B's serial has been wedged since its 15:12 OTA so the mechanism is
> invisible. Restarted B over the API 16:48:5x. Lesson for the cycle-time work: **a board without
> serial is a board that cannot be iterated on** — B's replug is the precondition for the next
> round. Open question to answer once B logs again: what holds a board 60 ms late with tags and
> ledger agreeing (the coarse path should have taken it: hard resync > 50 ms, catch-up below).
 After the 16:48:51 restart B re-locked in ~25 s (wire −56 µs at
> 16:49:15, observer B−A phase +21…+88 µs) — the 58 ms state does not recur on a clean boot, which
> points at the fleet-flash boot (three boards rebooting into each other's consensus churn) rather
> than at B's hardware. B's runtime params re-applied 16:51 (the restart had wiped them).
>
> **Applied align, both boards, cap 60 (16:48–17:03):** A's group delta +57…+77, bias railed at
> +60 within minutes; the wire went from −33 µs (16:50) to +140…+173 and STAYED — the applied bias
> moved the wire ~+170 µs and did not reduce the delta (r(delta, wire) = +0.96: the delta faithfully
> reports the wire, the correction just goes the wrong way in effect). Both boards were applying
> (B blind, params re-applied 16:51:50), so two-board runs cannot separate sign from symmetry.
> **Single-board step test (17:03–17:10):** B's align off (bias → 0), then A's +60 µs bias removed,
> wire medians before/after each step — the sign of "bias → wire" measured directly. Cycle time
> build 28: wire ≤ 20 µs from +311 s (B's 58 ms post-flash state and restart inside the window;
> not a fair number).
>
> **Align sign, settled from the definitions (not from a contaminated run):** `phase = render_tsf −
> render_server` (later render = larger phase); `delta = mine − robust_mean(peers)` → positive =
> LATE → deadline must move earlier → `bias −= delta·gain` — the ORIGINAL code. The build-23 flip
> was wrong; the 14:21 run it was read from still had a polluted group phase. Both applied runs
> since the flip (15:25–15:56 runaway; 16:48–17:03 wire +140…+173 with A railed at +60) are what an
> inverted correction looks like, and r(delta, wire) = +0.96 in the second says the *measurement*
> was right throughout. **Build 29 reverts the sign.** Shadow-only remains the default; the
> single-board step test (17:03–17:10) is the empirical check of "bias → wire" direction.
>
> **Single-board step test (17:03–17:10) — the empirical sign:** B align off (bias → 0): wire
> +143 → +105. A's +60 µs bias removed: wire +105 → +33 → +8. So **positive bias = this board
> plays EARLIER on the wire**, and with A at +60 the wire read +140 (A early) while A's delta was
> +58 → **positive delta = EARLY**. Early must play later → bias −= delta·gain — the original code,
> as build 29 has it. My derivation from the phase formula had two sign errors that cancelled;
> the code comment now records the measurement, not the derivation.
>
> **Build 29 (17:12:40), align applied with the measured sign, cap 60 (17:16:25–17:24):** A's bias
> +50 → +57 while its group delta shrank −139 → −23 and the wire closed −106 → −8 µs — the
> inter-device channel removing a standing offset, the first time it has done so. Ended by the
> 17:24:50 server-wide starvation (+256 µs) and A's clean bailout at 17:26:39. Bias pinned at the
> cap → cap raised to 150 (17:29); 30-minute hands-off window follows. Cycle time build 29: A inside
> 75 µs from +78 s (the wire's ≤20 µs criterion was not met before the events — the ~−100 µs standing
> offset is precisely what align was still removing).
>
> **30-min hands-off, align cap 150 / gain 0.05 / step 2 (17:29–17:59):** wire median −17 µs but
> wandering +55 → −6 (17:36 event) → +107 → the 17:45:15 server-wide starvation → −82 → −14 µs at
> 17:58 with a 2-min robust sd of **0.6 µs** — the quietest block of the day. A's bias climbed +14
> → +73 while its group delta shrank −50 → −19: the channel removes the standing offset, at a rate
> (~2 µs per 10-s report) slower than the events re-create it (50–100 µs every ~10 min). One
> starvation, zero bailouts, zero TAGFAULTs on A. 18:00: gain 0.1 / step 4 (half the time
> constant; pairing noise ~10 µs × 0.1 = 1 µs/step is still far under the wire's slow term),
> 45-minute window next.
>
> **45-min hands-off, align cap 150 / gain 0.1 / step 4 (18:00:07–18:45:07) — best window of the
> day:** n=27,126, **median +2.7 µs, robust sd 5.0 µs, p2p 27.8 µs**; change 0.19 / 1.2 / 2.4 / 3.6
> / 4.6 µs over 1/10/30/60/120 s; every 3-min median within ±8 µs at 0.4–3 µs spread; **zero
> starvations, bailouts, TAGFAULTs, silences on A.** A's bias rail-limited at +150 with the delta
> still −6…−20 → cap 300 at 18:46. Build 30 (not flashed — the bench is running well and a flash
> costs a cycle): compiled defaults knee 150, align applied, cap 300, gain 0.1, step 4, deadband 3;
> a YAML `render_align_max: 0ms` no longer forces the channel off.
>
> **19:11:49 the observer CRASHED and rebooted** (`rst:0xc RTC_SW_CPU_RST`): `assert failed:
> prvSendItemDoneNoSplit ringbuf.c:374 ((pxCurHeader->uxItemFlags & 8) == 0)` — a FreeRTOS ring
> buffer item completed/returned twice, in the I2S speaker DMA path (the observer runs the full
> speaker pipeline with no DAC attached). Just before it: `PADDISP pad=1281071 clamp=13559` — 1.28 M
> frames of padding debt on the observer. Not seen on A today (B blind). Backtrace
> Symbolised from the ELF: `xRingbufferSendComplete` ← `esphome::logger::TaskLogBuffer::
> send_message_thread_safe` ← `Logger::log_vprintf_non_main_thread_` ← `Logger::log_vprintf_`.
> **ESPHome's thread-safe logger buffer, not the servo**: the non-main-task log path double-
> completed a ring item, under today's per-chunk DEBUG volume (RAW every 10 ms from the player
> task, DEPTH/I2SDBG from the speaker/mixer tasks). Same path exists on the speakers. Mitigation
> when the diagnostics are no longer needed: drop RAW/DEPTH/I2SDBG to VERBOSE. First crash of the
> day on any board.
>
> **45-min hands-off, align cap 300 / gain 0.1 / step 4 (18:46–19:31):** n=25,155, **median +4.2 µs,
> robust sd 3.97 µs, p2p 46.9 µs**, change 0.19 / 1.2 / 2.2 / 3.7 / 4.6 µs over 1/10/30/60/120 s,
> 3-min medians 0…+11 µs at 0.6–2 µs spread, zero events. A's bias +195 → +238 (≈1 µs/min) against a
> delta averaging ≈ −5: the channel nulls the EXCHANGED phase, which sits ≈ +4 µs from the wire —
> that offset is the exchanged measurement's own bias and is this channel's floor; the slow bias
> creep is it integrating that bias. Cap → 500 (19:32) so it is not pinned; 60-min window next.
> Steady-state ledger (wire robust sd): build 14 8.9 → 17 4.9 → 21 21 (knee 25, wrong regime) →
> 27 16.8 (shadow) → 29 applied 5.0 → 4.0.
>
> **60-min hands-off, align cap 500 (19:32–20:32):** n=31,429, median +2.3 µs, robust sd 6.95 µs
> — the first 15 min carry the observer's 19:33 crash-rejoin jolt (+89 µs at 19:35, back inside ±10
> by 19:45: a consensus membership change lands on the speakers as a differential transient);
> from 19:50 every 5-min median is within −3.5…+9.9 µs at 1.3–6 µs spread. **Zero A events for
> the hour.** Defect: A's bias creeps +316 → +383 (~1.1 µs/min) nulling a −7…−14 µs exchanged
> delta while the wire sits at +2 — the channel integrates the exchanged phase's own ~10 µs bias
> and would pin at the cap in ~2 h. Deadband 3 → **15 µs** (20:33, and the compiled default): the
> channel now only acts on offsets larger than the measurement's bias. Open: an observer's flaky
> link should not be a consensus member at all — its every reconnect jolts the speakers.
>
> **20:34:51 A CRASHED — identical stack to the observer's 19:11 crash** (`xRingbufferSendComplete`
> ← `TaskLogBuffer::send_message_thread_safe` ← `Logger::log_vprintf_`, ringbuf.c:374 assert). Two
> boards in 80 min: the ESPHome thread-safe logger buffer under our volume — `RAW` alone is 382
> lines per 10 s from the player task (38/s), and nothing parses it (the analyser reads RPRE, a
> separate rare line). **Build 30 flashes now:** RAW → VERBOSE (compiled out at DEBUG), plus the
> measured defaults (knee 150, align applied, cap 500, gain 0.1, step 4, deadband 15). A speaker
> crash is a 20-s dropout and a membership jolt on the other; this outranks the graded window.
>
> **Build 30 landed 20:38:36 (fleet; speakers 01:36:28Z).** RAW gone (0 per 10 s). With the compiled
> defaults and no API tuning: wire inside 20 µs held from **+73 s**, align active from boot (bias
> +8 → +11 against a −16…−26 delta). Cycle-time ledger: 18 >450 · 19 242 · 22 209 · 24 74 · 25 67 ·
> 26 46 · 30 73 (defaults only). 60-min hands-off grade of the defaults follows (20:47–21:47).
>
> **21:04:53 server-wide starvation → A genuine TAGFAULT (−12.8 ms vs ledger −0.8) → build 30
> reconnected in 3 s → SHADOW diff −98 µs by 21:05:15, −5…−29 thereafter → wire +4.4 µs at 21:06.**
> The fault path end to end: ~12 s of disruption for the class that cost 40 minutes at 12:38 and
> 3 minutes at 16:38.
>
> **Build 30 defaults, 60-min hands-off (20:47–21:47), events included (one starvation → TAGFAULT
> → 3-s reconnect at 21:05):** n=30,499, **median −2.1 µs, robust sd 7.6 µs**, change 0.24 / 1.6 /
> 3.5 / 5.3 / 8.1 µs over 1/10/30/60/120 s, 5-min medians −16…+15 µs. Bias creep gone with deadband
> 15 (+102 → +110 in an hour vs +67 with deadband 3). The deadband's cost: up to ~15 µs of standing
> offset left alone (21:45: −12.6 with the delta at −5…−14) — the exchanged phase's own bias, which
> only a better exchanged signal (a published line, or a faster exchange) removes. State of the
> bench at 21:47: all three boards on build 30, no API tuning, watchdog armed, B's serial wedged.
>
> **Six-hour census 16:00–22:07 (byte-anchored; a first pass over a dateless 400 MB tail counted
> yesterday's evening too — the trap CLAUDE.md warns about, caught by "22:46" appearing at 22:07).**
> A: 5 boots (16:01, 16:43, 17:12 flashes; 20:34 crash; 20:37 flash), 1 crash (20:34, ESPHome logger),
> 6 real starvations (16:38, 16:41, 17:24, 17:26, 17:45, 21:04), 1 bailout (17:26), 4 TAGFAULTs (three
> in 16:38–16:41 under build 27's slow path, one at 21:05 handled in 12 s), 0 dead-session events,
> 0 repairs. Observer: 4 boots (one crash 19:11, logger), 9 real starvations, 3 bailouts. B: no log
> since its 15:12 serial wedge. Wire per 30 min: −42 (16:00, align runaway) · −2241 (16:30, B stuck
> after the fleet flash + false faults) · +8/+78 (17:00–17:59, sign flip runs) · then **+3.8, +3.0,
> +4.0, +3.5, +3.5, +0.9, −2.0, −4.3, +2.1 µs medians from 18:00 to 22:07 at robust sd 3–7 µs**,
> 25 µs in the half-hour holding the observer's crash-rejoin. Three no-correlation minutes after
> 18:00 (19:33 observer rejoin; 20:35–20:37 A crash + build-30 flash; 21:04–21:05 starvation).
>
> **22:13 B replugged (operator) — serial back after seven hours; build 30 confirmed on B.** 22:12:08
> server-wide starvation just before it: observer TAGFAULT → reconnect (build 30 path), A no fault.
>
> **New target (operator, 22:14): |A−B| < 100 µs within 5 s of a disturbance.** Baseline at 1-s
> resolution: A's 21:05 reconnect reached it at **+25 s** (from +204 µs at PI pace — below the 1 ms
> splice threshold nothing but the PI acts); B's 22:13 boot from +490 µs not within 75 s. **Build 31:**
> a resync window (`resync_win_s` 30) opened at engage, at every `mark_kp_event_` and at reconnect,
> inside which the fast splice arms at `resync_splice_us` (100) with no 4-s persistence wait. One
> frame per chunk is 0.85 ms/s, so 500 µs closes in ~0.6 s + pipeline 0.3 s + one block's lag.
> Steady state keeps 1 ms / 4 s so the common wander is never spliced.
>
> **Build 31 measured.** Boot (22:17:14): wire < 100 µs held from +27 s (14–17 s of it boot→engage).
> Injected 300 ms starvation on A (22:21:26, `scripts/bench/resync-test.py`, first run had the
> unawaited `execute_service` bug again — fixed): hard resync at once → 4.8 ms left → coarse
> catch-up to ~450 µs by +9 s (one bounded action per 500 ms blank ≈ 1 ms/s) → splice/PI → **< 100
> µs held from +12 s**. The splice bang-banged at the threshold (engage −121, one frame, release at
> −99: arm 100 / release 300). **Build 32:** release band = arm/2; in the window the coarse blank is
> `resync_blank_ms` (200) and the step 4× (frames/8 → frames/2 divisor).
>
> **Builds 32/33 (22:23, 22:31): two regressions caught within minutes each.** 32: the 200 ms
> in-window cadence let three misses accumulate inside one measurement lag → both boards TAGFAULTed
> and reconnected 20 s after boot on the normal post-boot settling (+3.1 vs −0.9 ms); 33 judges a
> correction only after max(blank, 1 s) and never in the first 20 s after engage. 33 boot: the
> continuous fast splice and the block error fought — 28 frames one way, 16 back, 3, 8, 9, 4 within
> two seconds — because the block error is held ~0.65 s while the splice moves the audio under the
> average; the wire drifted −0.4 ppm with both boards ~−130 µs (under the knee → tau 120).
> **Build 34: in the resync window the fast splice is OFF and the coarse path does step-and-verify —
> one bounded (≤ half a chunk) correction of the measured error per `resync_blank_ms` (900 ≈
> pipeline + block), arming at 100 µs — and the PI runs at the floor tau regardless of the knee.**
>
> **Build 34 measured (22:36:50):** boot < 100 µs from +48 s, zero post-boot faults. Injected 300 ms
> on B: **+11 s**. On A: +31 s — step-and-verify RANG (−4717 → +4125 → −1251 → +740 µs: full-error
> steps against a block-lagged measurement), then parked at −114 µs exactly as the 30-s window
> closed. 1000 ms on A: +21 s (18 s of it the analyser out of range, >17 ms). A false TAGFAULT at
> 22:42:39 on the second A injection: a 1-s judge lag still saw pre-correction samples in the block
> average → build 35 judge lag 2 s. **Build 36:** damped steps (`resync_gain` 0.6), window 60 s.
> Resync ledger (300 ms injection → |A−B| < 100 µs held 5 s): 31: 12 s · 34: 11 s (B) / 31 s (A).
>
> **Build 36 measured (22:45:32):** boot < 100 µs from **+24 s**; 300 ms injections A/B/A/B: **8, 9, 13,
> 17 s**; zero false faults. Traces: an initial over-correction (−3289 → +3357 → −828: the first
> post-resync block still averages pre-resync samples) and a 100–250 µs plateau walked down at 60 %
> per ~1.3 s. **Build 37:** in the window the ledger error may take the first step at t+0 (it knows
> the dropped chunks exactly; tags verify), tag steps wait a full block + pipeline (blank 1200), gain
> 0.8. Resync ledger: 31: 12 · 34: 11/31 · 36: 8/9/13/17.
>
> **22:56:57 B's measured error stepped −66 → −545 µs in 10 s with no coarse action on either board**
> (A flat within ±90): consensus spread 1.0–1.9 ms at the time (the observer's estimate in the set),
> RECON −52 ms (ledger-only sawtooth), align −4 µs. A per-board timebase step, 13 s after the boot
> window closed → recovered at steady-state gains, 450 → 224 µs in 35 s. **Build 38:** a block error
> past `resync_reopen_us` (400) re-opens the resync window whatever the cause.
>
> **22:58:48 A 300 ms injection (build 37): wire +1335 → 111 → 263 → 92 → 41 → −1 at +16 s, then kept
> going −10 → −50 → −71 → −145 (22:59:34).** The window (60 s fixed) stayed open after convergence
> and the coarse steps kept firing on the ±60–150 µs post-event noise, each 80 % step over-correcting
> against the other board: stairs AWAY from sync. **Build 39:** the window closes once |err| has been
> inside the arm threshold for `resync_close_s` (5); in-window arm `resync_splice_us` 100 → 150 (above
> the block noise). Build 38's re-open rule (> 400 µs) is included.
>
> **WHY the stairs away from sync (22:58:48 injection, attributed actuator by actuator):** during
> the slow drift (+18…+45 s) `trimB − trimA` = −7…−10 ppm against an equilibrium of ≈ −5.5 (crystals
> A +42.3 / B +36.8); the surplus −2…−4 ppm IS the wire slope (−2…−3 µs/s). Both boards read the
> same small positive error (common deadline wander, +30…+130 µs) but **A was in its resync window
> at kp 0.05 and B in steady state at kp 0.008** → (0.05 − 0.008) × 80 µs ≈ 3.4 ppm of differential
> rate, refreshed every block: the stairs. The ±80/250 µs steps at +45/+54 s are the same thing in
> position form (A's coarse steps on an error that was mostly common). **Rule: any gain that only
> one board has converts common-mode error into differential motion.** Build 40 removes the
> in-window PI boost (rate gains identical across boards; position corrections resync). Builds 38/39
> (re-open, close-on-converge, arm 150) stand — they bound the position steps, they were not the
> cause. The knee's error-proportional boost carries the same asymmetry whenever one board is above
> the knee and the other is not; it is tolerated because knee 150 sits above the wander, and noted.
>
> **The knee is the same asymmetry, chronic:** 22:13–22:59, one board above knee 150 with the other
> below in **21.2 %** of paired blocks (both above 8.9 %), median kp asymmetry 0.004 ppm/µs → 0.4 ppm
> of differential trim per 100 µs of common wander, for as long as the crossing lasts. Prediction:
> with a flat rate gain the 60–120 s wire term (4.6–9.7 µs today) shrinks. **Build 41: knee off**
> (tune_knee_us 1e6 → flat tau 120 / Ti 600 on both boards); acquisition = coarse step-and-verify
> only. To be graded on both the injections and a 45-min structure function.
>
> **Build 40 measured (23:03:50, in-window rate boost removed):** boot < 100 µs from +34 s; 300 ms
> injections A/B/A/B: **11, 34, 10, 8 s**, zero faults, and — the point of the build — **no stairs
> away from sync**: every tail is flat (+14, −114 → −96, +60, −33). What remains is a standing
> residual of 50–115 µs after the position steps, below the 150 µs in-window arm, which the
> symmetric PI at tau 120 drains over minutes (B's 34 s was a −114 µs park). Resync ledger: 31 12 ·
> 34 11/31 · 36 8/9/13/17 · 37 9/–/7/15 · 40 11/34/10/8. Next after build 41's data: let the
> in-window coarse arm at 100 µs when the error has PERSISTED one measurement lag (two blocks) —
> the same step-and-verify principle, not a lower magnitude threshold.
>
> **Build 41 boot (23:14:47, knee off) — the sawtooth, attributed:** ±260…500 µs jumps (A "corrected
> −22 frames" = −500 µs at 23:18:33 etc.) with −1.7 µs/s ramps between (symmetric PIs at tau 120
> draining A −21 / B +140 µs). **The jumps are A's in-window steps on its own err_tag, which after a
> boot into a running group is mostly the ±150 µs COMMON deadline wander** — a one-board position
> step on a common error is a differential error; B, just under the arm, did nothing. Position form
> of the gain-asymmetry rule. **Build 43:** an in-window step above `resync_local_us` (300) acts on
> err_tag (local by construction); below it the GROUP render delta must agree in sign and the step
> is the smaller of the two; no differential evidence → no step, the symmetric PI handles it. Build
> 42's 100 µs arm / one-step-per-block is included but now gated this way.
>
> **Review of 22:14–23:27 (13 firmware commits, builds 31–43), asked for by the operator:** the
> evening's instability has ONE cause — to reach "<100 µs in 5 s" one-board corrective actions were
> pushed into the ±150 µs regime where the common deadline wander lives. Build 30 never acted below
> 1 ms (splice, 4-s wait) or 2.9 ms (catch-up). Each regression is the same principle violated:
> 31 splice at 100 µs (thrash), 32 200-ms cadence (false faults), 34 in-window PI boost (3 ppm
> differential drift), 41 boot steps at 150 µs on common wander (±500 µs sawtooth). Keep: 40 (no
> rate boost), 41 (knee off — to be graded), 33/35 (judge lag), 39 (close on converge), 38
> (re-open), 43 (group-delta gating below 300). **Fallback without a reflash:** `resync_splice_us 300`
> — the window's floor becomes "local by construction", resync ~10 s.
>
> **23:22:03 (build 41): a COMMON +300…400 µs deadline step** — B's err +223 → +413 (window re-opened,
> B stepped −14 and −10 frames = −540 µs), and A's err rose +194 → +277 two seconds later with no
> action. B's one-board step on a common step put −330 µs on the wire. So "local by construction"
> does not hold at 300 µs: common timebase steps reach 400. `resync_local_us` → 2000 (only
> starvation-class errors skip the group-delta gate); below that every in-window step needs the
> differential measurement to agree. Included in build 43's compile if it makes 23:27.
>
> **Gate availability:** over the last hour A's reports carried a valid group render delta on 895 of
> 1433 (62 %; 34 % `render none`), typical |delta| 65 µs. So the group-gated in-window step is
> available roughly two reports in three, at the 3.3-s report cadence — the price of acting only on
> differential evidence is a slower resync (order 10 s) when the error is under 2 ms; above it the
> starvation-class path is unchanged.
>
> **Correction to the "group-wide" delivery pauses (2026-08-29 morning census, 11:00–11:40):** ring
> ran dry 21× on B, 7× on A, **0× on the observer**. Last night all three dipped together; this
> morning it is B-dominated and the observer sees nothing — so at least part of the problem is
> B's own link (B also logs 10 mapping flaps/hour to A's 2, and B is the board that stalled,
> wedged and rebooted unobserved at 22:46). **Refined:** A's 7 were all its own 11:28 OTA reboot —
> zero genuine pauses on A this morning; B's genuine events form ONE cluster, 11:32:24–11:35:12
> (17 dry-ring reports, two bailouts), observer 0. RSSI is fine and B is the strongest (−44 dBm vs
> A −48, observer −43), so it is not signal strength: a B-local receive stall (driver/AP-side per
> client) distinct from last night's server-wide class where all three rings dipped together.
>
> Two findings from running it: **"SPLITINJECT ramp complete" is an unreliable witness** — it
> logs only when the zero lands on a chunk that spends a whole frame, and this run reached zero
> silently; use the SYNCX `drift`/`split` step as the positive control. And the boards wobble
> ±150 µs in ANTI-PHASE post-reflash (render_align confirmed off) — judged on the settled
> window below.
>
> **Not yet run:** the flow-control read-site trace, tau grading (10 s vs 30 s), the cold-boot
> peak measurement, and the settled-bench comparison against the 2026-08-28 baseline
> (wire MAD 20.0 / sd 46.7 / p2p 242.8) — the last needs twenty uninterrupted minutes.

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

  The cold-boot wind-up plays **unmuted**, and the loop is running throughout — so the error follows
  the **closed-loop PI response to a `crystal_ppm` rate step with the integral at zero**, not an
  open-loop accrual. With `Ti = tau`, damping is `zeta = 0.5` and the peak is `~0.5·crystal/sqrt(Ki)`:

      tau = 30 s   peak ~600 us   vs the 1 ms splice threshold -- 1.6x margin
      tau = 10 s   peak ~200 us   -- 5x margin

  **Both are below the threshold, so the expected number of splice episodes during wind-up is ZERO**
  and the transient should be silent. Grade the peak on the bench: it is one number, visible in the
  first 30-90 s of a cold boot. The 1.6x margin at tau = 30 s is thin enough that ordinary variation
  in `crystal_ppm` across boards or with temperature could cross it — a second argument for the
  shorter tau, alongside mid-band convergence speed.

  For reference, `fast_splice_` runs **episodes, not lone splices**, if it does arm: it requires the
  effective error to hold at or above threshold for `FAST_SPLICE_PERSIST_US` (4 s), then corrects one
  frame per chunk (~870 µs/s) until inside `FAST_SPLICE_RELEASE_US` (300 µs) or 128 frames.

  Persisting the crystal offset across boots would remove the transient entirely; it is a separate
  work item and is not part of this plan.

  > **REVIEW 5 — the episode arithmetic is wrong a third time: it models the error as UNCORRECTED
  > while the loop is running.** "~1 ms accrues every ~25 s" is the open-loop rate; from handoff the
  > P term opposes the drift and the integral winds concurrently, so the error follows the
  > closed-loop PI response to a 40 ppm rate step with the integral at zero. With Ti = tau the
  > damping is ζ = Kp/(2·sqrt(Ki)) = 0.5 and the peak is roughly 0.5·crystal/sqrt(Ki): **~600-700 µs
  > at tau = Ti = 30 s, ~160-190 µs at 8-10 s — both BELOW the 1 ms splice threshold.** The expected
  > episode count is zero, not one per 25 s. Better news than claimed, but the narrative should
  > state the closed-loop peak (and grade it on the bench) rather than an accrual rate that assumes
  > the loop it describes does not exist.

  > **RESPONSE — correct, and the arithmetic checks. Third revision of this same paragraph.**
  >
  > Verified: with `Ti = tau` and `tau = 1/Kp`, `Ki = Kp/Ti = Kp²`, so `sqrt(Ki) = Kp` and
  > `zeta = Kp/(2·sqrt(Ki)) = 0.5` exactly. The response to a `crystal` rate step with the integral
  > at zero peaks at roughly `0.5·crystal/sqrt(Ki)`:
  >
  >     tau = 30 s   Kp = 0.033   peak ~ 0.5 x 40 / 0.033  = ~600 us
  >     tau = 10 s   Kp = 0.1     peak ~ 0.5 x 40 / 0.1    = ~200 us
  >
  > Both below the 1 ms threshold, so **the expected episode count is zero** and the wind-up should be
  > silent. I had modelled the error as accruing open-loop while describing a loop whose entire job is
  > to stop it accruing — the first version claimed four single-frame splices, the second ~30-frame
  > episodes every 25 s, and both assumed the controller was not running.
  >
  > **It also sharpens the tau choice, which is worth more than the correction itself.** At tau = 30 s
  > the peak is 600 us against a 1 ms threshold — 1.6x margin, so ordinary variation in `crystal_ppm`
  > across boards or temperature could push a cold boot over it. At tau = 8-10 s the margin is 5x.
  > That is now a second independent argument for the shorter tau, alongside the mid-band convergence
  > speed from REVIEW 2. Grade the peak on the bench: it is a single number visible in the first
  > 30-90 s of a cold boot.

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
  engages at all. **Adopted: converged LATCHES once `|err_tag|` has stayed inside `converge_fine_us`
  for a full integral time constant (Ti), and is cleared only by mute and hard events** — the same
  clearing conditions as today (set at `:3091`; cleared at `:3522`, and at `:2698`/`:2724` by the
  mute term `st.converged = st.converged && !mute_now`). It must latch: `converge_fine` sits
  ~8x below the splice threshold, so an un-latching definition would clear on any error above ~125 µs
  and disarm the splice path at exactly the errors splices exist to correct — a deadlock, not a
  degradation.
* **The splice in-flight horizon must be re-derived, not inherited.** `SPLICE_HIST = MEDIAN_WINDOW/2`
  (15 chunks) compensates for a splice reaching the 31-chunk median only after half a window. Driven
  by `err_tag` the blind spot is a different quantity: one pipeline depth (~10-12 chunks) before a
  splice shows in rendered audio, plus N/2 arrivals of averaging. Similar magnitude today by
  coincidence. **Derive it as `pipeline_depth_chunks + N/2`, taking the depth from the measured
  `render_latency()`**, so it tracks `buffer_duration`, the DMA span and N instead of assuming them.
  At ~870 µs/s of correction a ~300 ms blind spot hides ~260 µs — most of the 300 µs release band,
  and enough to overshoot and re-arm.

  > **REVIEW 5 — the name checks out (`converge_fine_us`, `snapcast_client.h:73`, config
  > `converge_fine`, default 2 ms), but the definition is half-stated.** Today `converged` is a
  > LATCH: set once (`:3091`), cleared only by mute and hard events (`:2698`, `:3522`), not by the
  > error re-leaving the band. Say whether the new definition latches the same way — it must,
  > because on the bench `converge_fine` runs well below the splice threshold (the config comment
  > calls 1 ms "8x converge_fine"), so an un-latching definition would disarm the splice path at
  > exactly the errors it exists to correct.

  > **RESPONSE — correct, and the omission would have deadlocked the fast path. It LATCHES.**
  >
  > Verified: `st.converged` is set at exactly one site (`:3091`) and cleared at one (`:3522`) — a
  > latch, not a window test — and the "1 ms is 8x converge_fine" comment appears twice in the source
  > (`__init__.py:309`, `snapcast_client.cpp:611`), so the band really does sit an order of magnitude
  > below the splice threshold.
  >
  > The consequence of getting this wrong is a **deadlock, not a degradation**: an un-latching
  > definition would clear `converged` as soon as the error left a ~125 µs band, which disarms the
  > `fast_splice_` gate — and the splice path is the only mechanism that acts between ~125 µs and the
  > millisecond scale. The error would then have to be corrected by the very path its own size had
  > just disabled.
  >
  > **Definition, now complete: converged LATCHES once `|err_tag|` has stayed inside `converge_fine_us`
  > for a full integral time constant, and is cleared only by mute and hard events — the same clearing
  > conditions as today.** "Half-stated" was exactly right: I specified the set condition and said
  > nothing about the clear, which is where the behaviour actually lives.

  > **REVIEW 6 — one miscount in the verification above, semantics unaffected.** "Cleared at one
  > site (`:3522`)" is wrong: `st.converged` is also cleared at `:2698` and `:2724`
  > (`st.converged = st.converged && !mute_now`) — three clear sites, of which two are the mute
  > clears. The adopted semantics ("cleared only by mute and hard events") already covers all
  > three, so nothing changes; corrected here only so the verification record stays exact.

  > **RESPONSE — correct, and the way I got it wrong is worth more than the correction.**
  >
  > Confirmed: four assignment sites, `:2698` and `:2724` (`st.converged = st.converged && !mute_now`),
  > `:3091` (set), `:3522` (clear). Three clear sites, two of them the mute clears.
  >
  > **My grep was `st.converged = true\|st.converged = false`.** It could only ever find literal
  > assignments, so a conditional clear was invisible to it by construction — and I reported the
  > result as "set at one site, cleared at one" with the confidence of having checked. The pattern
  > matched what I expected to find rather than what I needed to know.
  >
  > That is the same defect as the 256-byte log ceiling and the required trailing field in `SYNC_RE`,
  > which is why it belongs in the record rather than being quietly fixed: **a search narrow enough to
  > confirm your expectation will not disturb it.** The correct query was `st\.converged\s*=`, which
  > costs nothing extra and cannot miss a form.
  >
  > The adopted semantics were stated behaviourally — "cleared only by mute and hard events" — and
  > cover all three, so the design is unaffected. It happens to be right, not by having checked
  > properly, and that distinction is exactly what the verification record exists to preserve.
  >
  > `CLAUDE.md` gains the rule: **when verifying that state is only touched in known places, match the
  > variable and the assignment operator, never the expected values.**

  > **REVIEW 5 — the splice in-flight compensation horizon must be re-derived for `err_tag`.**
  > `SPLICE_HIST = MEDIAN_WINDOW / 2` (15 chunks, `snapcast_client.h:625`) exists because a splice
  > reaches the MEDIAN only after half its window; the comment above it records the overshoot limit
  > cycle that happens without it. Driven by `err_tag`, the lag is different in kind: one pipeline
  > depth (~10-12 chunks) before a splice is visible in rendered audio, plus half the averaging
  > window. Similar magnitude by coincidence, but it is a different quantity and must be derived
  > from the pipeline depth, not inherited — an episode correcting ~870 µs/s against a ~300 ms
  > blind spot overshoots the 300 µs release point without it.

  > **RESPONSE — correct, and this is the subtlest thing found in five rounds. Added as a work item.**
  >
  > Verified: `SPLICE_HIST = MEDIAN_WINDOW / 2` = 15 chunks, and `splice_sum` is the total of splices
  > within that horizon, subtracted as `in_flight_us` before the threshold test. Its purpose is
  > specific — a splice changes the rendered error immediately but reaches the 31-chunk MEDIAN only
  > after half a window, so without the compensation the loop keeps splicing against corrections it
  > has already made. The comment above it records the resulting limit cycle.
  >
  > **Driven by `err_tag` the blind spot is a different quantity with a similar magnitude, which is
  > the dangerous kind of coincidence.** It is one pipeline depth (~250-300 ms, ~10-12 chunks) before
  > a splice appears in rendered audio, plus half the averaging window (N/2 arrivals). Inheriting 15
  > chunks would be right by accident today and wrong the moment `buffer_duration`, the DMA span, or
  > N changes — none of which the constant references.
  >
  > The magnitude confirms it matters: one frame per chunk is 22.7 µs / 26.1 ms = **~870 µs/s**, so a
  > ~300 ms blind spot hides ~260 µs of correction — most of the 300 µs release band, which is
  > precisely enough to overshoot it and re-arm.
  >
  > **Work item: derive the horizon as `pipeline_depth_chunks + N/2` and take the depth from the
  > measured `render_latency()` rather than a constant**, so it tracks the configuration instead of
  > assuming it. This is the third thing in this plan that was correct only because two unrelated
  > numbers happened to be close (the others: the 1 ms / 2 ms threshold ordering, and `SPLICE_HIST`
  > matching the pipeline depth) — worth stating as a pattern, because each one is a latent bug
  > waiting for a config change.
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

### 2026-08-29 23:27–23:50 — build 43 graded; the group-delta gate was inverted; the analyser's probes are swapped relative to the logs

**Build 43 (flash 23:27:45), gate as written:** boot to |wire| ≤ 100 µs held 5 s: +31 s (steps A 2, B 5).
Injections (300 ms): A 12 s, B **not inside 100 µs within 75 s**, A 57 s, B 69 s. Build 41 was 11/34/10/8.
The gate made resync *worse*, and the log says exactly how.

**Mechanism (23:32:14, injection on B).** Hard resync inserted 1958 ms of silence; residual err_tag −380 µs
(early). Group render delta on B: −532 µs (RALIGN half-gap −266). Both signals said *early*, yet the gate
test `(gd > 0) != (target < 0)` read "disagree" and refused every step for 60 s — `corrected -0/+0` on
every report — and the PI (kp 0.008 ppm/µs) closed 400 µs in ~2 min, the +500 → +50 µs tail in the
injection trace. Same at 23:36:37 (err −1500, gd −1545, one minute flat) until the window closed and
the *fast splice* (outside the window) took it in one bite at 23:37:43.

**Signs, from the definitions and not from a label:** `phase = TSF(render) − server_time` (larger =
rendered later); `delta = mine − mean(peers)` → **delta > 0 = LATE**; `deadline = server_ts + buffer −
offset + bias` → **positive bias = plays LATER**; `err_tag = render − deadline` → > 0 = late. RALIGN's
`bias −= delta·gain` is right by derivation. The comment "delta > 0 = early / positive bias = earlier"
(PLAN 17:03 step test, code comments) came from reading the wire header "B − A, positive = B later".

**The analyser's probe b is on board A (e985e8).** Attribution table 23:27–23:42, 30-s bins:
`fs_b − fs_a` (analyser) against `(trim − crystal)_B − (trim − crystal)_A` (what the boards asked for):
+4.10/−3.94, +3.73/−3.42, +2.79/−2.76, +0.71/−0.44, −3.06/+2.81 — equal and opposite in every bin.
The channel the CSV calls "B" speeds up when board *A* asks for rate. So on the current probe placement
**wire + = board B (f04d74) EARLIER**, and every wire-sign statement since the probes were placed must be
read that way; the CSV's firmware columns (`crystal_b_ppm` from b.log) are correctly labelled by log,
so the *same file* carries both conventions. Consistent with everything tonight: 23:32 wire +394 =
B early, err_tag −380 ✓, PHASEIN B phase 383 µs smaller ✓; 23:37:43 B inserts 52 frames (later), wire
+1576 → +16 ✓. `scripts/i2s-skew.py` is the operator's file — not touched; noted here and in HANDOFF.

**Build 44** (`(gd > 0) != (target > 0)`, comments corrected). Same code otherwise.

**The 23:42:12 step (wire −160 → −460 in 20 s), while the gate was inverted.** A's TSF consensus spread
reached 851 µs; the deadline fell to the local fallback for 1.2 s ("holding integral +56.42 + P +2.41"),
re-engaged at 23:42:14 — engage opens a resync window — and A dropped 9 + 6 frames (−340 µs) on an
error that was **common** (A +327, B +572 at that instant). gd was −204 (A early) against err +327
(late): a genuine disagreement, which the inverted gate read as agreement. A swung +327 → −310; B, at
+570 with no step, decayed by PI. The ±300 µs common wander at 23:41–23:44 coincides with consensus
spreads of 600–900 µs and `Offset ramp +19 ppm (map +24)` glitches on both boards — timebase, not
playout; open item.

**"Overshooting 0" (23:38–23:44, operator's observation).** After B's 1.4 ms splice the wire crossed
zero and ran to −165 µs at −0.8 µs/s: the analyser's own `fs_b − fs_a` was +2.3 → +0.7 ppm over the
same two minutes and the boards' requested rate differential matched it bin for bin — it is the two
P-terms (kp 0.008 → 3 ppm at 400 µs) acting on errors that differed by 100–150 µs for a block-lag
after a one-board position step, then the differential PI's τ = 120 s step response. Not an actuator
fault; not the integrals (both moved +0.3 ppm together).

### 2026-08-30 00:00–00:05 — build 44 injections; align was the rate limit on the slow zero-crossing; an 18 ms timebase jolt

**Build 44 injections** (gate sign fixed): A 21 s, B **>75 s (flat at +1190)**, A 11 s, B 31 s. B's
23:51 episode: err −1400 with gd −1190 (agree) but no step for 17 s because the delay loop was OUT OF
RANGE (≥ 1000 µs holds the integral and returns before `dl_err_us` is refreshed for the coarse path);
then one +61-frame insert (1383 µs, more than the 553 µs then standing — the target was a block stale)
put B at +600 late, where it sat 45 s with **no step and nothing in the log saying which guard held
it**. Build 45 adds `RSTEP`/`RSKIP` — one line per in-window block: target, source, gd, ok, step,
adjust, and for a skipped block whether it was the blank or the one-step-per-block rule.

**"It crosses zero so slowly" (operator, 23:58).** The wire's crossing rate was 0.5 µs/s. Two actuators
own the differential: the PI (τ = 120 s, symmetric, and it drives err_tag → 0 on *each* board, which
does not make the wire zero) and render_align, the only actuator that moves the two boards' zero-points
relative to each other — capped at `align_step_us` 4 per 10-s cycle = **0.4 µs/s**, which is the slope
that was being watched. Set at runtime 00:01:09 on both: `align_gain 0.5`, `align_step_us 20`. Within
40 s B's bias went +31 → +91 in 20-µs steps, group deltas shrank (B −60 → −40, A +32 → +16) and the
wire closed at ~1.2 µs/s. Symmetric by construction (each board moves half its group delta); the cost
is the exchanged phase's ~10 µs noise × 0.5 into the deadline, held out by the 15 µs deadband. Both
boards' RALIGN signs read consistently with the corrected wire mapping (A late/B early ↔ wire +).
Not yet a compiled default; grade steady state first.

**00:00:12 timebase jolt, group-wide.** All three boards — observer included — saw −17.8 ms at the same
instant (`Consensus spread 1484 µs`, `Offset ramp +49.91 ppm (map −6.72)`), fast-spliced, hit the
128-frame bound "with −10.7 ms still standing", handed back to the PI, and the mapping returned by
00:00:22 (`map +40.56`; B re-opened its window on −628). The observer drives no DAC, so this is the
shared TSF→server mapping stepping, not playout — the consensus-jolt open item, now with a size: 18 ms
for ~10 s. The wire moved only ~+130 because both speakers moved together.

### 2026-08-30 00:03–00:06 — why align overshoots: its actuator is a deadline, its sensor is the audio, and the PI's τ sits between them

00:03:30–00:04:50, gain 0.5 / step 20: A's group delta sat at −50 → −53 → −64 → −51 → −41 for 80 s
while A's bias marched +116 → +136 → +156 → +176 → +196 → +210; the wire moved −40 → −52 → −43;
A's err_tag read −150…−250 (early) the whole time. Each 20 µs bias step moves A's *deadline* later;
the audio only follows when the PI (τ = 120 s) has turned that err_tag into position. The exchanged
phase is the audio, so it keeps reporting the gap for two minutes after the correction that closes it
is already in the deadline — and align, an integrator, keeps stepping. The code comment says "the loop
delay is ~2 reports"; it is ~2 reports plus τ. With gain 0.1 that lag gave the slow ±100 µs sawtooth
seen all evening; with 0.5 it gives this. Gain back to 0.1 at 00:05:28 (step cap left at 20).

**Fix (build 45): publish the settled phase.** `phase − err_tag` is where this frame's successors will
render once the PI has removed the error it is already acting on; publishing that instead of the raw
phase takes τ out of align's loop (and out of the resync gate, which reads the same delta), symmetrically
on every board. err_tag is what the PI is about to remove by definition, so no new tunable. Prediction:
group deltas fall within one or two align cycles of a bias step instead of ~120 s, so gain 0.5 becomes
usable, and the boot/injection tails that were align-limited close in tens of seconds.

### 2026-08-30 00:14–00:29 — builds 45/46: what RSTEP showed the first time it ran

**Build 45** (RSTEP/RSKIP + settled-phase publish): boot wire ±500 µs; injections >75 / >75 / >75 / >75 s.
`RSTEP src=tag gd=+45 ok=0` against err −1443: publishing `phase − err_tag` makes the group delta ≈ 0
whenever the error is what the PI will remove — the gate's evidence, zeroed by construction. Reverted
in 46; align's lag needs the peers' err_tag (a beacon field), not a local subtraction.

**Ledger steps in the window had no throttle.** A, 00:14:58: `err −1475, −3470, −5465, −7461, −9454`
on consecutive chunks, then `+26272` — one error stepped five times before the pipeline showed the
first. 97 ledger steps > 2 ms on A, 154 on B, in three minutes. Tag steps taken: A 3, B 0 (gd unknown
163/236 times at boot — the phases pair slowly after a double reboot). Build 46: ledger steps wait out
the blank like tag steps.

**Build 46 boot (00:27:50):** a 1-Hz ±4.5 ms limit cycle on A — `tag −2680, ledger +3592, tag −2732,
ledger +3656 …` — the two sources alternately stepping on each other's step. Stopped at 00:28:48 with
`resync_win_s 0` on both (window never opens; normal coarse thresholds + fast splice, the build-39
behaviour). Build 47: while tags are fresh the ledger does not step in the window at all; its role is
the first step after a hard resync, when tags are blanked.

**The 51 ms flip (open, instrument).** `RECON drift` = −51 ms exactly when the mixer's transfer buffer
reads full (`xfer=50000`), 0 otherwise; 40 % of reports, both boards, all night. Measured depth =
transfer-buffer available + sink in-flight + sink audio (mixer_speaker.cpp ~688); the full-buffer case
carries ~51 ms the accounting does not. With tags live it is harmless; after a TAGFAULT it is the
ledger the board steers on for 180 s — B's 56 ms phantom hard resync at 00:14:04. Two fixes owed: name
the double-counted term; re-trust tags on agreement, not on a timer.

### 2026-08-30 00:33–00:36 — anatomy of a post-storm split that never heals (B, window off)

B-only starvation 00:33:09 → 56/68 hard resyncs in two reports → from 00:34:14 `err_tag −17.6…−20.8 ms,
err_live +2.4 → −0.05 ms`, diff −20.0 ms constant for 2+ minutes. **The tags are right:** the analyser
lost correlation (boards > 17 ms apart) and B's own `Playout depth +20011 us vs group … for 126 s`
agrees. The ledger is the thing that broke in the storm (mis-accounted drops), and every actuator that
reads it is now steering toward a phantom:

* the fast splice engages on the tag error, inserts its 128-frame bound (2.9 ms), declares
  "measurement fault", hands back to the PI, re-arms 4 s later — six times;
* the coarse path alternates source: each splice blanks the tags (`RENDERTAG sup=1`, ages to 8.6 s) →
  `tags_fresh` false → coarse on the ledger median (+2.4 ms late → drop) → tags return → coarse on tags
  (−20 ms → insert). `corrected −144/+114 frames` per report: the two sources undo each other;
* TAGFAULT — the mechanism that exists for exactly this — never fires: its judge watches only
  `coarse_act_err_us`, and with the source flipping every few seconds no single tag-driven coarse
  action is ever judged three times against a still-standing err_tag.

Cleared only by the build-47 reboot. What it needs (not tonight): a split judged on `err_tag − err_live`
holding for N seconds *while the group depth agrees with the tags* → re-anchor the ledger from the tags
(or reconnect) — the evidence is already in the SHADOW line; nothing acts on it.

### 2026-08-30 00:42–00:51 — build 48 (ledger first step only): B 10 s / 10 s, A >75 / >75, and the reason for each

Boot: no limit cycle (steps A 8, B 6; ledger 4/4). Injections: A **>75 s** (tail +170), B **10 s**, A **>75 s**
(tail −330), B **10 s**. Zero TAGFAULTs.

*B's two successes* are the first clean step-and-verify runs: ledger first step (+5188 / +1548), two tag
steps, inside 100 µs at +10 s with a flat tail (+70, −75).

*A's two failures* are the gate without evidence: `gd=unknown` on every A decision in the first run,
and the residual (−235 by the tags, +170 on the wire) fell to the PI. Build 49 (per-block phase publish)
is aimed at exactly this.

**Overshoot inside the sequence, both boards.** After the ledger's first step (+2277) the next tag block
read *higher* (+4068), was stepped (+3254), the next read +2286, stepped again (+1828), and the next
read −235: the blocks used for the second and third steps still predated the earlier steps. The
in-window blank is `min(tune_blank_ms, tune_resync_blank_ms)` = min(500, 1200) = **500 ms**, below the
pipeline (~250 ms) plus one block (~650 ms) the comment promised. `RSKIP … since_act=-14 ms blank=500`
says the same: the block being judged is older than the action. Build 50: the window uses
`resync_blank_ms` outright. Prediction: one ledger step, one tag step, done — no third step, no sign
flip, and the tail lands inside the noise instead of 200–300 µs off.

### 2026-08-30 00:52–00:59 — build 49 (render phase every block): 9 / 11 / 19 / 15 s

First group delta at boot+14 s on both (was +21…+44 s); `gd` known on 175/175 A decisions and 174/175 B
after the first seconds (build 48: unknown on every A decision). Injections: A 9 s (tail +16), B 11 s
(−28), A 19 s (−100), B 15 s (−100). Zero TAGFAULTs. Every sequence still shows the stale-block second
step (ledger +1470 → next tag block +2822 → stepped) and the resulting third/fourth corrective steps of
opposite sign; build 50 carries the 1200 ms in-window blank for that.

**Analyzer after a boot:** `best coef 0.00` for the first 86–117 s after every reboot tonight while both
I2S clocks are measured and the boards, at first correlation, are within 4–250 µs. The instrument does
not re-align its PCM decode promptly after an I2S restart; the post-boot wire is unmeasured for ~100 s.
Operator's script — noted, not changed.

### 2026-08-30 01:00–01:09 — build 50 (1200 ms in-window blank): 8 / 41 / >75 / … — the blank did not cover the first step

Sequences still read: hard resync → ledger +1220 at t+1.8 s → **tag +1643 at t+2.7 s** → corrective
steps of the other sign. The blank governs the tag path through `coarse_act_us`, which only a TAG step
set; the ledger's first step set nothing the tag path waits on, so the next tag block — still predating
the ledger step — was stepped as before. Build 51: any in-window step sets `coarse_act_us`.

Tails +160 / −180 µs with the gate refusing on `|gd|` 15–89 < 100: the group delta reads the residual
50–150 µs smaller than the wire does. That is the delta's own accuracy (pairing skew, single samples),
and it is the floor for how small a residual the gate can *see*; below it only align and the PI act.
Next lever after 51: a better group delta (pair by extrapolation; several samples per report), not a
lower arm.

### 2026-08-30 01:11–01:20 — build 51 (any window step blanks the tag path): 11 / >75 / 14 / 65 s

The tag step now waits 1.6 s after the ledger step, and each sequence is a clean geometric descent:
`+5285 → +647 → +396 → +164 → +107` (A), `+8564 → +751 → +756 → +172 → +105` (B), one step per
~1.3 s, five steps, arm reached ~7 s after the hard resync (which itself comes ~2 s after the injection).
Each step realises only ~half of itself by the next decision (+647 stepped +517 → next block +396, not
+130), so the effective gain is ~0.4 and it takes five rounds. The fast splice already solves this with
in-flight accounting (`splice_hist`, effective = err − in_flight); the coarse target does not use it.

B's tails (−144, −190 for 40–70 s) are below the 100 µs arm by the tags' reading (+105 → +84 → stop)
while the wire still shows 150–190: the same tag-vs-wire ~60–100 µs residual as build 50, handed to
align/PI. Not a threshold problem; a measurement-accuracy floor for both the tag error and the group
delta at this timescale.

### 2026-08-30 01:22–01:31 — runtime A/B `resync_blank_ms 2000` on build 51: the "half-realised step" was ring depth

Four injections with a 2 s blank: 63 / >75 / 67 / >75 s — slower (cadence halved, tails unchanged), and the
descent still `+290 → +169 → +107`. Then the wire, step by step against RSTEP (`adj` frames × 22.7 µs):
adj 10 → wire moved 233 µs; adj 5 → 118; adj 3 → 117; adj 4 → 138; adj 7 → 280; adj 4 → 164; and each
big first step (85, 94, 118 frames) appears as exactly 1930 / 2130 / 2674 µs — **1:1, every step, ~2 s
after it was applied.** The drop is taken from the chunk being PUSHED, and the ring holds ~1.7 s of
audio ahead of the DAC (`buffered 1724 ms`), so the tags cannot see a step for ring depth + pipeline.
The 1200 ms blank judged the next block before the step existed there — builds 48–50's doubled second
step and sign flips — and 2000 ms saw about half of it. Not a gain, not a units error (checked
`push_chunk_`: skip = frames × frame_bytes), a horizon. The analyser adds ~1 s of its own.

**Build 52:** the coarse target subtracts the in-window steps applied within ring depth + one block
(`pend=` on RSTEP). Prediction: second step ≈ 0.16 e, third ≈ 0.03 e; inside the arm three steps after
the hard resync (~4 s), no sign flips. The same accounting the fast splice has had since `splice_hist`.

### 2026-08-30 01:35–01:44 — build 52: `pend=+0` at every decision (>75 / 11 / 44 / >75 s)

The subtraction was in place but its horizon was `pipe_depth_frames` (pushed − played ≈ 250 ms) plus
a block — under a second — while the step is taken from the chunk entering the **ring** (`ring_ms`,
~1.7 s ahead of the push). Every recorded step had aged out before the next decision, so nothing was
subtracted and the sequences read like build 51's (`+1288 ledger → +1069 → +646 → +400 → +113`: the
ledger step invisible 1.6 s later, half-visible at 2.9 s). Build 53: horizon = ring + pipeline + block.

### 2026-08-30 01:47–01:58 — build 53 (ring horizon in the subtraction): >75 / 51 / >75 / >75 — worse, and instructive

`pend=` now non-zero, but binary aging cannot represent a step that lands over a whole block: at 2.8 s
the ledger step was counted as landed while the raw tag error had dropped only 65 % of it, and later
decisions alternated between subtracting steps that had partly landed and counting none (`err=-571 …
pend=+2062` refused; next block `pend=+0`, err +902, stepped again). Raw tag error also read ~1.1 ms
above the ledger's at the first tag decision (3518 vs 2394): the small tag/ledger offset after a hard
resync, on top of everything.

Build 54 drops the cleverness: in the window the blank is at least ring + pipeline + block, computed
from the live ring depth. One decision per ~2.7 s, each on a block that wholly post-dates the previous
step. Prediction: ledger step → one tag step ~2.7 s later at ≈ 0.2 e → inside the arm ≈ 5.5 s after
the hard resync, no sign flips; if it does not beat build 49 (9/11/19/15) the build-49 behaviour
(1200 ms blank, no accounting) is the one to keep and the speed has to come from the first step's gain.

### 2026-08-30 02:00–02:11 — build 54 (blank ≥ ring + pipeline + block): 47 / >75 / 70 / >75

`RSKIP` every 0.65 s through the blank: after the ledger step (+3028 at 02:03:35.8) the tag error read
+5518, +5459, +5484, +5502 — flat — until 02:03:39.3 (+2990). The step became visible ~3.4 s after it
was applied; the computed blank was 2.6 s, so the tag step at 2.9 s (+4401) again doubled it. A block
mean is wholly post-step only a full block after the landing: horizon = ring + pipeline + **two**
blocks (build 55). Also 55: the ledger's first step is arithmetic and takes gain 1.0; the 0.8 damping
stays on the tag steps, which act on the lagged measurement. Prediction: ledger step → ~3.4 s → one tag
step of ≈ tag/ledger offset (~1–1.7 ms tonight) × 0.8 → inside the arm ≈ 7 s after the hard resync.

### 2026-08-30 02:13–02:24 — build 55 (two-block horizon, ledger gain 1.0): >75 / 65 / 46 / 64 — the ledger's step never reaches the tags

Clean cadence now (one decision per ~3.5 s, every tag step fully realised by the next: `+2498 → step
+1998 → +311`, `+4545 → +3636 → +509`). But the ledger's first step is invisible to the tags even a
full horizon later: `ledger +1938` → tags +2498; `ledger +4570` → tags +4545; `ledger +4393` → +4426;
`ledger +1698` → +2179. Either the frames it drops never reach the audio the tags measure, or the
ledger's error after an *early* hard resync (silence inserted) describes different audio than the
tags' — in both readings it is the ~1.5–2 ms tag/ledger offset seen after every hard resync tonight,
and every sequence pays one extra 3.5 s round for it. Then 0.8-gain tag steps descend geometrically:
four rounds × 3.5 s. Final runtime A/B: `resync_gain 1.0` — each tag step is now fully realised before
the next decision, so damping only buys rounds. Whether the ledger step is lost or mis-measured is the
first thing to find tomorrow (`push_chunk_` drop on the chunk after a silence insert; compare the wire
at the ledger step's expected landing).

### 2026-08-30 02:24–02:31 — `resync_gain 1.0` on build 55: >75 / 40 / **9** / 51 — and the halving named

Runs 3 and 4 (residual above `resync_local_us`, no gate bound): ONE tag step (+5193, +4808), window
closed converged 6.5 s later, wire inside 100 µs at +9 s (run 3). Runs 1 and 2 (below it): `+864 →
+430 → +222 → +108` at gain 1.0 — each step exactly half. The printed target is the gate's bound, and
the bound is `|gd|`, which for two devices is by definition HALF the pairwise gap (mine − mean of both).
The evidence the gate should bound by is the gap to the others: `gd · n/(n−1)`. Build 56.

Floor after that: hard resync ≈ +2 s after the injection, first tag decision ≈ +1.8 s later, the step
lands ≈ 3.5 s later, verify one round after → ~9 s. Under 5 s needs the step to land faster than the
ring (drop at the pipeline end, as the fast splice effectively does) or a shallower ring — a design
choice for tomorrow, not a tunable. Runs 4's −200 µs tail is the tag-vs-wire measurement floor again.

### 2026-08-30 02:33–02:41 — build 56 (gate bounds by the gap to the others; resync_gain 1.0): **9 / 27 / 9 / 9 s**

Every sequence: hard resync → ledger step (still invisible to the tags) → ONE tag step of the full
target 3.5 s later → window closed converged 6.5 s after that. Wire inside 100 µs held 5 s at +9 s in
three of four, tails −40 / +25 / −40 µs; run 2's tail sat at −260 (tag-vs-wire floor, then a +422 re-open
at +23 s) → 27 s. Zero TAGFAULTs all night from build 44 on.

**Night's ledger, 300 ms injection → |wire| < 100 µs held 5 s (A, B, A, B):**
41: 11/34/10/8 · 43: 12/>75/57/69 · 44: 21/>75/11/31 · 45: all >75 · 46 (window off): >75/>75/–/– ·
48: >75/10/>75/10 · 49: 9/11/19/15 · 50: 8/41/>75/45 · 51: 11/>75/14/65 · 52: >75/11/44/>75 ·
53: >75/51/>75/>75 · 54: 47/>75/70/>75 · 55: >75/65/46/64 · 55+gain 1.0: >75/40/9/51 · **56: 9/27/9/9**.
Build 41's numbers were the accidental over-stepping of a window with no throttle; 56's are one step
each, explained line by line.

**The 9 s is structural now:** ~2 s from injection to the hard resync (ring refill), ~1.8 s to the first
tag block, ~3.5 s for the step to travel ring + pipeline + block, then the close timer. To reach 5 s the
step has to land faster than the ring — apply the coarse correction at the pipeline end (as the fast
splice effectively does) or shorten the ring — and the ledger's first step has to actually reach the
audio (or be dropped from the design; today it costs nothing but also does nothing).

Runtime state left on both boards: `align_step_us 20` (default 4); everything else at compiled defaults.

### 2026-08-30 02:41–03:26 — build 56 steady state, 45 min untouched (`align_step_us 20`)

n=13589, median −10.3 µs, robust sd 9.0 µs, p2p 137 µs. Structure function: 1 s 0.22 · 10 s 1.28 ·
30 s 3.0 · 60 s 4.95 · 120 s 7.7 µs; 10-s block means robust sd 14.4 µs (within-block 0.31). Events: zero
starvations, bailouts, TAGFAULTs; 2 deadline fallbacks on A; 3 window re-opens on each board (the
|e| > 400 µs re-open now fires on common wander and takes a one-board step — the sawtooth risk the
gate was built for, now bounded by the gap rule; it is what widened the 10-s block sd from ~7 to 14
against builds 29/30). Align at step 20 also contributes 20 µs quanta. Steady state is otherwise the
same board as before the resync work: sub-µs at 1 s, wander-limited above 30 s.

### 2026-08-30 04:35 — unattended: group-wide starvation → TAGFAULT + reconnect on A (and the observer); B left with a 2.1 ms split

04:35:05 all three boards starved together (server/AP). A: tags +21.8 ms vs ledger +1.0 → TAGFAULT →
reconnect → by 04:36 tags and ledger agree at −600 µs and the PI is closing it. B: no fault, but from
04:36:05 `err_tag −450 … err_live +1650`, diff −2.1 ms, standing — below `TAG_SPLIT_US` (3 ms), so no
judge, no reconnect; the coarse path steers on tags, the fast splice and steering see the ledger's
+1.6 ms, and the two will fight or freeze exactly as at 00:34 (then 20 ms, now 2 ms). Third instance
tonight of a post-storm split that only a reconnect clears. The analyser lost correlation at 04:36:17
— the speakers' I2S restarted on reconnect, so that is its ~100 s blindness, not a 17 ms gap.

### 2026-08-30 06:07–07:07 — "sawtooth with a 5-minute period": what the hour actually contains

Wire, rival-gated 30-s medians, 45 min: sd 7.6 µs, p2p 29, mean −10. Autocorrelation decays smoothly
to zero at ~12 min — no line at 5 min; a meander with ~10-min correlation time on a standing offset.
r(wire, errB−errA) = −0.35, r(dwire/dt, trimB−trimA) = +0.30: the P-terms' rate noise integrating,
the √τ structure function seen since build 16. Group deltas: A median −8 (MAD 4), B +2 (MAD 4) — the
delta *sees* the standing offset every cycle; align does nothing with it because (a) deadband 15 and
(b) `int(−delta × 0.1)` truncates anything under 10 µs to zero. Both boards' biases sat frozen at
+195/+192 for the whole hour.

**Build 57:** fractional accumulation of align steps. Runtime on both: `align_deadband_us 5`,
`align_gain 0.03` (the correction reaches the audio through the PI's τ = 120 s; 0.1 per 10 s on that lag
hunted at ±100 µs on 2026-08-29), `align_step_us 20`. Prediction: the wire mean walks from −10 to
within ±3 over ~5–10 min and stays; the ±8 meander is untouched (that is P-term noise: `block_n` or τ,
each a response-time trade, or better tags).

### 2026-08-30 07:10–07:14 — why the wire is blind for 1.5–4 minutes after every boot: the boards play SILENCE until "Sync locked"

Not the analyzer. `push_chunk_(rec, drop, silent = !st.converged)` zeroes every slice until the servo
declares converged, and `frame_lag()` returns coef exactly 0.00 for a zero-variance channel. Tonight's
unmute delays (boot → `Sync locked … unmuting`): A 24 / 28 / 83 / 85 / 19 / 68 / **150 s**; B … 25 / 19 /
**186 s** (07:10 boot: A 07:13:19, B 07:13:55 → correlation at +191 s ✓). Converged requires the error
inside `2 × sync_deadband` for a median window with the timebase settled, AND the drift anchor within
100 µs — and A's anchor read **−49343** at 07:13:18 (the 51 ms `RECON drift` flip), "waiting for the
accounting to agree", so the instrument bug delays unmute too (up to 15 s by `UNMUTE_ANCHOR_MAX_WAIT_US`).

The long part is the PI: at boot both boards read the same ~−700 µs (common — the mapping, not a
mutual offset), the gate correctly refuses one-board steps on it, and each board closes it at τ = 120 s
before it will unmute. But mute-until-converged exists to hide *mutual* desync, and the group delta is
the direct measurement of that: at 07:11:20 both boards had |gd| < 30 µs while still 600 µs from their
deadlines. Proposal: unmute on group agreement (|gd| small for N s, or no peer) rather than on own
err_tag — seconds instead of minutes after a boot or reconnect — keeping the err_tag rule when the
delta is unknown. Operator's call: it trades a common ~0.5 ms offset from the server's timeline
(inaudible as such) for 2–3 minutes of silence.

### 2026-08-30 07:20 — build 58: unmute on group agreement (own error ≤ 4 ms); never_mute covers start-up

Operator: a few ms from the server's timeline is tolerable (3–5), mutual offset is not; and "never mute
means never mute, even during startup". Build 58: the unmute latch passes when own error is inside
2 × sync_deadband as before, OR the group delta is inside that band and own error ≤ `UNMUTE_COMMON_US`
(4 ms); delta unknown → own-error rule; `never_mute` no longer silences start-up at all. Prediction for
this boot: "Sync locked on group agreement" within ~20–40 s of boot (first pairing ≈ +14 s, a median
window of agreement), correlation on the wire at about the same time instead of +150…190 s. The build-57
align grade (deadband 1, gain 0.03) was cut short by this flash; it restarts after the boot settles.

### 2026-08-30 07:22–07:40 — build 58 boot; align at 0.03 crawls; build 59 delivers align steps by rate

**Build 58 boot (07:22:17):** connected/first audio +7.5 s; wire correlated **+14 s** at +28 µs (never_mute
now means no start-up silence); first group delta +15 s; B "Sync locked" +19.7 s (own error), A +26.4 s
"on group agreement" (own err 369 µs; the anchor held it 2.6 s reading −29 ms — the drift flip). Previous
boot: +150 / +186 s.

**Align at gain 0.03, deadband 1:** wire −60 → −31 µs in 12 min while the two biases moved a combined
78 µs and the deltas stayed at A −25 / B +17 the whole time — the correction is not reaching the phases
at the rate it is applied, because a bias moves the deadline and the audio follows through the PI at
τ = 120 s. The gain is small *because* of that lag; the lag is the thing to remove.

**Build 59 — ALIGN KICK:** a bias change of D is delivered as position immediately, by lowering/raising
the rate by up to 10 ppm until D has moved (5 µs in ~0.5 s), inside `delay_loop_update_` after the PI
output. Align's loop lag becomes the ~3.5 s tag visibility; runtime `align_gain 0.3`, `align_deadband_us
1`, `align_step_us 20`. Prediction: the wire's mean reaches 0 ± 5 µs within ~1–2 min of any offset and
holds; no multi-minute sawtooth. Risk to watch: the kick and the PI's own P response overlap for ~2 s
after each step (0.04 ppm at 5 µs — should be invisible).

**"A's rate PWM is larger than B's" (sub-second):** both brackets are the same width (A 53/309..47/274,
0.83 ppm; B 29/169..81/472, 0.88 ppm) — the difference is duty: A 0.97 (one opposite tick in ~33 → a
~0.3 s repeating bump), B 0.57 (toggles every couple of 10-ms ticks). Same 0.85 ppm × 10 ms ≈ 8 ns per
toggle; cosmetic on the fs trace. A second-order or dithered sigma-delta would whiten it.

### 2026-08-30 07:42–07:55 — build 59: the kick works; both biases march together; B crashed (ringbuf assert)

**Kick, verified in DLLOOP:** B 07:50:03 trim +47.24 against integral +51.35 (−4.1 ppm for one block
after a +2 µs bias step), 07:50:33 +46.89; A 07:49:33 +48.90 vs +56.26 after a larger step. Sign right
(bias up → slower → audio later).

**But both biases rise together** (A +28 → +32, B +17 → +38 in two minutes) because **both group deltas
read negative** — A −35…−4, B −1…−11; their sum −36, −9, −13, −15, −7, −6, −4 where two devices' deltas
must sum to zero. Each board sees the other as ~8 µs *later* than itself. Candidate mechanism: the
pairing uses the beacon's *receive* time (`phase_seen_us`) against my *sample* time; the peer's sample
can be up to ~1.6 s older than its receipt, and the shared mapping ramps at 2–3 ppm most of the time
(`Offset ramp +2.4…+3.2 ppm`), so an older phase reads later by ramp × age ≈ 3–5 µs — on both sides. A
common-mode march of the deadline (15 µs/min at gain 0.3) that ends at the ±500 µs cap. Fix: carry the
sample age in the beacon, pair on true sample instants, extrapolate by the ramp. Until then the sum of
deltas is the health check; a common march does not move the wire, but it will saturate align.

Wire, per minute, gain 0.3: +10.6, +11.1, +6.3, +4.9, (−13.6 during a fallback), +8.4, +4.1 — mean
closing but not at the predicted rate, consistent with the differential part of the steps being a
fraction of the common part.

**B crash 07:51:26:** `assert failed: prvSendItemDoneNoSplit ringbuf.c:374`, log rate ~5 lines/s at the
time (not the 38/s logger case). Backtrace decode pending the ELF. Then a 12 s server hole → bailout.

**B crash decoded** (build-59 ELF, `xtensa-esp32s3-elf-addr2line`): `xRingbufferSendComplete` ←
`TaskLogBuffer::send_message_thread_safe` ← `log_vprintf_non_main_thread_` ← `TsfSync::update_group_diagnostics_`
(tsf_sync.cpp:1050, the once-a-second "Render phase mine … delta" line) ← `service_tx_` (snap_net task).
Same class as the earlier logger crashes: ESPHome's non-main-thread log path into the TaskLogBuffer ring.
Build 60: that line is VERBOSE. Log rate was ~5 lines/s — this is not the 38/s overflow, it is the
thread-safe send itself.

**07:52:38 → server dead.** All three boards lost chunks; A's dead-session detector reconnected at +15 s,
B bailed out at 12 s late; both reconnected 07:53:15 and received nothing — snapserver's stream stopped
(source side). Build-59 grade cut short at 7 minutes.

### 2026-08-30 07:52–08:05 — both speakers wedged after a 40 s server outage; the network went with them

07:52:38 all three boards lost chunks. A: dead-session reconnect at +15 s; B: bailout. Both reconnected
07:53:15–17, `Stream started`, `State changed to PLAYING` — and then: the mixer (`Stopped
(bits=0x002002)` at 07:52:52) **never restarted**, I2S `written` frozen for the following 5 minutes, the
player printed nothing after its `PLAYER STALLED … phase=idle(record queue), ring=520704` line, and the
network task sat in `emit_pcm_`'s wait-for-ring-room loop (ring one chunk short of full) — no mDNS, no
ping, no API, OTA impossible; replug only. The observer (older build) recovered normally. The STALLED
line was cut by the 256-byte ceiling before `records=`, so whether the queue held records — the field
the code comment calls decisive — was not printed. Not resolved tonight: what the player is blocked
in (its mutex sections are all short; no "HELD BY SOMEONE ELSE" line). Build 61: `emit_pcm_` drops the
chunk after 2 s so the network task survives and the stall/dead-session paths can act; STALLED report
in two lines with records=, stream_active and iterations. The 'Render phase' log line (on B's 07:51
logger-ring crash stack) is VERBOSE. Reproduction is a ≥40 s server hole; the next one will name the
player's phase and the queue depth.

### 2026-08-30 08:09–08:37 — the wedge cleared itself (api.reboot_timeout), build 61 graded

Both boards came back by ESPHome's `api.reboot_timeout` (15 min without an API client: HA dropped them
at 07:54:57/07:55:37 → software resets 08:09:57/08:10:38, no panic). Build 61 flashed 08:14:04; locked
+25 s (B, own error) / +20 s (A, on group agreement).

**Align gain 0.3 with the kick, per-minute wire medians (µs):** −11, −6, −5, −7, −3, +0.7, −0.1 | +17 (event)
| +5.7, +3.3 | event | −24, −6, −23 (events) | −4, −0.3, −0.9, +5.9, +1.0, +0.5. Mean inside ±1 within ~6 min
of boot and again within ~4 min after each disturbance; MAD 0.6–2 µs in quiet minutes. No hunting.

**Common march persists:** A's bias −15 → +67 in 20 min while B's stayed +25…+50; A's delta read
negative in 17 of 20 cycles (−6…−18) with B's near zero or positive — the sum of deltas is still
negative on average. The pairing-age × mapping-ramp mechanism (07:42 entry) stands; at this rate A
reaches the +500 cap in ~2 h. Beacon sample-age fix next.

### 2026-08-30 08:40 — build 62: the beacon carries the render phase's sample age

`TsfPacket.render_phase_age_ms` appended (older senders' shorter packets get 0xFFFF and pair by
receipt as before); the receiver records the peer phase at `receipt − age` so the 300 ms pairing
window compares true sample instants. Prediction: A's and B's deltas sum to ~0 (they read −8 / +2 all
morning), the common bias march stops (A +80 µs in 20 min at gain 0.3), the wire mean unchanged (it
was never affected by the common part). Both speakers reflashed together; the observer keeps its
older beacon format and is handled by the short-packet path.

### 2026-08-30 08:44–08:53 — build 62 (beacon sample age): the deltas sum to zero; the march is gone

Per minute (wire median | A gd, bias | B gd, bias | sum): +7 | −1,−9 | −3,−3 | −4 · +5 | +1,−11 | +1,−6 | +2 ·
−4 | 0,−12 | −4,+4 | −4 · −8 | −4,−4 | +3,−2 | −1 · +2 | +3,−8 | +4,−8 | +7 · −4 | −2,−4 | 0,−9 | −2 · +8 |
−3,+1 | 0,−9 | −3 · +0.2 | −1,+2 | −1,−7 | −2 · +0.6 | +5,−7 | −1,−5 | +4. Delta sums −4…+7 (were −8…−15
every cycle before), both biases inside ±12 µs for ten minutes (A had walked +80 in the previous 20).
The wire mean is at 0 with ±8 µs per-minute wobble — the P-term meander, unchanged, as predicted.

### 2026-08-30 08:54–09:03 — build 62, second half: align chases a peer in transient; two timebase jolts

08:54 B TAGFAULT → reconnect. A's view of B: +1969, +1951, +2108 (rejected, > 500) then +89, +67, +47,
+19 as B stepped home — A walked its bias −6 → −75 toward where B *had been*; the wire swung −30…+25
and took four minutes to settle. 09:02:46 both boards fast-spliced +2.3 ms at the same instant (a
mapping step; both re-opened, both closed within 15 s). **Build 63:** a board whose resync window is
open or which is not converged keeps measuring its phase for its own gate but broadcasts
RENDER_PHASE_UNKNOWN, so peers hold still while it steps home. Prediction: on the next single-board
reconnect the other board's bias does not move (RALIGN deltas "unknown"/held), and the wire returns to
0 as soon as the resyncing board's own steps land (~10 s) instead of after a four-minute align
excursion.

### 2026-08-30 09:07–09:10 — build 63 regressed the boot: nobody broadcast a phase

Gating the beacon on "converged and window closed" meant that after a double boot neither board
offered a phase: no group delta → no group-agreement unmute, no gate evidence, align idle → both
crawled in on the PI, wire +528 µs for a minute (watchdog 09:08:18). The phase is untrustworthy only
for the ring's travel time after a position step or a hard resync (~3.5 s, builds 51–55). **Build 64:**
`in_transient` = a step or hard resync within the last 4 s (`kp_event_us`, `resync_step_at_us`);
converged/window play no part. Prediction: boot behaves as 62 (lock ~+20–25 s, wire correlated ~+14 s),
and a single-board reconnect still leaves the peer's bias untouched during the ~4 s of steps.

### 2026-08-30 09:09–09:37 — build 64 (4-s transient rule): boot as 62, quiet minutes at 0 ± 1 µs

Boot: lock +15 s (A) / +25 s (B), wire correlated +14 s. Quiet stretch 09:16–09:24, per-minute wire medians:
+2.9, +3.7, +5.7, −2.6, +1.2, +0.5, +1.3, **+0.1, +0.6**, MAD 0.3–1.9 µs; delta sums −5…+2; biases A +72…+84,
B +62…+81 (common drift ~+10 in 10 min, from +80 in 20 before the sample-age fix). 09:33 B reconnect: A's
view of B +1848 (rejected), A's bias 63 → 71 → 65 — it did **not** chase (build 62: −6 → −75). Recovery to
±3 within two minutes of each event (09:25 fallback, 09:31–33 reconnect).

Steady-state score at this point: mean pinned at 0 by align (gain 0.3, deadband 1, step 20, kick,
sample-age pairing, transient-quiet beacons); the residual is the ±3–8 µs per-minute meander from
P-term noise plus the timebase's own events. Build 65 makes the runtime align values the compiled
defaults and starts a 45-min untouched grade.

### 2026-08-30 09:43 — build 65, a server storm 4 min after boot, and a 10 ms ledger split on A

09:43:24 both boards took a storm (A 30 hard resyncs, B 2). Window steps: B `+38044 → +24910 → +11749`
(576-frame steps, one per block) closed at 09:43:39; A `+3252 → −14118 → −862 → −157` closed 09:43:52.
Wire: −1148 (median, during) → −95 → −41 → −9 → +6 over the following 60 s — the last 100 µs is
align's 0.3/cycle (95 → 66 → 46 → 32 → 23), i.e. ~40 s; the gate cannot see a residual that small.
Afterwards A carries `RECON drift=10000` **constant** and `SHADOW err_tag −100 / err_live +9900`: the
ledger believes 10 ms more audio is queued than is measured — one DMA block — the accounting-split
family again, this time steady rather than flipping. Audio is on the tags and is fine (wire ±10); it
becomes a fault only if the tags go stale. Recorded; not acted on during the 45-min grade.

### 2026-08-30 09:52 — TAGFAULT the wrong way round, then a minute of no action: build 66 re-trusts tags on agreement

A's steady +10 ms ledger split (after the 09:43 576-frame insert) tripped TAGFAULT at 09:52:10 — the
tags were right (+358), the ledger wrong (+10384). After the reconnect tags and ledger agreed within
40 µs at +15 ms, yet the 180-s distrust kept coarse decisions on the ledger, whose 52 ms flip showed
+15, +5, −7 ms in consecutive reports: `corrected −0/+0` for over a minute, wire +3.5 ms. Build 66:
three consecutive blocks of tag/ledger agreement inside 1 ms end the distrust (`Tags re-trusted`).
The timer stays as the fallback for genuine tag faults, where the two never re-agree.

### 2026-08-30 09:55–10:13 — build 66: re-trust in 13 s; quiet minutes at 0 ± 2 µs; a slower common march remains

Boot lock +16 s (A, own error) / +19 s (B, on group agreement). Quiet minutes 09:58–10:06: −1.6, −0.4,
+0.1, −0.9, (+7.9 event), +3.0, −1.2, +0.7, +3.1, +0.6 µs; MAD 0.6–2.3. 10:07:01 server hole → A
TAGFAULT 10:07:30 → **`Tags re-trusted` 10:07:43** (13 s, was 180); wire −91 → +3 by 10:09. B's view of A
read +342 for two minutes while A was off by ms without stepping (a reconnect leaves a board out of
band longer than the 4-s transient rule covers) — B's bias moved only +2 in that time (the 500 µs reject
and the deadband/gain cap held it), acceptable.

**Common march, slower:** A +46 → +94, B +72 → +115 in 15 min (~3 µs/min; was 8 before the sample-age
fix). Delta sums in quiet minutes −4…−5 with B at 0: a residual ~5 µs asymmetry in A's view. Left as is
it saturates the ±500 cap in ~2.5 h. Since a common bias shifts every deadline equally and carries no
information, the clean fix is to exchange biases in the beacon and re-centre: each board subtracts
the group-mean bias each cycle, keeping only the differential. Build 67.

### 2026-08-30 10:20 — build 67: biases in the beacon, group mean subtracted

`TsfPacket.render_bias_us` (INT32_MIN for older senders); each device publishes its bias every align
cycle and subtracts the group mean, at most 2 µs per cycle (the cycles are unsynchronised; this bounds
the differential a re-centre can open), delivered by the kick. Prediction: both biases stay near 0
indefinitely (were marching +3 µs/min together), the wire unchanged (the common part never moved it),
and the ±500 cap never approaches. Flashes after the build-66 grade completes.

**Build 66, 10:12–10:28:** −1.2, (−11.5 spike: A's view of B +1036 for one cycle, no event logged), +3.5,
−2.5, −2.2, +1.9, −2.4, −1.1, +1.8, −1.7, +1.0, +0.3, +3.5, −4.2, +0.2 µs; MAD 0.3–2.3. Delta sums −9…+6.
Biases A +90 → +134, B +114 → +141: the common march at ~3 µs/min, which build 67 removes.

### 2026-08-30 10:29–10:53 — build 67: re-centring holds the biases near 0; two more transients align must ignore

Biases stayed inside A −8…+23 / B −38…+12 through the quiet minutes (build 66: +90 → +134 / +114 → +141
over the same span) — the common march is gone. Quiet wire 10:32–10:37: +14, +9, +6, +5, +4 (closing
from a +22 boot offset more slowly than the deltas imply; noted). Events: 10:38 server hole → both
TAGFAULT → both `Tags re-trusted` by 10:40 → wire from +83 to ±5 in ~2.5 min. Two things align still
did wrong: (1) during A's own TAGFAULT recovery (tags distrusted, err_tag tens of ms) A kept aligning —
`st.converged` never cleared — and walked its bias −73 → −103 against a delta measured while it was
itself off; (2) 10:46–47 a B deadline fallback/re-engage (a real phase jump, not a step) is not covered
by the 4-s transient rule: A saw −72, B saw +203, B stepped +100 in a minute, wire +120. **Build 68:**
a `phase_transient_until_us` set by hard resyncs, window steps, AND deadline fallback/re-engage;
align runs only while own |err_tag| is inside the unmute band.

### 2026-08-30 10:56–11:01 — 28 % of the differential bias reaches the wire

Per align cycle, differential bias applied (ΔbiasA − ΔbiasB) against wire movement over the following
10 s, 29 cycles: slope **0.28**, Σbias −88 µs, Σwire −26 µs, sign right. The kick is delivered (A's
measured rate +5.0 ppm for the ~1 s after each −5 µs step; +2.3, +2.4 ppm over two windows after
another) and the deltas track the wire (gap ≈ 2 × gd), so the audio moves ~5 µs per kick and then
~70 % of it comes back — by what, unresolved by reading (P-term at 5 µs is 0.04 ppm; the fast/coarse
paths are far above; the deadline change reaches the tags one ring-travel later but that is a
transient, not a reversal). Also: half the kicks are invisible in DLLOOP only because it logs every
other block. **Experiment 11:08:** align frozen, A's bias zeroed via `align_max_us 0` (a −60 µs deadline
step, no kick — the PI alone delivers it at τ = 120 s), wire per 30 s for four minutes; expected −60 on
the wire if the deadline→wire gain is 1.0. Then cap restored.

### 2026-08-30 11:08–11:16 — the single-board step was contaminated; build 68 first quarter-hour

A's bias was +18 (not 60) when zeroed at 11:09:08 (`SERVOPARAM align_max_us=0`). Wire −15 → −20 → −23,
then 11:10:10 a common timebase step (A re-opened on +400 and took a −7945 µs tag step; B fast-spliced
−3.4 ms) — the wire swung +36 and drifted back to −31 by 11:14. Net −16 over five minutes against an
expected −18, but with that event in the middle it is not a measurement of the deadline→wire gain.
After restore, align brought the wire from −17 to −2 within 90 s with biases at ±4. Repeat in a quiet
hour with a larger step (set `align_max_us` to a small cap to *create* a known bias first).

Build 68 boot: lock +21/+25 s, both on group agreement. 11:05–11:07 wire −16, −15, −12 while the two
biases rose together (A −27 → −3, B +28 → +45): a residual common march faster than the 2 µs/cycle
re-centre, and B's bias moving against its own delta for two minutes (B gd +11 → steps −3, bias +17):
not understood yet. 11:16 wire −3.7 MAD 0.5, biases +4/−4.

### 2026-08-30 11:06 — the accumulator carried clamped excess: build 69

B, 11:06:39: one transient delta of −206 (A's phase, unflagged) → 0.3 × 206 = 62 → stepped the 20 µs cap
and *kept 42 in the fractional accumulator* (it subtracted the clamped step, not the integer part) →
+20, +20 on the next two cycles against deltas of +1 and +14 (bias +27 → +45 → +52). Every "bias moving
against its own delta" since build 57 and part of the 28 % delivery figure trace to this: biases were
moving on stale remainders while the deltas — and the wire — said otherwise. Build 69 discards the
integer part after clamping; the fraction alone carries over. Prediction: bias trails follow the
deltas cycle by cycle; the wire-vs-bias slope rises toward 1.

### 2026-08-30 11:19–12:02 — build 69 (accumulator fix): biases track the deltas; 40 min at |wire| ≲ 5 µs

Boot lock +23 s (A, group agreement) / +21 s (B). Biases now follow the deltas cycle by cycle and stay
inside ±18 the whole 40 min (A +18 → +1 as its delta went +4 → 0; B the mirror). Per-minute wire
medians: +7, +8, +8, +5, −1, +4, +1, +6, +2, −2, −0, −4, −5, +0, +6 | −7, −10, −5, +4, +2, −8, +20, +9,
+3, −1, +1, +4, +9 | −5, −8, −1, −2, +15, +10, +10, −1, +1, −7, −8, −3 µs — the ±8…20 excursions all
coincide with event counts (server holes at 11:36, 11:52, 11:55–57, 12:02) or a transient delta (11:44
A −30 / B +26 for one cycle). Quiet-minute MAD 0.4–1.6 µs. Delta sums ±6 except during those.

**Build 69, 11:22–12:02, rival-gated:** n = 11293, median **+0.9 µs**, robust sd 6.1, 56 % of samples inside
±5 µs, 84 % inside ±10, 96 % inside ±20 — with seven server holes inside the window. Structure
function: 1 s 0.24 · 10 s 2.1 · 30 s 4.7 · 60 s 6.7 · 120 s 10.3 µs (build 30, a quiet window: 0.24 /
1.6 / 3.5 / 5.3 / 8.1). `wire-sf.py` is not rival-gated and reported robust sd 138 for the same span —
the event rows; graded with the gated computation above instead.

**Build 69, 12:03–12:07:** +40 (no event logged), +1, −5, +12, −24. At 12:05 A's view of B read −158
while B logged two events (fast splice / fallback) and A stepped its bias −26 → −42 — a **fast splice**
is a transient the beacon rule does not yet cover (it is neither a window step nor a hard resync nor a
deadline change: 1 frame per chunk for seconds). Build 70: engaging the fast splice sets the phase
transient for its duration plus the horizon.

### 2026-08-30 12:10–12:57 — build 70 (fast splice marks the transient): neither board chases a faulting peer

Boot lock +28/+22 s. B's 12:16 TAGFAULT: A read +2492 (rejected), A's bias +8 → +9. A's 12:22 fault: B read
+1050 for three minutes, bias −17 → −4 (re-centre only). B's 12:49 fault: A −4 → −3; then B read A at
−15.4 ms for two minutes (A's own recovery), rejected, bias +10 → +21. Wire back inside ±5 within
~2 min of each. Quiet minutes 12:26–12:48: −4, −2, +3, +1, +0, (+8), −0, +3, (+13, +9, +13), +7, +5, +1,
+1, (+22), +9, +2, +7, +7, +3, +9, +2 — the bracketed excursions ride asymmetric delta readings (sums
−17…−21) with no logged event; still unexplained, 1–2 min each.

Rival-gated grade 12:12–12:57 (six holes/faults inside): median +2.5 µs, robust sd 9.7, 40/56/85 % inside
5/10/20 µs; SF 1 s 0.29 · 10 s 2.3 · 30 s 6.3 · 60 s 10.6 · 120 s 12.6. Build 69's window (seven holes):
median +0.9, sd 6.1, 56/84/96 %. `wire-sf.py` rewritten (its pairing produced impossible values).

### 2026-08-30 13:10 — build 71: bench hooks for a clean deadline step; PHASEIN on the speakers

`servo_param align_bias_us N` sets the render bias to N (deadline only; the PI delivers it at τ);
`align_bias_kick_us N` also queues the change as a kick. Protocol: `align_apply 0` on both, 60 s
baseline, A `align_bias_us 40`, wire every 30 s for 5 min (expect −40 on the wire if the deadline→wire
gain is 1, with the PI's τ = 120 s shape), then `align_bias_kick_us 0` (−40 kicked; expect the wire back
within ~1 s plus one ring travel), restore. Event counts printed beside every row so a timebase event
cannot pass as a result again. PHASEIN (both boards' raw phases, ages, group delta) is logged on the
speakers once per align cycle for the unexplained excursions.

### 2026-08-30 13:14–13:25 — the clean step test: deadline→wire gain ≈ 0.8–1.0 at τ ≈ 120 s; the kick path 1.0 in 30 s

Align frozen on both (wire held at +55, MAD 0.2). A `align_bias_us +40` (deadline only): wire +55 → +60
(30 s) → +67.5 (60 s) → +69 → +71 → +72 → +79.5 → +84 → +88 → +86 → **+86 (300 s)**: +31 of +40, the PI's
first-order shape (40 × (1 − e^(−t/120)) predicts +15.7 at 60 s, +37 at 300 s; measured +12.4 and +31 with
two small events inside). A `align_bias_kick_us 0` (−40 with the kick): +86 → **+46 within 30 s** and flat
for three minutes — the kick delivers the whole step at once. Restore: align took the standing +46 to
+0.8 in 90 s. Sign as the mapping says (A's bias up → wire up).

So both actuator paths are whole; the "28 %" of 10:56 was the accumulator carrying clamped excess (fixed
in 69) plus a 10-s window on a τ = 120 s path. Open question left from this run: why the wire sat at +55
when align was frozen five minutes after boot — the first minutes after a lock deserve a look with the
new PHASEIN lines.

### 2026-08-30 13:30 — build 72: three mechanisms from one afternoon's instruments

1. **Stairs** (operator's plot 13:24, +46 → 0 in 5–13 µs steps): the 10 ppm kick lands each align step
   inside ~1 s, then nothing for a cycle. Kick capped at 1.5 ppm — a 15 µs step becomes a 10-s ramp;
   still 10× faster than the PI's τ. Same convergence time, no stairs.
2. **Each board reads the other ~18 µs later** (PHASEIN on the speakers: A `4D74 d=+16`, B `85E8
   d=+20`, ages 0.8–1.5 s): phase = tsf − server drifts at the mapping rate (`map +41 ppm`), and the
   peer's sample was not extrapolated to my instant. Now `peer + drift × (mine_at − peer_at)`. The
   re-centring had been absorbing this as common mode; every delta was inflated by it.
3. **RECON drift = −52 245 µs exactly = 2 × 1152 frames, on 40 % of reports, always with xfer=50000.**
   The mixer has copied a slice the ledger adds only when the blocking `on_audio_write` returns; the
   pipeline is kept full by backpressure so writes block routinely and the mixer's snapshot lands inside
   one. The ledger was right; the comparison was made at the wrong instant. Snapshots inside a write
   window are now "not comparable". This "drift" armed the split repair, held the unmute anchor (`anchor
   reads −49343`) and was the ledger side of every TAGFAULT disagreement. Prediction: RECON |drift| > 5 ms
   on ~0 % of reports; far fewer TAGFAULTs after storms; unmute never waits on the anchor.

Also from the step test's baseline: 13:10–13:14 the wire was noisy (MAD 20–38) with single-cycle delta
readings of −62 (A) and +36/+52 (B) five minutes after boot — the PHASEIN lines around them are the
first raw look at that class.

### 2026-08-30 13:35 — the deadline-source flap is a first-order disturbance in its own right

Census (today, per hour): `deadline on local fallback` A 2–24, B 1–32; consensus spreads > 1 ms 20–100.
Each fallback flips the deadline by the shared-vs-local difference (~100 µs), the coarse path steps the
audio to it (`RSTEP src=ledger +124, +155, −157` at 13:11–13:14), the PI holds and then re-engages with
its P-term wherever it was (`P −9.47`), and the peer — correctly — sees a 100 µs move and aligns to it.
The 13:10–13:14 baseline noise (MAD 20–38 µs) and B's bias walk −2 → −41 were all this. The mapping
itself expires only after 5 s; these are single-call evaluation failures (a bad TSF sandwich or an
age-clamp rejection) that last one chunk. Fix: hold the last good shared offset, extrapolated by the
mapping drift, for a short grace period before declaring a fallback — a 1-s blip must not move the
deadline. Build 73.

**Build 73:** `shared_server_offset_us` holds the filtered offset — carried forward by the measured
crystal ratio, as the feed-forward already does — for up to 3 s when the TSF sample fails (NO_TSF); an
AGE_CLAMP (TSF reset) still falls back. Prediction: `deadline on local fallback` drops from 2–32/h to
~0 except at real mapping losses; the ~100 µs one-board steps and P-term holds that followed each flip
disappear from the wire; the peer's bias no longer walks after them.

### 2026-08-30 13:36–14:23 — build 73 graded: zero deadline fallbacks; the longest quiet stretch yet

**Fallbacks 0/0 in 47 minutes** (2–32/h before); the hold absorbed 11 (A) / 74 (B) TSF-sample blips.
After the boot transient (13:38–13:43, B's serial replug inside it), 40 minutes with zero
TAGFAULTs/reconnects/splices: per-minute medians ±5 µs with occasional ±8…±18 (14:12–13, 14:07), MAD
0.5–3.5, delta sums ±17, biases inside ±30 and re-centred. Open: the RECON census still shows |drift| >
5 ms on ~32 % of report lines — determine whether the line prints raw values on snapshots the
comparison (build 72) rightly refused, i.e. whether this is the instrument printing what the logic no
longer consumes.

**RECON census resolved:** the line is an unconditional diagnostic that recomputes drift from raw terms
even on snapshots the build-72 logic refuses. Every *consumer* is silent since the 73 boot — Unmute
held 0, anchor waits 0, repairs 0, drift-excess 0, SHADOW splits ≥10 ms 0, on both boards — so the
in-write exclusion works; only the print lies. Gate the diagnostic on `fill_comparable` in the next
build (74, batched). A 90-min untouched grade of 73 started 14:26.

### 2026-08-30 14:26 / 14:40 — with the ledger comparison fixed, the post-storm outlier is now the TAGS

B's holes at 14:26 and 14:40 both ran the same sequence: starvation → hard resync → three tag-driven
corrections leave err_tag at −45.7 / −24.5 ms while the ledger reads −0.8 ms → TAGFAULT → reconnect →
re-trust. This morning the ledger was the liar (the in-write snapshot); since build 72 the ledger reads
sane through these and the tags claim tens of ms. Next design item: what the tag stream measures in the
first seconds after a starvation refill (anchor from a pre-storm chunk? DMA tags spanning the silence?)
— the reconnect recovers in ~15 s, but three per afternoon is the remaining event class. TODO'd.

### 2026-08-30 14:25–15:55 — build 73, 90 minutes untouched through the worst server hour of the day

n = 22 177, median **+1.9 µs**, robust sd 11.5, 59 % inside ±10 µs, 77 % inside ±20 — with 30 starvations,
5 TAGFAULTs (all B, all the post-refill tag outlier), 7 reconnects, 1 splice each and 80 window re-opens
inside the window. **Deadline fallbacks: 0** (23 held blips). Biases ended at 0 / +1 µs. Every fault
recovered by reconnect + re-trust with the peer holding still; the quiet stretches sat at ±5 µs. The
remaining event class is the server holes themselves plus the post-refill tag outlier (TODO).

### 2026-08-30 16:00 — the post-refill tag outlier, mechanism and fix (build 75)

`err_tag = render − (anchor_deadline + (frame_server − anchor_server))`, and the anchor refreshes every
chunk. After a hard resync the deadline steps by the resync size, but the audio rendering for the next
ring + pipeline (~2–3.5 s) was pushed against the *old* deadline — its tags, read against the new
anchor, report the whole displacement. The hard-resync tag blank was `blank_ms` (500 ms), so the tags
re-entered mid-travel: −45.7 / −24.5 / +70.6 / +54.5 ms with the ledger under 1 ms, unmovable by
corrections, → TAGFAULT → reconnect, five times on B this afternoon. Build 75 blanks the tags for
`PHASE_TRANSIENT_US` (the measured travel horizon) at every kp event. Prediction: post-hole recovery
without any TAGFAULT — hard resync, quiet tags for ~4 s, tags return agreeing with the ledger.

### 2026-08-30 16:08 — B's second logger-ring crash; the Crystal line moves to the player task

Same assert (`prvSendItemDoneNoSplit ringbuf.c:374`), same class: a once-a-second ESP_LOGD from the
snap_net task (`TsfSync::update_group_diagnostics_` line 1060, the Crystal line) inside the ESPHome
logger's thread-safe path. Demoted to VERBOSE there; the client now logs the identical format from the
player task at align cadence, so the analyzer's `crystal_a/b_ppm` columns keep working. The snap_net
task now emits no periodic DEBUG lines; if a third crash arrives, it names the next one. Rides into
build 75.

### 2026-08-30 15:56–16:38 — build 74 graded; the cmp= field finds the exclusion's残 hole

Grade: quiet minutes ±5 µs (16:19–16:35 mostly MAD ≤ 2), events at 16:08 (B's logger crash) and 16:10.
The `cmp=` census: cmp=0 lines are exactly the old two-chunk artefact (xfer=50000, age 54–61 ms) —
refused as designed. But 97 of ~560 **cmp=1** lines still carry −48/−51 ms: the snapshot is itself
50–60 ms old, so it can fall inside a write window that had *ended* before the comparison ran, and the
single live begin/end pair misses it. Build 76: a ring of the last 8 completed write windows; a
snapshot inside any of them is not comparable. (B's 16:08 crash: the Crystal line — relocated to the
player task, see above; rides into 75.)

### 2026-08-30 16:50 — build 75's tag blank confirmed against a built-in control

A real server hole hit B (build 75) and the observer (old build, no fix) at 16:50:24 simultaneously.
The observer TAGFAULTed at 16:50:38 with the classic signature (err_tag −17.7 ms, ledger −0.7). B rode
it clean: tags and ledger agreed within 30–75 µs the whole way (+1.2 ms decaying to +77 µs, no fault,
no reconnect). Same disturbance, same moment, one variable.

### 2026-08-30 16:59 — the outlier's second and third doors: the render-gap blank and the rebaseline

A's injection recovery TAGFAULTed (+11.8 ms tags, +0.9 ledger) with the hard-resync blank already at
the horizon — the sequence was: three hard resyncs (blank set ✓, RSKIP showed blank=3242 ms), then
`Pipeline drained; re-baselining` at 16:59:00 **after** the blank was armed: the reseed moved the
prediction again, and the render-gap blank in the speaker callback still used 500 ms. SHADOW five
seconds later: err_tag +16.6 ms vs ledger +6.6. All three doors now use the same travel horizon
(hard resync, render gap, rebaseline) — committed as `b26c7a4`, rides into the next flash (with the
write-window ring). One mechanism, three entrances; the observer-vs-B control at 16:50 stands for the
door that was already closed.

### 2026-08-30 16:43–17:18 — build 75 graded

Injections: 34 / 11 / 32 / 24 s to <100 µs held 5 s, **zero TAGFAULTs during them** (prediction held; A's
one fault at 16:59 was the gap/rebaseline door, closed in `b26c7a4`). 30-min grade with the real 16:50
hole inside: median +1.6 µs, robust sd 9.0, 75 % inside ±10 µs. The 32–34 s recoveries are the tag-blank
horizon itself now in the path (tags quiet ~4 s → first step later than before); acceptable against the
faults it removes — revisit only if <15 s matters more than fault-free holes.

### 2026-08-30 17:35 — build 77: computed travel horizon; the <10 s resync project opens

Operator: take option 2, then drive post-hole convergence under 10 s. Build 77 replaces the fixed 4 s
tag blank with `travel_horizon_us_()` = ring + pipeline + two blocks (live mirrors, clamp 1–5 s;
~2.7 s at the 2 s server buffer). Its injection run prints per-event timelines (hard resync, each
RSTEP, window close, relative to the injection) so the 24–34 s budget decomposes into named intervals.
Known structure of the budget: hole 0.3 s → hard resync ~+2 s → ledger first step (now trustworthy,
gain 1.0) → tags return at the horizon → one verify step → close. If the ledger's first step lands
inside 100 µs — the ledger agrees with the tags to ~50 µs now — the target is reachable by trimming
the resync detection (+2 s) and the close timer's place in the metric.

### 2026-08-30 17:41–17:45 — build 77 timelines: the budget named, the variance is the first step

Injections 20 / 15 / **10** / 32 s, zero TAGFAULTs (a real group hole at 17:41:51 rode clean on both
speakers while the observer faulted — second controlled confirmation). The timeline when it works
(inj 3): hard resync +1.5 s → ledger step +2.5 → one tag step +7.1 → wire <100 µs at +10 s incl. the
5-s hold. The variance: the ledger's first step landed on mid-refill readings that bounce tens of ms
chunk-to-chunk (+41.8/+27.7/+36.4/+42.2 in 300 ms during the 17:41:51 burst), leaving 990 µs / 2.7 ms /
14 ms residuals that cost extra 2.7-s rounds and, twice, a −300 µs tail below the gate's floor.
**Build 78:** the ledger's first window step requires two consecutive readings within 20 % (500 µs
floor). Prediction: every injection ~10–13 s; tails only where the gate's gd floor leaves them.
cmp=1 big-drift census reads 49 % but the window was all bursts (genuine ledger displacement is real
and comparable during refill); the decisive check — drift consumers — reads zero on both boards.

### 2026-08-30 18:0x — build 78 graded, build 79: frame-exact pend, block-cadence decisions

78's six injections: 11 / 30 / 11 / 14 / 24 / 6 s. The stability gate landed every ledger first step
at +1.9 s, but the logs show the remaining cost is structural: pend read +0 at every decision (its
travel estimate — instantaneous ring+pipe+block — under-reads while the ring is drained post-hole),
so the next tag round re-stepped each ledger step in full (17:54:10 +2729 ledger → 17:54:13 +2766
tag), and every extra round cost a 3.2 s blank plus the landing. The landing instant is exactly
knowable: the drop is consumed where pushed_frames_total_ advances, tags are observed where
played_frames_total_ advances — landed when played passes the recorded index (+1 block for the tag
average, 6 s expiry against rebaselines). With pend frame-exact, the window's act blank is
unnecessary: one step per block (sameblk), err_pre − pend IS the coming residual, sign guard zeroes
any target the subtraction inverts. Judge path keeps the full horizon.
**Prediction:** tag residual steps fire one block (~0.65 s) after the ledger step instead of 3.4 s;
pend= prints nonzero at those decisions; all six injections ≤ ~11 s (2 s detect + step + land ~3 s +
5 s hold), no double-steps (no equal-size ledger/tag pairs), TAGFAULTs 0.

### 2026-08-30 18:14–18:21 — build 79 graded: 10/10/10 s clean, the residue is a deadline tail

Six injections 50 / 34 / 30 / 10 / 10 / 10 s, TAGFAULTs 0. The machinery predicted held: pend printed
in flight (+2721), the sign guard zeroed inverted targets, three runs hit the 10 s goal exactly
(which includes the 5 s hold — convergence ~5 s). Two residues:
1. One block of pend margin is marginal post-hole (pipe drained, landing ~12 ms out): +1429 stepped,
   0.64 s later pend=+0, +992 re-stepped → −1163 overshoot. Fixed: two blocks (build 54's lesson).
2. The slow runs are all post-window tails: wire and gd read −150..−450 µs while err_tag AND the
   ledger read ~0 (SHADOW diff −12..−56), and the observer's PHASEIN sees only ~20 % of the wire
   error in the beaconed phase — every on-device signal shares the deadline, so the deadline itself
   moved. The per-board deadline term is the shared-offset EWMA in tsf_sync; nothing on that path
   logs below the 2000 µs snap. Build 80 adds OFFDBG (rawgap/dflt/sandwich/trust, 0.5 Hz) before any
   fix — suspect the instrument-free path, do not fix blind.
**Prediction (hypothesis):** during an injection OFFDBG shows dflt walking ~200-400 µs on the
injected board (burst-biased samples or feed-forward across the stall) and relaxing over tens of
seconds, matching the wire tail. If dflt stays flat, the hypothesis is dead and the tail source
must be sought between the deadline and the DAC instead.

### 2026-08-30 18:34–18:38 — build 80 graded 30/10/10/10; OFFDBG retraction

**Retraction:** the shared-offset-filter hypothesis is dead. OFFDBG shows dflt dominated by the
genuine server-vs-local clock ramp (~+50–75 µs/s) and STATISTICALLY IDENTICAL between the one 30 s
tail run and the clean run on the same board (74 vs 68 µs/s) — the deadline filter did not move the
tail. Build 79's "deadline moved" reading conflated two tail species. What build 80's RSTEP streams
show: the tail is a sub-arm residual (±100–250 µs). In inj 1 err_tag, gd and the wire all AGREE
(+118→+239 / +49→+62 / +240) — a real, slowly-growing lateness below the 100 µs step arm, left to
the tracking-gain PI (τ 120 s) and align (10 s cadence), hence 20–40 s. The <10 s answer for the
end-game is acquisition gain inside the window with bumpless handoff, not more position steps.

### 2026-08-30 18:42–18:46 — runtime A/B: knee 25 / tau_min 5 kills the tails; goal met

knee_us default was 1e6 — the error-proportional gain boost was effectively OFF, so every sub-arm
residual decayed at flat τ=120 s: that was the whole 30–50 s tail. Set knee_us 25 / tau_min_s 5 live
on both boards (symmetric in the error, so wire-safe per the 08-29 per-board-boost lesson; the user
has ruled a few ms of server-timeline desync tolerable). Four injections: graded 14/10/13/14 s —
converged to <100 µs at +5…+9 s (grade includes the 5 s hold). kp visibly tracks |e| (0.008→0.048;
0.200 at the 5 s floor absorbing a ±1 ms straddle in inj 4, where pend=+929 also held correctly with
the two-block margin). Baked as compiled defaults for build 81.
Open: kp 0.2 × ~950 µs gave a brief +241 ppm trim transient (inaudible, settled in 2 blocks) — watch
the quiet-hour wire sd in the next soak for any cost of the boost at wander amplitudes (e≈80 µs →
τ_eff 37 s, common-mode).

### 2026-08-30 18:53–19:02 — build 81 confirmation interrupted by the ledger-step storm; fix for 82

Confirmation ran into real server holes overlapping the injections and exposed a systematic failure,
on A then reproduced on B: during a late refill burst the per-chunk scheduler hard-resyncs on ~96 of
128 chunks, EVERY hard resync re-arms the window's one-ledger-step latch, each re-armed ledger step
decides on median_err_us (a median that has not seen the previous steps), and the ledger path has no
in-flight subtraction (pend was gated coarse_on_tags). A: seven +576-frame steps in 2.5 s against a
flat +28 ms reading → −47 ms overshoot; the mid-storm rebaseline reseeded the accounting to ~0; the
tags then carried the truth alone (SHADOW diff +40651) and TAGFAULT sided with the ledger → 60 s
max-amplitude tug of war (tag inserts −576, scheduler hard-drops them back) ended only by the
reconnect. Grades: 19 / 54 / 10 / DNF / DNF / DNF, TAGFAULT A 1.
**Build 82:** every window decision subtracts the steps in flight — ledger source included — and the
sign guard compares against the pre-subtraction target. Step 2 of the storm becomes 28016 − 28224 =
−208 → wait. Prediction: a hole-overlapped injection shows at most ONE ledger step per landing, no
tag/ledger split >5 ms, no TAGFAULT, and the watchdog's B-SPLIT/ERR5MS events stop appearing on
natural holes.

### 2026-08-30 19:05 — build 82 unstable at boot: wait, do not compensate (build 83)

82's err−pend arithmetic diverged in the boot window: with fast landings smeared across the block
average, the binary landed/in-flight label mislabels a just-visible step and the subtraction
manufactures a correction from a step that already landed — tag steps −2715 → +5307 → −8178 →
+10449, ×1.5 per round, into a 16 ms split (A, 19:05–19:06). Build 52's lesson relearned: a block
straddling a landing cannot be fixed by arithmetic. **Build 83:** serial step-and-verify — while any
window step is in flight (frame-exact, 2-block margin, 6 s expiry), every coarse decision waits
(target zeroed, pend printed). One correction travels at a time; the next decision reads audio that
wholly post-dates it. This also serializes the ledger storm case by construction. Prediction:
injections ~10–13 s (one landing wait per round), no growing oscillations, no splits, TAGFAULTs 0.

### 2026-08-30 19:10–19:25 — build 83 under the jumbo-hole regime: the split is the invariant

83's six injections all DNF'd — not because of the serial rule (pend held correctly, e.g. RSTEP
err=+0 pend=−3560 waits) but because the bench regime changed at ~18:53: server bursts of 964 ms /
2.5 s / 6.3 s / 5.4 s lateness (observer bailouts 17:50, 18:57, 19:02, 19:19; starves ramping 4/30min
→ 7/30min at 19:0x). Every jumbo refill now plants a tag/ledger split (−5…−47 ms: tags+wire see
early audio, ledger reads ~0) and the system enters a stable ~10.4 s limit cycle: window tag steps
insert on err_tag (−3.6k/−10.7k alternating), an unattributed per-chunk actor drops −256…−544
frames/report on the ledger signal (0 hard resyncs — steer is gated off while tags live and bounded
1/chunk; suspicion is the fast-splice fallback in TAGFAULT gaps), and the two orbit around the
constant split. TAGFAULT→reconnect used to heal it; tonight the reconnect's own refill burst plants
the next split — A looped through 3 TAGFAULTs and was still split at −10 ms (B settled clean at
+35 µs). A rebooted to break the loop.
**Standing decisions:** stop injecting (the operator is now the dominant disturbance); leave 83
soaking. **Next builds (fresh eyes):** (1) actor-tagged corrected counters (split the Sync −X/+Y by
source) — the drop actor must be named, not guessed; (2) revisit re-arming the accounting-split
repair when the tag/ledger disagreement is large and persistent while tags are live (b291c42
disarmed it wholesale; the split, not the tug, is the invariant to remove); (3) the server-side
jumbo holes are the user's lever (snapserver buffer 2000→4000, or the Pi host).

### 2026-08-30 ~20:30 — build 84: 50 Hz phase exchange, averaged group delta (SHADOW)

Toward <1 µs (user-directed): the live gd pairs one phase sample per block per side (~1 pairing/s,
~10 µs-class noise each side) — the floor under align and the sub-arm gate. Raising the beacon rate
alone adds nothing (a re-sent sample creates no new sample instants), so the SAMPLE rate moves:
phases measured per tagged chunk (~94 Hz), shipped every 20 ms in no_mapping=1 packets the receive
path already parses (multicast only — touching the unicast roster from the player-side task would
race set_peers). Each peer sample pairs with the nearest own sample (≤60 ms; own ring at chunk
cadence, guarded by mapping_mutex_), rolls into a 1 s windowed mean, published as
render_group_delta_avg_us() and logged as `GDAVG avg= n= live=`. Nothing consumes it (shadow).
**Prediction:** n≈40–50 pairs/s (some multicast loss); GDAVG's second-to-second scatter well under
the live gd's (if pairs were independent, ~2 µs vs ~10; correlation will keep it above the √n
line — the shadow measures how far). If GDAVG tracks the wire's slow differential with µs-class
noise, it becomes align's input next; if it inherits the live gd's bias pathologies (the tail-era
3–5× under-read), that under-read is upstream of pairing and the investigation moves there.

### 2026-08-30 20:34–20:38 — build 85: unicast phase reports; the 5× under-read isolated

Unicast roster mirror fixed delivery: GDAVG n=1–2 → 15–28 pairs/s, and avg tracks live within
noise (avg −114/−195 vs live −133/−196) — the averaged channel works. It immediately sharpened the
open question: during the post-boot settle the rival-gated wire read a REAL −1620 µs while BOTH
delta estimators read ~±150 µs and each board's err_tag sat at +330/+290 (only 40 µs apart) — the
5× differential under-read is upstream of pairing, in what the phase/tag measurement can see.
OFFDBG rules out the shared-offset filters (rawgaps ±200 µs, symmetric, no standing split); SYNCX
feedback model identical on both boards (9999 µs/10 ms cap each). The wire decays at ~2.7 µs/s
(align/EWMA pace), so it is a boot-transient differential that on-device signals are largely blind
to. Next: offline cross-board comparison of per-chunk RAW render phases against the analyzer's
wire for the same span — decides whether phase-in-TSF disagrees with the wire (pairing/consensus
exonerated, tag stamping suspect) or agrees (then the loss is in consensus/extrapolation).

### 2026-08-30 20:36–20:40 — the under-read is in the phase VALUES, not the consensus

Matched-window comparison (20:36:31–20:37:25): rival-clean wire −1463…−1620 µs; the observer's raw
PHASEIN pairwise |phase(A) − phase(B)| ≤ ~200 µs over the same seconds. The render-phase values
under-measure the physical differential ~8× during a boot/settle transient — pairing, extrapolation
and consensus are exonerated (and GDAVG tracks the live delta through an event: +11399 vs +11407 at
n=18 during the 20:40 hole). Suspect: the tag/feedback timestamping follows a model rather than the
physical DMA during transients (SYNCX feedback sits at its 9999 µs/10 ms cap on both boards; a
capped model hides exactly the differential). Deep fix is tag-stamping-side; next session. The
analyzer also lost correlation (rival 0.89) while A drifted, so late-window wire numbers are gated
garbage — graded spans must stay rival-clean (they were).

### 2026-08-30 20:47–20:51 — standing blind offset specimen; column retraction

**Retraction:** "the analyzer lost correlation (rival 0.89)" misread the CSV — that column is
pcm_coef; rival sat at 0.030–0.035 (clean) throughout. The −1.5…−1.8 ms differential is REAL,
rival-clean, stood ~13 minutes without decaying through multiple converge cycles, with every
on-device signal reading fine (err_tag ~0 both, pairwise beacon phases ≤0.2 ms, pipeline depths
equal) — a live specimen of the blind-offset class the pipeline-divergence comment predicted.
pcm_coef degraded 0.98→0.905 (user confirmed) from the misalignment itself. Depth-state capture in
scratchpad/blind-offset-specimen-2048.txt (mixer DEPTH totals differ ~10–70 ms sample-to-sample —
noisy, needs matched-instant comparison). B (the late board) rebooted to restore the pair.
Root-cause queue for next session, sharpened: the displacement lives BELOW the tag/feedback
stamping (SYNCX feedback pinned at its 9999 µs/10 ms cap on both boards); truing the render tags
against the physical DMA is prerequisite to both the standing-offset class and the <1 µs goal —
GDAVG stays shadow until then.
