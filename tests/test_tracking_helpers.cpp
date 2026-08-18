#include <gtest/gtest.h>

#include "pickpoint/tracking.hpp"

using namespace pickpoint::tracking;

namespace {

Bytes from_hex(const char* hex) {
  Bytes out;
  for (const char* p = hex; p[0] && p[1]; p += 2) {
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int hi = nibble(p[0]);
    int lo = nibble(p[1]);
    if (hi < 0 || lo < 0) break;
    out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return out;
}

std::string to_hex(const Bytes& b) {
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(b.size() * 2);
  for (auto c : b) {
    out.push_back(hex[c >> 4]);
    out.push_back(hex[c & 0xf]);
  }
  return out;
}

}  // namespace

TEST(TrackingHelpers, BackoffFullJitter) {
  auto state = new_backoff(std::chrono::milliseconds(100), std::chrono::milliseconds(800), 0);
  EXPECT_EQ(next_delay(state, 0.0)->count(), 0);
  EXPECT_EQ(next_delay(state, 0.5)->count(), 100);
  EXPECT_EQ(next_delay(state, 0.999)->count(), 399);
}

TEST(TrackingHelpers, BackoffMaxAttempts) {
  auto state = new_backoff(std::chrono::milliseconds(10), std::chrono::milliseconds(0), 2);
  EXPECT_TRUE(next_delay(state, 0.0).has_value());
  EXPECT_TRUE(next_delay(state, 0.0).has_value());
  EXPECT_FALSE(next_delay(state, 0.0).has_value());
}

TEST(TrackingHelpers, BackoffReset) {
  auto state = new_backoff(std::chrono::milliseconds(10), std::chrono::milliseconds(0), 1);
  EXPECT_TRUE(next_delay(state, 0.0).has_value());
  EXPECT_FALSE(next_delay(state, 0.0).has_value());
  reset_backoff(state);
  EXPECT_TRUE(next_delay(state, 0.0).has_value());
}

TEST(TrackingHelpers, BufferAckThroughInFlightOnly) {
  Buffer q(10);
  q.push_staging(LatLng{1, 0, {}, {}, {}, {}, {}});
  std::uint64_t next = 0;
  auto assigned = q.assign_from_staging(&next, 8);
  ASSERT_EQ(assigned.size(), 1u);
  EXPECT_EQ(assigned[0].seq, 1u);
  q.push_staging(LatLng{2, 0, {}, {}, {}, {}, {}});
  q.ack_through(1);
  EXPECT_EQ(q.in_flight_size(), 0);
  EXPECT_EQ(q.staging_size(), 1);
}

TEST(TrackingHelpers, BufferOverflowCollapsesMiddle) {
  int dropped = 0;
  Buffer q(3, [&](int n) { dropped += n; });
  q.push_staging(LatLng{0, 0, {}, {}, {}, {}, {}});
  q.push_staging(LatLng{0, 0.00001, {}, {}, {}, {}, {}});
  q.push_staging(LatLng{0, 0.00002, {}, {}, {}, {}, {}});
  q.push_staging(LatLng{0, 0.00003, {}, {}, {}, {}, {}});
  EXPECT_EQ(q.size(), 3);
  EXPECT_GT(dropped, 0);
  auto got = q.peek_staging();
  ASSERT_FALSE(got.empty());
  EXPECT_GE(got.back().longitude, 0.00002);
}

TEST(TrackingHelpers, AssignSeqAfterStaging) {
  Buffer q(10);
  q.push_staging(LatLng{1, 0, {}, {}, {}, {}, {}});
  q.push_staging(LatLng{1.001, 0, {}, {}, {}, {}, {}});
  EXPECT_EQ(q.in_flight_size(), 0);
  std::uint64_t next = 40;
  auto assigned = q.assign_from_staging(&next, 8);
  ASSERT_EQ(assigned.size(), 2u);
  EXPECT_EQ(assigned[0].seq, 41u);
  EXPECT_EQ(assigned[1].seq, 42u);
  EXPECT_EQ(next, 42u);
  EXPECT_EQ(q.staging_size(), 0);
}

TEST(TrackingHelpers, PublishRateSpacing) {
  EXPECT_EQ(kMaxPublishHz, 50);
  EXPECT_EQ(kMinPublishInterval, std::chrono::milliseconds(20));
  auto now = std::chrono::steady_clock::now();
  EXPECT_TRUE(can_accept_publish(now, now, 1));
  auto next = next_publish_allowed_at(now, now, 1);
  EXPECT_EQ(next, now + kMinPublishInterval);
  EXPECT_FALSE(can_accept_publish(next, now + std::chrono::milliseconds(19), 1));
  EXPECT_TRUE(can_accept_publish(next, now + std::chrono::milliseconds(20), 1));
}

TEST(TrackingHelpers, PublishRateBatchSlots) {
  auto now = std::chrono::steady_clock::now();
  auto next = next_publish_allowed_at(std::chrono::steady_clock::time_point{}, now, 50);
  EXPECT_EQ(next, now + 50 * kMinPublishInterval);
  EXPECT_FALSE(can_accept_publish(next, now + std::chrono::milliseconds(999), 1));
  EXPECT_TRUE(can_accept_publish(next, now + std::chrono::milliseconds(1000), 1));
}

TEST(TrackingHelpers, BuildWsUrlDevice) {
  Config cfg;
  cfg.endpoint = "https://tracking.example.com";
  cfg.device = DeviceAuth{"id", "sec"};
  auto u = build_ws_url(cfg);
  EXPECT_NE(u.find("wss://tracking.example.com/v2/ws?"), std::string::npos);
  EXPECT_NE(u.find("client-id=id"), std::string::npos);
  EXPECT_NE(u.find("client-secret=sec"), std::string::npos);
}

TEST(TrackingHelpers, BuildWsUrlListener) {
  Config cfg;
  cfg.endpoint = "ws://localhost:1";
  cfg.listener = ListenerAuth{"jwt"};
  auto u = build_ws_url(cfg);
  EXPECT_NE(u.find("access-token=jwt"), std::string::npos);
}

TEST(TrackingHelpers, StampLatLngDefaultTimestamp) {
  auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  LatLng p;
  p.latitude = 1.0;
  p.longitude = 2.0;
  stamp_lat_lng(&p);
  auto after = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
  ASSERT_TRUE(p.timestamp_ms.has_value());
  EXPECT_GE(*p.timestamp_ms, before);
  EXPECT_LE(*p.timestamp_ms, after);
}

TEST(TrackingHelpers, StampLatLngPreservesTimestamp) {
  LatLng p;
  p.latitude = 1.0;
  p.longitude = 2.0;
  p.timestamp_ms = 42;
  stamp_lat_lng(&p);
  EXPECT_EQ(*p.timestamp_ms, 42);
}

TEST(TrackingHelpers, GoldenAckSeq1) {
  ServerMsg msg;
  msg.ack = Ack{1};
  auto b = encode_server_msg(msg);
  EXPECT_EQ(b, from_hex("8501000000"));
  auto round = decode_server_msg(b);
  ASSERT_TRUE(round.ack.has_value());
  EXPECT_EQ(round.ack->seq, 1u);
}

TEST(TrackingHelpers, GoldenLoc55N37E) {
  ClientMsg msg;
  Loc loc;
  loc.seq = 1;
  loc.points.push_back(LatLng{55, 37, {}, {}, {}, {}, {}});
  msg.loc = loc;
  auto b = encode_client_msg(msg);
  EXPECT_EQ(to_hex(b), "04010000000100c03b470340933402");
}

TEST(TrackingHelpers, GoldenResume) {
  auto msg = client_resume("00112233-4455-6677-8899-aabbccddeeff", 45);
  auto b = encode_client_msg(msg);
  EXPECT_EQ(to_hex(b), "0100112233445566778899aabbccddeeff2d000000");
}

TEST(TrackingHelpers, GoldenTrackStop) {
  ClientMsg msg;
  msg.track_stop = TrackStop{};
  auto b = encode_client_msg(msg);
  ASSERT_EQ(b.size(), 1u);
  EXPECT_EQ(b[0], 0x03);
}

TEST(TrackingHelpers, CodecRoundTripResume) {
  auto msg = client_resume("00112233-4455-6677-8899-aabbccddeeff", 9);
  auto b = encode_client_msg(msg);
  auto round = decode_client_msg(b);
  ASSERT_TRUE(round.resume.has_value());
  EXPECT_EQ(round.resume->track_uid, "00112233-4455-6677-8899-aabbccddeeff");
  EXPECT_EQ(round.resume->last_seq, 9u);
}

TEST(TrackingHelpers, CodecRoundTripHello) {
  ServerMsg out;
  Hello hello;
  hello.version = 2;
  hello.node_id = "cccccccc-cccc-cccc-cccc-cccccccccccc";
  hello.shard = 7;
  out.hello = hello;
  auto bytes = encode_server_msg(out);
  auto in = decode_server_msg(bytes);
  ASSERT_TRUE(in.hello.has_value());
  EXPECT_EQ(in.hello->node_id, "cccccccc-cccc-cccc-cccc-cccccccccccc");
  EXPECT_EQ(in.hello->shard, 7);
  EXPECT_EQ(in.hello->version, 2);
}

TEST(TrackingHelpers, DeviceAckVsListenerLocTypes) {
  ServerMsg ack_msg;
  ack_msg.ack = Ack{1};
  auto ack = encode_server_msg(ack_msg);
  ASSERT_FALSE(ack.empty());
  EXPECT_EQ(ack[0], kTypeAck);

  ServerMsg loc_msg;
  ServerLoc loc;
  loc.sub = 1;
  loc.seq = 1;
  loc.point = LatLng{55, 37, {}, {}, {}, {}, {}};
  loc_msg.loc = loc;
  auto loc_bytes = encode_server_msg(loc_msg);
  ASSERT_FALSE(loc_bytes.empty());
  EXPECT_EQ(loc_bytes[0], kTypeServerLoc);
  EXPECT_NE(ack[0], loc_bytes[0]);
}

TEST(TrackingHelpers, EncodeLocSplitsOnI16Overflow) {
  LatLng a{0, 0, {}, {}, {}, {}, {}};
  LatLng b{4, 0, {}, {}, {}, {}, {}};
  EXPECT_FALSE(micro_delta_fits(deg_to_micro(0), deg_to_micro(0), deg_to_micro(4), deg_to_micro(0)));
  auto frames = encode_loc_frames(2, {a, b});
  ASSERT_EQ(frames.size(), 2u);
  auto m0 = decode_client_msg(frames[0]);
  ASSERT_TRUE(m0.loc.has_value());
  EXPECT_EQ(m0.loc->seq, 1u);
  auto m1 = decode_client_msg(frames[1]);
  ASSERT_TRUE(m1.loc.has_value());
  EXPECT_EQ(m1.loc->seq, 2u);
  EXPECT_DOUBLE_EQ(m1.loc->points[0].latitude, 4);
}

TEST(TrackingHelpers, UnknownServerTypeIgnored) {
  auto msg = decode_server_msg(Bytes{0x8D});
  EXPECT_TRUE(msg.empty());
}

TEST(TrackingHelpers, UnknownClientTypeNotFatal) {
  auto msg = decode_client_msg(Bytes{0x09});
  ASSERT_TRUE(msg.unknown.has_value());
  EXPECT_EQ(*msg.unknown, 0x09);
}

TEST(TrackingHelpers, TrailingBytesIgnored) {
  ClientMsg msg;
  msg.track_stop = TrackStop{};
  auto b = encode_client_msg(msg);
  b.push_back(0xDE);
  b.push_back(0xAD);
  auto round = decode_client_msg(b);
  EXPECT_TRUE(round.track_stop.has_value());
}

TEST(TrackingHelpers, UnsubscribeIsSubHandle) {
  ClientMsg msg;
  msg.unsubscribe = Unsubscribe{7};
  auto b = encode_client_msg(msg);
  ASSERT_EQ(b.size(), 2u);
  EXPECT_EQ(b[0], 0x06);
  EXPECT_EQ(b[1], 0x07);
}

TEST(TrackingHelpers, FilterFirstPointAlwaysEmits) {
  NoiseFilter f;
  auto now = std::chrono::system_clock::time_point(std::chrono::seconds(1'700'000'000));
  auto [p, ok] = f.push(LatLng{55, 37, {}, {}, {}, {}, {}}, now);
  EXPECT_TRUE(ok);
  (void)p;
}

TEST(TrackingHelpers, FilterDropsNearbyBurst) {
  NoiseFilter f;
  auto now = std::chrono::system_clock::time_point(std::chrono::seconds(1'700'000'000));
  ASSERT_TRUE(f.push(LatLng{55, 37, {}, {}, {}, {}, {}}, now).second);
  bool ok = f.push(LatLng{55, 37.000005, {}, {}, {}, {}, {}}, now + std::chrono::milliseconds(10))
                .second;
  EXPECT_FALSE(ok);
}

TEST(TrackingHelpers, FilterHeartbeatEmitsCurrent) {
  NoiseFilter f;
  auto now = std::chrono::system_clock::time_point(std::chrono::seconds(1'700'000'000));
  f.push(LatLng{55, 37, {}, {}, {}, {}, {}}, now);
  auto [p, ok] = f.push(LatLng{55.000001, 37, {}, {}, {}, {}, {}}, now + std::chrono::seconds(1));
  EXPECT_TRUE(ok);
  EXPECT_DOUBLE_EQ(p.latitude, 55.000001);
}

TEST(TrackingHelpers, FilterHeadingJumpEmits) {
  NoiseFilter f;
  auto now = std::chrono::system_clock::time_point(std::chrono::seconds(1'700'000'000));
  LatLng a{55, 37, {}, {}, {}, {}, {}};
  a.heading = 0.0;
  LatLng b{55.000001, 37, {}, {}, {}, {}, {}};
  b.heading = 40.0;
  f.push(a, now);
  bool ok = f.push(b, now + std::chrono::milliseconds(20)).second;
  EXPECT_TRUE(ok);
}

TEST(TrackingHelpers, FatalResumeCodes) {
  EXPECT_TRUE(is_fatal_resume_error(ErrorCode::kAuth));
  EXPECT_TRUE(is_fatal_resume_error(ErrorCode::kTrackNotFound));
  EXPECT_FALSE(is_fatal_resume_error(ErrorCode::kFenced));
  EXPECT_FALSE(is_fatal_resume_error(ErrorCode::kTryAgain));
  EXPECT_FALSE(is_fatal_resume_error(ErrorCode::kUnauthorized));
  EXPECT_TRUE(is_retry_resume_error(ErrorCode::kFenced));
  EXPECT_TRUE(is_retry_resume_error(ErrorCode::kTryAgain));
}
