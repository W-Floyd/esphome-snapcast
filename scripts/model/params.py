"""Measured constants of the snapclient timing system, in one place.

Every value here is either read off the firmware (file:line given) or measured on the
four-device bench and recorded in TIMING.md / TODO.md. Nothing is invented: if a number
has no provenance comment it does not belong in this file.

Units: microseconds and frames unless a name says otherwise. ppm is parts per million of
CLOCK RATE (positive = running fast).
"""

from __future__ import annotations

from dataclasses import dataclass, field

# ---------------------------------------------------------------- audio / chunking

SAMPLE_RATE = 44100                      # bench stream MLS44
CHUNK_FRAMES = 1152                      # FLAC block; TIMING.md sec.2 ("26.2 ms at 44.1 kHz")
CHUNK_US = CHUNK_FRAMES * 1e6 / SAMPLE_RATE          # 26122 us
FRAME_US = 1e6 / SAMPLE_RATE                         # 22.68 us -- the splice quantum

# ---------------------------------------------------------------- pipeline depths

# TIMING.md sec.2. Only the total matters to the loop; the split matters to which
# instrument can see what.
PCM_RING_US = 1_700_000.0                # client-side decoded PCM ring (reported "buffered")
SOURCE_RING_US = 0.0                     # mixer source queue: small, folded into xfer below
XFER_BUFFER_US = 50_000.0                # TRANSFER_BUFFER_DURATION_MS, task-local
I2S_RING_US = 100_000.0                  # speaker buffer_duration
DMA_US = 40_000.0                        # 5 x ~8 ms descriptors
# What sits between "pushed" and the pin. This is the lever arm every rate error is
# multiplied by to become a phase error, so it is the single most load-bearing number
# in the instrument model below.
PIPELINE_US = XFER_BUFFER_US + I2S_RING_US + DMA_US   # ~190 ms; logs report 250-300 ms

# ---------------------------------------------------------------- DAC feedback

FEEDBACK_INTERVAL_US = 10_000.0          # played-frames callback cadence (logs: 10.000 ms)
FB_ALPHA = 1.0 / 64.0                    # snapcast_client.cpp:1108 pivot EWMA
FB_MIN_SAMPLES = 8                       # below this, prediction uses the raw pair
# Feedback lag the pivot EWMA costs: (1/alpha - 1)/2 callbacks is the mean age of the
# EWMA's mass -> ~0.32 s; TIMING.md quotes ~0.64 s for the full time constant and ~0.85 s
# for the loop including the median window.
FEEDBACK_LAG_US = (1.0 / FB_ALPHA) * FEEDBACK_INTERVAL_US    # 0.64 s

# ---------------------------------------------------------------- servo

MEDIAN_WINDOW = 31                       # snapcast_client.h:437
TRIM_KP_ACQUIRE = 0.5                    # ppm per us, snapcast_client.cpp:206
TRIM_KP_RUN = 0.25                       # ppm per us, snapcast_client.cpp:207
TRIM_CLAMP_MIN_PPM = 500.0
TRIM_CLAMP_MAX_PPM = 2000.0
SYNC_DEADBAND_US = 128.0                 # splice engage
CONVERGE_FINE_US = 2_000.0
HARD_RESYNC_US = 50_000.0
SPLIT_RAMP_US_PER_S = 100.0              # inject_split ramp rate (used by the bench tests)


def trim_clamp_ppm(converge_fine_us: float = CONVERGE_FINE_US, kp: float = TRIM_KP_ACQUIRE) -> float:
    """The derived clamp: the PI must be able to express Kp*converge_fine at the handoff."""
    return min(max(kp * converge_fine_us, TRIM_CLAMP_MIN_PPM), TRIM_CLAMP_MAX_PPM)


def trim_ki(kp: float) -> float:
    """Critically damped: Ki = Kp^2/4 (snapcast_client.cpp:2711)."""
    return kp * kp / 4.0


# ---------------------------------------------------------------- TSF / consensus

BEACON_INTERVAL_US = 1_000_000.0         # tsf_sync.cpp:75, every device
CONSENSUS_INTERVAL_US = 500_000.0        # tsf_sync.cpp:82
TMS_SLEW_US_PER_S = 50.0                 # publish slew, tsf_sync.cpp:170
TMS_SLEW_CATCHUP_US_PER_S = 300.0
TMS_CATCHUP_THRESHOLD_US = 1_000.0
MAP_SLEW_US_PER_S = 2 * TMS_SLEW_US_PER_S            # adoption slew, tsf_sync.cpp:194
MAP_SLEW_CATCHUP_US_PER_S = 2 * TMS_SLEW_CATCHUP_US_PER_S
MAP_SNAP_US = 20_000.0                   # above this the timebase re-anchors (steps)
PEER_MAP_STALE_US = 5_000_000.0
CONSENSUS_REWEIGHT_K = 2.0               # w = 1/(1+(d/(k*scale))^2)
CONSENSUS_SCALE_FLOOR_US = 50.0
CONSENSUS_PHASE_SCALE_FLOOR_US = 20.0
OFFSET_EWMA_ALPHA = 1.0 / 256.0          # shared_server_offset_us low-pass
OFFSET_SNAP_US = 2_000.0
PHASE_PAIR_WINDOW_US = 300_000.0
PHASE_STALE_US = 15_000_000.0

# TSF read sandwich: TIMING.md sec.1. The WIDTH is deterministic cost, the VARIATION is
# the noise that reaches the timebase.
SANDWICH_WIDTH_US = 46.0
SANDWICH_JITTER_US = 3.5

# ---------------------------------------------------------------- clock hardware

# Server-vs-TSF rate, measured identically by all four devices (TIMING.md sec.1).
TSF_VS_SERVER_PPM = -17.6
# Per-device crystal spread against TSF, from the boards' own `mine +NN ppm` reports.
CRYSTAL_PPM = {"a": 44.453, "b": 39.346}
# Kalman offset estimate wander, per device and uncorrelated -- the error TSF exists to
# make common-mode (TIMING.md sec.1: "+-100-300 us").
KALMAN_WANDER_SD_US = 150.0
KALMAN_WANDER_TAU_S = 12.0               # ~100 us/s of movement at that sd

# ---------------------------------------------------------------- measured reference

# Numbers the model is scored against. Source in the comment; these are the bench, not
# the model, and must never be edited to make a model look better.
@dataclass(frozen=True)
class Reference:
    name: str
    value: float
    unit: str
    source: str


REFERENCE = [
    Reference("wire skew median, leaderless 3-device", 0.47, "us", "TODO.md Sync, n=10726"),
    Reference("wire skew sd, leaderless 3-device", 8.06, "us", "TODO.md Sync, n=10726"),
    Reference("wire skew MAD, leaderless 3-device", 4.03, "us", "TODO.md Sync"),
    Reference("wire skew sd, 30 s slices", 5.0, "us", "TODO.md Sync (3.49-10.48, typ 3.5-6)"),
    Reference("wire skew sd, leader 2-device pinned", 3.58, "us", "TODO.md core affinity, n=4871"),
    Reference("wire skew median, leader 2-device pinned", 4.47, "us", "TODO.md core affinity"),
    Reference("wire skew sd, unpinned 2-device", 6.24, "us", "TODO.md core affinity, n=4949"),
    Reference("render_phase floor", 20.0, "us", "TODO.md: additive 10-30 us"),
    Reference("render_phase ratio at 500 ms", -1.0000, "-", "TODO.md latency step campaign"),
    Reference("render_phase ratio at 25 us", -0.129, "-", "TODO.md post-restore residual"),
    Reference("render_delta residual vs planted", 8.7, "us", "TODO.md n=9 campaign, +-3.4"),
    Reference("wander r(wire, on-device)", -0.98, "-", "TODO.md 30 s buckets, n=9"),
    Reference("trim p2p within 4 min", 90.0, "ppm", "TODO.md: A +10.5..+99.7, B +10.8..+93.8"),
    Reference("differential trim sd", 2.631, "ppm", "TODO.md analyser d_sd_ppm"),
    Reference("crystal delta B-A", -5.1, "ppm", "TODO.md: mine +44.453 vs +39.346"),
    Reference("loop measurement lag", 850_000.0, "us", "TIMING.md sec.3"),
    Reference("servo residual", 200.0, "us", "TIMING.md sec.5, white"),
    Reference("consecutive-diff sd ratio", 1.37, "-", "TIMING.md sec.5 (1.32-1.43 vs sqrt2)"),
]


@dataclass
class DeviceParams:
    """One board."""
    name: str
    crystal_ppm: float                   # its clock vs TSF
    pipeline_us: float = PIPELINE_US     # what sits between pushed and the pin
    kp: float = TRIM_KP_RUN
    sandwich_jitter_us: float = SANDWICH_JITTER_US
    kalman_wander_sd_us: float = KALMAN_WANDER_SD_US
    static_delay_us: float = 0.0
    # Deliberate, per-device model faults, for the experiments:
    accounting_bias_frames: float = 0.0  # persistent +-N frame miscount (the 22.7 us candidate)
    feedback_interval_us: float = FEEDBACK_INTERVAL_US


@dataclass
class SimParams:
    devices: list = field(default_factory=list)
    buffer_ms: float = 1000.0            # server bufferMs
    duration_s: float = 240.0            # a bench window
    seed: int = 1
    leaderless: bool = True              # False = one device publishes, others adopt verbatim
    render_align_gain: float = 0.0       # 0 = correction disabled, as on the bench
