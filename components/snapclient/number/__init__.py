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

# ESPHome has no microsecond constant.
UNIT_MICROSECOND = "µs"

from .. import CONF_SNAPCLIENT_ID, SnapclientHub, snapclient_ns

CODEOWNERS = ["@W-Floyd"]

CONF_SERVER_LATENCY = "server_latency"

SnapclientServerLatencyNumber = snapclient_ns.class_(
    "SnapclientServerLatencyNumber", cg.Component, number.Number
)
SnapclientServoParamNumber = snapclient_ns.class_(
    "SnapclientServoParamNumber", cg.Component, number.Number
)

# Tunables exposed to the frontend, each backed by set_servo_param(). Adding one is an entry
# here, not a new class. min/max/step are the useful editing range, not the code's own limits.
#   name: (servo_param, unit, min, max, step, icon)
SERVO_PARAM_NUMBERS = {
    "timing_target_us": ("timing_target_us", UNIT_MICROSECOND, 1.0, 500.0, 1.0, "mdi:target"),
    "blank_ms": ("blank_ms", UNIT_MILLISECOND, 0.0, 5000.0, 10.0, "mdi:blur-off"),
    "gap_blank_ms": ("gap_blank_ms", UNIT_MILLISECOND, 0.0, 2000.0, 10.0, "mdi:blur-off"),
    "tag_stale_ms": ("tag_stale_ms", UNIT_MILLISECOND, 100.0, 10000.0, 50.0, "mdi:timer-sand"),
}

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
            **{
                key: number.number_schema(
                    SnapclientServoParamNumber,
                    entity_category=ENTITY_CATEGORY_CONFIG,
                    icon=icon,
                    unit_of_measurement=unit,
                ).extend(
                    {
                        cv.GenerateID(CONF_SNAPCLIENT_ID): cv.use_id(SnapclientHub),
                    }
                )
                for key, (_param, unit, _lo, _hi, _step, icon) in SERVO_PARAM_NUMBERS.items()
            },
        },
    ),
    cv.only_on_esp32,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    kind = config[CONF_TYPE]
    if kind in SERVO_PARAM_NUMBERS:
        param, _unit, lo, hi, step, _icon = SERVO_PARAM_NUMBERS[kind]
        await number.register_number(var, config, min_value=lo, max_value=hi, step=step)
        cg.add(var.set_param(param))
    else:
        await number.register_number(
            var, config, min_value=-1000.0, max_value=1000.0, step=1.0
        )

    hub = await cg.get_variable(config[CONF_SNAPCLIENT_ID])
    await cg.register_parented(var, hub)
