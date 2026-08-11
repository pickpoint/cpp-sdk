#include "pickpoint/client.hpp"

#include <atomic>
#include <future>
#include <mutex>

#include "transport.hpp"

namespace pickpoint {
namespace {

Device device_from_json(const nlohmann::json& d) {
  Device out;
  out.id = d.value("id", std::int64_t{0});
  out.uid = d.value("uid", "");
  out.name = d.value("name", "");
  out.status = d.value("status", "");
  if (d.contains("description") && !d["description"].is_null())
    out.description = d["description"].get<std::string>();
  out.tracks_count = d.value("tracksCount", 0);
  out.type = d.value("type", "");
  out.secret = d.value("secret", "");
  if (d.contains("metadata") && !d["metadata"].is_null())
    out.metadata = d["metadata"].is_string() ? d["metadata"].get<std::string>()
                                             : d["metadata"].dump();
  out.created_at = d.value("createdAt", "");
  out.updated_at = d.value("updatedAt", "");
  if (d.contains("lastLocation")) out.last_location = d["lastLocation"];
  return out;
}

nlohmann::json device_input_json(const DeviceInput& in) {
  nlohmann::json j = {{"name", in.name}, {"type", in.type}};
  if (in.description) j["description"] = *in.description;
  if (in.metadata) j["metadata"] = *in.metadata;
  return j;
}

std::string b64_encode(const std::vector<std::uint8_t>& data) {
  static const char* k =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((data.size() + 2) / 3) * 4);
  for (size_t i = 0; i < data.size(); i += 3) {
    std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
    if (i + 1 < data.size()) n |= static_cast<std::uint32_t>(data[i + 1]) << 8;
    if (i + 2 < data.size()) n |= static_cast<std::uint32_t>(data[i + 2]);
    out.push_back(k[(n >> 18) & 63]);
    out.push_back(k[(n >> 12) & 63]);
    out.push_back(i + 1 < data.size() ? k[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < data.size() ? k[n & 63] : '=');
  }
  return out;
}

template <typename Fn>
auto run_batch(int concurrency, size_t n, Fn&& fn) -> std::vector<decltype(fn(size_t{0}))> {
  using T = decltype(fn(size_t{0}));
  std::vector<T> out(n);
  if (n == 0) return out;

  std::atomic<size_t> next{0};
  std::atomic<bool> abort{false};
  std::mutex err_mu;
  std::exception_ptr first_err;

  auto worker = [&]() {
    for (;;) {
      if (abort.load()) break;
      size_t i = next.fetch_add(1);
      if (i >= n) break;
      try {
        out[i] = fn(i);
      } catch (...) {
        {
          std::lock_guard<std::mutex> lock(err_mu);
          if (!first_err) first_err = std::current_exception();
        }
        abort.store(true);
        break;
      }
    }
  };

  int workers = std::max(1, std::min(concurrency, static_cast<int>(n)));
  std::vector<std::future<void>> futs;
  for (int w = 0; w < workers; ++w) futs.push_back(std::async(std::launch::async, worker));
  for (auto& f : futs) f.get();
  if (first_err) std::rethrow_exception(first_err);
  return out;
}

RequestOpts get_opts(std::string path, Query q, OnClientError on_err,
                     std::vector<std::uint8_t> empty = {}) {
  RequestOpts o;
  o.method = "GET";
  o.path = std::move(path);
  o.query = std::move(q);
  o.on_client_error = on_err;
  if (!empty.empty()) o.empty_body = std::move(empty);
  return o;
}

}  // namespace

Client::Client(Config cfg) {
  base_url_ = trim_slash(cfg.base_url.value_or(kDefaultBaseUrl));
  auto timeout = cfg.timeout.value_or(kDefaultTimeout);
  auto timeout_ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();
  int max_retries = cfg.max_retries.value_or(kDefaultMaxRetries);
  auto retry_base = cfg.retry_base.value_or(kDefaultRetryBase);
  if (retry_base < kMinRetryBase) retry_base = kMinRetryBase;
  concurrency_ = cfg.concurrency.value_or(kDefaultConcurrency);
  if (concurrency_ > kMaxConcurrency) concurrency_ = kMaxConcurrency;
  if (concurrency_ <= 0) concurrency_ = kDefaultConcurrency;

  auto auth = AuthState::from_config(cfg, base_url_, static_cast<long>(timeout_ms));
  transport_ = std::make_unique<Transport>(base_url_, std::move(auth), max_retries, retry_base,
                                           static_cast<long>(timeout_ms));
}

Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

Json Client::forward(const Query& q) {
  auto raw = transport_->do_request(
      get_opts("/v2/geocode/forward", q, OnClientError::kEmpty, {'[', ']'}));
  if (raw.empty()) return Json::array();
  return Json::parse(raw.begin(), raw.end());
}

Json Client::reverse(const Query& q) {
  auto raw = transport_->do_request(
      get_opts("/v2/geocode/reverse", q, OnClientError::kEmpty, {'n', 'u', 'l', 'l'}));
  if (raw.empty()) return nullptr;
  return Json::parse(raw.begin(), raw.end());
}

Json Client::lookup(const Query& q) {
  auto raw = transport_->do_request(
      get_opts("/v2/address/lookup", q, OnClientError::kEmpty, {'[', ']'}));
  if (raw.empty()) return Json::array();
  return Json::parse(raw.begin(), raw.end());
}

std::vector<Json> Client::forward_batch(const std::vector<Query>& qs) {
  return run_batch(concurrency_, qs.size(), [&](size_t i) { return forward(qs[i]); });
}

std::vector<Json> Client::reverse_batch(const std::vector<Query>& qs) {
  return run_batch(concurrency_, qs.size(), [&](size_t i) { return reverse(qs[i]); });
}

std::vector<Json> Client::lookup_batch(const std::vector<Query>& qs) {
  return run_batch(concurrency_, qs.size(), [&](size_t i) { return lookup(qs[i]); });
}

Json Client::search(const Query& q) {
  auto raw = transport_->do_request(get_opts("/v2/address/search", q, OnClientError::kThrow));
  return Json::parse(raw.begin(), raw.end());
}

Json Client::route(const Json& body) {
  RequestOpts o;
  o.method = "POST";
  o.path = "/v2/route";
  o.body = body;
  auto raw = transport_->do_request(o);
  return Json::parse(raw.begin(), raw.end());
}

Json Client::optimized_route(const Json& body) {
  RequestOpts o;
  o.method = "POST";
  o.path = "/v2/route/optimized";
  o.body = body;
  auto raw = transport_->do_request(o);
  return Json::parse(raw.begin(), raw.end());
}

Json Client::matrix(const Json& body) {
  RequestOpts o;
  o.method = "POST";
  o.path = "/v2/route/matrix";
  o.body = body;
  auto raw = transport_->do_request(o);
  return Json::parse(raw.begin(), raw.end());
}

Json Client::locate(const Json& body) {
  RequestOpts o;
  o.method = "POST";
  o.path = "/v2/route/locate";
  o.body = body;
  auto raw = transport_->do_request(o);
  return Json::parse(raw.begin(), raw.end());
}

Json Client::elevation(const Json& body) {
  RequestOpts o;
  o.method = "POST";
  o.path = "/v2/route/elevation";
  o.body = body;
  auto raw = transport_->do_request(o);
  return Json::parse(raw.begin(), raw.end());
}

DeviceListResult Client::devices_list(const DeviceListQuery& q) {
  Query query;
  if (q.skip && *q.skip > 0) query["skip"] = std::to_string(*q.skip);
  if (q.take && *q.take > 0) query["take"] = std::to_string(*q.take);
  if (q.search) query["search"] = *q.search;
  if (q.idle) query["idle"] = "1";
  auto raw = transport_->do_request(get_opts("/v2/devices", query, OnClientError::kThrow));
  auto j = Json::parse(raw.begin(), raw.end());
  DeviceListResult out;
  out.total = j.value("total", 0);
  for (const auto& item : j.value("data", Json::array())) out.data.push_back(device_from_json(item));
  return out;
}

Device Client::devices_get(const std::string& uid) {
  auto raw = transport_->do_request(get_opts("/v2/devices/" + url_encode(uid), {}, OnClientError::kThrow));
  return device_from_json(Json::parse(raw.begin(), raw.end()));
}

Device Client::devices_create(const DeviceInput& input) {
  RequestOpts o;
  o.method = "POST";
  o.path = "/v2/devices";
  o.body = device_input_json(input);
  auto raw = transport_->do_request(o);
  return device_from_json(Json::parse(raw.begin(), raw.end()));
}

Device Client::devices_update(const std::string& uid, const DeviceInput& input) {
  RequestOpts o;
  o.method = "PATCH";
  o.path = "/v2/devices/" + url_encode(uid);
  o.body = device_input_json(input);
  auto raw = transport_->do_request(o);
  return device_from_json(Json::parse(raw.begin(), raw.end()));
}

void Client::devices_delete(const std::string& uid) {
  RequestOpts o;
  o.method = "DELETE";
  o.path = "/v2/devices/" + url_encode(uid);
  transport_->do_request(o);
}

DeviceCommandResult Client::devices_command(const std::string& uid,
                                            const std::vector<std::uint8_t>& payload) {
  RequestOpts o;
  o.method = "POST";
  o.path = "/v2/devices/" + url_encode(uid) + "/command";
  o.body = Json{{"payload", b64_encode(payload)}};
  auto raw = transport_->do_request(o);
  auto j = Json::parse(raw.begin(), raw.end());
  return DeviceCommandResult{j.value("delivered", 0)};
}

}  // namespace pickpoint
