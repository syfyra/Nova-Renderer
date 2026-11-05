# nova_imgcmp: statistical comparison of noisy renders (Nova vs a Karma
# reference) plus HDR-FLIP, for tests/reference. Reads EXR through Houdini's
# OpenImageIO, so it runs with $HFS/bin on PATH.
add_executable(nova_imgcmp nova_imgcmp.cpp)
target_include_directories(nova_imgcmp SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
target_link_libraries(nova_imgcmp PRIVATE Nova::OIIO)
find_package(OpenMP COMPONENTS CXX)
if(OpenMP_CXX_FOUND)
    target_link_libraries(nova_imgcmp PRIVATE OpenMP::OpenMP_CXX) # FLIP's per-pixel loops
endif()
if(MSVC)
    target_compile_options(nova_imgcmp PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8)
    target_compile_definitions(nova_imgcmp PRIVATE NOMINMAX _USE_MATH_DEFINES)
endif()
