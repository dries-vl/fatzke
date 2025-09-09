#!/usr/bin/env sh
: '
@echo off
toktx --t2 --encode astc --astc_blk_d 12x12 --srgb --genmipmap win32/font_atlas.ktx2 ../data/font_atlas.png
objcopy -I binary -O elf64-x86-64 --rename-section .data=.rdata,alloc,load,readonly,data,contents win32/font_atlas.ktx2 win32/font_atlas.obj ^
 --redefine-sym _binary_win32_font_atlas_ktx2_start=font_atlas ^
 --redefine-sym _binary_win32_font_atlas_ktx2_end=font_atlas_end
del win32\font_atlas.ktx2
goto :eof
'
toktx --t2 --encode astc --astc_blk_d 12x12 --srgb --genmipmap linux/font_atlas.ktx2 ../data/font_atlas.png
objcopy -I binary -O default --rename-section .data=.rodata,alloc,load,readonly,data,contents linux/font_atlas.ktx2 linux/font_atlas.o \
--redefine-sym _binary_linux_font_atlas_ktx2_start=font_atlas \
--redefine-sym _binary_linux_font_atlas_ktx2_end=font_atlas_end
rm linux/font_atlas.ktx2
