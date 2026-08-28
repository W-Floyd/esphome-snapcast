#include "snapclient_text_sensor.h"

#if defined(USE_ESP32) && defined(USE_TEXT_SENSOR)

#include "esphome/core/log.h"

#include <cinttypes>
#include <cstdio>

namespace esphome::snapclient {

static const char *const TAG = "snapclient.text_sensor";

void SnapclientTsfStateTextSensor::publish_() {
  const TsfState state = this->parent_->get_tsf_state();
  if (this->published_ && state == this->last_state_) {
    return;
  }
  this->published_ = true;
  this->last_state_ = state;
  switch (state) {
    // There is no leader to be. What this reports is whether the timebase this device plays to is
    // shared with anybody: "Consensus" is the working state, "Solo" means it is running on its own
    // estimate because nothing else is audible on this AP/server/stream.
    case TsfState::CONSENSUS:
      this->publish_state("Consensus");
      break;
    case TsfState::SOLO:
      this->publish_state("Solo");
      break;
    default:
      this->publish_state("Inactive");
      break;
  }
}

void SnapclientTsfStateTextSensor::dump_config() { LOG_TEXT_SENSOR("", "Snapclient TSF State Text Sensor", this); }

void SnapclientStreamFormatTextSensor::setup() {
  this->parent_->add_stream_state_callback(
      [this](bool active, const StreamParams &params) { this->publish_(active, params); });
  this->publish_(false, StreamParams{});  // known-empty until a codec header arrives
}

// THREAD CONTEXT: Main loop (hub callback)
void SnapclientStreamFormatTextSensor::publish_(bool active, const StreamParams &params) {
  std::string value;
  if (active && params.valid()) {
    char buffer[40];
    snprintf(buffer, sizeof(buffer), "%" PRIu32 " Hz, %u bit, %u ch", params.sample_rate, params.bits_per_sample,
             params.channels);
    value = buffer;
  }
  if (this->published_ && value == this->last_) {
    return;
  }
  this->published_ = true;
  this->last_ = std::move(value);
  this->publish_state(this->last_);
}

void SnapclientStreamFormatTextSensor::dump_config() {
  LOG_TEXT_SENSOR("", "Snapclient Stream Format Text Sensor", this);
}

void SnapclientMetadataTextSensor::setup() {
  this->parent_->add_metadata_callback([this](const StreamMetadata &metadata) { this->publish_(metadata); });
  this->publish_(StreamMetadata{});  // known-empty until the control session reports
}

// THREAD CONTEXT: Main loop (hub callback)
void SnapclientMetadataTextSensor::publish_(const StreamMetadata &metadata) {
  const std::string *value;
  switch (this->field_) {
    case MetadataField::STREAM_NAME:
      value = &metadata.stream_name;
      break;
    case MetadataField::ARTIST:
      value = &metadata.artist;
      break;
    case MetadataField::ALBUM:
      value = &metadata.album;
      break;
    default:
      value = &metadata.title;
      break;
  }
  if (this->published_ && *value == this->last_) {
    return;
  }
  this->published_ = true;
  this->last_ = *value;
  this->publish_state(this->last_);
}

void SnapclientMetadataTextSensor::dump_config() { LOG_TEXT_SENSOR("", "Snapclient Metadata Text Sensor", this); }

}  // namespace esphome::snapclient

#endif  // USE_ESP32 && USE_TEXT_SENSOR
