#include "pickpoint/tracking.hpp"

#include <algorithm>
#include <cmath>

namespace pickpoint::tracking {
namespace {

constexpr double kHeartbeatSec = 1.0;
constexpr double kMinMoveM = 2.0;
constexpr double kHeadingJump = 25.0;
constexpr double kStopSpeed = 0.5;
constexpr double kEarthRadiusM = 6'371'000.0;
constexpr double kPi = 3.14159265358979323846;

double heading_delta(double a, double b) {
  double d = std::abs(b - a);
  if (d > 180.0) d = 360.0 - d;
  return d;
}

bool is_stopped(double speed) { return speed < kStopSpeed; }

}  // namespace

double haversine_m(const LatLng& a, const LatLng& b) {
  const double phi1 = a.latitude * kPi / 180.0;
  const double phi2 = b.latitude * kPi / 180.0;
  const double dphi = (b.latitude - a.latitude) * kPi / 180.0;
  const double dlmb = (b.longitude - a.longitude) * kPi / 180.0;
  const double s = std::sin(dphi / 2) * std::sin(dphi / 2) +
                   std::cos(phi1) * std::cos(phi2) * std::sin(dlmb / 2) * std::sin(dlmb / 2);
  return 2 * kEarthRadiusM * std::asin(std::min(1.0, std::sqrt(s)));
}

double perp_dist_m(const LatLng& a, const LatLng& c, const LatLng& b) {
  const double r = kEarthRadiusM;
  const double lat0 = a.latitude * kPi / 180.0;
  auto to_xy = [&](const LatLng& p) {
    double y = (p.latitude - a.latitude) * kPi / 180.0 * r;
    double x = (p.longitude - a.longitude) * kPi / 180.0 * std::cos(lat0) * r;
    return std::pair<double, double>{x, y};
  };
  auto [cx, cy] = to_xy(c);
  auto [bx, by] = to_xy(b);
  const double len2 = cx * cx + cy * cy;
  if (len2 < 1e-6) return haversine_m(a, b);
  return std::abs(bx * cy - by * cx) / std::sqrt(len2);
}

std::pair<LatLng, bool> NoiseFilter::push(const LatLng& p, std::chrono::system_clock::time_point now) {
  auto t = now;
  if (p.timestamp_ms) {
    t = std::chrono::system_clock::time_point(std::chrono::milliseconds(*p.timestamp_ms));
  }
  if (!last_emitted_) {
    emit(p, t);
    return {p, true};
  }
  if (last_emit_at_.time_since_epoch().count() != 0 &&
      std::chrono::duration<double>(t - last_emit_at_).count() >= kHeartbeatSec) {
    emit(p, t);
    return {p, true};
  }
  double acc = p.accuracy.value_or(0.0);
  if (haversine_m(*last_emitted_, p) >= std::max(kMinMoveM, 2.0 * acc)) {
    emit(p, t);
    return {p, true};
  }
  if (heading_jump(*last_emitted_, p)) {
    emit(p, t);
    return {p, true};
  }
  if (motion_edge(*last_emitted_, p)) {
    emit(p, t);
    return {p, true};
  }
  if (candidate_) {
    double speed = p.speed.value_or(0.0);
    double eps = std::max(kMinMoveM, acc);
    eps = std::max(eps, 0.5 * speed);
    if (perp_dist_m(*last_emitted_, p, *candidate_) >= eps) {
      auto emitted = *candidate_;
      emit(emitted, t);
      return {emitted, true};
    }
  }
  candidate_ = p;
  return {LatLng{}, false};
}

void NoiseFilter::reset() {
  last_emitted_.reset();
  candidate_.reset();
  last_emit_at_ = {};
}

void NoiseFilter::emit(const LatLng& p, std::chrono::system_clock::time_point t) {
  last_emitted_ = p;
  candidate_.reset();
  last_emit_at_ = t;
}

bool NoiseFilter::heading_jump(const LatLng& prev, const LatLng& cur) const {
  if (!prev.heading || !cur.heading) return false;
  return heading_delta(*prev.heading, *cur.heading) >= kHeadingJump;
}

bool NoiseFilter::motion_edge(const LatLng& prev, const LatLng& cur) const {
  if (!prev.speed || !cur.speed) return false;
  return is_stopped(*prev.speed) != is_stopped(*cur.speed);
}

}  // namespace pickpoint::tracking
