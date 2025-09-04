#!/usr/bin/env sh
: '
slangc shaders.slang -entry VS_Tri -entry PS_Tri -entry VS_Blit -entry PS_Blit -target spirv -fvk-use-entrypoint-name -o static/shaders.spv
xxd -i static/shaders.spv > static/shaders.h
del static/shaders.spv
tcc *.c -run -luser32 -lgdi32 -lvulkan-1 -I"%VULKAN_SDK%\Include" -L"%VULKAN_SDK%\Lib"
goto :eof
'
tcc *.c -run -lwayland-client -lvulkan
