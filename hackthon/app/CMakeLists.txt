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

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20 -g -Wall -Wextra")

include_directories(inc)
file(GLOB SOURCES "src/*.cpp")
add_executable(app ${SOURCES})
target_link_libraries(app PUBLIC pthread ssl crypto ${LIBEVENT_LIBRARIES} ${LUA_LIBRARIES})

# Unit tests for the core engine (lua_file) live in app/test. They only
# depend on GTest + Lua + pthread, not on libevent or networking.
if(BUILD_TESTING OR PROJECT_IS_TOP_LEVEL)
  add_subdirectory(test)
endif()
