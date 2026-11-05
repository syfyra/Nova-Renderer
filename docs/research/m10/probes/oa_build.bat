@echo off
rem Builds the OptiX accel probe into %1 (default: the current directory):
rem   oa_device.optixir / oa_device.ptx   device programs (nvcc, OptiX-IR and PTX)
rem   oa_cuda_kernel.ptx                  a plain CUDA kernel for launch-overhead comparisons
rem   oa_probe.exe                        the host program (MSVC; loads nvcuda.dll and nvoptix.dll at run time)
rem Needs CUDA 12.x (CUDA_PATH or the default install), the VS 2022 Build Tools,
rem and the OptiX 9.1 headers (OPTIX_INC, default: Nova's fetched copy).
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=.
set HERE=%~dp0
if "%CUDA_PATH%"=="" set CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9
if "%OPTIX_INC%"=="" set OPTIX_INC=%HERE%..\..\..\..\build\vs2022\_deps\nova_optix_headers-src\include
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set NVCC="%CUDA_PATH%\bin\nvcc.exe" -arch=compute_75 -use_fast_math -std=c++17 -I"%OPTIX_INC%" -I"%HERE%."
%NVCC% -optix-ir -o "%OUT%\oa_device.optixir" "%HERE%oa_device.cu" || exit /b 1
%NVCC% -ptx -o "%OUT%\oa_device.ptx" "%HERE%oa_device.cu" || exit /b 1
"%CUDA_PATH%\bin\nvcc.exe" -arch=compute_75 -ptx -o "%OUT%\oa_cuda_kernel.ptx" "%HERE%oa_cuda_kernel.cu" || exit /b 1
cl /nologo /O2 /std:c++20 /permissive- /EHsc /W3 /DNDEBUG /I"%OPTIX_INC%" /I"%CUDA_PATH%\include" /I"%HERE%." ^
   "%HERE%oa_probe.cpp" /Fo"%OUT%\\" /Fe"%OUT%\oa_probe.exe" /link Cfgmgr32.lib Advapi32.lib || exit /b 1
