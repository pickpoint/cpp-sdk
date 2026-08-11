#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "pickpoint/config.hpp"
#include "pickpoint/errors.hpp"

namespace pickpoint {

enum class OnClientError { kThrow, kEmpty };

struct RequestOpts {
  std::string method;
  std::string path;
  std::map<std::string, std::string> query;
  std::optional<nlohmann::json> body;
  OnClientError on_client_error = OnClientError::kThrow;
  std::optional<std::vector<std::uint8_t>> empty_body;
};

class AuthSession {
 public:
  virtual ~AuthSession() = default;
  virtual std::string token() = 0;
  virtual bool refresh_after_unauthorized() = 0;
};

class AuthState {
 public:
  static AuthState from_config(const Config& cfg, const std::string& base_url, long timeout_ms);

  void apply(struct curl_slist*& headers);
  bool refresh_after_unauthorized();
  bool is_bearer() const { return session_ != nullptr; }

 private:
  std::optional<std::string> api_key_;
  std::unique_ptr<AuthSession> session_;
};

class Transport {
 public:
  Transport(std::string base_url, AuthState auth, int max_retries,
            std::chrono::milliseconds retry_base, long timeout_ms);
  ~Transport() = default;

  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;

  std::vector<std::uint8_t> do_request(const RequestOpts& opts);

  const std::string& base_url() const { return base_url_; }
  long timeout_ms() const { return timeout_ms_; }

 private:
  std::string base_url_;
  AuthState auth_;
  int max_retries_;
  std::chrono::milliseconds retry_base_;
  long timeout_ms_;
};

std::string trim_slash(std::string s);
std::string url_encode(const std::string& s);

}  // namespace pickpoint
