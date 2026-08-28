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

    python3 scripts/i2s-skew.py --stream --interval 0 --count 0 --samples 400000 \
        --plot-every 0.0167 --plot-window 45 --annotate a.log b.log --out test.csv --plot test.svg

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

**MLS44 LOOPS EVERY 20 MINUTES AND EACH LOOP FIRES A ~100 ms HARD RESYNC ON EVERY CLIENT.**
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
