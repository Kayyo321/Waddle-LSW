@echo off
setlocal
rem Build the first-party WinRT adapter with the base Windows SDK; no downloads.
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "av_vs_install=%%i"
if not defined av_vs_install exit /b 1
call "%av_vs_install%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
if not exist build mkdir build
cl /nologo /std:c++20 /EHsc /W4 /WX /MT /LD /Isrc/av src/av/av_wgc.cpp /Fobuild\av_wgc.obj /Febuild\av_wgc.dll /link d3d11.lib runtimeobject.lib windowsapp.lib user32.lib
exit /b %errorlevel%
