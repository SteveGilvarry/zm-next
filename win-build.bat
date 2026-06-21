@echo off
REM ── zm-next Windows build driver (Phase 1 + DirectML) ────────────────────────
REM Sets up the MSVC build environment via vcvars64, then configures and builds
REM the Phase-1 subset (zmcore + selected plugins + gtests) with the DirectML EP.
REM Usage: win-build.bat [configure|build|test|all]   (default: all)

setlocal enableextensions

set "VS=C:\Program Files\Microsoft Visual Studio\2022\Professional"
set "VCVARS=%VS%\VC\Auxiliary\Build\vcvars64.bat"
set "VCPKG_ROOT=C:\Users\sgilvarry\Code\vcpkg"
set "SRC=C:\Users\sgilvarry\Code\zm-next"
set "BUILD=%SRC%\build"
set "ORT=C:\Users\sgilvarry\Code\ortdml\onnxruntime"
set "PORTABLE_CMAKE=C:\Users\sgilvarry\Code\cmake-portable\cmake-3.30.5-windows-x86_64\bin"

if not exist "%VCVARS%" (
  echo [win-build] ERROR: vcvars64.bat not found at "%VCVARS%" — MSVC C++ toolchain not installed yet.
  exit /b 90
)

call "%VCVARS%" || (echo [win-build] vcvars64 failed & exit /b 91)

REM Prefer the VS-bundled CMake/Ninja (installed with the C++ workload); fall back
REM to the portable CMake. Ninja comes from VS or vcpkg.
where cmake >nul 2>&1 || set "PATH=%PORTABLE_CMAKE%;%PATH%"
where cmake >nul 2>&1 || (echo [win-build] ERROR: cmake not found & exit /b 92)

REM Ensure a Ninja is on PATH for the Ninja generator (vcpkg's, else VS's).
where ninja >nul 2>&1 || set "PATH=%VCPKG_ROOT%\downloads\tools\ninja-1.13.2-windows;%PATH%"
where ninja >nul 2>&1 || set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
where ninja >nul 2>&1 || (echo [win-build] ERROR: ninja not found & exit /b 93)

set "ACTION=%~1"
if "%ACTION%"=="" set "ACTION=all"

if /i "%ACTION%"=="configure" goto :configure
if /i "%ACTION%"=="build"     goto :build
if /i "%ACTION%"=="test"      goto :test
if /i "%ACTION%"=="all"       goto :configure

:configure
echo [win-build] Configuring...
cmake -S "%SRC%" -B "%BUILD%" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Debug ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DONNXRUNTIME_ROOT=%ORT% ^
  -DZM_WITH_DIRECTML=ON ^
  || (echo [win-build] configure FAILED & exit /b 1)
if /i "%ACTION%"=="configure" goto :eof

:build
echo [win-build] Building...
cmake --build "%BUILD%" --parallel || (echo [win-build] build FAILED & exit /b 2)
if /i "%ACTION%"=="build" goto :eof

:test
echo [win-build] Running ctest...
ctest --test-dir "%BUILD%" --output-on-failure
goto :eof
