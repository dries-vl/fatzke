#!/usr/bin/env sh
: '
@echo off
toktx --t2 --encode astc --astc_blk_d 12x12 --genmipmap ../static/font_atlas.ktx2 font_atlas.png
xxd -i ../static/font_atlas.ktx2 > ../static/font_atlas.h
del ..\static\font_atlas.ktx2
goto :eof
'