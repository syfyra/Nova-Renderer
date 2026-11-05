@echo off
rem Builds the cw_ probes (docs/research/m10/cuda-wavefront.md) into %1
rem (default: %TEMP%\nova_cw_bin, outside the repository) with CUDA 12.9 for sm_120 and the
rem VS 2022 Build Tools as nvcc's host compiler. %2 selects one probe
rem (vm, vm_nofma, compact, sort, tex, vol, sizes, film, headers); default: all.
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%TEMP%\nova_cw_bin
if not exist "%OUT%" mkdir "%OUT%"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set NVCC="C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9\bin\nvcc.exe"
set ROOT=%~dp0..\..\..\..
set HINC=C:\Program Files\Side Effects Software\Houdini 22.0.466\toolkit\include
rem CUDA 12.9's CCCL does not compile CUB for sm_100+ on Windows (an inline-asm
rem "l" constraint on a 32-bit long in cuda/__ptx/.../clusterlaunchcontrol.h):
rem translation units with CUB are compiled from compute_90 PTX to sm_120 SASS.
set ARCH=-arch=sm_120
set CUBARCH=-gencode arch=compute_90,code=sm_120
set COMMON=-O3 -lineinfo -Xptxas -v -Xcompiler "/O2 /arch:AVX2 /utf-8 /EHsc" -DNOMINMAX -D_USE_MATH_DEFINES -I"%ROOT%"
set FLAGS=%COMMON% -std=c++20
rem NanoVDB 32.6's host-side builder (util/GridBuilder.h) uses std::result_of,
rem removed in C++20, and its CUDA helpers need extended lambdas: the volume
rem probe builds as C++17.
set FLAGS17=%COMMON% -std=c++17 --extended-lambda
set ONE=%~2
if "%ONE%"=="" goto all
goto %ONE%
:all
:vm
%NVCC% %FLAGS% %ARCH% "%~dp0cw_vm_probe.cu" -o "%OUT%\cw_vm_probe.exe" || exit /b 1
if not "%ONE%"=="" exit /b 0
:vm_nofma
%NVCC% %FLAGS% %ARCH% --fmad=false "%~dp0cw_vm_probe.cu" -o "%OUT%\cw_vm_probe_nofma.exe" || exit /b 1
if not "%ONE%"=="" exit /b 0
:compact
%NVCC% %FLAGS% %CUBARCH% "%~dp0cw_compact_probe.cu" -o "%OUT%\cw_compact_probe.exe" || exit /b 1
if not "%ONE%"=="" exit /b 0
:sort
%NVCC% %FLAGS% %CUBARCH% "%~dp0cw_sort_probe.cu" -o "%OUT%\cw_sort_probe.exe" || exit /b 1
if not "%ONE%"=="" exit /b 0
:tex
%NVCC% %FLAGS% %ARCH% "%~dp0cw_tex_probe.cu" -o "%OUT%\cw_tex_probe.exe" || exit /b 1
if not "%ONE%"=="" exit /b 0
:vol
%NVCC% %FLAGS17% %ARCH% -I"%HINC%" "%~dp0cw_vol_probe.cu" -o "%OUT%\cw_vol_probe.exe" || exit /b 1
if not "%ONE%"=="" exit /b 0
:sizes
%NVCC% %FLAGS% %ARCH% "%~dp0cw_sizes.cu" -o "%OUT%\cw_sizes.exe" || exit /b 1
if not "%ONE%"=="" exit /b 0
:film
%NVCC% %FLAGS% %ARCH% "%~dp0cw_film_probe.cu" -o "%OUT%\cw_film_probe.exe" || exit /b 1
if not "%ONE%"=="" exit /b 0
:headers
rem Compile only. The second command is expected to fail (core/volume/majorant.h).
%NVCC% -c %FLAGS% %ARCH% -Werror=cross-execution-space-call "%~dp0cw_header_check.cu" -o "%OUT%\cw_header_check.obj" || exit /b 1
echo cw_header_check: lights, light tree, LPE, phase, subsurface, hair, distribution compile as device code
%NVCC% -c %FLAGS% %ARCH% -Werror=cross-execution-space-call -I"%HINC%" -DCW_CHECK_MAJORANT "%~dp0cw_header_check.cu" -o "%OUT%\cw_header_check_majorant.obj"
exit /b 0
