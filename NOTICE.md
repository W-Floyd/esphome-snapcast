# Licensing and attribution

Copyright (c) 2026 William Floyd

This project's source is licensed under the **MIT License** — see [LICENSE](LICENSE).

## What MIT covers, and what it does not

MIT covers the source in this repository: the Snapcast protocol implementation, the
timing engine (`components/clock_sync`), the ESPHome component plumbing, the example
configurations, the test harness and the documentation.

It does **not** change the licence of the firmware you build from it. Every C++ file here
includes ESPHome headers, and ESPHome ships a split licence — the Python codebase is MIT,
while "the C++/runtime codebase (file extensions .c, .cpp, .h, .hpp, .tcc, .ino) are
published under the GPLv3 license". A compiled ESPHome firmware is therefore a combined
work covered by the GPLv3, and distributing that binary carries the GPLv3's obligations
regardless of this repository's own terms.

This is the ordinary arrangement for an ESPHome external component: permissive component
source, copyleft firmware. What it buys is that the source here can be read, lifted and
reused on its own terms — in another project, under another licence, or upstream.

## Prior art and credit

None of the following is a code dependency; the list is credit, not obligation. Where an
implementation here resembles one of these, it is because the same problem has one good
answer, and the source comments say so at each site.

| Source | Relationship |
|---|---|
| [badaix/snapcast](https://github.com/badaix/snapcast) | The protocol. Message layout, Time reply semantics, ClientInfo and effective-buffer composition were verified against the server source — interface facts, implemented here from scratch |
| [CarlosDerSeher/snapclient](https://github.com/CarlosDerSeher/snapclient) | Prior art for the playback control law. The median-filtered error signal, sample stuffing over silence insertion, and mute-until-synced are its ideas; the 128 µs engage threshold is its value. The implementation, the window length, the PI trim and the rate lock are not |
| [esphome/esphome](https://github.com/esphome/esphome) | The component framework. The architecture follows upstream `sendspin` — hub plus children, the `media_source` player contract, codegen idioms |
| [W-Floyd/ImmichFrame-snapweb](https://github.com/W-Floyd/ImmichFrame-snapweb) | Where the Kalman time filter was first written, by this project's author. `components/clock_sync/time_filter.h` is a C++ translation of that work — see below |

The Sage-Husa adaptive measurement noise and the Huber M-estimate weighting in
`time_filter.h` are published methods (Mohamed & Schwarz, 1999), cited in the file.

On the time filter specifically, since it is the one piece here translated from existing
code rather than written for this project. `badaix/snapweb` synchronises with a
`TimeProvider` holding a 100-sample median of `(c2s - s2c) / 2`; it contains no Kalman
filter. The `KalmanTimeFilter` in ImmichFrame-snapweb is a different mechanism written to
replace that one, and both commits touching the file are the author's. That fork is
GPL-3.0, as is its upstream, but an author retains copyright in their own contributions:
publishing this code there under the GPLv3 does not preclude licensing the same work
under MIT here.

## Relicensing note

Releases published before 2026-08-25 were distributed under the GPLv3. Those grants are
irrevocable and remain in force for the versions they were made on: anyone who received
an earlier release keeps GPLv3 rights to it. MIT applies from this commit forward.
