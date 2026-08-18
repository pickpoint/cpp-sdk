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

namespace pickpoint::tracking {

inline constexpr std::uint8_t kProtocolVersion = 2;
inline constexpr const char* kSubprotocol = "tracking.v2";
inline constexpr const char* kDefaultWsPath = "/v2/ws";

inline constexpr int kMaxPublishHz = 50;
inline constexpr auto kMinPublishInterval = std::chrono::milliseconds(1000 / kMaxPublishHz);
inline constexpr int kMaxEventBytes = 4 * 1024;
inline constexpr int kMaxEventHz = 1;
inline constexpr auto kMinEventInterval = std::chrono::seconds(1);

inline constexpr int kMaxStringBytes = 4096;
inline constexpr int kMaxLocPoints = 100;
inline constexpr int kMaxBufferPoints = 10'000;
inline constexpr int kMaxInFlightFrames = 8;

inline constexpr std::uint8_t kTypeResume = 0x01;
inline constexpr std::uint8_t kTypeTrackStart = 0x02;
inline constexpr std::uint8_t kTypeTrackStop = 0x03;
inline constexpr std::uint8_t kTypeLoc = 0x04;
inline constexpr std::uint8_t kTypeSubscribe = 0x05;
inline constexpr std::uint8_t kTypeUnsubscribe = 0x06;
inline constexpr std::uint8_t kTypeEvent = 0x07;
inline constexpr std::uint8_t kTypeCommandAck = 0x08;

inline constexpr std::uint8_t kTypeHello = 0x80;
inline constexpr std::uint8_t kTypeRelocate = 0x81;
inline constexpr std::uint8_t kTypeResumeOk = 0x82;
inline constexpr std::uint8_t kTypeTrackStarted = 0x83;
inline constexpr std::uint8_t kTypeTrackStopped = 0x84;
inline constexpr std::uint8_t kTypeAck = 0x85;
inline constexpr std::uint8_t kTypeServerLoc = 0x86;
inline constexpr std::uint8_t kTypeSubscribed = 0x87;
inline constexpr std::uint8_t kTypeError = 0x88;
inline constexpr std::uint8_t kTypeEventAdded = 0x89;
inline constexpr std::uint8_t kTypeCommand = 0x8A;
inline constexpr std::uint8_t kTypePresence = 0x8B;

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

enum class ErrorCode : std::uint8_t {
  kUnspecified = 0,
  kAuth = 1,
  kTrackNotFound = 2,
  kFenced = 3,
  kTryAgain = 4,
  kInvalid = 5,
  kUnauthorized = 6,
};

inline const char* to_string(ErrorCode c) {
  switch (c) {
    case ErrorCode::kAuth:
      return "AUTH";
    case ErrorCode::kTrackNotFound:
      return "TRACK_NOT_FOUND";
    case ErrorCode::kFenced:
      return "FENCED";
    case ErrorCode::kTryAgain:
      return "TRY_AGAIN";
    case ErrorCode::kInvalid:
      return "INVALID";
    case ErrorCode::kUnauthorized:
      return "UNAUTHORIZED";
    default:
      return "UNSPECIFIED";
  }
}

inline bool is_fatal_resume_error(ErrorCode code) {
  return code == ErrorCode::kTrackNotFound || code == ErrorCode::kAuth;
}

inline bool is_retry_resume_error(ErrorCode code) {
  return code == ErrorCode::kFenced || code == ErrorCode::kTryAgain;
}

inline bool is_auth_error(ErrorCode code) {
  return code == ErrorCode::kAuth || code == ErrorCode::kUnauthorized;
}

enum class CommandAckStatus : std::uint8_t {
  kUnspecified = 0,
  kOk = 1,
  kRejected = 2,
  kFailed = 3,
};

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
  std::vector<std::string> subscribe;
};

class Error : public std::runtime_error {
 public:
  Error(ErrorCode code, std::string message, std::uint32_t retry_after_ms = 0,
        std::string track_uid = {})
      : std::runtime_error(message.empty() ? "tracking error" : message),
        code(code),
        message(std::move(message)),
        retry_after_ms(retry_after_ms),
        track_uid(std::move(track_uid)) {}
  ErrorCode code;
  std::string message;
  std::uint32_t retry_after_ms = 0;
  std::string track_uid;
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

struct LatLng {
  double latitude = 0;
  double longitude = 0;
  std::optional<double> altitude;
  std::optional<double> accuracy;
  std::optional<double> heading;
  std::optional<double> speed;
  std::optional<std::int64_t> timestamp_ms;
};

struct InFlightPoint {
  std::uint32_t seq = 0;
  LatLng point;
};

class Buffer {
 public:
  explicit Buffer(int max_size = 10'000, std::function<void(int)> on_gap = {});
  int size() const;
  int staging_size() const;
  int in_flight_size() const;
  void push_staging(const LatLng& p);
  void push_in_flight(InFlightPoint p);
  void ack_through(std::uint32_t ack);
  std::vector<LatLng> peek_staging() const;
  std::vector<InFlightPoint> peek_in_flight() const;
  std::vector<InFlightPoint> assign_from_staging(std::uint64_t* next_seq, int max_frames);
  void clear();

 private:
  void enforce_cap();
  bool collapse_middle();

  int max_size_;
  std::function<void(int)> on_gap_;
  std::vector<LatLng> staging_;
  std::vector<InFlightPoint> in_flight_;
};

double haversine_m(const LatLng& a, const LatLng& b);
double perp_dist_m(const LatLng& a, const LatLng& c, const LatLng& b);

class NoiseFilter {
 public:
  std::pair<LatLng, bool> push(const LatLng& p, std::chrono::system_clock::time_point now);
  void reset();

 private:
  void emit(const LatLng& p, std::chrono::system_clock::time_point t);
  bool heading_jump(const LatLng& prev, const LatLng& cur) const;
  bool motion_edge(const LatLng& prev, const LatLng& cur) const;

  std::optional<LatLng> last_emitted_;
  std::optional<LatLng> candidate_;
  std::chrono::system_clock::time_point last_emit_at_{};
};

bool can_accept_publish(std::chrono::steady_clock::time_point next_allowed_at,
                        std::chrono::steady_clock::time_point now, int point_count = 1);
std::chrono::steady_clock::time_point next_publish_allowed_at(
    std::chrono::steady_clock::time_point next_allowed_at, std::chrono::steady_clock::time_point now,
    int point_count = 1);

std::string build_ws_url(const Config& cfg);

using Bytes = std::vector<std::uint8_t>;

struct Resume {
  std::string track_uid;
  std::uint32_t last_seq = 0;
};

struct TrackStart {
  std::optional<LatLng> location;
  std::vector<LatLng> route;
  Bytes metadata;
};

struct TrackStop {};

struct Loc {
  std::uint32_t seq = 0;
  std::vector<LatLng> points;
};

struct Subscribe {
  std::string device_uid;
  bool include_events = true;
  std::uint16_t min_interval_ms = 0;
};

struct Unsubscribe {
  std::uint8_t sub = 0;
};

struct Event {
  Bytes payload;
  std::int64_t timestamp_ms = 0;
};

struct CommandAck {
  std::string command_id;
  CommandAckStatus status = CommandAckStatus::kUnspecified;
  std::string message;
};

struct ClientMsg {
  std::optional<Resume> resume;
  std::optional<TrackStart> track_start;
  std::optional<TrackStop> track_stop;
  std::optional<Loc> loc;
  std::optional<Subscribe> subscribe;
  std::optional<Unsubscribe> unsubscribe;
  std::optional<Event> event;
  std::optional<CommandAck> command_ack;
  std::optional<std::uint8_t> unknown;
};

struct Hello {
  std::uint8_t version = 0;
  std::uint16_t shard = 0;
  std::string node_id;
};

struct Relocate {
  std::uint32_t retry_after_ms = 0;
  std::string endpoint;
};

struct ResumeOk {
  std::string track_uid;
  std::uint32_t last_acked = 0;
};

struct TrackStarted {
  std::string track_uid;
  Bytes metadata;
};

struct TrackStopped {
  std::string track_uid;
};

struct Ack {
  std::uint32_t seq = 0;
};

struct ServerLoc {
  std::uint8_t sub = 0;
  std::uint32_t seq = 0;
  LatLng point;
};

struct Subscribed {
  std::uint8_t sub = 0;
  std::string device_uid;
  std::string track_uid;
  bool online = false;
  std::optional<LatLng> last_location;
  std::optional<std::int64_t> last_seen_ms;
  std::vector<LatLng> route;
  double est_distance = 0;
  double est_duration = 0;
  std::string start_name;
  std::string end_name;
  Bytes metadata;
};

struct WireError {
  ErrorCode code = ErrorCode::kInvalid;
  std::uint32_t retry_after_ms = 0;
  std::string track_uid;
  std::string message;
};

struct EventAdded {
  std::uint8_t sub = 0;
  Bytes payload;
  std::int64_t timestamp_ms = 0;
};

struct Command {
  std::string command_id;
  Bytes payload;
  std::int64_t timestamp_ms = 0;
};

struct Presence {
  std::uint8_t sub = 0;
  bool online = false;
  std::int64_t last_seen_ms = 0;
};

struct ServerMsg {
  std::optional<Hello> hello;
  std::optional<Relocate> relocate;
  std::optional<ResumeOk> resume_ok;
  std::optional<TrackStarted> track_started;
  std::optional<TrackStopped> track_stopped;
  std::optional<Ack> ack;
  std::optional<ServerLoc> loc;
  std::optional<Subscribed> subscribed;
  std::optional<WireError> error;
  std::optional<EventAdded> event_added;
  std::optional<Command> command;
  std::optional<Presence> presence;

  bool empty() const {
    return !hello && !relocate && !resume_ok && !track_started && !track_stopped && !ack && !loc &&
           !subscribed && !error && !event_added && !command && !presence;
  }
};

void stamp_lat_lng(LatLng* p);
std::int32_t deg_to_micro(double d);
double micro_to_deg(std::int32_t m);
bool micro_delta_fits(std::int32_t prev_lat, std::int32_t prev_lon, std::int32_t lat,
                      std::int32_t lon);

Bytes encode_client_msg(const ClientMsg& msg);
ClientMsg decode_client_msg(const Bytes& data);
ClientMsg decode_client_msg(const std::string& data);
Bytes encode_server_msg(const ServerMsg& msg);
ServerMsg decode_server_msg(const Bytes& data);
ServerMsg decode_server_msg(const std::string& data);
ClientMsg client_resume(const std::string& track_uid, std::uint32_t last_seq);
std::vector<Bytes> encode_loc_frames(std::uint32_t last_seq, const std::vector<LatLng>& points);
std::vector<Bytes> encode_in_flight_frames(const std::vector<InFlightPoint>& pts);

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

  std::string start_track(const LatLng* loc = nullptr, const std::vector<LatLng>& route = {},
                          const std::string& metadata = {});
  std::uint64_t resume(const std::string& track_uid, std::uint64_t last_client_seq);
  std::pair<std::uint64_t, bool> publish(const LatLng& point);
  void stop_track(const std::string& track_uid = {});
  bool send_event(const std::string& payload);
  void subscribe(const std::string& device_uid);
  void subscribe(const std::string& device_uid, bool include_events,
                 std::uint16_t min_interval_ms);
  void unsubscribe(std::uint8_t sub);
  void send(const ClientMsg& msg);
  bool recv(ServerMsg& out, std::chrono::milliseconds timeout = std::chrono::milliseconds{-1});
  void close();

 private:
  explicit Client(Config cfg);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace pickpoint::tracking
