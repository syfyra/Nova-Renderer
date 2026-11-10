# The libraries of a build without Houdini (NOVA_CORE_ONLY): nova_core, its
# unit tests and nova_bsdf_precompute against system packages. It defines the
# same targets NovaHoudini.cmake does for the core:
#
#   Nova::TBB         oneTBB (find_package(TBB))
#   Nova::OpenSubdiv  OpenSubdiv's osdCPU (3.5 or newer)
#   Nova::NanoVDB     the header-only NanoVDB 32.6 or newer of ABI 32 (OpenVDB 11), the version Houdini 22.0 ships
#
# NanoVDB comes from NOVA_NANOVDB_INCLUDE_DIR or the system; when that is
# missing or older, the headers are fetched from the OpenVDB repository
# (-DNOVA_FETCH_NANOVDB=OFF turns the download off).

find_package(TBB CONFIG REQUIRED)
add_library(nova_tbb INTERFACE)
add_library(Nova::TBB ALIAS nova_tbb)
target_link_libraries(nova_tbb INTERFACE TBB::tbb)

# OpenSubdiv's own package config names static archives that distributions
# leave out (it then fails the configure), so the library is located by hand.
find_path(NOVA_OPENSUBDIV_INCLUDE_DIR opensubdiv/version.h)
find_library(NOVA_OPENSUBDIV_LIBRARY NAMES osdCPU)
if(NOT NOVA_OPENSUBDIV_INCLUDE_DIR OR NOT NOVA_OPENSUBDIV_LIBRARY)
    message(FATAL_ERROR "Nova: OpenSubdiv (osdCPU) not found. Install it (Debian/Ubuntu: libosd-dev) or set "
                        "NOVA_OPENSUBDIV_INCLUDE_DIR and NOVA_OPENSUBDIV_LIBRARY.")
endif()
add_library(nova_opensubdiv INTERFACE)
add_library(Nova::OpenSubdiv ALIAS nova_opensubdiv)
target_include_directories(nova_opensubdiv SYSTEM INTERFACE "${NOVA_OPENSUBDIV_INCLUDE_DIR}")
target_link_libraries(nova_opensubdiv INTERFACE "${NOVA_OPENSUBDIV_LIBRARY}")

option(NOVA_FETCH_NANOVDB "Fetch the NanoVDB headers if no usable version is installed" ON)
set(NOVA_NANOVDB_FETCH_TAG "v11.0.0" CACHE STRING "Tag of github.com/AcademySoftwareFoundation/openvdb to fetch (NanoVDB 32.6)")

# Sets _nova_nanovdb_ok when dir/nanovdb/NanoVDB.h is version 32.6 or a later
# version of ABI 32 (the grids' binary layout, core/volume/grid.h).
function(_nova_check_nanovdb dir)
    set(_nova_nanovdb_ok OFF PARENT_SCOPE)
    if(NOT EXISTS "${dir}/nanovdb/NanoVDB.h")
        return()
    endif()
    file(STRINGS "${dir}/nanovdb/NanoVDB.h" _lines REGEX "^#define NANOVDB_(MAJOR|MINOR|PATCH)_VERSION_NUMBER [0-9]+")
    foreach(_part MAJOR MINOR PATCH)
        set(_${_part} 0)
        foreach(_line IN LISTS _lines)
            if(_line MATCHES "^#define NANOVDB_${_part}_VERSION_NUMBER ([0-9]+)")
                set(_${_part} "${CMAKE_MATCH_1}")
            endif()
        endforeach()
    endforeach()
    set(NOVA_NANOVDB_VERSION "${_MAJOR}.${_MINOR}.${_PATCH}" PARENT_SCOPE)
    # util/CreateNanoGrid.h and util/GridBuilder.h (the tests) moved in 32.7.
    if(_MAJOR EQUAL 32 AND _MINOR GREATER_EQUAL 6 AND EXISTS "${dir}/nanovdb/util/CreateNanoGrid.h")
        set(_nova_nanovdb_ok ON PARENT_SCOPE)
    endif()
endfunction()

find_path(NOVA_NANOVDB_INCLUDE_DIR nanovdb/NanoVDB.h)
_nova_check_nanovdb("${NOVA_NANOVDB_INCLUDE_DIR}")
if(NOT _nova_nanovdb_ok)
    if(NOVA_NANOVDB_INCLUDE_DIR)
        message(STATUS "Nova: NanoVDB ${NOVA_NANOVDB_VERSION} at ${NOVA_NANOVDB_INCLUDE_DIR} is not 32.6 or newer of ABI 32")
    endif()
    if(NOT NOVA_FETCH_NANOVDB)
        message(FATAL_ERROR "Nova: no NanoVDB 32.6 headers. Set NOVA_NANOVDB_INCLUDE_DIR to the directory that holds "
                            "nanovdb/NanoVDB.h (OpenVDB 11's nanovdb/ directory), or configure with -DNOVA_FETCH_NANOVDB=ON.")
    endif()
    include(FetchContent)
    # Headers only: SOURCE_SUBDIR names no CMakeLists.txt, so OpenVDB's own
    # build is never configured.
    FetchContent_Declare(nova_openvdb
        GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/openvdb.git
        GIT_TAG "${NOVA_NANOVDB_FETCH_TAG}"
        GIT_SHALLOW ON
        SOURCE_SUBDIR nova-headers-only)
    FetchContent_MakeAvailable(nova_openvdb)
    set(NOVA_NANOVDB_INCLUDE_DIR "${nova_openvdb_SOURCE_DIR}/nanovdb" CACHE PATH "NanoVDB include directory" FORCE)
    _nova_check_nanovdb("${NOVA_NANOVDB_INCLUDE_DIR}")
    if(NOT _nova_nanovdb_ok)
        message(FATAL_ERROR "Nova: the fetched NanoVDB (${NOVA_NANOVDB_FETCH_TAG}) is not 32.6 or newer of ABI 32.")
    endif()
endif()
add_library(nova_nanovdb INTERFACE)
add_library(Nova::NanoVDB ALIAS nova_nanovdb)
target_include_directories(nova_nanovdb SYSTEM INTERFACE "${NOVA_NANOVDB_INCLUDE_DIR}")

message(STATUS "Nova: core-only build without Houdini (TBB ${TBB_VERSION}, OpenSubdiv at ${NOVA_OPENSUBDIV_LIBRARY}, "
               "NanoVDB ${NOVA_NANOVDB_VERSION} at ${NOVA_NANOVDB_INCLUDE_DIR})")
