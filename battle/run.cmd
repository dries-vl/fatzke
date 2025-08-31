#!/usr/bin/env sh
: '
@echo off
tcc main.c -run -luser32 -lgdi32 -lvulkan-1 -I"%VULKAN_SDK%\Include" -L"%VULKAN_SDK%\Lib"
goto :eof
'
tcc main.c -run -lwayland-client -lvulkan
