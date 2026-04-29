
cmake_minimum_required(VERSION 3.16.3)
project(hackthon)
find_package(PkgConfig REQUIRED)

# Turn on CTest integration for the whole tree so `ctest` run from the
# build directory discovers tests from every add_subdirectory below.
enable_testing()

add_subdirectory(app)
add_subdirectory(app/cli)
add_subdirectory(app/openvpn)
