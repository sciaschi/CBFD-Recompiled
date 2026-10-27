# Netplay's transport: the packets, sessions over GameNetworkingSockets (directly, or through ICE with a
# signalling server), a test program, the signalling server (netplay_server: tools/netplay/server/) and
# the packets' tests and fuzz target. The game includes it with CONKER_NETPLAY (host/CMakeLists.txt);
# it also builds alone, without the game or the ROM (as the CI does):
#
#   cmake -S host/src/netplay/net -B build-net -G Ninja && cmake --build build-net
#   build-net/packets_test && build-net/netplay_probe host 5000 & build-net/netplay_probe join 127.0.0.1:5000
#
# GameNetworkingSockets is downloaded while configuring (FetchContent, a pinned commit), only when this
# is built: not a submodule, as its own submodules (WebRTC, gigabytes) would come with build.sh's
# recursive update. It needs protobuf (on Windows from vcpkg: vcpkg.cmake), and OpenSSL for its crypto but
# on Windows (bcrypt, built in). Its own ICE client is on (NAT traversal, and relays through TURN), not
# WebRTC's.
cmake_minimum_required(VERSION 3.20)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include("${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.cmake")
    project(ConkerNetplay C CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED True)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

option(CONKER_NETPLAY_FUZZ "Build the packets' libFuzzer target (clang)" OFF)

# What the netplay server needs (signalling's packets, room codes, TURN credentials), and with it the game's
# packets and LAN beacons: no dependencies, for the tests and the fuzzer. The server builds only the first,
# so a change to the game's own packets doesn't touch it (its image isn't rebuilt for one).
add_library(conker_netplay_signal STATIC signal_packets.cpp join_code.cpp turn_credentials.cpp)
target_include_directories(conker_netplay_signal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(conker_netplay_signal PUBLIC cxx_std_20)
add_library(conker_netplay_packets STATIC packets.cpp lan_packets.cpp)
target_link_libraries(conker_netplay_packets PUBLIC conker_netplay_signal)

add_executable(packets_test packets_test.cpp)
target_link_libraries(packets_test PRIVATE conker_netplay_packets)
add_executable(join_code_test join_code_test.cpp)
target_link_libraries(join_code_test PRIVATE conker_netplay_packets)
add_executable(signal_packets_test signal_packets_test.cpp)
target_link_libraries(signal_packets_test PRIVATE conker_netplay_signal)
add_executable(turn_credentials_test turn_credentials_test.cpp)
target_link_libraries(turn_credentials_test PRIVATE conker_netplay_signal)
add_executable(lan_packets_test lan_packets_test.cpp)
target_link_libraries(lan_packets_test PRIVATE conker_netplay_packets)
# The connection test's STUN and TURN client (plain sockets, no dependencies).
add_library(conker_netplay_stun STATIC stun.cpp)
target_link_libraries(conker_netplay_stun PUBLIC conker_netplay_signal)
if (WIN32)
    target_link_libraries(conker_netplay_stun PUBLIC ws2_32)
endif()
add_executable(stun_test stun_test.cpp)
target_link_libraries(stun_test PRIVATE conker_netplay_stun)
# The server's status page and coturn's metrics (no dependencies).
add_library(conker_netplay_status STATIC server_status.cpp)
target_include_directories(conker_netplay_status PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(conker_netplay_status PUBLIC cxx_std_20)
add_executable(server_status_test server_status_test.cpp)
target_link_libraries(server_status_test PRIVATE conker_netplay_status)

if (CONKER_NETPLAY_FUZZ)
    add_executable(fuzz_packets fuzz_packets.cpp packets.cpp signal_packets.cpp lan_packets.cpp join_code.cpp)
    target_include_directories(fuzz_packets PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options(fuzz_packets PRIVATE -fsanitize=fuzzer,address,undefined -g)
    target_link_options(fuzz_packets PRIVATE -fsanitize=fuzzer,address,undefined)
    return()
endif()

include(FetchContent)
# v1.6.1. Its submodules (abseil, WebRTC, vjson) are only for WebRTC's ICE and its tests: its own ICE
# client needs none.
# Patched (patches/): relay-only ICE checks only from relay candidates (gns-relay-only-checks.patch).
FetchContent_Declare(GameNetworkingSockets
    GIT_REPOSITORY https://github.com/ValveSoftware/GameNetworkingSockets.git
    GIT_TAG d534c19aa760df3fb75fd20db13ba1932b8a5463
    GIT_SUBMODULES ""
    PATCH_COMMAND ${CMAKE_COMMAND} -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/patches/gns-relay-only-checks.patch
        -P ${CMAKE_CURRENT_SOURCE_DIR}/patches/apply.cmake
    UPDATE_DISCONNECTED TRUE
    EXCLUDE_FROM_ALL)
set(BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ENABLE_ICE ON CACHE BOOL "" FORCE)
set(USE_STEAMWEBRTC OFF CACHE BOOL "" FORCE)
if (WIN32)
    set(USE_CRYPTO "BCrypt" CACHE STRING "" FORCE)
endif()
FetchContent_MakeAvailable(GameNetworkingSockets)

add_library(conker_netplay_net STATIC session.cpp signalling.cpp lan.cpp connection_test.cpp)
target_link_libraries(conker_netplay_net PUBLIC conker_netplay_packets conker_netplay_stun GameNetworkingSockets::static)

add_executable(netplay_probe probe.cpp)
target_link_libraries(netplay_probe PRIVATE conker_netplay_net)

add_executable(netplay_server signal_server.cpp)
target_link_libraries(netplay_server PRIVATE conker_netplay_signal conker_netplay_status GameNetworkingSockets::static)
if (WIN32)
    target_link_libraries(netplay_server PRIVATE ws2_32)
endif()
