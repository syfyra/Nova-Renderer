@echo off
rem Builds the OptiX denoise spike with the VS 2022 Build Tools.
rem   build_spike.bat <output_dir>        -> optix_denoise_spike.exe
rem   build_spike.bat <output_dir> dll    -> optix_denoise_spike.dll (spike_main, for hython/ctypes)
rem Needs: CUDA 12.9 headers (cuda.h only; nvcuda.dll is loaded at run time,
rem cuda.lib is NOT linked), the OptiX 9.1 headers that the Nova configure
rem downloaded into build\vs2022\_deps, Houdini's OpenImageIO/Imath.
rem Run the exe with %HFS%\bin on PATH (OpenImageIO_sidefx.dll).
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%TEMP%\optix_spike
if not exist "%OUT%" mkdir "%OUT%"
set HFS=C:\Program Files\Side Effects Software\Houdini 22.0.466
set CUDA=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9
set HERE=%~dp0
set OPTIX=%HERE%..\..\..\..\build\vs2022\_deps\nova_optix_headers-src\include
set MODE=/Fe"%OUT%\optix_denoise_spike.exe"
if /I "%~2"=="dll" set MODE=/DSPIKE_DLL /LD /Fe"%OUT%\optix_denoise_spike.dll"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /MD /W3 ^
   /I"%CUDA%\include" /I"%OPTIX%" /I"%HFS%\toolkit\include" ^
   "%HERE%optix_denoise_spike.cpp" /Fo"%OUT%\\" %MODE% ^
   /link /LIBPATH:"%HFS%\custom\houdini\dsolib" OpenImageIO_sidefx.lib OpenImageIO_Util_sidefx.lib Imath_sidefx.lib advapi32.lib
exit /b %ERRORLEVEL%
