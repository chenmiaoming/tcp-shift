cmake_minimum_required(VERSION 3.16)
project(tcp_shift C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TCP_SHIFT_EXPERIMENTAL_SACK_EVIDENCE
    "Enable sender SACK negotiation, scoreboard tags, and delivery evidence"
    OFF
)

option(TCP_SHIFT_EXPERIMENTAL_RACK_TLP
    "Enable experimental RFC 8985 RACK-TLP sender loss detection"
    OFF
)

option(TCP_SHIFT_EXPERIMENTAL_ECN
    "Enable experimental RFC 3168 TCP ECN transport support"
    OFF
)

option(TCP_SHIFT_EXPERIMENTAL_BBR_EXPOSURE
    "Expose compact internal BBR through tcp-shift-p2 for provider qualification"
    OFF
)

option(TCP_SHIFT_LEGACY_INITIAL_WINDOW
    "Use the pinned lwIP legacy initial window instead of RFC 6928 IW10"
    OFF
)

if(DEFINED TCP_SHIFT_EXPERIMENTAL_SACK_RECOVERY OR
   DEFINED TCP_SHIFT_EXPERIMENTAL_LEGACY_SACK_SELECTOR)
    message(FATAL_ERROR
        "The legacy fixed-count SACK recovery selector has been removed; "
        "use TCP_SHIFT_EXPERIMENTAL_SACK_EVIDENCE=ON with "
        "TCP_SHIFT_EXPERIMENTAL_RACK_TLP=ON for RFC 8985 recovery")
endif()

if(TCP_SHIFT_EXPERIMENTAL_RACK_TLP AND
   NOT TCP_SHIFT_EXPERIMENTAL_SACK_EVIDENCE)
    message(FATAL_ERROR
        "RFC 8985 RACK-TLP requires TCP_SHIFT_EXPERIMENTAL_SACK_EVIDENCE=ON")
endif()

if(TCP_SHIFT_EXPERIMENTAL_BBR_EXPOSURE AND
   (NOT TCP_SHIFT_EXPERIMENTAL_SACK_EVIDENCE OR
    NOT TCP_SHIFT_EXPERIMENTAL_RACK_TLP))
    message(FATAL_ERROR
        "Experimental BBR exposure requires "
        "TCP_SHIFT_EXPERIMENTAL_SACK_EVIDENCE=ON and "
        "TCP_SHIFT_EXPERIMENTAL_RACK_TLP=ON")
endif()

if(TCP_SHIFT_EXPERIMENTAL_BBR_EXPOSURE AND TCP_SHIFT_EXPERIMENTAL_ECN)
    message(FATAL_ERROR
        "Experimental BBR exposure cannot be combined with ECN yet; "
        "compact BBR has no ECN congestion response")
endif()

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

if(TCP_SHIFT_EXPERIMENTAL_SACK_EVIDENCE)
    # lwipopts.h changes struct tcp_pcb when SACK is enabled. This definition is
    # an ABI requirement, not an implementation detail: every project target
    # that includes lwIP headers must see the same layout as tcp_shift_lwip.
    target_compile_definitions(tcp_shift_lwip PUBLIC
        TCP_SHIFT_EXPERIMENTAL_SACK_EVIDENCE=1
    )
endif()

if(TCP_SHIFT_EXPERIMENTAL_RACK_TLP)
    target_compile_definitions(tcp_shift_lwip PUBLIC
        TCP_SHIFT_EXPERIMENTAL_RACK_TLP=1
    )
endif()

if(TCP_SHIFT_EXPERIMENTAL_ECN)
    # The ECN patch conditionally extends struct tcp_pcb, so every target that
    # includes lwIP headers must share the same definition.
    target_compile_definitions(tcp_shift_lwip PUBLIC
        TCP_SHIFT_EXPERIMENTAL_ECN=1
    )
endif()

if(TCP_SHIFT_LEGACY_INITIAL_WINDOW)
    target_compile_definitions(tcp_shift_lwip PUBLIC
        TCP_SHIFT_LEGACY_INITIAL_WINDOW=1
    )
endif()

add_subdirectory(src/cc)

add_executable(tcp-shift-initial-window-contract
    src/tests/initial_window_test.c
)
target_include_directories(tcp-shift-initial-window-contract PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)
target_compile_options(tcp-shift-initial-window-contract PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
if(TCP_SHIFT_LEGACY_INITIAL_WINDOW)
    target_compile_definitions(tcp-shift-initial-window-contract PRIVATE
        TCP_SHIFT_LEGACY_INITIAL_WINDOW=1
    )
endif()

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

add_library(tcp_shift_lwip_tcp_memory STATIC
    src/lwip/tcp_memory.c
)
target_include_directories(tcp_shift_lwip_tcp_memory PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp_shift_lwip_tcp_memory PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(tcp_shift_lwip_tcp_memory PUBLIC tcp_shift_lwip)

add_library(tcp_shift_rack_tlp STATIC
    src/lwip/rack_tlp.c
)
target_include_directories(tcp_shift_rack_tlp PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_options(tcp_shift_rack_tlp PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(tcp_shift_prr STATIC
    src/lwip/prr.c
)
target_include_directories(tcp_shift_prr PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_options(tcp_shift_prr PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(tcp_shift_lwip_cc_adapter STATIC
    src/lwip/cc_adapter.c
    src/lwip/cc_selector.c
    src/lwip/cc_bbr_internal.c
    src/lwip/cc_bbr_accept.c
)
target_include_directories(tcp_shift_lwip_cc_adapter PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp_shift_lwip_cc_adapter PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(TCP_SHIFT_EXPERIMENTAL_BBR_EXPOSURE)
    target_compile_definitions(tcp_shift_lwip_cc_adapter PUBLIC
        TCP_SHIFT_EXPERIMENTAL_BBR_EXPOSURE=1
    )
endif()
target_link_libraries(tcp_shift_lwip_cc_adapter PUBLIC
    tcp_shift_cc
    tcp_shift_lwip
    tcp_shift_pacer
    tcp_shift_rack_tlp
    tcp_shift_prr
)

add_library(tcp_shift_lwip_cc_adapter_pacing_bypass_qualification STATIC
    src/lwip/cc_adapter.c
    src/lwip/cc_selector.c
    src/lwip/cc_bbr_internal.c
)
target_include_directories(tcp_shift_lwip_cc_adapter_pacing_bypass_qualification PRIVATE
    ${LWIP_INCLUDE_DIRS}
)
target_compile_options(tcp_shift_lwip_cc_adapter_pacing_bypass_qualification PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
target_compile_definitions(tcp_shift_lwip_cc_adapter_pacing_bypass_qualification PRIVATE
    TCP_SHIFT_P6_CUBIC_PACING_BYPASS_QUALIFICATION=1
)
target_link_libraries(tcp_shift_lwip_cc_adapter_pacing_bypass_qualification PUBLIC
    tcp_shift_cc
    tcp_shift_lwip
    tcp_shift_pacer
    tcp_shift_rack_tlp
    tcp_shift_prr
)

add_library(tcp_shift_lwip_cc_adapter_cubic_trace_qualification STATIC
    src/lwip/cc_adapter.c
    src/lwip/cc_selector.c
    src/lwip/cc_bbr_internal.c
)
target_include_directories(tcp_shift_lwip_cc_adapter_cubic_trace_qualification PRIVATE
    ${LWIP_INCLUDE_DIRS}
)
target_compile_options(tcp_shift_lwip_cc_adapter_cubic_trace_qualification PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
target_compile_definitions(tcp_shift_lwip_cc_adapter_cubic_trace_qualification PRIVATE
    TCP_SHIFT_P6_CUBIC_RECOVERY_TRACE_QUALIFICATION=1
)
target_link_libraries(tcp_shift_lwip_cc_adapter_cubic_trace_qualification PUBLIC
    tcp_shift_cc
    tcp_shift_lwip
    tcp_shift_pacer
    tcp_shift_rack_tlp
    tcp_shift_prr
)

add_library(tcp_shift_lwip_cc_adapter_cubic_single_flow_qualification STATIC
    src/lwip/cc_adapter.c
    src/lwip/cc_selector.c
    src/lwip/cc_bbr_internal.c
)
target_include_directories(tcp_shift_lwip_cc_adapter_cubic_single_flow_qualification PRIVATE
    ${LWIP_INCLUDE_DIRS}
)
target_compile_options(tcp_shift_lwip_cc_adapter_cubic_single_flow_qualification PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
target_compile_definitions(tcp_shift_lwip_cc_adapter_cubic_single_flow_qualification PRIVATE
    TCP_SHIFT_P6_CUBIC_RECOVERY_TRACE_QUALIFICATION=1
    TCP_SHIFT_P6_CUBIC_SINGLE_FLOW_NO_FAST_CONVERGENCE_QUALIFICATION=1
)
target_link_libraries(tcp_shift_lwip_cc_adapter_cubic_single_flow_qualification PUBLIC
    tcp_shift_cc
    tcp_shift_lwip
    tcp_shift_pacer
    tcp_shift_rack_tlp
    tcp_shift_prr
)

add_library(tcp_shift_lwip_cc_adapter_cubic_single_flow_prr_pacing_actual_qualification STATIC
    src/lwip/cc_adapter.c
    src/lwip/cc_selector.c
    src/lwip/cc_bbr_internal.c
)
target_include_directories(tcp_shift_lwip_cc_adapter_cubic_single_flow_prr_pacing_actual_qualification PRIVATE
    ${LWIP_INCLUDE_DIRS}
)
target_compile_options(tcp_shift_lwip_cc_adapter_cubic_single_flow_prr_pacing_actual_qualification PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
target_compile_definitions(tcp_shift_lwip_cc_adapter_cubic_single_flow_prr_pacing_actual_qualification PRIVATE
    TCP_SHIFT_P6_CUBIC_RECOVERY_TRACE_QUALIFICATION=1
    TCP_SHIFT_P6_CUBIC_SINGLE_FLOW_NO_FAST_CONVERGENCE_QUALIFICATION=1
    TCP_SHIFT_P6_CUBIC_PRR_PACING_REFRESH_ACTUAL_QUALIFICATION=1
)
target_link_libraries(tcp_shift_lwip_cc_adapter_cubic_single_flow_prr_pacing_actual_qualification PUBLIC
    tcp_shift_cc
    tcp_shift_lwip
    tcp_shift_pacer
    tcp_shift_rack_tlp
    tcp_shift_prr
)

add_library(tcp_shift_lwip_cc_adapter_cubic_single_flow_prr_pacing_raw_qualification STATIC
    src/lwip/cc_adapter.c
    src/lwip/cc_selector.c
    src/lwip/cc_bbr_internal.c
)
target_include_directories(tcp_shift_lwip_cc_adapter_cubic_single_flow_prr_pacing_raw_qualification PRIVATE
    ${LWIP_INCLUDE_DIRS}
)
target_compile_options(tcp_shift_lwip_cc_adapter_cubic_single_flow_prr_pacing_raw_qualification PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
target_compile_definitions(tcp_shift_lwip_cc_adapter_cubic_single_flow_prr_pacing_raw_qualification PRIVATE
    TCP_SHIFT_P6_CUBIC_RECOVERY_TRACE_QUALIFICATION=1
    TCP_SHIFT_P6_CUBIC_SINGLE_FLOW_NO_FAST_CONVERGENCE_QUALIFICATION=1
    TCP_SHIFT_P6_CUBIC_PRR_PACING_REFRESH_RAW_QUALIFICATION=1
)
target_link_libraries(tcp_shift_lwip_cc_adapter_cubic_single_flow_prr_pacing_raw_qualification PUBLIC
    tcp_shift_cc
    tcp_shift_lwip
    tcp_shift_pacer
    tcp_shift_rack_tlp
    tcp_shift_prr
)

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
    tcp_write=tcp_shift_lwip_tcp_memory_write
    tcp_sent=tcp_shift_lwip_tcp_memory_sent
)
target_link_libraries(tcp_shift_bridge PUBLIC
    tcp_shift_runtime
    tcp_shift_lwip
    tcp_shift_lwip_cc_adapter
    tcp_shift_lwip_tcp_memory
)

add_library(tcp_shift_bridge_cubic_pacing_bypass STATIC
    src/bridge/bridge.c
)
target_include_directories(tcp_shift_bridge_cubic_pacing_bypass PRIVATE
    ${LWIP_INCLUDE_DIRS}
)
target_compile_options(tcp_shift_bridge_cubic_pacing_bypass PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
target_compile_definitions(tcp_shift_bridge_cubic_pacing_bypass PRIVATE
    tcp_accept=tcp_shift_lwip_cc_accept_selected
    tcp_write=tcp_shift_lwip_tcp_memory_write
    tcp_sent=tcp_shift_lwip_tcp_memory_sent
)
target_link_libraries(tcp_shift_bridge_cubic_pacing_bypass PUBLIC
    tcp_shift_runtime
    tcp_shift_lwip
    tcp_shift_lwip_cc_adapter_pacing_bypass_qualification
    tcp_shift_lwip_tcp_memory
)

add_library(tcp_shift_bridge_cubic_trace STATIC
    src/bridge/bridge.c
)
target_include_directories(tcp_shift_bridge_cubic_trace PRIVATE
    ${LWIP_INCLUDE_DIRS}
)
target_compile_options(tcp_shift_bridge_cubic_trace PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
target_compile_definitions(tcp_shift_bridge_cubic_trace PRIVATE
    tcp_accept=tcp_shift_lwip_cc_accept_selected
    tcp_write=tcp_shift_lwip_tcp_memory_write
    tcp_sent=tcp_shift_lwip_tcp_memory_sent
)
target_link_libraries(tcp_shift_bridge_cubic_trace PUBLIC
    tcp_shift_runtime
    tcp_shift_lwip
    tcp_shift_lwip_cc_adapter_cubic_trace_qualification
    tcp_shift_lwip_tcp_memory
)

add_library(tcp_shift_bridge_cubic_single_flow STATIC
    src/bridge/bridge.c
)
target_include_directories(tcp_shift_bridge_cubic_single_flow PRIVATE
    ${LWIP_INCLUDE_DIRS}
)
target_compile_options(tcp_shift_bridge_cubic_single_flow PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
target_compile_definitions(tcp_shift_bridge_cubic_single_flow PRIVATE
    tcp_accept=tcp_shift_lwip_cc_accept_selected
    tcp_write=tcp_shift_lwip_tcp_memory_write
    tcp_sent=tcp_shift_lwip_tcp_memory_sent
)
target_link_libraries(tcp_shift_bridge_cubic_single_flow PUBLIC
    tcp_shift_runtime
    tcp_shift_lwip
    tcp_shift_lwip_cc_adapter_cubic_single_flow_qualification
    tcp_shift_lwip_tcp_memory
)

add_library(tcp_shift_bridge_cubic_single_flow_prr_pacing_actual STATIC
    src/bridge/bridge.c
)
target_include_directories(tcp_shift_bridge_cubic_single_flow_prr_pacing_actual PRIVATE
    ${LWIP_INCLUDE_DIRS}
)
target_compile_options(tcp_shift_bridge_cubic_single_flow_prr_pacing_actual PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
target_compile_definitions(tcp_shift_bridge_cubic_single_flow_prr_pacing_actual PRIVATE
    tcp_accept=tcp_shift_lwip_cc_accept_selected
    tcp_write=tcp_shift_lwip_tcp_memory_write
    tcp_sent=tcp_shift_lwip_tcp_memory_sent
)
target_link_libraries(tcp_shift_bridge_cubic_single_flow_prr_pacing_actual PUBLIC
    tcp_shift_runtime
    tcp_shift_lwip
    tcp_shift_lwip_cc_adapter_cubic_single_flow_prr_pacing_actual_qualification
    tcp_shift_lwip_tcp_memory
)

add_library(tcp_shift_bridge_cubic_single_flow_prr_pacing_raw STATIC
    src/bridge/bridge.c
)
target_include_directories(tcp_shift_bridge_cubic_single_flow_prr_pacing_raw PRIVATE
    ${LWIP_INCLUDE_DIRS}
)
target_compile_options(tcp_shift_bridge_cubic_single_flow_prr_pacing_raw PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
target_compile_definitions(tcp_shift_bridge_cubic_single_flow_prr_pacing_raw PRIVATE
    tcp_accept=tcp_shift_lwip_cc_accept_selected
    tcp_write=tcp_shift_lwip_tcp_memory_write
    tcp_sent=tcp_shift_lwip_tcp_memory_sent
)
target_link_libraries(tcp_shift_bridge_cubic_single_flow_prr_pacing_raw PUBLIC
    tcp_shift_runtime
    tcp_shift_lwip
    tcp_shift_lwip_cc_adapter_cubic_single_flow_prr_pacing_raw_qualification
    tcp_shift_lwip_tcp_memory
)

add_library(tcp_shift_bridge_bbr_internal STATIC
    src/bridge/bridge.c
)
target_include_directories(tcp_shift_bridge_bbr_internal PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp_shift_bridge_bbr_internal PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(tcp_shift_bridge_bbr_internal PRIVATE
    tcp_accept=tcp_shift_lwip_cc_accept_internal_bbr
    tcp_write=tcp_shift_lwip_tcp_memory_write
    tcp_sent=tcp_shift_lwip_tcp_memory_sent
)
target_link_libraries(tcp_shift_bridge_bbr_internal PUBLIC
    tcp_shift_runtime
    tcp_shift_lwip
    tcp_shift_lwip_cc_adapter
    tcp_shift_lwip_tcp_memory
)

add_library(tcp_shift_bridge_paced STATIC
    src/bridge/bridge.c
)
target_include_directories(tcp_shift_bridge_paced PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp_shift_bridge_paced PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(tcp_shift_bridge_paced PRIVATE
    tcp_accept=tcp_shift_lwip_cc_accept_fixed_pacing
    tcp_write=tcp_shift_lwip_tcp_memory_write
    tcp_sent=tcp_shift_lwip_tcp_memory_sent
)
target_link_libraries(tcp_shift_bridge_paced PUBLIC
    tcp_shift_runtime
    tcp_shift_lwip
    tcp_shift_lwip_cc_adapter
    tcp_shift_lwip_cc_pacing_qualification
    tcp_shift_lwip_tcp_memory
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
if(TCP_SHIFT_EXPERIMENTAL_BBR_EXPOSURE)
    target_compile_definitions(tcp-shift-p2 PRIVATE
        TCP_SHIFT_EXPERIMENTAL_BBR_EXPOSURE=1
    )
endif()
target_link_options(tcp-shift-p2 PRIVATE -Wl,--gc-sections)

add_executable(tcp-shift-p6-cubic-pacing-bypass
    src/p2_main.c
    src/platform.c
)
target_include_directories(tcp-shift-p6-cubic-pacing-bypass PRIVATE
    ${LWIP_INCLUDE_DIRS}
)
target_compile_options(tcp-shift-p6-cubic-pacing-bypass PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)
target_link_libraries(tcp-shift-p6-cubic-pacing-bypass PRIVATE
    tcp_shift_host
    tcp_shift_bridge_cubic_pacing_bypass
)
target_link_options(tcp-shift-p6-cubic-pacing-bypass PRIVATE
    -Wl,--gc-sections
)

add_executable(tcp-shift-p6-cubic-recovery-trace
    src/p2_main.c
    src/platform.c
)
target_include_directories(tcp-shift-p6-cubic-recovery-trace PRIVATE
    ${LWIP_INCLUDE_DIRS}
)
target_compile_options(tcp-shift-p6-cubic-recovery-trace PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)
target_link_libraries(tcp-shift-p6-cubic-recovery-trace PRIVATE
    tcp_shift_host
    tcp_shift_bridge_cubic_trace
)
target_link_options(tcp-shift-p6-cubic-recovery-trace PRIVATE
    -Wl,--gc-sections
)

add_executable(tcp-shift-p6-cubic-single-flow
    src/p2_main.c
    src/platform.c
)
target_include_directories(tcp-shift-p6-cubic-single-flow PRIVATE
    ${LWIP_INCLUDE_DIRS}
)
target_compile_options(tcp-shift-p6-cubic-single-flow PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)
target_link_libraries(tcp-shift-p6-cubic-single-flow PRIVATE
    tcp_shift_host
    tcp_shift_bridge_cubic_single_flow
)
target_link_options(tcp-shift-p6-cubic-single-flow PRIVATE
    -Wl,--gc-sections
)

add_executable(tcp-shift-p6-cubic-single-flow-prr-pacing-actual
    src/p2_main.c
    src/platform.c
)
target_include_directories(tcp-shift-p6-cubic-single-flow-prr-pacing-actual PRIVATE
    ${LWIP_INCLUDE_DIRS}
)
target_compile_options(tcp-shift-p6-cubic-single-flow-prr-pacing-actual PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)
target_link_libraries(tcp-shift-p6-cubic-single-flow-prr-pacing-actual PRIVATE
    tcp_shift_host
    tcp_shift_bridge_cubic_single_flow_prr_pacing_actual
)
target_link_options(tcp-shift-p6-cubic-single-flow-prr-pacing-actual PRIVATE
    -Wl,--gc-sections
)

add_executable(tcp-shift-p6-cubic-single-flow-prr-pacing-raw
    src/p2_main.c
    src/platform.c
)
target_include_directories(tcp-shift-p6-cubic-single-flow-prr-pacing-raw PRIVATE
    ${LWIP_INCLUDE_DIRS}
)
target_compile_options(tcp-shift-p6-cubic-single-flow-prr-pacing-raw PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)
target_link_libraries(tcp-shift-p6-cubic-single-flow-prr-pacing-raw PRIVATE
    tcp_shift_host
    tcp_shift_bridge_cubic_single_flow_prr_pacing_raw
)
target_link_options(tcp-shift-p6-cubic-single-flow-prr-pacing-raw PRIVATE
    -Wl,--gc-sections
)

add_executable(tcp-shift-p6-bbr src/p2_main.c src/platform.c)
target_include_directories(tcp-shift-p6-bbr PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp-shift-p6-bbr PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)
target_compile_definitions(tcp-shift-p6-bbr PRIVATE
    TCP_SHIFT_INTERNAL_BBR_QUALIFICATION=1
)
target_link_libraries(tcp-shift-p6-bbr PRIVATE
    tcp_shift_host
    tcp_shift_bridge_bbr_internal
)
target_link_options(tcp-shift-p6-bbr PRIVATE -Wl,--gc-sections)

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

add_executable(tcp-shift-tcp-memory-contract
    src/tests/tcp_memory_test.c
)
target_include_directories(tcp-shift-tcp-memory-contract PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp-shift-tcp-memory-contract PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
target_link_libraries(tcp-shift-tcp-memory-contract PRIVATE tcp_shift_lwip_tcp_memory)


add_executable(tcp-shift-bbr-exposure-selector-contract
    src/tests/bbr_exposure_selector_test.c
)
target_include_directories(tcp-shift-bbr-exposure-selector-contract PRIVATE
    ${LWIP_INCLUDE_DIRS}
)
target_compile_options(tcp-shift-bbr-exposure-selector-contract PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
target_link_libraries(tcp-shift-bbr-exposure-selector-contract PRIVATE
    tcp_shift_lwip_cc_adapter
)

add_executable(tcp-shift-rack-tlp-contract
    src/tests/rack_tlp_test.c
)
target_include_directories(tcp-shift-rack-tlp-contract PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(tcp-shift-rack-tlp-contract PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
target_link_libraries(tcp-shift-rack-tlp-contract PRIVATE tcp_shift_rack_tlp)

add_executable(tcp-shift-prr-contract
    src/tests/prr_test.c
)
target_include_directories(tcp-shift-prr-contract PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(tcp-shift-prr-contract PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
target_link_libraries(tcp-shift-prr-contract PRIVATE tcp_shift_prr)

add_executable(tcp-shift-rack-resource-size-contract
    src/tests/rack_resource_size_test.c
)
target_include_directories(tcp-shift-rack-resource-size-contract PRIVATE
    ${LWIP_INCLUDE_DIRS}
)
target_compile_options(tcp-shift-rack-resource-size-contract PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
target_link_libraries(tcp-shift-rack-resource-size-contract PRIVATE
    tcp_shift_lwip_cc_adapter
    tcp_shift_runtime
)

if(TCP_SHIFT_EXPERIMENTAL_RACK_TLP)
    add_executable(tcp-shift-rack-tlp-adapter-contract
        src/tests/rack_tlp_adapter_test.c
        src/platform.c
    )
    target_include_directories(tcp-shift-rack-tlp-adapter-contract PRIVATE
        ${LWIP_INCLUDE_DIRS}
    )
    target_compile_options(tcp-shift-rack-tlp-adapter-contract PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    target_link_libraries(tcp-shift-rack-tlp-adapter-contract PRIVATE
        tcp_shift_lwip_cc_adapter
        tcp_shift_lwip
    )

    add_executable(tcp-shift-rack-tlp-loop-lifecycle-contract
        src/tests/rack_tlp_loop_lifecycle_test.c
        src/platform.c
    )
    target_include_directories(tcp-shift-rack-tlp-loop-lifecycle-contract PRIVATE
        ${LWIP_INCLUDE_DIRS}
    )
    target_compile_options(tcp-shift-rack-tlp-loop-lifecycle-contract PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -ffunction-sections -fdata-sections
    )
    target_link_libraries(tcp-shift-rack-tlp-loop-lifecycle-contract PRIVATE
        tcp_shift_runtime
    )
    target_link_options(tcp-shift-rack-tlp-loop-lifecycle-contract PRIVATE
        -Wl,--gc-sections
    )
endif()
