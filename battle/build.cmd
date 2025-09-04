#!/usr/bin/env sh
: '
@echo off
gcc *.c -o main.exe -O3 -luser32 -lgdi32 -lvulkan-1 -I"%VULKAN_SDK%\Include" -L"%VULKAN_SDK%\Lib"
goto :eof
'
gcc *.c -o main.exe -O3 -lwayland-client -lvulkan
