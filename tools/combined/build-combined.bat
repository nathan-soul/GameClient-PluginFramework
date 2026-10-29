@echo off
REM Combined build: the gencast client with the plugin host in one exe.
REM The tree must already be on the combined branch; this script only configures and builds, so a
REM failure is always the compiler's and never a surprise checkout.
REM
REM Mirrors build-capture.bat on purpose: same preset, same triplet, same runtime, same _60 suffix.
REM The only difference is the branch it is expected to be on, which is checked below.

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64
set VCPKG_ROOT=C:\vcpkg
set VCPKG_DEFAULT_BINARY_CACHE=C:\vcpkg-cache
set PATH=C:\Users\Dennis\AppData\Local\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe;%PATH%
cd /d C:\build\plugin-capture

for /f "delims=" %%b in ('git rev-parse --abbrev-ref HEAD') do set BRANCH=%%b
echo BRANCH=%BRANCH%
if not "%BRANCH%"=="combined/gencast-overlay" (
    echo WRONG_BRANCH - expected combined/gencast-overlay
    exit /b 1
)

cmake --preset win32-vcpkg -DCMAKE_MAKE_PROGRAM="C:\Users\Dennis\AppData\Local\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe\ninja.exe" -DVCPKG_TARGET_TRIPLET=x86-windows-static-md -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" -DRTS_BUILD_OUTPUT_SUFFIX=_60 -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_GENERALS=OFF -DRTS_BUILD_CORE_TOOLS=OFF -DRTS_BUILD_ZEROHOUR_TOOLS=OFF -DRTS_BUILD_CORE_EXTRAS=OFF -DRTS_BUILD_ZEROHOUR_EXTRAS=OFF > C:\build\plugin-capture\configure-combined.log 2>&1
echo CONFIGURE_EXIT=%ERRORLEVEL%
if errorlevel 1 goto fail

cmake --build --preset win32-vcpkg > C:\build\plugin-capture\build-combined.log 2>&1
echo BUILD_EXIT=%ERRORLEVEL%
if errorlevel 1 goto fail

echo BUILD_OK
exit /b 0

:fail
echo BUILD_FAIL
exit /b 1
