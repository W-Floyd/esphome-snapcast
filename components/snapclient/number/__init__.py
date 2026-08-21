"""Number entities for snapclient: volume-curve dB range and server-side latency."""

import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_INITIAL_VALUE,
    CONF_RESTORE_VALUE,
    CONF_TYPE,
    ENTITY_CATEGORY_CONFIG,
    UNIT_DECIBEL,
    UNIT_MILLISECOND,
)

from .. import CONF_SNAPCLIENT_ID, SnapclientHub, snapclient_ns

CODEOWNERS = ["@W-Floyd"]

CONF_VOLUME_CURVE = "volume_curve"
CONF_SERVER_LATENCY = "server_latency"

SnapclientVolumeCurveNumber = snapclient_ns.class_(
    "SnapclientVolumeCurveNumber", cg.Component, number.Number
)
SnapclientServerLatencyNumber = snapclient_ns.class_(
    "SnapclientServerLatencyNumber", cg.Component, number.Number
)

CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
            CONF_VOLUME_CURVE: number.number_schema(
                SnapclientVolumeCurveNumber,
                entity_category=ENTITY_CATEGORY_CONFIG,
                icon="mdi:tune-variant",
                unit_of_measurement=UNIT_DECIBEL,
            ).extend(
                {
                    cv.GenerateID(CONF_SNAPCLIENT_ID): cv.use_id(SnapclientHub),
                    # 0 = linear (curve off); ESPHome speakers already apply a
                    # perceptual taper, so only enable on truly linear output paths
                    cv.Optional(CONF_INITIAL_VALUE, default=0.0): cv.float_range(
                        min=0.0, max=100.0
                    ),
                    cv.Optional(CONF_RESTORE_VALUE, default=True): cv.boolean,
                }
            ),
            # This client's latency setting ON THE SNAPSERVER (Client.SetLatency via
            # the control API); the server persists it and its ServerSettings pushes
            # keep the entity state in sync — no local persistence.
            CONF_SERVER_LATENCY: number.number_schema(
                SnapclientServerLatencyNumber,
                entity_category=ENTITY_CATEGORY_CONFIG,
                icon="mdi:timer-outline",
                unit_of_measurement=UNIT_MILLISECOND,
            ).extend(
                {
                    cv.GenerateID(CONF_SNAPCLIENT_ID): cv.use_id(SnapclientHub),
                }
            ),
        },
    ),
    cv.only_on_esp32,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    if config[CONF_TYPE] == CONF_VOLUME_CURVE:
        await number.register_number(
            var, config, min_value=0.0, max_value=100.0, step=1.0
        )
        cg.add(var.set_restore_value(config[CONF_RESTORE_VALUE]))
        cg.add(var.set_initial_value(config[CONF_INITIAL_VALUE]))
    else:
        await number.register_number(
            var, config, min_value=-1000.0, max_value=1000.0, step=1.0
        )

    hub = await cg.get_variable(config[CONF_SNAPCLIENT_ID])
    await cg.register_parented(var, hub)
