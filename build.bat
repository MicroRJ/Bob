@echo off
setlocal

if not exist selfhost\bob.exe (
    echo Missing selfhost\bob.exe.
    exit /b 1
)

selfhost\bob.exe build.elf %*
exit /b %errorlevel%
