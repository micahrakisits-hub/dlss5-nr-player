@echo off
setlocal
cd /d "%~dp0"
rem Set VCVARS64 explicitly for a portable or custom installation.
if defined VCVARS64 goto configure
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS64=%%I\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS64 (
    echo ERROR: MSVC C++ Build Tools not found. Install Desktop development with C++ and a Windows SDK.
    exit /b 1
)
:configure
if not exist "%VCVARS64%" (
    echo ERROR: vcvars64.bat not found at "%VCVARS64%".
    exit /b 1
)
call "%VCVARS64%"
if errorlevel 1 exit /b 1
cl /nologo /EHsc /O2 /MT nr_player.cpp /link /OUT:nr_player.exe d3d12.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib winmm.lib comctl32.lib comdlg32.lib shell32.lib dwmapi.lib uxtheme.lib
exit /b %errorlevel%
