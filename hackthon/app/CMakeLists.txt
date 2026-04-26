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

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20 -g -Wall -Wextra")

# ---------------------------------------------------------------------------
# gNMI protobuf library
# ---------------------------------------------------------------------------
# Proto files are vendored under app/idl/ with a flat two-level layout:
#   idl/gnmi/gnmi.proto
#   idl/gnmi_ext/gnmi_ext.proto
# The import in gnmi.proto was adjusted from the upstream Go module path to
# the shorter local path so protoc resolves it with -I app/idl.
set(IDL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/idl")

set(GNMI_PROTO_FILES
    "${IDL_DIR}/gnmi_ext/gnmi_ext.proto"
    "${IDL_DIR}/gnmi/gnmi.proto"
    "${IDL_DIR}/collector/collector.proto"
    "${IDL_DIR}/target/target.proto"
)

set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto_gen")
file(MAKE_DIRECTORY "${PROTO_GEN_DIR}")

# Output paths mirror the proto's relative path from IDL_DIR.
# Compilation order matters: gnmi_ext first (imported by gnmi),
# then gnmi (imported by target), then collector and target.
set(GNMI_GEN_SRCS
    "${PROTO_GEN_DIR}/gnmi_ext/gnmi_ext.pb.cc"
    "${PROTO_GEN_DIR}/gnmi/gnmi.pb.cc"
    "${PROTO_GEN_DIR}/collector/collector.pb.cc"
    "${PROTO_GEN_DIR}/target/target.pb.cc"
)
set(GNMI_GEN_HDRS
    "${PROTO_GEN_DIR}/gnmi_ext/gnmi_ext.pb.h"
    "${PROTO_GEN_DIR}/gnmi/gnmi.pb.h"
    "${PROTO_GEN_DIR}/collector/collector.pb.h"
    "${PROTO_GEN_DIR}/target/target.pb.h"
)

add_custom_command(
    OUTPUT  ${GNMI_GEN_SRCS} ${GNMI_GEN_HDRS}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
            --cpp_out=${PROTO_GEN_DIR}
            -I${IDL_DIR}
            -I${Protobuf_INCLUDE_DIRS}
            ${GNMI_PROTO_FILES}
    DEPENDS ${GNMI_PROTO_FILES}
    COMMENT "Generating C++ from gNMI proto files"
    VERBATIM
)

add_library(gnmi_proto STATIC ${GNMI_GEN_SRCS})
target_include_directories(gnmi_proto PUBLIC
    ${PROTO_GEN_DIR}
    ${Protobuf_INCLUDE_DIRS}
)
target_link_libraries(gnmi_proto PUBLIC ${Protobuf_LIBRARIES})

# ---------------------------------------------------------------------------
# Main application
# ---------------------------------------------------------------------------
include_directories(inc)
include_directories(openvpn/inc)
include_directories(${NGHTTP2_INCLUDE_DIRS})
file(GLOB SOURCES "src/*.cpp")
file(GLOB OPENVPN_SOURCES "openvpn/src/*.cpp")
add_executable(app ${SOURCES} ${OPENVPN_SOURCES})
target_include_directories(app PRIVATE ${PROTO_GEN_DIR})
target_link_libraries(app PUBLIC
  gnmi_proto
  pthread
  ssl
  crypto
  ${LIBEVENT_LIBRARIES}
  ${LIBEVENT_OPENSSL_LIBRARIES}
  ${NGHTTP2_LIBRARIES}
  ${LUA_LIBRARIES})

# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------
if(BUILD_TESTING OR PROJECT_IS_TOP_LEVEL)
  add_subdirectory(test)
  add_subdirectory(openvpn/test)
endif()
