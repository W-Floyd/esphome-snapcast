"""Clock estimation, sharing and steering for synchronized playback.

Grouped by CONSUMER -- these are the pieces the snapclient servo needs -- rather than by
category, which is why they differ in how general they are. Only the last is audio:

- ``KalmanTimeFilter`` -- 2-state (offset + drift) clock filter with Sage-Husa
  adaptive measurement noise and Huber outlier weighting. Takes round-trip samples and
  knows nothing about how they were obtained, nor about audio: header-only standard C++
  with no ESPHome include, so it lifts out of here unchanged and would serve NTP, PTP or
  any two drifting clocks.
- ``TsfSync`` -- 802.11 TSF as a shared timebase for co-located clients: leader
  election, a slew-limited published mapping, sandwiched TSF reads. Generic distributed
  clock sync; the only audio in it is the naming of two hooks
  (``set_playout_healthy``, ``set_pipeline_ms``), both of which mean "is this node's
  output on time" and "how deep is its output queue".
- ``RateLock`` -- steers the ESP32-S3 I2S MCLK fractional-N divider, so steady-state
  playback corrections change the sample clock instead of splicing frames. Genuinely
  audio-specific: it is an I2S peripheral driver, and I2S is an audio bus.

None of these include a protocol header or reference a protocol type, which is
what made the split mechanical. There is no YAML config: a consumer requests the
optional pieces through the helpers below, which is also what compiles them in.
"""

import esphome.codegen as cg

CODEOWNERS = ["@W-Floyd"]

clock_sync_ns = cg.esphome_ns.namespace("clock_sync")

KalmanTimeFilter = clock_sync_ns.class_("KalmanTimeFilter")
TsfSync = clock_sync_ns.class_("TsfSync")
RateLock = clock_sync_ns.class_("RateLock")


def request_rate_lock() -> None:
    """Compile in hardware rate steering (ESP32-S3 only; the backend is variant-gated)."""
    cg.add_define("USE_CLOCK_SYNC_RATE_LOCK", True)


def request_tsf_sync() -> None:
    """Compile in TSF group sync (wifi-only; silently inactive without it)."""
    cg.add_define("USE_CLOCK_SYNC_TSF_SYNC", True)
