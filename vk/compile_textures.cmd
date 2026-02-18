#!/usr/bin/env sh
: '
@echo off
setlocal EnableDelayedExpansion

for %%f in (data\*.png) do (
    set "name=%%~nf"
    set "in=%%f"
    set "out=static\win32\%%~nf.obj"

    rem Build if output is missing OR input is newer than output
    if not exist "!out!" goto build
    for %%o in ("!out!") do if "!in!" NEQ "" if "%%~tf" GTR "%%~to" goto build

    echo Skipping %%f (up to date)
    goto next

:build
    echo Building %%f
    toktx --t2 --encode astc --astc_blk_d 6x6 --srgb --genmipmap ^
        static/win32/%%~nf.ktx2 %%f

    objcopy -I binary -O elf64-x86-64 ^
        --rename-section .data=.rdata,alloc,load,readonly,data,contents ^
        static/win32/%%~nf.ktx2 static/win32/%%~nf.obj ^
        --redefine-sym _binary_static_win32_%%~nf_ktx2_start=%%~nf ^
        --redefine-sym _binary_static_win32_%%~nf_ktx2_end=%%~nf_end

    del static\win32\%%~nf.ktx2

:next
)
goto :eof
'

# Linux portion
for img in data/*.png; do
    [ -e "$img" ] || continue
    base="$(basename "$img" .png)"
    out="static/linux/${base}.o"

    # Build if output is missing OR input is newer than output
    if [ ! -e "$out" ] || [ "$img" -nt "$out" ]; then
        echo "Building $img"
        toktx --t2 --encode astc --astc_blk_d 6x6 --srgb --genmipmap \
            "static/linux/${base}.ktx2" "$img"

        objcopy \
            -I binary -O default \
            --rename-section .data=.rodata,alloc,load,readonly,data,contents \
            "static/linux/${base}.ktx2" \
            "static/linux/${base}.o" \
            --redefine-sym "_binary_static_linux_${base}_ktx2_start=${base}" \
            --redefine-sym "_binary_static_linux_${base}_ktx2_end=${base}_end"

        rm "static/linux/${base}.ktx2"
    else
        echo "Skipping $img (up to date)"
    fi
done