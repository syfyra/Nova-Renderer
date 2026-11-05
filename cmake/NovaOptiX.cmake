# Locates CUDA and the OptiX headers. M0 only detects them; the OptiX denoiser
# (M7) and the GPU backend (M10) build on the targets defined here.
#
# Result:
#   NOVA_HAS_OPTIX        ON if CUDA and the OptiX headers were found
#   Nova::OptiX           INTERFACE target: the OptiX and CUDA headers (the driver's DLLs are loaded at run time)
#
# OptiX itself ships with the NVIDIA driver; building only needs its headers.
# They come from an installed OptiX SDK or, if none is found and CUDA is
# present, are downloaded from github.com/NVIDIA/optix-dev. A failed download
# (offline, firewalled) only disables OptiX; it never fails the configure.

option(NOVA_WITH_OPTIX "Use CUDA/OptiX if available (OptiX denoiser)" ON)
option(NOVA_FETCH_OPTIX "Fetch the OptiX headers if no OptiX SDK is installed" ON)
set(NOVA_OPTIX_ROOT "" CACHE PATH "OptiX SDK root (contains include/optix.h)")
set(NOVA_OPTIX_FETCH_TAG "v9.1.0" CACHE STRING "Tag of github.com/NVIDIA/optix-dev to fetch")

set(NOVA_HAS_OPTIX OFF)

if(NOVA_WITH_OPTIX)
    # Fall back to the default installer location when CUDA_PATH is not set
    # (e.g. in shells started before CUDA was installed).
    if(NOT CUDAToolkit_ROOT AND NOT DEFINED ENV{CUDA_PATH})
        if(DEFINED ENV{ProgramW6432}) # 64-bit Program Files, even from 32-bit parents
            file(TO_CMAKE_PATH "$ENV{ProgramW6432}" _nova_program_files)
        else()
            file(TO_CMAKE_PATH "$ENV{ProgramFiles}" _nova_program_files)
        endif()
        file(GLOB _nova_cuda_roots LIST_DIRECTORIES true "${_nova_program_files}/NVIDIA GPU Computing Toolkit/CUDA/v12.*")
        list(SORT _nova_cuda_roots COMPARE NATURAL ORDER DESCENDING)
        if(_nova_cuda_roots)
            list(GET _nova_cuda_roots 0 CUDAToolkit_ROOT)
        endif()
    endif()
    find_package(CUDAToolkit 12)

    file(TO_CMAKE_PATH "$ENV{ProgramData}" _nova_program_data)
    file(GLOB _nova_optix_sdks LIST_DIRECTORIES true "${_nova_program_data}/NVIDIA Corporation/OptiX SDK *")
    list(SORT _nova_optix_sdks COMPARE NATURAL ORDER DESCENDING)
    find_path(NOVA_OPTIX_INCLUDE_DIR optix.h
        HINTS "${NOVA_OPTIX_ROOT}" "$ENV{OptiX_INSTALL_DIR}" "$ENV{OPTIX_ROOT}"
        PATHS ${_nova_optix_sdks}
        PATH_SUFFIXES include
        NO_DEFAULT_PATH)

    if(NOT NOVA_OPTIX_INCLUDE_DIR AND NOVA_FETCH_OPTIX AND CUDAToolkit_FOUND)
        set(_nova_optix_dir "${CMAKE_BINARY_DIR}/_deps/optix-dev-${NOVA_OPTIX_FETCH_TAG}")
        set(_nova_optix_status "0;cached")
        if(NOT EXISTS "${_nova_optix_dir}/include/optix.h")
            set(_nova_optix_zip "${_nova_optix_dir}.zip")
            # No EXPECTED_HASH: it turns a failed download into a hard error.
            # The extracted header's OPTIX_VERSION is checked below instead.
            file(DOWNLOAD "https://github.com/NVIDIA/optix-dev/archive/refs/tags/${NOVA_OPTIX_FETCH_TAG}.zip"
                 "${_nova_optix_zip}" STATUS _nova_optix_status INACTIVITY_TIMEOUT 30)
            list(GET _nova_optix_status 0 _nova_optix_rc)
            if(_nova_optix_rc EQUAL 0)
                file(REMOVE_RECURSE "${_nova_optix_dir}.tmp")
                file(ARCHIVE_EXTRACT INPUT "${_nova_optix_zip}" DESTINATION "${_nova_optix_dir}.tmp")
                file(GLOB _nova_optix_extracted LIST_DIRECTORIES true "${_nova_optix_dir}.tmp/*")
                list(LENGTH _nova_optix_extracted _nova_optix_count)
                if(_nova_optix_count EQUAL 1 AND EXISTS "${_nova_optix_extracted}/include/optix.h")
                    file(REMOVE_RECURSE "${_nova_optix_dir}")
                    file(RENAME "${_nova_optix_extracted}" "${_nova_optix_dir}")
                endif()
                file(REMOVE_RECURSE "${_nova_optix_dir}.tmp")
            endif()
            file(REMOVE "${_nova_optix_zip}")
        endif()
        if(EXISTS "${_nova_optix_dir}/include/optix.h")
            set(NOVA_OPTIX_INCLUDE_DIR "${_nova_optix_dir}/include" CACHE PATH "OptiX include directory" FORCE)
        else()
            message(WARNING "Nova: could not download the OptiX headers (${_nova_optix_status}). OptiX stays disabled; "
                            "install the OptiX SDK, set NOVA_OPTIX_ROOT, or configure with -DNOVA_FETCH_OPTIX=OFF.")
        endif()
    endif()

    if(CUDAToolkit_FOUND AND NOVA_OPTIX_INCLUDE_DIR)
        file(STRINGS "${NOVA_OPTIX_INCLUDE_DIR}/optix.h" _nova_optix_version_line REGEX "^#define OPTIX_VERSION [0-9]+")
        string(REGEX REPLACE "^#define OPTIX_VERSION ([0-9]+).*" "\\1" NOVA_OPTIX_VERSION "${_nova_optix_version_line}")
        if(NOVA_OPTIX_VERSION VERSION_GREATER_EQUAL 80000)
            set(NOVA_HAS_OPTIX ON)
        else()
            message(WARNING "Nova: ${NOVA_OPTIX_INCLUDE_DIR}/optix.h is not OptiX 8 or newer; OptiX stays disabled.")
        endif()
    endif()

    if(NOVA_HAS_OPTIX)
        add_library(nova_optix INTERFACE)
        add_library(Nova::OptiX ALIAS nova_optix)
        # Headers only: core/denoise loads nvcuda.dll and nvoptix.dll at run time,
        # so Nova still loads on machines without an NVIDIA driver.
        target_include_directories(nova_optix SYSTEM INTERFACE "${NOVA_OPTIX_INCLUDE_DIR}" ${CUDAToolkit_INCLUDE_DIRS})
        target_compile_definitions(nova_optix INTERFACE NOVA_HAS_OPTIX=1)
    endif()
endif()

if(NOVA_HAS_OPTIX)
    message(STATUS "Nova: CUDA ${CUDAToolkit_VERSION}, OptiX ${NOVA_OPTIX_VERSION} headers at ${NOVA_OPTIX_INCLUDE_DIR}")
else()
    message(STATUS "Nova: OptiX denoiser disabled (CUDA found: ${CUDAToolkit_FOUND}, OptiX headers: ${NOVA_OPTIX_INCLUDE_DIR})")
endif()
