#!/usr/bin/env sh
: '
@echo off
tcc *.c static/win32/*.obj -luser32 -lgdi32 -lvulkan-1 -I"%VULKAN_SDK%\Include" -L"%VULKAN_SDK%\Lib" -run
goto :eof
'
tcc *.c static/linux/*.o -lwayland-client -lvulkan -run
