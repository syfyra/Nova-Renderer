find_package(Catch2 3 CONFIG REQUIRED)

add_executable(nova_tests
    test_math.cpp
    test_denoise.cpp
    test_sampling.cpp
    test_sobol.cpp
    test_filter.cpp
    test_shapes.cpp
    test_accel.cpp
    test_instancing.cpp
    test_camera.cpp
    test_motion.cpp
    test_subdiv.cpp
    test_render.cpp
    test_light_energy.cpp
    test_furnace.cpp
    test_bsdf_stats.cpp
    test_bsdf_lobes.cpp
    test_bsdf_lobes2.cpp
    test_closures.cpp
    test_material_furnace.cpp
    test_shading_vm.cpp
    test_attributes.cpp
    test_spheres.cpp
    test_noise.cpp
    test_textures.cpp
    test_ray_cones.cpp
    test_opacity.cpp
    test_ray_state.cpp
    test_shading_queries.cpp
    test_graph_closures.cpp
    test_displacement.cpp
    test_lpe.cpp
    test_lpe_render.cpp
    test_utility_aovs.cpp
    test_crypto.cpp
    test_ray_limits.cpp
    test_fake_caustics.cpp
    test_karma_shading.cpp
    test_volume.cpp
    test_volume_shader.cpp
    test_volume_render.cpp
    test_volume_scatter.cpp
    test_volume_shell.cpp
    test_atmosphere.cpp
    test_curves.cpp
    test_subsurface.cpp
    test_hair.cpp
    test_light_tree.cpp
    test_emitters.cpp
    test_scene_view.cpp
    test_holdouts.cpp
    test_light_filter.cpp
    test_sky.cpp
)
target_link_libraries(nova_tests PRIVATE nova_core Catch2::Catch2WithMain)
# The GPU backend's tests (docs/design/M10.md); they skip without a device.
if(TARGET nova_gpu_programs)
    target_sources(nova_tests PRIVATE test_gpu_accel.cpp test_gpu_render.cpp test_gpu_fixes.cpp)
    target_link_libraries(nova_tests PRIVATE Nova::OptiX)
endif()
target_compile_definitions(nova_tests PRIVATE NOVA_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(nova_tests PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8)
endif()

# CUDA compile check (docs/design/M3.md K14): the kernel-side core headers
# must build as device code, so a host-only construct breaks the build now and
# not only in M10. Never run; skipped without a CUDA compiler.
option(NOVA_CUDA_CHECK "Compile the kernel-side core headers with nvcc" ON)
set(NOVA_CUDA_ARCH "120" CACHE STRING "CUDA architecture for the compile check (120: RTX 50 series)")
if(NOVA_CUDA_CHECK)
    include(CheckLanguage)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        add_library(nova_cuda_check OBJECT cuda/kernel_headers.cu)
        target_include_directories(nova_cuda_check PRIVATE "${PROJECT_SOURCE_DIR}")
        set_target_properties(nova_cuda_check PROPERTIES
            CUDA_ARCHITECTURES "${NOVA_CUDA_ARCH}" CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON)
        target_compile_options(nova_cuda_check PRIVATE
            $<$<COMPILE_LANGUAGE:CUDA>:-Werror=cross-execution-space-call -Xcompiler=/utf-8>)
        target_compile_definitions(nova_cuda_check PRIVATE NOMINMAX _USE_MATH_DEFINES)
        message(STATUS "Nova: CUDA compile check of the kernel headers (sm_${NOVA_CUDA_ARCH})")
    else()
        message(STATUS "Nova: no CUDA compiler; the kernel-header compile check is skipped")
    endif()
endif()

# Hydra-level tests against the delegate's object files.
add_executable(nova_hydra_tests test_hydra.cpp test_hydra_mtlx.cpp test_hydra_oiio.cpp test_hydra_materials.cpp
    test_hydra_textures.cpp test_hydra_normals.cpp test_hydra_bench.cpp test_hydra_karma.cpp test_hydra_graphs.cpp
    test_hydra_displacement.cpp test_hydra_vex.cpp test_ipr_latency.cpp test_hydra_volumes.cpp test_hydra_velocity.cpp
    test_hydra_holdouts.cpp
    test_hydra_filters.cpp
    test_hydra_sky.cpp test_hydra_medium.cpp test_hydra_karma_settings.cpp)
target_compile_definitions(nova_hydra_tests PRIVATE NOVA_HFS="${HFS}" NOVA_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(nova_hydra_tests PRIVATE hdNova_objects Catch2::Catch2WithMain
    Houdini::Dep::pxr_usd Houdini::Dep::pxr_usdGeom Houdini::Dep::pxr_usdShade Houdini::Dep::pxr_usdImaging)
# The CVEX and volume bridges next to the executable, where hdNova looks.
add_dependencies(nova_hydra_tests nova_cvex nova_volume)
add_custom_command(TARGET nova_hydra_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:nova_cvex>" "$<TARGET_FILE_DIR:nova_hydra_tests>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:nova_volume>" "$<TARGET_FILE_DIR:nova_hydra_tests>")

# tbb12.dll and the pxr DLLs come from Houdini's bin directory.
include(Catch)
catch_discover_tests(nova_tests
    DISCOVERY_MODE PRE_TEST
    DL_PATHS "${HFS}/bin"
)
catch_discover_tests(nova_hydra_tests
    DISCOVERY_MODE PRE_TEST
    DL_PATHS "${HFS}/bin"
    PROPERTIES LABELS hydra
)

# The CVEX bridge (docs/design/M5.md M5.2): nova_cvex.dll loaded at run time,
# as hdNova loads it; Houdini's DLLs from $HFS/bin.
add_executable(nova_cvex_tests test_cvex.cpp)
target_compile_definitions(nova_cvex_tests PRIVATE NOVA_CVEX_DLL="$<TARGET_FILE:nova_cvex>"
                                                   NOVA_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(nova_cvex_tests PRIVATE nova_core nova_cvex_transform Catch2::Catch2WithMain)
add_dependencies(nova_cvex_tests nova_cvex)
catch_discover_tests(nova_cvex_tests
    DISCOVERY_MODE PRE_TEST
    DL_PATHS "${HFS}/bin"
    PROPERTIES LABELS cvex ENVIRONMENT "HFS=${HFS}"
)
# The volume bridge (docs/design/M8.md D1): nova_volume.dll loaded at run time,
# as hdNova loads it; Houdini's DLLs from $HFS/bin.
add_executable(nova_volume_tests test_volume_bridge.cpp)
target_compile_definitions(nova_volume_tests PRIVATE NOVA_VOLUME_DLL="$<TARGET_FILE:nova_volume>"
                                                     NOVA_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(nova_volume_tests PRIVATE nova_core Catch2::Catch2WithMain)
add_dependencies(nova_volume_tests nova_volume)
catch_discover_tests(nova_volume_tests
    DISCOVERY_MODE PRE_TEST
    DL_PATHS "${HFS}/bin"
    PROPERTIES LABELS volume ENVIRONMENT "HFS=${HFS}"
)

# The renderer checks again with the sampling-free fallback (docs/design/M5.md,
# notes on M5.4): it must stay unbiased.
add_test(NAME cvex_safe_sampling COMMAND nova_cvex_tests "[render]")
set_tests_properties(cvex_safe_sampling PROPERTIES LABELS cvex
    ENVIRONMENT "HFS=${HFS};NOVA_CVEX_SAFE_SAMPLING=1;PATH=${HFS}/bin\\;$ENV{PATH}")

# husk smoke test of the Hydra delegate, using the package staged by hdNova.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND AND EXISTS "${HFS}/bin/husk.exe")
    add_test(NAME husk_scenes
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/husk/check_scenes.py"
                --hfs "${HFS}" --stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova" --scenes "${CMAKE_CURRENT_SOURCE_DIR}/scenes"
                --out "${CMAKE_CURRENT_BINARY_DIR}/husk/$<CONFIG>")
    set_tests_properties(husk_scenes PROPERTIES LABELS husk TIMEOUT 300)
endif()

# Solaris UI (M4.7): Nova tabs, parameter files, the Nova Render Settings
# LOP, rendered with husk; hython in an isolated environment (needs a license).
if(EXISTS "${HFS}/bin/hython.exe")
    add_test(NAME houdini_solaris_ui
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/houdini/run_hython.ps1"
                -Script "${CMAKE_CURRENT_SOURCE_DIR}/houdini/test_solaris_ui.py"
                -Stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova" -Hfs "${HFS}"
                "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/houdini_ui/$<CONFIG>")
    set_tests_properties(houdini_solaris_ui PROPERTIES LABELS houdini TIMEOUT 600)
    # The Nova Material Builder (M4.8): its USD equals Karma's builder's, and
    # a material with Karma's AO, ray switch and ramp renders in Nova and Karma.
    add_test(NAME houdini_material_builder
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/houdini/run_hython.ps1"
                -Script "${CMAKE_CURRENT_SOURCE_DIR}/houdini/test_material_builder.py"
                -Stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova" -Hfs "${HFS}"
                "${CMAKE_CURRENT_BINARY_DIR}/houdini_builder/$<CONFIG>")
    set_tests_properties(houdini_material_builder PROPERTIES LABELS houdini TIMEOUT 600)
    # A stock VEX Material Builder (M5.4): Principled Shader Core and noise,
    # rendered through the CVEX bridge by husk from the usda, and by Karma.
    add_test(NAME houdini_vex_builder
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/houdini/run_hython.ps1"
                -Script "${CMAKE_CURRENT_SOURCE_DIR}/houdini/test_vex_builder.py"
                -Stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova" -Hfs "${HFS}"
                "${CMAKE_CURRENT_BINARY_DIR}/houdini_vex_builder/$<CONFIG>")
    set_tests_properties(houdini_vex_builder PROPERTIES LABELS houdini TIMEOUT 600)
    # The Karma sky LOPs (M11 P3) still author the scenes husk_karma_sky
    # was measured with.
    add_test(NAME houdini_karma_sky_lops
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/houdini/run_hython.ps1"
                -Script "${CMAKE_CURRENT_SOURCE_DIR}/houdini/test_karma_sky_lops.py" -Hfs "${HFS}"
                "${CMAKE_CURRENT_BINARY_DIR}/houdini_karma_sky_lops/$<CONFIG>")
    set_tests_properties(houdini_karma_sky_lops PROPERTIES LABELS houdini TIMEOUT 600)
    # The Karma Sky Atmosphere and Physical Sky LOPs (M11 P7) still author the
    # scenes husk_karma_atmosphere was measured with.
    add_test(NAME houdini_karma_atmosphere_lops
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/houdini/run_hython.ps1"
                -Script "${CMAKE_CURRENT_SOURCE_DIR}/houdini/test_karma_atmosphere_lops.py" -Hfs "${HFS}"
                "${CMAKE_CURRENT_BINARY_DIR}/houdini_karma_atmosphere_lops/$<CONFIG>")
    set_tests_properties(houdini_karma_atmosphere_lops PROPERTIES LABELS houdini TIMEOUT 600)
    # Volume data paths (M8.0): in-memory SOP volumes, .bgeo.sc, husk on a
    # USD-ROP-saved stage, Hydra in the cooking process.
    add_test(NAME houdini_volumes
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/houdini/run_hython.ps1"
                -Script "${CMAKE_CURRENT_SOURCE_DIR}/houdini/test_volumes.py"
                -Stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova" -Hfs "${HFS}"
                "${NOVA_STAGE_ROOT}/$<CONFIG>/nova" "${CMAKE_CURRENT_BINARY_DIR}/houdini_volumes/$<CONFIG>")
    set_tests_properties(houdini_volumes PROPERTIES LABELS houdini TIMEOUT 600 ENVIRONMENT "HFS=${HFS}")
    # The Karma Pyro Material from Houdini's recipe (M8.1): a volume shader
    # without warnings, reading only the fields it needs, bounded everywhere.
    add_test(NAME houdini_pyro_material
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/houdini/run_hython.ps1"
                -Script "${CMAKE_CURRENT_SOURCE_DIR}/houdini/test_pyro_material.py"
                -Stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova" -Hfs "${HFS}"
                "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/houdini_pyro_material/$<CONFIG>")
    set_tests_properties(houdini_pyro_material PROPERTIES LABELS houdini TIMEOUT 600 ENVIRONMENT "HFS=${HFS}")
    # Absorption and emission against Karma CPU (M8.2): the calibration cubes
    # rendered by both through husk.
    add_test(NAME houdini_volume_karma
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/houdini/run_hython.ps1"
                -Script "${CMAKE_CURRENT_SOURCE_DIR}/houdini/test_volume_karma.py"
                -Stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova" -Hfs "${HFS}"
                "${CMAKE_CURRENT_BINARY_DIR}/houdini_volume_karma/$<CONFIG>")
    set_tests_properties(houdini_volume_karma PROPERTIES LABELS houdini TIMEOUT 900 ENVIRONMENT "HFS=${HFS}")
endif()

# Instancing check; Houdini's own Python brings numpy and OpenImageIO.
if(EXISTS "${HFS}/python313/python.exe" AND EXISTS "${HFS}/bin/husk.exe")
    add_test(NAME husk_instancing
        COMMAND "${HFS}/python313/python.exe" -I "${CMAKE_CURRENT_SOURCE_DIR}/husk/check_instancing.py"
                --hfs "${HFS}" --stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova" --scenes "${CMAKE_CURRENT_SOURCE_DIR}/scenes"
                --out "${CMAKE_CURRENT_BINARY_DIR}/husk_instancing/$<CONFIG>")
    set_tests_properties(husk_instancing PROPERTIES LABELS husk TIMEOUT 300)
    add_test(NAME husk_motion
        COMMAND "${HFS}/python313/python.exe" -I "${CMAKE_CURRENT_SOURCE_DIR}/husk/check_motion.py"
                --hfs "${HFS}" --stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova" --scenes "${CMAKE_CURRENT_SOURCE_DIR}/scenes"
                --out "${CMAKE_CURRENT_BINARY_DIR}/husk_motion/$<CONFIG>")
    set_tests_properties(husk_motion PROPERTIES LABELS husk TIMEOUT 300)
    add_test(NAME husk_textures
        COMMAND "${HFS}/python313/python.exe" -I "${CMAKE_CURRENT_SOURCE_DIR}/husk/check_textures.py"
                --hfs "${HFS}" --stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova" --scenes "${CMAKE_CURRENT_SOURCE_DIR}/scenes"
                --out "${CMAKE_CURRENT_BINARY_DIR}/husk_textures/$<CONFIG>")
    set_tests_properties(husk_textures PROPERTIES LABELS husk TIMEOUT 300)
    add_test(NAME husk_render_pass
        COMMAND "${HFS}/python313/python.exe" -I "${CMAKE_CURRENT_SOURCE_DIR}/husk/check_render_pass.py"
                --hfs "${HFS}" --stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova" --scenes "${CMAKE_CURRENT_SOURCE_DIR}/scenes"
                --out "${CMAKE_CURRENT_BINARY_DIR}/husk_render_pass/$<CONFIG>")
    set_tests_properties(husk_render_pass PROPERTIES LABELS husk TIMEOUT 300)
    # Karma's render vars (M6): light path expressions and their relations.
    add_test(NAME husk_aovs
        COMMAND "${HFS}/python313/python.exe" -I "${CMAKE_CURRENT_SOURCE_DIR}/husk/check_aovs.py"
                --hfs "${HFS}" --stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova" --scenes "${CMAKE_CURRENT_SOURCE_DIR}/scenes"
                --out "${CMAKE_CURRENT_BINARY_DIR}/husk_aovs/$<CONFIG>"
                $<$<TARGET_EXISTS:nova_cli>:--cli> $<$<TARGET_EXISTS:nova_cli>:$<TARGET_FILE:nova_cli>>)
    set_tests_properties(husk_aovs PROPERTIES LABELS husk TIMEOUT 300)
    add_test(NAME husk_sss
        COMMAND "${HFS}/python313/python.exe" -I "${CMAKE_CURRENT_SOURCE_DIR}/husk/check_sss.py"
                --hfs "${HFS}" --stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova"
                --out "${CMAKE_CURRENT_BINARY_DIR}/husk_sss/$<CONFIG>")
    set_tests_properties(husk_sss PROPERTIES LABELS husk TIMEOUT 600)
    add_test(NAME husk_hair
        COMMAND "${HFS}/python313/python.exe" -I "${CMAKE_CURRENT_SOURCE_DIR}/husk/check_hair.py"
                --hfs "${HFS}" --stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova"
                --out "${CMAKE_CURRENT_BINARY_DIR}/husk_hair/$<CONFIG>")
    set_tests_properties(husk_hair PROPERTIES LABELS husk TIMEOUT 600)
    add_test(NAME husk_lights
        COMMAND "${HFS}/python313/python.exe" -I "${CMAKE_CURRENT_SOURCE_DIR}/husk/check_lights.py"
                --hfs "${HFS}" --stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova"
                --out "${CMAKE_CURRENT_BINARY_DIR}/husk_lights/$<CONFIG>")
    set_tests_properties(husk_lights PROPERTIES LABELS husk TIMEOUT 600)
    # Karma's Shadow Catcher and Background Plate (docs/design/M11-P2.md):
    # probes and the LOPs' scenes (husk/karma_nodes) against Karma's values.
    add_test(NAME husk_karma_nodes
        COMMAND "${HFS}/python313/python.exe" -I "${CMAKE_CURRENT_SOURCE_DIR}/husk/check_karma_nodes.py"
                --hfs "${HFS}" --stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova"
                --out "${CMAKE_CURRENT_BINARY_DIR}/husk_karma_nodes/$<CONFIG>")
    set_tests_properties(husk_karma_nodes PROPERTIES LABELS husk TIMEOUT 900)
    # Karma's Fog Box (docs/design/M11-P5.md): scenes built by the LOP
    # (husk/fogbox), Karma's values and analytic ones; CPU and Nova XPU.
    add_test(NAME husk_karma_fogbox
        COMMAND "${HFS}/python313/python.exe" -I "${CMAKE_CURRENT_SOURCE_DIR}/husk/check_karma_fogbox.py"
                --hfs "${HFS}" --stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova"
                --out "${CMAKE_CURRENT_BINARY_DIR}/husk_karma_fogbox/$<CONFIG>")
    set_tests_properties(husk_karma_fogbox PROPERTIES LABELS husk TIMEOUT 900)
    # Karma's Blocker Light Filter (docs/design/M11-P4.md): scenes from the LOP
    # against Karma's measured ratios.
    add_test(NAME husk_karma_filters
        COMMAND "${HFS}/python313/python.exe" -I "${CMAKE_CURRENT_SOURCE_DIR}/husk/check_karma_filters.py"
                --hfs "${HFS}" --stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova"
                --out "${CMAKE_CURRENT_BINARY_DIR}/husk_karma_filters/$<CONFIG>")
    set_tests_properties(husk_karma_filters PROPERTIES LABELS husk TIMEOUT 600)
    # Karma's sky dome (docs/design/M11-P3.md): scenes built by the Karma LOPs
    # (husk/karma_sky), Karma's values in the script; CPU and Nova XPU.
    add_test(NAME husk_karma_sky
        COMMAND "${HFS}/python313/python.exe" -I "${CMAKE_CURRENT_SOURCE_DIR}/husk/check_karma_sky.py"
                --hfs "${HFS}" --stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova"
                --out "${CMAKE_CURRENT_BINARY_DIR}/husk_karma_sky/$<CONFIG>")
    set_tests_properties(husk_karma_sky PROPERTIES LABELS husk TIMEOUT 900)
    # The Karma Sky Atmosphere (docs/design/M11-P7.md): scenes built by the
    # Karma LOPs, Nova CPU and XPU against Karma's values and the model.
    add_test(NAME husk_karma_atmosphere
        COMMAND "${HFS}/python313/python.exe" -I "${CMAKE_CURRENT_SOURCE_DIR}/husk/check_karma_atmosphere.py"
                --hfs "${HFS}" --stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova"
                --out "${CMAKE_CURRENT_BINARY_DIR}/husk_karma_atmosphere/$<CONFIG>")
    set_tests_properties(husk_karma_atmosphere PROPERTIES LABELS husk TIMEOUT 1200)
    add_test(NAME husk_emitters
        COMMAND "${HFS}/python313/python.exe" -I "${CMAKE_CURRENT_SOURCE_DIR}/husk/check_emitters.py"
                --hfs "${HFS}" --stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova"
                --out "${CMAKE_CURRENT_BINARY_DIR}/husk_emitters/$<CONFIG>")
    set_tests_properties(husk_emitters PROPERTIES LABELS husk TIMEOUT 600)
    # Karma's render settings in Nova (docs/design/M11-P1.md).
    add_test(NAME husk_karma_props
        COMMAND "${HFS}/python313/python.exe" -I "${CMAKE_CURRENT_SOURCE_DIR}/husk/check_karma_props.py"
                --hfs "${HFS}" --stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova"
                --out "${CMAKE_CURRENT_BINARY_DIR}/husk_karma_props/$<CONFIG>")
    set_tests_properties(husk_karma_props PROPERTIES LABELS husk TIMEOUT 900)
    add_test(NAME husk_subdiv
        COMMAND "${HFS}/python313/python.exe" -I "${CMAKE_CURRENT_SOURCE_DIR}/husk/check_subdiv.py"
                --hfs "${HFS}" --stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova" --scenes "${CMAKE_CURRENT_SOURCE_DIR}/scenes"
                --out "${CMAKE_CURRENT_BINARY_DIR}/husk_subdiv/$<CONFIG>")
    set_tests_properties(husk_subdiv PROPERTIES LABELS husk TIMEOUT 300)
    add_test(NAME husk_materials
        COMMAND "${HFS}/python313/python.exe" -I "${CMAKE_CURRENT_SOURCE_DIR}/husk/check_materials.py"
                --hfs "${HFS}" --stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova"
                --out "${CMAKE_CURRENT_BINARY_DIR}/husk_materials/$<CONFIG>")
    set_tests_properties(husk_materials PROPERTIES LABELS husk TIMEOUT 300)
endif()

# nova_cli renders of tests/scenes: no husk, no Houdini license.
if(Python3_FOUND AND TARGET nova_cli)
    add_test(NAME cli_scenes
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/cli/check_cli.py"
                --cli "$<TARGET_FILE:nova_cli>" --hfs "${HFS}" --scenes "${CMAKE_CURRENT_SOURCE_DIR}/scenes"
                --out "${CMAKE_CURRENT_BINARY_DIR}/cli/$<CONFIG>")
    set_tests_properties(cli_scenes PROPERTIES LABELS cli TIMEOUT 300
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${HFS}/bin;PXR_PLUGINPATH_NAME=unset:")
endif()

# Reference images (tests/reference): one test per scene of manifest.json,
# Nova rendered twice through husk and compared statistically with a Karma
# CPU reference (or an analytic value) by nova_imgcmp. Scenes marked
# "pending" exit 77 and are reported as skipped.
if(Python3_FOUND AND EXISTS "${HFS}/bin/husk.exe" AND TARGET nova_imgcmp)
    set(_nova_ref "${CMAKE_CURRENT_SOURCE_DIR}/reference")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_nova_ref}/manifest.json")
    file(READ "${_nova_ref}/manifest.json" _nova_manifest)
    string(JSON _nova_count LENGTH "${_nova_manifest}" scenes)
    math(EXPR _nova_last "${_nova_count} - 1")
    foreach(_i RANGE ${_nova_last})
        string(JSON _nova_scene GET "${_nova_manifest}" scenes ${_i} name)
        add_test(NAME reference_${_nova_scene}
            COMMAND Python3::Interpreter "${_nova_ref}/tools/check_references.py"
                    --hfs "${HFS}" --stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova"
                    --imgcmp "$<TARGET_FILE:nova_imgcmp>" --refdir "${_nova_ref}"
                    --out "${CMAKE_CURRENT_BINARY_DIR}/reference/$<CONFIG>" --scene ${_nova_scene})
        set_tests_properties(reference_${_nova_scene} PROPERTIES LABELS reference TIMEOUT 600 SKIP_RETURN_CODE 77)
        if(TARGET nova_cli) # the same comparison, rendered with nova_cli
            add_test(NAME reference_cli_${_nova_scene}
                COMMAND Python3::Interpreter "${_nova_ref}/tools/check_references.py"
                        --hfs "${HFS}" --stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova"
                        --imgcmp "$<TARGET_FILE:nova_imgcmp>" --refdir "${_nova_ref}" --cli "$<TARGET_FILE:nova_cli>"
                        --out "${CMAKE_CURRENT_BINARY_DIR}/reference_cli/$<CONFIG>" --scene ${_nova_scene})
            set_tests_properties(reference_cli_${_nova_scene} PROPERTIES LABELS "reference;cli" TIMEOUT 600
                                 SKIP_RETURN_CODE 77)
        endif()
    endforeach()
endif()
