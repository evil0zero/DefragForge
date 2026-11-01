@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

rem Optional overrides: DEFRAGFORGE_TOOLSET, DEFRAGFORGE_TARGET, DEFRAGFORGE_PLATFORM.
set "BUILD_CONFIGURATION=Release"
if /I "%~1"=="Debug" set "BUILD_CONFIGURATION=Debug"

set "OSVER="
for /f "tokens=2 delims=[]" %%A in ('ver') do set "OSVER=%%A"
for /f "tokens=2 delims= " %%A in ("!OSVER!") do set "OSVER=%%A"
for /f "tokens=1,2 delims=." %%A in ("!OSVER!") do (
    set "OSMAJOR=%%A"
    set "OSMINOR=%%B"
)
set "TARGET=0x0501"
set "TOOLSET=v100"
if "!OSMAJOR!"=="6" (
    set "TARGET=0x0600"
    if "!OSMINOR!"=="1" set "TARGET=0x0601"
    if "!OSMINOR!"=="2" set "TARGET=0x0602"
    if "!OSMINOR!"=="3" set "TARGET=0x0603"
    if NOT "!OSMINOR!"=="0" set "TOOLSET=v141"
)
if "!OSMAJOR!"=="10" (
    set "TARGET=0x0A00"
    set "TOOLSET=v143"
)
if defined DEFRAGFORGE_TARGET set "TARGET=%DEFRAGFORGE_TARGET%"
if defined DEFRAGFORGE_TOOLSET set "TOOLSET=%DEFRAGFORGE_TOOLSET%"

set "ARCH=Win32"
if /I "%PROCESSOR_ARCHITECTURE%"=="AMD64" set "ARCH=x64"
if /I "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "ARCH=x64"
if /I "%PROCESSOR_ARCHITEW6432%"=="AMD64" set "ARCH=x64"
if defined DEFRAGFORGE_PLATFORM set "ARCH=%DEFRAGFORGE_PLATFORM%"

echo Host Windows version: !OSVER!
echo Compile-time target: !TARGET!
echo Toolset: !TOOLSET!, Architecture: !ARCH!, Configuration: !BUILD_CONFIGURATION!

set "MSBUILD=MSBuild.exe"
where MSBuild.exe >nul 2>&1
if errorlevel 1 (
    if exist "%WINDIR%\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe" (
        set "MSBUILD=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe"
    ) else (
        echo ERROR: Visual Studio or Microsoft Build Tools must be installed.
        exit /b 2
    )
)
"!MSBUILD!" DefragForge.sln /m /t:Build /p:Configuration=!BUILD_CONFIGURATION! /p:Platform=!ARCH! /p:DefragTarget=!TARGET! /p:DefragToolset=!TOOLSET! /nologo
if errorlevel 1 (
    echo Build failed. Install the matching toolset or set DEFRAGFORGE_TOOLSET.
    exit /b 1
)
echo Build complete: out\!ARCH!\!BUILD_CONFIGURATION!\DefragForge.exe
exit /b 0
