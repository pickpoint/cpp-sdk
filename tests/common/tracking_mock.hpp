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

inline std::string encode_server_msg(const ::tracking::v2::ServerMsg& msg) {
  std::string out;
  if (!msg.SerializeToString(&out)) throw std::runtime_error("encode server msg");
  return out;
}

struct MockConn {
  std::shared_ptr<ix::WebSocket> ws;
  mutable std::mutex mu;
  std::vector<::tracking::v2::ClientMsg> messages;

  void send(const ::tracking::v2::ServerMsg& msg) {
    if (ws) ws->sendBinary(encode_server_msg(msg));
  }
  void close() {
    if (ws) ws->close();
  }
};

struct MockOpts {
  bool auto_reply = true;
  std::function<void(const ::tracking::v2::ClientMsg&, MockConn&)> on_msg;
  std::function<void(int, MockConn&)> before_hello;
  std::shared_ptr<::tracking::v2::Relocate> relocate_on_connect;
};

class MockServer {
 public:
  explicit MockServer(MockOpts opts = {}) : opts_(std::move(opts)) {
    ix::initNetSystem();
    port_ = free_tcp_port();
    server_ = std::make_unique<ix::WebSocketServer>(port_, "127.0.0.1");
    server_->disablePerMessageDeflate();

    // Use only setOnClientMessageCallback — setOnConnectionCallback requires
    // setOnMessageCallback on the socket and otherwise aborts the handshake.
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
              ::tracking::v2::ServerMsg out;
              *out.mutable_relocate() = *opts_.relocate_on_connect;
              conn->send(out);
            } else {
              ::tracking::v2::ServerMsg out;
              out.mutable_hello()->set_node_id("mock-1");
              conn->send(out);
            }
            return;
          }
          if (msg->type != ix::WebSocketMessageType::Message) return;
          auto conn = find_conn(webSocket);
          if (!conn) return;
          ::tracking::v2::ClientMsg cm;
          if (!cm.ParseFromString(msg->str)) return;
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

  ::tracking::v2::ClientMsg wait_msg(std::function<bool(const ::tracking::v2::ClientMsg&)> pred,
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

  static void handle_auto(MockConn& c, const ::tracking::v2::ClientMsg& msg) {
    switch (msg.body_case()) {
      case ::tracking::v2::ClientMsg::kTrackStart: {
        ::tracking::v2::ServerMsg out;
        out.mutable_track_started()->set_track_uid("track-mock-1");
        c.send(out);
        break;
      }
      case ::tracking::v2::ClientMsg::kTrackStop: {
        ::tracking::v2::ServerMsg out;
        out.mutable_track_stopped()->set_track_uid(msg.track_stop().track_uid());
        c.send(out);
        break;
      }
      case ::tracking::v2::ClientMsg::kResume: {
        ::tracking::v2::ServerMsg out;
        out.mutable_resume_ok()->set_track_uid(msg.resume().track_uid());
        out.mutable_resume_ok()->set_last_acked_seq(0);
        c.send(out);
        break;
      }
      case ::tracking::v2::ClientMsg::kLocationAdd: {
        ::tracking::v2::ServerMsg out;
        auto* la = out.mutable_location_added();
        la->set_track_uid(msg.location_add().track_uid());
        la->set_client_seq(msg.location_add().client_seq());
        la->set_device_uid("dev-1");
        *la->mutable_point() = msg.location_add().point();
        c.send(out);
        break;
      }
      case ::tracking::v2::ClientMsg::kLocationBatch: {
        ::tracking::v2::ServerMsg out;
        auto* la = out.mutable_location_added();
        la->set_track_uid(msg.location_batch().track_uid());
        la->set_client_seq(msg.location_batch().client_seq());
        la->set_device_uid("dev-1");
        c.send(out);
        break;
      }
      case ::tracking::v2::ClientMsg::kSubscribe: {
        ::tracking::v2::ServerMsg out;
        out.mutable_subscribed()->set_device_uid(msg.subscribe().device_uid());
        out.mutable_subscribed()->set_track_uid("track-mock-1");
        c.send(out);
        break;
      }
      default:
        break;
    }
  }

  MockOpts opts_;
  std::unique_ptr<ix::WebSocketServer> server_;
  mutable std::mutex mu_;
  std::vector<std::shared_ptr<MockConn>> connections_;
  int port_ = 0;
  std::string url_;
};

inline ::tracking::v2::ServerMsg server_error(::tracking::v2::ErrorCode code,
                                              const std::string& message) {
  ::tracking::v2::ServerMsg msg;
  msg.mutable_error()->set_code(code);
  msg.mutable_error()->set_message(message);
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
