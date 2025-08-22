#!/usr/bin/env sh
: '
@echo off
tcc main.c -run -lwayland-client
goto :eof
'
exec tcc main.c -run
