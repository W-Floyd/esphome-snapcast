"""Channel mode select entity (Stereo / Left / Right / Mono) for snapclient."""

import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_RESTORE_VALUE, ENTITY_CATEGORY_CONFIG

from .. import CONF_SNAPCLIENT_ID, SnapclientHub, snapclient_ns

CODEOWNERS = ["@W-Floyd"]

# Order must match the ChannelMode enum
CHANNEL_MODE_OPTIONS = ["Stereo", "Left", "Right", "Mono"]

SnapclientChannelModeSelect = snapclient_ns.class_(
    "SnapclientChannelModeSelect", cg.Component, select.Select
)

CONFIG_SCHEMA = cv.All(
    select.select_schema(
        SnapclientChannelModeSelect,
        entity_category=ENTITY_CATEGORY_CONFIG,
        icon="mdi:speaker-multiple",
    ).extend(
        {
            cv.GenerateID(CONF_SNAPCLIENT_ID): cv.use_id(SnapclientHub),
            cv.Optional(CONF_RESTORE_VALUE, default=True): cv.boolean,
        }
    ),
    cv.only_on_esp32,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await select.register_select(var, config, options=CHANNEL_MODE_OPTIONS)

    hub = await cg.get_variable(config[CONF_SNAPCLIENT_ID])
    await cg.register_parented(var, hub)
    cg.add(var.set_restore_value(config[CONF_RESTORE_VALUE]))
