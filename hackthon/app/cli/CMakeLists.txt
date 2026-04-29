cmake_minimum_required(VERSION 3.16.3)
project(hackthon)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(GTest REQUIRED)
find_package(Protobuf REQUIRED)
pkg_check_modules(LIBEVENT REQUIRED libevent)
pkg_check_modules(LIBEVENT_OPENSSL REQUIRED libevent_openssl)
pkg_check_modules(NGHTTP2 REQUIRED libnghttp2)

find_package(Lua REQUIRED)
include_directories(${LUA_INCLUDE_DIR})

find_library(READLINE_LIBRARY NAMES readline)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20 -g -Wall -Wextra")

include_directories(inc)
include_directories(../inc)
include_directories(../openvpn/inc)
include_directories(${NGHTTP2_INCLUDE_DIRS})
# Generated gNMI proto headers (gnmi.pb.h etc.) live here at build time.
include_directories(${CMAKE_CURRENT_BINARY_DIR}/../../app/proto_gen)

file(GLOB SOURCES "src/*.cpp")

# Shared production sources needed by the CLI binary.
# gnmi_client + http2 + grpc_session provide the gRPC-over-nghttp2 stack used
# when the user types gnmi_get / gnmi_update / gnmi_replace / gnmi_delete.
set(SHARED_SOURCES
  "../src/framework.cpp"
  "../src/fs_app.cpp"
  "../src/mqtt_io.cpp"
  "../src/gnmi_client.cpp"
  "../src/grpc_session.cpp"
  "../src/http2.cpp"
  "../src/lua_engine.cpp"
  "../openvpn/src/vpn_tunnel_client.cpp"
)

add_executable(cli_app
  ${SOURCES}
  ${SHARED_SOURCES})

target_link_libraries(cli_app PRIVATE
  gnmi_proto
  mosquitto
  pthread
  ssl
  crypto
  readline
  ${LIBEVENT_LIBRARIES}
  ${LIBEVENT_OPENSSL_LIBRARIES}
  ${NGHTTP2_LIBRARIES}
  ${LUA_LIBRARIES})

# Completion-helper tests. Deliberately isolated from readline / protobuf.
if((BUILD_TESTING OR PROJECT_IS_TOP_LEVEL) AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test")
  add_subdirectory(test)
endif()
