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
  # audio_timing is required: snapclient AUTO_LOADs it, and a `components:` filter hides
  # anything it does not name. Omit the filter to also get wifi_bssid_select / wifi_tools.
  - source: github://W-Floyd/esphome-snapcast
    components: [snapclient, audio_timing]
  # Forked speaker stack, pending upstream: makes the audio between our push point and the
  # DAC readable, which the sync engine needs. Tracks a release tag; see TODO.md.
  - source:
      type: git
      url: https://github.com/W-Floyd/esphome
      ref: speaker-buffered-bytes
    components: [speaker, i2s_audio, mixer, audio, media_source, speaker_source]

snapclient:
  id: snap
  server: 192.168.1.10        # omit to discover via mDNS

media_source:
  - platform: snapclient
    id: snap_source

speaker:
  - platform: i2s_audio
    id: my_speaker
    sample_rate: 48000        # match the stream: nothing resamples, see below
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

**PCM, FLAC and Opus** — set your stream to `codec=flac`, `codec=pcm` or `codec=opus`.
All three are 16-bit only. Vorbis is unsupported.

Opus is opt-in (`opus: true`), because libopus is the one codec with a real cost: ~87 KB
of flash, and at runtime ~40 KB of decoder state plus a 120 KB pseudostack (tunable down
to 60 KB under the `audio:` component's `codecs: opus:` key). On a board without PSRAM it
does not fit — `opus_decoder_create` left 18 KB free on a bare ESP32 — so treat PSRAM as
a requirement rather than a recommendation there. snapserver also resamples every Opus
stream to 48 kHz stereo.

**Match the sample rate.** Nothing in this path resamples. The rate is negotiated at
runtime from the codec header, so a mismatch is survivable — the speaker is reconfigured
to whatever the server sends — but `speaker_source` discards the write that triggers the
reconfigure, so every stream start pays a pipeline restart, which is the failure mode
[TIMING.md](TIMING.md) traces to silent playback offsets. Set the speaker and
`media_pipeline` to the stream's real format and check it with the `stream_format` text
sensor.

### Options

| Option | Default | Description |
|---|---|---|
| `server` | mDNS | Snapserver host or IP |
| `port` | `1704` | stream port |
| `name` | node name | `HostName` in Hello |
| `buffer_size` | `524288` | decoded PCM buffer, bytes (PSRAM-preferred) |
| `flac` | `true` | compile in FLAC decoding |
| `opus` | `false` | compile in Opus decoding (libopus; wants PSRAM) |
| `time_sync_interval` | `250ms` | while streaming; idle uses `max(this, 2s)` |
| `sync_deadband` | `128us` | median error that engages steering; raise on jittery links |
| `converge_fine` | `2ms` | coarse→fine servo handoff, and whether a hard resync re-mutes; must sit above `2 × sync_deadband` and below `hard_resync_threshold` or convergence cannot finish |
| `hard_resync_threshold` | `50ms` | beyond this, drop chunks / insert silence |
| `pause_behavior` | `allow` | local PAUSE/STOP: `allow`, `resume` (undone once audio flows), `ignore` (refused) |
| `keepalive_hold` | `never` | bridge a chunk gap with silence before letting the stream end; `never` keeps the speaker always ready |
| `stream_idle_timeout` | `3s` | ends the stream when disconnected; while connected `keepalive_hold` applies |
| `channel_mode` | `stereo` | `stereo`, `left`, `right`, `mono` |
| `phase_invert` | `none` | `none`, `left`, `right`, `both` |
| `rate_lock` | off | steer the S3's I2S divider instead of splicing frames; takes `i2s_port` (default `0`) |
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
    type: stream_title               # also stream_artist, stream_album, stream_name
  - platform: snapclient
    type: stream_format              # what the server is really sending, e.g.
                                     # "48000 Hz, 16 bit, 2 ch"; also tsf_role

text:
  - platform: snapclient             # manual "host" or "host:port" server override,
                                     # highest precedence of all the server settings
```

## Design

Two FreeRTOS tasks. The **network** task does TCP/mDNS, the Hello handshake, time sync
and PCM/FLAC/Opus decode into a timestamped PCM ring; a full ring backpressures TCP. The
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

Derived from upstream `sendspin` (architecture), `badaix/snapcast` (protocol ground
truth), `CarlosDerSeher/snapclient` (the control law and channel modes) and
`badaix/snapweb` (the Kalman time filter) — see [NOTICE.md](NOTICE.md).

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
standing in for the absent I2S device. `CODEC=flac|pcm|opus` picks the stream codec; the
opus case builds a separate config, because libopus needs the PSRAM that QEMU emulates
but the default config deliberately does without.

virtual_speaker measures the audio it consumes — zero crossings for the fundamental, peak
for silence — so the harness can tell a correct decode from a plausible-looking wrong one.
Against its 440 Hz source, expect `signal ~440 Hz` and `underruns 0`.

What it does **not** validate is sync accuracy: the median error wanders several ms under
QEMU and does not converge, identically for every codec, because the emulated timebase and
virtual DAC feedback are not the thing being modelled. Use real hardware and
`scripts/raw-sync.py` for that. Give QEMU an idle host, too — under load the guest takes
an interrupt-watchdog panic with both cores parked in the idle task, which is the emulator
missing its deadlines rather than a firmware fault.

## License

[GPLv3](LICENSE). Inherited rather than chosen: ESPHome's C++ runtime is GPLv3, and parts
of the timing engine are ports of GPLv3 code. [NOTICE.md](NOTICE.md) has the details and
the attributions.
