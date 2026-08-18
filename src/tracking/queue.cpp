#include "pickpoint/tracking.hpp"

#include <algorithm>

namespace pickpoint::tracking {
namespace {

int take_frame_points(const std::vector<LatLng>& pts, int max_frames) {
  int frames = 0;
  std::size_t i = 0;
  while (frames < max_frames && i < pts.size()) {
    auto start = i;
    auto prev_lat = deg_to_micro(pts[i].latitude);
    auto prev_lon = deg_to_micro(pts[i].longitude);
    ++i;
    while (i < pts.size() && static_cast<int>(i - start) < kMaxLocPoints) {
      auto lat = deg_to_micro(pts[i].latitude);
      auto lon = deg_to_micro(pts[i].longitude);
      if (!micro_delta_fits(prev_lat, prev_lon, lat, lon)) break;
      prev_lat = lat;
      prev_lon = lon;
      ++i;
    }
    ++frames;
  }
  return static_cast<int>(i);
}

}  // namespace

Buffer::Buffer(int max_size, std::function<void(int)> on_gap)
    : max_size_(max_size > 0 ? max_size : kMaxBufferPoints), on_gap_(std::move(on_gap)) {}

int Buffer::size() const { return static_cast<int>(staging_.size() + in_flight_.size()); }
int Buffer::staging_size() const { return static_cast<int>(staging_.size()); }
int Buffer::in_flight_size() const { return static_cast<int>(in_flight_.size()); }

void Buffer::push_staging(const LatLng& p) {
  staging_.push_back(p);
  enforce_cap();
}

void Buffer::push_in_flight(InFlightPoint p) {
  in_flight_.push_back(std::move(p));
  enforce_cap();
}

void Buffer::ack_through(std::uint32_t ack) {
  auto it = std::remove_if(in_flight_.begin(), in_flight_.end(),
                           [ack](const InFlightPoint& p) { return p.seq <= ack; });
  in_flight_.erase(it, in_flight_.end());
}

std::vector<LatLng> Buffer::peek_staging() const { return staging_; }
std::vector<InFlightPoint> Buffer::peek_in_flight() const { return in_flight_; }

std::vector<InFlightPoint> Buffer::assign_from_staging(std::uint64_t* next_seq, int max_frames) {
  if (!next_seq || max_frames <= 0 || staging_.empty()) return {};
  int n = take_frame_points(staging_, max_frames);
  std::vector<LatLng> chunk(staging_.begin(), staging_.begin() + n);
  staging_.erase(staging_.begin(), staging_.begin() + n);
  std::vector<InFlightPoint> out;
  out.reserve(static_cast<std::size_t>(n));
  for (const auto& p : chunk) {
    ++*next_seq;
    InFlightPoint item{static_cast<std::uint32_t>(*next_seq), p};
    out.push_back(item);
    in_flight_.push_back(item);
  }
  return out;
}

void Buffer::clear() {
  staging_.clear();
  in_flight_.clear();
}

void Buffer::enforce_cap() {
  while (size() > max_size_) {
    if (collapse_middle()) {
      if (on_gap_) on_gap_(1);
      continue;
    }
    if (!in_flight_.empty()) {
      in_flight_.erase(in_flight_.begin());
    } else if (!staging_.empty()) {
      staging_.erase(staging_.begin());
    } else {
      return;
    }
    if (on_gap_) on_gap_(1);
  }
}

bool Buffer::collapse_middle() {
  const int n_inf = in_flight_size();
  const int n_st = staging_size();
  const int total = n_inf + n_st;
  if (total < 3) return false;
  auto at = [&](int i) -> LatLng {
    if (i < n_inf) return in_flight_[static_cast<std::size_t>(i)].point;
    return staging_[static_cast<std::size_t>(i - n_inf)];
  };
  for (int i = 1; i < total - 1; ++i) {
    if (perp_dist_m(at(i - 1), at(i + 1), at(i)) < 2.0) {
      if (i < n_inf) {
        in_flight_.erase(in_flight_.begin() + i);
      } else {
        staging_.erase(staging_.begin() + (i - n_inf));
      }
      return true;
    }
  }
  return false;
}

}  // namespace pickpoint::tracking
