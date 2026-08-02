@echo off
REM Build hdr-laptop-calibration.exe with MSVC.
REM Requires Visual Studio 2022 (or Build Tools) with the "Desktop development
REM with C++" workload and a Windows 10/11 SDK for the C++/WinRT headers.

setlocal

REM Skip the toolchain lookup if we are already in a developer prompt.
if defined VSINSTALLDIR goto :compile

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [-] vswhere.exe not found. Is Visual Studio installed?
    exit /b 1
)

REM Read vswhere's output via a temp file. A `for /f` backtick command would be
REM parsed wrong here: the ")" in "%ProgramFiles(x86)%" closes the "in (...)"
REM group early, silently leaving the wrong compiler on PATH.
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\vspath.txt"
set "VSPATH="
set /p VSPATH=<"%TEMP%\vspath.txt"
del "%TEMP%\vspath.txt" 2>nul
if not defined VSPATH (
    echo [-] No Visual Studio installation with the C++ toolset was found.
    exit /b 1
)

REM stderr is discarded: vcvars64.bat prints a harmless "vswhere.exe is not
REM recognized" on some installs while still configuring the environment
REM correctly. The "where cl" check below is what actually verifies it worked.
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if errorlevel 1 (
    echo [-] Failed to initialize the VC++ environment.
    exit /b 1
)

:compile
where cl >nul 2>nul
if errorlevel 1 (
    echo [-] cl.exe is not on PATH. The VC++ environment was not set up.
    exit /b 1
)

cd /d "%~dp0"

REM /std:c++17   minimum required by C++/WinRT
REM /EHsc        standard C++ exception model
REM /permissive- strict conformance, recommended for C++/WinRT
REM /MT          static CRT, so the exe runs without the VC++ redistributable
cl /nologo /W3 /O2 /MT /std:c++17 /EHsc /permissive- ^
   main.cpp ^
   /Fe:hdr-laptop-calibration.exe
if errorlevel 1 (
    echo.
    echo [-] Build failed.
    exit /b 1
)

echo.
echo [+] Build succeeded: hdr-laptop-calibration.exe
endlocal
