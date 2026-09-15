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

add_subdirectory(src/cc)

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

add_library(tcp_shift_pacer STATIC
    src/runtime/pacer.c
)
target_include_directories(tcp_shift_pacer PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(tcp_shift_pacer PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(tcp_shift_runtime STATIC
    src/runtime/lwip_loop.c
)
target_include_directories(tcp_shift_runtime PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp_shift_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(tcp_shift_runtime PUBLIC tcp_shift_l3 tcp_shift_pacer)

add_library(tcp_shift_probe STATIC
    src/lwip/probe_listener.c
)
target_include_directories(tcp_shift_probe PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp_shift_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(tcp_shift_probe PUBLIC tcp_shift_lwip)

add_library(tcp_shift_lwip_cc_adapter STATIC
    src/lwip/cc_adapter.c
    src/lwip/cc_selector.c
)
target_include_directories(tcp_shift_lwip_cc_adapter PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp_shift_lwip_cc_adapter PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(tcp_shift_lwip_cc_adapter PUBLIC tcp_shift_cc tcp_shift_lwip)

add_library(tcp_shift_lwip_cc_pacing_qualification STATIC
    src/lwip/cc_pacing_qualification.c
)
target_include_directories(tcp_shift_lwip_cc_pacing_qualification PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp_shift_lwip_cc_pacing_qualification PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
target_link_libraries(tcp_shift_lwip_cc_pacing_qualification PUBLIC
    tcp_shift_lwip_cc_adapter
    tcp_shift_cc
    tcp_shift_lwip
)

add_library(tcp_shift_bridge STATIC
    src/bridge/bridge.c
)
target_include_directories(tcp_shift_bridge PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp_shift_bridge PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(tcp_shift_bridge PRIVATE
    tcp_accept=tcp_shift_lwip_cc_accept_selected
)
target_link_libraries(tcp_shift_bridge PUBLIC
    tcp_shift_runtime
    tcp_shift_lwip
    tcp_shift_lwip_cc_adapter
)

add_library(tcp_shift_bridge_paced STATIC
    src/bridge/bridge.c
)
target_include_directories(tcp_shift_bridge_paced PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp_shift_bridge_paced PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(tcp_shift_bridge_paced PRIVATE
    tcp_accept=tcp_shift_lwip_cc_accept_fixed_pacing
)
target_link_libraries(tcp_shift_bridge_paced PUBLIC
    tcp_shift_runtime
    tcp_shift_lwip
    tcp_shift_lwip_cc_adapter
    tcp_shift_lwip_cc_pacing_qualification
)

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

add_executable(tcp-shift-p2 src/p2_main.c src/platform.c)
target_include_directories(tcp-shift-p2 PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp-shift-p2 PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)
target_link_libraries(tcp-shift-p2 PRIVATE
    tcp_shift_host
    tcp_shift_bridge
)
target_link_options(tcp-shift-p2 PRIVATE -Wl,--gc-sections)

add_executable(tcp-shift-p5c-paced src/p2_main.c src/platform.c)
target_include_directories(tcp-shift-p5c-paced PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp-shift-p5c-paced PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)
target_link_libraries(tcp-shift-p5c-paced PRIVATE
    tcp_shift_host
    tcp_shift_bridge_paced
)
target_link_options(tcp-shift-p5c-paced PRIVATE -Wl,--gc-sections)

add_executable(tcp-shift-p2-ipv6 src/p2_ipv6_main.c src/platform.c)
target_include_directories(tcp-shift-p2-ipv6 PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp-shift-p2-ipv6 PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)
target_link_libraries(tcp-shift-p2-ipv6 PRIVATE
    tcp_shift_host
    tcp_shift_bridge
)
target_link_options(tcp-shift-p2-ipv6 PRIVATE -Wl,--gc-sections)

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

add_executable(tcp-shift-p1-ipv6-ext-header-probe
    src/tests/ipv6_extension_header_probe.c
)
target_compile_options(tcp-shift-p1-ipv6-ext-header-probe PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

add_executable(tcp-shift-p5-fixed-pacing-policy-contract
    src/tests/fixed_pacing_policy_test.c
)
target_include_directories(tcp-shift-p5-fixed-pacing-policy-contract PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(tcp-shift-p5-fixed-pacing-policy-contract PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
target_link_libraries(tcp-shift-p5-fixed-pacing-policy-contract PRIVATE tcp_shift_cc)

add_executable(tcp-shift-p5-pacer-contract
    src/tests/pacer_scheduler_test.c
)
target_include_directories(tcp-shift-p5-pacer-contract PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(tcp-shift-p5-pacer-contract PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
target_link_libraries(tcp-shift-p5-pacer-contract PRIVATE tcp_shift_pacer)

add_executable(tcp-shift-p5-pacer-loop-contract
    src/tests/pacer_loop_test.c
    src/platform.c
)
target_include_directories(tcp-shift-p5-pacer-loop-contract PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp-shift-p5-pacer-loop-contract PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)
target_link_libraries(tcp-shift-p5-pacer-loop-contract PRIVATE tcp_shift_runtime)
target_link_options(tcp-shift-p5-pacer-loop-contract PRIVATE -Wl,--gc-sections)
