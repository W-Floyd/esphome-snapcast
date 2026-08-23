import esphome.codegen as cg
from esphome.components import select, wifi
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_RESTORE_VALUE, ENTITY_CATEGORY_CONFIG

CODEOWNERS = ["@W-Floyd"]
DEPENDENCIES = ["wifi"]

wifi_bssid_select_ns = cg.esphome_ns.namespace("wifi_bssid_select")
WifiBssidSelect = wifi_bssid_select_ns.class_(
    "WifiBssidSelect", select.Select, cg.Component
)

# Options are built at runtime from scan results, so codegen supplies only the
# placeholder the entity publishes before the first scan lands.
AUTOMATIC = "Automatic"

CONFIG_SCHEMA = cv.All(
    select.select_schema(
        WifiBssidSelect,
        entity_category=ENTITY_CATEGORY_CONFIG,
        icon="mdi:router-wireless",
    ).extend(
        {
            cv.Optional(CONF_RESTORE_VALUE, default=True): cv.boolean,
        }
    ),
    cv.only_on_esp32,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await select.register_select(var, config, options=[AUTOMATIC])
    # Scan results name the candidate APs; connect state says which SSID to filter to
    wifi.request_wifi_scan_results_listener()
    wifi.request_wifi_connect_state_listener()
    cg.add(var.set_restore_value(config[CONF_RESTORE_VALUE]))
