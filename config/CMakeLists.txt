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
    target_include_directories(battle PRIVATE "$ENV{VULKAN_SDK}/Include")
    target_link_libraries(battle PRIVATE user32 gdi32 vulkan-1)
else()
    target_link_libraries(battle PRIVATE X11 Xrandr Xi Xpresent rt vulkan)
endif()

# webgpu
file(GLOB SRC "${CMAKE_SOURCE_DIR}/../webgpu/*.c")
add_executable(webgpu ${SRC})
if(WIN32)
    target_link_directories(webgpu PRIVATE "${CMAKE_SOURCE_DIR}/../webgpu")
    target_link_libraries(webgpu PRIVATE user32 gdi32 wgpu_native)
else()
    target_link_directories(webgpu PRIVATE "${CMAKE_SOURCE_DIR}/../webgpu")
    target_link_libraries(webgpu PRIVATE X11 wgpu_native)
endif()

# vk
file(GLOB SRC "${CMAKE_SOURCE_DIR}/../vk/*.c")
add_executable(vk ${SRC})
if(WIN32)
    target_include_directories(battle PRIVATE "$ENV{VULKAN_SDK}/Include")
    target_link_libraries(vk PRIVATE user32 gdi32 wgpu_native)
else()
    target_link_libraries(vk PRIVATE X11 wgpu_native)
endif()
