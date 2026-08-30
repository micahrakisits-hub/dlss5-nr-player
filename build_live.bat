@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /EHsc /O2 /MT nr_live.cpp /link /OUT:nr_live.exe d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib user32.lib
