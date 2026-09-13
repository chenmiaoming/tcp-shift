cmake_minimum_required(VERSION 3.16)
project(tcp_shift C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(LWIP_DIR "${CMAKE_SOURCE_DIR}/.deps/lwip" CACHE PATH "Path to the pinned lwIP checkout")
if(NOT EXISTS "${LWIP_DIR}/src/Filelists.cmake")
    message(FATAL_ERROR "lwIP source not found at ${LWIP_DIR}; run scripts/fetch-lwip.sh first")
endif()

set(LWIP_INCLUDE_DIRS
    "${LWIP_DIR}/src/include"
    "${LWIP_DIR}/contrib/ports/unix/port/include"
    "${CMAKE_SOURCE_DIR}/src"
)

# Filelists generates lwip/init.h. Its broad library targets remain
# EXCLUDE_FROM_ALL; tcp-shift links only the explicit source allowlist below.
include("${LWIP_DIR}/src/Filelists.cmake")

set(TCP_SHIFT_LWIP_SRCS
    "${LWIP_DIR}/src/core/init.c"
    "${LWIP_DIR}/src/core/def.c"
    "${LWIP_DIR}/src/core/inet_chksum.c"
    "${LWIP_DIR}/src/core/ip.c"
    "${LWIP_DIR}/src/core/mem.c"
    "${LWIP_DIR}/src/core/memp.c"
    "${LWIP_DIR}/src/core/netif.c"
    "${LWIP_DIR}/src/core/pbuf.c"
    "${LWIP_DIR}/src/core/stats.c"
    "${LWIP_DIR}/src/core/tcp.c"
    "${LWIP_DIR}/src/core/tcp_in.c"
    "${LWIP_DIR}/src/core/tcp_out.c"
    "${LWIP_DIR}/src/core/timeouts.c"
    "${LWIP_DIR}/src/core/ipv4/icmp.c"
    "${LWIP_DIR}/src/core/ipv4/ip4.c"
    "${LWIP_DIR}/src/core/ipv4/ip4_addr.c"
    "${LWIP_DIR}/src/core/ipv4/ip4_frag.c"
    "${LWIP_DIR}/src/core/ipv6/icmp6.c"
    "${LWIP_DIR}/src/core/ipv6/inet6.c"
    "${LWIP_DIR}/src/core/ipv6/ip6.c"
    "${LWIP_DIR}/src/core/ipv6/ip6_addr.c"
    "${LWIP_DIR}/src/core/ipv6/nd6.c"
)

# The pinned lwIP ND6 unit leaves two otherwise useful diagnostics unused when
# tcp-shift deliberately disables RS/SLAAC. Keep the warnings visible, but do
# not let only those two upstream diagnostics become build errors. Project code
# and every other warning in the lwIP allowlist remain under -Werror.
set_source_files_properties(
    "${LWIP_DIR}/src/core/ipv6/nd6.c"
    PROPERTIES COMPILE_OPTIONS
        "-Wno-error=unused-parameter;-Wno-error=unused-variable"
)

add_library(tcp_shift_lwip STATIC ${TCP_SHIFT_LWIP_SRCS})
target_include_directories(tcp_shift_lwip PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp_shift_lwip PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)

add_library(tcp_shift_host STATIC
    src/host/tun.c
    src/host/ifconfig.c
    src/host/ifconfig_ipv6.c
    src/host/nft_ingress.c
)
target_include_directories(tcp_shift_host PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(tcp_shift_host PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(tcp_shift_l3 STATIC
    src/lwip/l3_tun.c
)
target_include_directories(tcp_shift_l3 PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp_shift_l3 PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(tcp_shift_l3 PUBLIC tcp_shift_lwip)

add_library(tcp_shift_runtime STATIC
    src/runtime/lwip_loop.c
)
target_include_directories(tcp_shift_runtime PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp_shift_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(tcp_shift_runtime PUBLIC tcp_shift_l3)

add_library(tcp_shift_probe STATIC
    src/lwip/probe_listener.c
)
target_include_directories(tcp_shift_probe PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp_shift_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(tcp_shift_probe PUBLIC tcp_shift_lwip)

add_executable(tcp-shift-config-contract src/config_contract.c)
target_include_directories(tcp-shift-config-contract PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp-shift-config-contract PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(tcp-shift src/main.c src/platform.c)
target_include_directories(tcp-shift PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp-shift PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)
target_link_libraries(tcp-shift PRIVATE tcp_shift_lwip)
target_link_options(tcp-shift PRIVATE -Wl,--gc-sections)

add_executable(tcp-shift-p1 src/p1_main.c src/platform.c)
target_include_directories(tcp-shift-p1 PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp-shift-p1 PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)
target_link_libraries(tcp-shift-p1 PRIVATE
    tcp_shift_host
    tcp_shift_runtime
    tcp_shift_probe
)
target_link_options(tcp-shift-p1 PRIVATE -Wl,--gc-sections)

# P1b IPv6-only bring-up reuses the same host/L3/runtime/probe libraries. It is
# intentionally a temporary qualification entrypoint, not a second datapath.
add_executable(tcp-shift-p1-ipv6 src/p1_ipv6_main.c src/platform.c)
target_include_directories(tcp-shift-p1-ipv6 PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp-shift-p1-ipv6 PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)
target_link_libraries(tcp-shift-p1-ipv6 PRIVATE
    tcp_shift_host
    tcp_shift_runtime
    tcp_shift_probe
)
target_link_options(tcp-shift-p1-ipv6 PRIVATE -Wl,--gc-sections)

add_executable(tcp-shift-p1-backpressure-contract
    src/tests/l3_tun_backpressure_test.c
    src/platform.c
)
target_include_directories(tcp-shift-p1-backpressure-contract PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp-shift-p1-backpressure-contract PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)
target_link_libraries(tcp-shift-p1-backpressure-contract PRIVATE tcp_shift_l3)
target_link_options(tcp-shift-p1-backpressure-contract PRIVATE -Wl,--gc-sections)

add_executable(tcp-shift-p1-rx-contract
    src/tests/l3_tun_rx_contract.c
    src/platform.c
)
target_include_directories(tcp-shift-p1-rx-contract PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp-shift-p1-rx-contract PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)
target_link_libraries(tcp-shift-p1-rx-contract PRIVATE tcp_shift_l3)
target_link_options(tcp-shift-p1-rx-contract PRIVATE -Wl,--gc-sections)
