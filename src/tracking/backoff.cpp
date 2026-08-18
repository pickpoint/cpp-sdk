#include "pickpoint/tracking.hpp"

#include <algorithm>
#include <cmath>

namespace pickpoint::tracking {

BackoffState new_backoff(std::chrono::milliseconds min_delay, std::chrono::milliseconds max_delay,
                         int max_attempts) {
  BackoffState s;
  s.min_delay = min_delay.count() > 0 ? min_delay : std::chrono::milliseconds(500);
  s.max_delay = max_delay.count() > 0 ? max_delay : std::chrono::milliseconds(30'000);
  s.max_attempts = max_attempts;
  return s;
}

std::optional<std::chrono::milliseconds> next_delay(BackoffState& state, double rnd) {
  if (state.max_attempts > 0 && state.attempt >= state.max_attempts) return std::nullopt;
  if (rnd < 0.0) rnd = 0.0;
  if (rnd > 1.0) rnd = 1.0;
  double min_ms = static_cast<double>(state.min_delay.count());
  double max_ms = static_cast<double>(state.max_delay.count());
  double exp = min_ms * std::pow(2.0, state.attempt);
  if (exp > max_ms) exp = max_ms;
  ++state.attempt;
  return std::chrono::milliseconds(static_cast<int>(std::floor(rnd * exp)));
}

void reset_backoff(BackoffState& state) { state.attempt = 0; }

bool can_accept_publish(std::chrono::steady_clock::time_point next_allowed_at,
                        std::chrono::steady_clock::time_point now, int point_count) {
  if (point_count <= 0) return true;
  return now >= next_allowed_at;
}

std::chrono::steady_clock::time_point next_publish_allowed_at(
    std::chrono::steady_clock::time_point next_allowed_at, std::chrono::steady_clock::time_point now,
    int point_count) {
  auto start = next_allowed_at > now ? next_allowed_at : now;
  return start + kMinPublishInterval * std::max(0, point_count);
}

}  // namespace pickpoint::tracking
