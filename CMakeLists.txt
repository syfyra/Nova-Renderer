cmake_minimum_required(VERSION 3.26)

# vcpkg provides Embree 4 and Catch2 (see vcpkg.json). Static libraries with
# the dynamic CRT keep extra DLLs out of the Houdini process.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
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
set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "vcpkg triplet")

# Houdini only ships release libraries, so Debug builds are not supported.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CONFIGURATION_TYPES "Release;RelWithDebInfo" CACHE STRING "Build configurations" FORCE)

project(Nova VERSION 0.1.0 LANGUAGES CXX)
message(STATUS "Nova: vcpkg toolchain ${CMAKE_TOOLCHAIN_FILE}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(NovaHoudini)
include(NovaOptiX)
find_package(embree 4 CONFIG REQUIRED)
# A second TBB or an Embree DLL inside the Houdini process must not happen.
if(NOT EMBREE_STATIC_LIB OR NOT EMBREE_TASKING_SYSTEM STREQUAL "INTERNAL" OR NOT EMBREE_RAY_MASK)
    message(FATAL_ERROR "Nova needs a static Embree with INTERNAL tasking and ray masks (got static=${EMBREE_STATIC_LIB}, "
                        "tasking=${EMBREE_TASKING_SYSTEM}, ray_mask=${EMBREE_RAY_MASK}). Check vcpkg.json and the triplet.")
endif()

set(NOVA_INSTALL_DIR "${CMAKE_SOURCE_DIR}/dist" CACHE PATH "Houdini package output directory")
option(NOVA_BUILD_TESTS "Build unit tests" ON)

add_subdirectory(core)
add_subdirectory(hydra)
add_subdirectory(houdini)
add_subdirectory(tools)

if(NOVA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
