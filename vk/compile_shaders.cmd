#!/usr/bin/env sh

slangc shaders.slang \
  -target spirv \
  -fvk-use-entrypoint-name \
  -fvk-use-gl-layout \
  -entry cs_build_visible \
  -entry cs_prepare_indirect \
  -entry vs_main \
  -entry fs_main \
  -o static/shaders.spv

