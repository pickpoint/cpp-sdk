#include "pickpoint/tracking.hpp"

#include <chrono>

namespace pickpoint::tracking {

void stamp_lat_lng(::tracking::v2::LatLng* p) {
  if (!p) return;
  if (!p->has_timestamp_ms()) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    p->set_timestamp_ms(ms);
  }
}

std::string encode_client_msg(const ::tracking::v2::ClientMsg& msg) {
  std::string out;
  if (!msg.SerializeToString(&out)) throw Error(::tracking::v2::ERROR_CODE_INVALID, "serialize failed");
  return out;
}

::tracking::v2::ServerMsg decode_server_msg(const std::string& data) {
  ::tracking::v2::ServerMsg msg;
  if (!msg.ParseFromString(data)) throw Error(::tracking::v2::ERROR_CODE_INVALID, "parse failed");
  return msg;
}

::tracking::v2::ClientMsg client_resume(const std::string& track_uid, std::uint64_t last_client_seq) {
  ::tracking::v2::ClientMsg msg;
  auto* r = msg.mutable_resume();
  r->set_track_uid(track_uid);
  r->set_last_client_seq(last_client_seq);
  return msg;
}

}  // namespace pickpoint::tracking
