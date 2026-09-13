cmake_minimum_required(VERSION 3.16)
project(tcp_shift C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(LWIP_DIR "${CMAKE_SOURCE_DIR}/.deps/lwip" CACHE PATH "Path to the pinned lwIP checkout")
if(NOT EXISTS "${LWIP_DIR}/src/Filelists.cmake")
    message(FATAL_ERROR "lwIP source not found at ${LWIP_DIR}; run scripts/fetch-lwip.sh first")
endif()

set(LWIP_INCLUDE_DIRS
    "${LWIP_DIR}/src/include"
    "${LWIP_DIR}/contrib"
    "${LWIP_DIR}/contrib/ports/unix/port/include"
    "${CMAKE_SOURCE_DIR}/src"
)

include("${LWIP_DIR}/src/Filelists.cmake")

add_executable(tcp-shift
    src/main.c
    "${LWIP_DIR}/contrib/ports/unix/port/sys_arch.c"
)

target_include_directories(tcp-shift PRIVATE ${LWIP_INCLUDE_DIRS})
target_link_libraries(tcp-shift PRIVATE lwipcore)
target_compile_options(tcp-shift PRIVATE -Wall -Wextra -Wpedantic)
