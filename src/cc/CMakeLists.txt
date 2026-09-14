cmake_minimum_required(VERSION 3.16)

set(TCP_SHIFT_CC_STANDALONE OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(tcp_shift_cc C)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS OFF)
    set(TCP_SHIFT_CC_STANDALONE ON)
endif()

add_library(tcp_shift_cc STATIC
    cc.c
    reno.c
)
target_include_directories(tcp_shift_cc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(tcp_shift_cc PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(TCP_SHIFT_CC_STANDALONE)
    add_executable(tcp-shift-cc-contract
        ../tests/cc_contract_test.c
    )
    target_compile_options(tcp-shift-cc-contract PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(tcp-shift-cc-contract PRIVATE tcp_shift_cc)
endif()
