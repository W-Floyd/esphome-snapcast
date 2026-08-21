"""Snapcast client hub component, modeled on ESPHome's sendspin component."""

import esphome.codegen as cg
from esphome.components import audio, network, socket, wifi
from esphome.components.esp32 import only_on_variant
from esphome.components.esp32.const import VARIANT_ESP32S3
import esphome.config_validation as cv
from esphome.const import CONF_BUFFER_SIZE, CONF_ID, CONF_NAME, CONF_PORT
from esphome.types import ConfigType

CODEOWNERS = ["@W-Floyd"]
DEPENDENCIES = ["network"]
# audio: micro decoder libraries (FLAC); json: ArduinoJson for hello/settings payloads;
# ring_buffer: the decoded-PCM buffer between the network and player tasks;
# mdns: server auto-discovery via _snapcast._tcp when no server is configured.
AUTO_LOAD = ["audio", "json", "mdns", "ring_buffer"]
DOMAIN = "snapclient"

CONF_SERVER = "server"
CONF_SNAPCLIENT_ID = "snapclient_id"
CONF_FLAC = "flac"
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

PhaseMode = snapclient_ns.enum("PhaseMode", is_class=True)
PHASE_MODES = {
    "none": PhaseMode.NONE,
    "left": PhaseMode.LEFT,
    "right": PhaseMode.RIGHT,
    "both": PhaseMode.BOTH,
}
CONF_PHASE_INVERT = "phase_invert"
CONF_SYNC_DEADBAND = "sync_deadband"
CONF_RATE_LOCK = "rate_lock"
CONF_I2S_PORT = "i2s_port"


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
    _request_networking,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

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

    cg.add(var.set_channel_mode(config[CONF_CHANNEL_MODE]))
    cg.add(var.set_phase_mode(config[CONF_PHASE_INVERT]))

    if CONF_RATE_LOCK in config:
        cg.add_define("USE_SNAPCLIENT_RATE_LOCK", True)
        cg.add(var.set_rate_lock_port(config[CONF_RATE_LOCK][CONF_I2S_PORT]))

    if config[CONF_FLAC]:
        # snapserver's default stream codec; pulls micro_flac from esp-audio-libs
        audio.request_flac_support()
        cg.add_define("USE_SNAPCLIENT_FLAC", True)
