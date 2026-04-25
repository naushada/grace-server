cmake_minimum_required(VERSION 3.16.3)

# GTest + pthread + Lua are always required. The framework / client /
# server / fs_app tests additionally pull in libevent (via pkg-config)
# and OpenSSL for the ssl/crypto symbols referenced transitively by
# libevent's bufferevent_openssl objects on some distros.
find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
find_package(Lua REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVENT REQUIRED libevent)
pkg_check_modules(NGHTTP2 REQUIRED libnghttp2)

include(GoogleTest)

include_directories(
  ${LUA_INCLUDE_DIR}
  ${LIBEVENT_INCLUDE_DIRS}
  ${NGHTTP2_INCLUDE_DIRS}
  ${CMAKE_CURRENT_SOURCE_DIR}/../inc
)

# Use the parent flag set — stays in sync with the rest of the project.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20 -g -Wall -Wextra")

# Production sources exercised by the tests. http2.cpp is included because
# client_app.cpp now owns an http2_session and the http2_test exercises it
# directly.
set(APP_SOURCES_UNDER_TEST
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/client_app.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/framework.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/fs_app.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/http2.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/lua_engine.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/server_app.cpp
)

file(GLOB TEST_SOURCES "*.cpp")

add_executable(app_tests
  ${TEST_SOURCES}
  ${APP_SOURCES_UNDER_TEST}
)

target_link_libraries(app_tests PRIVATE
  gnmi_proto
  GTest::gtest
  GTest::gtest_main
  Threads::Threads
  ${LIBEVENT_LIBRARIES}
  ${NGHTTP2_LIBRARIES}
  ssl
  crypto
  ${LUA_LIBRARIES}
)

gtest_discover_tests(app_tests)
