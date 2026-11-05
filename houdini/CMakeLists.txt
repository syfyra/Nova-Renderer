# Houdini package. The package puts dist/nova on HOUDINI_PATH; Houdini then
# loads the USD plugins from dso/usd_plugins and reads UsdRenderers.json.
# HDK components: the CVEX bridge (M5, cvex_bridge/) and the volume bridge
# (M8, volume_bridge/).

add_subdirectory(cvex_bridge)
add_subdirectory(volume_bridge)

file(TO_CMAKE_PATH "${NOVA_INSTALL_DIR}" _nova_install_dir)
set(NOVA_PACKAGE_ROOT "${_nova_install_dir}/nova")

install(FILES package/UsdRenderers.json DESTINATION "${NOVA_PACKAGE_ROOT}")
# Solaris UI (docs/design/M4.md 3.7): Nova tabs on the stock LOPs and the
# viewport options (generated .ds files), and Nova's HDAs.
install(DIRECTORY package/soho DESTINATION "${NOVA_PACKAGE_ROOT}")
install(DIRECTORY package/otls DESTINATION "${NOVA_PACKAGE_ROOT}" OPTIONAL PATTERN backup EXCLUDE)
# The Nova Material Builder (M4.8): tab/shelf tool and its Python module.
install(DIRECTORY package/toolbar DESTINATION "${NOVA_PACKAGE_ROOT}")
install(DIRECTORY package/python3.13libs DESTINATION "${NOVA_PACKAGE_ROOT}"
        PATTERN "__pycache__" EXCLUDE)
install(FILES package/usd_plugins_plugInfo.json
        DESTINATION "${NOVA_PACKAGE_ROOT}/dso/usd_plugins"
        RENAME plugInfo.json)

configure_file(package/nova.json.in "${CMAKE_CURRENT_BINARY_DIR}/nova.json" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/nova.json" DESTINATION "${_nova_install_dir}")
