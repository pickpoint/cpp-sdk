#include "pickpoint/tracking.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace pickpoint::tracking {
namespace {

constexpr std::uint8_t kPfAlt = 1 << 0;
constexpr std::uint8_t kPfAcc = 1 << 1;
constexpr std::uint8_t kPfTime = 1 << 4;
constexpr std::int32_t kLatMin = -90'000'000;
constexpr std::int32_t kLatMax = 90'000'000;
constexpr std::int32_t kLonMin = -180'000'000;
constexpr std::int32_t kLonMax = 180'000'000;

[[noreturn]] void fail(const char* msg) { throw Error(ErrorCode::kInvalid, msg); }

struct Reader {
  const std::uint8_t* p = nullptr;
  std::size_t n = 0;

  void need(std::size_t k) {
    if (n < k) fail("truncated frame");
  }

  std::uint8_t u8() {
    need(1);
    auto v = p[0];
    ++p;
    --n;
    return v;
  }

  std::uint16_t u16() {
    need(2);
    std::uint16_t v = static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
    p += 2;
    n -= 2;
    return v;
  }

  std::uint32_t u32() {
    need(4);
    std::uint32_t v = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                      (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    p += 4;
    n -= 4;
    return v;
  }

  std::uint64_t u64() {
    need(8);
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    p += 8;
    n -= 8;
    return v;
  }

  std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
  std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
  std::int64_t i64() { return static_cast<std::int64_t>(u64()); }

  double f64() {
    std::uint64_t bits = u64();
    double out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
  }

  std::array<std::uint8_t, 16> raw_uuid() {
    need(16);
    std::array<std::uint8_t, 16> out{};
    std::memcpy(out.data(), p, 16);
    p += 16;
    n -= 16;
    return out;
  }

  std::string uuid();
  std::string uuid_opt();
  std::string str();
  Bytes bytes();
};

void put_u8(Bytes& w, std::uint8_t v) { w.push_back(v); }

void put_u16(Bytes& w, std::uint16_t v) {
  w.push_back(static_cast<std::uint8_t>(v));
  w.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put_u32(Bytes& w, std::uint32_t v) {
  w.push_back(static_cast<std::uint8_t>(v));
  w.push_back(static_cast<std::uint8_t>(v >> 8));
  w.push_back(static_cast<std::uint8_t>(v >> 16));
  w.push_back(static_cast<std::uint8_t>(v >> 24));
}

void put_u64(Bytes& w, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) w.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

void put_i16(Bytes& w, std::int16_t v) { put_u16(w, static_cast<std::uint16_t>(v)); }
void put_i32(Bytes& w, std::int32_t v) { put_u32(w, static_cast<std::uint32_t>(v)); }
void put_i64(Bytes& w, std::int64_t v) { put_u64(w, static_cast<std::uint64_t>(v)); }

void put_f64(Bytes& w, double v) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  put_u64(w, bits);
}

int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::array<std::uint8_t, 16> parse_uuid(const std::string& s) {
  std::array<std::uint8_t, 16> out{};
  if (s.empty()) return out;
  std::string h;
  h.reserve(s.size());
  for (char c : s) {
    if (c != '-') h.push_back(c);
  }
  if (h.size() != 32) return out;
  for (int i = 0; i < 16; ++i) {
    int hi = hex_nibble(h[static_cast<std::size_t>(i * 2)]);
    int lo = hex_nibble(h[static_cast<std::size_t>(i * 2 + 1)]);
    if (hi < 0 || lo < 0) return std::array<std::uint8_t, 16>{};
    out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return out;
}

std::string format_uuid(const std::array<std::uint8_t, 16>& b) {
  static const char* hex = "0123456789abcdef";
  std::string h;
  h.resize(32);
  for (int i = 0; i < 16; ++i) {
    h[static_cast<std::size_t>(i * 2)] = hex[b[static_cast<std::size_t>(i)] >> 4];
    h[static_cast<std::size_t>(i * 2 + 1)] = hex[b[static_cast<std::size_t>(i)] & 0xf];
  }
  return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) +
         "-" + h.substr(20, 12);
}

void put_uuid(Bytes& w, const std::string& s) {
  auto b = parse_uuid(s);
  w.insert(w.end(), b.begin(), b.end());
}

void put_str(Bytes& w, const std::string& s) {
  auto n = s.size();
  if (n > static_cast<std::size_t>(kMaxStringBytes)) n = static_cast<std::size_t>(kMaxStringBytes);
  put_u16(w, static_cast<std::uint16_t>(n));
  w.insert(w.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

void put_bytes(Bytes& w, const Bytes& b) {
  auto n = b.size();
  if (n > static_cast<std::size_t>(kMaxStringBytes)) n = static_cast<std::size_t>(kMaxStringBytes);
  put_u16(w, static_cast<std::uint16_t>(n));
  w.insert(w.end(), b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n));
}

std::string Reader::uuid() { return format_uuid(raw_uuid()); }

std::string Reader::uuid_opt() {
  auto raw = raw_uuid();
  for (auto x : raw) {
    if (x != 0) return format_uuid(raw);
  }
  return {};
}

std::string Reader::str() {
  auto len = u16();
  if (len > kMaxStringBytes) fail("invalid frame");
  need(len);
  std::string out(reinterpret_cast<const char*>(p), len);
  p += len;
  n -= len;
  return out;
}

Bytes Reader::bytes() {
  auto len = u16();
  if (len > kMaxStringBytes) fail("invalid frame");
  need(len);
  Bytes out(p, p + len);
  p += len;
  n -= len;
  return out;
}

void check_coord(std::int32_t lat, std::int32_t lon) {
  if (lat < kLatMin || lat > kLatMax || lon < kLonMin || lon > kLonMax) fail("invalid frame");
}

std::int32_t sat_add_i32(std::int32_t a, std::int32_t b) {
  auto s = static_cast<std::int64_t>(a) + static_cast<std::int64_t>(b);
  if (s > std::numeric_limits<std::int32_t>::max()) return std::numeric_limits<std::int32_t>::max();
  if (s < std::numeric_limits<std::int32_t>::min()) return std::numeric_limits<std::int32_t>::min();
  return static_cast<std::int32_t>(s);
}

struct MicroXY {
  std::int32_t lat = 0;
  std::int32_t lon = 0;
};

MicroXY write_point(Bytes& w, const LatLng& p, const MicroXY* prev) {
  auto lat = deg_to_micro(p.latitude);
  auto lon = deg_to_micro(p.longitude);
  std::uint8_t flags = 0;
  if (p.altitude) flags |= kPfAlt;
  if (p.accuracy) flags |= kPfAcc;
  if (p.timestamp_ms) flags |= kPfTime;
  put_u8(w, flags);
  if (prev) {
    if (!micro_delta_fits(prev->lat, prev->lon, lat, lon)) fail("intra-frame delta overflows i16");
    put_i16(w, static_cast<std::int16_t>(lat - prev->lat));
    put_i16(w, static_cast<std::int16_t>(lon - prev->lon));
  } else {
    put_i32(w, lat);
    put_i32(w, lon);
  }
  if (p.altitude) put_i32(w, static_cast<std::int32_t>(std::llround(*p.altitude * 1000.0)));
  if (p.accuracy) {
    double cm = std::llround(*p.accuracy * 100.0);
    if (cm < 0) cm = 0;
    if (cm > std::numeric_limits<std::uint16_t>::max()) cm = std::numeric_limits<std::uint16_t>::max();
    put_u16(w, static_cast<std::uint16_t>(cm));
  }
  if (p.timestamp_ms) put_i64(w, *p.timestamp_ms);
  return MicroXY{lat, lon};
}

void write_abs(Bytes& w, const LatLng& p) { write_point(w, p, nullptr); }

LatLng read_point(Reader& r, const MicroXY* prev, MicroXY* out_xy) {
  auto flags = r.u8();
  std::int32_t lat = 0;
  std::int32_t lon = 0;
  if (prev) {
    auto dlat = r.i16();
    auto dlon = r.i16();
    lat = sat_add_i32(prev->lat, dlat);
    lon = sat_add_i32(prev->lon, dlon);
  } else {
    lat = r.i32();
    lon = r.i32();
  }
  check_coord(lat, lon);
  LatLng p;
  p.latitude = micro_to_deg(lat);
  p.longitude = micro_to_deg(lon);
  if (flags & kPfAlt) p.altitude = static_cast<double>(r.i32()) / 1000.0;
  if (flags & kPfAcc) p.accuracy = static_cast<double>(r.u16()) / 100.0;
  if (flags & kPfTime) p.timestamp_ms = r.i64();
  if (out_xy) *out_xy = MicroXY{lat, lon};
  return p;
}

void write_route_abs(Bytes& w, const std::vector<LatLng>& route) {
  auto n = route.size();
  if (n > std::numeric_limits<std::uint16_t>::max()) n = std::numeric_limits<std::uint16_t>::max();
  put_u16(w, static_cast<std::uint16_t>(n));
  for (std::size_t i = 0; i < n; ++i) {
    put_i32(w, deg_to_micro(route[i].latitude));
    put_i32(w, deg_to_micro(route[i].longitude));
  }
}

std::vector<LatLng> read_route_abs(Reader& r) {
  auto n = r.u16();
  std::vector<LatLng> out;
  out.reserve(n);
  for (std::uint16_t i = 0; i < n; ++i) {
    auto lat = r.i32();
    auto lon = r.i32();
    check_coord(lat, lon);
    LatLng p;
    p.latitude = micro_to_deg(lat);
    p.longitude = micro_to_deg(lon);
    out.push_back(p);
  }
  return out;
}

Bytes encode_loc_frame(std::uint32_t seq, const std::vector<LatLng>& points) {
  if (points.empty()) fail("empty loc");
  if (static_cast<int>(points.size()) > kMaxLocPoints) fail("invalid frame");
  Bytes w;
  put_u8(w, kTypeLoc);
  put_u32(w, seq);
  put_u8(w, static_cast<std::uint8_t>(points.size()));
  const MicroXY* prev = nullptr;
  MicroXY cur{};
  MicroXY prev_store{};
  for (const auto& p : points) {
    cur = write_point(w, p, prev);
    prev_store = cur;
    prev = &prev_store;
  }
  return w;
}

CommandAckStatus command_ack_from_u8(std::uint8_t v) {
  switch (v) {
    case 1:
      return CommandAckStatus::kOk;
    case 2:
      return CommandAckStatus::kRejected;
    case 3:
      return CommandAckStatus::kFailed;
    default:
      return CommandAckStatus::kUnspecified;
  }
}

std::optional<ErrorCode> error_code_from_u8(std::uint8_t v) {
  if (v >= 1 && v <= 6) return static_cast<ErrorCode>(v);
  return std::nullopt;
}

Reader make_reader(const Bytes& data) { return Reader{data.data(), data.size()}; }

Reader make_reader(const std::string& data) {
  return Reader{reinterpret_cast<const std::uint8_t*>(data.data()), data.size()};
}

ClientMsg decode_client(Reader r) {
  if (r.n == 0) fail("truncated frame");
  auto typ = r.u8();
  ClientMsg msg;
  switch (typ) {
    case kTypeResume: {
      Resume m;
      m.track_uid = r.uuid();
      m.last_seq = r.u32();
      msg.resume = std::move(m);
      return msg;
    }
    case kTypeTrackStart: {
      TrackStart m;
      auto flags = r.u8();
      if (flags & 1) m.location = read_point(r, nullptr, nullptr);
      m.route = read_route_abs(r);
      m.metadata = r.bytes();
      msg.track_start = std::move(m);
      return msg;
    }
    case kTypeTrackStop:
      msg.track_stop = TrackStop{};
      return msg;
    case kTypeLoc: {
      Loc m;
      m.seq = r.u32();
      auto count = r.u8();
      if (count == 0 || count > kMaxLocPoints) fail("invalid frame");
      const MicroXY* prev = nullptr;
      MicroXY cur{};
      MicroXY prev_store{};
      for (int i = 0; i < count; ++i) {
        auto p = read_point(r, prev, &cur);
        m.points.push_back(p);
        prev_store = cur;
        prev = &prev_store;
      }
      msg.loc = std::move(m);
      return msg;
    }
    case kTypeSubscribe: {
      Subscribe m;
      m.device_uid = r.uuid();
      auto flags = r.u8();
      m.include_events = (flags & 1) != 0;
      m.min_interval_ms = r.u16();
      msg.subscribe = std::move(m);
      return msg;
    }
    case kTypeUnsubscribe: {
      Unsubscribe m;
      m.sub = r.u8();
      msg.unsubscribe = m;
      return msg;
    }
    case kTypeEvent: {
      Event m;
      m.payload = r.bytes();
      m.timestamp_ms = r.i64();
      msg.event = std::move(m);
      return msg;
    }
    case kTypeCommandAck: {
      CommandAck m;
      m.command_id = r.uuid();
      m.status = command_ack_from_u8(r.u8());
      m.message = r.str();
      msg.command_ack = std::move(m);
      return msg;
    }
    case 0x00:
    case 0x7F:
    case 0xFF:
      fail("invalid frame");
    default:
      if (typ >= 0x01 && typ <= 0x7E) {
        msg.unknown = typ;
        return msg;
      }
      fail("invalid frame");
  }
}

ServerMsg decode_server(Reader r) {
  if (r.n == 0) fail("truncated frame");
  auto typ = r.u8();
  ServerMsg msg;
  switch (typ) {
    case kTypeHello: {
      Hello m;
      m.version = r.u8();
      m.shard = r.u16();
      m.node_id = r.uuid();
      msg.hello = std::move(m);
      return msg;
    }
    case kTypeRelocate: {
      Relocate m;
      m.retry_after_ms = r.u32();
      m.endpoint = r.str();
      msg.relocate = std::move(m);
      return msg;
    }
    case kTypeResumeOk: {
      ResumeOk m;
      m.track_uid = r.uuid();
      m.last_acked = r.u32();
      msg.resume_ok = std::move(m);
      return msg;
    }
    case kTypeTrackStarted: {
      TrackStarted m;
      m.track_uid = r.uuid();
      m.metadata = r.bytes();
      msg.track_started = std::move(m);
      return msg;
    }
    case kTypeTrackStopped: {
      TrackStopped m;
      m.track_uid = r.uuid();
      msg.track_stopped = std::move(m);
      return msg;
    }
    case kTypeAck: {
      Ack m;
      m.seq = r.u32();
      msg.ack = m;
      return msg;
    }
    case kTypeServerLoc: {
      ServerLoc m;
      m.sub = r.u8();
      m.seq = r.u32();
      m.point = read_point(r, nullptr, nullptr);
      msg.loc = std::move(m);
      return msg;
    }
    case kTypeSubscribed: {
      Subscribed s;
      s.sub = r.u8();
      s.device_uid = r.uuid();
      s.track_uid = r.uuid_opt();
      s.online = r.u8() != 0;
      auto flags = r.u8();
      if (flags & 1) s.last_location = read_point(r, nullptr, nullptr);
      if (flags & 2) s.last_seen_ms = r.i64();
      if (flags & 4) s.route = read_route_abs(r);
      s.est_distance = r.f64();
      s.est_duration = r.f64();
      s.start_name = r.str();
      s.end_name = r.str();
      s.metadata = r.bytes();
      msg.subscribed = std::move(s);
      return msg;
    }
    case kTypeError: {
      auto code_u8 = r.u8();
      auto code = error_code_from_u8(code_u8);
      if (!code) fail("invalid frame");
      WireError e;
      e.code = *code;
      e.retry_after_ms = r.u32();
      e.track_uid = r.uuid_opt();
      e.message = r.str();
      msg.error = std::move(e);
      return msg;
    }
    case kTypeEventAdded: {
      EventAdded m;
      m.sub = r.u8();
      m.payload = r.bytes();
      m.timestamp_ms = r.i64();
      msg.event_added = std::move(m);
      return msg;
    }
    case kTypeCommand: {
      Command m;
      m.command_id = r.uuid();
      m.payload = r.bytes();
      m.timestamp_ms = r.i64();
      msg.command = std::move(m);
      return msg;
    }
    case kTypePresence: {
      Presence m;
      m.sub = r.u8();
      m.online = r.u8() != 0;
      m.last_seen_ms = r.i64();
      msg.presence = m;
      return msg;
    }
    case 0x00:
    case 0x7F:
    case 0xFF:
    case 0x8C:
      fail("invalid frame");
    default:
      if (typ >= 0x80 && typ <= 0xFE) return ServerMsg{};
      fail("invalid frame");
  }
}

}  // namespace

void stamp_lat_lng(LatLng* p) {
  if (!p) return;
  if (!p->timestamp_ms) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    p->timestamp_ms = ms;
  }
}

std::int32_t deg_to_micro(double d) { return static_cast<std::int32_t>(std::llround(d * 1'000'000.0)); }

double micro_to_deg(std::int32_t m) { return static_cast<double>(m) / 1'000'000.0; }

bool micro_delta_fits(std::int32_t prev_lat, std::int32_t prev_lon, std::int32_t lat,
                      std::int32_t lon) {
  auto dlat = static_cast<std::int64_t>(lat) - static_cast<std::int64_t>(prev_lat);
  auto dlon = static_cast<std::int64_t>(lon) - static_cast<std::int64_t>(prev_lon);
  return dlat >= std::numeric_limits<std::int16_t>::min() &&
         dlat <= std::numeric_limits<std::int16_t>::max() &&
         dlon >= std::numeric_limits<std::int16_t>::min() &&
         dlon <= std::numeric_limits<std::int16_t>::max();
}

Bytes encode_client_msg(const ClientMsg& msg) {
  Bytes w;
  if (msg.resume) {
    put_u8(w, kTypeResume);
    put_uuid(w, msg.resume->track_uid);
    put_u32(w, msg.resume->last_seq);
    return w;
  }
  if (msg.track_start) {
    put_u8(w, kTypeTrackStart);
    std::uint8_t flags = 0;
    if (msg.track_start->location) flags |= 1;
    put_u8(w, flags);
    if (msg.track_start->location) write_abs(w, *msg.track_start->location);
    write_route_abs(w, msg.track_start->route);
    put_bytes(w, msg.track_start->metadata);
    return w;
  }
  if (msg.track_stop) {
    put_u8(w, kTypeTrackStop);
    return w;
  }
  if (msg.loc) return encode_loc_frame(msg.loc->seq, msg.loc->points);
  if (msg.subscribe) {
    put_u8(w, kTypeSubscribe);
    put_uuid(w, msg.subscribe->device_uid);
    put_u8(w, msg.subscribe->include_events ? 1 : 0);
    put_u16(w, msg.subscribe->min_interval_ms);
    return w;
  }
  if (msg.unsubscribe) {
    put_u8(w, kTypeUnsubscribe);
    put_u8(w, msg.unsubscribe->sub);
    return w;
  }
  if (msg.event) {
    put_u8(w, kTypeEvent);
    put_bytes(w, msg.event->payload);
    put_i64(w, msg.event->timestamp_ms);
    return w;
  }
  if (msg.command_ack) {
    put_u8(w, kTypeCommandAck);
    put_uuid(w, msg.command_ack->command_id);
    put_u8(w, static_cast<std::uint8_t>(msg.command_ack->status));
    put_str(w, msg.command_ack->message);
    return w;
  }
  fail("invalid frame");
}

ClientMsg decode_client_msg(const Bytes& data) { return decode_client(make_reader(data)); }

ClientMsg decode_client_msg(const std::string& data) { return decode_client(make_reader(data)); }

Bytes encode_server_msg(const ServerMsg& msg) {
  Bytes w;
  if (msg.hello) {
    put_u8(w, kTypeHello);
    put_u8(w, msg.hello->version);
    put_u16(w, msg.hello->shard);
    put_uuid(w, msg.hello->node_id);
    return w;
  }
  if (msg.relocate) {
    put_u8(w, kTypeRelocate);
    put_u32(w, msg.relocate->retry_after_ms);
    put_str(w, msg.relocate->endpoint);
    return w;
  }
  if (msg.resume_ok) {
    put_u8(w, kTypeResumeOk);
    put_uuid(w, msg.resume_ok->track_uid);
    put_u32(w, msg.resume_ok->last_acked);
    return w;
  }
  if (msg.track_started) {
    put_u8(w, kTypeTrackStarted);
    put_uuid(w, msg.track_started->track_uid);
    put_bytes(w, msg.track_started->metadata);
    return w;
  }
  if (msg.track_stopped) {
    put_u8(w, kTypeTrackStopped);
    put_uuid(w, msg.track_stopped->track_uid);
    return w;
  }
  if (msg.ack) {
    put_u8(w, kTypeAck);
    put_u32(w, msg.ack->seq);
    return w;
  }
  if (msg.loc) {
    put_u8(w, kTypeServerLoc);
    put_u8(w, msg.loc->sub);
    put_u32(w, msg.loc->seq);
    write_abs(w, msg.loc->point);
    return w;
  }
  if (msg.subscribed) {
    const auto& s = *msg.subscribed;
    put_u8(w, kTypeSubscribed);
    put_u8(w, s.sub);
    put_uuid(w, s.device_uid);
    put_uuid(w, s.track_uid);
    put_u8(w, s.online ? 1 : 0);
    std::uint8_t flags = 0;
    if (s.last_location) flags |= 1;
    if (s.last_seen_ms) flags |= 2;
    if (!s.route.empty()) flags |= 4;
    put_u8(w, flags);
    if (s.last_location) write_abs(w, *s.last_location);
    if (s.last_seen_ms) put_i64(w, *s.last_seen_ms);
    if (flags & 4) write_route_abs(w, s.route);
    put_f64(w, s.est_distance);
    put_f64(w, s.est_duration);
    put_str(w, s.start_name);
    put_str(w, s.end_name);
    put_bytes(w, s.metadata);
    return w;
  }
  if (msg.error) {
    put_u8(w, kTypeError);
    put_u8(w, static_cast<std::uint8_t>(msg.error->code));
    put_u32(w, msg.error->retry_after_ms);
    put_uuid(w, msg.error->track_uid);
    put_str(w, msg.error->message);
    return w;
  }
  if (msg.event_added) {
    put_u8(w, kTypeEventAdded);
    put_u8(w, msg.event_added->sub);
    put_bytes(w, msg.event_added->payload);
    put_i64(w, msg.event_added->timestamp_ms);
    return w;
  }
  if (msg.command) {
    put_u8(w, kTypeCommand);
    put_uuid(w, msg.command->command_id);
    put_bytes(w, msg.command->payload);
    put_i64(w, msg.command->timestamp_ms);
    return w;
  }
  if (msg.presence) {
    put_u8(w, kTypePresence);
    put_u8(w, msg.presence->sub);
    put_u8(w, msg.presence->online ? 1 : 0);
    put_i64(w, msg.presence->last_seen_ms);
    return w;
  }
  fail("invalid frame");
}

ServerMsg decode_server_msg(const Bytes& data) { return decode_server(make_reader(data)); }

ServerMsg decode_server_msg(const std::string& data) { return decode_server(make_reader(data)); }

ClientMsg client_resume(const std::string& track_uid, std::uint32_t last_seq) {
  ClientMsg msg;
  msg.resume = Resume{track_uid, last_seq};
  return msg;
}

std::vector<Bytes> encode_loc_frames(std::uint32_t last_seq, const std::vector<LatLng>& points) {
  if (points.empty()) return {};
  auto n = static_cast<std::uint32_t>(points.size());
  auto first_seq = last_seq + 1 - n;
  std::vector<Bytes> out;
  std::size_t i = 0;
  while (i < points.size()) {
    auto start = i;
    auto prev_lat = deg_to_micro(points[i].latitude);
    auto prev_lon = deg_to_micro(points[i].longitude);
    ++i;
    while (i < points.size() && static_cast<int>(i - start) < kMaxLocPoints) {
      auto lat = deg_to_micro(points[i].latitude);
      auto lon = deg_to_micro(points[i].longitude);
      if (!micro_delta_fits(prev_lat, prev_lon, lat, lon)) break;
      prev_lat = lat;
      prev_lon = lon;
      ++i;
    }
    std::vector<LatLng> chunk(points.begin() + static_cast<std::ptrdiff_t>(start),
                              points.begin() + static_cast<std::ptrdiff_t>(i));
    auto seq = first_seq + static_cast<std::uint32_t>(i) - 1;
    out.push_back(encode_loc_frame(seq, chunk));
  }
  return out;
}

std::vector<Bytes> encode_in_flight_frames(const std::vector<InFlightPoint>& pts) {
  if (pts.empty()) return {};
  std::vector<Bytes> out;
  std::size_t i = 0;
  while (i < pts.size()) {
    auto start = i;
    auto prev_lat = deg_to_micro(pts[i].point.latitude);
    auto prev_lon = deg_to_micro(pts[i].point.longitude);
    ++i;
    while (i < pts.size() && static_cast<int>(i - start) < kMaxLocPoints) {
      if (pts[i].seq != pts[i - 1].seq + 1) break;
      auto lat = deg_to_micro(pts[i].point.latitude);
      auto lon = deg_to_micro(pts[i].point.longitude);
      if (!micro_delta_fits(prev_lat, prev_lon, lat, lon)) break;
      prev_lat = lat;
      prev_lon = lon;
      ++i;
    }
    std::vector<LatLng> chunk;
    chunk.reserve(i - start);
    for (auto j = start; j < i; ++j) chunk.push_back(pts[j].point);
    out.push_back(encode_loc_frame(pts[i - 1].seq, chunk));
  }
  return out;
}

}  // namespace pickpoint::tracking
