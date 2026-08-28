"""Text sensors for snapclient: TSF group-sync state, stream format, and metadata."""

import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TYPE, ENTITY_CATEGORY_DIAGNOSTIC

from .. import CONF_SNAPCLIENT_ID, SnapclientHub, snapclient_ns

CODEOWNERS = ["@W-Floyd"]

CONF_TSF_STATE = "tsf_state"
CONF_STREAM_FORMAT = "stream_format"

SnapclientTsfStateTextSensor = snapclient_ns.class_(
    "SnapclientTsfStateTextSensor", cg.Component, text_sensor.TextSensor
)
SnapclientStreamFormatTextSensor = snapclient_ns.class_(
    "SnapclientStreamFormatTextSensor", cg.Component, text_sensor.TextSensor
)
SnapclientMetadataTextSensor = snapclient_ns.class_(
    "SnapclientMetadataTextSensor", cg.Component, text_sensor.TextSensor
)
MetadataField = snapclient_ns.enum("MetadataField", is_class=True)

# Stream metadata from the persistent control session (empty when the server's
# control port is disabled or the stream carries no metadata)
_METADATA_TYPES = {
    "stream_name": (MetadataField.STREAM_NAME, "mdi:playlist-music"),
    "stream_title": (MetadataField.TITLE, "mdi:music-note"),
    "stream_artist": (MetadataField.ARTIST, "mdi:account-music"),
    "stream_album": (MetadataField.ALBUM, "mdi:album"),
}


def _hub_schema(schema):
    return schema.extend(
        {
            cv.GenerateID(CONF_SNAPCLIENT_ID): cv.use_id(SnapclientHub),
        }
    )


CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
            # "Consensus" / "Solo" / "Inactive" — whether the TSF timebase this
            # device plays to is shared with anyone. There is no leader: every device
            # publishes its own server→TSF estimate and adopts the average. Solo means
            # nothing else is audible on this AP/server/stream; Inactive when tsf_sync
            # is off, no wifi, no session, or no mapping yet
            CONF_TSF_STATE: _hub_schema(
                text_sensor.text_sensor_schema(
                    SnapclientTsfStateTextSensor,
                    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                    icon="mdi:account-group",
                )
            ),
            # The format snapserver is actually sending, e.g. "48000 Hz, 16 bit, 2 ch".
            # Negotiated from the codec header at runtime, not fixed by the YAML, so
            # this is how you check that a `media_pipeline` matches the server -- a
            # mismatch is survivable but reconfigures the speaker on every stream.
            CONF_STREAM_FORMAT: _hub_schema(
                text_sensor.text_sensor_schema(
                    SnapclientStreamFormatTextSensor,
                    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                    icon="mdi:waveform",
                )
            ),
            **{
                key: _hub_schema(
                    text_sensor.text_sensor_schema(
                        SnapclientMetadataTextSensor, icon=icon
                    )
                )
                for key, (_, icon) in _METADATA_TYPES.items()
            },
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

    if config[CONF_TYPE] in _METADATA_TYPES:
        cg.add(var.set_field(_METADATA_TYPES[config[CONF_TYPE]][0]))
