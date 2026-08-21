"""Virtual speaker for QEMU testing: consumes audio at exactly the sample rate and
feeds the audio_output_callback, standing in for an I2S DAC that QEMU cannot emulate."""

import esphome.codegen as cg
from esphome.components import speaker
import esphome.config_validation as cv
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_ID,
    CONF_NUM_CHANNELS,
    CONF_SAMPLE_RATE,
)

AUTO_LOAD = ["audio", "ring_buffer"]
CODEOWNERS = ["@W-Floyd"]
DEPENDENCIES = ["esp32"]

virtual_speaker_ns = cg.esphome_ns.namespace("virtual_speaker")
VirtualSpeaker = virtual_speaker_ns.class_(
    "VirtualSpeaker", cg.Component, speaker.Speaker
)

CONFIG_SCHEMA = speaker.SPEAKER_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(VirtualSpeaker),
        cv.Optional(CONF_SAMPLE_RATE, default=48000): cv.int_range(8000, 48000),
        cv.Optional(CONF_NUM_CHANNELS, default=2): cv.int_range(1, 2),
        cv.Optional(CONF_BITS_PER_SAMPLE, default=16): cv.one_of(16, int=True),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)
    cg.add(var.set_stream_params(config[CONF_SAMPLE_RATE], config[CONF_NUM_CHANNELS]))
