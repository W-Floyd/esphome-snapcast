#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/core/helpers.h"

#include <cstdint>
#include <string>
#include <vector>

namespace esphome::snapclient {

/// @brief Stream metadata for this client's group, from the server's control API.
struct StreamMetadata {
  std::string stream_name;  // stream id on the server (its configured name)
  std::string title;
  std::string artist;  // multiple artists joined with ", "
  std::string album;

  bool operator==(const StreamMetadata &o) const {
    return this->stream_name == o.stream_name && this->title == o.title && this->artist == o.artist &&
           this->album == o.album;
  }
  bool operator!=(const StreamMetadata &o) const { return !(*this == o); }
};

/// @brief Field selector for the metadata text sensors.
enum class MetadataField : uint8_t { STREAM_NAME, TITLE, ARTIST, ALBUM };

/// @brief Persistent JSON-RPC session on the snapserver control port (1705).
///
/// Replaces per-call sockets: one connection alongside the stream carries
/// Server.GetStatus (this client's group -> stream -> metadata, plus the
/// connected-client roster for TSF unicast beacons), receives the server's
/// notifications (Stream.OnUpdate, Group.OnStreamChanged, Server.OnUpdate,
/// Client.On*) live, and sends control calls (Client.SetLatency).
///
/// Entirely non-blocking (non-blocking connect + drained reads), driven from the
/// network task's service tick; reconnects with backoff while a stream session is
/// up. The control port is optional server-side ([tcp] can be disabled): callers
/// must treat connected() == false as "fall back to one-shot sockets / no metadata".
///
/// THREAD CONTEXT: service(), send_set_latency(), take_peers(): network task.
/// take_metadata(): main loop (mutex-guarded handoff).
class ControlSession {
 public:
  explicit ControlSession(std::string client_id) : client_id_(std::move(client_id)) {}
  ~ControlSession() { this->close(); }

  /// @brief Drives connect/read/dispatch. @p host: the active stream session's
  /// server (empty tears the session down).
  void service(int64_t now_us, const std::string &host);

  /// @brief Tears the session down (stream session ended).
  void close();

  bool connected() const { return this->state_ == State::READY; }

  /// @brief Sends Client.SetLatency through the session.
  /// @return false when not connected (caller falls back to a one-shot socket).
  bool send_set_latency(int32_t latency_ms, int64_t now_us);

  /// @brief Fetches the latest metadata if it changed since the last call.
  /// @brief Takes the name of the stream this client's group is playing, when it has changed.
  ///
  /// Separate from take_metadata() on purpose: the identity is known whenever the server status
  /// resolves this client into a group, whereas metadata is optional and a process stream has
  /// none. Tying the two together left the TSF stream scope unset for every metadata-less stream.
  /// @return true when a new identity was taken; `out` is untouched otherwise.
  bool take_stream_identity(std::string &out);

  bool take_metadata(StreamMetadata &out);

  /// @brief Fetches the latest connected-client roster (sockaddr s_addr values)
  /// if it changed since the last call.
  bool take_peers(std::vector<uint32_t> &out);

 protected:
  enum class State : uint8_t { IDLE, CONNECTING, READY };

  void fail_(int64_t now_us, const char *reason);
  void start_connect_(int64_t now_us);
  void poll_connect_(int64_t now_us);
  void read_(int64_t now_us);
  void handle_line_(const std::string &line, int64_t now_us);
  // ArduinoJson types stay out of this header (it is included via the client
  // everywhere); the void* parameters are JsonVariant*, cast inside the .cpp.
  /// Parses a full server status object (GetStatus result / Server.OnUpdate).
  void parse_server_(const void *server_variant);
  /// Parses one stream object: id + properties.metadata (GetStatus, Stream.OnUpdate).
  void handle_stream_(const void *stream_variant);
  /// Stores a metadata object, whichever notification shape carried it.
  void set_metadata_(const char *stream_name, const void *metadata_variant);
  void request_status_() { this->status_pending_ = true; }
  bool send_raw_(const char *data, size_t len, int64_t now_us);

  const std::string client_id_;

  int sock_{-1};
  State state_{State::IDLE};
  std::string host_;
  int64_t next_connect_us_{0};
  int64_t connect_deadline_us_{0};
  bool was_ready_{false};  // log connect/disconnect once per transition

  std::string rx_buf_;
  bool discarding_{false};  // oversize line: drop until the next newline

  bool status_pending_{false};
  int64_t last_status_req_us_{0};
  bool warned_not_found_{false};  // once per session

  // Learned identity of this client on the server
  std::string group_id_;
  std::string stream_id_;

  // Handoffs (metadata crosses to the main loop; peers stay on the network task
  // but share the mutex for simplicity)
  Mutex mutex_;
  StreamMetadata metadata_;
  bool metadata_dirty_{false};
  std::string stream_identity_;
  bool stream_identity_dirty_{false};
  std::vector<uint32_t> peers_;
  bool peers_dirty_{false};
};

}  // namespace esphome::snapclient

#endif  // USE_ESP32
