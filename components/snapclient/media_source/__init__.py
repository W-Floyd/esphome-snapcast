"""Snapcast media source platform, modeled on sendspin's media_source platform."""

import esphome.codegen as cg
from esphome.components import media_source
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.types import ConfigType

from .. import CONF_SNAPCLIENT_ID, SnapclientHub, snapclient_ns

CODEOWNERS = ["@W-Floyd"]

CONF_STATIC_DELAY = "static_delay"

SnapclientMediaSource = snapclient_ns.class_(
    "SnapclientMediaSource",
    cg.Component,
    media_source.MediaSource,
)

CONFIG_SCHEMA = cv.All(
    media_source.media_source_schema(
        SnapclientMediaSource,
    ).extend(
        {
            cv.GenerateID(CONF_SNAPCLIENT_ID): cv.use_id(SnapclientHub),
            # Per-device latency trim, subtracted from every chunk deadline. The
            # equivalent of desktop snapclient's --latency option.
            cv.Optional(CONF_STATIC_DELAY, default="0ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(max=cv.TimePeriod(seconds=2)),
            ),
        }
    ),
    cv.only_on_esp32,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await media_source.register_media_source(var, config)

    hub = await cg.get_variable(config[CONF_SNAPCLIENT_ID])
    await cg.register_parented(var, hub)

    cg.add(hub.set_audio_listener(var))
    cg.add(var.set_static_delay_ms(config[CONF_STATIC_DELAY].total_milliseconds))
