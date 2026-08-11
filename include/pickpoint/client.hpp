#pragma once

#include <memory>
#include <string>
#include <vector>

#include "pickpoint/config.hpp"
#include "pickpoint/types.hpp"

namespace pickpoint {

class Transport;

class Client {
 public:
  explicit Client(Config cfg);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) noexcept;
  Client& operator=(Client&&) noexcept;

  Json forward(const Query& q);
  Json reverse(const Query& q);  // null if empty
  Json lookup(const Query& q);
  std::vector<Json> forward_batch(const std::vector<Query>& qs);
  std::vector<Json> reverse_batch(const std::vector<Query>& qs);
  std::vector<Json> lookup_batch(const std::vector<Query>& qs);

  Json search(const Query& q);

  Json route(const Json& body);
  Json optimized_route(const Json& body);
  Json matrix(const Json& body);
  Json locate(const Json& body);
  Json elevation(const Json& body);

  DeviceListResult devices_list(const DeviceListQuery& q = {});
  Device devices_get(const std::string& uid);
  Device devices_create(const DeviceInput& input);
  Device devices_update(const std::string& uid, const DeviceInput& input);
  void devices_delete(const std::string& uid);
  DeviceCommandResult devices_command(const std::string& uid, const std::vector<std::uint8_t>& payload);

  int concurrency() const { return concurrency_; }

 private:
  std::string base_url_;
  int concurrency_ = kDefaultConcurrency;
  std::unique_ptr<Transport> transport_;
};

}  // namespace pickpoint
