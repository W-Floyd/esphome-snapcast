# Timing architecture

How a chunk of audio gets from a snapserver timestamp to a speaker cone at the
same instant on every device, what each stage contributes, and which quantities
are measurable. Written after a night of hardware debugging on a four-device
fleet; every figure below is measured on that fleet unless marked otherwise.

The short version: there is **one control loop**, it is closed on a **prediction**
of when audio will render, and the single most important property of the whole
design is that *an error in that prediction is invisible to the loop*. Most of
this document exists to explain that sentence and what we do about it.

---

## 1. The clock chain

Four clocks are involved, and every one of them matters:

| Clock | Source | Shared? |
|---|---|---|
| **Server time** | snapserver's monotonic clock; every chunk carries a timestamp | Yes — identical value on every client for the same audio |
| **AP TSF** | the access point's 802.11 timing counter, read via `esp_wifi_get_tsf_time()` | Yes — all clients on one BSS read the same counter |
| **Local time** | this device's `esp_timer` | No |
| **DAC clock** | the I2S bit clock, derived from a PLL through a fractional divider | No |

The job is to render server time *T* at the same physical instant everywhere.
Server time and TSF are shared; local and DAC clocks are not. So the architecture
is: express the deadline in a shared clock, then steer the DAC clock so audio
lands on it.

### Why TSF rather than each device's own estimate

Each client independently estimates its offset from server time by Kalman-filtering
NTP-style exchanges. That estimate wanders by roughly ±100–300 µs, and the wander is
**uncorrelated between devices** — which is precisely the error that moves a stereo
image, since only *relative* timing is audible.

TSF sidesteps this. One elected leader publishes a single mapping from TSF to server
time; every member — leader included — computes deadlines from that *published* line.
The mapping's own error becomes common-mode and cancels between devices. What remains
per-device is only the noise in reading TSF locally, which is much smaller.

Election, failover and the Kalman fallback are documented at the top of
`components/audio_timing/tsf_sync.h`. Measured: the four devices agree on the server-versus-TSF rate to within
2 ppm (all reading −16.7 to −18.5 ppm), which is a good independent check that they
are genuinely sharing one timebase.

### Reading TSF: the sandwich

`esp_wifi_get_tsf_time()` is bracketed by two `esp_timer` reads and the midpoint is
paired with the TSF value. The bracket width is reported alongside every sample.

Measured: the width is **~42–50 µs and highly consistent** (7 µs spread). That is not
jitter — it is the deterministic cost of the API call. Two consequences:

- A threshold below ~42 µs is unachievable. Demanding one only burns retries.
- Because the width is *consistent*, the TSF is latched at a consistent point within
  the call, so the midpoint carries a consistent **bias** — which identical devices
  share, so it cancels. The noise that matters is the *variation* in width, i.e. a
  few µs.

One device in the fleet reads 83 µs median with excursions to 122 µs. Best-of-N
sampling had been hiding that entirely, so the retry count is not a free parameter.

---

## 2. The pipeline, stage by stage

Audio crosses five buffers between the network and the pin. Every one adds latency,
and the total is what the control loop has to predict.

```
snapserver ──network──▶ PCM ring buffer          (client, ~1.7 s)
                          │
                          ▼  player task pushes decoded frames
                        SourceSpeaker ring        (mixer input)
                          │
                          ▼  mixer task
                        output transfer buffer    (50 ms, TRANSFER_BUFFER_DURATION_MS)
                          │
                          ▼
                        i2s_audio ring            (buffer_duration, 100 ms)
                          │
                          ▼
                        I2S DMA descriptors       (5 × ~8 ms ≈ 40 ms)
                          │
                          ▼
                        pin ──▶ DAC ──▶ amp ──▶ driver ──▶ air
```

A chunk is **one codec block**, not a fixed duration of our choosing: with FLAC that
is 1152 frames, measured at 26.2 ms at 44.1 kHz. It follows the encoder, so it changes
with codec and rate — which matters because several loop constants are expressed per
chunk.

### Reporting the fill

The client counts what it pushes and is told what has rendered, so it maintains
`accounted = pushed − played`. That is an **accumulator**: it integrates two counters
and has no self-correcting term, so a frame miscounted once stays miscounted.

Against that we can now query the pipeline for what it actually holds. This required
upstream additions, because every layer computed the number and then discarded it at
the API boundary (`has_buffered_data()` returned `available() > 0`):

- `Speaker::buffered_bytes(size_t &)` — returns `false` when a platform cannot report,
  which is deliberately distinct from reporting zero.
- `i2s_audio` reports its ring **plus the full DMA descriptor span**. The DMA term must
  include silence padding: the lockstep write records count only *real* frames, so
  padding never reaches the played-frames callback, yet it still takes time to clock out.
- `mixer` reports its source queue **plus the output transfer buffer** — which was
  task-local and therefore unreachable, and was exactly the ~50 ms residual left over
  after the DMA was counted.
- `MediaSourceListener::buffered_bytes` carries the query across the same boundary the
  writes cross.

With all stages reported, `accounted` and the observed `fill` agree to within ~10 ms.

---

## 3. The control loop

Per chunk, in the player task:

```
deadline  = server_ts + buffer_ms − latency − shared_offset      (shared clock)
predicted = feedback_pivot_time + (pushed − pivot_frames)/rate   (when it will render)
error     = predicted − deadline
```

`error > 0` means the chunk would play late. The loop drives `error` to zero using, in
order of preference:

1. **Rate lock** (`rate_lock.{h,cpp}`) — steers the I2S MCLK fractional divider, so
   corrections are pure rate changes with no waveform discontinuity. Continuous PI, no
   deadband. This is the steady-state mechanism.
2. **Frame splices** — insert or drop frames when the rate lock is unavailable or the
   error is far outside the fine band. Coarse but fast.
3. **Hard resync** — drop or insert whole chunks beyond `hard_resync_threshold`
   (50 ms). Audible, and used only to recover from a real disturbance.

### The rate lock and its baseline

MCLK = SRC / (N + b/a), with the fraction in a single 32-bit register so it can be
swapped atomically. The integer part is never written live — doing so requires IDF's
"double division" workaround, which bursts MCLK ~6.5× and is unusable on a running
channel.

Trims are *relative to a baseline*, so a wrong baseline is a DC offset the servo must
cancel out of its own authority. For 44.1 kHz × 256 from 160 MHz the ideal divider is
`6250/441 = 14 + 76/441` — **exactly representable**, so where the driver picks a worse
approximation we recompute and use the ideal.

One caution, learned the hard way: after the first trim, the divider register holds
*our own* value, not the driver's. Re-reading it as a "baseline" then reinterprets the
servo's learned offset as driver error. The code detects this by comparing against the
last value it wrote.

### Gains

```
Kp    = 0.5 ppm/µs
Ki    = Kp²/4                              (critically damped; computed, not written)
clamp = clamp(Kp × converge_fine, 500, 2000) ppm
```

The clamp is **derived, not chosen**. The PI takes over at `converge_fine`, so to
behave as a linear controller anywhere in that band the output must be able to express
the proportional term at the handoff: `clamp ≥ Kp × converge_fine`. A fixed 500 ppm
against the 2 ms default violated that by 2×, leaving the upper half of the fine band
saturated *by construction* — every recovery entered the PI stage already railed.

Bandwidth is set by **disturbance tracking, not settling**. The loop trails a ramp by
`rate/Kp`, and lag bounds Kp for phase margin. Measured lag ≈ 0.85 s, dominated by the
feedback-pivot EWMA (α = 1/64 over 10.000 ms callbacks ≈ 0.64 s), with the median
window contributing the rest.

---

## 4. What is measurable, and the blind spot

This is the part worth reading twice.

`error = predicted − deadline` is computed from `predicted`. **If `predicted` is wrong,
the loop steers real audio to the wrong time and reports zero error.** The metric and
the audio are displaced together. A loop closed on a reference cannot see an error in
that reference.

Measured consequence: devices sitting 10 ms and 52 ms out of alignment, plainly audible,
while every on-device metric read clean — medians inside 90 µs, no resyncs, drift small.

So the diagnostic hierarchy is:

| Instrument | Sees | Blind to |
|---|---|---|
| `median` in the sync report | tracking error against *this device's* prediction | any error in the prediction itself |
| `pipeline` / `fill` / `drift` | accumulator vs observed pipeline content | anything downstream of the reported stages |
| `depth ±N ms` (TSF beacon) | this device's depth vs the leader's | a fault shared by both |
| **`raw-sync.py`** | **actual inter-device rendering, from raw observations** | anything past the I2S pin |

### raw-sync.py

The client emits one `RAW` line per sync report containing **only direct
observations** — no servo state, no prediction:

```
RAW s_ts=… pushed=… played=… played_ts=… tsf=… tsf_local=… sw=… rate=…
```

From which:

```
server_time_of_last_rendered_frame = s_ts − (pushed − played) × 1e6 / rate
tsf_time_of_that_frame             = played_ts + (tsf − tsf_local)
```

`(played, played_ts)` is ground truth from the DAC feedback. `(s_ts, pushed)` anchors
the frame count to server audio time, which is the same number on every device for the
same audio. `(tsf, tsf_local)` converts to the one clock the devices provably share.
Two devices in sync satisfy the same linear relation between those quantities; fitting
each and differencing at a common server time gives the real offset.

Two details matter for trusting it:

- **Robust fitting is essential.** Every starvation re-baselines the accounting, which
  steps `pushed − played` and shifts that sample. Plain least squares let a handful of
  steps tilt the line (residuals 440–1220 µs); iterative 2.5σ rejection brings that to
  181–385 µs and tightens confidence 4×.
- **The rate offsets are a free validation.** Server-versus-TSF rate is physically
  common to all devices. Before rejection the four fits scattered +0.2 to +14.5 ppm;
  after, they agree at −16.7 to −18.5 ppm. Four independent fits converging on a shared
  physical quantity is evidence the method measures what it claims.

---

## 5. Error budget

Measured, at 44.1 kHz:

| Term | Contribution | Notes |
|---|---|---|
| TSF read noise | ~±3.5 µs | variation in a ~42 µs deterministic bracket; per-device, does not cancel |
| Published mapping error | ~0 | common-mode by construction — this is what TSF buys |
| Servo residual | ~200 µs, **white noise** | see below |
| Frame quantisation | ~0 with rate lock | 22.7 µs per frame on the splice fallback |
| DAC/amp/driver | fixed | identical hardware, cancels between devices |
| **Speaker placement** | **29 µs per cm** | dominates everything below ~100 µs; trim with `static_delay` |

Achieved inter-device alignment, robust-fit with ±2σ bounds:

```
a-b  −14.2 ±40.0 µs   not significant
a-c  −19.6 ±60.8 µs   not significant
b-c   −5.4 ±59.1 µs   not significant
a-d  −99.0 ±42.9 µs   significant
b-d  −84.7 ±40.4 µs   significant
```

Three devices aligned to within measurement error; one genuinely ~90 µs out, and that
one is also the weakest station on a saturated channel. The measurement floor is ~40 µs,
set by the per-sample residual over ~47 samples — certifying 10 µs needs ~750 samples
(≈45 min of quiet playback) or a lower residual.

### The residual is noise, not lag

This determines which fixes help, and it is easy to get backwards. Consecutive-difference
σ divided by σ came out **1.32–1.43** against √2 ≈ 1.41 for pure white noise. So the
residual is *variance*, not tracking lag.

Therefore: **raise gain and it gets worse** (a faster loop chases noise into the output);
**average harder and it gets better** at no tracking cost, since there is no ramp being
trailed. `MEDIAN_WINDOW` was lengthened 15 → 31 for exactly this reason — the opposite of
an earlier change that shortened it to buy bandwidth on the assumption the residual was lag.

---

## 6. Failure modes worth knowing

**Correcting a measurement artefact displaces real audio.** The accounted-vs-observed
residual was applied as a timing correction. While pipeline stages were unreported that
residual was real unmeasured audio; once all stages were reported it became measurement
offset (non-atomic sampling, quantisation, publish staleness). Applying it then
manufactured relative offsets of 10 ms and 52 ms — the largest audible defect of the
session, introduced while trying to fix that defect. It is now measured and reported but
**not applied**.

**Assuming a fill instead of measuring it.** A starvation re-baseline zeroed the accounting
on the assumption the pipeline was empty. It usually is — DAC feedback gaps of 660–760 ms
confirm a genuine full drain — but the assumption was never checked, and cost three
diagnoses' worth of investigation.

**Per-beacon allowances masquerading as rates.** The mapping slew limit was applied per
beacon, silently coupling tracking speed to the beacon interval, and starving tracking
when a broadcast was late. Now a rate, scaled by the measured interval.

**Constants that encode a relationship.** `Ki` is `Kp²/4`; the trim clamp is
`Kp × converge_fine`; the muted steer size is a *slew rate*, not a frame count (8 frames
is ~6800 ppm at 44.1 kHz and proportionally less as rate rises). Written as literals,
these drift apart silently.

**The ear outperformed every instrument.** Across five separate episodes a listener
identified which device was offset, and its persistence, before any metric agreed —
because the metrics were measured against a corrupted reference. The instruments that
eventually worked were the ones built from raw observations.

---

## 7. Configuration reference

| Setting | Default | Effect on timing |
|---|---|---|
| `sync_deadband` | 128 µs | splice servo engage threshold (disengage at half); the rate-lock PI has no deadband |
| `converge_fine` | 2 ms | coarse→fine handoff, and the derived trim clamp scales from it |
| `hard_resync_threshold` | 50 ms | beyond this, whole chunks are dropped or silence inserted |
| `rate_lock` | opt-in, S3 only | hardware clock steering; splices remain the fallback |
| `tsf_sync` | opt-in | shared timebase; falls back to per-device Kalman when unavailable |
| `static_delay` | 0 | per-device trim — the right tool for speaker placement asymmetry |
| speaker `buffer_duration` | 100 ms | i2s ring depth; bounds recovery cushion |
| speaker/mixer `timeout` | **`never`** | at the 500 ms default a delivery hiccup tears the pipeline down and rebuilds it at a different fill |

Cross-field validation enforces `2 × sync_deadband < converge_fine < hard_resync_threshold`.
Below the lower bound the coarse splices limit-cycle outside the band the unmute gate
requires, and a client can stay muted indefinitely — this has happened.

---

## 8. What this does not solve

Sync is not delivery. Every dropout observed on the fleet had the same signature:
lateness growing ~1 s per second of wall clock with a healthy RSSI, meaning data stopped
arriving outright. The timing architecture shortens the *recovery* from such an event; it
cannot prevent one.

On the reference fleet the cause was airtime, not the clients: fourteen stations on one
2.4 GHz channel at 81 utilization (against 13–14 on every other radio), including two
BLE proxies transmitting at 1–6 Mbps, where under DCF a slow station consumes airtime
far out of proportion to the data it carries.
