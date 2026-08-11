#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include <httplib.h>

namespace pickpoint::test {

class HttpMock {
 public:
  using Handler = std::function<void(const httplib::Request&, httplib::Response&)>;

  explicit HttpMock(Handler handler) : handler_(std::move(handler)) {
    auto wrap = [this](const httplib::Request& req, httplib::Response& res) {
      if (req.path == "/__pickpoint_ready") {
        res.status = 204;
        return;
      }
      if (handler_) handler_(req, res);
    };
    svr_.Get(R"(.*)", wrap);
    svr_.Post(R"(.*)", wrap);
    svr_.Patch(R"(.*)", wrap);
    svr_.Delete(R"(.*)", wrap);
    port_ = svr_.bind_to_any_port("127.0.0.1");
    thread_ = std::thread([this] { svr_.listen_after_bind(); });
    for (int i = 0; i < 200; ++i) {
      httplib::Client c("127.0.0.1", port_);
      c.set_connection_timeout(0, 100'000);
      auto r = c.Get("/__pickpoint_ready");
      if (r && r->status == 204) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  ~HttpMock() {
    svr_.stop();
    if (thread_.joinable()) thread_.join();
  }

  std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port_); }

 private:
  Handler handler_;
  httplib::Server svr_;
  int port_ = 0;
  std::thread thread_;
};

inline std::int64_t expires_at_ms(std::chrono::milliseconds from_now) {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() +
         from_now.count();
}

}  // namespace pickpoint::test
