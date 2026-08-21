#include "snapcast_proto.h"

#ifdef USE_ESP32

#include "esphome/components/json/json_util.h"
#include "esphome/core/version.h"

#include <cstring>

namespace esphome::snapclient {

// The ESP32 is little-endian like the wire format, but explicit byte access keeps
// the (de)serialization alignment-safe.
static void write_u16(uint8_t *out, uint16_t v) {
  out[0] = v & 0xFF;
  out[1] = (v >> 8) & 0xFF;
}
static void write_u32(uint8_t *out, uint32_t v) {
  out[0] = v & 0xFF;
  out[1] = (v >> 8) & 0xFF;
  out[2] = (v >> 16) & 0xFF;
  out[3] = (v >> 24) & 0xFF;
}
static uint16_t read_u16(const uint8_t *in) { return static_cast<uint16_t>(in[0]) | (static_cast<uint16_t>(in[1]) << 8); }
static uint32_t read_u32(const uint8_t *in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) | (static_cast<uint32_t>(in[2]) << 16) |
         (static_cast<uint32_t>(in[3]) << 24);
}
static int32_t read_i32(const uint8_t *in) { return static_cast<int32_t>(read_u32(in)); }

void BaseMessage::serialize(uint8_t *out) const {
  write_u16(out + 0, this->type);
  write_u16(out + 2, this->id);
  write_u16(out + 4, this->refers_to);
  write_u32(out + 6, static_cast<uint32_t>(this->sent.sec));
  write_u32(out + 10, static_cast<uint32_t>(this->sent.usec));
  write_u32(out + 14, static_cast<uint32_t>(this->received.sec));
  write_u32(out + 18, static_cast<uint32_t>(this->received.usec));
  write_u32(out + 22, this->size);
}

BaseMessage BaseMessage::deserialize(const uint8_t *in) {
  BaseMessage msg;
  msg.type = read_u16(in + 0);
  msg.id = read_u16(in + 2);
  msg.refers_to = read_u16(in + 4);
  msg.sent.sec = read_i32(in + 6);
  msg.sent.usec = read_i32(in + 10);
  msg.received.sec = read_i32(in + 14);
  msg.received.usec = read_i32(in + 18);
  msg.size = read_u32(in + 22);
  return msg;
}

bool ServerSettings::parse(const uint8_t *payload, size_t len, ServerSettings &out) {
  // Like all Snapcast JSON payloads, the JSON is preceded by a 4-byte length prefix
  if (len < 4) {
    return false;
  }
  const uint32_t json_len = read_u32(payload);
  if (json_len > len - 4) {
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, reinterpret_cast<const char *>(payload + 4), json_len) != DeserializationError::Ok ||
      !doc.is<JsonObjectConst>()) {
    return false;
  }
  JsonObjectConst root = doc.as<JsonObjectConst>();
  out.buffer_ms = root["bufferMs"] | out.buffer_ms;
  out.latency = root["latency"] | out.latency;
  out.volume = static_cast<uint8_t>(root["volume"] | static_cast<int>(out.volume));
  out.muted = root["muted"] | out.muted;
  return true;
}

bool CodecHeaderView::parse(const uint8_t *data, size_t size, CodecHeaderView &out) {
  if (size < 4) {
    return false;
  }
  uint32_t codec_len = read_u32(data);
  if (size < 4 + codec_len + 4) {
    return false;
  }
  out.codec = reinterpret_cast<const char *>(data + 4);
  out.codec_len = codec_len;
  uint32_t payload_len = read_u32(data + 4 + codec_len);
  if (size < 4 + codec_len + 4 + payload_len) {
    return false;
  }
  out.payload = data + 4 + codec_len + 4;
  out.payload_len = payload_len;
  return true;
}

bool CodecHeaderView::codec_is(const char *name) const {
  return strlen(name) == this->codec_len && memcmp(name, this->codec, this->codec_len) == 0;
}

bool WireChunkView::parse(const uint8_t *data, size_t size, WireChunkView &out) {
  if (size < 12) {
    return false;
  }
  out.timestamp.sec = read_i32(data);
  out.timestamp.usec = read_i32(data + 4);
  uint32_t payload_len = read_u32(data + 8);
  if (size < 12 + payload_len) {
    return false;
  }
  out.payload = data + 12;
  out.payload_len = payload_len;
  return true;
}

void TimePayload::serialize(uint8_t *out) const {
  write_u32(out, static_cast<uint32_t>(this->latency.sec));
  write_u32(out + 4, static_cast<uint32_t>(this->latency.usec));
}

bool TimePayload::parse(const uint8_t *data, size_t size, TimePayload &out) {
  if (size < WIRE_SIZE) {
    return false;
  }
  out.latency.sec = read_i32(data);
  out.latency.usec = read_i32(data + 4);
  return true;
}

/// Serializes @p doc with the 4-byte little-endian length prefix Snapcast uses for
/// JSON payloads.
static std::string length_prefixed(JsonDocument &doc) {
  std::string body;
  serializeJson(doc, body);
  std::string out;
  out.resize(4 + body.size());
  write_u32(reinterpret_cast<uint8_t *>(out.data()), body.size());
  memcpy(out.data() + 4, body.data(), body.size());
  return out;
}

std::string build_hello_payload(const char *mac, const std::string &hostname) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  root["MAC"] = mac;
  root["HostName"] = hostname;
  // The version of the snapclient protocol implementation we are compatible with,
  // not the ESPHome version (which goes in ClientName).
  root["Version"] = "0.17.1";
  root["ClientName"] = "ESPHome " ESPHOME_VERSION;
  root["OS"] = "esp-idf";
  root["Arch"] = "esp32";
  root["Instance"] = 1;
  root["ID"] = mac;
  root["SnapStreamProtocolVersion"] = 2;
  return length_prefixed(doc);
}

std::string build_client_info_payload(uint8_t volume_percent, bool muted) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  root["volume"] = volume_percent;
  root["muted"] = muted;
  return length_prefixed(doc);
}

}  // namespace esphome::snapclient

#endif  // USE_ESP32
