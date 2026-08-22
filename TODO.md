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
- **Upstream: `speaker_source` media player spins forever on an unmixable
  announcement.** A format mismatch cannot be resolved by retrying, but that is
  what it does. Observed with an announcement pipeline at 48000/mono against a
  44100/stereo stream (our own misconfiguration, since fixed): the mixer set
  `Error flag: Incompatible audio streams` once, and the player then stayed in
  `ANNOUNCING` allocating a fresh 9600-byte ring buffer ~16x/second — 1742 of them
  in under two minutes, error flag never cleared, still going until the device was
  rebooted. Two defects: unbounded allocation on an unsatisfiable retry, and no
  terminal failure for the announcement. The log flood is a third-order harm that
  matters on a congested link, since streamed logs compete with audio for the
  radio. Should fail the announcement (and clear the flag) when the mixer reports
  an incompatible format, rather than looping.
- **Upstream: expose the speaker's queued frame count.** `speaker::Speaker` offers
  only `virtual bool has_buffered_data() const` — a bool, not a count — so the
  fill level between our feedback point and the DAC is unobservable. That is the
  root of the silent-offset class of bug: after a starvation the accounting can
  re-baseline against a wrong fill and settle playing ~100-250 ms out while every
  sync metric reads ~0, because the error is measured against the same corrupted
  prediction. Both re-baseline paths currently infer the fill by different
  assumptions instead of reading it. A `size_t buffered_frames()` would let them
  use ground truth and retire the whole class. Same PR territory as
  `set_rate_adjustment()`.
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
