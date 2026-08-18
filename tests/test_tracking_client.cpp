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
  LatLng loc;
  loc.latitude = 1;
  loc.longitude = 2;
  c->start_track(&loc);
  int accepted = 0;
  for (int i = 0; i < kMaxPublishHz * 3; ++i) {
    LatLng p;
    p.latitude = i;
    if (c->publish(p).second) ++accepted;
  }
  EXPECT_EQ(accepted, 1);
  EXPECT_EQ(c->client_seq(), 1u);
  std::this_thread::sleep_for(kMinPublishInterval + std::chrono::milliseconds(5));
  LatLng p;
  p.latitude = 9;
  p.longitude = 9;
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
  LatLng loc;
  loc.latitude = 1;
  loc.longitude = 1;
  auto uid = c->start_track(&loc);
  LatLng p;
  p.latitude = 2;
  p.longitude = 2;
  ASSERT_TRUE(c->publish(p).second);
  wait_for([&] { return c->last_acked_seq() == 1; }, std::chrono::seconds(2));
  auto acked = c->resume(uid, 1);
  EXPECT_EQ(acked, 0u);
  ms.wait_msg([](const ClientMsg& m) { return m.resume.has_value(); });
  c->close();
}

TEST(TrackingClient, ListenerSubscribeAndLocation) {
  MockOpts opts;
  opts.auto_reply = true;
  opts.on_msg = [](const ClientMsg& msg, MockConn& conn) {
    if (msg.subscribe) {
      std::thread([&conn]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ServerMsg out;
        ServerLoc loc;
        loc.sub = 1;
        loc.seq = 3;
        loc.point = LatLng{1.5, 2.5, {}, {}, {}, {}, {}};
        out.loc = loc;
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
  c->subscribe(kMockDeviceUid);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    ServerMsg msg;
    if (!c->recv(msg, std::chrono::milliseconds(200))) continue;
    if (msg.loc) {
      EXPECT_DOUBLE_EQ(msg.loc->point.latitude, 1.5);
      c->close();
      return;
    }
  }
  FAIL() << "no location";
}

TEST(TrackingClient, AuthErrorWithoutRefreshCloses) {
  MockOpts opts;
  opts.auto_reply = false;
  opts.on_msg = [](const ClientMsg& msg, MockConn& conn) {
    if (msg.track_start) conn.send(server_error(ErrorCode::kAuth, "bad creds"));
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
    EXPECT_EQ(e.code, ErrorCode::kAuth);
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
  opts.on_msg = [](const ClientMsg& msg, MockConn& conn) {
    if (msg.track_start) conn.send(server_error(ErrorCode::kUnauthorized, "expired"));
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
  LatLng a;
  a.latitude = 1;
  a.longitude = 2;
  c->publish(a);
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  LatLng b;
  b.latitude = 3;
  b.longitude = 4;
  c->publish(b);
  EXPECT_EQ(c->client_seq(), 2u);
  auto first = ms.wait_conn();
  first->close();
  auto resume = ms.wait_msg([](const ClientMsg& m) { return m.resume.has_value(); },
                            std::chrono::seconds(8));
  EXPECT_EQ(resume.resume->track_uid, uid);
  EXPECT_EQ(resume.resume->last_seq, 2u);
  int starts = 0;
  for (auto& conn : ms.connections()) {
    std::lock_guard<std::mutex> lock(conn->mu);
    for (const auto& m : conn->messages) {
      if (m.track_start) ++starts;
    }
  }
  EXPECT_EQ(starts, 1);
  wait_for([&] { return c->state() == ConnectionState::kOpen; });
  c->close();
}

TEST(TrackingReconnect, TrackNotFoundClearsCursor) {
  MockOpts opts;
  opts.auto_reply = false;
  opts.on_msg = [](const ClientMsg& msg, MockConn& conn) {
    if (msg.track_start) {
      ServerMsg out;
      out.track_started = TrackStarted{"dddddddd-dddd-dddd-dddd-dddddddddddd", {}};
      conn.send(out);
    } else if (msg.resume) {
      conn.send(server_error(ErrorCode::kTrackNotFound, "track expired"));
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
  EXPECT_EQ(c->track_uid(), "dddddddd-dddd-dddd-dddd-dddddddddddd");
  ms.wait_conn()->close();
  ms.wait_msg([](const ClientMsg& m) { return m.resume.has_value(); }, std::chrono::seconds(8));
  wait_for([&] { return c->track_uid().empty(); });
  c->close();
}

TEST(TrackingReconnect, FencedResumeRetriesNotFatal) {
  std::atomic<int> resumes{0};
  MockOpts opts;
  opts.auto_reply = false;
  opts.on_msg = [&](const ClientMsg& msg, MockConn& conn) {
    if (msg.track_start) {
      ServerMsg out;
      out.track_started = TrackStarted{kMockTrackUid, {}};
      conn.send(out);
    } else if (msg.resume) {
      int n = resumes.fetch_add(1) + 1;
      if (n == 1) {
        conn.send(server_error(ErrorCode::kFenced, "draining"));
      } else {
        ServerMsg out;
        out.resume_ok = ResumeOk{kMockTrackUid, 0};
        conn.send(out);
      }
    }
  };
  MockServer ms(opts);
  Config cfg;
  cfg.endpoint = ms.url();
  cfg.device = DeviceAuth{"c", "s"};
  cfg.reconnect_min_delay = std::chrono::milliseconds(15);
  cfg.reconnect_max_delay = std::chrono::milliseconds(30);
  auto c = Client::connect(cfg);
  c->start_track();
  ms.wait_conn()->close();
  wait_for([&] { return resumes.load() >= 2 && c->track_uid() == kMockTrackUid; },
           std::chrono::seconds(8));
  c->close();
}

TEST(TrackingReconnect, RelocateDialsNewEndpoint) {
  MockServer target;
  auto rel = std::make_shared<Relocate>();
  rel->endpoint = target.url();
  rel->retry_after_ms = 10;
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
  EXPECT_EQ(uid, kMockTrackUid);
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
  LatLng p;
  p.latitude = 9;
  p.longitude = 9;
  auto [seq, ok] = c->publish(p);
  EXPECT_TRUE(ok);
  EXPECT_EQ(seq, 0u);
  {
    std::lock_guard<std::mutex> lock(release_mu);
    released = true;
  }
  release_cv.notify_all();
  ms.wait_msg([](const ClientMsg& m) { return m.resume.has_value(); }, std::chrono::seconds(8));
  ms.wait_msg([](const ClientMsg& m) { return m.loc.has_value(); }, std::chrono::seconds(8));
  wait_for([&] { return c->client_seq() == 1; }, std::chrono::seconds(3));
  c->close();
}
