# esphome-snapcast

A native [Snapcast](https://github.com/badaix/snapcast) client for ESPHome,
structured after ESPHome's upstream `sendspin` component: a hub that owns the
protocol client, and a `media_source` platform that feeds synchronized PCM into the
standard `speaker_source` media player pipeline.

Unlike wrapper approaches that side-load a complete snapclient firmware's FreeRTOS
tasks, this component implements the Snapcast binary protocol natively and plays
through ESPHome's own audio stack — so announcements, volume, mixing, and any
`speaker` platform all work as usual.

## Features

- Snapcast binary protocol v2: Hello, ServerSettings, CodecHeader, WireChunk, Time,
  ClientInfo (local volume/mute reported back to the server).
- Server auto-discovery via mDNS (`_snapcast._tcp`) when no `server:` is configured.
- **PCM** and **FLAC** streams (FLAC is snapserver's default), decoded with ESPHome's
  own `micro_flac` (esp-audio-libs). `opus`/`vorbis` are not yet supported — set your
  stream to `codec=flac` or `codec=pcm`.
- Clock sync via a 2-state Kalman filter (offset + drift) with Sage-Husa adaptive
  measurement noise and Huber M-estimate outlier rejection — robust against wifi
  latency spikes without manual thresholds.
- Playback sync driven by real DAC feedback: the speaker reports exactly when frames
  were written to the DAC, and the player drops/pads frames (soft, inaudible) or hard
  resyncs (beyond `hard_resync_threshold`) to hold the server's `bufferMs` deadline.
- Volume/mute round-trip: `ServerSettings` drive the media player; local changes go
  back via `ClientInfo`.
- Channel routing — Stereo / Left / Right / Mono (e.g. two devices as a synchronized
  L/R pair) — via a `select` entity or the hub's `channel_mode` option, persisted.
- Volume taper: an optional exponential volume curve (configurable dB range, like the
  esp32 snapclient's) between the Snapcast slider and the speaker gain, exposed as a
  `number` slider entity, persisted. **Leave it at 0 for normal setups**: ESPHome's
  speaker stack is already perceptual (i2s software volume maps the slider linearly
  in dB over a 49 dB range, and audio_dac drivers write dB-scaled registers), so the
  curve is only for output paths with truly linear volume — enabling it on top of the
  built-in taper double-applies the log curve.
- Wifi power save is disabled only while a stream is playing.

## Requirements

- ESP32 (esp-idf), PSRAM strongly recommended (the PCM buffer prefers PSRAM).
- ESPHome new enough to have the `media_source` / `speaker_source` components
  (the sendspin-era audio pipeline).

## Usage

See [example/snapclient-example.yaml](example/snapclient-example.yaml) for a minimal
config.

For a full smart-speaker setup — Snapcast mixed with Home Assistant announcements via a
`mixer` speaker and dual pipelines, so the music ducks under announcements instead of
stopping and stays in sync throughout — put the shared parts in one file and include it
from thin per-board files that supply only the hardware substitutions (pins, flash and
PSRAM size, TX power). ESPHome's `packages:` plus `substitutions:` is all that takes, and
it keeps board differences from multiplying through the audio config.

The short version:

```yaml
external_components:
  - source: github://W-Floyd/esphome-snapcast
    components: [snapclient]

snapclient:
  id: snap
  server: 192.168.1.10

media_source:
  - platform: snapclient
    id: snap_source

speaker:
  - platform: i2s_audio
    id: my_speaker
    sample_rate: 48000   # match your snapserver stream rate
    ...

media_player:
  - platform: speaker_source
    name: Media Player
    media_pipeline:
      speaker: my_speaker
      sources: [snap_source]
      sample_rate: 48000
      num_channels: 2
```

### Options

| Option | Default | Description |
|---|---|---|
| `server` | mDNS discovery | Snapserver host or IP; omit to discover via `_snapcast._tcp` |
| `port` | `1704` | Snapserver stream port |
| `name` | node name | `HostName` sent in Hello; the server's default display name |
| `buffer_size` | `524288` | Decoded PCM buffer in bytes (PSRAM-preferred) |
| `flac` | `true` | Compile in FLAC decoding |
| `time_sync_interval` | `250ms` | Time sync cadence while streaming; idle clients sync at max(this, 2s). Bursts run at connect and stream start |
| `sync_deadband` | `128us` | Median error at which the steering servo engages (reference parity); holds stereo-pair imaging pinned — raise on very jittery links |
| `hard_resync_threshold` | `50ms` | Sync error beyond which chunks are dropped / silence inserted |
| `pause_behavior` | `allow` | What a local PAUSE/STOP does. `allow`: honoured. `resume`: honoured, then undone once audio flows again — survives a "stop everything" automation, at the cost of an audible gap. `ignore`: refused outright, no gap, but a deliberate stop is refused too |
| `keepalive_hold` | `never` | Chunk gap bridged with keepalive silence before the stream may end, so inter-track gaps cost no re-lock. `never` holds the pipeline for the whole session (speaker always ready to play in sync) at the cost of a continuously fed DAC and nonstop TSF beaconing |
| `stream_idle_timeout` | `3s` | No wire chunks for this long ⇒ stream ends. Governs the **disconnected** case; while connected `keepalive_hold` applies |
| `channel_mode` | `stereo` | Boot default routing: `stereo`, `left`, `right`, `mono` |
| `phase_invert` | `none` | Boot default polarity inversion: `none`, `left`, `right`, `both` |
| `static_delay` (media_source) | `0ms` | Per-device latency trim, like `snapclient --latency` |

### Entities

```yaml
select:
  - platform: wifi_bssid_select
    name: Access Point      # "Automatic" + one option per scan-discovered AP of the
                            # connected SSID. A preference, not a hard pin: it falls
                            # back to any AP after 20 s and re-arms on the next
                            # disconnect, so a working link is never dropped to chase
                            # the preferred AP. Its own component -- nothing about it
                            # is Snapcast-specific.
  - platform: snapclient
    type: channel_mode
    name: Channel Mode      # Stereo / Left / Right / Mono, persisted; overrides channel_mode
  - platform: snapclient
    type: pause_behavior
    name: Pause Behavior   # Allow / Resume / Ignore at runtime, persisted; overrides
                           # the hub's pause_behavior default
  - platform: snapclient
    type: phase
    name: Phase             # polarity inversion: None / Left / Right / Both, persisted;
                            # overrides phase_invert (fixes an out-of-phase driver in software)

number:
  - platform: snapclient
    type: volume_curve
    name: Volume Curve      # dB range of the extra volume taper; 0 = off (recommended —
                            # ESPHome speakers already apply a 49 dB perceptual taper)
  - platform: snapclient
    type: server_latency
    name: Server Latency    # this client's latency ON the snapserver (Client.SetLatency
                            # via the control API); server-persisted, stays in sync with
                            # changes made from snapweb or other controllers
```

### Sync accuracy notes

- Run the media pipeline at the stream's native sample rate. The sync engine counts
  frames it pushes against the frames the speaker reports written to the DAC; a
  resampler between the two skews that ratio and degrades (but does not break) sync.
- Trim per-device offsets (DAC group delay etc.) with `static_delay`, or per-client
  latency in the Snapcast server UI.

## Design

### Architecture

Structured like ESPHome's upstream `sendspin` component: `SnapclientHub` (a thin
`Component` adapter) owns a plain `SnapcastClient` core and fans its events out to
child platforms via `CallbackManager`s; children (`media_source`, `select`, `number`)
inherit `SnapclientChild`, pinned one setup step after the hub. The core runs two
FreeRTOS tasks:

- **Network task** — TCP connection (or mDNS discovery), Hello handshake, message
  framing, time sync, FLAC/PCM decode into a timestamped PCM ring buffer
  (PSRAM-preferred). A full ring blocks the task, backpressuring TCP exactly like a
  desktop snapclient.
- **Player task** — pops timestamped chunks, computes each chunk's local playout
  deadline (`server_ts + bufferMs − serverLatency − clock_offset − static_delay`),
  and pushes PCM into the `speaker_source` pipeline, steering playback against the
  speaker's DAC-write feedback.

All events cross to the main loop through a queue drained in `hub.loop()`; tasks
never touch ESPHome entities directly.

### Synchronization

- **Clock offset**: Snapcast Time messages (250 ms cadence while streaming, burst at
  connect/stream-start, RTT-gated against congestion outliers) feed a 2-state Kalman
  filter — offset + drift with Sage-Husa adaptive measurement noise and Huber
  M-estimate outlier weighting.
- **Playout position**: the speaker reports DAC writes as (frames, timestamp); an
  exponentially-weighted pivot extrapolated along the exact nominal sample rate
  smooths the DMA-burst quantization to microsecond scale.
- **Steering** (ported from the reference esp32 snapclient's control law): a
  median-filtered error drives a bang-bang servo with hysteresis — engage at
  `sync_deadband` (128 µs), trim one frame per chunk using sample stuffing (repeat
  the last frame; never insert silence mid-music), disengage at half. Larger errors
  correct proportionally (>10 ms) or by hard resync (>50 ms, dropping whole chunks /
  inserting silence). Playback stays **muted until the first in-band lock** (~1–2 s,
  steering hard through silence), so convergence is inaudible; hard resyncs re-mute,
  turning recovery storms into silent gaps.
- **Pipeline-flush re-baseline**: a starved-then-restarted speaker pipeline discards
  pushed-but-unplayed frames; a >500 ms feedback gap re-baselines the frame
  accounting, preventing a permanent hard-resync spiral.
- **Bridged chunk gaps**: a gap is filled with silence for up to 2 minutes, so the
  speaker/mixer never hit their no-data timeout across an inter-track gap. Ending the
  stream tears the pipeline down, and rebuilding playout phase costs a mute plus
  7–16 s of re-lock — measured on ordinary 17–18 s gaps. Beyond the hold the stream
  ends normally: holding it open across a 7.5 h overnight silence was *worse* than the
  teardown, resuming with a 6.9 h stale deadline on every device at once.

A planned v2 ([PLAN-rate-lock.md](PLAN-rate-lock.md)) replaces steady-state frame
splices with hardware rate steering via the S3's fractional I2S clock divider.

**[TIMING.md](TIMING.md)** documents the whole timing architecture as built and
measured on hardware: the four-clock chain, every buffer stage and its latency, the
control loop and why its gains are what they are, the measured error budget, and —
most usefully — which quantities the on-device metrics structurally *cannot* see, plus
the raw-observation instrument (`scripts/raw-sync.py`) that can.

### Key sources

- `esphome/components/sendspin` — architecture template: hub + children, the
  media_source player contract, codegen idioms.
- `badaix/snapcast` — binary protocol ground truth (message layout, Time reply
  semantics, ClientInfo, effective-buffer composition), verified against server
  source.
- esp32 `snapclient` (`lightsnapcast`) — the sync control law (median filters,
  128/64 µs steering thresholds, sample stuffing, mute-until-synced), channel modes,
  and the volume curve.
- ImmichFrame-snapweb's `snapstream.ts` — the Sage-Husa/Huber Kalman time filter
  (itself derived from esp32 snapclient's `TimeFilter.c`), ported to C++ in
  `time_filter.h`.

## Flashing

```bash
esphome run example/snapclient-example.yaml --device /dev/ttyACM0   # serial
esphome run example/snapclient-example.yaml --device kitchen.local  # OTA
```

**Serial flashing needs the factory image**, not the app image. Serial writes at offset
`0x0`, so it needs `firmware.factory.bin` (bootloader + partition table + app); handing
it the app-only image puts an app where the bootloader belongs, and the ROM then loads
it, fails the image SHA-256 check, prints "Attempting to boot anyway" and watchdog-loops
at roughly 1.3 s per cycle until a correct flash recovers it. Plain `esphome run` picks
the right artifact on its own — the trap is passing `--file` with an app image, which
esphome honours on the serial path from 2026.8.0 onward.

Docker note: mount the **repo root**, so `external_components: path: ../components`
resolves — `docker run --rm -v "$PWD":/config ghcr.io/esphome/esphome run
example/snapclient-example.yaml`. On Linux add `--device=/dev/ttyACM0` for serial.
Docker Desktop on macOS cannot pass USB through, so build in Docker and flash from the
host, use https://web.esphome.io with `firmware.factory.bin`, or flash once over USB and
use OTA after that.

## Testing without hardware

[tests/run-qemu-test.sh](tests/run-qemu-test.sh) runs the real firmware on an emulated
ESP32 (espressif QEMU, OpenCores ethernet) against a local snapserver streaming a
FLAC sine wave. QEMU has no I2S device, so
[tests/components/virtual_speaker](tests/components/virtual_speaker) stands in for the
DAC: it consumes audio at exactly the sample rate and provides the same
`audio_output_callback` feedback an I2S speaker does. Watch the serial log for the
periodic `Sync error: avg X us` line — it converges to ~1 ms average / <10 ms peak
under QEMU's imperfect timing (real hardware is tighter).

## Not yet implemented

- Opus / Vorbis codecs (Snapcast uses raw non-Ogg framing; needs bespoke decoders).
- Retargeting servers at runtime via `snapcast://host:port` URIs.
