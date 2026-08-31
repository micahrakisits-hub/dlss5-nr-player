@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d E:\Work\nr-video
cl /nologo /EHsc /std:c++17 /O2 /Fe:nr_worker.exe nr_worker.cpp /link d3d11.lib d3d12.lib dxgi.lib user32.lib ole32.lib
if %errorlevel% neq 0 exit /b 1
echo BUILT nr_worker.exe
