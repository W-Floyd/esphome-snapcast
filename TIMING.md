# Timing architecture

How a chunk gets from a snapserver timestamp to a speaker cone at the same instant on
every device, and which quantities are measurable. Every figure is measured on a
four-device fleet unless marked otherwise.

The short version: there is **one control loop**, closed on a **prediction** of when
audio will render, and the most important property of the design is that *an error in
that prediction is invisible to the loop*. Most of this document explains that sentence.

---

## 1. The clock chain

| Clock | Source | Shared? |
|---|---|---|
| **Server time** | snapserver's monotonic clock; every chunk carries a timestamp | Yes — identical on every client for the same audio |
| **AP TSF** | the AP's 802.11 timing counter, via `esp_wifi_get_tsf_time()` | Yes — all clients on one BSS read the same counter |
| **Local time** | this device's `esp_timer` | No |
| **DAC clock** | the I2S bit clock, from a PLL through a fractional divider | No |

Render server time *T* at the same physical instant everywhere: express the deadline in
a shared clock, then steer the DAC clock so audio lands on it.

### Why TSF rather than each device's own estimate

Each client Kalman-filters NTP-style exchanges into an offset estimate that wanders
±100–300 µs, **uncorrelated between devices** — precisely the error that moves a stereo
image, since only *relative* timing is audible.

TSF sidesteps it. One elected leader publishes a single TSF→server mapping; every member,
leader included, computes deadlines from that published line, so the mapping's own error
is common-mode and cancels. What remains per-device is only local TSF read noise.

Election, failover and the Kalman fallback: `components/audio_timing/tsf_sync.h`.
Measured: the four devices agree on the server-versus-TSF rate within 2 ppm (−16.7 to
−18.5 ppm), an independent check that they share one timebase.

### Reading TSF: the sandwich

`esp_wifi_get_tsf_time()` is bracketed by two `esp_timer` reads, midpoint paired with the
TSF value, bracket width reported per sample.

Width is **~42–50 µs, highly consistent** (7 µs spread) — not jitter but the deterministic
cost of the call. So a threshold below ~42 µs is unachievable and only burns retries; and
because the width is consistent the latch point is consistent, making the midpoint bias
common to identical devices. Only the *variation*, a few µs, matters.

One device reads 83 µs median with excursions to 122 µs. Best-of-N sampling hid that
entirely, so retry count is not a free parameter.

---

## 2. The pipeline

Five buffers between network and pin; the total is what the loop must predict.

```
snapserver ──network──▶ PCM ring buffer          (client, ~1.7 s)
                          │  player task pushes decoded frames
                        SourceSpeaker ring        (mixer input)
                          │  mixer task
                        output transfer buffer    (50 ms, TRANSFER_BUFFER_DURATION_MS)
                        i2s_audio ring            (buffer_duration, 100 ms)
                        I2S DMA descriptors       (5 × ~8 ms ≈ 40 ms)
                          ▼
                        pin ──▶ DAC ──▶ amp ──▶ driver ──▶ air
```

A chunk is **one codec block**, not a duration of our choosing: FLAC gives 1152 frames,
measured at 26.2 ms at 44.1 kHz. It follows the encoder, so it changes with codec and
rate — and several loop constants are expressed per chunk.

### Reporting the fill

The client maintains `accounted = pushed − played`. That is an **accumulator**: two
counters, no self-correcting term, so a frame miscounted once stays miscounted.

Against it we can query what the pipeline actually holds. Every layer computed that number
and discarded it at the API boundary (`has_buffered_data()` returned `available() > 0`),
so this needed upstream additions:

- `Speaker::buffered_bytes(size_t &)` — returns `false` when a platform cannot report,
  deliberately distinct from reporting zero.
- `i2s_audio` reports its ring **plus the full DMA descriptor span**, including silence
  padding: lockstep writes record only *real* frames, so padding never reaches the
  played-frames callback, yet still takes time to clock out.
- `mixer` reports its source queue **plus the output transfer buffer** — task-local and
  therefore unreachable, and exactly the ~50 ms residual left after counting DMA.
- `MediaSourceListener::buffered_bytes` carries the query across the boundary the writes
  cross.

With all stages reported, `accounted` and observed `fill` agree within ~10 ms.

---

## 3. The control loop

Per chunk, in the player task:

```
deadline  = server_ts + buffer_ms − latency − shared_offset      (shared clock)
predicted = feedback_pivot_time + (pushed − pivot_frames)/rate   (when it will render)
error     = predicted − deadline
```

`error > 0` means late. The loop drives it to zero, in order of preference:

1. **Rate lock** (`rate_lock.{h,cpp}`) — steers the I2S MCLK fractional divider, so
   corrections are pure rate changes with no waveform discontinuity. Continuous PI, no
   deadband. The steady-state mechanism.
2. **Frame splices** — insert or drop frames where the rate lock is unavailable or the
   error is far outside the fine band. Coarse but fast.
3. **Hard resync** — whole chunks beyond `hard_resync_threshold`. Audible; real
   disturbances only.

### The rate lock and its baseline

MCLK = SRC / (N + b/a), the fraction in one 32-bit register so it swaps atomically. The
integer part is never written live: that needs IDF's "double division" workaround, which
bursts MCLK ~6.5× and is unusable on a running channel.

Trims are *relative to a baseline*, so a wrong baseline is a DC offset the servo must
cancel out of its own authority. For 44.1 kHz × 256 from 160 MHz the ideal divider is
`6250/441 = 14 + 76/441` — exactly representable, so where the driver picks a worse
approximation we recompute and use the ideal.

Learned the hard way: after the first trim the register holds *our* value, not the
driver's. Re-reading it as a baseline reinterprets the servo's learned offset as driver
error. The code detects this by comparing against the last value it wrote.

### Gains

```
Kp    = 0.5 ppm/µs
Ki    = Kp²/4                              (critically damped; computed, not written)
clamp = clamp(Kp × converge_fine, 500, 2000) ppm
```

The clamp is **derived, not chosen**. The PI takes over at `converge_fine`, so to behave
linearly anywhere in that band the output must express the proportional term at the
handoff: `clamp ≥ Kp × converge_fine`. A fixed 500 ppm against the 2 ms default violated
that by 2×, leaving the upper half of the fine band saturated *by construction* — every
recovery entered the PI stage already railed.

Bandwidth is set by **disturbance tracking, not settling**. The loop trails a ramp by
`rate/Kp`, and lag bounds Kp for phase margin. Measured lag ≈ 0.85 s, dominated by the
feedback-pivot EWMA (α = 1/64 over 10.000 ms callbacks ≈ 0.64 s).

---

## 4. What is measurable, and the blind spot

`error = predicted − deadline` is computed from `predicted`. **If `predicted` is wrong,
the loop steers real audio to the wrong time and reports zero error** — metric and audio
displaced together. A loop closed on a reference cannot see an error in that reference.

Measured consequence: devices 10 ms and 52 ms out of alignment, plainly audible, while
every on-device metric read clean — medians inside 90 µs, no resyncs, drift small.

| Instrument | Sees | Blind to |
|---|---|---|
| `median` in the sync report | tracking error against *this device's* prediction | any error in the prediction itself |
| `pipeline` / `fill` / `drift` | accumulator vs observed pipeline content | anything downstream of the reported stages |
| `depth ±N ms` (TSF beacon) | this device's depth vs the leader's | a fault shared by both |
| **`raw-sync.py`** | **actual inter-device rendering, from raw observations** | anything past the I2S pin |

### raw-sync.py

One `RAW` line per sync report, containing **only direct observations** — no servo state,
no prediction:

```
RAW s_ts=… pushed=… played=… played_ts=… tsf=… tsf_local=… sw=… rate=…

server_time_of_last_rendered_frame = s_ts − (pushed − played) × 1e6 / rate
tsf_time_of_that_frame             = played_ts + (tsf − tsf_local)
```

`(played, played_ts)` is ground truth from the DAC feedback; `(s_ts, pushed)` anchors the
frame count to server audio time, the same number on every device for the same audio;
`(tsf, tsf_local)` converts to the one clock the devices provably share. Two synced
devices satisfy the same linear relation; fit each and difference at a common server time.

Two details matter for trusting it:

- **Robust fitting is essential.** Every starvation re-baselines the accounting, stepping
  `pushed − played`. Plain least squares let a handful of steps tilt the line (residuals
  440–1220 µs); iterative 2.5σ rejection gives 181–385 µs and tightens confidence 4×.
- **The rate offsets are a free validation.** Server-versus-TSF rate is physically common
  to all devices. Before rejection the four fits scattered +0.2 to +14.5 ppm; after, they
  agree at −16.7 to −18.5 ppm. Four independent fits converging on a shared physical
  quantity is evidence the method measures what it claims.

---

## 5. Error budget

Measured at 44.1 kHz:

| Term | Contribution | Notes |
|---|---|---|
| TSF read noise | ~±3.5 µs | variation in a ~42 µs deterministic bracket; per-device |
| Published mapping error | ~0 | common-mode by construction — what TSF buys |
| Servo residual | ~200 µs, **white noise** | see below |
| Frame quantisation | ~0 with rate lock | 22.7 µs per frame on the splice fallback |
| DAC/amp/driver | fixed | identical hardware, cancels between devices |
| **Speaker placement** | **29 µs per cm** | dominates everything below ~100 µs; trim with `static_delay` |

Achieved inter-device alignment, robust-fit with ±2σ:

```
a-b  −14.2 ±40.0 µs   not significant
a-c  −19.6 ±60.8 µs   not significant
b-c   −5.4 ±59.1 µs   not significant
a-d  −99.0 ±42.9 µs   significant
b-d  −84.7 ±40.4 µs   significant
```

Three devices aligned within measurement error; one genuinely ~90 µs out, and that one is
the weakest station on a saturated channel. The floor is ~40 µs, set by per-sample
residual over ~47 samples — certifying 10 µs needs ~750 samples (≈45 min of quiet
playback) or a lower residual.

### The residual is noise, not lag

Easy to get backwards, and it determines which fixes help. Consecutive-difference σ
divided by σ came out **1.32–1.43** against √2 ≈ 1.41 for pure white noise. So the
residual is *variance*, not tracking lag.

Therefore **raising gain makes it worse** (a faster loop chases noise into the output) and
**averaging harder makes it better** at no tracking cost, there being no ramp to trail.
`MEDIAN_WINDOW` went 15 → 31 for exactly this reason — reversing an earlier change that
shortened it to buy bandwidth, on the assumption the residual was lag.

---

## 6. Failure modes worth knowing

**Correcting a measurement artefact displaces real audio.** The accounted-vs-observed
residual was applied as a timing correction. While stages went unreported that residual
was real unmeasured audio; once all were reported it became measurement offset
(non-atomic sampling, quantisation, publish staleness). Applying it then manufactured
relative offsets of 10 ms and 52 ms — the session's largest audible defect, introduced
while trying to fix that defect. Now measured and reported but **not applied**.

**Assuming a fill instead of measuring it.** A starvation re-baseline zeroed the
accounting assuming an empty pipeline. It usually is — DAC feedback gaps of 660–760 ms
confirm a genuine drain — but the assumption went unchecked and cost three diagnoses.

**Per-beacon allowances masquerading as rates.** The mapping slew limit was applied per
beacon, silently coupling tracking speed to the beacon interval and starving tracking when
a broadcast was late. Now a rate, scaled by the measured interval.

**Constants that encode a relationship.** `Ki` is `Kp²/4`; the trim clamp is
`Kp × converge_fine`; the muted steer size is a *slew rate*, not a frame count (8 frames
is ~6800 ppm at 44.1 kHz, proportionally less as rate rises). Written as literals, these
drift apart silently.

**The ear outperformed every instrument.** Across five episodes a listener identified
which device was offset, and its persistence, before any metric agreed — because the
metrics were measured against a corrupted reference. The instruments that eventually
worked were built from raw observations.

---

## 7. Configuration reference

| Setting | Default | Effect on timing |
|---|---|---|
| `sync_deadband` | 128 µs | splice servo engage threshold (disengage at half); the rate-lock PI has no deadband |
| `converge_fine` | 2 ms | coarse→fine handoff, and the derived trim clamp scales from it |
| `hard_resync_threshold` | 50 ms | beyond this, whole chunks dropped or silence inserted |
| `rate_lock` | opt-in, S3 only | hardware clock steering; splices remain the fallback |
| `tsf_sync` | opt-in | shared timebase; falls back to per-device Kalman |
| `static_delay` | 0 | per-device trim — the right tool for placement asymmetry |
| speaker `buffer_duration` | 100 ms | i2s ring depth; bounds recovery cushion |
| speaker/mixer `timeout` | **`never`** | at the 500 ms default a delivery hiccup tears the pipeline down and rebuilds it at a different fill |

Cross-field validation enforces `2 × sync_deadband < converge_fine < hard_resync_threshold`.
Below the lower bound the coarse splices limit-cycle outside the band the unmute gate
requires, and a client can stay muted indefinitely — this has happened.

---

## 8. What this does not solve

Sync is not delivery. Every dropout observed had the same signature: lateness growing ~1 s
per second of wall clock with healthy RSSI, meaning data stopped arriving. This
architecture shortens *recovery*; it cannot prevent the event.

On the reference fleet the cause was airtime, not the clients: fourteen stations on one
2.4 GHz channel at 81% utilisation (against 13–14 on every other radio), including two BLE
proxies transmitting at 1–6 Mbps, where under DCF a slow station consumes airtime far out
of proportion to the data it carries.
