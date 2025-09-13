# CMAKE configuration to help CLion understand and debug
cmake_minimum_required(VERSION 3.16)
project(fatzke C)
set(CMAKE_C_STANDARD 99)

# fatzke
file(GLOB SRC "${CMAKE_SOURCE_DIR}/../fatzke/*.c")
add_executable(fatzke ${SRC})
if(WIN32)
    target_link_libraries(fatzke PRIVATE user32 gdi32)
else()
    target_link_libraries(fatzke PRIVATE wayland-client)
endif()
set_target_properties(fatzke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")

# battle
file(GLOB SRC "${CMAKE_SOURCE_DIR}/../battle/*.c")
add_executable(battle ${SRC})
if(WIN32)
    target_include_directories(my_vulkan_app PRIVATE "$ENV{VULKAN_SDK}/Include")
    target_link_libraries(battle PRIVATE user32 gdi32 vulkan-1)
else()
    target_link_libraries(battle PRIVATE X11 Xrandr Xi Xpresent rt vulkan)
endif()
