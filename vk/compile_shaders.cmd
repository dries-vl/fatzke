#!/usr/bin/env sh
: '
@echo off
slangc shaders.slang ^
  -target spirv ^
  -fvk-use-entrypoint-name ^
  -fvk-use-gl-layout ^
  -entry cs_build_visible ^
  -entry cs_prepare_indirect ^
  -entry vs_main ^
  -entry fs_main ^
  -o static/win32/shaders.spv
objcopy -I binary -O elf64-x86-64 --rename-section .data=.rdata,alloc,load,readonly,data,contents static/win32/shaders.spv static/win32/shaders.obj ^
 --redefine-sym _binary_static_win32_shaders_spv_start=shaders ^
 --redefine-sym _binary_static_win32_shaders_spv_end=shaders_end
del static\win32\shaders.spv
goto :eof
'
slangc shaders.slang \
  -target spirv \
  -fvk-use-entrypoint-name \
  -fvk-use-gl-layout \
  -entry cs_build_visible \
  -entry cs_prepare_indirect \
  -entry vs_main \
  -entry fs_main \
  -o static/linux/shaders.spv
objcopy -I binary -O default --rename-section .data=.rodata,alloc,load,readonly,data,contents static/linux/shaders.spv static/linux/shaders.o \
 --redefine-sym _binary_static_linux_shaders_spv_start=shaders \
 --redefine-sym _binary_static_linux_shaders_spv_end=shaders_end
rm static/linux/shaders.spv

