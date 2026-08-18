#pragma once

#include <arpa/inet.h>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include "pickpoint/tracking.hpp"

namespace pickpoint::test {

inline constexpr const char* kMockTrackUid = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
inline constexpr const char* kMockDeviceUid = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
inline constexpr const char* kMockNodeId = "cccccccc-cccc-cccc-cccc-cccccccccccc";

inline int free_tcp_port() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("socket");
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    throw std::runtime_error("bind");
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
    ::close(fd);
    throw std::runtime_error("getsockname");
  }
  int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

struct MockConn {
  std::shared_ptr<ix::WebSocket> ws;
  mutable std::mutex mu;
  std::vector<pickpoint::tracking::ClientMsg> messages;

  void send(const pickpoint::tracking::ServerMsg& msg) {
    if (ws) {
      auto bin = pickpoint::tracking::encode_server_msg(msg);
      ws->sendBinary(std::string(bin.begin(), bin.end()));
    }
  }
  void close() {
    if (ws) ws->close();
  }
};

struct MockOpts {
  bool auto_reply = true;
  std::function<void(const pickpoint::tracking::ClientMsg&, MockConn&)> on_msg;
  std::function<void(int, MockConn&)> before_hello;
  std::shared_ptr<pickpoint::tracking::Relocate> relocate_on_connect;
};

class MockServer {
 public:
  explicit MockServer(MockOpts opts = {}) : opts_(std::move(opts)) {
    ix::initNetSystem();
    port_ = free_tcp_port();
    server_ = std::make_unique<ix::WebSocketServer>(port_, "127.0.0.1");
    server_->disablePerMessageDeflate();

    server_->setOnClientMessageCallback(
        [this](std::shared_ptr<ix::ConnectionState>, ix::WebSocket& webSocket,
               const ix::WebSocketMessagePtr& msg) {
          if (msg->type == ix::WebSocketMessageType::Open) {
            auto conn = std::make_shared<MockConn>();
            for (auto& c : server_->getClients()) {
              if (c.get() == &webSocket) {
                conn->ws = c;
                break;
              }
            }
            if (!conn->ws) return;
            int idx = 0;
            {
              std::lock_guard<std::mutex> lock(mu_);
              connections_.push_back(conn);
              idx = static_cast<int>(connections_.size());
            }
            if (opts_.before_hello) opts_.before_hello(idx, *conn);
            if (opts_.relocate_on_connect && idx == 1) {
              pickpoint::tracking::ServerMsg out;
              out.relocate = *opts_.relocate_on_connect;
              conn->send(out);
            } else {
              pickpoint::tracking::ServerMsg out;
              pickpoint::tracking::Hello hello;
              hello.version = pickpoint::tracking::kProtocolVersion;
              hello.node_id = kMockNodeId;
              out.hello = hello;
              conn->send(out);
            }
            return;
          }
          if (msg->type != ix::WebSocketMessageType::Message) return;
          auto conn = find_conn(webSocket);
          if (!conn) return;
          pickpoint::tracking::ClientMsg cm;
          try {
            cm = pickpoint::tracking::decode_client_msg(msg->str);
          } catch (...) {
            return;
          }
          {
            std::lock_guard<std::mutex> lock(conn->mu);
            conn->messages.push_back(cm);
          }
          if (opts_.on_msg) opts_.on_msg(cm, *conn);
          if (opts_.auto_reply) handle_auto(*conn, cm);
        });

    auto res = server_->listenAndStart();
    if (!res) throw std::runtime_error("mock ws listenAndStart failed on port " + std::to_string(port_));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    url_ = "ws://127.0.0.1:" + std::to_string(port_);
  }

  ~MockServer() {
    if (server_) server_->stop();
  }

  const std::string& url() const { return url_; }
  int port() const { return port_; }
  int conn_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<int>(connections_.size());
  }

  std::shared_ptr<MockConn> wait_conn(std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (!connections_.empty()) return connections_.front();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("wait_conn timeout");
  }

  pickpoint::tracking::ClientMsg wait_msg(
      std::function<bool(const pickpoint::tracking::ClientMsg&)> pred,
      std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& c : connections_) {
          std::lock_guard<std::mutex> cl(c->mu);
          for (const auto& m : c->messages) {
            if (pred(m)) return m;
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("wait_msg timeout");
  }

  std::vector<std::shared_ptr<MockConn>> connections() const {
    std::lock_guard<std::mutex> lock(mu_);
    return connections_;
  }

 private:
  std::shared_ptr<MockConn> find_conn(ix::WebSocket& webSocket) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& c : connections_) {
      if (c->ws.get() == &webSocket) return c;
    }
    return nullptr;
  }

  void handle_auto(MockConn& c, const pickpoint::tracking::ClientMsg& msg) {
    using namespace pickpoint::tracking;
    if (msg.track_start) {
      ServerMsg out;
      out.track_started = TrackStarted{kMockTrackUid, {}};
      c.send(out);
    } else if (msg.track_stop) {
      ServerMsg out;
      out.track_stopped = TrackStopped{kMockTrackUid};
      c.send(out);
    } else if (msg.resume) {
      ServerMsg out;
      out.resume_ok = ResumeOk{msg.resume->track_uid, 0};
      c.send(out);
    } else if (msg.loc) {
      ServerMsg out;
      out.ack = Ack{msg.loc->seq};
      c.send(out);
    } else if (msg.subscribe) {
      ServerMsg out;
      Subscribed s;
      s.sub = next_sub_++;
      s.device_uid = msg.subscribe->device_uid;
      s.track_uid = kMockTrackUid;
      s.online = true;
      out.subscribed = s;
      c.send(out);
    }
  }

  MockOpts opts_;
  std::unique_ptr<ix::WebSocketServer> server_;
  mutable std::mutex mu_;
  std::vector<std::shared_ptr<MockConn>> connections_;
  int port_ = 0;
  std::string url_;
  std::uint8_t next_sub_ = 1;
};

inline pickpoint::tracking::ServerMsg server_error(pickpoint::tracking::ErrorCode code,
                                                   const std::string& message) {
  pickpoint::tracking::ServerMsg msg;
  pickpoint::tracking::WireError err;
  err.code = code;
  err.message = message;
  msg.error = err;
  return msg;
}

inline void wait_for(std::function<bool()> pred,
                     std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
  throw std::runtime_error("wait_for timeout");
}

}  // namespace pickpoint::test
