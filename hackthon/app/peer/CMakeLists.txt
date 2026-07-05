cmake_minimum_required(VERSION 3.16.3)
project(hackthon)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(Curses REQUIRED)
pkg_check_modules(LIBEVENT REQUIRED libevent)
pkg_check_modules(LIBEVENT_OPENSSL REQUIRED libevent_openssl)
pkg_check_modules(NGHTTP2 REQUIRED libnghttp2)

find_package(Lua REQUIRED)
include_directories(${LUA_INCLUDE_DIR})

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20 -g -Wall -Wextra")

include_directories(inc)
include_directories(../inc)
include_directories(../openvpn/inc)   # tls_config.hpp
include_directories(${NGHTTP2_INCLUDE_DIRS})
include_directories(${CURSES_INCLUDE_DIRS})
# Generated gNMI proto headers (gnmi.pb.h etc.) live here at build time.
include_directories(${CMAKE_CURRENT_BINARY_DIR}/../../app/proto_gen)

file(GLOB SOURCES "src/*.cpp")

# Shared production sources needed to run a local gNMI server and make outbound
# gRPC calls. Mirrors app/cli but adds server_app/client_app for the local
# server side (the remote peer pushes Set operations to us).
set(SHARED_SOURCES
  "../src/framework.cpp"
  "../src/server_app.cpp"
  "../src/client_app.cpp"
  "../src/gnmi_client.cpp"
  "../src/grpc_session.cpp"
  "../src/http2.cpp"
  "../src/lua_engine.cpp"
)

add_executable(gnmi_peer
  ${SOURCES}
  ${SHARED_SOURCES})

target_link_libraries(gnmi_peer PRIVATE
  gnmi_proto
  pthread
  ssl
  crypto
  ${CURSES_LIBRARIES}
  ${LIBEVENT_LIBRARIES}
  ${LIBEVENT_OPENSSL_LIBRARIES}
  ${NGHTTP2_LIBRARIES}
  ${LUA_LIBRARIES})
