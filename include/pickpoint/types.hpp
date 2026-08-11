#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pickpoint/config.hpp"

namespace pickpoint {

using Json = nlohmann::json;
using Query = std::map<std::string, std::string>;

struct TokenPair {
  std::string access_token;
  std::string refresh_token;
  std::int64_t expires_at = 0;
  int expires_in = 0;
  std::vector<std::string> scopes;
};

struct Device {
  std::int64_t id = 0;
  std::string uid;
  std::string name;
  std::string status;
  std::optional<std::string> description;
  int tracks_count = 0;
  std::string type;
  std::string secret;
  std::optional<std::string> metadata;
  std::string created_at;
  std::string updated_at;
  Json last_location = nullptr;
};

struct DeviceInput {
  std::string name;
  std::string type;
  std::optional<std::string> description;
  std::optional<std::string> metadata;
};

struct DeviceListQuery {
  std::optional<int> skip;
  std::optional<int> take;
  std::optional<std::string> search;
  bool idle = false;
};

struct DeviceListResult {
  std::vector<Device> data;
  int total = 0;
};

struct DeviceCommandResult {
  int delivered = 0;
};

TokenPair mint_client_tokens(const Config& cfg,
                             const std::vector<std::string>& scopes = {},
                             std::optional<int> ttl_sec = std::nullopt);

}  // namespace pickpoint
