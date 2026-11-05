# Research probe (M8 volumes, docs/research/m8/houdini-volumes.md). Stand-alone,
# not part of the Nova build:
#   cmake -S docs/research/m8/probes/hv_volume_probe -B <builddir> -G "Visual Studio 17 2022" -A x64
#   cmake --build <builddir> --config Release
# Targets:
#   hdVolumeProbe       HDK-free Hydra delegate "VolumeProbe" (pxr libs only), staged
#                       as a Houdini package in <builddir>/pkg (HOUDINI_PATH=<builddir>/pkg;&)
#   hv_volume_helper    HDK-linked DLL resolving fields via $HFS/houdini/dso/USD_SopVol.dll
#   hv_vdb2nano         HDK-free exe: OpenVDB (openvdb_sesi) .vdb -> NanoVDB .nvdb round trip
cmake_minimum_required(VERSION 3.26)
project(hvVolumeProbe CXX)

set(HFS "C:/Program Files/Side Effects Software/Houdini 22.0.466" CACHE PATH "Houdini install")
list(APPEND CMAKE_PREFIX_PATH "${HFS}/toolkit/cmake")
find_package(Houdini REQUIRED)

set(_common_defs NOMINMAX _USE_MATH_DEFINES WIN32_LEAN_AND_MEAN __TBB_NO_IMPLICIT_LINKAGE=1
    _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)
set(_common_opts /permissive- /Zc:inline- /Zc:__cplusplus /bigobj /utf-8 /EHsc /MD /wd4244 /wd4305 /wd4267)

# --- HDK-free delegate -------------------------------------------------------
add_library(hdVolumeProbe SHARED hv_probe_delegate.cpp)
target_compile_features(hdVolumeProbe PRIVATE cxx_std_20)
target_include_directories(hdVolumeProbe SYSTEM PRIVATE "${HFS}/toolkit/include" "${HFS}/toolkit/include/python3.13")
target_link_libraries(hdVolumeProbe PRIVATE
    Houdini::Dep::pxr_arch Houdini::Dep::pxr_tf Houdini::Dep::pxr_gf Houdini::Dep::pxr_vt Houdini::Dep::pxr_sdf
    Houdini::Dep::pxr_plug Houdini::Dep::pxr_trace Houdini::Dep::pxr_work Houdini::Dep::pxr_hd Houdini::Dep::pxr_hf
    Houdini::Dep::pxr_cameraUtil Houdini::Dep::pxr_python Houdini::Dep::tbb Houdini::Dep::python3.13)
target_compile_options(hdVolumeProbe PRIVATE ${_common_opts})
target_compile_definitions(hdVolumeProbe PRIVATE ${_common_defs}
    MFB_PACKAGE_NAME=hdVolumeProbe MFB_ALT_PACKAGE_NAME=hdVolumeProbe MFB_PACKAGE_MODULE=HdVolumeProbe)

# --- HDK helper ----------------------------------------------------------------
add_library(hv_volume_helper SHARED hv_volume_helper.cpp)
target_compile_features(hv_volume_helper PRIVATE cxx_std_20)
target_link_libraries(hv_volume_helper PRIVATE Houdini Houdini::Dep::openvdb_sesi)
# NanoVDB only compiles its OpenVDB adaptor (createNanoGrid(openvdb::Grid)) with this define
target_compile_definitions(hv_volume_helper PRIVATE NANOVDB_USE_OPENVDB)

# --- HDK-free OpenVDB -> NanoVDB -----------------------------------------------
add_executable(hv_vdb2nano hv_vdb2nano.cpp)
target_compile_features(hv_vdb2nano PRIVATE cxx_std_20)
target_include_directories(hv_vdb2nano SYSTEM PRIVATE "${HFS}/toolkit/include")
target_link_libraries(hv_vdb2nano PRIVATE Houdini::Dep::openvdb_sesi Houdini::Dep::tbb Houdini::Dep::blosc Houdini::Dep::z)
target_compile_options(hv_vdb2nano PRIVATE ${_common_opts})
target_compile_definitions(hv_vdb2nano PRIVATE ${_common_defs} NANOVDB_USE_OPENVDB)

set(_pkg "${CMAKE_BINARY_DIR}/pkg")
add_custom_command(TARGET hdVolumeProbe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_pkg}/dso/usd_plugins/hdVolumeProbe/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:hdVolumeProbe>" "${_pkg}/dso/usd_plugins/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/plugInfo.json"
            "${_pkg}/dso/usd_plugins/hdVolumeProbe/resources/plugInfo.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/usd_plugins_plugInfo.json"
            "${_pkg}/dso/usd_plugins/plugInfo.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/UsdRenderers.json" "${_pkg}/UsdRenderers.json"
    VERBATIM)
