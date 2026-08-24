# Licensing and attribution

Copyright (c) 2026 William Floyd

This project is licensed under the **GNU General Public License v3.0** — see
[LICENSE](LICENSE).

GPLv3 is inherited rather than chosen. Three independent reasons, any one of which
would be sufficient on its own:

**ESPHome's C++ is GPLv3.** ESPHome ships a split license: the Python codebase is MIT,
while "the C++/runtime codebase (file extensions .c, .cpp, .h, .hpp, .tcc, .ino) are
published under the GPLv3 license". Every C++ file here includes ESPHome headers and is
compiled into an ESPHome firmware, so it is a derivative work of GPLv3 code. This is
true of any ESPHome external component with a C++ half.

**Parts of the timing engine are ports of GPLv3 code**, listed below. A translation into
another language is a derivative work, not a clean-room reimplementation.

**The protocol implementation was written against a GPLv3 reference**, `badaix/snapcast`,
used as ground truth for message layout and semantics.

## Third-party code

| Source | License | Used for |
|---|---|---|
| [badaix/snapcast](https://github.com/badaix/snapcast) | GPL-3.0 | Binary protocol ground truth — message layout, Time reply semantics, ClientInfo, effective-buffer composition — verified against the server source |
| [badaix/snapweb](https://github.com/badaix/snapweb) | GPL-3.0 | The Sage-Husa / Huber Kalman time filter, ported from `snapstream.ts` to C++ in `components/audio_timing/time_filter.h` |
| [CarlosDerSeher/snapclient](https://github.com/CarlosDerSeher/snapclient) | GPL-3.0 | The playback control law — median filters, the 128/64 µs steering thresholds, sample stuffing, mute-until-synced — and channel-mode routing |
| [esphome/esphome](https://github.com/esphome/esphome) | GPL-3.0 (C++) / MIT (Python) | The component framework; the architecture follows upstream `sendspin` (hub + children, the `media_source` player contract, codegen idioms) |

Snapweb's filter is itself derived from the esp32 snapclient's `TimeFilter.c`, so those
two entries share an ancestor.

## What this means in practice

GPLv3 permits commercial use. Anyone consuming this component via `external_components`
is building their own firmware, which is the case the GPL is written for, and no
obligation is triggered by simply using it.

The obligation appears on **distribution** of a binary or a modified version: recipients
must be able to get the corresponding source under the same terms. The one thing it
forecloses is shipping a closed-source product containing this code — which was never
available to grant, given what it derives from.
