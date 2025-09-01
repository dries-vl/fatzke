# CMAKE configuration to help CLion understand and debug
cmake_minimum_required(VERSION 3.16)
project(fatzke C)
set(CMAKE_C_STANDARD 99)

function(add_bin name src_dir libs_win libs_unix)
    file(GLOB SRCS CONFIGURE_DEPENDS "${src_dir}/*.c")

    if(SRCS STREQUAL "")
        message(FATAL_ERROR "add_bin(${name}): no .c files found in ${src_dir}")
    endif()

    add_executable(${name} ${SRCS})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR})

    if(WIN32)
        if(NOT "${libs_win_norm}" STREQUAL "")
            target_link_libraries(${name} PRIVATE ${libs_win_norm})
        endif()
    else()
        if(NOT "${libs_unix_norm}" STREQUAL "")
            target_link_libraries(${name} PRIVATE ${libs_unix_norm})
        endif()
    endif()

    set_target_properties(${name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../${name}"
    )
endfunction()

# one-liners
add_bin(fatzke ../fatzke "gdi32;user32" "wayland-client")
add_bin(battle ../battle "user32;gdi32" "wayland-client;vulkan")
