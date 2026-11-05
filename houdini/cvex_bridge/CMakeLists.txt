# The CVEX bridge (docs/design/M5.md D4-D7): VEX surface code runs in CVEX
# with its BSDF evaluated inside VEX.
#   nova_cvex_transform  the VFL rewrite (no Houdini dependency; unit tested)
#   nova_cvex            nova_cvex.dll against the HDK (CVEX, VEX, VCC, UT);
#                        hdNova loads it on demand next to hdNova.dll.

add_library(nova_cvex_transform STATIC transform.cpp)
target_include_directories(nova_cvex_transform PUBLIC "${PROJECT_SOURCE_DIR}")

add_library(nova_cvex SHARED bridge.cpp)
target_include_directories(nova_cvex PRIVATE "${PROJECT_SOURCE_DIR}")
target_include_directories(nova_cvex SYSTEM PRIVATE "${HFS}/toolkit/include")
# The HDK's flags and defines, without linking all of its libraries.
get_target_property(_nova_hdk_defines Houdini INTERFACE_COMPILE_DEFINITIONS)
get_target_property(_nova_hdk_options Houdini INTERFACE_COMPILE_OPTIONS)
target_compile_definitions(nova_cvex PRIVATE ${_nova_hdk_defines})
target_compile_options(nova_cvex PRIVATE ${_nova_hdk_options})
target_link_libraries(nova_cvex PRIVATE nova_cvex_transform Nova::TBB Houdini::CVEX Houdini::VEX Houdini::VCC Houdini::UT
                                        Houdini::SYS)

# Installed next to hdNova.dll, which loads it on demand (staged by nova_stage).
install(TARGETS nova_cvex RUNTIME DESTINATION "${NOVA_INSTALL_DIR}/nova/dso/usd_plugins")
