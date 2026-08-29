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
