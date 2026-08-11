#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "common/tracking_mock.hpp"
#include "pickpoint/tracking.hpp"

using namespace pickpoint::tracking;
using namespace pickpoint::test;

TEST(TrackingClient, PublishRateLimit) {
  MockServer ms;
  Config cfg;
  cfg.endpoint = ms.url();
  cfg.device = DeviceAuth{"c", "s"};
  cfg.disable_reconnect = true;
  auto c = Client::connect(cfg);
  ::tracking::v2::LatLng loc;
  loc.set_latitude(1);
  loc.set_longitude(2);
  c->start_track(&loc);
  int accepted = 0;
  for (int i = 0; i < kMaxPublishHz * 3; ++i) {
    ::tracking::v2::LatLng p;
    p.set_latitude(i);
    if (c->publish(p).second) ++accepted;
  }
  EXPECT_EQ(accepted, 1);
  EXPECT_EQ(c->client_seq(), 1u);
  std::this_thread::sleep_for(kMinPublishInterval + std::chrono::milliseconds(5));
  ::tracking::v2::LatLng p;
  p.set_latitude(9);
  p.set_longitude(9);
  auto [seq, ok] = c->publish(p);
  EXPECT_TRUE(ok);
  EXPECT_EQ(seq, 2u);
  c->close();
}

TEST(TrackingClient, SendEventLimits) {
  MockServer ms;
  Config cfg;
  cfg.endpoint = ms.url();
  cfg.device = DeviceAuth{"c", "s"};
  cfg.disable_reconnect = true;
  auto c = Client::connect(cfg);
  c->start_track();
  EXPECT_THROW(c->send_event(std::string(kMaxEventBytes + 1, 'x')), Error);
  EXPECT_TRUE(c->send_event("a"));
  EXPECT_FALSE(c->send_event("b"));
  c->close();
}

TEST(TrackingClient, ResumeAfterPublish) {
  MockServer ms;
  Config cfg;
  cfg.endpoint = ms.url();
  cfg.device = DeviceAuth{"c", "s"};
  cfg.disable_reconnect = true;
  auto c = Client::connect(cfg);
  ::tracking::v2::LatLng loc;
  loc.set_latitude(1);
  loc.set_longitude(1);
  auto uid = c->start_track(&loc);
  ::tracking::v2::LatLng p;
  p.set_latitude(2);
  p.set_longitude(2);
  ASSERT_TRUE(c->publish(p).second);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    ::tracking::v2::ServerMsg msg;
    if (c->recv(msg, std::chrono::milliseconds(100)) &&
        msg.body_case() == ::tracking::v2::ServerMsg::kLocationAdded) {
      break;
    }
  }
  auto acked = c->resume(uid, 1);
  EXPECT_EQ(acked, 0u);
  ms.wait_msg([](const ::tracking::v2::ClientMsg& m) {
    return m.body_case() == ::tracking::v2::ClientMsg::kResume;
  });
  c->close();
}

TEST(TrackingClient, ListenerSubscribeAndLocation) {
  MockOpts opts;
  opts.auto_reply = true;
  opts.on_msg = [](const ::tracking::v2::ClientMsg& msg, MockConn& conn) {
    if (msg.body_case() == ::tracking::v2::ClientMsg::kSubscribe) {
      std::thread([&conn, uid = msg.subscribe().device_uid()]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ::tracking::v2::ServerMsg out;
        auto* la = out.mutable_location_added();
        la->set_device_uid(uid);
        la->set_track_uid("t1");
        la->set_client_seq(3);
        la->mutable_point()->set_latitude(1.5);
        la->mutable_point()->set_longitude(2.5);
        conn.send(out);
      }).detach();
    }
  };
  MockServer ms(opts);
  Config cfg;
  cfg.endpoint = ms.url();
  cfg.listener = ListenerAuth{"jwt"};
  cfg.disable_reconnect = true;
  auto c = Client::connect(cfg);
  c->subscribe("device-1");
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    ::tracking::v2::ServerMsg msg;
    if (!c->recv(msg, std::chrono::milliseconds(200))) continue;
    if (msg.body_case() == ::tracking::v2::ServerMsg::kLocationAdded) {
      EXPECT_DOUBLE_EQ(msg.location_added().point().latitude(), 1.5);
      c->close();
      return;
    }
  }
  FAIL() << "no location";
}

TEST(TrackingClient, AuthErrorWithoutRefreshCloses) {
  MockOpts opts;
  opts.auto_reply = false;
  opts.on_msg = [](const ::tracking::v2::ClientMsg& msg, MockConn& conn) {
    if (msg.body_case() == ::tracking::v2::ClientMsg::kTrackStart) {
      conn.send(server_error(::tracking::v2::ERROR_CODE_AUTH, "bad creds"));
    }
  };
  MockServer ms(opts);
  Config cfg;
  cfg.endpoint = ms.url();
  cfg.device = DeviceAuth{"c", "s"};
  cfg.reconnect_min_delay = std::chrono::milliseconds(10);
  cfg.reconnect_max_delay = std::chrono::milliseconds(20);
  auto c = Client::connect(cfg);
  try {
    c->start_track();
    FAIL();
  } catch (const Error& e) {
    EXPECT_EQ(e.code, ::tracking::v2::ERROR_CODE_AUTH);
  }
  wait_for([&] { return c->state() == ConnectionState::kClosed; }, std::chrono::seconds(3));
  c->close();
}

TEST(TrackingClient, AuthErrorRefreshRedials) {
  std::atomic<int> hellos{0};
  std::atomic<bool> refreshed{false};
  MockOpts opts;
  opts.auto_reply = false;
  opts.before_hello = [&](int, MockConn&) { hellos.fetch_add(1); };
  opts.on_msg = [](const ::tracking::v2::ClientMsg& msg, MockConn& conn) {
    if (msg.body_case() == ::tracking::v2::ClientMsg::kTrackStart) {
      conn.send(server_error(::tracking::v2::ERROR_CODE_UNAUTHORIZED, "expired"));
    }
  };
  MockServer ms(opts);
  Config cfg;
  cfg.endpoint = ms.url();
  cfg.device = DeviceAuth{"c", "s"};
  cfg.reconnect_min_delay = std::chrono::milliseconds(15);
  cfg.reconnect_max_delay = std::chrono::milliseconds(40);
  cfg.hello_timeout = std::chrono::seconds(2);
  cfg.refresh_auth = [&]() -> std::pair<std::optional<DeviceAuth>, std::optional<ListenerAuth>> {
    refreshed.store(true);
    return {DeviceAuth{"c2", "s2"}, std::nullopt};
  };
  auto c = Client::connect(cfg);
  std::thread([&] {
    try {
      c->start_track();
    } catch (...) {
    }
  }).detach();
  wait_for([&] { return refreshed.load(); }, std::chrono::seconds(3));
  wait_for([&] { return hellos.load() >= 2; }, std::chrono::seconds(5));
  c->close();
}

TEST(TrackingReconnect, SendsResumeNotTrackStart) {
  MockServer ms;
  Config cfg;
  cfg.endpoint = ms.url();
  cfg.device = DeviceAuth{"c", "s"};
  cfg.reconnect_min_delay = std::chrono::milliseconds(20);
  cfg.reconnect_max_delay = std::chrono::milliseconds(50);
  auto c = Client::connect(cfg);
  auto uid = c->start_track();
  ::tracking::v2::LatLng a;
  a.set_latitude(1);
  a.set_longitude(2);
  c->publish(a);
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  ::tracking::v2::LatLng b;
  b.set_latitude(3);
  b.set_longitude(4);
  c->publish(b);
  EXPECT_EQ(c->client_seq(), 2u);
  auto first = ms.wait_conn();
  first->close();
  auto resume = ms.wait_msg(
      [](const ::tracking::v2::ClientMsg& m) {
        return m.body_case() == ::tracking::v2::ClientMsg::kResume;
      },
      std::chrono::seconds(8));
  EXPECT_EQ(resume.resume().track_uid(), uid);
  EXPECT_EQ(resume.resume().last_client_seq(), 2u);
  int starts = 0;
  for (auto& conn : ms.connections()) {
    std::lock_guard<std::mutex> lock(conn->mu);
    for (const auto& m : conn->messages) {
      if (m.body_case() == ::tracking::v2::ClientMsg::kTrackStart) ++starts;
    }
  }
  EXPECT_EQ(starts, 1);
  wait_for([&] { return c->state() == ConnectionState::kOpen; });
  c->close();
}

TEST(TrackingReconnect, TrackNotFoundClearsCursor) {
  MockOpts opts;
  opts.auto_reply = false;
  opts.on_msg = [](const ::tracking::v2::ClientMsg& msg, MockConn& conn) {
    if (msg.body_case() == ::tracking::v2::ClientMsg::kTrackStart) {
      ::tracking::v2::ServerMsg out;
      out.mutable_track_started()->set_track_uid("t-gone");
      conn.send(out);
    } else if (msg.body_case() == ::tracking::v2::ClientMsg::kResume) {
      conn.send(server_error(::tracking::v2::ERROR_CODE_TRACK_NOT_FOUND, "track expired"));
    }
  };
  MockServer ms(opts);
  Config cfg;
  cfg.endpoint = ms.url();
  cfg.device = DeviceAuth{"c", "s"};
  cfg.reconnect_min_delay = std::chrono::milliseconds(20);
  cfg.reconnect_max_delay = std::chrono::milliseconds(40);
  auto c = Client::connect(cfg);
  c->start_track();
  EXPECT_EQ(c->track_uid(), "t-gone");
  ms.wait_conn()->close();
  ms.wait_msg([](const ::tracking::v2::ClientMsg& m) {
    return m.body_case() == ::tracking::v2::ClientMsg::kResume;
  }, std::chrono::seconds(8));
  wait_for([&] { return c->track_uid().empty(); });
  c->close();
}

TEST(TrackingReconnect, RelocateDialsNewEndpoint) {
  MockServer target;
  auto rel = std::make_shared<::tracking::v2::Relocate>();
  rel->set_endpoint(target.url());
  rel->set_retry_after_ms(10);
  MockOpts gopts;
  gopts.auto_reply = false;
  gopts.relocate_on_connect = rel;
  MockServer gateway(gopts);
  Config cfg;
  cfg.endpoint = gateway.url();
  cfg.device = DeviceAuth{"c", "s"};
  cfg.disable_reconnect = true;
  auto c = Client::connect(cfg);
  EXPECT_EQ(c->state(), ConnectionState::kOpen);
  EXPECT_GE(target.conn_count(), 1);
  auto uid = c->start_track();
  EXPECT_EQ(uid, "track-mock-1");
  c->close();
}

TEST(TrackingReconnect, QueueFlushAfterResume) {
  std::mutex release_mu;
  std::condition_variable release_cv;
  bool released = false;
  MockOpts opts;
  opts.auto_reply = true;
  opts.before_hello = [&](int idx, MockConn&) {
    if (idx >= 2) {
      std::unique_lock<std::mutex> lock(release_mu);
      release_cv.wait(lock, [&] { return released; });
    }
  };
  MockServer ms(opts);
  Config cfg;
  cfg.endpoint = ms.url();
  cfg.device = DeviceAuth{"c", "s"};
  cfg.reconnect_min_delay = std::chrono::milliseconds(20);
  cfg.reconnect_max_delay = std::chrono::milliseconds(50);
  auto c = Client::connect(cfg);
  c->start_track();
  ms.wait_conn()->close();
  wait_for([&] { return c->state() == ConnectionState::kReconnecting; }, std::chrono::seconds(3));
  ::tracking::v2::LatLng p;
  p.set_latitude(9);
  p.set_longitude(9);
  auto [seq, ok] = c->publish(p);
  EXPECT_TRUE(ok);
  EXPECT_EQ(seq, 1u);
  {
    std::lock_guard<std::mutex> lock(release_mu);
    released = true;
  }
  release_cv.notify_all();
  ms.wait_msg([](const ::tracking::v2::ClientMsg& m) {
    return m.body_case() == ::tracking::v2::ClientMsg::kResume;
  }, std::chrono::seconds(8));
  ms.wait_msg([](const ::tracking::v2::ClientMsg& m) {
    return m.body_case() == ::tracking::v2::ClientMsg::kLocationBatch ||
           m.body_case() == ::tracking::v2::ClientMsg::kLocationAdd;
  }, std::chrono::seconds(8));
  c->close();
}
