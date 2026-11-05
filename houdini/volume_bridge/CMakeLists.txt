# The volume bridge (docs/design/M8.md D1): nova_volume.dll resolves USD
# volume fields through Houdini (in-memory "op:" paths, .vdb, .bgeo.sc) and
# hands them to hdNova as NanoVDB grids (core/volume/bridge.h). hdNova loads
# it on demand next to hdNova.dll.

add_library(nova_volume SHARED bridge.cpp)
target_include_directories(nova_volume PRIVATE "${PROJECT_SOURCE_DIR}")
target_include_directories(nova_volume SYSTEM PRIVATE "${HFS}/toolkit/include")
# The HDK's flags and defines, without linking all of its libraries.
get_target_property(_nova_hdk_defines Houdini INTERFACE_COMPILE_DEFINITIONS)
get_target_property(_nova_hdk_options Houdini INTERFACE_COMPILE_OPTIONS)
target_compile_definitions(nova_volume PRIVATE ${_nova_hdk_defines} NANOVDB_USE_OPENVDB)
target_compile_options(nova_volume PRIVATE ${_nova_hdk_options})
# libCE: UT_VoxelArray's headers pull in Houdini's OpenCL wrappers.
target_link_libraries(nova_volume PRIVATE Nova::TBB Houdini::GT Houdini::GEO Houdini::GA Houdini::CE Houdini::UT
                                          Houdini::SYS Houdini::Dep::openvdb_sesi)

install(TARGETS nova_volume RUNTIME DESTINATION "${NOVA_INSTALL_DIR}/nova/dso/usd_plugins")
