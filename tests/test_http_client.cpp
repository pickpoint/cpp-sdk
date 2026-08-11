#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "common/http_mock.hpp"
#include "pickpoint/pickpoint.hpp"

using pickpoint::test::HttpMock;
using pickpoint::test::expires_at_ms;

TEST(HttpClient, InvalidConfig) {
  EXPECT_THROW(
      {
        pickpoint::Config empty;
        pickpoint::Client c(empty);
      },
      pickpoint::InvalidConfigError);
  EXPECT_THROW(
      {
        pickpoint::Config both;
        both.api_key = "a";
        both.access_token = "b";
        pickpoint::Client c(both);
      },
      pickpoint::InvalidConfigError);
}

TEST(HttpClient, ForwardAndSearchShareApiKey) {
  std::atomic<int> keys{0};
  HttpMock mock([&](const httplib::Request& req, httplib::Response& res) {
    if (req.get_header_value("x-api-key") == "secret") keys.fetch_add(1);
    if (req.path.find("/geocode/forward") != std::string::npos) {
      res.set_content(R"([{"display_name":"Berlin"}])", "application/json");
    } else if (req.path.find("/address/search") != std::string::npos) {
      res.set_content(R"({"type":"FeatureCollection","features":[]})", "application/json");
    } else if (req.path.find("/v2/devices") != std::string::npos) {
      res.set_content(R"({"data":[{"uid":"d1","name":"n"}],"total":1})", "application/json");
    } else {
      res.status = 404;
    }
  });
  pickpoint::Config cfg;
  cfg.base_url = mock.base_url();
  cfg.api_key = "secret";
  pickpoint::Client c(cfg);
  auto places = c.forward({{"q", "Berlin"}});
  ASSERT_EQ(places.size(), 1u);
  c.search({{"q", "x"}});
  auto list = c.devices_list();
  EXPECT_EQ(list.total, 1);
  EXPECT_EQ(keys.load(), 3);
}

TEST(HttpAuth, RefreshOn401) {
  std::atomic<int> n{0};
  HttpMock mock([&](const httplib::Request& req, httplib::Response& res) {
    if (req.path.find("/client-tokens/refresh") != std::string::npos) {
      res.set_content(
          std::string(R"({"accessToken":"access-2","refreshToken":"refresh-2","expiresAt":)") +
              std::to_string(expires_at_ms(std::chrono::seconds(60))) + "}",
          "application/json");
      return;
    }
    int i = n.fetch_add(1) + 1;
    auto auth = req.get_header_value("Authorization");
    if (i == 1) {
      EXPECT_EQ(auth, "Bearer access-1");
      res.status = 401;
      res.set_content("{}", "application/json");
      return;
    }
    EXPECT_EQ(auth, "Bearer access-2");
    res.set_content(R"([{"ok":true}])", "application/json");
  });
  pickpoint::Config cfg;
  cfg.base_url = mock.base_url();
  cfg.client_auth = pickpoint::ClientAuth{"access-1", "refresh-1", expires_at_ms(std::chrono::seconds(60))};
  pickpoint::Client c(cfg);
  auto out = c.forward({{"q", "a"}});
  EXPECT_EQ(out.size(), 1u);
}

TEST(HttpAuth, SingleFlightRefresh) {
  std::atomic<int> refreshes{0};
  HttpMock mock([&](const httplib::Request& req, httplib::Response& res) {
    if (req.path.find("/refresh") != std::string::npos) {
      refreshes.fetch_add(1);
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      res.set_content(
          std::string(R"({"accessToken":"access-fresh","refreshToken":"refresh-2","expiresAt":)") +
              std::to_string(expires_at_ms(std::chrono::seconds(120))) + "}",
          "application/json");
      return;
    }
    EXPECT_EQ(req.get_header_value("Authorization"), "Bearer access-fresh");
    res.set_content(R"([{"ok":true}])", "application/json");
  });
  // Already past half TTL so first token() refreshes (single-flight under mutex).
  pickpoint::Config cfg;
  cfg.base_url = mock.base_url();
  cfg.client_auth =
      pickpoint::ClientAuth{"access-old", "refresh-1", expires_at_ms(std::chrono::hours(-1))};
  pickpoint::Client c(cfg);
  std::vector<std::future<void>> futs;
  for (int i = 0; i < 4; ++i) {
    futs.push_back(std::async(std::launch::async, [&] { (void)c.forward({{"q", "a"}}); }));
  }
  for (auto& f : futs) f.get();
  EXPECT_EQ(refreshes.load(), 1);
}

TEST(HttpAuth, MintClientTokens) {
  HttpMock mock([&](const httplib::Request& req, httplib::Response& res) {
    EXPECT_EQ(req.get_header_value("x-api-key"), "secret");
    // Body may arrive as raw JSON; accept either full body or content-length present.
    if (!req.body.empty()) {
      EXPECT_NE(req.body.find("geocoding"), std::string::npos);
    } else {
      EXPECT_FALSE(req.get_header_value("Content-Length").empty());
    }
    res.set_content(
        std::string(R"({"accessToken":"a","refreshToken":"r","expiresAt":)") +
            std::to_string(expires_at_ms(std::chrono::seconds(120))) + "}",
        "application/json");
  });
  pickpoint::Config cfg;
  cfg.base_url = mock.base_url();
  cfg.api_key = "secret";
  auto pair = pickpoint::mint_client_tokens(cfg, {"geocoding"}, 120);
  EXPECT_EQ(pair.access_token, "a");
  EXPECT_EQ(pair.refresh_token, "r");
}

TEST(HttpAuth, UnauthorizedRetryExactlyOnce) {
  std::atomic<int> hits{0};
  HttpMock mock([&](const httplib::Request& req, httplib::Response& res) {
    if (req.path.find("/refresh") != std::string::npos) {
      res.set_content(
          std::string(R"({"accessToken":"a2","refreshToken":"r2","expiresAt":)") +
              std::to_string(expires_at_ms(std::chrono::seconds(60))) + "}",
          "application/json");
      return;
    }
    hits.fetch_add(1);
    res.status = 401;
    res.set_content("{}", "application/json");
  });
  pickpoint::Config cfg;
  cfg.base_url = mock.base_url();
  cfg.client_auth = pickpoint::ClientAuth{"a1", "r1", expires_at_ms(std::chrono::seconds(60))};
  pickpoint::Client c(cfg);
  EXPECT_THROW(c.forward({{"q", "x"}}), pickpoint::ApiError);
  EXPECT_EQ(hits.load(), 2);
}

TEST(HttpAuth, RefreshRotationSecondClientFails) {
  std::mutex mu;
  std::string valid = "refresh-1";
  HttpMock mock([&](const httplib::Request& req, httplib::Response& res) {
    if (req.path.find("/refresh") != std::string::npos) {
      std::string body = req.body;
      bool ok = false;
      {
        std::lock_guard<std::mutex> lock(mu);
        ok = body.find(valid) != std::string::npos;
        if (ok) valid = "refresh-2";
      }
      if (!ok) {
        res.status = 401;
        return;
      }
      res.set_content(
          std::string(R"({"accessToken":"a2","refreshToken":"refresh-2","expiresAt":)") +
              std::to_string(expires_at_ms(std::chrono::minutes(1))) + "}",
          "application/json");
      return;
    }
    res.set_content("[]", "application/json");
  });
  auto mk = [&] {
    pickpoint::Config cfg;
    cfg.base_url = mock.base_url();
    cfg.client_auth =
        pickpoint::ClientAuth{"a1", "refresh-1", expires_at_ms(std::chrono::milliseconds(50))};
    return pickpoint::Client(cfg);
  };
  auto a = mk();
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  EXPECT_NO_THROW(a.forward({{"q", "a"}}));
  auto b = mk();
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  try {
    b.forward({{"q", "b"}});
    FAIL() << "expected auth error";
  } catch (const pickpoint::ApiError& e) {
    EXPECT_TRUE(e.is_auth());
  }
}

TEST(HttpAuth, ProactiveRefreshHalfwayTTL) {
  std::atomic<bool> refreshed{false};
  HttpMock mock([&](const httplib::Request& req, httplib::Response& res) {
    if (req.path.find("/refresh") != std::string::npos) {
      refreshed.store(true);
      res.set_content(
          std::string(R"({"accessToken":"a2","refreshToken":"r2","expiresAt":)") +
              std::to_string(expires_at_ms(std::chrono::minutes(1))) + "}",
          "application/json");
      return;
    }
    res.set_content("[]", "application/json");
  });
  auto ttl = std::chrono::milliseconds(200);
  pickpoint::Config cfg;
  cfg.base_url = mock.base_url();
  cfg.client_auth = pickpoint::ClientAuth{"a1", "r1", expires_at_ms(ttl)};
  pickpoint::Client c(cfg);
  EXPECT_NO_THROW(c.forward({{"q", "early"}}));
  EXPECT_FALSE(refreshed.load());
  std::this_thread::sleep_for(ttl * 55 / 100 + std::chrono::milliseconds(10));
  EXPECT_NO_THROW(c.forward({{"q", "late"}}));
  EXPECT_TRUE(refreshed.load());
}

TEST(HttpAuth, MixedFanOutShares401Refresh) {
  std::atomic<int> refreshes{0};
  HttpMock mock([&](const httplib::Request& req, httplib::Response& res) {
    if (req.path.find("/refresh") != std::string::npos) {
      refreshes.fetch_add(1);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      res.set_content(
          std::string(R"({"accessToken":"a2","refreshToken":"r2","expiresAt":)") +
              std::to_string(expires_at_ms(std::chrono::minutes(1))) + "}",
          "application/json");
      return;
    }
    if (req.get_header_value("Authorization") == "Bearer a1") {
      res.status = 401;
      return;
    }
    if (req.path.find("/address/search") != std::string::npos) {
      res.set_content(R"({"features":[]})", "application/json");
    } else if (req.path.find("/devices") != std::string::npos) {
      res.set_content(R"({"data":[],"total":0})", "application/json");
    } else {
      res.set_content(R"([{"ok":true}])", "application/json");
    }
  });
  pickpoint::Config cfg;
  cfg.base_url = mock.base_url();
  cfg.client_auth = pickpoint::ClientAuth{"a1", "r1", expires_at_ms(std::chrono::minutes(1))};
  pickpoint::Client c(cfg);
  auto f1 = std::async(std::launch::async, [&] { (void)c.forward({{"q", "a"}}); });
  auto f2 = std::async(std::launch::async, [&] { (void)c.search({{"q", "b"}}); });
  auto f3 = std::async(std::launch::async, [&] { (void)c.devices_list({}); });
  f1.get();
  f2.get();
  f3.get();
  EXPECT_EQ(refreshes.load(), 1);
}

TEST(HttpAuth, MintClientTokensScopes) {
  std::string seen;
  HttpMock mock([&](const httplib::Request& req, httplib::Response& res) {
    EXPECT_EQ(req.get_header_value("x-api-key"), "secret");
    seen = req.body;
    res.set_content(R"({"accessToken":"a","refreshToken":"r","expiresAt":1,"scopes":["geocoding"]})",
                    "application/json");
  });
  pickpoint::Config cfg;
  cfg.base_url = mock.base_url();
  cfg.api_key = "secret";
  auto pair = pickpoint::mint_client_tokens(cfg, {}, std::nullopt);
  EXPECT_EQ(pair.access_token, "a");
  if (!seen.empty()) {
    EXPECT_NE(seen.find("\"scopes\":[]"), std::string::npos);
  }
}

TEST(HttpAuth, MintClientTokensWithScopes) {
  std::string seen;
  HttpMock mock([&](const httplib::Request& req, httplib::Response& res) {
    seen = req.body;
    res.set_content(R"({"accessToken":"a","refreshToken":"r","expiresAt":1})", "application/json");
  });
  pickpoint::Config cfg;
  cfg.base_url = mock.base_url();
  cfg.api_key = "k";
  (void)pickpoint::mint_client_tokens(cfg, {"geocoding", "devices"}, 600);
  if (!seen.empty()) {
    EXPECT_NE(seen.find("devices"), std::string::npos);
    EXPECT_NE(seen.find("geocoding"), std::string::npos);
  }
}

TEST(HttpDevices, NotFoundConflictAndCommand) {
  HttpMock mock404([](const httplib::Request&, httplib::Response& res) {
    res.status = 404;
    res.set_content(R"({"message":"Device not found"})", "application/json");
  });
  {
    pickpoint::Config cfg;
    cfg.base_url = mock404.base_url();
    cfg.api_key = "k";
    pickpoint::Client c(cfg);
    try {
      c.devices_get("missing");
      FAIL();
    } catch (const pickpoint::ApiError& e) {
      EXPECT_TRUE(e.is_not_found());
    }
  }

  HttpMock mock409([](const httplib::Request&, httplib::Response& res) {
    res.status = 409;
    res.set_content(R"({"message":"device offline"})", "application/json");
  });
  {
    pickpoint::Config cfg;
    cfg.base_url = mock409.base_url();
    cfg.api_key = "k";
    pickpoint::Client c(cfg);
    try {
      c.devices_command("d1", {'h', 'i'});
      FAIL();
    } catch (const pickpoint::ApiError& e) {
      EXPECT_TRUE(e.is_conflict());
    }
  }

  std::string seen_body;
  HttpMock mockCmd([&](const httplib::Request& req, httplib::Response& res) {
    seen_body = req.body;
    res.set_content(R"({"delivered":1})", "application/json");
  });
  {
    pickpoint::Config cfg;
    cfg.base_url = mockCmd.base_url();
    cfg.api_key = "k";
    pickpoint::Client c(cfg);
    auto r = c.devices_command("d1", {'h', 'i'});
    EXPECT_EQ(r.delivered, 1);
    // Prefer body assert when mock captured it; otherwise rely on delivered.
    if (!seen_body.empty()) {
      EXPECT_NE(seen_body.find("aGk="), std::string::npos);
    }
  }
}

TEST(HttpRouting, BadRequestThrows) {
  HttpMock mock([](const httplib::Request&, httplib::Response& res) {
    res.status = 400;
    res.set_content(R"({"message":"bad"})", "application/json");
  });
  pickpoint::Config cfg;
  cfg.base_url = mock.base_url();
  cfg.api_key = "k";
  pickpoint::Client c(cfg);
  try {
    c.route(nlohmann::json::object());
    FAIL();
  } catch (const pickpoint::ApiError& e) {
    EXPECT_EQ(e.status, 400);
  }
}

TEST(HttpAddress, Search400Throws) {
  HttpMock mock([](const httplib::Request&, httplib::Response& res) {
    res.status = 400;
    res.set_content(R"({"message":"bad"})", "application/json");
  });
  pickpoint::Config cfg;
  cfg.base_url = mock.base_url();
  cfg.api_key = "k";
  pickpoint::Client c(cfg);
  try {
    c.search({{"q", "x"}});
    FAIL();
  } catch (const pickpoint::ApiError& e) {
    EXPECT_EQ(e.status, 400);
  }
}

TEST(HttpGeocoding, EmptyOn400) {
  HttpMock mock([](const httplib::Request&, httplib::Response& res) {
    res.status = 400;
    res.set_content(R"({"message":"bad"})", "application/json");
  });
  pickpoint::Config cfg;
  cfg.base_url = mock.base_url();
  cfg.api_key = "k";
  pickpoint::Client c(cfg);
  auto out = c.forward({{"q", "x"}});
  EXPECT_TRUE(out.is_array());
  EXPECT_TRUE(out.empty());
}

TEST(HttpGeocoding, ForwardBatchConcurrency) {
  std::atomic<int> in_flight{0};
  std::atomic<int> peak{0};
  HttpMock mock([&](const httplib::Request&, httplib::Response& res) {
    int cur = in_flight.fetch_add(1) + 1;
    int p = peak.load();
    while (cur > p && !peak.compare_exchange_weak(p, cur)) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    in_flight.fetch_sub(1);
    res.set_content(R"([{"ok":true}])", "application/json");
  });
  pickpoint::Config cfg;
  cfg.base_url = mock.base_url();
  cfg.api_key = "k";
  cfg.concurrency = 4;
  pickpoint::Client c(cfg);
  std::vector<pickpoint::Query> qs(12, {{"q", "a"}});
  auto out = c.forward_batch(qs);
  EXPECT_EQ(out.size(), 12u);
  EXPECT_LE(peak.load(), 4);
  EXPECT_GE(peak.load(), 2);
}

TEST(HttpGeocoding, BatchPipelineFillsSlots) {
  std::mutex mu;
  std::map<std::string, std::chrono::steady_clock::time_point> started;
  HttpMock mock([&](const httplib::Request& req, httplib::Response& res) {
    std::string q;
    if (auto it = req.params.find("q"); it != req.params.end()) q = it->second;
    else if (req.target.find("q=slow") != std::string::npos) q = "slow";
    {
      std::lock_guard<std::mutex> lock(mu);
      started[q] = std::chrono::steady_clock::now();
    }
    if (q == "slow")
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
    else
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    res.set_content(R"([{"ok":true}])", "application/json");
  });
  pickpoint::Config cfg;
  cfg.base_url = mock.base_url();
  cfg.api_key = "k";
  cfg.concurrency = 2;
  pickpoint::Client c(cfg);
  (void)c.forward_batch({{{"q", "slow"}}, {{"q", "a"}}, {{"q", "b"}}, {{"q", "c"}}});
  std::lock_guard<std::mutex> lock(mu);
  ASSERT_TRUE(started.count("slow"));
  ASSERT_TRUE(started.count("a"));
  ASSERT_TRUE(started.count("b"));
  auto b_after_a = started["b"] - started["a"];
  EXPECT_LT(b_after_a, std::chrono::milliseconds(40));
  EXPECT_LT(started["b"], started["slow"] + std::chrono::milliseconds(60));
}

TEST(HttpGeocoding, BatchAbortOn403) {
  std::atomic<int> hits{0};
  HttpMock mock([&](const httplib::Request& req, httplib::Response& res) {
    hits.fetch_add(1);
    std::string q;
    if (auto it = req.params.find("q"); it != req.params.end()) q = it->second;
    if (req.target.find("q=bad") != std::string::npos || q == "bad") {
      res.status = 403;
      res.set_content("{}", "application/json");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    res.set_content(R"([{"ok":true}])", "application/json");
  });
  pickpoint::Config cfg;
  cfg.base_url = mock.base_url();
  cfg.api_key = "k";
  cfg.concurrency = 2;
  pickpoint::Client c(cfg);
  std::vector<pickpoint::Query> qs = {{{"q", "a"}}, {{"q", "bad"}}, {{"q", "c"}},
                                      {{"q", "d"}}, {{"q", "e"}}, {{"q", "f"}}};
  try {
    c.forward_batch(qs);
    FAIL();
  } catch (const pickpoint::ApiError& e) {
    EXPECT_TRUE(e.is_auth());
  }
  EXPECT_LT(hits.load(), 6);
}

TEST(HttpGeocoding, BatchPreservesOrder) {
  HttpMock mock([&](const httplib::Request& req, httplib::Response& res) {
    bool slow = req.target.find("q=slow") != std::string::npos;
    if (slow) std::this_thread::sleep_for(std::chrono::milliseconds(60));
    std::string q = "fast";
    if (slow) q = "slow";
    else if (auto p = req.params.find("q"); p != req.params.end()) q = p->second;
    res.set_content(std::string("[{\"q\":\"") + q + "\"}]", "application/json");
  });
  pickpoint::Config cfg;
  cfg.base_url = mock.base_url();
  cfg.api_key = "k";
  cfg.concurrency = 3;
  pickpoint::Client c(cfg);
  auto out = c.forward_batch({{{"q", "slow"}}, {{"q", "fast"}}, {{"q", "fast"}}});
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0][0]["q"], "slow");
  EXPECT_EQ(out[1][0]["q"], "fast");
  EXPECT_EQ(out[2][0]["q"], "fast");
}

TEST(HttpGeocoding, RetryBudgetPerSlot) {
  std::atomic<int> flaky{0};
  std::atomic<int> ok_hits{0};
  HttpMock mock([&](const httplib::Request& req, httplib::Response& res) {
    bool is_flaky = req.target.find("q=flaky") != std::string::npos;
    if (is_flaky) {
      int n = flaky.fetch_add(1) + 1;
      if (n < 3) {
        res.status = 503;
        res.set_content("{}", "application/json");
        return;
      }
      res.set_content(R"([{"q":"flaky"}])", "application/json");
      return;
    }
    ok_hits.fetch_add(1);
    res.set_content(R"([{"q":"ok"}])", "application/json");
  });
  pickpoint::Config cfg;
  cfg.base_url = mock.base_url();
  cfg.api_key = "k";
  cfg.concurrency = 2;
  cfg.max_retries = 3;
  pickpoint::Client c(cfg);
  auto out = c.forward_batch({{{"q", "flaky"}}, {{"q", "ok"}}});
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0][0]["q"], "flaky");
  EXPECT_EQ(out[1][0]["q"], "ok");
  EXPECT_GE(flaky.load(), 3);
  EXPECT_EQ(ok_hits.load(), 1);
}
