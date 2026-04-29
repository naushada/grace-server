cmake_minimum_required(VERSION 3.16.3)
project(hackthon_openvpn)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
pkg_check_modules(LIBEVENT REQUIRED libevent)
pkg_check_modules(LIBEVENT_OPENSSL REQUIRED libevent_openssl)
pkg_check_modules(NGHTTP2 REQUIRED libnghttp2)

find_package(Lua REQUIRED)
include_directories(${LUA_INCLUDE_DIR})

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20 -g -Wall -Wextra")

# vpn headers (tls_config.hpp, vpn_*.hpp)
include_directories(inc)
# shared app headers (framework.hpp, server_app.hpp, gnmi_client.hpp, ...)
include_directories(../inc)
include_directories(${NGHTTP2_INCLUDE_DIRS})
# Generated gNMI proto headers — produced at build time under app/proto_gen/.
# From <build>/app/openvpn this resolves to <build>/app/proto_gen.
include_directories(${CMAKE_CURRENT_BINARY_DIR}/../proto_gen)

# Sources shared by both standalone binaries.
set(SHARED_SOURCES
  src/vpn_client.cpp
  src/vpn_server.cpp
  src/vpn_peer.cpp
  ../src/framework.cpp
  ../src/server_app.cpp
  ../src/client_app.cpp
  ../src/gnmi_client.cpp
  ../src/grpc_session.cpp
  ../src/http2.cpp
  ../src/lua_engine.cpp
  ../src/mqtt_io.cpp
)

# ---------------------------------------------------------------------------
# vpn_server — VPN tunnel server with optional TLS and MQTT subscriber.
# ---------------------------------------------------------------------------
add_executable(vpn_server
  src/vpn_server_main.cpp
  ${SHARED_SOURCES}
)
target_include_directories(vpn_server PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../proto_gen)
target_link_libraries(vpn_server PRIVATE
  gnmi_proto
  mosquitto
  Threads::Threads
  ssl
  crypto
  ${LIBEVENT_LIBRARIES}
  ${LIBEVENT_OPENSSL_LIBRARIES}
  ${NGHTTP2_LIBRARIES}
  ${LUA_LIBRARIES}
)

# ---------------------------------------------------------------------------
# vpn_client — VPN tunnel client with optional TLS and embedded gNMI server.
# ---------------------------------------------------------------------------
add_executable(vpn_client
  src/vpn_client_main.cpp
  ${SHARED_SOURCES}
)
target_include_directories(vpn_client PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../proto_gen)
target_link_libraries(vpn_client PRIVATE
  gnmi_proto
  mosquitto
  Threads::Threads
  ssl
  crypto
  ${LIBEVENT_LIBRARIES}
  ${LIBEVENT_OPENSSL_LIBRARIES}
  ${NGHTTP2_LIBRARIES}
  ${LUA_LIBRARIES}
)
