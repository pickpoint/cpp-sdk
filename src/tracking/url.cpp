#include "pickpoint/tracking.hpp"

#include <stdexcept>

namespace pickpoint::tracking {

std::string build_ws_url(const Config& cfg) {
  std::string raw = cfg.endpoint;
  // trim
  while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t')) raw.erase(raw.begin());
  while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t')) raw.pop_back();
  if (raw.empty()) throw std::invalid_argument("tracking: Endpoint is required");
  if (raw.find("://") == std::string::npos) raw = "ws://" + raw;

  auto scheme_end = raw.find("://");
  std::string scheme = raw.substr(0, scheme_end);
  std::string rest = raw.substr(scheme_end + 3);
  if (scheme == "http") scheme = "ws";
  else if (scheme == "https") scheme = "wss";
  else if (scheme != "ws" && scheme != "wss")
    throw std::invalid_argument("tracking: unsupported scheme");

  auto slash = rest.find('/');
  std::string host = slash == std::string::npos ? rest : rest.substr(0, slash);
  std::string path = cfg.ws_path.empty() ? "/v2/tracking/ws" : cfg.ws_path;

  std::string q;
  if (cfg.device) {
    q = "client-id=" + cfg.device->client_id + "&client-secret=" + cfg.device->client_secret;
  } else if (cfg.listener) {
    q = "access-token=" + cfg.listener->access_token;
  }
  return scheme + "://" + host + path + (q.empty() ? "" : "?" + q);
}

}  // namespace pickpoint::tracking
