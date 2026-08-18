cmake_minimum_required(VERSION 3.16)
project(pickpoint VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(PICKPOINT_BUILD_TESTS "Build unit tests" ON)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

include(FetchContent)

FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE
)
FetchContent_Declare(
  ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG v11.4.5
  GIT_SHALLOW TRUE
)
set(USE_TLS ON CACHE BOOL "" FORCE)
set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(json ixwebsocket)

add_library(pickpoint
  src/client.cpp
  src/mint.cpp
  src/transport.cpp
  src/tracking/backoff.cpp
  src/tracking/client.cpp
  src/tracking/codec.cpp
  src/tracking/filter.cpp
  src/tracking/queue.cpp
  src/tracking/rate.cpp
  src/tracking/url.cpp
)
add_library(pickpoint::pickpoint ALIAS pickpoint)

target_include_directories(pickpoint
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(pickpoint
  PUBLIC
    nlohmann_json::nlohmann_json
  PRIVATE
    CURL::libcurl
    ixwebsocket
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

include(GNUInstallDirs)
install(TARGETS pickpoint
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/pickpoint DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(PICKPOINT_BUILD_TESTS)
  enable_testing()
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
  )
  FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.3
    GIT_SHALLOW TRUE
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest httplib)

  add_executable(pickpoint_tests
    tests/test_http_helpers.cpp
    tests/test_http_client.cpp
    tests/test_tracking_helpers.cpp
    tests/test_tracking_client.cpp
    tests/test_e2e_geocode_batch.cpp
  )
  target_include_directories(pickpoint_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${httplib_SOURCE_DIR}
  )
  target_link_libraries(pickpoint_tests PRIVATE pickpoint GTest::gtest_main ixwebsocket)
  include(GoogleTest)
  gtest_discover_tests(pickpoint_tests DISCOVERY_TIMEOUT 30)
endif()
