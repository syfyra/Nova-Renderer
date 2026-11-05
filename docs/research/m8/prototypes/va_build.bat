@echo off
rem Builds va_volume_lab.exe into %1 (default: the current directory) with the
rem VS 2022 Build Tools and Houdini's header-only NanoVDB 32.6. /Zc:__cplusplus is required:
rem without it MSVC reports 199711L and GridBuilder.h falls back to std::result_of
rem (removed in C++20). Flags as nova_core (core/CMakeLists.txt).
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=.
if "%HFS%"=="" set "HFS=C:\Program Files\Side Effects Software\Houdini 22.0.466"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /std:c++20 /Zc:__cplusplus /permissive- /arch:AVX2 /EHsc /W3 /DNDEBUG /I"%HFS%\toolkit\include" "%~dp0va_volume_lab.cpp" /Fo"%OUT%\\" /Fe"%OUT%\va_volume_lab.exe"
