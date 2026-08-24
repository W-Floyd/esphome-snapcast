"""Snapcast client hub component, modeled on ESPHome's sendspin component."""

import esphome.codegen as cg
from esphome.components import audio, audio_timing, network, ota, socket, wifi
from esphome.components.esp32 import only_on_variant
from esphome.components.esp32.const import VARIANT_ESP32S3
import esphome.config_validation as cv
from esphome.const import CONF_BUFFER_SIZE, CONF_ID, CONF_NAME, CONF_NEVER, CONF_PORT
from esphome.types import ConfigType

CODEOWNERS = ["@W-Floyd"]
DEPENDENCIES = ["network"]
# audio: micro decoder libraries (FLAC, Opus); json: ArduinoJson for hello/settings payloads;
# ring_buffer: the decoded-PCM buffer between the network and player tasks;
# mdns: server auto-discovery via _snapcast._tcp when no server is configured.
# audio_timing: the protocol-agnostic clock filter / TSF sync / I2S rate lock,
# which this component drives but does not own
AUTO_LOAD = ["audio", "audio_timing", "json", "mdns", "ring_buffer"]
DOMAIN = "snapclient"

CONF_SERVER = "server"
CONF_SNAPCLIENT_ID = "snapclient_id"
CONF_FLAC = "flac"
CONF_OPUS = "opus"
CONF_TIME_SYNC_INTERVAL = "time_sync_interval"
CONF_HARD_RESYNC_THRESHOLD = "hard_resync_threshold"
CONF_STREAM_IDLE_TIMEOUT = "stream_idle_timeout"
CONF_CHANNEL_MODE = "channel_mode"

snapclient_ns = cg.esphome_ns.namespace("snapclient")
SnapclientHub = snapclient_ns.class_("SnapclientHub", cg.Component)

ChannelMode = snapclient_ns.enum("ChannelMode", is_class=True)
CHANNEL_MODES = {
    "stereo": ChannelMode.STEREO,
    "left": ChannelMode.LEFT_ONLY,
    "right": ChannelMode.RIGHT_ONLY,
    "mono": ChannelMode.MONO,
}

PauseBehavior = snapclient_ns.enum("PauseBehavior", is_class=True)
PAUSE_BEHAVIORS = {
    "allow": PauseBehavior.ALLOW,
    "resume": PauseBehavior.RESUME,
    "ignore": PauseBehavior.IGNORE,
}

PhaseMode = snapclient_ns.enum("PhaseMode", is_class=True)
PHASE_MODES = {
    "none": PhaseMode.NONE,
    "left": PhaseMode.LEFT,
    "right": PhaseMode.RIGHT,
    "both": PhaseMode.BOTH,
}
CONF_PHASE_INVERT = "phase_invert"
CONF_SYNC_DEADBAND = "sync_deadband"
CONF_CONVERGE_FINE = "converge_fine"
CONF_RATE_LOCK = "rate_lock"
CONF_I2S_PORT = "i2s_port"
CONF_TSF_SYNC = "tsf_sync"
CONF_TIMING_DIAGNOSTICS = "timing_diagnostics"
CONF_KEEPALIVE_HOLD = "keepalive_hold"
CONF_PAUSE_BEHAVIOR = "pause_behavior"


def _none_to_empty_dict(value):
    return {} if value is None else value


# Hardware rate lock: steady-state sync corrections steer the I2S clock divider
# instead of splicing frames -- zero waveform discontinuities once locked. Opt-in
# and S3-only for now (the backend steers the S3's MCLK fractional-N divider); the
# frame-splice servo remains the automatic fallback everywhere else.
RATE_LOCK_SCHEMA = cv.All(
    _none_to_empty_dict,
    cv.Schema(
        {
            # The i2s_audio bus port driving the speaker (first bus is port 0)
            cv.Optional(CONF_I2S_PORT, default=0): cv.int_range(min=0, max=1),
        }
    ),
    only_on_variant(
        supported=[VARIANT_ESP32S3],
        msg_prefix="rate_lock (hardware I2S clock steering)",
    ),
)


def _validate_convergence_bands(config: ConfigType) -> ConfigType:
    """The three sync thresholds must stay ordered, or convergence cannot finish.

    Unmuting requires the median to hold inside 2x sync_deadband, and only the
    fine mechanism can hold it there -- the coarse splices limit-cycle at roughly
    +-800us. So if converge_fine drops to or below the unmute band, the handoff
    happens inside the band the servo is trying to satisfy and a muted client can
    oscillate forever (observed fleet-wide before the handoff existed). Likewise a
    converge_fine at or above hard_resync_threshold would let the hard-resync path
    fire before the fine mechanism ever engages.
    """
    unmute_band_us = 2 * config[CONF_SYNC_DEADBAND].total_microseconds
    fine_us = config[CONF_CONVERGE_FINE].total_microseconds
    hard_us = config[CONF_HARD_RESYNC_THRESHOLD].total_milliseconds * 1000
    if fine_us <= unmute_band_us:
        raise cv.Invalid(
            f"{CONF_CONVERGE_FINE} ({fine_us}us) must exceed twice "
            f"{CONF_SYNC_DEADBAND} ({unmute_band_us}us): the coarse splices cannot "
            f"settle inside the unmute band, so the client would stay muted",
            path=[CONF_CONVERGE_FINE],
        )
    if fine_us >= hard_us:
        raise cv.Invalid(
            f"{CONF_CONVERGE_FINE} ({fine_us}us) must be below "
            f"{CONF_HARD_RESYNC_THRESHOLD} ({hard_us}us), or the hard-resync path "
            f"fires before the fine mechanism engages",
            path=[CONF_CONVERGE_FINE],
        )
    return config


def _request_networking(config: ConfigType) -> ConfigType:
    """Request the networking features synchronized streaming needs."""
    network.require_high_performance_networking()
    socket.consume_sockets(1, "snapclient")(config)
    # The hub requests high performance + roaming suppression while a stream is
    # playing and releases both after (a roam scan stalls the radio long enough to
    # starve playback)
    wifi.enable_runtime_power_save_control()
    wifi.enable_runtime_roaming_suppression()
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SnapclientHub),
            # Omit to discover the server via mDNS (_snapcast._tcp)
            cv.Optional(CONF_SERVER): cv.string_strict,
            cv.Optional(CONF_PORT, default=1704): cv.port,
            # Hello HostName; the server derives the default display name from it.
            cv.Optional(CONF_NAME): cv.string,
            # Decoded PCM buffer between the network and player tasks; PSRAM-preferred.
            # Sized for >1 s of 48 kHz/16-bit stereo by default.
            cv.Optional(CONF_BUFFER_SIZE, default=524288): cv.int_range(min=65536),
            cv.Optional(CONF_FLAC, default=True): cv.boolean,
            # Opt-in: libopus costs ~200 KB flash and, at runtime, a 120 KB
            # pseudostack plus ~40 KB of decoder state. snapserver's Opus streams
            # are always 48 kHz stereo (it resamples), so a mono or 44.1 kHz
            # pipeline has to be configured for that. Tune the memory placement
            # under the top-level `audio:` component's `codecs: opus:` key.
            cv.Optional(CONF_OPUS, default=False): cv.boolean,
            # Cadence while a stream is active; idle clients sync at max(this, 2s)
            cv.Optional(CONF_TIME_SYNC_INTERVAL, default="250ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(
                    min=cv.TimePeriod(milliseconds=100),
                    max=cv.TimePeriod(seconds=60),
                ),
            ),
            # Median sync error at which the steering servo engages (disengaging at
            # half). The reference esp32 snapclient uses 128us; steering trims one
            # frame per chunk, holding a stereo pair's image pinned. Raise on very
            # jittery links if corrections chatter.
            cv.Optional(CONF_SYNC_DEADBAND, default="128us"): cv.All(
                cv.positive_time_period_microseconds,
                cv.Range(
                    min=cv.TimePeriod(microseconds=100),
                    max=cv.TimePeriod(milliseconds=20),
                ),
            ),
            # Coarse->fine handoff for muted convergence. Above this median error
            # the servo uses hard multi-frame splices (fast, but they limit-cycle
            # at ~+-800us with the measurement lag); below it the PI trim / single
            # frames take over and actually settle inside the deadband. This is
            # also the boundary that decides whether a hard resync re-mutes: an
            # excursion that stays inside it is trim-only, so it is inaudible and
            # does not force a re-lock. Lower it to spend longer on the fast
            # mechanism (quicker lock, but risks the splice limit cycle never
            # satisfying the unmute gate); raise it to hand off earlier.
            cv.Optional(CONF_CONVERGE_FINE, default="2ms"): cv.All(
                cv.positive_time_period_microseconds,
                cv.Range(
                    min=cv.TimePeriod(microseconds=250),
                    max=cv.TimePeriod(milliseconds=50),
                ),
            ),
            cv.Optional(CONF_HARD_RESYNC_THRESHOLD, default="50ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(
                    min=cv.TimePeriod(milliseconds=10),
                    max=cv.TimePeriod(milliseconds=1000),
                ),
            ),
            # Boot defaults; the snapclient select entities override them
            cv.Optional(CONF_CHANNEL_MODE, default="stereo"): cv.enum(
                CHANNEL_MODES, lower=True
            ),
            cv.Optional(CONF_PHASE_INVERT, default="none"): cv.enum(
                PHASE_MODES, lower=True
            ),
            cv.Optional(CONF_RATE_LOCK): RATE_LOCK_SCHEMA,
            # TSF group sync: same-AP clients share one
            # server->TSF mapping so their mutual sync is us-class; wifi-only,
            # silently inactive elsewhere (Kalman fallback)
            cv.Optional(CONF_TSF_SYNC, default=False): cv.boolean,
            # Per-chunk raw timing samples for scripts/raw-sync.py, which measures true
            # inter-device rendering from direct observations. Deliberately separate from
            # the log level: it emits ~38 lines/s/device, and the situation where DEBUG
            # logs are most wanted -- chasing dropouts -- is the one where that extra
            # traffic on a congested channel does the most harm. Off by default.
            cv.Optional(CONF_TIMING_DIAGNOSTICS, default=False): cv.boolean,
            # How long a chunk gap is bridged with keepalive silence before the
            # stream is allowed to end. Ending it tears the audio pipeline down, and
            # rebuilding playout phase costs a mute plus 7-16 s of re-lock -- so an
            # ordinary inter-track gap (17-18 s measured) is worth bridging.
            #
            # Defaults to `never`: hold the pipeline for the whole session so the
            # speaker stays ready to play in sync. The cost is a continuously fed DAC,
            # the radio pinned in high-performance mode and nonstop TSF beaconing --
            # irrelevant on a mains-powered speaker, which is what this component is
            # for, but set a duration on anything battery-powered.
            #
            # Note what `never` does NOT buy: it is not sufficient on its own for
            # instant resumption. A stream that resumed after 7.5 h idle still came
            # back with a 6.9 h stale deadline and took 9.6-16 s to settle, with the
            # pipeline held the entire time. The residual cost there was the servo
            # settling plus a TSF re-election, not the teardown this prevents.
            # What a local PAUSE or STOP does. A fixed multiroom speaker and a desk
            # speaker want opposite answers, so this is configurable:
            #
            #   allow   honoured; the client stays silent until something plays it again
            #   resume  honoured, then undone once audio is flowing again -- survives a
            #           "stop everything" automation, at the cost of a real audible gap
            #   ignore  refused outright, so there is no gap at all; the media player
            #           keeps reporting PLAYING and the transport button does nothing.
            #           A deliberate stop is refused too, so anything that relies on
            #           stopping this player will not work.
            cv.Optional(CONF_PAUSE_BEHAVIOR, default="allow"): cv.enum(
                PAUSE_BEHAVIORS, lower=True
            ),
            cv.Optional(CONF_KEEPALIVE_HOLD, default=CONF_NEVER): cv.Any(
                cv.positive_time_period_milliseconds,
                cv.one_of(CONF_NEVER, lower=True),
            ),
            cv.Optional(CONF_STREAM_IDLE_TIMEOUT, default="3s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(
                    min=cv.TimePeriod(milliseconds=500),
                    max=cv.TimePeriod(seconds=30),
                ),
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    _validate_convergence_bands,
    _request_networking,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Pause the stream during an OTA: audio and the firmware transfer share the
    # radio (no-op when no ota component is configured)
    ota.request_ota_state_listeners()

    cg.add(var.set_server(config.get(CONF_SERVER, ""), config[CONF_PORT]))
    if CONF_NAME in config:
        cg.add(var.set_client_name(config[CONF_NAME]))
    cg.add(var.set_buffer_size(config[CONF_BUFFER_SIZE]))
    cg.add(
        var.set_time_sync_interval(
            config[CONF_TIME_SYNC_INTERVAL].total_milliseconds
        )
    )
    cg.add(var.set_sync_deadband(config[CONF_SYNC_DEADBAND].total_microseconds))
    cg.add(var.set_converge_fine(config[CONF_CONVERGE_FINE].total_microseconds))
    cg.add(
        var.set_hard_resync_threshold(
            config[CONF_HARD_RESYNC_THRESHOLD].total_milliseconds
        )
    )
    cg.add(
        var.set_stream_idle_timeout(
            config[CONF_STREAM_IDLE_TIMEOUT].total_milliseconds
        )
    )
    # 0 is the "never release while connected" sentinel on the C++ side
    cg.add(var.set_pause_behavior(config[CONF_PAUSE_BEHAVIOR]))
    hold = config[CONF_KEEPALIVE_HOLD]
    cg.add(var.set_keepalive_hold(0 if hold == CONF_NEVER else hold.total_milliseconds))

    cg.add(var.set_channel_mode(config[CONF_CHANNEL_MODE]))
    cg.add(var.set_phase_mode(config[CONF_PHASE_INVERT]))

    if CONF_RATE_LOCK in config:
        audio_timing.request_rate_lock()
        cg.add(var.set_rate_lock_port(config[CONF_RATE_LOCK][CONF_I2S_PORT]))

    if config[CONF_TSF_SYNC]:
        audio_timing.request_tsf_sync()
    if config[CONF_TIMING_DIAGNOSTICS]:
        cg.add_define("USE_SNAPCLIENT_TIMING_DIAG", True)

    if config[CONF_FLAC]:
        # snapserver's default stream codec; pulls micro_flac from esp-audio-libs
        audio.request_flac_support()
        cg.add_define("USE_SNAPCLIENT_FLAC", True)

    if config[CONF_OPUS]:
        # Pulls micro-opus (libopus). Snapcast frames Opus raw -- one packet per wire
        # chunk, no Ogg -- so the decode path calls libopus directly rather than going
        # through esp-audio-libs' Ogg-expecting decoder.
        audio.request_opus_support()
        cg.add_define("USE_SNAPCLIENT_OPUS", True)
