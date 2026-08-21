#include "control_session.h"

#ifdef USE_ESP32

#include "esphome/components/json/json_util.h"
#include "esphome/core/log.h"

#include <fcntl.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include <cstdio>
#include <cstring>

namespace esphome::snapclient {

static const char *const TAG = "snapclient.control";

static constexpr int64_t RECONNECT_BACKOFF_US = 5000000;
static constexpr int64_t CONNECT_TIMEOUT_US = 5000000;
// Notifications can arrive in bursts (many clients); coalesce status re-fetches
static constexpr int64_t STATUS_MIN_INTERVAL_US = 2000000;
// A GetStatus response for a large installation can be tens of KB on one line
static constexpr size_t RX_LINE_CAP = 32768;

void ControlSession::close() {
  if (this->sock_ >= 0) {
    ::close(this->sock_);
    this->sock_ = -1;
  }
  this->warned_not_found_ = false;
  if (this->was_ready_) {
    this->was_ready_ = false;
    ESP_LOGD(TAG, "Control session closed");
  }
  this->state_ = State::IDLE;
  this->rx_buf_.clear();
  this->discarding_ = false;
  this->status_pending_ = false;
  this->host_.clear();
  this->next_connect_us_ = 0;
}

void ControlSession::fail_(int64_t now_us, const char *reason) {
  if (this->sock_ >= 0) {
    ::close(this->sock_);
    this->sock_ = -1;
  }
  if (this->was_ready_) {
    this->was_ready_ = false;
    ESP_LOGD(TAG, "Control session lost (%s); retrying", reason);
  }
  this->state_ = State::IDLE;
  this->rx_buf_.clear();
  this->discarding_ = false;
  this->status_pending_ = false;
  this->next_connect_us_ = now_us + RECONNECT_BACKOFF_US;
}

void ControlSession::start_connect_(int64_t now_us) {
  struct addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *res = nullptr;
  // Blocking only for hostname targets; the usual case is an IP literal (instant)
  if (getaddrinfo(this->host_.c_str(), "1705", &hints, &res) != 0 || res == nullptr) {
    this->fail_(now_us, "resolve");
    return;
  }
  this->sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (this->sock_ < 0) {
    freeaddrinfo(res);
    this->fail_(now_us, "socket");
    return;
  }
  fcntl(this->sock_, F_SETFL, fcntl(this->sock_, F_GETFL, 0) | O_NONBLOCK);
  const int rc = connect(this->sock_, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);
  if (rc == 0) {
    this->state_ = State::READY;
    this->was_ready_ = true;
    this->request_status_();
    ESP_LOGD(TAG, "Control session connected to %s:1705", this->host_.c_str());
  } else if (errno == EINPROGRESS) {
    this->state_ = State::CONNECTING;
    this->connect_deadline_us_ = now_us + CONNECT_TIMEOUT_US;
  } else {
    this->fail_(now_us, "connect");
  }
}

void ControlSession::poll_connect_(int64_t now_us) {
  fd_set wfds;
  FD_ZERO(&wfds);
  FD_SET(this->sock_, &wfds);
  struct timeval tv = {.tv_sec = 0, .tv_usec = 0};
  const int r = select(this->sock_ + 1, nullptr, &wfds, nullptr, &tv);
  if (r > 0) {
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(this->sock_, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err == 0) {
      this->state_ = State::READY;
      this->was_ready_ = true;
      this->request_status_();
      ESP_LOGD(TAG, "Control session connected to %s:1705", this->host_.c_str());
    } else {
      this->fail_(now_us, "connect");
    }
  } else if (now_us > this->connect_deadline_us_) {
    this->fail_(now_us, "connect timeout");
  }
}

bool ControlSession::send_raw_(const char *data, size_t len, int64_t now_us) {
  const ssize_t n = send(this->sock_, data, len, 0);
  if (n != static_cast<ssize_t>(len)) {
    // Control traffic is tiny and rare; a short/failed send means a broken session
    this->fail_(now_us, "send");
    return false;
  }
  return true;
}

void ControlSession::service(int64_t now_us, const std::string &host) {
  if (host.empty()) {
    if (this->state_ != State::IDLE || this->sock_ >= 0) {
      this->close();
    }
    return;
  }
  if (host != this->host_) {
    this->close();
    this->host_ = host;
  }
  switch (this->state_) {
    case State::IDLE:
      if (now_us >= this->next_connect_us_) {
        this->start_connect_(now_us);
      }
      break;
    case State::CONNECTING:
      this->poll_connect_(now_us);
      break;
    case State::READY:
      this->read_(now_us);
      if (this->state_ == State::READY && this->status_pending_ &&
          now_us - this->last_status_req_us_ >= STATUS_MIN_INTERVAL_US) {
        static const char REQ[] = "{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"Server.GetStatus\"}\r\n";
        if (this->send_raw_(REQ, sizeof(REQ) - 1, now_us)) {
          this->status_pending_ = false;
          this->last_status_req_us_ = now_us;
        }
      }
      break;
  }
}

void ControlSession::read_(int64_t now_us) {
  char buf[512];
  while (this->state_ == State::READY) {
    const int n = recv(this->sock_, buf, sizeof(buf), MSG_DONTWAIT);
    if (n == 0) {
      this->fail_(now_us, "closed by server");
      return;
    }
    if (n < 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        return;  // drained
      }
      this->fail_(now_us, "recv");
      return;
    }
    this->rx_buf_.append(buf, n);
    size_t pos;
    while ((pos = this->rx_buf_.find('\n')) != std::string::npos) {
      std::string line = this->rx_buf_.substr(0, pos);
      this->rx_buf_.erase(0, pos + 1);
      if (this->discarding_) {
        this->discarding_ = false;  // the truncated line's tail; drop it
        continue;
      }
      this->handle_line_(line, now_us);
    }
    if (this->rx_buf_.size() > RX_LINE_CAP) {
      ESP_LOGW(TAG, "Oversized control line (> %u bytes), dropping", (unsigned) RX_LINE_CAP);
      this->rx_buf_.clear();
      this->discarding_ = true;
    }
  }
}

/// One filter serves every line shape we care about: GetStatus responses,
/// Server.OnUpdate (same server object under params), Stream.OnUpdate, and
/// Group.OnStreamChanged. Everything else deserializes to nearly nothing.
static void build_filter(JsonDocument &f) {
  f["id"] = true;
  f["method"] = true;
  for (const char *root : {"result", "params"}) {
    // to<JsonObject>() CREATES the nested object: plain `JsonVariant v = f[x][y]`
    // is a read that binds nothing on a missing key, and writes through the
    // unbound variant vanish -- which silently emptied this whole filter branch
    // (GetStatus responses deserialized with `server` stripped, so group/stream/
    // metadata never populated while everything else looked healthy)
    JsonObject server = f[root]["server"].to<JsonObject>();
    server["groups"][0]["id"] = true;
    server["groups"][0]["stream_id"] = true;
    server["groups"][0]["clients"][0]["id"] = true;
    server["groups"][0]["clients"][0]["connected"] = true;
    server["groups"][0]["clients"][0]["host"]["ip"] = true;
    server["streams"][0]["id"] = true;
    server["streams"][0]["properties"]["metadata"] = true;
  }
  f["params"]["id"] = true;
  f["params"]["stream_id"] = true;
  f["params"]["stream"]["id"] = true;
  f["params"]["stream"]["properties"]["metadata"] = true;
}

void ControlSession::handle_line_(const std::string &line, int64_t now_us) {
  JsonDocument filter;
  build_filter(filter);
  JsonDocument doc;
  if (deserializeJson(doc, line, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
    return;
  }
  const char *method = doc["method"];
  if (method != nullptr) {
    if (strcmp(method, "Stream.OnUpdate") == 0) {
      JsonVariant stream = doc["params"]["stream"];
      const char *sid = stream["id"];
      if (sid != nullptr && this->stream_id_ == sid) {
        this->handle_stream_(&stream);
      } else if (this->stream_id_.empty()) {
        this->request_status_();  // don't know our stream yet
      }
    } else if (strcmp(method, "Group.OnStreamChanged") == 0) {
      const char *gid = doc["params"]["id"];
      if (gid != nullptr && this->group_id_ == gid) {
        this->request_status_();  // our group switched streams
      }
    } else if (strcmp(method, "Server.OnUpdate") == 0) {
      JsonVariant server = doc["params"]["server"];
      this->parse_server_(&server);
    } else if (strcmp(method, "Client.OnConnect") == 0 || strcmp(method, "Client.OnDisconnect") == 0) {
      // Roster changed (regrouping arrives as Server.OnUpdate); volume/mute/name
      // notifications are deliberately ignored -- they carry nothing we surface
      this->request_status_();
    }
    return;
  }
  JsonVariant server = doc["result"]["server"];
  if (!server.isNull()) {
    this->parse_server_(&server);
  }
}

void ControlSession::parse_server_(const void *server_variant) {
  const JsonVariant &server = *static_cast<const JsonVariant *>(server_variant);
  if (server.isNull()) {
    return;
  }
  std::vector<uint32_t> peers;
  std::string my_stream, my_group;
  JsonArray groups = server["groups"];
  for (JsonVariant group : groups) {
    bool mine = false;
    JsonArray clients = group["clients"];
    for (JsonVariant client : clients) {
      const char *cid = client["id"];
      // Case-insensitive: we send the pretty (uppercase) MAC as our Hello ID, but
      // snapserver reports client ids as lowercase MACs in its status
      if (cid != nullptr && strcasecmp(cid, this->client_id_.c_str()) == 0) {
        mine = true;
      }
      if (!(client["connected"] | false)) {
        continue;
      }
      const char *ip = client["host"]["ip"];
      if (ip == nullptr) {
        continue;
      }
      if (strncmp(ip, "::ffff:", 7) == 0) {
        ip += 7;  // snapserver reports IPv4-mapped IPv6 addresses
      }
      const in_addr_t addr = inet_addr(ip);
      if (addr != INADDR_NONE) {
        peers.push_back(addr);  // may include ourselves; the beacon's mac check drops it
      }
    }
    if (mine) {
      const char *sid = group["stream_id"];
      my_stream = sid != nullptr ? sid : "";
      const char *gid = group["id"];
      my_group = gid != nullptr ? gid : "";
    }
  }
  if (my_group.empty()) {
    // Fires even without a state transition (empty -> empty is the silent
    // failure mode: never matched at all), once per session
    if (!this->warned_not_found_) {
      this->warned_not_found_ = true;
      ESP_LOGW(TAG, "This client not found in server status (id '%s'); no metadata", this->client_id_.c_str());
    }
  } else if (my_stream != this->stream_id_ || my_group != this->group_id_) {
    ESP_LOGD(TAG, "This client: group '%s', stream '%s'", my_group.c_str(), my_stream.c_str());
    this->warned_not_found_ = false;
  }
  this->group_id_ = my_group;
  this->stream_id_ = my_stream;

  JsonArray streams = server["streams"];
  for (JsonVariant stream : streams) {
    const char *sid = stream["id"];
    if (sid != nullptr && this->stream_id_ == sid) {
      this->handle_stream_(&stream);
      break;
    }
  }

  this->mutex_.lock();
  if (peers != this->peers_) {
    this->peers_ = std::move(peers);
    this->peers_dirty_ = true;
  }
  this->mutex_.unlock();
}

void ControlSession::handle_stream_(const void *stream_variant) {
  const JsonVariant &stream = *static_cast<const JsonVariant *>(stream_variant);
  StreamMetadata md;
  md.stream_name = stream["id"] | "";
  JsonVariant meta = stream["properties"]["metadata"];
  md.title = meta["title"] | "";
  md.album = meta["album"] | "";
  JsonVariant artist = meta["artist"];
  if (artist.is<const char *>()) {
    md.artist = artist.as<const char *>();
  } else {
    // Usually an array of artists; join for display
    JsonArray artists = artist;
    for (JsonVariant a : artists) {
      const char *name = a.as<const char *>();
      if (name == nullptr) {
        continue;
      }
      if (!md.artist.empty()) {
        md.artist += ", ";
      }
      md.artist += name;
    }
  }
  this->mutex_.lock();
  if (md != this->metadata_) {
    this->metadata_ = std::move(md);
    this->metadata_dirty_ = true;
  }
  this->mutex_.unlock();
}

bool ControlSession::send_set_latency(int32_t latency_ms, int64_t now_us) {
  if (this->state_ != State::READY) {
    return false;
  }
  char req[192];
  const int len = snprintf(req, sizeof(req),
                           "{\"id\":100,\"jsonrpc\":\"2.0\",\"method\":\"Client.SetLatency\","
                           "\"params\":{\"id\":\"%s\",\"latency\":%ld}}\r\n",
                           this->client_id_.c_str(), static_cast<long>(latency_ms));
  return this->send_raw_(req, len, now_us);
}

bool ControlSession::take_metadata(StreamMetadata &out) {
  this->mutex_.lock();
  const bool dirty = this->metadata_dirty_;
  if (dirty) {
    out = this->metadata_;
    this->metadata_dirty_ = false;
  }
  this->mutex_.unlock();
  return dirty;
}

bool ControlSession::take_peers(std::vector<uint32_t> &out) {
  this->mutex_.lock();
  const bool dirty = this->peers_dirty_;
  if (dirty) {
    out = this->peers_;
    this->peers_dirty_ = false;
  }
  this->mutex_.unlock();
  return dirty;
}

}  // namespace esphome::snapclient

#endif  // USE_ESP32
