# nova_cli: standalone renderer (.usd -> EXR) for tests and CI. hdNova is
# linked in statically; at run time only Houdini's USD, TBB, Python and OIIO
# DLLs are needed ($HFS/bin on PATH). VEX Material Builder materials load the
# CVEX bridge (nova_cvex.dll, copied next to the executable) and with it
# Houdini's VEX libraries; scenes without them never do.

add_executable(nova_cli main.cpp sampler.cpp)
target_link_libraries(nova_cli PRIVATE
    hdNova_objects # nova_core, Nova::OIIO, the delegate's pxr libraries and compile options
    Houdini::Dep::pxr_usd
    Houdini::Dep::pxr_usdGeom
    Houdini::Dep::pxr_usdRender
    Houdini::Dep::pxr_usdImaging
)
if(MSVC)
    target_compile_options(nova_cli PRIVATE /W4)
endif()
# Start/debug from Visual Studio with Houdini's DLLs.
set_target_properties(nova_cli PROPERTIES VS_DEBUGGER_ENVIRONMENT "PATH=${HFS}/bin;%PATH%")

# Plug metadata next to the executable, registered by nova_cli from <exe dir>/usd_plugins:
#  - hdNova: hydra/plugInfo.json with an empty LibraryPath (the code is linked
#    in; PlugPlugin::_Load treats an empty path as already loaded)
#  - novaUsdImagingAdapters: the UsdImaging Camera adapter, which Houdini only
#    declares through its HDK plugin USD_HD.dll
file(READ "${PROJECT_SOURCE_DIR}/hydra/plugInfo.json" _nova_pluginfo)
string(REGEX REPLACE "\"LibraryPath\"[ \t]*:[ \t]*\"[^\"]*\"" "\"LibraryPath\": \"\""
       _nova_pluginfo "${_nova_pluginfo}")
set(_nova_cli_plugins "$<TARGET_FILE_DIR:nova_cli>/usd_plugins")
file(GENERATE OUTPUT "${_nova_cli_plugins}/hdNova/resources/plugInfo.json"
     CONTENT "${_nova_pluginfo}")
file(GENERATE OUTPUT "${_nova_cli_plugins}/novaUsdImagingAdapters/resources/plugInfo.json"
     INPUT "${CMAKE_CURRENT_SOURCE_DIR}/usdImagingAdapters.plugInfo.json")
#  - usdKarma: Houdini's Karma USD schemas (KarmaSkyDomeLight, KarmaLightAPI,
#    ...; a resource-only plugin, no code), which husk finds on Houdini's
#    plugin path (docs/design/M11-P3.md)
file(GENERATE OUTPUT "${_nova_cli_plugins}/usdKarma/resources/plugInfo.json"
     CONTENT "{ \"Includes\": [ \"${HFS}/houdini/dso/usd_plugins/usdKarma/resources/\" ] }\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/hydra/plugInfo.json")

add_dependencies(nova_cli nova_cvex nova_volume)
add_custom_command(TARGET nova_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:nova_cvex>" "$<TARGET_FILE_DIR:nova_cli>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:nova_volume>" "$<TARGET_FILE_DIR:nova_cli>")
