# nova_bsdf_precompute: integrates the BSDF energy tables (core/bsdf/energy.h)
# with the lobes' own code. Only the lobe headers and TBB; never links
# nova_core, which contains the generated tables.
add_executable(nova_bsdf_precompute main.cpp)
target_include_directories(nova_bsdf_precompute PRIVATE "${PROJECT_SOURCE_DIR}")
target_link_libraries(nova_bsdf_precompute PRIVATE Nova::TBB)
if(MSVC)
    target_compile_options(nova_bsdf_precompute PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /arch:AVX2)
    target_compile_definitions(nova_bsdf_precompute PRIVATE NOMINMAX _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
endif()

# Regenerates core/bsdf/tables/bsdf_tables.cpp (minutes of CPU time; not
# part of the default build). Commit the result.
set(_nova_table_inputs
    "${PROJECT_SOURCE_DIR}/core/bsdf/common.h"
    "${PROJECT_SOURCE_DIR}/core/bsdf/microfacet.h"
    "${PROJECT_SOURCE_DIR}/core/bsdf/fresnel.h"
    "${PROJECT_SOURCE_DIR}/core/bsdf/conductor.h"
    "${PROJECT_SOURCE_DIR}/core/bsdf/dielectric.h"
    "${PROJECT_SOURCE_DIR}/core/bsdf/energy.h"
    "${PROJECT_SOURCE_DIR}/core/bsdf/diffuse.h"
    "${PROJECT_SOURCE_DIR}/core/bsdf/sheen.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
add_custom_target(nova_bsdf_tables
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${HFS}/bin;$ENV{PATH}"
            $<TARGET_FILE:nova_bsdf_precompute> -o "${PROJECT_SOURCE_DIR}/core/bsdf/tables/bsdf_tables.cpp"
            --hash-inputs ${_nova_table_inputs}
    DEPENDS nova_bsdf_precompute
    COMMENT "Integrating the BSDF energy tables"
    VERBATIM)
