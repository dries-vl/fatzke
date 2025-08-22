#!/usr/bin/env sh
: '
@echo off
tcc main.c -run || (echo [ERROR] tcc failed & pause & exit /b 1)
goto :eof
'
exec tcc main.c -run
