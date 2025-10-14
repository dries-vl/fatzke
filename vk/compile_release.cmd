#!/usr/bin/env sh
: '
@echo off
gcc *.c static/win32/*.obj -luser32 -lgdi32 -lvulkan-1 -I"%VULKAN_SDK%\Include" -L"%VULKAN_SDK%\Lib" -O3 -DNDEBUG -march=x86-64-v3 -fomit-frame-pointer -flto -s -o vk.exe
goto :eof
'
gcc *.c static/linux/*.o -lwayland-client -lxkbcommon -lvulkan -L. -O3 -DNDEBUG -march=x86-64-v3 -fomit-frame-pointer -flto -s -o vk.exe
