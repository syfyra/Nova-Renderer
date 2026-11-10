# Locates Houdini ($HFS). Nova uses the libraries bundled with Houdini (USD,
# Hydra, TBB, ...) so that everything loaded into the Houdini process shares
# one ABI.
#
# Result:
#   HFS              Houdini installation root
#   Houdini::*       imported targets from $HFS/toolkit/cmake/HoudiniConfig.cmake
#   Nova::TBB        Houdini's oneTBB without the rest of the HDK
#   Nova::OpenSubdiv Houdini's OpenSubdiv (osdCPU)
#   Nova::NanoVDB    Houdini's header-only NanoVDB
#   Nova::OIIO       Houdini's OpenImageIO (namespace HOIIO_*, not part of HoudiniConfig)
#   Nova::MaterialX  Houdini's MaterialX Core + Format (not part of HoudiniConfig)

if(NOT HFS)
    if(DEFINED ENV{HFS})
        set(_nova_hfs "$ENV{HFS}")
    else()
        set(_nova_hfs "C:/Program Files/Side Effects Software/Houdini 22.0.466")
    endif()
    set(HFS "${_nova_hfs}" CACHE PATH "Houdini installation directory ($HFS)")
endif()
file(TO_CMAKE_PATH "${HFS}" HFS)

if(NOT EXISTS "${HFS}/toolkit/cmake/HoudiniConfig.cmake")
    message(FATAL_ERROR "Nova: no Houdini installation at HFS='${HFS}'. Pass -DHFS=<Houdini install dir>.")
endif()

list(APPEND CMAKE_PREFIX_PATH "${HFS}/toolkit/cmake")
find_package(Houdini REQUIRED)
message(STATUS "Nova: Houdini ${Houdini_VERSION} at ${HFS}")

# oneTBB headers live in $HFS/toolkit/include; the headers would auto-link
# "tbb12.lib" by name, so that is disabled in favour of the explicit import lib.
add_library(nova_tbb INTERFACE)
add_library(Nova::TBB ALIAS nova_tbb)
target_include_directories(nova_tbb SYSTEM INTERFACE "${HFS}/toolkit/include")
target_link_libraries(nova_tbb INTERFACE Houdini::Dep::tbb)
target_compile_definitions(nova_tbb INTERFACE __TBB_NO_IMPLICIT_LINKAGE=1)

# OpenSubdiv 3.7 comes from Houdini as a static /MD archive (libosdCPU_md.lib,
# declared as Houdini::Dep::osdCPU) and NanoVDB 32.6 as headers; both have
# their headers in $HFS/toolkit/include. A core-only build takes them from the
# system instead (NovaStandalone.cmake).
add_library(nova_opensubdiv INTERFACE)
add_library(Nova::OpenSubdiv ALIAS nova_opensubdiv)
target_include_directories(nova_opensubdiv SYSTEM INTERFACE "${HFS}/toolkit/include")
target_link_libraries(nova_opensubdiv INTERFACE Houdini::Dep::osdCPU)
add_library(nova_nanovdb INTERFACE)
add_library(Nova::NanoVDB ALIAS nova_nanovdb)
target_include_directories(nova_nanovdb SYSTEM INTERFACE "${HFS}/toolkit/include")

# OpenImageIO ships with Houdini (OpenImageIO_sidefx.dll in $HFS/bin) but has
# no target in HoudiniConfig. Its headers need Imath, which does.
foreach(_lib OpenImageIO OpenImageIO_Util)
    set(_implib "${HFS}/custom/houdini/dsolib/${_lib}_sidefx.lib")
    if(NOT EXISTS "${_implib}")
        message(FATAL_ERROR "Nova: ${_implib} not found")
    endif()
    add_library(nova_${_lib} SHARED IMPORTED)
    set_target_properties(nova_${_lib} PROPERTIES
        IMPORTED_IMPLIB "${_implib}"
        IMPORTED_LOCATION "${HFS}/bin/${_lib}_sidefx.dll")
endforeach()
add_library(nova_oiio INTERFACE)
add_library(Nova::OIIO ALIAS nova_oiio)
target_include_directories(nova_oiio SYSTEM INTERFACE "${HFS}/toolkit/include")
target_link_libraries(nova_oiio INTERFACE nova_OpenImageIO nova_OpenImageIO_Util Houdini::Dep::Imath_sidefx)

# MaterialX 1.39 ships with Houdini (MaterialXCore.dll, MaterialXFormat.dll in
# $HFS/bin) without a target in HoudiniConfig. Only hydra/ links it; the data
# libraries are in $HFS/houdini/materialx/libraries.
foreach(_lib MaterialXCore MaterialXFormat)
    set(_implib "${HFS}/custom/houdini/dsolib/${_lib}.lib")
    if(NOT EXISTS "${_implib}")
        message(FATAL_ERROR "Nova: ${_implib} not found")
    endif()
    add_library(nova_${_lib} SHARED IMPORTED)
    set_target_properties(nova_${_lib} PROPERTIES
        IMPORTED_IMPLIB "${_implib}"
        IMPORTED_LOCATION "${HFS}/bin/${_lib}.dll")
endforeach()
add_library(nova_materialx INTERFACE)
add_library(Nova::MaterialX ALIAS nova_materialx)
target_include_directories(nova_materialx SYSTEM INTERFACE "${HFS}/toolkit/include")
target_link_libraries(nova_materialx INTERFACE nova_MaterialXCore nova_MaterialXFormat)
