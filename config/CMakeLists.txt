# CMAKE configuration to help CLion understand and debug
cmake_minimum_required(VERSION 4.0)
project(../fatzke C)

set(CMAKE_C_STANDARD 99)

function(add_bin name src libs_win libs_unix)
    if(WIN32)
        add_executable(${name} ${src})
        if(NOT "${libs_win}" STREQUAL "")
            target_link_libraries(${name} PRIVATE ${libs_win})
        endif()
    else()
        add_executable(${name} ${src})
        if(NOT "${libs_unix}" STREQUAL "")
            target_link_libraries(${name} PRIVATE ${libs_unix})
        endif()
    endif()
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../${name}")
endfunction()

# add each binary project in one line
add_bin(fatzke ../fatzke/main.c user32;gdi32 wayland-client)
