# Research probe (M9 curves, docs/research/m9/curves-hair.md). Stand-alone, not
# part of the Nova build (same pattern as docs/research/m8/probes/hv_volume_probe):
#   cmake -S docs/research/m9/probes/ch_curves_probe -B <builddir> -G "Visual Studio 17 2022" -A x64
#   cmake --build <builddir> --config Release
# hdCurvesProbe: HDK-free Hydra delegate "CurvesProbe" (pxr libs only), staged as
# a Houdini package in <builddir>/pkg (HOUDINI_PATH=<builddir>/pkg;&).
cmake_minimum_required(VERSION 3.26)
project(chCurvesProbe CXX)

set(HFS "C:/Program Files/Side Effects Software/Houdini 22.0.466" CACHE PATH "Houdini install")
list(APPEND CMAKE_PREFIX_PATH "${HFS}/toolkit/cmake")
find_package(Houdini REQUIRED)

add_library(hdCurvesProbe SHARED ch_probe_delegate.cpp)
target_compile_features(hdCurvesProbe PRIVATE cxx_std_20)
target_include_directories(hdCurvesProbe SYSTEM PRIVATE "${HFS}/toolkit/include" "${HFS}/toolkit/include/python3.13")
target_link_libraries(hdCurvesProbe PRIVATE
    Houdini::Dep::pxr_arch Houdini::Dep::pxr_tf Houdini::Dep::pxr_gf Houdini::Dep::pxr_vt Houdini::Dep::pxr_sdf
    Houdini::Dep::pxr_plug Houdini::Dep::pxr_trace Houdini::Dep::pxr_work Houdini::Dep::pxr_hd Houdini::Dep::pxr_hf
    Houdini::Dep::pxr_cameraUtil Houdini::Dep::pxr_python Houdini::Dep::tbb Houdini::Dep::python3.13)
target_compile_options(hdCurvesProbe PRIVATE /permissive- /Zc:inline- /Zc:__cplusplus /bigobj /utf-8 /EHsc /MD
    /wd4244 /wd4305 /wd4267)
target_compile_definitions(hdCurvesProbe PRIVATE NOMINMAX _USE_MATH_DEFINES WIN32_LEAN_AND_MEAN
    __TBB_NO_IMPLICIT_LINKAGE=1 _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS
    _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING
    MFB_PACKAGE_NAME=hdCurvesProbe MFB_ALT_PACKAGE_NAME=hdCurvesProbe MFB_PACKAGE_MODULE=HdCurvesProbe)

set(_pkg "${CMAKE_BINARY_DIR}/pkg")
add_custom_command(TARGET hdCurvesProbe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_pkg}/dso/usd_plugins/hdCurvesProbe/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:hdCurvesProbe>" "${_pkg}/dso/usd_plugins/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/plugInfo.json"
            "${_pkg}/dso/usd_plugins/hdCurvesProbe/resources/plugInfo.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/usd_plugins_plugInfo.json"
            "${_pkg}/dso/usd_plugins/plugInfo.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/UsdRenderers.json" "${_pkg}/UsdRenderers.json"
    VERBATIM)
