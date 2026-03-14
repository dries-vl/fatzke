#!/usr/bin/env sh
: '
@echo off
setlocal EnableDelayedExpansion

for %%F in (data\*.png) do (
    set "in=%%F"
    set "name=%%~nF"

    echo %%~nF | findstr /R "_data$" >nul
    if not errorlevel 1 (
        set "out=static\win32\%%~nF.obj"
        if not exist "!out!" (
            call :build_raw "%%F" "%%~nF"
        ) else (
            for %%O in ("!out!") do (
                if "%%~tF" GTR "%%~tO" call :build_raw "%%F" "%%~nF"
            )
        )
    ) else (
        set "out=static\win32\%%~nF.obj"
        if not exist "!out!" (
            call :build_ktx "%%F" "%%~nF"
        ) else (
            for %%O in ("!out!") do (
                if "%%~tF" GTR "%%~tO" call :build_ktx "%%F" "%%~nF"
            )
        )
    )
)
goto :eof

:build_ktx
echo Building compressed %~1
toktx --t2 --encode astc --astc_blk_d 4x4 --srgb --genmipmap ^
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

:build_raw
echo Building raw %~1
magick "%~1" -channel R -separate -depth 8 "gray:static/win32/%~2.raw"
if errorlevel 1 exit /b 1

objcopy -I binary -O elf64-x86-64 ^
    --rename-section .data=.rdata,alloc,load,readonly,data,contents ^
    "static/win32/%~2.raw" "static/win32/%~2.obj" ^
    --redefine-sym _binary_static_win32_%~2_raw_start=%~2 ^
    --redefine-sym _binary_static_win32_%~2_raw_end=%~2_end
if errorlevel 1 exit /b 1

del "static\win32\%~2.raw"
exit /b 0
'

# Linux portion
for img in data/*.png; do
    [ -e "$img" ] || continue

    base="$(basename "$img" .png)"
    out="static/linux/${base}.o"

    case "$base" in
        *_data)
            kind=raw
            ;;
        *)
            kind=ktx
            ;;
    esac

    if [ ! -e "$out" ] || [ "$img" -nt "$out" ]; then
        echo "Building $img"

        if [ "$kind" = raw ]; then
            raw="static/linux/${base}.raw"

            magick "$img" -channel R -separate -depth 8 "gray:${raw}"

            objcopy \
                -I binary -O default \
                --rename-section .data=.rodata,alloc,load,readonly,data,contents \
                "$raw" "$out" \
                --redefine-sym "_binary_static_linux_${base}_raw_start=${base}" \
                --redefine-sym "_binary_static_linux_${base}_raw_end=${base}_end" \
                || exit 1

            rm "$raw"
        else
            ktx="static/linux/${base}.ktx2"

            toktx --t2 --encode astc --astc_blk_d 4x4 --srgb --genmipmap \
                "$ktx" "$img" || exit 1

            objcopy \
                -I binary -O default \
                --rename-section .data=.rodata,alloc,load,readonly,data,contents \
                "$ktx" "$out" \
                --redefine-sym "_binary_static_linux_${base}_ktx2_start=${base}" \
                --redefine-sym "_binary_static_linux_${base}_ktx2_end=${base}_end" \
                || exit 1

            rm "$ktx"
        fi
    else
        echo "Skipping $img (up to date)"
    fi
done