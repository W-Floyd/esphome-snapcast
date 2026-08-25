"""Hardware sample-clock steering for the ESP32-S3's I2S peripheral.

``RateLock`` drives the I2S MCLK fractional-N divider, so steady-state playback
corrections change the sample clock instead of splicing frames.

This is the audio-specific half of synchronized playback; ``clock_sync`` estimates and
shares the timebase and knows nothing about what is played. There is deliberately no
dependency either way -- this component includes nothing from ``clock_sync``, and pairing
the two is the consuming component's job. A dependency would force anyone who wants I2S
steering to pull in a Kalman filter they never call.

S3-only. The backend pokes ``soc/i2s_struct.h`` registers for the S3's fractional-N
divider; the frame-splice servo remains the fallback everywhere else. Requested through
the helper below rather than YAML -- the user-facing option is ``rate_lock:`` on the
consuming component.
"""

import esphome.codegen as cg

CODEOWNERS = ["@W-Floyd"]

i2s_rate_lock_ns = cg.esphome_ns.namespace("i2s_rate_lock")

RateLock = i2s_rate_lock_ns.class_("RateLock")


def request_rate_lock() -> None:
    """Compile in hardware rate steering (ESP32-S3 only; the backend is variant-gated)."""
    cg.add_define("USE_I2S_RATE_LOCK", True)
