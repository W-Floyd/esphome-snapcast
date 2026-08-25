"""Radio-level WiFi utilities.

Nothing here is Snapcast-specific -- two pieces of hard-won esp_wifi knowledge, kept as
versioned compiled code rather than as YAML lambdas:

- ``wifi_tools::set_max_tx_power(dbm)`` -- sets the radio's max TX power and logs what
  the driver ACTUALLY accepted, because it quantizes the request into bins and then
  clamps to the PHY init data and the country limit. Call it from a template number's
  set_action; the readback is the only trustworthy value.
- ``diagnostics`` -- periodic RSSI + negotiated PHY mode at DEBUG. The PHY mode is the
  valuable half: a fallback from HT20 to 11G/11B is the classic marginal-link tell.

This also confines <esp_wifi.h> to one translation unit, so board files no longer need
`esphome: includes: [<esp_wifi.h>]` just to let a lambda reach into the driver.
"""

import esphome.codegen as cg
from esphome.components import wifi  # noqa: F401  (ensures wifi is configured)
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_UPDATE_INTERVAL

CODEOWNERS = ["@W-Floyd"]
DEPENDENCIES = ["wifi"]

CONF_DIAGNOSTICS = "diagnostics"
CONF_DUMP_STATISTICS = "dump_statistics"

wifi_tools_ns = cg.esphome_ns.namespace("wifi_tools")
WifiDiagnostics = wifi_tools_ns.class_("WifiDiagnostics", cg.PollingComponent)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(CONF_DIAGNOSTICS): cv.Schema(
                {
                    cv.GenerateID(): cv.declare_id(WifiDiagnostics),
                    # `never` keeps set_max_tx_power available without the polling
                    cv.Optional(CONF_UPDATE_INTERVAL, default="10s"): cv.update_interval,
                    # See the header: unproven, verbose, and a suspect for log stalls
                    cv.Optional(CONF_DUMP_STATISTICS, default=False): cv.boolean,
                }
            ),
        }
    ),
    cv.only_on_esp32,
)


async def to_code(config):
    if CONF_DIAGNOSTICS in config:
        diag = config[CONF_DIAGNOSTICS]
        var = cg.new_Pvariable(diag[CONF_ID])
        await cg.register_component(var, diag)
        cg.add(var.set_dump_statistics(diag[CONF_DUMP_STATISTICS]))
