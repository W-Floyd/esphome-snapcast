# TODO

## Persistent control session (metadata + control)

Replace the one-shot `Client.SetLatency` socket with a persistent JSON-RPC
connection to the server's control port (1705), following the pattern observed in
FutureProofHomes Satellite1's `dev-snapcast` branch (`SnapcastControlSession`):

- Keep one connection open alongside the stream; subscribe to notifications.
- `Server.GetStatus` on connect (and on `Stream.OnUpdate` / group-change
  notifications) to learn this client's group, its stream, and the stream's
  metadata.
- Surface stream metadata as entities: `text_sensor` platform for track
  title / artist / album (and stream name), updated live from `Stream.OnUpdate`
  properties — the missing piece for display-equipped devices.
- Route `Client.SetLatency` (and any future control calls, e.g. group volume or
  stream switching) through the session instead of per-call sockets.
- Reconnect with backoff, tied to the stream connection's lifecycle; all parsing on
  the network task, entities updated via the existing hub event queue.
- Scope guard: control-port availability is optional (server may disable `[tcp]`);
  everything must degrade gracefully to today's behavior — the Server Latency
  number entity falls back to one-shot sockets or goes unavailable.

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
