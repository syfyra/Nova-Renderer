cmake_minimum_required(VERSION 3.26)

# NOVA_CORE_ONLY builds the renderer without Houdini: nova_core, its unit tests
# (nova_tests) and nova_bsdf_precompute, against system libraries (Embree 4,
# oneTBB, OpenSubdiv, NanoVDB, Catch2; see cmake/NovaStandalone.cmake). The
# Hydra delegate, the Houdini package, nova_cli and the GPU backend need
# Houdini and Windows; they are the full build. It is the default away from
# Windows, where the full build does not exist.
if(CMAKE_HOST_WIN32)
    set(_nova_core_only_default OFF)
else()
    set(_nova_core_only_default ON)
endif()
option(NOVA_CORE_ONLY "Build only nova_core, its unit tests and nova_bsdf_precompute, without Houdini" ${_nova_core_only_default})

# vcpkg provides Embree 4 and Catch2 (see vcpkg.json). Static libraries with
# the dynamic CRT keep extra DLLs out of the Houdini process.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND NOT NOVA_CORE_ONLY)
    # Prefer a regular vcpkg clone. Visual Studio developer prompts point
    # VCPKG_ROOT at VS's bundled, read-only vcpkg (it has vcpkg-bundle.json),
    # which resolves packages differently.
    if(DEFINED ENV{VCPKG_ROOT} AND NOT EXISTS "$ENV{VCPKG_ROOT}/vcpkg-bundle.json")
        set(_nova_vcpkg_root "$ENV{VCPKG_ROOT}")
    elseif(EXISTS "$ENV{USERPROFILE}/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(_nova_vcpkg_root "$ENV{USERPROFILE}/vcpkg")
    elseif(DEFINED ENV{VCPKG_ROOT})
        set(_nova_vcpkg_root "$ENV{VCPKG_ROOT}")
    endif()
    if(_nova_vcpkg_root)
        file(TO_CMAKE_PATH "${_nova_vcpkg_root}" _nova_vcpkg_root)
        set(CMAKE_TOOLCHAIN_FILE "${_nova_vcpkg_root}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "vcpkg toolchain")
    endif()
endif()
if(NOT NOVA_CORE_ONLY)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "vcpkg triplet")
endif()

# Houdini only ships release libraries, so Debug builds are not supported
# (a core-only build has no such limit).
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
if(NOT NOVA_CORE_ONLY)
    set(CMAKE_CONFIGURATION_TYPES "Release;RelWithDebInfo" CACHE STRING "Build configurations" FORCE)
endif()

project(Nova VERSION 0.1.5 LANGUAGES CXX)
if(NOT NOVA_CORE_ONLY)
    message(STATUS "Nova: vcpkg toolchain ${CMAKE_TOOLCHAIN_FILE}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
if(NOVA_CORE_ONLY)
    include(NovaStandalone)
    set(NOVA_HAS_OPTIX OFF) # the GPU backend and the OptiX denoiser load NVIDIA's Windows DLLs
else()
    include(NovaHoudini)
    include(NovaOptiX)
endif()
find_package(embree 4 CONFIG REQUIRED)
if(NOVA_CORE_ONLY)
    # A system Embree: the features vcpkg.json asks for, as far as Embree's
    # package config reports them (not the instance geometry).
    foreach(_nova_embree_feature RAY_MASK FILTER_FUNCTION RAY_PACKETS GEOMETRY_TRIANGLE GEOMETRY_QUAD GEOMETRY_CURVE
                                 GEOMETRY_POINT GEOMETRY_USER)
        if(NOT EMBREE_${_nova_embree_feature})
            message(FATAL_ERROR "Nova needs an Embree built with EMBREE_${_nova_embree_feature}=ON (${embree_DIR}).")
        endif()
    endforeach()
# A second TBB or an Embree DLL inside the Houdini process must not happen.
elseif(NOT EMBREE_STATIC_LIB OR NOT EMBREE_TASKING_SYSTEM STREQUAL "INTERNAL" OR NOT EMBREE_RAY_MASK)
    message(FATAL_ERROR "Nova needs a static Embree with INTERNAL tasking and ray masks (got static=${EMBREE_STATIC_LIB}, "
                        "tasking=${EMBREE_TASKING_SYSTEM}, ray_mask=${EMBREE_RAY_MASK}). Check vcpkg.json and the triplet.")
endif()

set(NOVA_INSTALL_DIR "${CMAKE_SOURCE_DIR}/dist" CACHE PATH "Houdini package output directory")
option(NOVA_BUILD_TESTS "Build unit tests" ON)

add_subdirectory(core)
if(NOT NOVA_CORE_ONLY)
    add_subdirectory(hydra)
    add_subdirectory(houdini)
endif()
add_subdirectory(tools)

if(NOVA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
