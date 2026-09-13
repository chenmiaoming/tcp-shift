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
# EXCLUDE_FROM_ALL; tcp-shift links only the explicit P0 source allowlist below.
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
)

add_library(tcp_shift_lwip STATIC ${TCP_SHIFT_LWIP_SRCS})
target_include_directories(tcp_shift_lwip PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp_shift_lwip PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)

# Linux host integration is deliberately separate from the lwIP transport code.
# This is a source/library boundary, not a process boundary.
add_library(tcp_shift_host STATIC
    src/host/tun.c
    src/host/ifconfig.c
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

# P0 remains an unprivileged reproducible initialization artifact.
add_executable(tcp-shift src/main.c src/platform.c)
target_include_directories(tcp-shift PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp-shift PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)
target_link_libraries(tcp-shift PRIVATE tcp_shift_lwip)
target_link_options(tcp-shift PRIVATE -Wl,--gc-sections)

# Temporary P1 bring-up executable. It now owns TUN creation plus the host-side
# IPv4 address/MTU/link state. DNAT/firewall ownership remains outside this
# temporary entrypoint until its lifecycle contract is proven separately.
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
