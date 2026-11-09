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
# OpenSubdiv 3.7 comes from Houdini as a static /MD archive (libosdCPU_md.lib,
# declared as Houdini::Dep::osdCPU); its headers are in $HFS/toolkit/include,
# which Nova::TBB already adds.
target_link_libraries(nova_core
    PUBLIC  Nova::TBB
    PRIVATE embree Houdini::Dep::osdCPU
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
    set(_nova_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
    add_library(nova_gpu_programs OBJECT gpu/programs.cu)
    set_target_properties(nova_gpu_programs PROPERTIES
        CUDA_OPTIX_COMPILATION ON CUDA_ARCHITECTURES "120" CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON)
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
    # The wavefront kernels (D8): a fatbin (sm_120 code and compute_120 PTX)
    # loaded with the driver API. Flush-to-zero as on the CPU (renderer.cpp).
    add_library(nova_gpu_kernels OBJECT gpu/kernels.cu)
    set_target_properties(nova_gpu_kernels PROPERTIES
        CUDA_FATBIN_COMPILATION ON CUDA_ARCHITECTURES "120" CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON)
    target_include_directories(nova_gpu_kernels PRIVATE "${PROJECT_SOURCE_DIR}")
    target_include_directories(nova_gpu_kernels SYSTEM PRIVATE "${HFS}/toolkit/include") # NanoVDB
    target_compile_definitions(nova_gpu_kernels PRIVATE NOMINMAX _USE_MATH_DEFINES)
    target_compile_options(nova_gpu_kernels PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:--fmad=false -ftz=true -Werror=cross-execution-space-call -Xcompiler=/utf-8>)
    add_custom_command(OUTPUT "${_nova_gen}/core/gpu/kernels_bin.cpp"
        COMMAND ${CMAKE_COMMAND} "-DINPUTS=$<TARGET_OBJECTS:nova_gpu_kernels>"
                "-DOUT=${_nova_gen}/core/gpu/kernels_bin.cpp" -DNAME=kNovaKernels
                -P "${CMAKE_CURRENT_SOURCE_DIR}/gpu/embed.cmake"
        DEPENDS nova_gpu_kernels $<TARGET_OBJECTS:nova_gpu_kernels> "${CMAKE_CURRENT_SOURCE_DIR}/gpu/embed.cmake"
        COMMAND_EXPAND_LISTS VERBATIM)
    target_sources(nova_core PRIVATE gpu/cuda_api.cpp gpu/device.cpp gpu/optix_scene.cpp gpu/gpu_renderer.cpp
                                     "${_nova_gen}/core/gpu/programs_ir.cpp" "${_nova_gen}/core/gpu/kernels_bin.cpp")
    target_compile_definitions(nova_core PUBLIC NOVA_HAS_GPU=1)
    message(STATUS "Nova: GPU backend (CUDA ${CMAKE_CUDA_COMPILER_VERSION}, OptiX ${NOVA_OPTIX_VERSION})")
endif()

if(MSVC)
    target_compile_options(nova_core PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /arch:AVX2)
    # Generated tables: 9-digit literals round-trip to the stored floats.
    set_source_files_properties(bsdf/tables/bsdf_tables.cpp PROPERTIES COMPILE_OPTIONS /wd4305)
    target_compile_definitions(nova_core PUBLIC NOMINMAX _USE_MATH_DEFINES)
endif()
