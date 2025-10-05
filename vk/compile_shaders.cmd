#!/usr/bin/env sh

slangc shaders.slang \
  -target spirv \
  -fvk-use-entrypoint-name \
  -fvk-use-gl-layout \
  -entry cs_instance \
  -entry cs_prepare \
  -entry cs_meshlet \
  -entry vs_main \
  -entry fs_main \
  -o static/shaders.spv

