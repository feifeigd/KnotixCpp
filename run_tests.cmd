@echo off
rem One-click unit test runner: configure, build and run knotix_tests.
rem Usage: run_tests.cmd [Debug|Release]   (default: Debug)
setlocal EnableExtensions

set "SCRIPT_DIR=%~dp0"
rem %~dp0 ends with a backslash, which would escape closing quotes later.
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "BUILD_DIR=%SCRIPT_DIR%\build"
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"

rem --- locate CMake and CTest --------------------------------------------
where cmake >nul 2>&1
if %ERRORLEVEL%==0 (
    set "CMAKE=cmake"
    set "CTEST=ctest"
) else if exist "C:\Program Files\CMake\bin\cmake.exe" (
    set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
    set "CTEST=C:\Program Files\CMake\bin\ctest.exe"
) else (
    set "CMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    set "CTEST=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
)

rem --- MSVC environment ---------------------------------------------------
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
    echo Failed to initialize the Visual Studio environment.
    exit /b 1
)

rem --- configure ----------------------------------------------------------
echo === Configuring (%CONFIG%) ===
"%CMAKE%" -S "%SCRIPT_DIR%" -B "%BUILD_DIR%"
if errorlevel 1 (
    echo Configure failed; retrying with vcpkg manifest install disabled...
    "%CMAKE%" -S "%SCRIPT_DIR%" -B "%BUILD_DIR%" -DVCPKG_MANIFEST_INSTALL=OFF
    if errorlevel 1 exit /b 1
)

rem --- build --------------------------------------------------------------
echo === Building knotix_tests (%CONFIG%) ===
"%CMAKE%" --build "%BUILD_DIR%" --config %CONFIG% --target knotix_tests
if errorlevel 1 exit /b 1

rem --- run -----------------------------------------------------------------
rem The test executable needs the CAF/CURL DLLs from vcpkg on PATH.
if /i "%CONFIG%"=="Debug" (
    set "PATH=%BUILD_DIR%\vcpkg_installed\x64-windows\debug\bin;%PATH%"
) else (
    set "PATH=%BUILD_DIR%\vcpkg_installed\x64-windows\bin;%PATH%"
)
echo === Running tests ===
"%CTEST%" --test-dir "%BUILD_DIR%" -C %CONFIG% --output-on-failure
