cmake_minimum_required(VERSION 3.16.3)

find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVENT REQUIRED libevent)

include(GoogleTest)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20 -g -Wall -Wextra")

include_directories(
  ${LIBEVENT_INCLUDE_DIRS}
  ${CMAKE_CURRENT_SOURCE_DIR}/../inc       # openvpn_client.hpp, openvpn_server.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../inc    # framework.hpp
)

# Production sources exercised by the tests.
set(OPENVPN_SRCS_UNDER_TEST
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/openvpn_client.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/openvpn_server.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/openvpn_peer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/framework.cpp
)

add_executable(openvpn_tests
  openvpn_test.cpp
  ${OPENVPN_SRCS_UNDER_TEST}
)

target_link_libraries(openvpn_tests PRIVATE
  GTest::gtest
  GTest::gtest_main
  Threads::Threads
  ${LIBEVENT_LIBRARIES}
  ssl
  crypto
)

gtest_discover_tests(openvpn_tests)
