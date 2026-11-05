@echo off
rem Builds sss_lab.exe into %1 (default: the current directory) with the
rem VS 2022 Build Tools. No dependencies.
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /std:c++20 /permissive- /arch:AVX2 /EHsc /W3 /DNDEBUG "%~dp0sss_lab.cpp" /Fo"%OUT%\\" /Fe"%OUT%\sss_lab.exe"
