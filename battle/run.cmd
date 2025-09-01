#!/usr/bin/env sh
: '
@echo off
tcc *.c -run -luser32 -lgdi32 -lvulkan-1 -I"%VULKAN_SDK%\Include" -L"%VULKAN_SDK%\Lib"
goto :eof
'
tcc *.c -run -lwayland-client -lvulkan
