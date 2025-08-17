cmake_minimum_required(VERSION 4.0)
project(fatzke C)

set(CMAKE_C_STANDARD 11)

add_executable(fatzke main.c)

if(windows)
    target_link_libraries(fatzke PRIVATE user32 gdi32)
else()
    target_link_libraries(fatzke PRIVATE wayland-client)
endif()

set_target_properties(fatzke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")
