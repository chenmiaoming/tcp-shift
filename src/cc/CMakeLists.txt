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
    observation.c
    transport_pacing.c
    reno.c
    bbr.c
    bbr_drain.c
    cubic.c
    cubic_hystart.c
    cubic_controller.c
)
target_include_directories(tcp_shift_cc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_options(tcp_shift_cc PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(TCP_SHIFT_CC_STANDALONE)
    add_executable(tcp-shift-cc-contract
        ../tests/cc_contract_test.c
    )
    target_compile_options(tcp-shift-cc-contract PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(tcp-shift-cc-contract PRIVATE tcp_shift_cc)

    add_executable(tcp-shift-cc-registry-contract
        ../tests/cc_registry_test.c
    )
    target_compile_options(tcp-shift-cc-registry-contract PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    target_link_libraries(tcp-shift-cc-registry-contract PRIVATE tcp_shift_cc)

    add_executable(tcp-shift-cc-observation-contract
        ../tests/cc_observation_test.c
    )
    target_compile_options(tcp-shift-cc-observation-contract PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    target_link_libraries(tcp-shift-cc-observation-contract PRIVATE tcp_shift_cc)

    add_executable(tcp-shift-transport-pacing-contract
        ../tests/transport_pacing_test.c
    )
    target_compile_options(tcp-shift-transport-pacing-contract PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    target_link_libraries(tcp-shift-transport-pacing-contract PRIVATE tcp_shift_cc)

    add_executable(tcp-shift-cubic-model-contract
        ../tests/cubic_model_test.c
    )
    target_compile_options(tcp-shift-cubic-model-contract PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    target_link_libraries(tcp-shift-cubic-model-contract PRIVATE tcp_shift_cc)

    add_executable(tcp-shift-cubic-hystart-contract
        ../tests/cubic_hystart_test.c
    )
    target_compile_options(tcp-shift-cubic-hystart-contract PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    target_link_libraries(tcp-shift-cubic-hystart-contract PRIVATE tcp_shift_cc)

    add_executable(tcp-shift-cubic-controller-contract
        ../tests/cubic_controller_test.c
    )
    target_compile_options(tcp-shift-cubic-controller-contract PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    target_link_libraries(tcp-shift-cubic-controller-contract PRIVATE tcp_shift_cc)
endif()