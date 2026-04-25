cmake_minimum_required(VERSION 3.16.3)

# These tests intentionally depend on only GTest, pthread, and Lua — they do
# not link against readline or protobuf, which keeps the build footprint
# small and the tests hermetic.
find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
find_package(Lua REQUIRED)

include(GoogleTest)

include_directories(
  ${LUA_INCLUDE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../inc
  ${CMAKE_CURRENT_SOURCE_DIR}/../../inc
)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20 -g -Wall -Wextra")

# Production sources the tests exercise.
set(CLI_SOURCES_UNDER_TEST
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/completion_helpers.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/lua_engine.cpp
)

file(GLOB TEST_SOURCES "*.cpp")

add_executable(cli_tests
  ${TEST_SOURCES}
  ${CLI_SOURCES_UNDER_TEST}
)

target_link_libraries(cli_tests PRIVATE
  GTest::gtest
  GTest::gtest_main
  Threads::Threads
  ${LUA_LIBRARIES}
)

gtest_discover_tests(cli_tests)
