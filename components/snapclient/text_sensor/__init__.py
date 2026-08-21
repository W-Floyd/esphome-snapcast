"""Text sensors for snapclient: TSF group-sync role."""

import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_ID, ENTITY_CATEGORY_DIAGNOSTIC

from .. import CONF_SNAPCLIENT_ID, SnapclientHub, snapclient_ns

CODEOWNERS = ["@W-Floyd"]

CONF_TSF_ROLE = "tsf_role"

SnapclientTsfRoleTextSensor = snapclient_ns.class_(
    "SnapclientTsfRoleTextSensor", cg.Component, text_sensor.TextSensor
)

CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
            # "Leader" / "Follower" / "Inactive" — which role this device holds in
            # the TSF group sync (PLAN-tsf-sync.md); Inactive when tsf_sync is off,
            # no wifi, no session, or no election result yet
            CONF_TSF_ROLE: text_sensor.text_sensor_schema(
                SnapclientTsfRoleTextSensor,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:account-star",
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
    await text_sensor.register_text_sensor(var, config)

    hub = await cg.get_variable(config[CONF_SNAPCLIENT_ID])
    await cg.register_parented(var, hub)
