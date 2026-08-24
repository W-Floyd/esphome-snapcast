"""Number entity for snapclient: this client's latency on the snapserver."""

import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_TYPE,
    ENTITY_CATEGORY_CONFIG,
    UNIT_MILLISECOND,
)

from .. import CONF_SNAPCLIENT_ID, SnapclientHub, snapclient_ns

CODEOWNERS = ["@W-Floyd"]

CONF_SERVER_LATENCY = "server_latency"

SnapclientServerLatencyNumber = snapclient_ns.class_(
    "SnapclientServerLatencyNumber", cg.Component, number.Number
)

CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
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
    await number.register_number(
        var, config, min_value=-1000.0, max_value=1000.0, step=1.0
    )

    hub = await cg.get_variable(config[CONF_SNAPCLIENT_ID])
    await cg.register_parented(var, hub)
