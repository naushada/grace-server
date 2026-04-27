cmake_minimum_required(VERSION 3.16.3)

find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVENT REQUIRED libevent)
pkg_check_modules(LIBEVENT_OPENSSL REQUIRED libevent_openssl)
pkg_check_modules(NGHTTP2 REQUIRED libnghttp2)

include(GoogleTest)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20 -g -Wall -Wextra")

find_package(Lua REQUIRED)

include_directories(
  ${LIBEVENT_INCLUDE_DIRS}
  ${NGHTTP2_INCLUDE_DIRS}
  ${LUA_INCLUDE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../inc       # openvpn headers + tls_config.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../inc    # framework.hpp, gnmi_client.hpp, ...
  ${CMAKE_CURRENT_BINARY_DIR}/../../proto_gen  # gnmi.pb.h etc.
)

# Production sources exercised by the tests.
# gnmi_client.cpp (and its deps grpc_session + http2) must be included because
# openvpn_server.cpp calls gnmi_client::push_async at link time.
set(OPENVPN_SRCS_UNDER_TEST
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/openvpn_client.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/openvpn_server.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/openvpn_peer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/framework.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/gnmi_client.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/grpc_session.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/http2.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/lua_engine.cpp
)

add_executable(openvpn_tests
  openvpn_test.cpp
  ${OPENVPN_SRCS_UNDER_TEST}
)

target_link_libraries(openvpn_tests PRIVATE
  gnmi_proto
  GTest::gtest
  GTest::gtest_main
  Threads::Threads
  ${LIBEVENT_LIBRARIES}
  ${LIBEVENT_OPENSSL_LIBRARIES}
  ${NGHTTP2_LIBRARIES}
  ${LUA_LIBRARIES}
  ssl
  crypto
)

gtest_discover_tests(openvpn_tests)
