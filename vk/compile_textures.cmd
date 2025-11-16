#!/usr/bin/env sh
: '
@echo off
for %%f in (data\*.png) do (
    set "name=%%~nf"

    toktx --t2 --encode astc --astc_blk_d 12x12 --srgb --genmipmap ^
        static/win32/%%~nf.ktx2 data/%%f

    objcopy -I binary -O elf64-x86-64 ^
        --rename-section .data=.rdata,alloc,load,readonly,data,contents ^
        static/win32/%%~nf.ktx2 static/win32/%%~nf.obj ^
        --redefine-sym _binary_win32_%%~nf_ktx2_start=%%~nf ^
        --redefine-sym _binary_win32_%%~nf_ktx2_end=%%~nf_end

    del static\win32\%%~nf.ktx2
)
goto :eof
'

# Linux portion
for img in data/*.png; do
    base="$(basename "$img" .png)"

    toktx --t2 --encode astc --astc_blk_d 12x12 --srgb --genmipmap \
        "static/linux/${base}.ktx2" "$img"

    objcopy \
        -I binary -O default \
        --rename-section .data=.rodata,alloc,load,readonly,data,contents \
        "static/linux/${base}.ktx2" \
        "static/linux/${base}.o" \
        --redefine-sym "_binary_static_linux_${base}_ktx2_start=${base}" \
        --redefine-sym "_binary_static_linux_${base}_ktx2_end=${base}_end"

    rm "static/linux/${base}.ktx2"
done