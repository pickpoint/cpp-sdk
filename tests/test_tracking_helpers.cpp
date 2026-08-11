#include <gtest/gtest.h>

#include "pickpoint/tracking.hpp"

using pickpoint::tracking::BackoffState;
using pickpoint::tracking::Config;
using pickpoint::tracking::DeviceAuth;
using pickpoint::tracking::ListenerAuth;
using pickpoint::tracking::OfflineQueue;
using pickpoint::tracking::build_ws_url;
using pickpoint::tracking::can_accept_publish;
using pickpoint::tracking::client_resume;
using pickpoint::tracking::encode_client_msg;
using pickpoint::tracking::new_backoff;
using pickpoint::tracking::next_delay;
using pickpoint::tracking::next_publish_allowed_at;
using pickpoint::tracking::reset_backoff;
using pickpoint::tracking::stamp_lat_lng;
using pickpoint::tracking::kMaxPublishHz;
using pickpoint::tracking::kMinPublishInterval;

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

TEST(TrackingHelpers, OfflineQueueAckThrough) {
  OfflineQueue q(10);
  ::tracking::v2::LatLng p;
  p.set_latitude(1.0);
  q.enqueue(1, p);
  q.enqueue(2, p);
  q.enqueue(3, p);
  q.ack_through(2);
  auto got = q.peek_all();
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].seq, 3u);
}

TEST(TrackingHelpers, OfflineQueueDropOldest) {
  OfflineQueue q(2);
  ::tracking::v2::LatLng p;
  p.set_latitude(1.0);
  EXPECT_EQ(q.enqueue(1, p), 0);
  EXPECT_EQ(q.enqueue(2, p), 0);
  EXPECT_EQ(q.enqueue(3, p), 1);
  auto got = q.peek_all();
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].seq, 2u);
  EXPECT_EQ(got[1].seq, 3u);
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
  EXPECT_NE(u.find("wss://tracking.example.com/v2/tracking/ws?"), std::string::npos);
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
  ::tracking::v2::LatLng p;
  p.set_latitude(1.0);
  p.set_longitude(2.0);
  stamp_lat_lng(&p);
  auto after = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
  ASSERT_TRUE(p.has_timestamp_ms());
  EXPECT_GE(p.timestamp_ms(), before);
  EXPECT_LE(p.timestamp_ms(), after);
}

TEST(TrackingHelpers, StampLatLngPreservesTimestamp) {
  ::tracking::v2::LatLng p;
  p.set_latitude(1.0);
  p.set_longitude(2.0);
  p.set_timestamp_ms(42);
  stamp_lat_lng(&p);
  EXPECT_EQ(p.timestamp_ms(), 42);
}

TEST(TrackingHelpers, GoldenResumeWire) {
  auto msg = client_resume("track-uid-9", 42);
  auto b = encode_client_msg(msg);
  EXPECT_EQ(b.size(), 17u);
  static const char* want_hex = "0a0f0a0b747261636b2d7569642d39102a";
  std::string got_hex;
  static const char* hex = "0123456789abcdef";
  for (unsigned char c : b) {
    got_hex.push_back(hex[c >> 4]);
    got_hex.push_back(hex[c & 0xf]);
  }
  EXPECT_EQ(got_hex, want_hex);
}

TEST(TrackingHelpers, CodecRoundTripResume) {
  auto msg = client_resume("t1", 9);
  auto b = encode_client_msg(msg);
  ::tracking::v2::ClientMsg round;
  ASSERT_TRUE(round.ParseFromString(b));
  EXPECT_EQ(round.resume().track_uid(), "t1");
  EXPECT_EQ(round.resume().last_client_seq(), 9u);
}

TEST(TrackingHelpers, CodecRoundTripHello) {
  ::tracking::v2::ServerMsg out;
  out.mutable_hello()->set_node_id("n1");
  out.mutable_hello()->set_shard(7);
  std::string bytes;
  ASSERT_TRUE(out.SerializeToString(&bytes));
  auto in = pickpoint::tracking::decode_server_msg(bytes);
  EXPECT_EQ(in.hello().node_id(), "n1");
  EXPECT_EQ(in.hello().shard(), 7);
}
