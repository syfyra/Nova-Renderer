# nova_core: the renderer itself. It must not depend on the HDK or USD so that
# it can be shared with the CUDA backend and standalone tools. Headers live
# next to their sources; includes are rooted at the repository, e.g.
# #include "core/math/vec.h".

add_library(nova_core STATIC
    math/mat.cpp
    math/distribution.cpp
    scene/scene.cpp
    scene/material.cpp
    scene/visibility.cpp
    shading/ir.cpp
    shading/compiler.cpp
    texture/memory_sampler.cpp
    texture/texel_stream.cpp
    texture/texture_budget.cpp
    integrator/material_eval.cpp
    integrator/shading_queries.cpp
    accel/embree_accel.cpp
    lights/lights.cpp
    lights/light_sampler.cpp
    lights/light_tree.cpp
    lights/sky.cpp
    integrator/wavefront.cpp
    integrator/camera_samples.cpp
    integrator/renderer.cpp
    integrator/guide_field.cpp
    film/film.cpp
    film/filter.cpp
    film/lpe_compile.cpp
    film/crypto.cpp
    geometry/subdiv.cpp
    geometry/displace.cpp
    geometry/curves.cpp
    volume/majorant.cpp
    volume/build.cpp
    volume/shell.cpp
    shading/interval.cpp
    integrator/volume_eval.cpp
    integrator/volume_media.cpp
    bsdf/tables/bsdf_tables.cpp
    bsdf/surface.cpp
    denoise/optix_denoiser.cpp
    bake/bake.cpp
    bake/mikktspace.cpp
    bake/image_filter.cpp
)

target_include_directories(nova_core PUBLIC "${PROJECT_SOURCE_DIR}")
target_compile_features(nova_core PUBLIC cxx_std_20)
# TBB, OpenSubdiv and NanoVDB come from Houdini (cmake/NovaHoudini.cmake) or,
# in a core-only build, from the system (cmake/NovaStandalone.cmake).
target_link_libraries(nova_core
    PUBLIC  Nova::TBB Nova::NanoVDB
    PRIVATE embree Nova::OpenSubdiv
)
if(NOVA_HAS_OPTIX)
    target_link_libraries(nova_core PRIVATE Nova::OptiX)
endif()

# The GPU backend (docs/design/M10.md D2): device programs compiled by nvcc
# and embedded in nova_core; the host code loads the driver and OptiX at run
# time, so Nova still loads without an NVIDIA driver.
option(NOVA_WITH_GPU "Build the GPU backend (CUDA + OptiX)" ON)
set(NOVA_HAS_GPU OFF)
if(NOVA_WITH_GPU AND NOVA_HAS_OPTIX)
    include(CheckLanguage)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        set(NOVA_HAS_GPU ON)
    endif()
endif()
if(NOVA_HAS_GPU)
    # The GPU architectures Nova XPU has code for (docs/design/M10.md, section 8), as compute
    # capabilities: 75 Turing (RTX 20, GTX 16), 80 Ampere, whose code every 8.x GPU runs (RTX 30,
    # RTX 40), 120 Blackwell (RTX 50). nvcc compiles architectures below 100 with another
    # compiler than those from 100 on, so the kernels are built once for each group that is
    # listed: as code for its architectures and as PTX for its oldest, which the driver compiles
    # for a GPU without code of its own (minutes, once per driver). The oldest architecture
    # listed is the oldest GPU Nova XPU runs on. A shorter list builds faster ("120": a third of
    # the time) and is for development only: tools/installer/build.ps1 refuses to package it.
    set(NOVA_GPU_ARCHS_RELEASE "75;80;120")
    # The architectures Nova XPU has run on: 120, the RTX 5080 it is developed on. A GPU that runs
    # the code of none of them (code for X.y runs on X.z with z >= y) is experimental: Nova XPU
    # renders on it only with NOVA_XPU_EXPERIMENTAL_GPUS set, which the installer's option does
    # (tools/installer). An architecture is added here once Nova XPU is checked on such a card;
    # tools/installer/build.ps1 hands the list to the installer.
    set(NOVA_GPU_ARCHS_TESTED "120")
    set(NOVA_GPU_ARCHS "${NOVA_GPU_ARCHS_RELEASE}" CACHE STRING
        "CUDA architectures the Nova XPU kernels are compiled for (release: ${NOVA_GPU_ARCHS_RELEASE})")
    set(_nova_archs ${NOVA_GPU_ARCHS})
    list(REMOVE_DUPLICATES _nova_archs)
    list(SORT _nova_archs COMPARE NATURAL)
    set(_nova_gencode "")
    foreach(_group IN ITEMS old new)
        set(_codes "")
        foreach(_arch IN LISTS _nova_archs)
            if(NOT _arch MATCHES "^[0-9]+$" OR _arch LESS 75)
                message(FATAL_ERROR "NOVA_GPU_ARCHS: '${_arch}' is not a compute capability from 75 on (${NOVA_GPU_ARCHS})")
            endif()
            if((_group STREQUAL "old" AND _arch LESS 100) OR (_group STREQUAL "new" AND NOT _arch LESS 100))
                list(APPEND _codes "${_arch}")
            endif()
        endforeach()
        if(_codes)
            list(GET _codes 0 _first)
            list(TRANSFORM _codes PREPEND "sm_")
            list(JOIN _codes "," _codes)
            list(APPEND _nova_gencode "--generate-code=arch=compute_${_first},code=[compute_${_first},${_codes}]")
        endif()
    endforeach()
    if(NOT _nova_gencode)
        message(FATAL_ERROR "NOVA_GPU_ARCHS is empty")
    endif()
    list(GET _nova_archs 0 NOVA_GPU_MIN_ARCH)
    list(JOIN _nova_archs "," _nova_arch_text)
    if(NOT "${_nova_archs}" STREQUAL "${NOVA_GPU_ARCHS_RELEASE}")
        message(WARNING "Nova: NOVA_GPU_ARCHS is ${_nova_arch_text}, not the release's ${NOVA_GPU_ARCHS_RELEASE}: "
                        "a development build of Nova XPU")
    endif()

    set(_nova_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
    # The OptiX programs, for the oldest architecture: OptiX compiles them for the device.
    add_library(nova_gpu_programs OBJECT gpu/programs.cu)
    set_target_properties(nova_gpu_programs PROPERTIES
        CUDA_OPTIX_COMPILATION ON CUDA_ARCHITECTURES "${NOVA_GPU_MIN_ARCH}-virtual" CUDA_STANDARD 20
        CUDA_STANDARD_REQUIRED ON)
    target_include_directories(nova_gpu_programs PRIVATE "${PROJECT_SOURCE_DIR}" "${NOVA_OPTIX_INCLUDE_DIR}")
    target_include_directories(nova_gpu_programs SYSTEM PRIVATE "${HFS}/toolkit/include") # NanoVDB (majorant.h)
    target_compile_definitions(nova_gpu_programs PRIVATE NOMINMAX _USE_MATH_DEFINES)
    target_compile_options(nova_gpu_programs PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:--fmad=false -ftz=true -Xcompiler=/utf-8>)
    add_custom_command(OUTPUT "${_nova_gen}/core/gpu/programs_ir.cpp"
        COMMAND ${CMAKE_COMMAND} "-DINPUTS=$<TARGET_OBJECTS:nova_gpu_programs>"
                "-DOUT=${_nova_gen}/core/gpu/programs_ir.cpp" -DNAME=kNovaProgramsIr
                -P "${CMAKE_CURRENT_SOURCE_DIR}/gpu/embed.cmake"
        DEPENDS nova_gpu_programs $<TARGET_OBJECTS:nova_gpu_programs> "${CMAKE_CURRENT_SOURCE_DIR}/gpu/embed.cmake"
        COMMAND_EXPAND_LISTS VERBATIM)
    # The wavefront kernels (D8): a fatbin (per group of NOVA_GPU_ARCHS its
    # architectures' code and its oldest one's PTX) loaded with the driver
    # API, which takes from it what fits the device. The architecture flags
    # are spelled out here (CUDA_ARCHITECTURES has no form for several codes
    # from one PTX); --threads builds the groups and their codes at once.
    # Flush-to-zero as on the CPU (renderer.cpp).
    add_library(nova_gpu_kernels OBJECT gpu/kernels.cu)
    set_target_properties(nova_gpu_kernels PROPERTIES
        CUDA_FATBIN_COMPILATION ON CUDA_ARCHITECTURES OFF CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON)
    target_include_directories(nova_gpu_kernels PRIVATE "${PROJECT_SOURCE_DIR}")
    target_include_directories(nova_gpu_kernels SYSTEM PRIVATE "${HFS}/toolkit/include") # NanoVDB
    target_compile_definitions(nova_gpu_kernels PRIVATE NOMINMAX _USE_MATH_DEFINES)
    target_compile_options(nova_gpu_kernels PRIVATE ${_nova_gencode} "SHELL:--threads 0"
        $<$<COMPILE_LANGUAGE:CUDA>:--fmad=false -ftz=true -Werror=cross-execution-space-call -Xcompiler=/utf-8>)
    add_custom_command(OUTPUT "${_nova_gen}/core/gpu/kernels_bin.cpp"
        COMMAND ${CMAKE_COMMAND} "-DINPUTS=$<TARGET_OBJECTS:nova_gpu_kernels>"
                "-DOUT=${_nova_gen}/core/gpu/kernels_bin.cpp" -DNAME=kNovaKernels
                -P "${CMAKE_CURRENT_SOURCE_DIR}/gpu/embed.cmake"
        DEPENDS nova_gpu_kernels $<TARGET_OBJECTS:nova_gpu_kernels> "${CMAKE_CURRENT_SOURCE_DIR}/gpu/embed.cmake"
        COMMAND_EXPAND_LISTS VERBATIM)
    # For the tests: the kernels as the architectures below 100 get them (that group's PTX),
    # assembled for the GPU of the machine that runs the tests, so they run what GPUs before the
    # RTX 50 series run, up to the assembler's target. Only where both groups are built.
    list(LENGTH _nova_gencode _nova_groups)
    if(NOVA_BUILD_TESTS AND _nova_groups EQUAL 2)
        set(NOVA_GPU_CHECK_ARCH "120" CACHE STRING
            "Architecture of the GPU the tests run on (100 or newer): they load the older architectures' kernels built for it")
        add_library(nova_gpu_kernels_old OBJECT gpu/kernels.cu)
        set_target_properties(nova_gpu_kernels_old PROPERTIES
            CUDA_FATBIN_COMPILATION ON CUDA_ARCHITECTURES OFF CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON
            NOVA_GPU_CHECK_ARCH "${NOVA_GPU_CHECK_ARCH}")
        target_include_directories(nova_gpu_kernels_old PRIVATE "${PROJECT_SOURCE_DIR}")
        target_include_directories(nova_gpu_kernels_old SYSTEM PRIVATE "${HFS}/toolkit/include") # NanoVDB
        target_compile_definitions(nova_gpu_kernels_old PRIVATE NOMINMAX _USE_MATH_DEFINES)
        target_compile_options(nova_gpu_kernels_old PRIVATE
            "--generate-code=arch=compute_${NOVA_GPU_MIN_ARCH},code=[sm_${NOVA_GPU_CHECK_ARCH}]"
            $<$<COMPILE_LANGUAGE:CUDA>:--fmad=false -ftz=true -Werror=cross-execution-space-call -Xcompiler=/utf-8>)
    endif()
    target_sources(nova_core PRIVATE gpu/cuda_api.cpp gpu/device.cpp gpu/optix_scene.cpp gpu/gpu_renderer.cpp
                                     "${_nova_gen}/core/gpu/programs_ir.cpp" "${_nova_gen}/core/gpu/kernels_bin.cpp")
    target_compile_definitions(nova_core PUBLIC NOVA_HAS_GPU=1)
    # What the device is checked against (core/gpu/device.cpp).
    list(JOIN NOVA_GPU_ARCHS_TESTED "," _nova_tested_text)
    set_source_files_properties(gpu/device.cpp PROPERTIES COMPILE_DEFINITIONS
        "NOVA_GPU_MIN_ARCH=${NOVA_GPU_MIN_ARCH};NOVA_GPU_ARCHS=\"${_nova_arch_text}\";NOVA_GPU_TESTED_ARCHS=\"${_nova_tested_text}\"")
    message(STATUS "Nova: GPU backend (CUDA ${CMAKE_CUDA_COMPILER_VERSION}, OptiX ${NOVA_OPTIX_VERSION}, "
                   "kernels for sm ${_nova_arch_text})")
endif()

if(MSVC)
    target_compile_options(nova_core PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /arch:AVX2)
    # Generated tables: 9-digit literals round-trip to the stored floats.
    set_source_files_properties(bsdf/tables/bsdf_tables.cpp PROPERTIES COMPILE_OPTIONS /wd4305)
    target_compile_definitions(nova_core PUBLIC NOMINMAX _USE_MATH_DEFINES)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    # GCC and Clang: the instruction sets of /arch:AVX2 (the SIMD shading VM
    # is in headers, hence PUBLIC), and no FMA contraction, as MSVC's
    # /fp:precise and the GPU kernels' --fmad=false: one image on every build.
    target_compile_options(nova_core PUBLIC -march=haswell -mtune=generic -ffp-contract=off)
    target_compile_options(nova_core PRIVATE -Wall)
else()
    message(FATAL_ERROR "Nova: nova_core needs an x86-64 CPU with AVX2 (CMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}).")
endif()
