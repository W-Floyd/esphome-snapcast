# esphome-snapcast

A native [Snapcast](https://github.com/badaix/snapcast) client for ESPHome. Implements
the binary protocol directly and plays through ESPHome's own audio stack — so
announcements, volume, mixing and any `speaker` platform work as usual — rather than
side-loading a snapclient firmware's tasks. Structured after upstream `sendspin`: a hub
owning the protocol client, plus a `media_source` platform feeding the
`speaker_source` pipeline.

Components in this repo:

| Component | |
|---|---|
| `snapclient` | the Snapcast client |
| `audio_timing` | clock filter, 802.11 TSF group sync, I2S rate steering — protocol-agnostic |
| `wifi_bssid_select` | preferred-AP picker that falls back rather than stranding the device |
| `wifi_tools` | TX power (with driver readback) and radio diagnostics |

## Requirements

ESP32 (esp-idf), PSRAM strongly recommended. ESPHome new enough for
`media_source` / `speaker_source`.

## Usage

```yaml
external_components:
  - source: github://W-Floyd/esphome-snapcast
    components: [snapclient]

snapclient:
  id: snap
  server: 192.168.1.10        # omit to discover via mDNS

media_source:
  - platform: snapclient
    id: snap_source

speaker:
  - platform: i2s_audio
    id: my_speaker
    sample_rate: 48000        # match the stream; a resampler degrades sync
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

Full example: [example/snapclient-example.yaml](example/snapclient-example.yaml). For a
smart speaker with ducked HA announcements, put the shared config in one file and include
it from thin per-board files via `packages:` + `substitutions:`.

**PCM and FLAC only** — set your stream to `codec=flac` or `codec=pcm`. Opus and Vorbis
use raw non-Ogg framing and need bespoke decoders.

### Options

| Option | Default | Description |
|---|---|---|
| `server` | mDNS | Snapserver host or IP |
| `port` | `1704` | stream port |
| `name` | node name | `HostName` in Hello |
| `buffer_size` | `524288` | decoded PCM buffer, bytes (PSRAM-preferred) |
| `flac` | `true` | compile in FLAC decoding |
| `time_sync_interval` | `250ms` | while streaming; idle uses `max(this, 2s)` |
| `sync_deadband` | `128us` | median error that engages steering; raise on jittery links |
| `hard_resync_threshold` | `50ms` | beyond this, drop chunks / insert silence |
| `pause_behavior` | `allow` | local PAUSE/STOP: `allow`, `resume` (undone once audio flows), `ignore` (refused) |
| `keepalive_hold` | `never` | bridge a chunk gap with silence before letting the stream end; `never` keeps the speaker always ready |
| `stream_idle_timeout` | `3s` | ends the stream when disconnected; while connected `keepalive_hold` applies |
| `channel_mode` | `stereo` | `stereo`, `left`, `right`, `mono` |
| `phase_invert` | `none` | `none`, `left`, `right`, `both` |
| `rate_lock` | off | steer the S3's I2S divider instead of splicing frames |
| `tsf_sync` | `false` | share one server→TSF mapping between same-AP clients |
| `timing_diagnostics` | `false` | per-chunk `RAW` lines for `scripts/raw-sync.py` |
| `static_delay` (media_source) | `0ms` | per-device latency trim |

### Entities

All persisted, and each overrides the matching hub option:

```yaml
select:
  - platform: wifi_bssid_select      # Access Point: a preference, not a hard pin
  - platform: snapclient
    type: channel_mode               # Stereo / Left / Right / Mono
  - platform: snapclient
    type: phase                      # fixes an out-of-phase driver in software
  - platform: snapclient
    type: pause_behavior             # Allow / Resume / Ignore
  - platform: snapclient
    type: server                     # "Automatic" + each mDNS-discovered snapserver

number:
  - platform: snapclient
    type: server_latency             # this client's latency ON the server

text_sensor:
  - platform: snapclient
    type: stream_title               # also stream_artist, stream_album, stream_name, tsf_role

text:
  - platform: snapclient             # manual "host" or "host:port" server override,
                                     # highest precedence of all the server settings
```

## Design

Two FreeRTOS tasks. The **network** task does TCP/mDNS, the Hello handshake, time sync
and FLAC/PCM decode into a timestamped PCM ring; a full ring backpressures TCP. The
**player** task pops chunks, computes each one's deadline
(`server_ts + bufferMs − serverLatency − clock_offset − static_delay`) and steers
playback against the speaker's DAC-write feedback. Events reach the main loop through a
queue, so tasks never touch entities.

Sync comes from a 2-state Kalman clock filter (Sage-Husa adaptive noise, Huber outlier
weighting), a smoothed DAC-feedback pivot, and a median-filtered servo that trims one
frame per chunk by sample stuffing. Playback is muted until first lock, so convergence is
inaudible. Optionally, co-located clients share one 802.11 TSF timebase and steady-state
corrections steer the I2S clock rather than splicing frames.

**[TIMING.md](TIMING.md)** is the real documentation: the clock chain, every buffer stage
and its latency, why the gains are what they are, the measured error budget, and which
quantities the on-device metrics structurally *cannot* see — plus the raw-observation
instrument (`scripts/raw-sync.py`) that can.

Derived from: upstream `sendspin` (architecture), `badaix/snapcast` (protocol ground
truth), esp32 `snapclient`/`lightsnapcast` (the control law, channel modes, volume
curve), and ImmichFrame-snapweb's `snapstream.ts` (the Kalman time filter).

## Flashing

```bash
esphome run example/snapclient-example.yaml --device /dev/ttyACM0   # serial
esphome run example/snapclient-example.yaml --device kitchen.local  # OTA
```

Serial writes at `0x0` and needs `firmware.factory.bin`, not the app image — plain
`esphome run` picks correctly, but passing `--file` with an app image (honoured on the
serial path from esphome 2026.8.0) yields a SHA-256 mismatch and a ~1.3 s watchdog boot
loop. In Docker, mount the repo root so `path: ../components` resolves; Docker Desktop on
macOS cannot pass USB through, so build there and flash from the host.

## Testing without hardware

[tests/run-qemu-test.sh](tests/run-qemu-test.sh) runs the real firmware under QEMU
against a local snapserver, with [virtual_speaker](tests/components/virtual_speaker)
standing in for the absent I2S device. Sync converges to ~1 ms average under QEMU's
imperfect timing; real hardware is far tighter.
