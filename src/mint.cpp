#include "pickpoint/types.hpp"

#include <curl/curl.h>

#include "transport.hpp"

namespace pickpoint {
namespace {

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::vector<std::uint8_t>*>(userdata);
  size_t n = size * nmemb;
  out->insert(out->end(), reinterpret_cast<std::uint8_t*>(ptr),
              reinterpret_cast<std::uint8_t*>(ptr) + n);
  return n;
}

}  // namespace

TokenPair mint_client_tokens(const Config& cfg, const std::vector<std::string>& scopes,
                             std::optional<int> ttl_sec) {
  if (!cfg.api_key || cfg.api_key->empty()) {
    throw InvalidConfigError("mint_client_tokens requires api_key");
  }
  std::string base = trim_slash(cfg.base_url.value_or(kDefaultBaseUrl));
  auto timeout = cfg.timeout.value_or(kDefaultTimeout);
  auto timeout_ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();

  nlohmann::json body = {{"scopes", scopes}};
  if (ttl_sec && *ttl_sec > 0) body["ttlSec"] = *ttl_sec;
  std::string body_s = body.dump();

  CURL* curl = curl_easy_init();
  if (!curl) throw ApiError(0, "NETWORK", "curl init failed");
  std::vector<std::uint8_t> resp;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/json");
  headers = curl_slist_append(headers, "Content-Type: application/json");
  std::string key_h = "x-api-key: " + *cfg.api_key;
  headers = curl_slist_append(headers, key_h.c_str());
  std::string url = base + "/v2/client-tokens";
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_s.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_s.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
  CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (rc != CURLE_OK) {
    throw ApiError(0, "NETWORK", std::string("mint client tokens network error: ") + curl_easy_strerror(rc));
  }
  if (status < 200 || status >= 300) {
    throw ApiError(static_cast<int>(status), "CLIENT_ERROR",
                   "mint client tokens failed (" + std::to_string(status) + ")", resp);
  }
  auto j = nlohmann::json::parse(resp.begin(), resp.end());
  TokenPair pair;
  pair.access_token = j.value("accessToken", "");
  pair.refresh_token = j.value("refreshToken", "");
  pair.expires_at = j.value("expiresAt", std::int64_t{0});
  pair.expires_in = j.value("expiresIn", 0);
  if (j.contains("scopes") && j["scopes"].is_array()) {
    for (const auto& s : j["scopes"]) pair.scopes.push_back(s.get<std::string>());
  }
  return pair;
}

}  // namespace pickpoint
