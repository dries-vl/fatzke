#!/usr/bin/env sh
: '
@echo off
~/Downloads/KTX-Software-4.4.0-Linux-x86_64/bin/toktx --t2 --encode astc --astc_blk_d 12x12 --genmipmap static/font_atlas.ktx2 data/font_atlas.png
goto :eof
'
~/Downloads/KTX-Software-4.4.0-Linux-x86_64/bin/toktx --t2 --encode astc --astc_blk_d 12x12 --genmipmap static/font_atlas.ktx2 data/font_atlas.png
~/Downloads/compressonatorcli-4.5.52-Linux/compressonatorcli -fd BC1 -mipsize 1 -fx KTX data/font_atlas.png font_atlas.dds
