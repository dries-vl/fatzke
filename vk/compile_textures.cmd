#!/usr/bin/env sh
: '
@echo off
setlocal EnableDelayedExpansion

for %%F in (data\*.png) do (
    set "in=%%F"
    set "name=%%~nF"
    set "out=static\win32\%%~nF.obj"

    rem build if missing
    if not exist "!out!" (
        call :build "%%F" "%%~nF"
    ) else (
        rem build if input newer than output
        for %%O in ("!out!") do (
            if "%%~tF" GTR "%%~tO" call :build "%%F" "%%~nF"
        )
    )
)
goto :eof

:build
echo Building %~1
toktx --t2 --encode astc --astc_blk_d 6x6 --srgb --genmipmap ^
    "static/win32/%~2.ktx2" "%~1"
if errorlevel 1 exit /b 1

objcopy -I binary -O elf64-x86-64 ^
    --rename-section .data=.rdata,alloc,load,readonly,data,contents ^
    "static/win32/%~2.ktx2" "static/win32/%~2.obj" ^
    --redefine-sym _binary_static_win32_%~2_ktx2_start=%~2 ^
    --redefine-sym _binary_static_win32_%~2_ktx2_end=%~2_end
if errorlevel 1 exit /b 1

del "static\win32\%~2.ktx2"
exit /b 0
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
