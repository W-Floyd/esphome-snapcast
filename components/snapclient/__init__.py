"""Snapcast client hub component, modeled on ESPHome's sendspin component."""

import esphome.codegen as cg
from esphome.components import audio, network, socket, wifi
import esphome.config_validation as cv
from esphome.const import CONF_BUFFER_SIZE, CONF_ID, CONF_NAME, CONF_PORT
from esphome.types import ConfigType

CODEOWNERS = ["@W-Floyd"]
DEPENDENCIES = ["network"]
# audio: micro decoder libraries (FLAC); json: ArduinoJson for hello/settings payloads;
# ring_buffer: the decoded-PCM buffer between the network and player tasks.
AUTO_LOAD = ["audio", "json", "ring_buffer"]
DOMAIN = "snapclient"

CONF_SERVER = "server"
CONF_SNAPCLIENT_ID = "snapclient_id"
CONF_FLAC = "flac"
CONF_TIME_SYNC_INTERVAL = "time_sync_interval"
CONF_HARD_RESYNC_THRESHOLD = "hard_resync_threshold"
CONF_STREAM_IDLE_TIMEOUT = "stream_idle_timeout"

snapclient_ns = cg.esphome_ns.namespace("snapclient")
SnapclientHub = snapclient_ns.class_("SnapclientHub", cg.Component)


def _request_networking(config: ConfigType) -> ConfigType:
    """Request the networking features synchronized streaming needs."""
    network.require_high_performance_networking()
    socket.consume_sockets(1, "snapclient")(config)
    # The hub requests high performance while a stream is playing and releases it after
    wifi.enable_runtime_power_save_control()
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SnapclientHub),
            cv.Required(CONF_SERVER): cv.string_strict,
            cv.Optional(CONF_PORT, default=1704): cv.port,
            # Hello HostName; the server derives the default display name from it.
            cv.Optional(CONF_NAME): cv.string,
            # Decoded PCM buffer between the network and player tasks; PSRAM-preferred.
            # Sized for >1 s of 48 kHz/16-bit stereo by default.
            cv.Optional(CONF_BUFFER_SIZE, default=524288): cv.int_range(min=65536),
            cv.Optional(CONF_FLAC, default=True): cv.boolean,
            cv.Optional(CONF_TIME_SYNC_INTERVAL, default="1s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(
                    min=cv.TimePeriod(milliseconds=100),
                    max=cv.TimePeriod(seconds=60),
                ),
            ),
            cv.Optional(CONF_HARD_RESYNC_THRESHOLD, default="50ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(
                    min=cv.TimePeriod(milliseconds=10),
                    max=cv.TimePeriod(milliseconds=1000),
                ),
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
    _request_networking,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_server(config[CONF_SERVER], config[CONF_PORT]))
    if CONF_NAME in config:
        cg.add(var.set_client_name(config[CONF_NAME]))
    cg.add(var.set_buffer_size(config[CONF_BUFFER_SIZE]))
    cg.add(
        var.set_time_sync_interval(
            config[CONF_TIME_SYNC_INTERVAL].total_milliseconds
        )
    )
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

    if config[CONF_FLAC]:
        # snapserver's default stream codec; pulls micro_flac from esp-audio-libs
        audio.request_flac_support()
        cg.add_define("USE_SNAPCLIENT_FLAC", True)
