#!/usr/bin/env sh
: '
@echo off
tcc main.c -run -luser32 -lgdi32 -lvulkan
goto :eof
'
tcc main.c -run -lwayland-client -lvulkan
