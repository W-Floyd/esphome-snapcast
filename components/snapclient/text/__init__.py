"""Text entity for snapclient: manual server override."""

import esphome.codegen as cg
from esphome.components import text
import esphome.config_validation as cv
from esphome.const import CONF_RESTORE_VALUE, ENTITY_CATEGORY_CONFIG

from .. import CONF_SNAPCLIENT_ID, SnapclientHub, snapclient_ns

CODEOWNERS = ["@W-Floyd"]

SnapclientServerText = snapclient_ns.class_(
    "SnapclientServerText", cg.Component, text.Text
)

# Manual server override ("host" or "host:port"; empty clears). Takes precedence
# over the server select entity, the YAML `server:`, and mDNS discovery.
CONFIG_SCHEMA = cv.All(
    text.text_schema(
        SnapclientServerText,
        entity_category=ENTITY_CATEGORY_CONFIG,
        icon="mdi:server-network-outline",
        mode="TEXT",
    ).extend(
        {
            cv.GenerateID(CONF_SNAPCLIENT_ID): cv.use_id(SnapclientHub),
            cv.Optional(CONF_RESTORE_VALUE, default=True): cv.boolean,
        }
    ),
    cv.only_on_esp32,
)


async def to_code(config):
    var = await text.new_text(config)
    await cg.register_component(var, config)

    hub = await cg.get_variable(config[CONF_SNAPCLIENT_ID])
    await cg.register_parented(var, hub)
    cg.add(var.set_restore_value(config[CONF_RESTORE_VALUE]))
    # Matches the flash-persisted buffer (StoredValue)
    cg.add(var.traits.set_max_length(64))
