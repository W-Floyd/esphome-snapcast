"""Select entities for snapclient: channel mode and phase inversion."""

import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_RESTORE_VALUE, CONF_TYPE, ENTITY_CATEGORY_CONFIG

from .. import CONF_SNAPCLIENT_ID, SnapclientHub, snapclient_ns

CODEOWNERS = ["@W-Floyd"]

CONF_CHANNEL_MODE = "channel_mode"
CONF_PHASE = "phase"
CONF_SERVER = "server"
CONF_PAUSE_BEHAVIOR = "pause_behavior"
CONF_SYNC_RESILIENCE = "sync_resilience"

# Option orders must match the ChannelMode / PhaseMode / PauseBehavior enums
CHANNEL_MODE_OPTIONS = ["Stereo", "Left", "Right", "Mono"]
PHASE_OPTIONS = ["None", "Left", "Right", "Both"]
PAUSE_BEHAVIOR_OPTIONS = ["Allow", "Resume", "Ignore"]
# Ascending tolerance of audible artifacts, matching the SyncResilience enum. "Mute on storms"
# is the historical behaviour and stays the default; the other two trade audible corrections for
# never going silent. None of them changes the first lock after a start or reconnect.
SYNC_RESILIENCE_OPTIONS = ["Mute on storms", "Play through storms", "Never mute"]
# Mono-summing amps (MAX98357A style) analog-mix L+R: inverting one side of
# duplicated content cancels to silence, so only whole-program inversion is offered;
# "Stereo" is just an undefined analog (L+R)/2, so the digital Mix stands in for it
PHASE_OPTIONS_MONO = ["None", "Inverted"]
CHANNEL_MODE_OPTIONS_MONO = ["Mix", "Left", "Right"]
CONF_MONO_DAC = "mono_dac"

_MONO_DAC_EXTENSION = {
    # Set for MAX98357A-style mono-summing amps; collapses the options to the
    # combinations that are meaningful on a single summed output
    cv.Optional(CONF_MONO_DAC, default=False): cv.boolean,
}

SnapclientChannelModeSelect = snapclient_ns.class_(
    "SnapclientChannelModeSelect", cg.Component, select.Select
)
SnapclientPhaseSelect = snapclient_ns.class_(
    "SnapclientPhaseSelect", cg.Component, select.Select
)
SnapclientServerSelect = snapclient_ns.class_(
    "SnapclientServerSelect", cg.Component, select.Select
)
SnapclientPauseBehaviorSelect = snapclient_ns.class_(
    "SnapclientPauseBehaviorSelect", cg.Component, select.Select
)
SnapclientSyncResilienceSelect = snapclient_ns.class_(
    "SnapclientSyncResilienceSelect", cg.Component, select.Select
)


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
            ).extend(_MONO_DAC_EXTENSION),
            CONF_PHASE: _select_schema(SnapclientPhaseSelect, "mdi:sine-wave").extend(
                _MONO_DAC_EXTENSION
            ),
            # Discovered-server picker: "Automatic" + one option per snapserver found
            # via mDNS; selecting one overrides the connection target (a manual
            # server text entity takes precedence over this)
            CONF_SERVER: _select_schema(
                SnapclientServerSelect, "mdi:server-network"
            ),
            # What a local PAUSE/STOP does. Runtime-selectable because the right answer
            # belongs to the room rather than the build: Ignore for a fixed multiroom
            # speaker whose group is the source of truth, Allow for one whose transport
            # buttons should work. Overrides the hub's pause_behavior default.
            CONF_PAUSE_BEHAVIOR: _select_schema(
                SnapclientPauseBehaviorSelect, "mdi:pause-octagon-outline"
            ),
            # How much audible disruption to accept rather than mute through a resync
            # episode. Runtime-selectable because it is a property of what the speaker
            # carries: music in a group is usually better off silent through a storm,
            # speech or a lone speaker is better off rough.
            CONF_SYNC_RESILIENCE: _select_schema(
                SnapclientSyncResilienceSelect, "mdi:volume-vibrate"
            ),
        },
    ),
    cv.only_on_esp32,
)

_OPTIONS = {
    CONF_CHANNEL_MODE: CHANNEL_MODE_OPTIONS,
    CONF_PHASE: PHASE_OPTIONS,
    CONF_SERVER: ["Automatic"],
    CONF_PAUSE_BEHAVIOR: PAUSE_BEHAVIOR_OPTIONS,
    CONF_SYNC_RESILIENCE: SYNC_RESILIENCE_OPTIONS,
}


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    options = _OPTIONS[config[CONF_TYPE]]
    if config[CONF_TYPE] in (CONF_PHASE, CONF_CHANNEL_MODE):
        if config[CONF_MONO_DAC]:
            options = (
                PHASE_OPTIONS_MONO
                if config[CONF_TYPE] == CONF_PHASE
                else CHANNEL_MODE_OPTIONS_MONO
            )
        cg.add(var.set_mono_dac(config[CONF_MONO_DAC]))
    await select.register_select(var, config, options=options)

    hub = await cg.get_variable(config[CONF_SNAPCLIENT_ID])
    await cg.register_parented(var, hub)
    cg.add(var.set_restore_value(config[CONF_RESTORE_VALUE]))
