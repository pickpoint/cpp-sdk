# Pickpoint C++ SDK

C++17 client for [Pickpoint](https://pickpoint.io) — HTTP geocoding, routing, devices, and live GPS.

HTTP (`pickpoint::Client`) and live GPS (`pickpoint::tracking::Client`) are **two clients**. Live GPS is a WebSocket session at `wss://tracking.pickpoint.io/v2/ws`, subprotocol `tracking.v2`.

A dropped socket is not a new trip. The SDK reconnects and **Resumes** the same `track_uid`.

First `publish` starts the trip if none is live. `close` sends `TrackStop` then hangs up. Call `start_track` only to supersede (new order / `TRACK_NOT_FOUND`) or to set a route.

## Tracking

### Device (publisher)

```cpp
#include <pickpoint/tracking.hpp>

pickpoint::tracking::Config cfg;
cfg.endpoint = "wss://tracking.pickpoint.io";  // host; SDK appends /v2/ws
cfg.device = pickpoint::tracking::DeviceAuth{deviceUid, deviceSecret};

auto session = pickpoint::tracking::Client::connect(std::move(cfg));
session->publish({55.75, 37.61});  // TrackStart if idle
session->close();                  // TrackStop + hang up
```

### Listener (dashboard)

`access_token` is the **client-token** from `POST /v2/client-tokens` (scope `devices`) — mint on your backend, same token as HTTP `client_auth`. C++ has no mint helper; call that HTTP endpoint (or mint from another SDK) and pass the string here.

```cpp
#include <iostream>
#include <pickpoint/tracking.hpp>

pickpoint::tracking::Config cfg;
cfg.endpoint = "wss://tracking.pickpoint.io";
cfg.listener = pickpoint::tracking::ListenerAuth{accessToken};  // pair.accessToken
cfg.subscribe = {deviceUid};

auto session = pickpoint::tracking::Client::connect(std::move(cfg));

pickpoint::tracking::ServerMsg msg;
while (session->recv(msg)) {
  if (msg.loc) {
    std::cout << msg.loc->point.latitude << " "
              << msg.loc->point.longitude << "\n";
  }
}
```

Wire format: [pickpoint-proto](https://github.com/pickpoint/pickpoint-proto).

## Dependencies

- CMake ≥ 3.16, C++17 compiler
- libcurl, OpenSSL

## Build & test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPICKPOINT_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure -E '^E2E\.'
# or:
./build/pickpoint_tests --gtest_filter='-E2E.*'
```

E2E geocode batch tests require `PICKPOINT_API_KEY`.

## Contributing

Fork and open a pull request against **`dev`** — not `main`. Only [pickpoint](https://github.com/pickpoint) organization members can merge `dev` or `main`. Releases are `dev` → `main`.
