#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace pickpoint {

inline constexpr const char* kDefaultBaseUrl = "https://api.pickpoint.io";
inline constexpr int kDefaultMaxRetries = 3;
inline constexpr auto kDefaultRetryBase = std::chrono::milliseconds(1000);
inline constexpr auto kMinRetryBase = std::chrono::milliseconds(200);
inline constexpr auto kDefaultTimeout = std::chrono::seconds(30);
inline constexpr int kMaxConcurrency = 20;
inline constexpr int kDefaultConcurrency = 20;
inline constexpr double kClientAuthRefreshAt = 0.5;

struct ClientAuth {
  std::string access_token;
  std::string refresh_token;
  std::int64_t expires_at = 0;  // unix epoch ms
};

struct Config {
  std::optional<std::string> api_key;
  std::optional<ClientAuth> client_auth;
  std::optional<std::string> access_token;
  std::optional<std::string> base_url;
  std::optional<int> max_retries;
  std::optional<std::chrono::milliseconds> retry_base;
  std::optional<std::chrono::milliseconds> timeout;
  std::optional<int> concurrency;
};

}  // namespace pickpoint
