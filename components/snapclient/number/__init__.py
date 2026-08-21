"""Volume-curve dB range slider entity for snapclient."""

import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_INITIAL_VALUE,
    CONF_RESTORE_VALUE,
    ENTITY_CATEGORY_CONFIG,
    UNIT_DECIBEL,
)

from .. import CONF_SNAPCLIENT_ID, SnapclientHub, snapclient_ns

CODEOWNERS = ["@W-Floyd"]

SnapclientVolumeCurveNumber = snapclient_ns.class_(
    "SnapclientVolumeCurveNumber", cg.Component, number.Number
)

CONFIG_SCHEMA = cv.All(
    number.number_schema(
        SnapclientVolumeCurveNumber,
        entity_category=ENTITY_CATEGORY_CONFIG,
        icon="mdi:tune-variant",
        unit_of_measurement=UNIT_DECIBEL,
    ).extend(
        {
            cv.GenerateID(CONF_SNAPCLIENT_ID): cv.use_id(SnapclientHub),
            # 0 = linear (curve off); the reference esp32 snapclient ships with 60
            cv.Optional(CONF_INITIAL_VALUE, default=0.0): cv.float_range(
                min=0.0, max=100.0
            ),
            cv.Optional(CONF_RESTORE_VALUE, default=True): cv.boolean,
        }
    ),
    cv.only_on_esp32,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await number.register_number(var, config, min_value=0.0, max_value=100.0, step=1.0)

    hub = await cg.get_variable(config[CONF_SNAPCLIENT_ID])
    await cg.register_parented(var, hub)
    cg.add(var.set_restore_value(config[CONF_RESTORE_VALUE]))
    cg.add(var.set_initial_value(config[CONF_INITIAL_VALUE]))
