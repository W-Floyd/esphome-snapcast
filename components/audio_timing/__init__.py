"""Protocol-agnostic audio timing primitives.

Split out of the snapclient component because none of it is Snapcast-specific:

- ``KalmanTimeFilter`` -- 2-state (offset + drift) clock filter with Sage-Husa
  adaptive measurement noise and Huber outlier weighting. Takes round-trip
  samples; knows nothing about how they were obtained.
- ``TsfSync`` -- 802.11 TSF as a shared timebase for co-located clients: leader
  election, a slew-limited published mapping, sandwiched TSF reads.
- ``RateLock`` -- steers the ESP32-S3 I2S MCLK fractional-N divider, so steady-state
  playback corrections change the sample clock instead of splicing frames.

None of these include a protocol header or reference a protocol type, which is
what made the split mechanical. There is no YAML config: a consumer requests the
optional pieces through the helpers below, which is also what compiles them in.
"""

import esphome.codegen as cg

CODEOWNERS = ["@W-Floyd"]

audio_timing_ns = cg.esphome_ns.namespace("audio_timing")

KalmanTimeFilter = audio_timing_ns.class_("KalmanTimeFilter")
TsfSync = audio_timing_ns.class_("TsfSync")
RateLock = audio_timing_ns.class_("RateLock")


def request_rate_lock() -> None:
    """Compile in hardware rate steering (ESP32-S3 only; the backend is variant-gated)."""
    cg.add_define("USE_AUDIO_TIMING_RATE_LOCK", True)


def request_tsf_sync() -> None:
    """Compile in TSF group sync (wifi-only; silently inactive without it)."""
    cg.add_define("USE_AUDIO_TIMING_TSF_SYNC", True)
