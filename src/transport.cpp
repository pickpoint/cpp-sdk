#include "transport.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

namespace pickpoint {
namespace {

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::vector<std::uint8_t>*>(userdata);
  size_t n = size * nmemb;
  out->insert(out->end(), reinterpret_cast<std::uint8_t*>(ptr),
              reinterpret_cast<std::uint8_t*>(ptr) + n);
  return n;
}

std::string message_from_body(const std::vector<std::uint8_t>& raw, int status) {
  try {
    auto j = nlohmann::json::parse(raw.begin(), raw.end());
    if (j.contains("message") && j["message"].is_string()) return j["message"].get<std::string>();
    if (j.contains("error") && j["error"].is_string()) return j["error"].get<std::string>();
  } catch (...) {
  }
  return "HTTP " + std::to_string(status);
}

void sleep_backoff(std::chrono::milliseconds base, int attempt) {
  static thread_local std::mt19937 rng{std::random_device{}()};
  double exp = static_cast<double>(base.count()) * std::pow(2.0, attempt);
  std::uniform_real_distribution<double> dist(0.0, exp);
  auto ms = static_cast<int>(dist(rng));
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

class StaticSession : public AuthSession {
 public:
  explicit StaticSession(std::string token) : token_(std::move(token)) {}
  std::string token() override { return token_; }
  bool refresh_after_unauthorized() override { return false; }

 private:
  std::string token_;
};

class ClientAuthSession : public AuthSession {
 public:
  ClientAuthSession(ClientAuth initial, std::string base_url, long timeout_ms)
      : access_(std::move(initial.access_token)),
        refresh_(std::move(initial.refresh_token)),
        expires_at_(initial.expires_at),
        issued_at_(std::chrono::steady_clock::now()),
        base_url_(std::move(base_url)),
        timeout_ms_(timeout_ms) {
    if (access_.empty() || refresh_.empty() || expires_at_ == 0) {
      throw InvalidConfigError("client_auth requires access_token, refresh_token, and expires_at");
    }
  }

  std::string token() override {
    std::lock_guard<std::mutex> lock(mu_);
    if (needs_proactive_refresh()) {
      refresh_locked();
    }
    return access_;
  }

  bool refresh_after_unauthorized() override {
    std::lock_guard<std::mutex> lock(mu_);
    // Coalesce concurrent 401 retries: if another waiter just refreshed, reuse.
    using namespace std::chrono;
    auto now = steady_clock::now();
    if (last_refresh_at_.time_since_epoch().count() != 0 &&
        now - last_refresh_at_ < seconds(2)) {
      return true;
    }
    try {
      refresh_locked();
      return true;
    } catch (...) {
      return false;
    }
  }

 private:
  bool needs_proactive_refresh() const {
    using namespace std::chrono;
    auto now_ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    auto elapsed = duration_cast<milliseconds>(steady_clock::now() - issued_at_).count();
    std::int64_t issued_abs = now_ms - elapsed;
    std::int64_t ttl_ms = expires_at_ - issued_abs;
    if (ttl_ms <= 0) return true;  // already expired
    return elapsed >= static_cast<std::int64_t>(ttl_ms * kClientAuthRefreshAt);
  }

  void refresh_locked() {
    CURL* curl = curl_easy_init();
    if (!curl) throw ApiError(0, "NETWORK", "curl init failed");
    std::string url = base_url_ + "/v2/client-tokens/refresh";
    nlohmann::json body = {{"refreshToken", refresh_}};
    std::string body_s = body.dump();
    std::vector<std::uint8_t> resp;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_s.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_s.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms_);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
      throw ApiError(0, "REFRESH_FAILED", std::string("refresh network error: ") + curl_easy_strerror(rc));
    }
    if (status < 200 || status >= 300) {
      throw ApiError(static_cast<int>(status), "REFRESH_FAILED", "refresh failed");
    }
    auto j = nlohmann::json::parse(resp.begin(), resp.end());
    access_ = j.value("accessToken", "");
    refresh_ = j.value("refreshToken", refresh_);
    expires_at_ = j.value("expiresAt", std::int64_t{0});
    issued_at_ = std::chrono::steady_clock::now();
    last_refresh_at_ = issued_at_;
    if (access_.empty()) throw ApiError(0, "REFRESH_FAILED", "empty access token");
  }

  std::mutex mu_;
  std::string access_;
  std::string refresh_;
  std::int64_t expires_at_;
  std::chrono::steady_clock::time_point issued_at_;
  std::chrono::steady_clock::time_point last_refresh_at_{};
  std::string base_url_;
  long timeout_ms_;
};

}  // namespace

std::string trim_slash(std::string s) {
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

std::string url_encode(const std::string& s) {
  CURL* c = curl_easy_init();
  if (!c) return s;
  char* enc = curl_easy_escape(c, s.c_str(), static_cast<int>(s.size()));
  std::string out = enc ? enc : s;
  if (enc) curl_free(enc);
  curl_easy_cleanup(c);
  return out;
}

AuthState AuthState::from_config(const Config& cfg, const std::string& base_url, long timeout_ms) {
  int n = (cfg.api_key ? 1 : 0) + (cfg.client_auth ? 1 : 0) + (cfg.access_token ? 1 : 0);
  if (n != 1) {
    throw InvalidConfigError("provide exactly one of api_key, client_auth, access_token");
  }
  AuthState a;
  if (cfg.api_key) {
    a.api_key_ = *cfg.api_key;
  } else if (cfg.access_token) {
    a.session_ = std::make_unique<StaticSession>(*cfg.access_token);
  } else {
    a.session_ = std::make_unique<ClientAuthSession>(*cfg.client_auth, base_url, timeout_ms);
  }
  return a;
}

void AuthState::apply(struct curl_slist*& headers) {
  headers = curl_slist_append(headers, "Accept: application/json");
  if (api_key_) {
    std::string h = "x-api-key: " + *api_key_;
    headers = curl_slist_append(headers, h.c_str());
    return;
  }
  std::string h = "Authorization: Bearer " + session_->token();
  headers = curl_slist_append(headers, h.c_str());
}

bool AuthState::refresh_after_unauthorized() {
  if (!session_) return false;
  return session_->refresh_after_unauthorized();
}

Transport::Transport(std::string base_url, AuthState auth, int max_retries,
                     std::chrono::milliseconds retry_base, long timeout_ms)
    : base_url_(std::move(base_url)),
      auth_(std::move(auth)),
      max_retries_(max_retries),
      retry_base_(retry_base),
      timeout_ms_(timeout_ms) {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::vector<std::uint8_t> Transport::do_request(const RequestOpts& opts) {
  int attempt = 0;
  bool auth_retried = false;

  for (;;) {
    std::string url = base_url_ + opts.path;
    if (!opts.query.empty()) {
      url += "?";
      bool first = true;
      for (const auto& [k, v] : opts.query) {
        if (!first) url += "&";
        first = false;
        url += url_encode(k) + "=" + url_encode(v);
      }
    }

    std::string body_s;
    if (opts.body) body_s = opts.body->dump();

    std::vector<std::uint8_t> resp;
    CURL* curl = curl_easy_init();
    if (!curl) throw ApiError(0, "NETWORK", "curl init failed");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms_);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    struct curl_slist* headers = nullptr;
    auth_.apply(headers);
    std::string method = opts.method;
    if (method.empty()) method = opts.body ? "POST" : "GET";

    if (opts.body) {
      headers = curl_slist_append(headers, "Content-Type: application/json");
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_s.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_s.size()));
    }

    if (method == "POST") {
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
    } else if (method == "GET") {
      curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (method == "PATCH") {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
      if (!opts.body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    } else if (method == "DELETE") {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    } else {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
      if (attempt >= max_retries_) {
        throw ApiError(0, "NETWORK", std::string("network error: ") + curl_easy_strerror(rc));
      }
      sleep_backoff(retry_base_, attempt);
      ++attempt;
      continue;
    }

    if (status == 401) {
      if (!auth_retried && auth_.is_bearer() && auth_.refresh_after_unauthorized()) {
        auth_retried = true;
        continue;
      }
      throw ApiError(401, "API_AUTH", "auth failed (401)", resp);
    }
    if (status == 402 || status == 403) {
      throw ApiError(static_cast<int>(status), "API_AUTH", "auth failed", resp);
    }
    if (status == 204) return {};
    if (status == 409) {
      throw ApiError(409, "CONFLICT", message_from_body(resp, 409), resp);
    }
    if (status == 400 || (status >= 404 && status < 500)) {
      if (opts.on_client_error == OnClientError::kEmpty) {
        return opts.empty_body.value_or(std::vector<std::uint8_t>{});
      }
      std::string code = status == 404 ? "NOT_FOUND" : "CLIENT_ERROR";
      throw ApiError(static_cast<int>(status), code, message_from_body(resp, static_cast<int>(status)),
                     resp);
    }
    if (status >= 500) {
      if (attempt >= max_retries_) {
        throw ApiError(static_cast<int>(status), "SERVER_ERROR",
                       message_from_body(resp, static_cast<int>(status)), resp);
      }
      sleep_backoff(retry_base_, attempt);
      ++attempt;
      continue;
    }
    if (status < 200 || status >= 300) {
      throw ApiError(static_cast<int>(status), "HTTP_ERROR",
                     message_from_body(resp, static_cast<int>(status)), resp);
    }
    return resp;
  }
}

}  // namespace pickpoint
