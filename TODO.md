# TODO

## Persistent control session — DONE (control_session.{h,cpp})

Implemented as specified: persistent non-blocking JSON-RPC session on 1705 tied
to the stream session's lifecycle, `Server.GetStatus` + live notifications,
metadata text sensors (`stream_title` / `stream_artist` / `stream_album` /
`stream_name`), `Client.SetLatency` routed through it with one-shot fallback.
Bonus: the TSF unicast peer roster rides the session (live, non-blocking),
replacing the blocking off-stream fetch except as a control-port-disabled
fallback. Verified degrading gracefully in QEMU with `[tcp] enabled = false`.

## Other candidates (unordered)

- **Hardware rate lock** — see [PLAN-rate-lock.md](PLAN-rate-lock.md).
- **Upstream `speaker::set_rate_adjustment()` / timed-play API** — Satellite1's
  forked `I2SAudioSpeaker::sync_play()` is independent evidence of demand; a shared
  upstream interface beats both projects carrying private mechanisms.
- **Opus codec** — Snapcast uses raw (non-Ogg) Opus framing; would need a bespoke
  decode path (esp-audio-libs' opus decoder expects Ogg).
- **Crossfade on servo frame drops** — drops are the last (near-inaudible)
  discontinuity class; a 2-4 sample crossfade at the splice point would zero it.
  Likely moot if rate lock lands.
- **Runtime server retargeting** — `snapcast://host:port` URIs currently warn;
  needs thread-safe reconfiguration of the network task's target.
- **Encoded-FLAC buffering** — buffer compressed chunks and decode just-in-time in
  the player task, halving PSRAM needs (Satellite1 validates feasibility); only
  worth it if a PSRAM-less board variant matters.
