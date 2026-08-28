# HANDOFF — snapclient sync work, as of 2026-08-28 00:30

## Bench layout

| board | role | log | flashed with |
|---|---|---|---|
| `snapclient-supermini-e985e8` | speaker A, **logic-analyser channel A** | `a.log` | `example/esp32-s3-supermini.yaml` |
| `snapclient-supermini-f04d74` | speaker B, **logic-analyser channel B** | `b.log` | `example/esp32-s3-supermini.yaml` |
| `snapclient-observer-e99574`  | TSF observer, drives no DAC | `observer.log` | `example/observer-supermini.yaml` |
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
