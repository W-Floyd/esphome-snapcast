#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <cstddef>
#include <cstdint>
#include <string>

namespace esphome::snapclient {

/// Snapcast binary protocol message types (SnapStreamProtocolVersion 2).
enum class MessageType : uint16_t {
  BASE = 0,
  CODEC_HEADER = 1,
  WIRE_CHUNK = 2,
  SERVER_SETTINGS = 3,
  TIME = 4,
  HELLO = 5,
  STREAM_TAGS = 6,
  CLIENT_INFO = 7,
};

/// Snapcast wire timestamp: seconds + microseconds, both little-endian int32.
struct TimeVal {
  int32_t sec{0};
  int32_t usec{0};

  static TimeVal from_us(int64_t us) {
    return TimeVal{static_cast<int32_t>(us / 1000000), static_cast<int32_t>(us % 1000000)};
  }
  int64_t to_us() const { return static_cast<int64_t>(this->sec) * 1000000 + this->usec; }
};

/// 26-byte little-endian header preceding every Snapcast message.
struct BaseMessage {
  uint16_t type{0};
  uint16_t id{0};
  uint16_t refers_to{0};
  TimeVal sent{};
  TimeVal received{};
  uint32_t size{0};  // payload size in bytes

  static constexpr size_t WIRE_SIZE = 26;

  /// Serializes into @p out, which must hold at least WIRE_SIZE bytes.
  void serialize(uint8_t *out) const;
  /// Deserializes from @p in, which must hold at least WIRE_SIZE bytes.
  static BaseMessage deserialize(const uint8_t *in);
};

/// ServerSettings payload (JSON).
struct ServerSettings {
  int32_t buffer_ms{1000};
  int32_t latency{0};
  uint8_t volume{100};  // percent
  bool muted{false};

  /// @return true on successful parse.
  static bool parse(const char *json, size_t len, ServerSettings &out);
};

/// CodecHeader payload view: codec name + codec-specific initialization blob.
/// Points into the caller's receive buffer; valid only while that buffer is.
struct CodecHeaderView {
  const char *codec{nullptr};
  size_t codec_len{0};
  const uint8_t *payload{nullptr};
  size_t payload_len{0};

  /// @return true if @p data of @p size parsed cleanly.
  static bool parse(const uint8_t *data, size_t size, CodecHeaderView &out);

  bool codec_is(const char *name) const;
};

/// WireChunk payload view: server timestamp + encoded audio.
/// Points into the caller's receive buffer; valid only while that buffer is.
struct WireChunkView {
  TimeVal timestamp{};
  const uint8_t *payload{nullptr};
  size_t payload_len{0};

  static bool parse(const uint8_t *data, size_t size, WireChunkView &out);
};

/// Time payload: a single TimeVal. In the server's reply, this is the
/// client-to-server latency (server receive time - client send time).
struct TimePayload {
  TimeVal latency{};

  static constexpr size_t WIRE_SIZE = 8;

  void serialize(uint8_t *out) const;
  static bool parse(const uint8_t *data, size_t size, TimePayload &out);
};

/// Builds the length-prefixed Hello JSON payload.
/// @param mac Pretty MAC address, doubles as the client ID.
/// @param hostname Display hostname reported to the server.
std::string build_hello_payload(const char *mac, const std::string &hostname);

/// Builds the length-prefixed ClientInfo JSON payload reporting local volume state.
std::string build_client_info_payload(uint8_t volume_percent, bool muted);

}  // namespace esphome::snapclient

#endif  // USE_ESP32
