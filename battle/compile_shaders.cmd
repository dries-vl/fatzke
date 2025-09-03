#!/usr/bin/env sh
: '
@echo off
slangc shaders.slang -entry VS_Tri -entry PS_Tri -entry VS_Blit -entry PS_Blit -target spirv -fvk-use-
entrypoint-name -o static/shaders.spv; xxd -i static/shaders.spv > static/shaders.h
goto :eof
'
slangc shaders.slang -entry VS_Tri -entry PS_Tri -entry VS_Blit -entry PS_Blit -target spirv -fvk-use-
entrypoint-name -o static/shaders.spv; xxd -i static/shaders.spv > static/shaders.h
