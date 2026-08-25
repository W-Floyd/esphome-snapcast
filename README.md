# esphome-snapcast

A native [Snapcast](https://github.com/badaix/snapcast) client for ESPHome. Speaks the
binary protocol directly and plays through ESPHome's own audio stack, so announcements,
volume and mixing work as usual. Structured after upstream `sendspin`: a hub owning the
protocol client, plus a `media_source` feeding the `speaker_source` pipeline.

| Component | |
|---|---|
| `snapclient` | the Snapcast client |
| `clock_sync` | clock filter and 802.11 TSF group sync — no audio in it |
| `i2s_rate_lock` | S3 I2S sample-clock steering, the audio-specific half |
| `wifi_bssid_select` | preferred-AP picker that falls back rather than stranding the device |
| `wifi_tools` | TX power (with driver readback) and radio diagnostics |

ESP32 (esp-idf), PSRAM strongly recommended. ESPHome new enough for `media_source` /
`speaker_source`.

## Usage

```yaml
external_components:
  # snapclient AUTO_LOADs clock_sync, and a `components:` filter hides anything it
  # does not name. Omit the filter to also get wifi_bssid_select / wifi_tools.
  - source: github://W-Floyd/esphome-snapcast
    components: [snapclient, clock_sync]
  # Forked speaker stack, pending upstream: makes the audio between our push point and
  # the DAC readable, which the sync engine needs. Tracks a release tag; see TODO.md.
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
```

Then a normal `i2s_audio` speaker and a `speaker_source` media player whose
`media_pipeline` lists `snap_source`, both at the stream's sample rate.

**[example/snapclient-example.yaml](example/snapclient-example.yaml) is the complete
reference** — every option below, defaults shown commented out.

For a real speaker, [snapclient-base.yaml](example/snapclient-base.yaml) is a working
build with ducked HA announcements, entities and diagnostics, included as a package by
thin per-board files: [esp32-s3-supermini.yaml](example/esp32-s3-supermini.yaml) and
[m5stamps3-bat.yaml](example/m5stamps3-bat.yaml).

## Codecs

`pcm`, `flac` and `opus`, 16-bit only. Vorbis is unsupported.

Opus is opt-in (`opus: true`): ~87 KB of flash, plus ~40 KB of decoder state and a 120 KB
pseudostack at runtime. On a board without PSRAM it does not fit — `opus_decoder_create`
left 18 KB free on a bare ESP32 — so treat PSRAM as a requirement there. snapserver
resamples every Opus stream to 48 kHz stereo.

**Match the sample rate.** Nothing in this path resamples. A mismatch is survivable — the
rate is negotiated from the codec header and the speaker reconfigured — but
`speaker_source` discards the write that triggers the reconfigure, so every stream start
pays a pipeline restart, the failure mode [TIMING.md](TIMING.md) traces to silent
playback offsets. Check with the `stream_format` sensor.

## Options

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
| `rate_lock` | off | steer the S3's I2S divider instead of splicing frames; requires `i2s_audio_id` naming the bus |
| `tsf_sync` | `false` | share one server→TSF mapping between same-AP clients |
| `timing_diagnostics` | `false` | per-chunk `RAW` lines for `scripts/raw-sync.py` |
| `static_delay` (media_source) | `0ms` | per-device latency trim |

**Entities**, all persisted, each overriding the matching option above:
`select` — `channel_mode`, `phase`, `pause_behavior`, `server`;
`number` — `server_latency` (set on the server, not locally);
`text` — manual `host[:port]` override, highest precedence;
`text_sensor` — `stream_format`, `tsf_role`, `stream_name`, `stream_title`,
`stream_artist`, `stream_album`.

## Design

Two FreeRTOS tasks. The **network** task does TCP/mDNS, the Hello handshake, time sync
and PCM/FLAC/Opus decode into a timestamped PCM ring; a full ring backpressures TCP. The
**player** task pops chunks, computes each deadline
(`server_ts + bufferMs − serverLatency − clock_offset − static_delay`) and steers
playback against the speaker's DAC-write feedback. Events reach the main loop through a
queue, so tasks never touch entities.

Sync comes from a 2-state Kalman clock filter (Sage-Husa adaptive noise, Huber outlier
weighting), a smoothed DAC-feedback pivot, and a median-filtered servo trimming one frame
per chunk by sample stuffing. Playback is muted until first lock, so convergence is
inaudible. Optionally, co-located clients share one 802.11 TSF timebase and steady-state
corrections steer the I2S clock rather than splicing.

**[TIMING.md](TIMING.md)** is the real documentation: the clock chain, every buffer stage,
why the gains are what they are, the measured error budget, and which quantities the
on-device metrics structurally *cannot* see — plus `scripts/raw-sync.py`, which can.

Credit to `sendspin` (architecture), `badaix/snapcast` (protocol) and
`CarlosDerSeher/snapclient` (prior art for the control law) — see [NOTICE.md](NOTICE.md).

## Flashing

```bash
esphome run example/snapclient-example.yaml --device /dev/ttyACM0   # serial
esphome run example/snapclient-example.yaml --device kitchen.local  # OTA
```

Serial writes at `0x0` and needs `firmware.factory.bin`, not the app image — plain
`esphome run` picks correctly, but `--file` with an app image (honoured on the serial path
from esphome 2026.8.0) yields a SHA-256 mismatch and a ~1.3 s watchdog boot loop. Docker
Desktop on macOS cannot pass USB through: build there, flash from the host.

## Testing without hardware

[tests/run-qemu-test.sh](tests/run-qemu-test.sh) runs the real firmware under QEMU against
a local snapserver, with [virtual_speaker](tests/components/virtual_speaker) standing in
for the I2S device. `CODEC=flac|pcm|opus` picks the codec; the opus case builds a separate
config, since libopus needs the PSRAM QEMU emulates but the default config does without.

virtual_speaker measures what it consumes — zero crossings for the fundamental, peak for
silence — so the harness distinguishes a correct decode from a plausible-looking wrong
one. Against its 440 Hz source expect `signal ~440 Hz` and `underruns 0`.

It does **not** validate sync: the median wanders several ms and never converges,
identically for every codec, because the emulated timebase and virtual DAC feedback are
not the thing being modelled. Use hardware and `scripts/raw-sync.py`. Give QEMU an idle
host, too — under load the guest takes an interrupt-watchdog panic with both cores parked
in the idle task, which is the emulator missing deadlines, not a firmware fault.

## License

[MIT](LICENSE) for the source here. Note that a firmware built from it is a combined work
with ESPHome's GPLv3 C++ runtime, so distributed binaries still carry GPLv3 obligations —
the usual arrangement for an ESPHome external component. [NOTICE.md](NOTICE.md) has the
details and the credits.
