#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "pickpoint/pickpoint.hpp"

namespace {

constexpr int kE2EBatchSize = 1000;

std::optional<pickpoint::Config> e2e_config() {
  const char* key = std::getenv("PICKPOINT_API_KEY");
  if (!key || !*key) return std::nullopt;
  pickpoint::Config cfg;
  cfg.api_key = key;
  const char* base = std::getenv("PICKPOINT_BASE_URL");
  cfg.base_url = base && *base ? base : "https://beta-api.pickpoint.io";
  cfg.timeout = std::chrono::seconds(60);
  return cfg;
}

}  // namespace

TEST(E2E, ForwardBatch1000) {
  auto cfg = e2e_config();
  if (!cfg) GTEST_SKIP() << "PICKPOINT_API_KEY not set";
  pickpoint::Client c(*cfg);
  std::vector<pickpoint::Query> qs(kE2EBatchSize, {{"q", "Berlin"}, {"limit", "1"}});
  auto start = std::chrono::steady_clock::now();
  auto out = c.forward_batch(qs);
  auto wall = std::chrono::steady_clock::now() - start;
  ASSERT_EQ(out.size(), static_cast<size_t>(kE2EBatchSize));
  for (size_t i = 0; i < out.size(); ++i) {
    ASSERT_FALSE(out[i].empty()) << "slot " << i;
  }
  RecordProperty("wall_ms",
                 std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(wall).count()));
}

TEST(E2E, ReverseBatch1000) {
  auto cfg = e2e_config();
  if (!cfg) GTEST_SKIP() << "PICKPOINT_API_KEY not set";
  pickpoint::Client c(*cfg);
  std::vector<pickpoint::Query> qs(kE2EBatchSize, {{"lat", "52.52"}, {"lon", "13.405"}});
  auto out = c.reverse_batch(qs);
  ASSERT_EQ(out.size(), static_cast<size_t>(kE2EBatchSize));
  for (size_t i = 0; i < out.size(); ++i) {
    ASSERT_FALSE(out[i].is_null()) << "slot " << i;
  }
}
