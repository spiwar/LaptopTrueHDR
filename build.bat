@echo off
REM Build hdr-laptop-calibration.exe with MSVC.
REM Requires Visual Studio 2022 (or Build Tools) with the "Desktop development
REM with C++" workload and a Windows 10/11 SDK for the C++/WinRT headers.

setlocal

REM Skip the toolchain lookup if we are already in a developer prompt.
if defined VSINSTALLDIR goto :compile

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\vspath.txt" 2>nul
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

:compile
where cl >nul 2>nul
if errorlevel 1 (
    echo [-] cl.exe is not on PATH. The VC++ environment was not set up.
    exit /b 1
)

cd /d "%~dp0"

REM /MT links the CRT statically, so the exe runs without the VC++ redistributable.
cl /nologo /W3 /O2 /MT /std:c++17 /EHsc /permissive- ^
   main.cpp ^
   /Fe:hdr-laptop-calibration.exe
if errorlevel 1 exit /b 1

echo [+] Build succeeded: hdr-laptop-calibration.exe
