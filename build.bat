@echo off
setlocal

if not exist blessed\bob.exe (
    echo Missing blessed\bob.exe.
    exit /b 1
)

blessed\bob.exe build.elf %*
exit /b %errorlevel%
