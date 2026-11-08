# hdNova: Hydra render delegate plugin loaded by Solaris, the viewport and husk.
# The sources are compiled once as an object library that both the plugin DLL
# and the Hydra-level tests link.

add_library(hdNova_objects OBJECT
    rendererPlugin.cpp
    renderDelegate.cpp
    renderPass.cpp
    aov.cpp
    crypto.cpp
    denoise.cpp
    renderBuffer.cpp
    atmosphere.cpp
    medium.cpp
    mesh.cpp
    sphere.cpp
    curves.cpp
    points.cpp
    volume.cpp
    field.cpp
    volumeDependencies.cpp
    primvarUtil.cpp
    karmaSettings.cpp
    instancer.cpp
    camera.cpp
    light.cpp
    lightFilter.cpp
    textureCache.cpp
    colorSpace.cpp
    textureConvert.cpp
    textureSystem.cpp
    meshSubdiv.cpp
    implicitSurfaceSceneIndexPlugin.cpp
    resyncSceneIndex.cpp
    velocitySceneIndex.cpp
    material/nodeDefDatabase.cpp
    material/networkSource.cpp
    material/translator.cpp
    material/vexShaders.cpp
    material/principled.cpp
    material/vexBridge.cpp
    material/registry.cpp
    material/material.cpp
)

target_link_libraries(hdNova_objects PUBLIC
    nova_core
    Nova::OIIO
    Nova::MaterialX
    Houdini::Dep::OpenColorIO
    Houdini::Dep::pxr_arch
    Houdini::Dep::pxr_tf
    Houdini::Dep::pxr_gf
    Houdini::Dep::pxr_js
    Houdini::Dep::pxr_vt
    Houdini::Dep::pxr_sdf
    Houdini::Dep::pxr_ar
    Houdini::Dep::pxr_plug
    Houdini::Dep::pxr_work
    Houdini::Dep::pxr_trace
    Houdini::Dep::pxr_hd
    Houdini::Dep::pxr_hdsi
    Houdini::Dep::pxr_hf
    Houdini::Dep::pxr_pxOsd
    Houdini::Dep::pxr_cameraUtil
    Houdini::Dep::pxr_usdLux
    Houdini::Dep::pxr_python
    Houdini::Dep::tbb
    Houdini::Dep::python3.13
)

# Houdini's USD is built with Python support, so pxr headers need Python.h.
target_include_directories(hdNova_objects SYSTEM PUBLIC
    "${HFS}/toolkit/include"
    "${HFS}/toolkit/include/python3.13"
)

if(MSVC)
    # /Zc:inline- keeps the ARCH_CONSTRUCTOR symbols that TF_REGISTRY_FUNCTION
    # relies on (same as USD's own MSVC flags).
    target_compile_options(hdNova_objects PUBLIC /W3 /permissive- /Zc:inline- /Zc:__cplusplus /bigobj /utf-8
                                                 /wd4244 /wd4305 /wd4267)
    # pxr headers still use the C++17-era std::atomic_* shared_ptr overloads.
    target_compile_definitions(hdNova_objects PUBLIC
        NOMINMAX _USE_MATH_DEFINES WIN32_LEAN_AND_MEAN
        _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)
    target_compile_definitions(hdNova_objects PRIVATE
        MFB_PACKAGE_NAME=hdNova MFB_ALT_PACKAGE_NAME=hdNova MFB_PACKAGE_MODULE=HdNova)
endif()

add_library(hdNova SHARED)
target_link_libraries(hdNova PRIVATE hdNova_objects)

# Stage a complete package per configuration in the build tree for the husk
# tests, so they never touch dist/ (a running Houdini keeps the installed
# hdNova.dll locked). The target runs on every build; copy_if_different keeps
# it cheap and also picks up edits to the JSON files.
set(NOVA_STAGE_ROOT "${CMAKE_BINARY_DIR}/stage" CACHE INTERNAL "Packages staged for tests, one per configuration")
set(_nova_stage "${NOVA_STAGE_ROOT}/$<CONFIG>/nova")
add_custom_target(nova_stage ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_nova_stage}/dso/usd_plugins/hdNova/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:hdNova>" "${_nova_stage}/dso/usd_plugins/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:nova_cvex>" "${_nova_stage}/dso/usd_plugins/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:nova_volume>" "${_nova_stage}/dso/usd_plugins/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/plugInfo.json"
            "${_nova_stage}/dso/usd_plugins/hdNova/resources/plugInfo.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/houdini/package/usd_plugins_plugInfo.json"
            "${_nova_stage}/dso/usd_plugins/plugInfo.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/houdini/package/UsdRenderers.json"
            "${_nova_stage}/UsdRenderers.json"
    # Solaris UI (M4.7): parameter files and HDAs.
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${PROJECT_SOURCE_DIR}/houdini/package/soho"
            "${_nova_stage}/soho"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PROJECT_SOURCE_DIR}/houdini/package/otls"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${PROJECT_SOURCE_DIR}/houdini/package/otls"
            "${_nova_stage}/otls"
    # The Nova Material Builder (M4.8): shelf tool and its module.
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${PROJECT_SOURCE_DIR}/houdini/package/toolbar"
            "${_nova_stage}/toolbar"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${PROJECT_SOURCE_DIR}/houdini/package/python3.13libs"
            "${_nova_stage}/python3.13libs"
    # Nova's icon for the HDAs and tools.
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${PROJECT_SOURCE_DIR}/houdini/package/config"
            "${_nova_stage}/config"
    VERBATIM)
add_dependencies(nova_stage hdNova nova_cvex nova_volume)

set(NOVA_USD_PLUGIN_DIR "${NOVA_INSTALL_DIR}/nova/dso/usd_plugins")
install(TARGETS hdNova RUNTIME DESTINATION "${NOVA_USD_PLUGIN_DIR}")
install(FILES plugInfo.json DESTINATION "${NOVA_USD_PLUGIN_DIR}/hdNova/resources")
# Symbols for crash dumps from Houdini.
install(FILES "$<TARGET_PDB_FILE:hdNova>" DESTINATION "${NOVA_USD_PLUGIN_DIR}"
        CONFIGURATIONS RelWithDebInfo OPTIONAL)
