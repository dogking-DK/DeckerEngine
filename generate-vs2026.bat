@echo off
setlocal

rem Use the repository preset from any caller working directory.
pushd "%~dp0"
if errorlevel 1 exit /b 1

where.exe cmake.exe >nul 2>&1
if errorlevel 1 (
    echo [ERROR] CMake was not found. Install CMake 4.2 or newer and add it to PATH. >&2
    popd
    exit /b 1
)

cmake.exe --preset windows-dev
set "dk_configure_exit=%errorlevel%"
if not "%dk_configure_exit%"=="0" goto finish

echo.
if exist "out\build\windows-dev\DeckerEngine.slnx" (
    echo Generated solution: "%~dp0out\build\windows-dev\DeckerEngine.slnx"
) else if exist "out\build\windows-dev\DeckerEngine.sln" (
    echo Generated solution: "%~dp0out\build\windows-dev\DeckerEngine.sln"
) else (
    echo [ERROR] CMake finished but the expected solution was not found. >&2
    set "dk_configure_exit=1"
)

:finish
popd
exit /b %dk_configure_exit%
