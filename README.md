# snapclient-esphome

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
- Wifi power save is disabled only while a stream is playing.

## Requirements

- ESP32 (esp-idf), PSRAM strongly recommended (the PCM buffer prefers PSRAM).
- ESPHome new enough to have the `media_source` / `speaker_source` components
  (the sendspin-era audio pipeline).

## Usage

See [example/snapclient-example.yaml](example/snapclient-example.yaml) for a minimal
config, and [example/esp32-s3-supermini.yaml](example/esp32-s3-supermini.yaml) for the
full smart-speaker pattern on real hardware (Speakeasy OTS pinout) — Snapcast mixed
with Home Assistant announcements via a `mixer` speaker and dual pipelines, with the
music ducking under announcements while staying in sync. The short version:

```yaml
external_components:
  - source: github://W-Floyd/snapclient-esphome
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
| `time_sync_interval` | `1s` | Steady-state time sync cadence (a fast burst runs at connect) |
| `hard_resync_threshold` | `50ms` | Sync error beyond which chunks are dropped / silence inserted |
| `stream_idle_timeout` | `3s` | No wire chunks for this long ⇒ stream ends |
| `static_delay` (media_source) | `0ms` | Per-device latency trim, like `snapclient --latency` |

### Sync accuracy notes

- Run the media pipeline at the stream's native sample rate. The sync engine counts
  frames it pushes against the frames the speaker reports written to the DAC; a
  resampler between the two skews that ratio and degrades (but does not break) sync.
- Trim per-device offsets (DAC group delay etc.) with `static_delay`, or per-client
  latency in the Snapcast server UI.

## Design

Key sources this is modeled on / ported from:

- `esphome/components/sendspin` (architecture: hub + children, media_source player).
- Snapcast binary protocol per `badaix/snapcast` and the esp32 `snapclient` project's
  `lightsnapcast` component.
- The Kalman time filter is a C++ port of the Sage-Husa/Huber filter from
  ImmichFrame-snapweb's `snapstream.ts` (itself derived from esp32 snapclient's
  `TimeFilter.c`).

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
