# Pickpoint C++ SDK

C++17 client for Pickpoint HTTP APIs and realtime tracking (WebSocket).

## Dependencies

- CMake ≥ 3.16, C++17 compiler
- libcurl, OpenSSL
- By default (`PICKPOINT_BUNDLED_PROTOBUF=ON`) protobuf **v31.1** is fetched and built with the project. This avoids Apple Clang ABI crashes with Homebrew protobuf 29.x (`ParseFromString` segfault — [protobuf#21447](https://github.com/protocolbuffers/protobuf/issues/21447)).

Optional: `-DPICKPOINT_BUNDLED_PROTOBUF=OFF` after installing a same-compiler protobuf (`brew reinstall --build-from-source protobuf`).

## Build & test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPICKPOINT_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
# or:
./build/pickpoint_tests --gtest_filter='-E2E.*'
```

E2E geocode batch tests require `PICKPOINT_API_KEY`.
