"""Clock offset estimation and shared-timebase distribution.

Neither piece knows anything about audio, or about the protocol that feeds it. Clock
STEERING, which is audio-specific, lives in ``i2s_rate_lock``; pairing the two is the
consuming component's job.

- ``KalmanTimeFilter`` -- 2-state (offset + drift) clock filter with Sage-Husa
  adaptive measurement noise and Huber outlier weighting. Takes round-trip samples and
  knows nothing about how they were obtained, nor about audio: header-only standard C++
  with no ESPHome include, so it lifts out of here unchanged and would serve NTP, PTP or
  any two drifting clocks.
- ``TsfSync`` -- 802.11 TSF as a shared timebase for co-located clients: leaderless
  consensus over every node's server→TSF estimate, a slew-limited published line,
  sandwiched TSF reads. Generic distributed clock sync; the only audio in it is the
  naming of two diagnostic hooks (``set_pipeline_us``, ``set_render_phase_us``), which
  mean "how deep is this node's output queue" and "when does it render a known frame".

Neither includes a protocol header or references a protocol type. There is no YAML
config: a consumer requests the optional pieces through the helpers below, which is also
what compiles them in.
"""

import esphome.codegen as cg

CODEOWNERS = ["@W-Floyd"]

clock_sync_ns = cg.esphome_ns.namespace("clock_sync")

KalmanTimeFilter = clock_sync_ns.class_("KalmanTimeFilter")
TsfSync = clock_sync_ns.class_("TsfSync")


def request_tsf_sync() -> None:
    """Compile in TSF group sync (wifi-only; silently inactive without it)."""
    cg.add_define("USE_CLOCK_SYNC_TSF_SYNC", True)
