#!/usr/bin/env sh
: '
@echo off
slangc ../shaders.slang -entry VS_Tri -entry PS_Tri -entry VS_Blit -entry PS_Blit -target spirv -fvk-use-entrypoint-name -o win32/shaders.spv
objcopy -I binary --rename-section .data=.rdata,alloc,load,readonly,data,contents win32/shaders.spv win32/shaders.obj \
 --redefine-sym _binary_win32_shaders_spv_start=shaders \
 --redefine-sym _binary_win32_shaders_spv_end=shaders_end
del win32/shaders.spv
goto :eof
'
slangc ../shaders.slang -entry VS_Tri -entry PS_Tri -entry VS_Blit -entry PS_Blit -target spirv -fvk-use-entrypoint-name -o linux/shaders.spv
objcopy -I binary -O default --rename-section .data=.rodata,alloc,load,readonly,data,contents linux/shaders.spv linux/shaders.o \
 --redefine-sym _binary_linux_shaders_spv_start=shaders \
 --redefine-sym _binary_linux_shaders_spv_end=shaders_end
rm linux/shaders.spv
