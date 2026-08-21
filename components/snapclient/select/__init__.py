"""Select entities for snapclient: channel mode and phase inversion."""

import esphome.codegen as cg
from esphome.components import select, wifi
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_RESTORE_VALUE, CONF_TYPE, ENTITY_CATEGORY_CONFIG

from .. import CONF_SNAPCLIENT_ID, SnapclientHub, snapclient_ns

CODEOWNERS = ["@W-Floyd"]

CONF_CHANNEL_MODE = "channel_mode"
CONF_PHASE = "phase"
CONF_SERVER = "server"

# Option orders must match the ChannelMode / PhaseMode enums
CHANNEL_MODE_OPTIONS = ["Stereo", "Left", "Right", "Mono"]
PHASE_OPTIONS = ["None", "Left", "Right", "Both"]
# Mono-summing amps (MAX98357A style) analog-mix L+R: inverting one side of
# duplicated content cancels to silence, so only whole-program inversion is offered
PHASE_OPTIONS_MONO = ["None", "Inverted"]
CONF_MONO_DAC = "mono_dac"

SnapclientChannelModeSelect = snapclient_ns.class_(
    "SnapclientChannelModeSelect", cg.Component, select.Select
)
SnapclientPhaseSelect = snapclient_ns.class_(
    "SnapclientPhaseSelect", cg.Component, select.Select
)
SnapclientServerSelect = snapclient_ns.class_(
    "SnapclientServerSelect", cg.Component, select.Select
)
SnapclientBssidSelect = snapclient_ns.class_(
    "SnapclientBssidSelect", cg.Component, select.Select
)
CONF_BSSID = "bssid"


def _select_schema(class_, icon):
    return select.select_schema(
        class_,
        entity_category=ENTITY_CATEGORY_CONFIG,
        icon=icon,
    ).extend(
        {
            cv.GenerateID(CONF_SNAPCLIENT_ID): cv.use_id(SnapclientHub),
            cv.Optional(CONF_RESTORE_VALUE, default=True): cv.boolean,
        }
    )


CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
            CONF_CHANNEL_MODE: _select_schema(
                SnapclientChannelModeSelect, "mdi:speaker-multiple"
            ),
            CONF_PHASE: _select_schema(SnapclientPhaseSelect, "mdi:sine-wave").extend(
                {
                    # Set for MAX98357A-style mono-summing amps: single-side
                    # inversion would cancel to silence, so the options collapse
                    # to None / Inverted
                    cv.Optional(CONF_MONO_DAC, default=False): cv.boolean,
                }
            ),
            # Discovered-server picker: "Automatic" + one option per snapserver found
            # via mDNS; selecting one overrides the connection target (a manual
            # server text entity takes precedence over this)
            CONF_SERVER: _select_schema(
                SnapclientServerSelect, "mdi:server-network"
            ),
            # AP (BSSID) picker: "Automatic" + one option per scan-discovered AP of
            # the connected SSID. Pinning an AP removes mid-stream roams and
            # guarantees a synchronized group shares one TSF timebase. Standalone --
            # needs wifi, not the snapclient hub.
            CONF_BSSID: cv.All(
                select.select_schema(
                    SnapclientBssidSelect,
                    entity_category=ENTITY_CATEGORY_CONFIG,
                    icon="mdi:router-wireless",
                ).extend(
                    {
                        cv.Optional(CONF_RESTORE_VALUE, default=True): cv.boolean,
                    }
                ),
                cv.requires_component("wifi"),
            ),
        },
    ),
    cv.only_on_esp32,
)

_OPTIONS = {
    CONF_CHANNEL_MODE: CHANNEL_MODE_OPTIONS,
    CONF_PHASE: PHASE_OPTIONS,
    CONF_SERVER: ["Automatic"],
    CONF_BSSID: ["Automatic"],
}


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    options = _OPTIONS[config[CONF_TYPE]]
    if config[CONF_TYPE] == CONF_PHASE:
        if config[CONF_MONO_DAC]:
            options = PHASE_OPTIONS_MONO
        cg.add(var.set_mono_dac(config[CONF_MONO_DAC]))
    await select.register_select(var, config, options=options)

    if config[CONF_TYPE] == CONF_BSSID:
        # Standalone wifi entity: scan results + connect state drive the options
        wifi.request_wifi_scan_results_listener()
        wifi.request_wifi_connect_state_listener()
    else:
        hub = await cg.get_variable(config[CONF_SNAPCLIENT_ID])
        await cg.register_parented(var, hub)
    cg.add(var.set_restore_value(config[CONF_RESTORE_VALUE]))
