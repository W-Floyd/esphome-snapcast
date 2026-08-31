# HANDOFF — snapclient sync work, as of 2026-08-30 21:00

## Bench layout

| board | role | log | flashed with |
|---|---|---|---|
| `snapclient-supermini-e985e8` | speaker A, **logic-analyser channel A**, `/dev/tty.usbmodem101` | `a.log` | `example/esp32-s3-supermini.yaml` |
| `snapclient-supermini-f04d74` | speaker B, **logic-analyser channel B**, `/dev/tty.usbmodem1101` | `b.log` | `example/esp32-s3-supermini.yaml` |
| `snapclient-observer-e99574`  | TSF observer, drives no DAC, `/dev/tty.usbmodem201101` | `observer.log` | `example/observer-supermini.yaml` |
| `f049c8` | unreachable all session, unflashed | — | — |
| `f04fc4`, `ESP32-Caster`, `a56b60` | other speakers, not in the probed group | — | — |

`./reflash.sh` flashes the fleet. **`e99574` is deliberately absent from the speaker lines** —
flashing it as a speaker puts I2S back on its live DAC and renames it, breaking the observer.

Analyser (keep running; it writes `test.csv` + `test.svg`):

    python3 scripts/i2s-skew.py --stream --interval 0 --count 0 --samplerate 12M --samples 200000 \
        --plot-every 0.0167 --plot-window 45 --annotate a.log b.log observer.log \
        --out test.csv --plot test.svg

**`observer.log` goes THIRD.** The first two `--annotate` logs are taken as board A and board B in
that order; anything after is parsed for events only. The observer is the only board that emits
`PHASEIN` -- the group-consensus INPUTS, naming which peer moved -- so leaving it out throws away
the one line that can attribute a group-delta excursion to a device.

Two annotation thresholds, both defaulted so they need no flag: `--rendertag-us` (500) marks a
`RENDERTAG` line whose measured and inferred phases disagree, i.e. a ledger bias the inferred form
cannot see, and always marks `measured=unknown` because the signal refusing IS the event.
`--phasein-us` (1000) marks a peer publishing a phase that far from ours, and names it. Drop it to
200 to see the ordinary spread rather than only excursions.

**`--log-tail-mb` defaults to 4 MB and that is far too small for `--replot` on these logs.** It
reached back only ~12 minutes of `observer.log` and silently reported zero events for a window
that contained a 36-second group-wide burst -- an under-read that looks exactly like "nothing
happened". Use `--log-tail-mb 60` when replotting history. The default is fine for `--stream`,
where it only primes baselines before following the logs forward.

**The analyser goes silent rather than exiting when the USB capture device drops**
(`LIBUSB_ERROR_NO_DEVICE` in its stderr). A stalled `test.csv` usually means that, not a bug.

**NEVER REDIRECT ITS STDOUT TO A FILE.** When it cannot CLAIM the device -- PulseView holding it,
`LIBUSB_ERROR_ACCESS` -- it does not go quiet, it error-loops without exiting. Redirected, that
wrote a **143 GB** `skew.out` and filled the disk (2026-08-28), which then killed both live
analyser instances mid-window and froze `test.csv`. Nothing warned: the boards kept logging
normally, `test.csv` simply stopped, and no preflight checks free space. Leave it on the terminal,
and check `df` before trusting a window.

**BACK UP `test.csv` BEFORE RESTARTING THE ANALYSER** -- `--out test.csv` truncates it, so a
restart destroys the window you just measured.

## Stimulus — this matters more than anything else

The boards play **MLS44**, a maximum-length-sequence stimulus, not music. On music the analyser
cannot resolve this problem at all: adjacent frames correlate at ~0.997, so whole-frame errors
masquerade as findings. On MLS the runner-up correlation is ~0.03.

    server:  192.168.1.2, snapserver in docker, config /mnt/fast/dockge/snapcast/config/
    stream:  MLS44, file:///data/mls44.pcm, added at runtime via Stream.AddStream (loops)
    group:   4eb19e5e — A, B and the observer, all latency 0
    restore: scripts/bench/group-orig.json has the original grouping

`scripts/test-signal.py --self-test` proves the sidelobe level; regenerate with `--out-file`.

**FIXED 2026-08-28: MLS44 now loops SEAMLESSLY via a process stream.** The stream URI is now

    process:///usr/share/snapserver/sandbox/mls44-loop.sh?chunk_ms=26&codec=flac&name=MLS44&sampleformat=44100:16:2

reading a script that does `while cat /data/mls44.pcm; do :; done`, so there is no EOF and
snapserver never goes idle. Confirmed at the source before the change: the server log showed
`AsioStream ... End of file` -> `playing => idle` -> `idle => playing` across **101 ms**, matching
the -101/-102/-104/-107/-118 ms client resyncs exactly.

**IT IS EPHEMERAL AND WILL COME BACK.** `/usr/share/snapserver/sandbox/` is inside the container
IMAGE, not a mounted volume, and snapserver refuses a process stream anywhere else
("Process stream executable must be located in '/usr/share/snapserver/sandbox'"). So the script is
lost on any container recreate/update, and MLS44 reverts to the 20-minute EOF. For permanence, add
to `/opt/stacks/snapcast/compose.yaml`:

      - ${DOCKER_ROOT}/snapcast/sandbox/:/usr/share/snapserver/sandbox/

and keep the script in `${DOCKER_ROOT}/snapcast/sandbox/`. That needs a container restart, which
interrupts Spotify, so it was not done unprompted. A copy of the script also lives at
`/mnt/fast/dockge/snapcast/data/mls44-loop.sh`, which IS on a mounted volume and survives.

TWO TRAPS FROM DOING THIS, both of which cost time:
* `while :; do cat f; done` SPINS. When the reader closes the pipe, cat dies on SIGPIPE and the
  loop restarts it immediately, forever -- a throwaway `| head -c 8` test left it burning 3.2% CPU
  inside the container. Use `while cat f; do :; done`, which loops on clean EOF and exits on
  SIGPIPE.
* **`Stream.RemoveStream` CLEARS THE GROUP'S `stream_id`.** Removing and re-adding MLS44 left group
  4eb19e5e with `stream=''` and all three boards silent until `Group.SetStream` was re-issued. Any
  script that touches a stream must re-point the group in a `finally`.

Historical, for the record:

**MLS44 LOOPED EVERY 20 MINUTES AND EACH LOOP FIRED A ~100 ms HARD RESYNC ON EVERY CLIENT.**
Measured 2026-08-28 overnight: both boards hard-resync at :06:03, :26:03 and :46:03 past the hour,
same second, same magnitude (-101, -102, -104, -107, -118 ms), i.e. group-wide and server-side
rather than a client fault. A 20-minute file at 44.1 kHz/16/2 is ~212 MB, which fits
`file:///data/mls44.pcm`. So the stimulus injects a disturbance into every measurement three times
an hour, and always has.

CONSEQUENCE FOR MEASUREMENT: a window longer than ~18 minutes cannot avoid one. Keep windows short
and check for a resync inside them, or make the loop seamless (pad the file to an exact chunk
boundary, or use a longer one). Every "settled" number in TODO.md was taken between loop events
without knowing they were there.

They are COMMON-MODE and mostly absorbed -- the standing offset held +132 us for 1.5 h across
several of them. The big steps come from the rarer wedge episodes (A at 06:24, B at 08:00 with
`PLAYER STALLED`), not from the loop.

## Before measuring anything

    python3 scripts/bench/preflight.py    # refuses unless all three boards are latency 0, same stream

A campaign killed mid-cycle once left a board at `latency=500 ms` server-side and every
measurement afterwards was contaminated — including one reported as a finding. Campaign scripts
now `trap` to restore on exit; check anyway.

The bench tooling lives in `scripts/bench/` (`preflight.py`, `snapctl.py`, `group-orig.json`).
It used to live only in a session scratchpad under `/private/tmp`, i.e. one temp cleanup away from
gone, while this file referenced it as load-bearing. `preflight.py` resolves `snapctl.py` as its
own sibling, so the directory moves as a unit.

## State of the work

**Achieved, sustained:** ~+4.5 µs median, sd 3.6 between the pair, correction **disabled**. The
wins were all removing interference, not adding control — stream-scoped TSF leadership,
phase-only follower beacons (sd 81.6 → 4.62), CPU1 pinning (sd 6.24 → 3.58).

**`render_align` is disabled and unproven** (`render_align_max: 0ms`). It now fails for one
understood reason: median-of-three is discontinuous — measured hopping ±96 µs while the
underlying data sat at ±12. Fix is to average for small groups.

**LEADERLESS IS IMPLEMENTED, FLASHED AND MEASURED (2026-08-28): median -3.75 us, sd 4.32,
MAD 2.19, three devices, zero churn.** Against the old leader-based best of median +4.5 / sd 3.6
on a TWO-device group. The one wrong turn was step 4 of the plan: a slew on the adopted mapping
made it path-dependent, which destroyed the exact common-mode cancellation a single published line
gave for free, and cost 2.7x on sd (9.72) until it was deleted. `TODO.md` has the full table.

Older note, kept for the sequence:

**`PLAN-leaderless.md` IS IMPLEMENTED AND UNFLASHED (2026-08-28).** Consensus averaging replaced
the leader: every device beacons its own raw server↔TSF line, everyone adopts the robustly
weighted mean, nobody publishes the consensus back, and the adopted mapping slews rather than
steps. Election, takeover, `always_healthy` and the health hook are deleted. Both firmwares
compile; **nothing has been measured**. Judge it on sd against the analyser with the churn gone,
not on the median — see the Sync section of `TODO.md` for the bar and the two things to suspect
if sd worsens.

`e99574` no longer needs special status: `tsf_observer` now only enables the phase-input log.

`TODO.md` carries the full record, including retractions. Two findings were reported and later
overturned; both are struck through rather than deleted, because how they happened is useful.

## THE DELAY-CONTROLLED SERVO — IMPLEMENTED, FLASHED, GRADED (2026-08-28 evening → 2026-08-29)

**This is now the live design on all five boards (build 14, `29ca74f`).** The rate servo steers on
the MEASURED render error (`err_tag` = tagged frame's render instant − its deadline), setpoint zero,
in 32-arrival blocks at ~3 Hz, PI with Kp = 1/tau (tau 10 s) and Ki = Kp/Ti (Ti 120 s). The ledger
prediction survives only as the tags-absent fallback for the per-chunk scheduling path. The integral
(the crystal offset) is persisted to NVS as a 300 s EMA and restored at boot, so a cold boot engages
at +57 ppm with no wind-up. Full narrative, every retraction included: `PLAN-delay-controlled-servo.md`.

    wire (B−A, MLS44, rival-gated CSV)     morning baseline      build 14, 11.5 quiet min
    median / MAD / sd / p2p                +1.2 / 20.0 / 46.7 / 243   +4.0 / 5.3 / 8.9 / 45 µs
    on-device group delta MAD              A 26, B 16                A 22, B 15
    on-device A−B loop-error differential  —                         median +2, MAD 9, r(A,B) 0.995
    event census (resync/splice/OOR/repair) —                        zero on both boards

**Headline test passed:** `inject_split(+1000)` moved the ledger +1020 µs and the audio not at all
(the old servo displaced ~1100 µs on the same injection that morning).

**What the bench found in twelve builds (each measured, fixed, re-measured):**

* Above the splice threshold the fast path owns position; the PI never runs there (build 3).
* Never re-seed the integral; every hold programs the integral (out of range) or carries P decaying
  over tau (tag loss, mapping flap). Both omissions produced audible limit cycles (builds 3, 11).
* Speaker-callback stalls (60–1500 ms, fleet-wide) stamp completions late → phases and err_tag lie
  by the stall length while tag-age reads normal. Blank the tag stream on any feedback gap > 50 ms (4).
* Ti = tau let the integral swing ~57 ppm p-p chasing the ±600 µs / ~60 s common-mode wander; Ti is
  now decoupled (120 s) and an out-of-range integral > 20 ppm from its EMA snaps back (12).
* **The accounting-split repair is disarmed while tags are live** (13): it fires all day in
  equal-and-opposite pairs from the mixer-ring drift sawtooth (−29026/+29024 µs, …) and under the
  new design each step went straight to the hard-resync path as a 30–50 ms audible move.
* **Coarse decisions (hard resync, storm mute, aggressive catch-up) use err_tag while tags are
  live** (14): unrepaired, the prediction's bias rode the wander into the 50 ms threshold.
* Autotune exists (`servo_param autotune 1`) but is one-sided (slows on ringing only): a
  standing mean with r1 ≈ 1 is what tracking the common-mode wander looks like on ONE device.

**Bench workflow that made twelve builds possible in an evening:** `./reflash-speakers.sh` (one
build, two OTAs); `scripts/servo-param.py <name> <value> <hosts>` for tau_s, ti_s, block_n,
splice_us, gate timings, autotune, persist — session-local, reboot restores flashed defaults;
`scripts/bench/dl-window.py --a-off N --b-off N` grades a log window anchored on byte offsets;
`scripts/bench/wire-window.py --from HH:MM:SS --to HH:MM:SS` grades the analyser CSV, rival-gated.

**What the device can see of the wire (measured 21:22–21:27):** errA−errB vs analyser r = 0.88,
bias 2.5 µs, 13 µs/s noise — an honest but coarse estimate (→ ~2 µs after a minute). The
render-phase difference is far worse (sd 46 µs, biased +70 µs, stall spikes). Any slow differential
feed-forward should be built on exchanged err_tag, not on the phases. The analyser resolves each
capture to ~26 ns (`scatter_ns`), so every µs on the wire is board behaviour.

**Open, by audible impact:**

1. **Server-side delivery pauses** — the dominant audible event all day (`no chunk records for 3 s,
   ring holds 0`: 120–297/hour at 14–15h, 8–20/hour tonight), seen by all three boards at the same
   instants; the Pi host / process-stream loop / AP. The client cannot fix a source that stops.
2. Speaker-task feedback stalls — blanked, root cause open.
3. Consensus steps around OTA/replug (operator-induced) and boot-time mapping flapping; the
   play-before-time-sync early-side wedge (18:36, `b.log`) has no bailout.
4. `block_n` 32 vs 64 A/B on a quiet span (probably a wash); tau 30 only once integrals are within
   ~1 ppm (standing error is integral_error/Kp); the ~+4 µs standing offset (feed-forward on
   exchanged err_tag); dead-time compensation for the ±5 µs wander residual.
5. HA `number`/`switch` entities on the tunables; parse `DLLOOP` into the analyser CSV.

## Builds 15–27 in one afternoon (2026-08-29, 11:00–16:00) — what changed and what the ledger says

All committed on `main`; `PLAN-delay-controlled-servo.md` carries the blow-by-blow with the evidence.

**Mechanisms now in the firmware**
* **Rate-lock dither** (build 17): the MCLK fractional divider's achievable ratios are 0.5–1.2 ppm
  apart at our operating point; `RateLock::tick()` (speaker callback, ~100 Hz) sigma-delta dithers
  between the two bracketing ratios. Residual ≤ one step × one tick ≈ 10 ns.
* **Error-proportional gain** (builds 19–21): Kp = (1/tau)·max(1, |err|/knee), effective tau floored at
  `tau_min_s`; Ti is NOT boosted; the bumpless transfer keys on the tuned 1/tau only (with a
  per-block-varying kp it had become a hidden integrator — build 20 diverged from zero).
  Defaults tau 120 / Ti 600 / block 64 / knee 25; on the bench knee 150 (above the ±350 µs common
  wander). A magnitude knee cannot separate "far from setpoint" from "the common wander" — that is
  the standing limitation.
* **Tag fault** (builds 18/23/25): a coarse correction on err_tag that leaves err_tag unmoved is a
  miss only while tag and ledger DISAGREE (> 3 ms); three misses → TAGFAULT: tags distrusted 180 s,
  the accounting-split repair pre-armed (it is what actually closes the split — the LEDGER slips,
  the tags were right), reconnect only on a second fault without a repair.
* **Dead-session detector** (build 26): 15 s without any received byte on a connected session →
  reconnect. Both boards starved 4+ min at 15:07 after the server dropped their sessions with no
  FIN; nothing else could notice.
* **Cold start** (build 25): no NVS integral → seed from the TSF own-crystal estimate and run the
  fast boot Ti 180 s. A restored board never sees either (fast Ti on a restored board integrated
  the post-boot transient into +13 ppm — build 17).
* **Observer publishes no render phase** (build 22): its +9.5 ms phase had made the speakers'
  group render delta bimodal and useless.
* **render_align** is runtime-tunable (`align_max_us / align_gain / align_deadband_us /
  align_reject_us / align_step_us / align_apply`) and SHADOW-ONLY by default (build 27). **Sign
  measured on the wire (17:03–17:10, single-board step):** positive bias = this board plays EARLIER;
  positive group delta = this board is EARLY; so `bias -= delta*gain` (the original code; build 29
  restores it after a wrong flip in build 23 that made two live runs run away). `align_max_us 0`
  clears the bias. Enable with `align_apply 1`, cap small (60 µs) first.
* Runtime tunables persist nothing: every reflash resets them (`servo-param.py`) — build 30's
  defaults ARE the operating point, so a reflash no longer needs a param pass.
* `RAW` is VERBOSE since build 30: two boards crashed in ESPHome's thread-safe logger buffer
  (`TaskLogBuffer::send_message_thread_safe` → `xRingbufferSendComplete` assert) under 38 lines/s.

**Steady state with the inter-device channel applied (build 29, knee 150, align gain 0.1 / step 4):**
18:00–18:45 (cap 150): n=27,126, wire median +2.7 µs, robust sd 5.0 µs, p2p 27.8 µs; 18:46–19:31
(cap 300): n=25,155, **median +4.2 µs, robust sd 4.0 µs, p2p 46.9 µs**, 3-min medians 0…+11 µs;
20:47–21:47 on build 30's compiled defaults with a starvation/TAGFAULT/reconnect inside: median −2.1 µs,
robust sd 7.6 µs, 5-min medians −16…+15 µs, bias creep gone (deadband 15),
1-s change 0.19 µs, every 3-min median within ±8 µs, zero events. Build 30 (`be736f3`, FLASHED fleet-wide
20:38, boots to the operating point with no API tuning; wire ≤ 20 µs at +73 s) makes that the compiled default: knee 150, align applied, cap 500, gain 0.1,
step 4, deadband 15 (the exchanged phase carries a ~10 µs bias of its own; a 3 µs deadband made the
bias creep ~1 µs/min forever). 19:32–20:32 (cap 500, deadband 3): median +2.3 µs, quiet-part 5-min
medians within ±10 µs, zero A events; the observer's crash-rejoin at 19:33 jolted the wire +89 µs. The channel removes a standing offset at ~4 µs per 10-s report; events
(starvations, bailouts) re-create 50–100 µs offsets every ~10 min on a bad hour, so the two
numbers to watch are the event census and the 3-min medians.

**Resync after a disturbance (builds 31–37, 22:15–23:00; target |A−B| < 100 µs within 5 s).**
Measured with `scripts/bench/resync-test.py HOST 300` (an `inject_starvation` over the API, then
the wire timed back inside the band at 1-s resolution). Mechanism: a **resync window**
(`resync_win_s` 60) opened at engage, at every `mark_kp_event_` and at reconnect, in which the
coarse path does **step-and-verify** — arms at `resync_splice_us` (100), corrects `resync_gain` (0.8)
of the measured error once per `resync_blank_ms` (1200 ≈ a full block + pipeline, so the judging
block starts after the step), bounded at half a chunk; the ledger may take the first step at t+0
(it knows the dropped chunks exactly), the continuous fast splice is OFF inside the window (it
bang-banged against the block-averaged error), and the PI runs at the floor tau regardless of the
knee. Tag-fault judgement waits 2 s and never fires in the first 20 s after engage (builds 32/33
faulted both boards 20 s after boot on the normal settling). Ledger (300 ms injection → <100 µs held
5 s): build 31 12 s · 34 11/31 · 36 8/9/13/17 · 37 9/–/7/15 · 40 11/34/10/8 (flat tails, no drift). Boot to
<100 µs: 31 27 s · 34 48 · 36 24 · 37 22 · 40 34. No per-board rate boost anywhere since build 40 (see the
rule below); build 41 removes the knee for the same reason; build 42 arms the in-window step at 100 µs
with one step per block.

**THE RULE THAT EXPLAINS THE STAIRS (measured 22:58, build 37): any gain that only one board has
converts common-mode error into differential motion.** After a one-board disturbance A ran its
resync-window PI at kp 0.05 while B sat at 0.008; the same +30…+130 µs common deadline wander then
produced (0.05 − 0.008) × 80 µs ≈ 3.4 ppm of *differential* trim, refreshed every block — the wire
walked away from zero at 2–3 µs/s in block-sized stairs (`trimB − trimA` −7…−10 ppm against a
crystal-set equilibrium of −5.5). Build 40 removes the in-window rate boost. Consequences: the rate
loop's gain must be the same function of the error on every board (the knee's error-proportional
boost is the residual asymmetry — tolerated only because knee 150 sits above the wander); resync
is done by *position* corrections (bounded one-offs), never by a local rate boost; the
attribution table that found this is worth reproducing for any future "why does it drift" —
wire slope vs `trimB − trimA` vs coarse frames vs align biases, 3-s bins.

**Cycle time** (wire |A−B| ≤ 20 µs held 20 s, from the reboot line; `scripts/bench/converge-time.py`):
build 18 >450 s · 19 242 · 22 209 · 24 74 · 25 67 · 26 46. Boot→engage is ~15–20 s of that.

**Steady state** (`scripts/bench/wire-sf.py`, structure function): with tau 30 the 1-s change fell
0.63 → 0.38 µs and with knee 150 to 0.30; the slow term (5–7 µs over 20–60 s) is a per-board error
that does not average — the exchanged render phase has the same 7 µs and does not track the wire
— so it is the measurement, not the mapping alone. <1 µs needs a differential channel with a
better exchanged signal (publish a line, or the 30 Hz exchange, now that there is a consumer).

**Bench tooling**: `watch-bench.py` (persistent Monitor: TAGFAULT/stalls/bailouts/|err|>5 ms/split
>5 ms/wire >200 µs/no-correlation/log gaps), `converge-time.py`, `wire-sf.py`, `wire-vs-common.py`,
`align-shadow.py`, `dl-window.py`, `wire-window.py`, `servo-param.py`.

**Open**: root cause of the tag/ledger split after a chunk-drop storm on an empty ring (repair
bounds its cost; mechanism unknown — every player-side path read is aligned); align sign; the
post-boot starvation cluster (3 stalls in 12 min after a boot, same AP/channel, TX power ruled
out); B's USB serial wedges on every OTA (logger restart does not recover it; replug does — never
within 2 min of the OTA); server `buffer` 2000 → 4000+ ms would ride out RTO back-off holes.

## `render_align`'s replacement signal — IMPLEMENTED, FLASHED, GRADED, PASSES (2026-08-28)

`PLAN-render-align-signal.md` is built, on all five boards, and **it passes its ratio test: 0.94
against a logic-analyser truth, where the signal it replaces reads 0.12.** `render_align_max` still
stays 0ms -- passing the ratio test proves the signal SEES a displacement, which is not the same as
being quiet enough to steer with. See "What is still open" at the end of this section.

What it does: the client attaches an identity to the audio it hands down -- `(server_ts, frame
offset into that chunk)` -- and the i2s sink hands that identity back when THAT audio renders,
paired with the instant it did. Render phase is then

    phase = TSF(adjusted_ts - real_frames/rate) - (tag.server_ts + tag.offset/rate)

with `pushed` and `played` appearing nowhere in it. That is the whole point: the old form inferred
the rendering frame's server time from the frame ledger, and a device cannot detect that its own
counter is biased by consulting that counter.

Where it lives:

| repo | file | role |
|---|---|---|
| esphome fork | `audio/audio.h` | `RenderTag`, `RenderTagTrack` (tags bound to frame positions in one queue) |
| esphome fork | `speaker/speaker.h` | `set_next_render_tag`, `supports_render_tags`, `add_tagged_output_callback` |
| esphome fork | `i2s_audio/speaker/*` | `WriteRecord` carries the tag per DMA descriptor; standard writer reports |
| esphome fork | `mixer/speaker/*` | forwards a tag only while ONE source contributes; marks blended runs untagged |
| esphome fork | `speaker_source`, `media_source` | pass-through both ways |
| this repo | `snapclient/snapcast_client.cpp` | tags every `push_chunk_` write, consumes `notify_audio_played_tagged` |

**THE GRADING RUN (2026-08-28 12:16, A perturbed, B untouched as control).**

`inject_split(+1000)` ramped onto A at 100 us/s, wire truth from the analyser:

    wire displacement of A (truth)     +1084 us
    on-device gap change (measured)    -1020 us, sd 0.6, n=21   ->  RATIO 0.94   PASS
    same statistic on B (control)             0 us             ->  flat, as it must be

The statistic that matters is the WITHIN-BOARD gap `measured - inferred`, because it cancels every
common-mode term -- group drift, board wander, TSF consensus motion -- and isolates exactly the one
thing the two estimators disagree about. The gap also tracked the injection's own 100 us/s ramp
(-578 us mid-ramp at +12 s), which is what makes it causation rather than coincidence.

The differential-against-control form of the same test gave 1.14 / 0.12, but its pre-window
straddled a reconnect on A, so prefer the gap statistic. `inject_split(0)` IS NOT A RESTORE --
reverse with the negated value, and expect ~1 frame (22.7 us) of quantisation residual, which shows
up as `split +22` on the board afterwards.

To repeat it: ramp `inject_split(+1000)` on one board, read `measured=` and `inferred=` off the
`RENDERTAG` line. Both are on one line, so it is one run on one firmware, not a two-firmware
comparison.

Secondary check, NOT YET DONE: the resync-residual ratio should move from 0.13 toward 1.0.

**WHAT IS STILL OPEN — read this before enabling `render_align`.**

Passing the ratio test proves the signal SEES a displacement. It does not prove it is quiet enough
to steer with, and on today's evidence it is not. Quiet-bench group delta, 2026-08-28 12:52-13:05,
five-device TSF consensus:

    excluding outliers   A  n=50  median  +4  MAD 20  sd 87.5  p2p 461
                         B  n=50  median +12  MAD 23  sd 82.3  p2p 383
    including them       33 of 83 samples per board -- ONE 36-second group-wide burst

Re-measured after the 100 ms freshness gate went on (13:16, same bench, group of 4):

                         outliers    median   MAD    sd    p2p
    A  before            33/83          +4     20   87.5   461
       after              0/104        -24     34   68.8   408
    B  before            33/83         +12     23   82.3   383
       after              0/103        -21     27   61.0   399

The multi-millisecond burst samples are GONE -- zero outliers in 207 samples across both boards --
and sd fell ~25%. **Do not read the MAD columns as a regression.** The "before" core of 50 was what
survived discarding 40% of samples as outliers, i.e. conditioned on sitting outside a disruption
burst; the "after" core discards nothing. They are not like-for-like populations. The gate is also
provably not the cause: in the whole post-fix window there were ZERO `measured=unknown` publications,
so on a healthy bench it rejects nothing and cannot have moved the MAD.

MAD 27-34 us is still inside the band that disqualified `RECON drift` in the plan, the tails are
still heavy, and before the gate 40% of samples sat in the MILLISECONDS during one burst. That burst is common-mode across boards and coincides with the
five-device consensus re-forming, so **a device re-locking drags the group median** -- which is the
thing to understand before anything steers on this.

**What a real disruption showed (13:00:15, unprovoked, caught in the quiet-bench window).**

This is the useful half of that burst, because it is the case the signal exists for:

    time      measured   inferred    age     tags   what happened
    13:00:17  UNKNOWN    -...424363  3.26 s  168    gate fired; INFERRED returned a confident
                                                    wrong number ~2 ms out in the same report
    13:00:19  -...837659 -...086550  0.42 s   13    gate PASSED a stale reading, wrong by 1.6 ms
    13:00:21  -...378754 -...386101  3 ms    206    fresh; both ~40 ms out -- a REAL excursion

So `measured` never fabricated: where it had nothing trustworthy it published
RENDER_PHASE_UNKNOWN while the old signal published a plausible wrong number. That is the intended
difference between the two, observed in the wild rather than argued.

It also found the freshness gate was too loose at 1 s, since fixed to 100 ms (ten DMA descriptors --
tagged renders arrive every ~10 ms while tagged audio flows at all, so 0.42 s does not mean "a bit
stale", it means STOPPED). Both bad samples above publish nothing at the new bound.

Things to know before debugging it:

* **`tags=0` on the RENDERTAG line is a configuration answer, not a fault.** Tags are deliberately
  suppressed through a resampler, while the mixer is blending an announcement, and for audio the
  client inserts itself (silence, splices, repeated frames). `sup=` on the same line says whether
  the path claims to support them at all.
* **When there is no fresh tagged observation the client publishes `RENDER_PHASE_UNKNOWN`, not the
  old inferred value.** That is deliberate: a silent fallback to a signal blind to ledger bias,
  dressed as one that is not, is exactly how this failure was missed for a whole measurement
  history. `inferred=` is logged but nothing acts on it.
* Tag entries are collapsed when contiguous (`RenderTagTrack::continues_last_`), so a producer
  re-stating identity on every write costs one entry per chunk, not one per write. An evicted tag
  yields a SKIPPED reading, never a fabricated one.

## Where the alignment problem actually went (2026-08-28, end of session)

**`render_align` is still 0ms, and should stay there -- but the reason changed.** Measured on a
settled bench, two independent instruments agreeing:

    on-device group delta   A median  -9 us  MAD 26   B median +0 us  MAD 16
                            n=232/230, availability 100%, ZERO |delta|>1000 outliers
    logic analyser (B-A)    median +1.2 us   MAD 20.0   sd 46.7   p2p 242.8   rival 0.029

The pair is aligned to about a microsecond. The plan disqualified `RECON drift` because
"correcting a tens-of-us error with a 23-45 us MAD signal injects noise of its own size"; that
objection still holds at MAD 16-26, but it is now academic, because **the error being corrected is
1-9 us**. Any correction would be pure injected noise.

None of that came from `render_align`. It came from:

| fix | effect |
|---|---|
| stream scoping repaired | foreign-stream peers out of the consensus; the ms-scale bursts vanished |
| freshness gate 1 s -> 100 ms | stale phases refused rather than published |
| mixer forwards tags per boundary | `off` within one chunk went 22% -> 100% |
| LEAVING THE BENCH ALONE | sync medians 200-400 us -> ~20 us |

That last row is not a joke. Thirteen reflashes in one session made the operator the dominant
disturbance: |median error| is 154 us within 15 s of a consensus membership change against 93 us
elsewhere (p90 674 vs 286), and every reflash causes five of them.

**The largest remaining inter-device term is the SPLIT-HOLD**, and it is understood:

    15:03:13  A: applied=+64.00 ppm  samples=0    splithold=128   <- A freezes
    15:03:14  B: applied=+38.15 ppm  samples=128  splithold=0     <- B keeps steering
              ~26 ppm differential x 3 s hold = ~78 us of skew

While a sustained accounting split is timed toward `DRIFT_REPAIR_HOLD_US` (3 s), the trim is
pinned to the PI's INTEGRAL -- deliberately, because steering on a prediction about to be declared
wrong plants a permanent displacement (measured: -101.5 us per repair before the integral-hold
existed, ~25 us predicted after). Each board is individually right. The PAIR is wrong anyway,
because neither knows the other is holding. Do NOT "fix" this by making the hold common-mode: that
would freeze every device's transient PI output simultaneously, which is worse.

**The tagged render signal sees what the ledger-based one cannot, and this is now measured twice.**
`inject_split(+1000)` on A, reading the two error signals side by side:

    err_live  barely moved (-57..+95)   the servo NULLS it by displacing real audio, so it is
                                        structurally blind to the displacement it just created
    err_tag   moved ~1100 us            it measures where the audio actually is
    diff      -1020..-1052              ratio 1.02-1.05 against 1000 us injected
                                        recovered to -33 us within ~15 s of the negated restore

Same result as the render-PHASE test earlier the same day (0.94 against 0.12). What is NOT yet
established is whether the tag signal is quiet enough to close a loop on -- accuracy is proven,
precision is not. See `CLAUDE.md` for why the first two attempts to measure that were both wrong.

**B WEDGED ONCE (14:56), and it needed the power pulled.** Four `Pipeline refusing audio for 2 s`
warnings, then TOTAL log silence for 2 m 23 s -- not a zombied player task still emitting other
components' lines, but complete stoppage. That matches a documented mixer failure mode ("stayed on
wifi and answered pings, but declared healthy API clients unresponsive and refused OTA"), whose
mitigation is ALREADY in the code being run. So either it does not cover this path or there is a
second wedge with the same presentation. 4 MB of `b.log` around it is the only evidence that
survives a power cycle; one instance is not a rate.

## Traps that cost real time

* **`a.log`/`b.log` span days and carry no date.** `grep "^\[13:5"` matches a previous day's build.
  Anchor on file position (`tail -N`), never on timestamps.
* **Verify the perturbation happened.** Three campaigns measured nothing: `execute_service` is a
  coroutine and silently did nothing unawaited; 1/5/100 ms latency steps are absorbed without a
  resync; `inject_starvation` never trips the storm. Only a **500 ms** step forces a mute.
* **`inject_split(0)` IS NOT A RESTORE.** The field means "no request pending" (`0 when none`), so
  zero does nothing and the board stays displaced. Reverse with the NEGATED value
  (`inject_split(-1000)`) and confirm on the wire that it came back. A restore that silently
  no-ops is the same contamination class as the stray `latency=500 ms` above — B sat +1017 µs out
  before this was noticed (2026-08-28).
* **A STEP perturbation reads as `nan`, not as movement.** `frame_lag`'s continuity guard rejects a
  lag jump that cannot physically happen between captures, so a 5 ms latency step produced 100%
  `nan` while a ramped 1 ms `inject_split` tracked to within 2%. Perturb by RAMP, or expect no wire
  measurement across the step itself.
* **`esphome` lives at a versioned Cellar path** — resolve it as
  `$(head -1 "$(command -v esphome)" | sed 's|^#!||')`; a brew upgrade broke it mid-session.
* **`./reflash.sh` exits 0 even when every build failed.** With Docker Desktop not running,
  all four configs failed on `failed to connect to the docker API` and the script still returned
  0 and printed no summary — a silent no-op flash that looks identical to a successful one. Check
  for `OTA successful` per device, not the exit code. (Docker also dropped out MID-RUN once,
  taking only the last config with it.)
* **Commit before flashing.** A `git checkout` bundled into a flash command destroyed 20 minutes
  of uncommitted work.
* **Log lines truncate.** `pad=` at the end of `RECON` reads as `pad=882` for a counter in the
  tens of millions; `tsf=` is cut off entirely. Add a short dedicated line instead.
* **A replug within ~2 min of an OTA reboot ROLLS BACK to the previous firmware** while the OTA log
  says successful (the new app has not marked itself valid yet). Both boards reported the previous
  build's compile time via API `device_info` after a 40 s replug. Verify the running build over the
  API after every flash; restore wedged USB tails by restarting the `esphome logs` process, not by
  replugging.
* **An IDE restart kills the `esphome logs` tails** (they live in its terminals); the logs freeze
  and the analyser keeps annotating from frozen files. Port map: `usbmodem101` A, `usbmodem1101` B,
  `usbmodem201101` observer. Append (`>>`) when restarting so byte offsets stay valid.
* **`SPLITINJECT ramp complete` is an unreliable witness** — it logs only when the zero lands on a
  chunk that spends a whole frame. Use the SYNCX `drift`/`split` step as the positive control.
* **Grade the wire from the CSV, not from plots.** `scripts/bench/wire-window.py` reads `test.csv`
  with the rival gate; the analyser writes NaN rows on PCM-lock loss.


## Build 44 (2026-08-29 23:46) — gate sign, and the wire's sign

* The resync group-delta gate compared the wrong signs in build 43 and refused every step on which
  err_tag and the group render delta agreed (injection recoveries 12 / >75 / 57 / 69 s). Fixed:
  `delta > 0` and `err_tag > 0` are both LATE; `+bias` = plays later. Derivations in PLAN 23:27–23:50.
* **The analyser's probe b is on board A (e985e8).** `test.csv` `offset_ns` positive = board B
  (f04d74, b.log) EARLIER, the opposite of the header's wording; the CSV's firmware columns are by log.
  Measured from the rate-attribution table (`fs_b − fs_a` vs requested trim differential), not inferred.
  Every earlier "A early / B late" reading of the wire in HANDOFF/PLAN should be read swapped.

## Builds 45–50 (2026-08-30 00:14–01:00) — the resync window made to work, one line at a time

`RSTEP`/`RSKIP` (one line per in-window block: target, source, group delta, verdict, step) found, in
order: the ledger stepping every chunk (no throttle), the ledger and the tags stepping on each other
(1-Hz ±4.5 ms limit cycle), the gate starved of evidence (group delta paired by coincidence, unknown
for 20–40 s after boot), and the in-window blank being 500 ms instead of the promised 1200. Fixes:
ledger takes only the first step of a window (48), render phase published every block (49), blank =
`resync_blank_ms` (50). Injection recovery to <100 µs held 5 s: build 49 **9 / 11 / 19 / 15 s**.

Open, with evidence in PLAN: the 51 ms `RECON drift` flip whenever the mixer transfer buffer reads
full; post-storm tag/ledger splits that nothing acts on (SHADOW shows them; TAGFAULT's judge only
watches coarse actions); align's lag (deadline actuator, audio sensor, PI τ between — needs peers'
err_tag in the beacon); the analyzer's ~100 s blindness after an I2S restart.

## Build 56 (2026-08-30 02:33) — resync 9 / 27 / 9 / 9 s; how it got there and what is structural

Builds 51–56 each fixed one thing RSTEP showed: a ledger step blanks the tag path (51); the coarse target
subtracts steps in flight (52/53 — subsumed); the in-window blank is the measured visibility horizon,
ring + pipeline + two blocks ≈ 3.5 s, computed from the live ring depth (54/55); the ledger's first step
takes the full target (55); the gate bounds a step by the gap to the others, `gd·n/(n−1)`, not by the
delta to the mean (56); `resync_gain` 1.0 (56). Result: one tag step per injection, converged.

Structural floor ≈ 9 s (ring travel 3.5 s per round, two rounds, plus 2 s to the hard resync). The
ledger's first step never reaches the tags (PLAN 02:13); the tag error reads 60–250 µs from the wire
after a step (measurement floor, also the group delta's). Both are the next design items; neither is a
tunable. Also still open: the 51 ms `RECON drift` flip; post-storm splits nothing acts on; align's lag;
the analyzer's ~100 s blindness after an I2S restart; the observer's own bailouts (old build).

## Builds 57–62 (2026-08-30 07:10–08:55) — start-up, align, and the wedge

* **Boot → audio → lock:** `never_mute` now means no start-up silence; the unmute latch also passes on
  *group agreement* (|group delta| in band, own error ≤ 4 ms). 07:22 boot: audio at +7.5 s, wire
  correlated +14 s at +28 µs, "Sync locked" +20/+26 s (was +150/+186). The previous "analyzer blind for
  100 s after boot" was the boards playing silence until converged — retracted as an analyzer fault.
* **Align (57–62):** fractional step accumulation; bias changes delivered as position by a ≤10 ppm rate
  kick (removes the τ = 120 s lag that forced gain 0.03); beacon carries the phase's sample age so peers
  pair on sample instants (the two deltas summed −8…−15 before, ±4 after; the common bias march is gone).
  Runtime: `align_gain 0.3`, `align_deadband_us 1`, `align_step_us 20` — compiled defaults still 0.1/15/4.
  Wire mean at 0 ± 1 within minutes; ±8 µs per-minute meander remains (P-term noise).
* **The wedge (07:52):** 40 s server hole → reconnect → mixer never restarted, player silent, network
  task parked in `emit_pcm_`'s ring-room wait → unreachable until `api.reboot_timeout` (15 min) reset both.
  Build 61: `emit_pcm_` drops after 2 s instead; STALLED report split (records= now printed). Root cause
  of the player's silence still open. Logger: per-second ESP_LOGD from non-main tasks crashes the
  TaskLogBuffer ring (B 07:51) — 'Render phase' demoted; audit the rest.
* Dither: A's bracket duty 0.97 vs B's 0.57 gives A a visible ~0.3 s bump at the same 0.85 ppm gap; cosmetic.

## Builds 63–65 (2026-08-30 09:07–09:45)

* A board broadcasts no render phase for the 4 s after a position step or hard resync (63 tried "window
  open or unconverged" and silenced both boards at boot — no group delta, +528 µs crawl; 64 fixed the rule).
  Peers no longer chase a reconnecting board (A's bias moved 63 → 71 → 65 during B's 09:33 reconnect,
  vs −6 → −75 on build 62).
* Build 64 quiet minutes: wire medians +0.1…+5.7 µs, MAD 0.3–1.9; delta sums ~0; recovery to ±3 within
  ~2 min of a timebase event. Build 65 makes align gain 0.3 / deadband 1 / step 20 the compiled defaults;
  a 45-min untouched grade started 09:45.

## Builds 66–69 (2026-08-30 09:55–11:25)

* 66: a tag distrust ends after three blocks of tag/ledger agreement within 1 ms (was a 180-s timer;
  measured 13 s at 10:07 vs minutes of ledger-only steering on a ledger that flips by 52 ms).
* 67: biases exchanged in the beacon; each device subtracts the group mean (≤2 µs/cycle). The common
  march (+3 µs/min on both) is gone; biases hold within ±40 of zero.
* 68: one `phase_transient_until_us` (window steps, hard resyncs, deadline fallback/re-engage) silences
  the beacon; align runs only while own |err_tag| is in the unmute band. Boot lock +21/+25 s.
* 69: align's fractional accumulator discards the clamped excess — a transient delta over the 20 µs cap
  had been queuing +20 steps against later deltas (the "bias moves against its delta" oddities).
* Measured, open: over 29 cycles only 28 % of the differential bias reached the wire within 10 s
  (kicks verified delivered at +5 ppm·s each); a single-board deadline-step test was contaminated by a
  timebase event — repeat in a quiet hour. Server holes every 10–30 min remain the dominant disturbance.
* Build 69 grade, 40 min with seven server holes: wire median +0.9 µs, robust sd 6.1, 84 % of samples
  inside ±10 µs; SF 1 s 0.24 / 60 s 6.7 / 120 s 10.3 µs. Biases inside ±18 and tracking the deltas.
* Build 70: a fast splice marks the phase transient (engage); neither board chased a faulting peer in
  three faults. Grade 12:12–12:57 (six holes): median +2.5 µs, robust sd 9.7, 85 % inside ±20 µs.
  Unexplained: 1–2-min excursions to +10…+22 on asymmetric delta readings with no logged event.
* Clean step test (build 71 hooks): deadline→wire gain ≈ 0.8–1.0 with the PI's τ; the kick path delivers a
  40 µs step whole within 30 s; align closes a standing 46 µs in 90 s. The 28 % figure is retracted.
* Build 73 (shared-offset hold through TSF-sample blips): deadline fallbacks 0/0 in 47 min (were
  2–32/h); 40 quiet minutes at ±5 µs with zero faults — the flap was a first-order disturbance.
* Build 73, 90 min untouched through ~30 server holes: median +1.9 µs, robust sd 11.5, 77 % inside
  ±20 µs; deadline fallbacks 0; biases at 0/+1. Remaining event classes: the holes, and the post-refill
  tag outlier (tags read tens of ms while the ledger is sane → TAGFAULT → 15 s reconnect, B ×5).
* Build 75 (tag blank spans the ring's travel): a real 16:50 hole hit B and the observer (old build)
  simultaneously — the observer TAGFAULTed with the classic signature, B rode it clean (tag/ledger
  within 75 µs throughout). The post-refill tag outlier is fixed; injections + grade in the task log.
* Build 75 grade: injections 34/11/32/24 s, zero TAGFAULTs during them; 30 min median +1.6 µs, 75 %
  inside ±10. The remaining two outlier doors (render gap, rebaseline) closed in b26c7a4 → next flash.

## Builds 77–81 (2026-08-30 17:36–18:50) — the <10 s convergence goal, met

* 77 named the budget: injections 20/15/10/32 s; the whole variance was the ledger's first step
  landing on mid-refill readings (residuals 1–14 ms; the burst's ledger errors bounce 27→45 ms
  chunk-to-chunk).
* 78: the ledger's first window step waits for two consecutive readings within 20 % (500 µs floor).
  Landed every first step at +1.9 s, but exposed the next layer: pend read +0 at every decision — its
  travel estimate (instantaneous ring+pipe+block) under-reads while the ring is drained post-hole, so
  each ledger step was re-stepped in full by the next tag round.
* 79: frame-exact in-flight accounting — a step has landed exactly when `played_frames_total_` passes
  the push index it was applied at (+ margin), with a sign guard so the subtraction can never
  manufacture a wrong-way step; window decisions run at block cadence (the 3.2 s act blank removed;
  the judge path keeps the horizon). 10/10/10 s on the clean runs. Margin re-learned as TWO blocks
  (build 54's lesson: +1429 stepped, 0.64 s later pend=+0, +992 re-stepped → −1163 overshoot).
* 80 instrumented the remaining 30–50 s tails (OFFDBG): the shared-offset-filter hypothesis was
  retracted — the filter's motion is the genuine server-vs-local clock ramp and identical in tail and
  clean runs. The tail is a sub-arm residual (±100–450 µs) that decayed at flat τ 120 s because
  knee_us defaulted to 1e6: the error-proportional boost was OFF.
* Runtime A/B (no reflash): knee_us 25 / tau_min_s 5 on both boards — symmetric in the error, so
  wire-safe per the 08-29 per-board-boost lesson. Injections 14/10/13/14 s **including the 5 s
  hold**, i.e. <100 µs at +5…+9 s. Baked as compiled defaults in 81. TAGFAULTs 0 throughout.
* Open: quiet-hour wire sd under the boost (e≈80 µs wander now runs τ_eff≈37 s, common-mode — watch
  the soak); cmp=1 big-drift census still nonzero on 77 (census window was all bursts; consumers all
  read zero); the hard-resync tag door is now the critical path (~3.4 s) and could go frame-exact
  like pend; the group delta under-reads the physical differential ~3–5× in tails (phase pairing /
  extrapolation — unexplained, the wire and err_tag agree with each other).

## Builds 82–83 and the 18:53 regime change (2026-08-30 evening)

The <10 s result above held only until the server's holes changed class at ~18:53: bursts of 964 ms
to 6.3 s of lateness every few minutes (observer bailouts 17:50 / 18:57 / 19:02 / 19:19). Under
those, every refill plants a tag/ledger split (tags+wire see early audio, ledger reads ~0) and the
window's tag steps then fight an unattributed per-chunk drop actor in a stable ~10 s limit cycle;
TAGFAULT→reconnect used to heal it, but a reconnect's own refill burst plants the next split (A
looped through 3 TAGFAULTs; broken by a reboot). Two machinery defects found and fixed on the way:
82 made ledger steps subtract in-flight corrections (the 18:54 seven-step storm), and its err−pend
arithmetic was itself unstable at boot — 83 replaced it with serial step-and-verify (never step
while a step is in flight; frame-exact landing test). B rode the same regime settled at +35 µs.
State at 19:26: both boards settled on 83, injections stopped, PLAN carries the standing decisions:
actor-tagged corrected counters first, then revisit re-arming the split repair on large persistent
disagreement; the jumbo holes themselves are server-side (buffer 2000→4000 / the Pi).

## Builds 84–86: the 50 Hz phase exchange and what it found (2026-08-30, 20:25–20:56)

Toward <1 µs: phases are now SAMPLED per tagged chunk (94 Hz) and exchanged in phase-only packets;
paired deltas roll into a 1 s averaged group delta (`GDAVG`, shadow only, validated against the
live delta in quiet and through an event). Findings, in order of importance:
* **The render-phase values under-measure a real differential ~8×** (matched window: rival-clean
  wire −1.5 ms, pairwise beacon phases ≤0.2 ms). Pairing/consensus exonerated; suspect is the
  tag/feedback stamping (SYNCX feedback pinned at its 10 ms cap on both boards). This blocks the
  <1 µs goal and explains the "blind standing offset" class. First job next session.
* **Build 85's unicast phase loop physically displaced the audio a stable −1.5 ms** (bucketed wire:
  +2 µs → −1460 µs at exactly 85's boot; survived a board reboot; cleared the moment 86 disabled the
  loop). ~100–200 sendto/s from the tag-observation thread. Mechanism owed; candidates in PLAN. Safe
  redesign when needed: batch ~10 samples/packet from the NETWORK task at 5 Hz.
* GDAVG currently degrades to n≈1–2/s (multicast-only; this AP drops client-to-client multicast) —
  harmless while shadow. Live build: 86 on both speakers; observer untouched all session (control).
