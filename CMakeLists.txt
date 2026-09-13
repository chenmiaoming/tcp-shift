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
    "${CMAKE_SOURCE_DIR}/src"
)

# Filelists.cmake supplies version generation and source groups. Do not link its
# catch-all lwipcore target: that target intentionally compiles IPv6, API, PPP,
# 6LoWPAN, and generic netif sources that tcp-shift P0 does not own.
include("${LWIP_DIR}/src/Filelists.cmake")

add_library(tcp_shift_lwip STATIC
    ${lwipcore_SRCS}
    ${lwipcore4_SRCS}
)
target_include_directories(tcp_shift_lwip PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp_shift_lwip PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)

add_executable(tcp-shift-config-contract src/config_contract.c)
target_include_directories(tcp-shift-config-contract PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp-shift-config-contract PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(tcp-shift src/main.c)
target_include_directories(tcp-shift PRIVATE ${LWIP_INCLUDE_DIRS})
target_compile_options(tcp-shift PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -ffunction-sections -fdata-sections
)
target_link_libraries(tcp-shift PRIVATE tcp_shift_lwip)
target_link_options(tcp-shift PRIVATE -Wl,--gc-sections)
