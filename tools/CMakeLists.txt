# Standalone tools:
#   nova_cli     renders a .usd scene to EXR without husk, the HDK or a license (tests, CI)
#   nova_imgcmp  statistical comparison of noisy renders plus HDR-FLIP (tests/reference)
#   nova_bsdf_precompute  integrates the BSDF energy tables (target nova_bsdf_tables)
add_subdirectory(nova_cli)
add_subdirectory(nova_imgcmp)
add_subdirectory(nova_bsdf_precompute)
