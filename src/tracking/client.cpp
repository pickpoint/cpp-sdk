#include "pickpoint/tracking.hpp"

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <queue>
#include <random>
#include <set>
#include <thread>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

namespace pickpoint::tracking {
namespace {

Error error_from_wire(const ::tracking::v2::Error& err) {
  return Error(err.code(), err.message());
}

bool is_fatal_resume(::tracking::v2::ErrorCode code) {
  return code == ::tracking::v2::ERROR_CODE_TRACK_NOT_FOUND ||
         code == ::tracking::v2::ERROR_CODE_FENCED || code == ::tracking::v2::ERROR_CODE_AUTH ||
         code == ::tracking::v2::ERROR_CODE_UNAUTHORIZED;
}

bool is_auth(::tracking::v2::ErrorCode code) {
  return code == ::tracking::v2::ERROR_CODE_AUTH || code == ::tracking::v2::ERROR_CODE_UNAUTHORIZED;
}

}  // namespace

struct Client::Impl {
  Config cfg;
  mutable std::mutex mu;
  ConnectionState state = ConnectionState::kConnecting;
  std::string track_uid;
  std::uint64_t client_seq = 0;
  std::uint64_t last_acked_seq = 0;
  OfflineQueue queue;
  BackoffState backoff;
  std::chrono::steady_clock::time_point next_publish_at{};
  std::chrono::steady_clock::time_point next_event_at{};
  std::set<std::string> subscriptions;
  bool intentional = false;
  std::uint64_t dial_gen = 0;

  std::unique_ptr<ix::WebSocket> ws;
  std::queue<::tracking::v2::ServerMsg> recv_q;
  std::condition_variable recv_cv;

  std::optional<std::promise<std::string>> start_wait;
  std::optional<std::promise<void>> stop_wait;
  std::optional<std::promise<std::uint64_t>> resume_wait;

  std::thread reconnect_thread;
  bool reconnect_stop = false;
  std::mutex stop_mu;
  std::vector<std::thread> stop_threads;

  Impl(Config c)
      : cfg(std::move(c)),
        queue(cfg.max_queue_size),
        backoff(new_backoff(cfg.reconnect_min_delay, cfg.reconnect_max_delay,
                            cfg.reconnect_max_attempts)) {
    if (cfg.hello_timeout.count() <= 0) cfg.hello_timeout = std::chrono::milliseconds(10'000);
  }

  ~Impl() { shutdown(); }

  // WebSocket::stop() joins the worker thread. Never call it from that worker
  // (e.g. from dispatch/handle_auth_error) and never while holding mu.
  void stop_socket(std::unique_ptr<ix::WebSocket> sock) {
    if (!sock) return;
    std::lock_guard<std::mutex> lock(stop_mu);
    stop_threads.emplace_back([s = std::move(sock)]() mutable { s->stop(); });
  }

  void join_stops() {
    std::vector<std::thread> threads;
    {
      std::lock_guard<std::mutex> lock(stop_mu);
      threads.swap(stop_threads);
    }
    for (auto& t : threads) {
      if (t.joinable()) t.join();
    }
  }

  void shutdown() {
    std::unique_ptr<ix::WebSocket> sock;
    {
      std::lock_guard<std::mutex> lock(mu);
      intentional = true;
      reconnect_stop = true;
      state = ConnectionState::kClosed;
      reject_pending(Error(::tracking::v2::ERROR_CODE_INVALID, "client closed"));
      sock = std::move(ws);
    }
    stop_socket(std::move(sock));
    if (reconnect_thread.joinable()) reconnect_thread.join();
    join_stops();
  }

  void reject_pending(const Error& err) {
    if (start_wait) {
      try {
        start_wait->set_exception(std::make_exception_ptr(err));
      } catch (...) {
      }
      start_wait.reset();
    }
    if (stop_wait) {
      try {
        stop_wait->set_exception(std::make_exception_ptr(err));
      } catch (...) {
      }
      stop_wait.reset();
    }
    if (resume_wait) {
      try {
        resume_wait->set_exception(std::make_exception_ptr(err));
      } catch (...) {
      }
      resume_wait.reset();
    }
  }

  void push_recv(::tracking::v2::ServerMsg msg) {
    std::lock_guard<std::mutex> lock(mu);
    if (recv_q.size() < 64) {
      recv_q.push(std::move(msg));
      recv_cv.notify_one();
    }
  }

  void send_locked(const ::tracking::v2::ClientMsg& msg) {
    if (intentional && state == ConnectionState::kClosed) {
      throw Error(::tracking::v2::ERROR_CODE_INVALID, "closed");
    }
    if (!ws) throw Error(::tracking::v2::ERROR_CODE_INVALID, "socket not open");
    auto bin = encode_client_msg(msg);
    ws->sendBinary(bin);
  }

  void dispatch(const ::tracking::v2::ServerMsg& msg) {
    switch (msg.body_case()) {
      case ::tracking::v2::ServerMsg::kResumeOk: {
        std::promise<std::uint64_t> done;
        {
          std::lock_guard<std::mutex> lock(mu);
          if (!msg.resume_ok().track_uid().empty()) track_uid = msg.resume_ok().track_uid();
          last_acked_seq = msg.resume_ok().last_acked_seq();
          if (client_seq < last_acked_seq) client_seq = last_acked_seq;
          queue.ack_through(last_acked_seq);
          if (resume_wait) {
            done = std::move(*resume_wait);
            resume_wait.reset();
          }
        }
        flush_queue();
        try {
          done.set_value(msg.resume_ok().last_acked_seq());
        } catch (...) {
        }
        push_recv(msg);
        break;
      }
      case ::tracking::v2::ServerMsg::kTrackStarted: {
        std::promise<std::string> done;
        {
          std::lock_guard<std::mutex> lock(mu);
          track_uid = msg.track_started().track_uid();
          client_seq = 0;
          last_acked_seq = 0;
          queue.clear();
          if (start_wait) {
            done = std::move(*start_wait);
            start_wait.reset();
          }
        }
        try {
          done.set_value(msg.track_started().track_uid());
        } catch (...) {
        }
        push_recv(msg);
        break;
      }
      case ::tracking::v2::ServerMsg::kTrackStopped: {
        std::promise<void> done;
        {
          std::lock_guard<std::mutex> lock(mu);
          if (track_uid == msg.track_stopped().track_uid()) {
            track_uid.clear();
            queue.clear();
          }
          if (stop_wait) {
            done = std::move(*stop_wait);
            stop_wait.reset();
          }
        }
        try {
          done.set_value();
        } catch (...) {
        }
        push_recv(msg);
        break;
      }
      case ::tracking::v2::ServerMsg::kLocationAdded: {
        {
          std::lock_guard<std::mutex> lock(mu);
          if (msg.location_added().client_seq() > last_acked_seq)
            last_acked_seq = msg.location_added().client_seq();
          queue.ack_through(msg.location_added().client_seq());
        }
        push_recv(msg);
        break;
      }
      case ::tracking::v2::ServerMsg::kError: {
        Error err = error_from_wire(msg.error());
        {
          std::lock_guard<std::mutex> lock(mu);
          if (resume_wait) {
            if (is_fatal_resume(err.code)) {
              track_uid.clear();
              queue.clear();
            }
            try {
              resume_wait->set_exception(std::make_exception_ptr(err));
            } catch (...) {
            }
            resume_wait.reset();
          } else if (start_wait) {
            try {
              start_wait->set_exception(std::make_exception_ptr(err));
            } catch (...) {
            }
            start_wait.reset();
          } else if (stop_wait) {
            try {
              stop_wait->set_exception(std::make_exception_ptr(err));
            } catch (...) {
            }
            stop_wait.reset();
          }
        }
        if (is_auth(err.code)) handle_auth_error();
        push_recv(msg);
        break;
      }
      case ::tracking::v2::ServerMsg::kRelocate:
        std::thread([this, rel = msg.relocate()]() {
          try {
            handle_relocate(rel, true);
          } catch (...) {
          }
        }).detach();
        break;
      default:
        push_recv(msg);
        break;
    }
  }

  void flush_queue() {
    ::tracking::v2::ClientMsg msg;
    {
      std::lock_guard<std::mutex> lock(mu);
      if (track_uid.empty() || state != ConnectionState::kOpen || !ws) return;
      auto pending = queue.peek_all();
      if (pending.empty()) return;
      auto* batch = msg.mutable_location_batch();
      batch->set_track_uid(track_uid);
      batch->set_client_seq(pending.back().seq);
      for (auto& p : pending) {
        auto* pt = batch->add_points();
        *pt = p.point;
        stamp_lat_lng(pt);
      }
    }
    try {
      std::lock_guard<std::mutex> lock(mu);
      send_locked(msg);
    } catch (...) {
    }
  }

  void resubscribe() {
    std::vector<std::string> subs;
    {
      std::lock_guard<std::mutex> lock(mu);
      subs.assign(subscriptions.begin(), subscriptions.end());
    }
    for (const auto& d : subs) {
      ::tracking::v2::ClientMsg msg;
      msg.mutable_subscribe()->set_device_uid(d);
      try {
        std::lock_guard<std::mutex> lock(mu);
        send_locked(msg);
      } catch (...) {
      }
    }
  }

  void send_resume_and_wait() {
    std::future<std::uint64_t> fut;
    {
      std::lock_guard<std::mutex> lock(mu);
      if (track_uid.empty()) return;
      resume_wait.emplace();
      fut = resume_wait->get_future();
      ::tracking::v2::ClientMsg msg;
      msg.mutable_resume()->set_track_uid(track_uid);
      msg.mutable_resume()->set_last_client_seq(client_seq);
      send_locked(msg);
    }
    fut.get();
  }

  void handle_relocate(const ::tracking::v2::Relocate& rel, bool send_resume) {
    if (!rel.endpoint().empty()) {
      std::lock_guard<std::mutex> lock(mu);
      cfg.endpoint = rel.endpoint();
    }
    if (rel.retry_after_ms() > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(rel.retry_after_ms()));
    }
    bool intentional_local = false;
    {
      std::lock_guard<std::mutex> lock(mu);
      if (!track_uid.empty()) send_resume = true;
      intentional_local = intentional;
    }
    if (intentional_local) throw Error(::tracking::v2::ERROR_CODE_INVALID, "closed");
    dial(send_resume);
  }

  void handle_auth_error() {
    RefreshAuthFn refresh;
    {
      std::lock_guard<std::mutex> lock(mu);
      refresh = cfg.refresh_auth;
    }
    if (!refresh) {
      std::unique_ptr<ix::WebSocket> sock;
      {
        std::lock_guard<std::mutex> lock(mu);
        intentional = true;
        state = ConnectionState::kClosed;
        sock = std::move(ws);
      }
      stop_socket(std::move(sock));
      return;
    }
    try {
      auto [device, listener] = refresh();
      std::unique_ptr<ix::WebSocket> sock;
      bool send_resume = false;
      bool redial = false;
      {
        std::lock_guard<std::mutex> lock(mu);
        if (device) {
          cfg.device = device;
          cfg.listener.reset();
        }
        if (listener) {
          cfg.listener = listener;
          cfg.device.reset();
        }
        send_resume = !track_uid.empty();
        ++dial_gen;
        sock = std::move(ws);
        redial = !intentional;
      }
      stop_socket(std::move(sock));
      if (redial) {
        std::thread([this, send_resume]() {
          try {
            dial(send_resume);
          } catch (...) {
          }
        }).detach();
      }
    } catch (...) {
      std::lock_guard<std::mutex> lock(mu);
      intentional = true;
      state = ConnectionState::kClosed;
    }
  }

  void schedule_reconnect(bool send_resume) {
    if (reconnect_thread.joinable()) {
      reconnect_stop = true;
      reconnect_thread.join();
      reconnect_stop = false;
    }
    reconnect_thread = std::thread([this, send_resume]() {
      static thread_local std::mt19937 rng{std::random_device{}()};
      std::uniform_real_distribution<double> dist(0.0, 1.0);
      for (;;) {
        std::optional<std::chrono::milliseconds> delay;
        {
          std::lock_guard<std::mutex> lock(mu);
          if (intentional || reconnect_stop) return;
          state = ConnectionState::kReconnecting;
          delay = next_delay(backoff, dist(rng));
          if (!delay) {
            state = ConnectionState::kClosed;
            reject_pending(Error(::tracking::v2::ERROR_CODE_INVALID, "reconnect attempts exhausted"));
            return;
          }
        }
        std::this_thread::sleep_for(*delay);
        {
          std::lock_guard<std::mutex> lock(mu);
          if (intentional || reconnect_stop) return;
        }
        try {
          dial(send_resume);
          return;
        } catch (...) {
          std::lock_guard<std::mutex> lock(mu);
          if (intentional || state == ConnectionState::kOpen) return;
        }
      }
    });
  }

  void on_socket_closed(std::uint64_t gen) {
    // Called from the WS worker thread — do not destroy/stop the socket here
    // (would join this thread). Detach ownership and stop asynchronously.
    bool should_reconnect = false;
    bool send_resume = false;
    std::unique_ptr<ix::WebSocket> sock;
    {
      std::lock_guard<std::mutex> lock(mu);
      if (gen != dial_gen) return;
      sock = std::move(ws);
      if (intentional) {
        state = ConnectionState::kClosed;
        stop_socket(std::move(sock));
        return;
      }
      if (cfg.disable_reconnect) {
        state = ConnectionState::kClosed;
        reject_pending(Error(::tracking::v2::ERROR_CODE_INVALID, "connection closed"));
        stop_socket(std::move(sock));
        return;
      }
      send_resume = !track_uid.empty();
      should_reconnect = true;
    }
    stop_socket(std::move(sock));
    if (should_reconnect) schedule_reconnect(send_resume);
  }

  void dial(bool send_resume) {
    std::string url;
    std::uint64_t gen = 0;
    std::chrono::milliseconds hello_to{10'000};
    std::unique_ptr<ix::WebSocket> old_sock;
    {
      std::lock_guard<std::mutex> lock(mu);
      ++dial_gen;
      gen = dial_gen;
      if (state == ConnectionState::kOpen || state == ConnectionState::kReconnecting)
        state = ConnectionState::kReconnecting;
      else
        state = ConnectionState::kConnecting;
      url = build_ws_url(cfg);
      hello_to = cfg.hello_timeout;
      old_sock = std::move(ws);
    }
    stop_socket(std::move(old_sock));

    auto socket = std::make_unique<ix::WebSocket>();
    socket->setUrl(url);
    socket->disableAutomaticReconnection();

    // Handshake state must outlive dial()'s stack — the WS callback stays registered.
    struct HelloGate {
      std::mutex mu;
      std::promise<::tracking::v2::ServerMsg> promise;
      bool done = false;
    };
    auto gate = std::make_shared<HelloGate>();
    auto hello_fut = gate->promise.get_future();

    socket->setOnMessageCallback([this, gen, gate](const ix::WebSocketMessagePtr& msg) {
      if (msg->type == ix::WebSocketMessageType::Message) {
        try {
          auto sm = decode_server_msg(msg->str);
          {
            std::lock_guard<std::mutex> gl(gate->mu);
            if (!gate->done) {
              gate->done = true;
              gate->promise.set_value(std::move(sm));
              return;
            }
          }
          dispatch(sm);
        } catch (...) {
        }
      } else if (msg->type == ix::WebSocketMessageType::Close ||
                 msg->type == ix::WebSocketMessageType::Error) {
        std::string detail = "ws closed";
        if (msg->type == ix::WebSocketMessageType::Error) {
          detail = "ws error: " + msg->errorInfo.reason;
        } else {
          detail = "ws closed: " + msg->closeInfo.reason;
        }
        {
          std::lock_guard<std::mutex> gl(gate->mu);
          if (!gate->done) {
            gate->done = true;
            try {
              gate->promise.set_exception(
                  std::make_exception_ptr(Error(::tracking::v2::ERROR_CODE_INVALID, detail)));
            } catch (...) {
            }
          }
        }
        on_socket_closed(gen);
      }
    });

    // Prefer binary frames; subprotocol is requested when supported by the peer.
    socket->addSubProtocol(kSubprotocol);
    socket->disablePerMessageDeflate();
    socket->disableAutomaticReconnection();
    socket->start();

    if (hello_fut.wait_for(hello_to) != std::future_status::ready) {
      socket->stop();
      throw Error(::tracking::v2::ERROR_CODE_INVALID, "hello timeout");
    }
    auto first = hello_fut.get();
    if (first.body_case() == ::tracking::v2::ServerMsg::kRelocate) {
      socket->stop();
      handle_relocate(first.relocate(), send_resume);
      return;
    }
    if (first.body_case() == ::tracking::v2::ServerMsg::kError) {
      socket->stop();
      throw error_from_wire(first.error());
    }
    if (first.body_case() != ::tracking::v2::ServerMsg::kHello) {
      socket->stop();
      throw Error(::tracking::v2::ERROR_CODE_INVALID, "expected hello");
    }

    {
      std::lock_guard<std::mutex> lock(mu);
      if (gen != dial_gen || intentional) {
        socket->stop();
        throw Error(::tracking::v2::ERROR_CODE_INVALID, "dial superseded");
      }
      ws = std::move(socket);
      state = ConnectionState::kOpen;
      reset_backoff(backoff);
    }

    if (send_resume) send_resume_and_wait();
    resubscribe();
  }
};

Client::Client(Config cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}
Client::~Client() = default;

std::unique_ptr<Client> Client::connect(Config cfg) {
  if (cfg.endpoint.empty()) throw Error(::tracking::v2::ERROR_CODE_INVALID, "Endpoint is required");
  if (!cfg.device && !cfg.listener)
    throw Error(::tracking::v2::ERROR_CODE_INVALID, "Device or Listener auth is required");
  ix::initNetSystem();
  auto client = std::unique_ptr<Client>(new Client(std::move(cfg)));
  client->impl_->dial(false);
  return client;
}

ConnectionState Client::state() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->state;
}
std::string Client::track_uid() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->track_uid;
}
std::uint64_t Client::client_seq() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->client_seq;
}
std::uint64_t Client::last_acked_seq() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->last_acked_seq;
}

void Client::send(const ::tracking::v2::ClientMsg& msg) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->send_locked(msg);
}

std::string Client::start_track(const ::tracking::v2::LatLng* loc,
                                const std::vector<::tracking::v2::LatLng>& route,
                                const std::string& metadata) {
  std::future<std::string> fut;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->start_wait.emplace();
    fut = impl_->start_wait->get_future();
    ::tracking::v2::ClientMsg msg;
    auto* start = msg.mutable_track_start();
    if (loc) {
      *start->mutable_location() = *loc;
      stamp_lat_lng(start->mutable_location());
    }
    for (auto p : route) {
      stamp_lat_lng(&p);
      *start->add_route() = p;
    }
    if (!metadata.empty()) start->set_metadata(metadata);
    impl_->send_locked(msg);
  }
  return fut.get();
}

std::uint64_t Client::resume(const std::string& track_uid, std::uint64_t last_client_seq) {
  std::future<std::uint64_t> fut;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->track_uid = track_uid;
    impl_->client_seq = last_client_seq;
    impl_->resume_wait.emplace();
    fut = impl_->resume_wait->get_future();
    ::tracking::v2::ClientMsg msg;
    msg.mutable_resume()->set_track_uid(track_uid);
    msg.mutable_resume()->set_last_client_seq(last_client_seq);
    impl_->send_locked(msg);
  }
  return fut.get();
}

std::pair<std::uint64_t, bool> Client::publish(const ::tracking::v2::LatLng& point) {
  ::tracking::v2::ClientMsg msg;
  std::uint64_t seq = 0;
  bool open = false;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->track_uid.empty()) return {0, false};
    auto now = std::chrono::steady_clock::now();
    if (!can_accept_publish(impl_->next_publish_at, now, 1)) return {impl_->client_seq, false};
    impl_->next_publish_at = next_publish_allowed_at(impl_->next_publish_at, now, 1);
    ++impl_->client_seq;
    seq = impl_->client_seq;
    auto pt = point;
    stamp_lat_lng(&pt);
    impl_->queue.enqueue(seq, pt);
    open = impl_->state == ConnectionState::kOpen && impl_->ws;
    auto* add = msg.mutable_location_add();
    add->set_track_uid(impl_->track_uid);
    add->set_client_seq(seq);
    *add->mutable_point() = pt;
  }
  if (open) {
    try {
      std::lock_guard<std::mutex> lock(impl_->mu);
      impl_->send_locked(msg);
    } catch (...) {
    }
  }
  return {seq, true};
}

void Client::stop_track(const std::string& track_uid) {
  std::string uid = track_uid;
  if (uid.empty()) uid = this->track_uid();
  if (uid.empty()) throw Error(::tracking::v2::ERROR_CODE_INVALID, "no active track");
  std::future<void> fut;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->stop_wait.emplace();
    fut = impl_->stop_wait->get_future();
    ::tracking::v2::ClientMsg msg;
    msg.mutable_track_stop()->set_track_uid(uid);
    impl_->send_locked(msg);
  }
  fut.get();
}

bool Client::send_event(const std::string& payload) {
  if (static_cast<int>(payload.size()) > kMaxEventBytes)
    throw Error(::tracking::v2::ERROR_CODE_INVALID, "event payload exceeds 4 KiB");
  ::tracking::v2::ClientMsg msg;
  bool open = false;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->track_uid.empty())
      throw Error(::tracking::v2::ERROR_CODE_INVALID, "startTrack() before sendEvent()");
    auto now = std::chrono::steady_clock::now();
    if (impl_->next_event_at.time_since_epoch().count() != 0 && now < impl_->next_event_at)
      return false;
    impl_->next_event_at = now + kMinEventInterval;
    open = impl_->state == ConnectionState::kOpen && impl_->ws;
    auto* ev = msg.mutable_event();
    ev->set_track_uid(impl_->track_uid);
    ev->set_payload(payload);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    ev->set_timestamp_ms(ms);
  }
  if (!open) return true;
  send(msg);
  return true;
}

void Client::subscribe(const std::string& device_uid) {
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->subscriptions.insert(device_uid);
  }
  ::tracking::v2::ClientMsg msg;
  msg.mutable_subscribe()->set_device_uid(device_uid);
  send(msg);
}

bool Client::recv(::tracking::v2::ServerMsg& out, std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(impl_->mu);
  auto pred = [&] { return !impl_->recv_q.empty(); };
  if (timeout.count() < 0) {
    impl_->recv_cv.wait(lock, pred);
  } else if (!impl_->recv_cv.wait_for(lock, timeout, pred)) {
    return false;
  }
  out = std::move(impl_->recv_q.front());
  impl_->recv_q.pop();
  return true;
}

void Client::close() { impl_->shutdown(); }

}  // namespace pickpoint::tracking
