#!/usr/bin/env sh
: '
@echo off
gcc *.c static/win32/*.obj -O3 -luser32 -lgdi32 -lvulkan-1 -I"%VULKAN_SDK%\Include" -L"%VULKAN_SDK%\Lib" -o main.exe
goto :eof
'
gcc *.c static/linux/*.o -O3 -lwayland-client -lvulkan -o main
