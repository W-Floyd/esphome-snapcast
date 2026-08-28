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

## Stimulus — this matters more than anything else

The boards play **MLS44**, a maximum-length-sequence stimulus, not music. On music the analyser
cannot resolve this problem at all: adjacent frames correlate at ~0.997, so whole-frame errors
masquerade as findings. On MLS the runner-up correlation is ~0.03.

    server:  192.168.1.2, snapserver in docker, config /mnt/fast/dockge/snapcast/config/
    stream:  MLS44, file:///data/mls44.pcm, added at runtime via Stream.AddStream (loops)
    group:   4eb19e5e — A, B and the observer, all latency 0
    restore: scratchpad/group-orig.json has the original grouping

`scripts/test-signal.py --self-test` proves the sidelobe level; regenerate with `--out-file`.

## Before measuring anything

    python3 <scratchpad>/preflight.py     # refuses unless both boards are latency 0, same stream

A campaign killed mid-cycle once left a board at `latency=500 ms` server-side and every
measurement afterwards was contaminated — including one reported as a finding. Campaign scripts
now `trap` to restore on exit; check anyway.

## State of the work

**Achieved, sustained:** ~+4.5 µs median, sd 3.6 between the pair, correction **disabled**. The
wins were all removing interference, not adding control — stream-scoped TSF leadership,
phase-only follower beacons (sd 81.6 → 4.62), CPU1 pinning (sd 6.24 → 3.58).

**`render_align` is disabled and unproven** (`render_align_max: 0ms`). It now fails for one
understood reason: median-of-three is discontinuous — measured hopping ±96 µs while the
underlying data sat at ±12. Fix is to average for small groups.

**Next, and preferred:** `PLAN-leaderless.md` — consensus averaging instead of a leader. Four of
this session's bugs were downstream of "there is a leader and it changes".

`TODO.md` carries the full record, including retractions. Two findings were reported and later
overturned; both are struck through rather than deleted, because how they happened is useful.

## Traps that cost real time

* **`a.log`/`b.log` span days and carry no date.** `grep "^\[13:5"` matches a previous day's build.
  Anchor on file position (`tail -N`), never on timestamps.
* **Verify the perturbation happened.** Three campaigns measured nothing: `execute_service` is a
  coroutine and silently did nothing unawaited; 1/5/100 ms latency steps are absorbed without a
  resync; `inject_starvation` never trips the storm. Only a **500 ms** step forces a mute.
* **`esphome` lives at a versioned Cellar path** — resolve it as
  `$(head -1 "$(command -v esphome)" | sed 's|^#!||')`; a brew upgrade broke it mid-session.
* **Commit before flashing.** A `git checkout` bundled into a flash command destroyed 20 minutes
  of uncommitted work.
* **Log lines truncate.** `pad=` at the end of `RECON` reads as `pad=882` for a counter in the
  tens of millions; `tsf=` is cut off entirely. Add a short dedicated line instead.
