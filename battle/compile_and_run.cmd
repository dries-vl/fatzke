#!/usr/bin/env sh
: '
@echo off
tcc *.c static/win32/*.obj -luser32 -lgdi32 -lvulkan-1 -I"%VULKAN_SDK%\Include" -L"%VULKAN_SDK%\Lib" -run
goto :eof
'
# one-liner you can put in a .desktop Exec=
VT=3;
HOME_VT=$(sed -n 's/^tty\([0-9][0-9]*\).*/\1/p' /sys/class/tty/tty0/active)
systemd-run --user --collect -p TTYPath=/dev/tty$VT -p StandardInput=tty -p StandardOutput=tty -p StandardError=tty -E MYGAME_HOME_VT="$HOME_VT" tcc -O2 -run *.c static/linux/*.o -ldrm -lvulkan -- "$@"
chvt $VT
