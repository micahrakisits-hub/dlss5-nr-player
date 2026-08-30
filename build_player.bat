@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /EHsc /O2 /MT nr_player.cpp /link /OUT:nr_player.exe d3d12.lib dxgi.lib d3dcompiler.lib user32.lib winmm.lib comctl32.lib
