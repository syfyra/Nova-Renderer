# Hosek-Wilkie sky model data (vendored)

`ArHosekSkyModelData_CIEXYZ.h` is the CIE XYZ coefficient data of the analytic sky model from
"An Analytic Model for Full Spectral Sky-Dome Radiance" (Lukas Hosek, Alexander Wilkie, SIGGRAPH 2012),
taken unchanged from the authors' sample implementation, BSD 3-Clause (see `LICENSE`).

- Version: 1.4a (February 22nd, 2013)
- Source: https://cgg.mff.cuni.cz/projects/SkylightModelling/HosekWilkie_SkylightModel_C_Source.1.4a.zip
  (SHA-1 of the archive: `c8462c0fafa405a79e9a1453112661ec6e46abe5`)
- SHA-1 of `ArHosekSkyModelData_CIEXYZ.h`: `e4d8da6c655917df3babf7fb09bc948393014a90` (66,862 bytes)
- Local changes: none. Included by `core/lights/sky.cpp` only (Nova's Karma sky dome, docs/design/M11-P3.md);
  the state cooking and radiance evaluation there are Nova's own code following the paper.
