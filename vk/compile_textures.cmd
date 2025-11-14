#!/usr/bin/env sh
: '
@echo off
toktx --t2 --encode astc --astc_blk_d 12x12 --srgb --genmipmap static/win32/font_atlas.ktx2 data/font_atlas.png
objcopy -I binary -O elf64-x86-64 --rename-section .data=.rdata,alloc,load,readonly,data,contents static/win32/font_atlas.ktx2 static/win32/font_atlas.obj ^
 --redefine-sym _binary_win32_font_atlas_ktx2_start=font_atlas ^
 --redefine-sym _binary_win32_font_atlas_ktx2_end=font_atlas_end
del static\win32\font_atlas.ktx2
goto :eof
'
toktx --t2 --encode astc --astc_blk_d 12x12 --srgb --genmipmap static/linux/font_atlas.ktx2 data/font_atlas.png
objcopy -I binary -O default --rename-section .data=.rodata,alloc,load,readonly,data,contents static/linux/font_atlas.ktx2 static/linux/font_atlas.o \
--redefine-sym _binary_static_linux_font_atlas_ktx2_start=font_atlas \
--redefine-sym _binary_static_linux_font_atlas_ktx2_end=font_atlas_end
rm static/linux/font_atlas.ktx2
