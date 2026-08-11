#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "messages.pb.h"

namespace pickpoint::tracking {

inline constexpr const char* kSubprotocol = "tracking.v2.proto";
inline constexpr int kMaxPublishHz = 50;
inline constexpr auto kMinPublishInterval = std::chrono::milliseconds(1000 / kMaxPublishHz);
inline constexpr int kMaxEventBytes = 4 * 1024;
inline constexpr int kMaxEventHz = 1;
inline constexpr auto kMinEventInterval = std::chrono::seconds(1);

enum class ConnectionState { kConnecting, kOpen, kReconnecting, kClosed };

inline const char* to_string(ConnectionState s) {
  switch (s) {
    case ConnectionState::kConnecting:
      return "connecting";
    case ConnectionState::kOpen:
      return "open";
    case ConnectionState::kReconnecting:
      return "reconnecting";
    case ConnectionState::kClosed:
      return "closed";
  }
  return "closed";
}

struct DeviceAuth {
  std::string client_id;
  std::string client_secret;
};

struct ListenerAuth {
  std::string access_token;
};

using RefreshAuthFn = std::function<std::pair<std::optional<DeviceAuth>, std::optional<ListenerAuth>>()>;

struct Config {
  std::string endpoint;
  std::optional<DeviceAuth> device;
  std::optional<ListenerAuth> listener;
  std::string ws_path;
  bool disable_reconnect = false;
  std::chrono::milliseconds reconnect_min_delay{0};
  std::chrono::milliseconds reconnect_max_delay{0};
  int reconnect_max_attempts = 0;
  RefreshAuthFn refresh_auth;
  int max_queue_size = 10'000;
  std::chrono::milliseconds hello_timeout{10'000};
};

class Error : public std::runtime_error {
 public:
  Error(::tracking::v2::ErrorCode code, std::string message)
      : std::runtime_error(message.empty() ? "tracking error" : message),
        code(code),
        message(std::move(message)) {}
  ::tracking::v2::ErrorCode code;
  std::string message;
};

struct BackoffState {
  int attempt = 0;
  std::chrono::milliseconds min_delay{500};
  std::chrono::milliseconds max_delay{30'000};
  int max_attempts = 0;
};

BackoffState new_backoff(std::chrono::milliseconds min_delay, std::chrono::milliseconds max_delay,
                         int max_attempts);
std::optional<std::chrono::milliseconds> next_delay(BackoffState& state, double rnd);
void reset_backoff(BackoffState& state);

struct QueuedPoint {
  std::uint64_t seq = 0;
  ::tracking::v2::LatLng point;
};

class OfflineQueue {
 public:
  explicit OfflineQueue(int max_size = 10'000);
  int size() const;
  int enqueue(std::uint64_t seq, const ::tracking::v2::LatLng& point);
  void ack_through(std::uint64_t ack);
  std::vector<QueuedPoint> peek_all() const;
  void clear();

 private:
  int max_size_;
  std::vector<QueuedPoint> items_;
};

bool can_accept_publish(std::chrono::steady_clock::time_point next_allowed_at,
                        std::chrono::steady_clock::time_point now, int point_count = 1);
std::chrono::steady_clock::time_point next_publish_allowed_at(
    std::chrono::steady_clock::time_point next_allowed_at, std::chrono::steady_clock::time_point now,
    int point_count = 1);

std::string build_ws_url(const Config& cfg);

void stamp_lat_lng(::tracking::v2::LatLng* p);
std::string encode_client_msg(const ::tracking::v2::ClientMsg& msg);
::tracking::v2::ServerMsg decode_server_msg(const std::string& data);
::tracking::v2::ClientMsg client_resume(const std::string& track_uid, std::uint64_t last_client_seq);

class Client {
 public:
  static std::unique_ptr<Client> connect(Config cfg);

  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  ConnectionState state() const;
  std::string track_uid() const;
  std::uint64_t client_seq() const;
  std::uint64_t last_acked_seq() const;

  std::string start_track(const ::tracking::v2::LatLng* loc = nullptr,
                          const std::vector<::tracking::v2::LatLng>& route = {},
                          const std::string& metadata = {});
  std::uint64_t resume(const std::string& track_uid, std::uint64_t last_client_seq);
  std::pair<std::uint64_t, bool> publish(const ::tracking::v2::LatLng& point);
  void stop_track(const std::string& track_uid = {});
  bool send_event(const std::string& payload);
  void subscribe(const std::string& device_uid);
  void send(const ::tracking::v2::ClientMsg& msg);
  bool recv(::tracking::v2::ServerMsg& out, std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});
  void close();

 private:
  explicit Client(Config cfg);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pickpoint::tracking
