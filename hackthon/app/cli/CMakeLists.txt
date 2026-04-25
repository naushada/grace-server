cmake_minimum_required(VERSION 3.16.3)
project(hackthon)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
# Find GoogleTest
find_package(GTest REQUIRED)
# Use pkg-config to find libevent and set variables
pkg_check_modules(LIBEVENT REQUIRED libevent)

find_package(Lua REQUIRED)
include_directories(${LUA_INCLUDE_DIR})

# Find Readline (You may need to add a FindReadline.cmake or use a simple check)
find_library(READLINE_LIBRARY NAMES readline)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20 -g -Wall -Wextra")

include_directories(inc)
include_directories(../inc)
file(GLOB SOURCES "src/*.cpp")

file(GLOB SHARED_SOURCES 
  "../src/framework.cpp" 
  "../src/fs_app.cpp" 
  "../src/lua_engine.cpp" 
  "../src/lua_file.cpp")

add_executable(cli_app
  ${SOURCES}
  ${SHARED_SOURCES})
target_link_libraries(cli_app PRIVATE
  pthread
  ssl
  crypto
  readline
  protobuf
  ${LIBEVENT_LIBRARIES}
  ${LUA_LIBRARIES})

# Completion-helper tests. Deliberately isolated from readline / protobuf.
if((BUILD_TESTING OR PROJECT_IS_TOP_LEVEL) AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test")
  add_subdirectory(test)
endif()
