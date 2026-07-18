@echo off
setlocal

call vcvars64 >nul
if errorlevel 1 exit /b %errorlevel%

if not exist build mkdir build

if not exist elf\build\elf.lib (
    echo Missing elf\build\elf.lib. Run elf\build.bat lib first.
    exit /b 1
)

echo Building Bob...
clang-cl /nologo /std:c11 /W4 /WX /Zi /Od src\base.c src\logger.c src\profiler.c src\graph.c src\executor.c src\c_include_scan.c src\elf_adapter.c src\platform_win32.c src\vcvars_cache.c src\bob.c ^
    /Isrc ^
    /Ielf\include /Ielf\src\base /Ielf\src\api /Ielf\src\core ^
    /Ielf\src\core\table /Ielf\src\core\atom /Ielf\src\platform ^
    /Ielf\src\vm /Ielf\src\libs /Ielf\src\compiler ^
    /Ielf\src\compiler\frontend /Ielf\src\compiler\middle ^
    /Ielf\src\compiler\backend /Febuild\bob.exe ^
    /link elf\build\elf.lib
if errorlevel 1 exit /b %errorlevel%

echo Building tests...
clang-cl /nologo /std:c11 /W4 /WX /Zi /Od src\base.c src\logger.c src\profiler.c src\graph.c src\executor.c src\c_include_scan.c src\elf_adapter.c src\platform_win32.c src\vcvars_cache.c tests\tests.c ^
    /Isrc ^
    /Ielf\include /Ielf\src\base /Ielf\src\api /Ielf\src\core ^
    /Ielf\src\core\table /Ielf\src\core\atom /Ielf\src\platform ^
    /Ielf\src\vm /Ielf\src\libs /Ielf\src\compiler ^
    /Ielf\src\compiler\frontend /Ielf\src\compiler\middle ^
    /Ielf\src\compiler\backend /Febuild\tests.exe ^
    /link elf\build\elf.lib
if errorlevel 1 exit /b %errorlevel%

echo Running tests...
build\tests.exe --test
if errorlevel 1 exit /b %errorlevel%

if /i "%~1"=="example" build\tests.exe --build-example
if /i "%~1"=="tasks" build\tests.exe --build-task-table
exit /b %errorlevel%
