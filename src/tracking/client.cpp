#include "pickpoint/tracking.hpp"

#include <atomic>
#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <queue>
#include <random>
#include <thread>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

namespace pickpoint::tracking {
namespace {

Error error_from_wire(const WireError& err) {
  return Error(err.code, err.message, err.retry_after_ms, err.track_uid);
}

std::string bytes_to_string(const Bytes& b) { return std::string(b.begin(), b.end()); }

struct SubOpts {
  bool include_events = true;
  std::uint16_t min_interval = 0;
  std::uint8_t handle = 0;
};

}  // namespace

struct Client::Impl {
  Config cfg;
  mutable std::mutex mu;
  ConnectionState state = ConnectionState::kConnecting;
  std::string track_uid;
  std::uint64_t client_seq = 0;
  std::uint64_t last_acked_seq = 0;
  Buffer buf;
  NoiseFilter filter;
  int unacked_frames = 0;
  BackoffState backoff;
  std::chrono::steady_clock::time_point next_publish_at{};
  std::chrono::steady_clock::time_point next_event_at{};
  std::map<std::string, SubOpts> subscriptions;
  std::map<std::uint8_t, std::string> sub_by_handle;
  bool intentional = false;
  std::uint64_t dial_gen = 0;

  std::unique_ptr<ix::WebSocket> ws;
  std::queue<ServerMsg> recv_q;
  std::condition_variable recv_cv;

  std::optional<std::promise<std::string>> start_wait;
  std::optional<std::promise<void>> stop_wait;
  std::optional<std::promise<std::uint64_t>> resume_wait;
  bool starting = false;

  std::thread reconnect_thread;
  bool reconnect_stop = false;
  std::mutex stop_mu;
  std::vector<std::thread> stop_threads;

  Impl(Config c)
      : cfg(std::move(c)),
        buf(cfg.max_queue_size),
        backoff(new_backoff(cfg.reconnect_min_delay, cfg.reconnect_max_delay,
                            cfg.reconnect_max_attempts)) {
    if (cfg.hello_timeout.count() <= 0) cfg.hello_timeout = std::chrono::milliseconds(10'000);
    for (const auto& uid : cfg.subscribe) {
      if (!uid.empty()) subscriptions[uid] = SubOpts{};
    }
  }

  ~Impl() { shutdown(); }

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
    {
      std::lock_guard<std::mutex> lock(mu);
      if (!track_uid.empty() && ws) {
        ClientMsg msg;
        msg.track_stop = TrackStop{};
        try {
          send_locked(msg);
        } catch (...) {
        }
      }
    }
    std::unique_ptr<ix::WebSocket> sock;
    {
      std::lock_guard<std::mutex> lock(mu);
      intentional = true;
      reconnect_stop = true;
      state = ConnectionState::kClosed;
      reject_pending(Error(ErrorCode::kInvalid, "client closed"));
      sock = std::move(ws);
    }
    stop_socket(std::move(sock));
    if (reconnect_thread.joinable()) reconnect_thread.join();
    join_stops();
  }

  void reject_pending(const Error& err) {
    starting = false;
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

  void push_recv(ServerMsg msg) {
    std::lock_guard<std::mutex> lock(mu);
    if (recv_q.size() < 64) {
      recv_q.push(std::move(msg));
      recv_cv.notify_one();
    }
  }

  void send_raw_locked(const Bytes& bin) {
    if (intentional && state == ConnectionState::kClosed) {
      throw Error(ErrorCode::kInvalid, "closed");
    }
    if (!ws) throw Error(ErrorCode::kInvalid, "socket not open");
    ws->sendBinary(bytes_to_string(bin));
  }

  void send_locked(const ClientMsg& msg) {
    if (msg.loc && msg.loc->points.size() > 1) {
      for (const auto& f : encode_loc_frames(msg.loc->seq, msg.loc->points)) {
        send_raw_locked(f);
      }
      return;
    }
    send_raw_locked(encode_client_msg(msg));
  }

  void send_assigned(const std::vector<InFlightPoint>& pts) {
    ix::WebSocket* sock = nullptr;
    {
      std::lock_guard<std::mutex> lock(mu);
      if (state != ConnectionState::kOpen || !ws || pts.empty()) return;
      sock = ws.get();
    }
    auto frames = encode_in_flight_frames(pts);
    {
      std::lock_guard<std::mutex> lock(mu);
      unacked_frames += static_cast<int>(frames.size());
    }
    for (const auto& f : frames) {
      try {
        std::lock_guard<std::mutex> lock(mu);
        if (!ws || ws.get() != sock) return;
        send_raw_locked(f);
      } catch (...) {
      }
    }
  }

  void resend_in_flight() {
    std::vector<InFlightPoint> pts;
    {
      std::lock_guard<std::mutex> lock(mu);
      if (state != ConnectionState::kOpen || !ws || track_uid.empty()) return;
      pts = buf.peek_in_flight();
    }
    send_assigned(pts);
  }

  void flush_staging() {
    std::vector<InFlightPoint> assigned;
    {
      std::lock_guard<std::mutex> lock(mu);
      if (state != ConnectionState::kOpen || !ws || track_uid.empty()) return;
      int window = kMaxInFlightFrames - unacked_frames;
      assigned = buf.assign_from_staging(&client_seq, window);
    }
    send_assigned(assigned);
  }

  void dispatch(const ServerMsg& msg) {
    if (msg.empty()) return;
    if (msg.resume_ok) {
      std::promise<std::uint64_t> done;
      bool have = false;
      {
        std::lock_guard<std::mutex> lock(mu);
        if (!msg.resume_ok->track_uid.empty()) track_uid = msg.resume_ok->track_uid;
        last_acked_seq = msg.resume_ok->last_acked;
        if (client_seq < last_acked_seq) client_seq = last_acked_seq;
        buf.ack_through(msg.resume_ok->last_acked);
        unacked_frames = 0;
        if (resume_wait) {
          done = std::move(*resume_wait);
          resume_wait.reset();
          have = true;
        }
      }
      resend_in_flight();
      flush_staging();
      if (have) {
        try {
          done.set_value(msg.resume_ok->last_acked);
        } catch (...) {
        }
      }
      push_recv(msg);
      return;
    }
    if (msg.track_started) {
      std::promise<std::string> done;
      bool have = false;
      {
        std::lock_guard<std::mutex> lock(mu);
        track_uid = msg.track_started->track_uid;
        client_seq = 0;
        last_acked_seq = 0;
        unacked_frames = 0;
        starting = false;
        if (start_wait) {
          done = std::move(*start_wait);
          start_wait.reset();
          have = true;
        }
      }
      if (have) {
        try {
          done.set_value(msg.track_started->track_uid);
        } catch (...) {
        }
      }
      flush_staging();
      push_recv(msg);
      return;
    }
    if (msg.track_stopped) {
      std::promise<void> done;
      bool have = false;
      {
        std::lock_guard<std::mutex> lock(mu);
        if (track_uid == msg.track_stopped->track_uid || msg.track_stopped->track_uid.empty()) {
          track_uid.clear();
          buf.clear();
          filter.reset();
        }
        if (stop_wait) {
          done = std::move(*stop_wait);
          stop_wait.reset();
          have = true;
        }
      }
      if (have) {
        try {
          done.set_value();
        } catch (...) {
        }
      }
      push_recv(msg);
      return;
    }
    if (msg.ack) {
      {
        std::lock_guard<std::mutex> lock(mu);
        if (msg.ack->seq > last_acked_seq) last_acked_seq = msg.ack->seq;
        buf.ack_through(msg.ack->seq);
        unacked_frames = 0;
      }
      flush_staging();
      return;
    }
    if (msg.error) {
      Error err = error_from_wire(*msg.error);
      {
        std::lock_guard<std::mutex> lock(mu);
        if (resume_wait) {
          if (is_fatal_resume_error(err.code)) {
            track_uid.clear();
            buf.clear();
            filter.reset();
            client_seq = 0;
            last_acked_seq = 0;
          }
          try {
            resume_wait->set_exception(std::make_exception_ptr(err));
          } catch (...) {
          }
          resume_wait.reset();
        } else if (err.code == ErrorCode::kTrackNotFound) {
          track_uid.clear();
          buf.clear();
          filter.reset();
        }
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
      }
      if (is_auth_error(err.code)) handle_auth_error();
      push_recv(msg);
      return;
    }
    if (msg.relocate) {
      std::thread([this, rel = *msg.relocate]() {
        try {
          handle_relocate(rel, true);
        } catch (...) {
        }
      }).detach();
      return;
    }
    if (msg.subscribed) {
      {
        std::lock_guard<std::mutex> lock(mu);
        auto it = subscriptions.find(msg.subscribed->device_uid);
        if (it != subscriptions.end()) {
          it->second.handle = msg.subscribed->sub;
          sub_by_handle[msg.subscribed->sub] = msg.subscribed->device_uid;
        }
      }
      push_recv(msg);
      return;
    }
    push_recv(msg);
  }

  void resubscribe() {
    std::vector<std::pair<std::string, SubOpts>> subs;
    {
      std::lock_guard<std::mutex> lock(mu);
      subs.assign(subscriptions.begin(), subscriptions.end());
      sub_by_handle.clear();
      for (auto& kv : subscriptions) kv.second.handle = 0;
    }
    for (const auto& s : subs) {
      ClientMsg msg;
      msg.subscribe = Subscribe{s.first, s.second.include_events, s.second.min_interval};
      try {
        std::lock_guard<std::mutex> lock(mu);
        send_locked(msg);
      } catch (...) {
      }
    }
  }

  void send_resume_and_wait() {
    for (;;) {
      std::future<std::uint64_t> fut;
      {
        std::lock_guard<std::mutex> lock(mu);
        if (track_uid.empty()) return;
        resume_wait.emplace();
        fut = resume_wait->get_future();
        ClientMsg msg;
        msg.resume = Resume{track_uid, static_cast<std::uint32_t>(client_seq)};
        send_locked(msg);
      }
      try {
        fut.get();
        return;
      } catch (const Error& err) {
        if (!is_retry_resume_error(err.code)) throw;
        auto delay = std::chrono::milliseconds(err.retry_after_ms);
        if (delay.count() <= 0) {
          static thread_local std::mt19937 rng{std::random_device{}()};
          std::uniform_real_distribution<double> dist(0.0, 1.0);
          std::lock_guard<std::mutex> lock(mu);
          auto next = next_delay(backoff, dist(rng));
          if (!next) throw;
          delay = *next;
        }
        std::this_thread::sleep_for(delay);
      }
    }
  }

  void handle_relocate(const Relocate& rel, bool send_resume) {
    if (!rel.endpoint.empty()) {
      std::lock_guard<std::mutex> lock(mu);
      cfg.endpoint = rel.endpoint;
    }
    if (rel.retry_after_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(rel.retry_after_ms));
    }
    bool intentional_local = false;
    {
      std::lock_guard<std::mutex> lock(mu);
      if (!track_uid.empty()) send_resume = true;
      intentional_local = intentional;
    }
    if (intentional_local) throw Error(ErrorCode::kInvalid, "closed");
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
            reject_pending(Error(ErrorCode::kInvalid, "reconnect attempts exhausted"));
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
        reject_pending(Error(ErrorCode::kInvalid, "connection closed"));
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
      unacked_frames = 0;
    }
    stop_socket(std::move(old_sock));

    auto socket = std::make_unique<ix::WebSocket>();
    socket->setUrl(url);
    socket->disableAutomaticReconnection();

    struct HelloGate {
      std::mutex mu;
      std::promise<ServerMsg> promise;
      bool done = false;
    };
    auto gate = std::make_shared<HelloGate>();
    auto hello_fut = gate->promise.get_future();

    socket->setOnMessageCallback([this, gen, gate](const ix::WebSocketMessagePtr& msg) {
      if (msg->type == ix::WebSocketMessageType::Message) {
        if (!msg->binary) {
          std::lock_guard<std::mutex> gl(gate->mu);
          if (!gate->done) {
            gate->done = true;
            try {
              gate->promise.set_exception(
                  std::make_exception_ptr(Error(ErrorCode::kInvalid, "text frames not allowed")));
            } catch (...) {
            }
          }
          return;
        }
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
          if (!sm.empty()) dispatch(sm);
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
                  std::make_exception_ptr(Error(ErrorCode::kInvalid, detail)));
            } catch (...) {
            }
          }
        }
        on_socket_closed(gen);
      }
    });

    socket->addSubProtocol(kSubprotocol);
    socket->disablePerMessageDeflate();
    socket->disableAutomaticReconnection();
    socket->start();

    if (hello_fut.wait_for(hello_to) != std::future_status::ready) {
      socket->stop();
      throw Error(ErrorCode::kInvalid, "hello timeout");
    }
    auto first = hello_fut.get();
    if (first.relocate) {
      socket->stop();
      handle_relocate(*first.relocate, send_resume);
      return;
    }
    if (first.error) {
      socket->stop();
      throw error_from_wire(*first.error);
    }
    if (!first.hello) {
      socket->stop();
      throw Error(ErrorCode::kInvalid, "expected hello");
    }
    if (first.hello->version != kProtocolVersion) {
      socket->stop();
      throw Error(ErrorCode::kInvalid, "unsupported protocol version");
    }

    {
      std::lock_guard<std::mutex> lock(mu);
      if (gen != dial_gen || intentional) {
        socket->stop();
        throw Error(ErrorCode::kInvalid, "dial superseded");
      }
      ws = std::move(socket);
      state = ConnectionState::kOpen;
      reset_backoff(backoff);
    }

    if (send_resume) {
      try {
        send_resume_and_wait();
      } catch (const Error& err) {
        if (!(is_fatal_resume_error(err.code) && !is_auth_error(err.code))) throw;
      }
    }
    resubscribe();
  }
};

Client::Client(Config cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}
Client::~Client() = default;

std::unique_ptr<Client> Client::connect(Config cfg) {
  if (cfg.endpoint.empty()) throw Error(ErrorCode::kInvalid, "Endpoint is required");
  if (!cfg.device && !cfg.listener)
    throw Error(ErrorCode::kInvalid, "Device or Listener auth is required");
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

void Client::send(const ClientMsg& msg) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->send_locked(msg);
}

std::string Client::start_track(const LatLng* loc, const std::vector<LatLng>& route,
                                const std::string& metadata) {
  std::future<std::string> fut;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->buf.clear();
    impl_->filter.reset();
    impl_->client_seq = 0;
    impl_->last_acked_seq = 0;
    impl_->start_wait.emplace();
    impl_->starting = true;
    fut = impl_->start_wait->get_future();
    ClientMsg msg;
    TrackStart start;
    if (loc) start.location = *loc;
    start.route = route;
    start.metadata.assign(metadata.begin(), metadata.end());
    msg.track_start = std::move(start);
    impl_->send_locked(msg);
  }
  return fut.get();
}

std::uint64_t Client::resume(const std::string& track_uid, std::uint64_t last_client_seq) {
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->track_uid = track_uid;
    impl_->client_seq = last_client_seq;
  }
  impl_->send_resume_and_wait();
  return last_acked_seq();
}

std::pair<std::uint64_t, bool> Client::publish(const LatLng& point) {
  bool do_start = false;
  LatLng start_pt;
  InFlightPoint item;
  bool send_now = false;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->track_uid.empty() && !impl_->starting) {
      impl_->starting = true;
      impl_->buf.clear();
      impl_->filter.reset();
      impl_->client_seq = 0;
      impl_->last_acked_seq = 0;
      start_pt = point;
      do_start = true;
    } else {
      auto now_sys = std::chrono::system_clock::now();
      auto now_steady = std::chrono::steady_clock::now();
      auto [emitted, ok] = impl_->filter.push(point, now_sys);
      if (!ok) return {impl_->client_seq, false};
      bool open = impl_->state == ConnectionState::kOpen && impl_->ws && !impl_->track_uid.empty();
      bool window_ok = impl_->unacked_frames < kMaxInFlightFrames;
      if (!open || !window_ok) {
        stamp_lat_lng(&emitted);
        impl_->buf.push_staging(emitted);
        return {impl_->client_seq, true};
      }
      if (!can_accept_publish(impl_->next_publish_at, now_steady, 1)) {
        return {impl_->client_seq, false};
      }
      impl_->next_publish_at = next_publish_allowed_at(impl_->next_publish_at, now_steady, 1);
      ++impl_->client_seq;
      item = InFlightPoint{static_cast<std::uint32_t>(impl_->client_seq), emitted};
      impl_->buf.push_in_flight(item);
      send_now = true;
    }
  }
  if (do_start) {
    ClientMsg msg;
    TrackStart start;
    start.location = start_pt;
    msg.track_start = std::move(start);
    try {
      send(msg);
    } catch (...) {
      std::lock_guard<std::mutex> lock(impl_->mu);
      impl_->starting = false;
      return {0, false};
    }
    return {0, true};
  }
  if (send_now) impl_->send_assigned({item});
  return {item.seq, true};
}

void Client::stop_track(const std::string& track_uid) {
  std::string uid = track_uid;
  if (uid.empty()) uid = this->track_uid();
  if (uid.empty()) return;
  std::future<void> fut;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->stop_wait.emplace();
    fut = impl_->stop_wait->get_future();
    ClientMsg msg;
    msg.track_stop = TrackStop{};
    impl_->send_locked(msg);
  }
  fut.get();
}

bool Client::send_event(const std::string& payload) {
  if (static_cast<int>(payload.size()) > kMaxEventBytes)
    throw Error(ErrorCode::kInvalid, "event payload exceeds 4 KiB");
  ClientMsg msg;
  bool open = false;
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->track_uid.empty())
      throw Error(ErrorCode::kInvalid, "startTrack() before sendEvent()");
    auto now = std::chrono::steady_clock::now();
    if (impl_->next_event_at.time_since_epoch().count() != 0 && now < impl_->next_event_at)
      return false;
    impl_->next_event_at = now + kMinEventInterval;
    open = impl_->state == ConnectionState::kOpen && impl_->ws;
    Event ev;
    ev.payload.assign(payload.begin(), payload.end());
    ev.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    msg.event = std::move(ev);
  }
  if (!open) return true;
  send(msg);
  return true;
}

void Client::subscribe(const std::string& device_uid) { subscribe(device_uid, true, 0); }

void Client::subscribe(const std::string& device_uid, bool include_events,
                       std::uint16_t min_interval_ms) {
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto it = impl_->subscriptions.find(device_uid);
    if (it != impl_->subscriptions.end()) {
      it->second.include_events = include_events;
      it->second.min_interval = min_interval_ms;
    } else {
      impl_->subscriptions[device_uid] = SubOpts{include_events, min_interval_ms, 0};
    }
  }
  ClientMsg msg;
  msg.subscribe = Subscribe{device_uid, include_events, min_interval_ms};
  send(msg);
}

void Client::unsubscribe(std::uint8_t sub) {
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto it = impl_->sub_by_handle.find(sub);
    if (it != impl_->sub_by_handle.end()) {
      impl_->subscriptions.erase(it->second);
      impl_->sub_by_handle.erase(it);
    }
  }
  ClientMsg msg;
  msg.unsubscribe = Unsubscribe{sub};
  send(msg);
}

bool Client::recv(ServerMsg& out, std::chrono::milliseconds timeout) {
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
