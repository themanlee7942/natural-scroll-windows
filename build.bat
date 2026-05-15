@echo off
REM Build scroll_toggle.exe using MinGW-w64 gcc (run from MSYS2 / w64devkit).
REM If you don't have gcc on Windows, install MSYS2 then `pacman -S mingw-w64-x86_64-gcc`.

setlocal
set OUT=bin\scroll_toggle.exe

if not exist bin mkdir bin

windres app.rc -O coff -o app.res.o || goto :err
gcc -O2 -Wall -mwindows -municode ^
    scroll_toggle.c app.res.o ^
    -lsetupapi -ladvapi32 -luser32 -lcfgmgr32 ^
    -o "%OUT%" || goto :err

REM Strip symbols to shrink the binary.
strip "%OUT%" 2>nul

echo.
echo  Built %OUT%
exit /b 0

:err
echo.
echo  Build failed.
exit /b 1
