# M8 research: volume rendering algorithms and their integration into Nova

Status: research for M8 (docs/PLAN.md: "Volumen: NanoVDB aus Houdini-VDB-Prims, heterogen, Emission/Blackbody, Volume-Light-Sampling | Pyro-Shots renderbar"). Started 2026-10-14 by an earlier agent (cut off before the report), finished 2026-11-03. Scope: the algorithms and how they fit into Nova's integrator. How volumes get from Houdini to Nova and Karma's conventions (parameter names, emission/density conventions, step rates, LPE events for volumes) are covered by the companion report on Houdini volumes (`docs/research/m8/houdini-volumes.md`, probes `hv_*`).

Every statement carries its source:
- **(lit)**: literature, papers or open-source code, with a reference in section 8. "(lit, from memory)" marks items not re-checked against the source in this research.
- **(code)**: read from Nova's code or from the NanoVDB headers in `$HFS/toolkit/include`, with file names.
- **(meas)**: measured with the prototype `prototypes/va_volume_lab.cpp`, experiment id E1 to E9 (section 2). Machine: AMD Ryzen 7 9800X3D, 8 cores. Built with MSVC 14.44, `/O2 /arch:AVX2`.
- **(proto)**: implemented in the prototype but not measured as such.
- **(design)**: a proposal for Nova.

---

## 0. Findings

1. **Everything here is unbiased, and simple enough.** Free flights use spectral tracking with throughput-weighted *average* probabilities and with absorption carried as a weight instead of ending the path (survival biasing). Transmittance on shadow rays uses ratio tracking. Both run against a coarse grid of majorants. Emission is collected at every tentative collision. None of this adds sampling state to the path (only a volume event counter for the ray limits): the throughput stays a plain RGB `beta`, so Nova's MIS, clamp, Russian roulette, LPE and ray-limit code applies unchanged. (meas E4; design)
2. **Survival biasing is the largest single win.** In the furnace E4 it lowers variance 1.7-1.9x, at 10-30 % more time than the same tracker with sampled absorption. Efficiency relative to pbrt-v4's scheme is 1.83x on a grey emissive medium, 1.92x on a grey non-emissive one and 1.32x on a moderately chromatic one. For the emission-only estimator (E7) it is 2.4x over a real-collision estimator and 1.8x over pbrt-v4's tentative-collision estimator. (meas E4, E7)
3. **Hero-channel spectral MIS is only better in one case: dense, strongly chromatic media.** Miller et al. 2019 as in pbrt-v4 wins there by 1.4x (albedo 0.99, red extinction 6.7x the blue), because it tracks against the hero channel's majorant (32 instead of 74 lookups per path). It needs `r_u`/`r_l` rescaled probabilities through the *whole* integrator, surfaces included. That is not worth it for M8; it is a possible later optimization. (meas E4; lit)
4. **Do not apply spectral MIS per free flight with a uniform hero.** It is unbiased in theory but heavy-tailed in practice: in the dense cases the red variance is 90-850x higher, and the means come out 4-19 % low at 400k paths. Picking the hero by throughput (D2) repairs this, but D2 is still no better than the simpler option in finding 1. (meas E4)
5. **NanoVDB's node statistics are not majorants for trilinear lookups.** In-leaf samples exceed their leaf's max in 0.37 % of cases (up to 17x), and samples in tile regions exceed the tile value in 0.02 %. The cause is that trilinear support reaches one voxel into the neighbouring node. `getNodeInfo()` also returns the *enclosing internal node's* stats for tiles, not the tile value (code `NanoVDB.h:8197`). A majorant grid that takes each cell's max over voxels [lo, hi+1] had **0 violations in 2 million samples**. It builds in about 6 ms from the stored values. (meas E2; code)
6. **One majorant cell per 8³ voxels** (the NanoVDB leaf size) is the right default:
   - Delta tracking: 285 ns per free flight at k=8 against 1563 ns with a global majorant (5.5x faster). At 4x the density: 319 against 3127 ns (9.8x).
   - Ratio tracking peaks at k=32 (6.3e5 vs 4.3e5 at k=8). Residual ratio tracking peaks at k=2-4 (1.0-1.5e6 vs 6.8e5 at k=8).
   - k=8 is the best for free flights, which every path segment needs, and reaches 0.45-0.68x of the best k for the shadow-ray estimators.
   - Rule: grow k by powers of two only to keep the dense grid at or under 128³ cells. (meas E1)
7. **Every estimator was verified unbiased against exact references:**
   - E1: optical depth, exact for trilinear interpolation along a line, |z| < 2.5 in 36 tests.
   - E3: free-flight CDF, KS distance below the 95 % critical value in 8 of 9 ray/seed pairs (the ninth at 1.05x of it).
   - E4: the furnace, 1 ± 3σ over 5 seeds at 1.6 M paths.
   - E5: single scattering vs quadrature, |z| ≤ 1.3 at 32 M samples.
   - E7: emission vs Gauss-Legendre. (meas)
8. **Stochastic trilinear lookups** pick one voxel with the trilinear weights. They cost 9.2 ns against 28.5 ns for trilinear and stay unbiased inside ratio and delta tracking, because the estimators are linear in σ. Ratio tracking became 16-19 % faster for 2-7 % more variance. This is a cheap later optimization and very attractive on the GPU (1 fetch instead of 8). It is not for nonlinear fields such as temperature to blackbody. Cycles 5.0 does the same (lit). (meas E9)
9. **Emissive volumes as lights pay off.** Next-event estimation from a per-cell emission-power CDF beats unidirectional collection by 4x (point under the fire) to 15x (point off to the side) in efficiency for irradiance from a fire. MIS of both is unbiased and always at least as good in variance. In the full integrator the unidirectional half is free (the continuation ray exists anyway), so MIS is the choice. (meas E8)
10. **Equiangular sampling** cuts single-scattering variance by 70x to 10⁶x for a point light in homogeneous media (the most where the medium is thin and the light near the ray), and one-sample MIS sits in between. It matters for fog with point or spot lights, not for typical pyro lit by dome, distant or rect lights. Recommendation: defer it to after M8's core. (meas E6)
11. **Unlike Cycles, Nova needs no medium stack for VDB volumes.** VDB volumes are not bounded by surfaces, so the media along a ray come from ray-versus-box tests. Overlapping volumes add their σ and their majorants. The existing `inMedium`/`sigmaA` Beer absorption of dielectrics stays as it is, and the two combine multiplicatively. A stack only becomes necessary for mesh-bounded uniform volumes, and those are deferred. (code `wavefront.cpp:738-745, 1298-1303`; design)
12. **Volume shaders stay fixed-function in M8**: density, temperature and flame fields drive σ_s, σ_a and emission through a POD material and a blackbody LUT.
    - The bytecode VM works on lane batches and cannot provide a majorant for an arbitrary program.
    - Cycles has to evaluate the shader on a dense grid to build its octree. RenderMan asks the user for a "max density".
    - Karma XPU supports only a fixed set of volume VOPs. (code `core/shading/program.h`; lit)
13. **The NanoVDB in Houdini 22.0.466 is 32.6.0** (`NANOVDB_*_VERSION_NUMBER` in `NanoVDB.h`), with the pre-12.0 `util/` header layout, although its OpenVDB is 13.0.0. Present: `CreateNanoGrid.h`, `GridBuilder.h`, `HDDA.h` (`HDDA`, `DDA`, `TreeMarcher`, `firstActive`, `ZeroCrossing`), `SampleFromVoxels.h`, `NodeManager.h`, `GridStats.h`, `IO.h`, `Ray.h`, `PNanoVDB.h`, `CNanoVDB.h`, and CUDA helpers under `util/cuda/`. The grid format is compatible across 32.x; the API is not (lit: 32.7 moved headers to `math/`/`tools/`). (code; lit)
14. **Blackbody**: a 128-entry LUT, linear in 1/T over 500-40000 K, holds the luminance-normalized Rec.709 colour to 8.6e-4 (256 entries: 2.2e-4). The intensity convention differs between renderers:
    - Cycles: σ_SB·1e-6/π·T⁴ times a luminance-1 colour.
    - pbrt-v4: a spectrum with peak 1, times a scale.
    - Arnold: "intensity 1 is physical".
    - Which one Karma uses belongs to the companion report. (meas `va_blackbody.py`; lit)

## 1. Recommended design in one page

- **Data** (design): `core/volume/`.
  - A `VolumeRecord` per volume prim holds NanoVDB float grids per field (density, temperature, flame, optional vel), each with its own index-to-world transform, plus the object transform or motion keys.
  - Each volume has a fixed-function `VolumeMaterial` POD (section 6.2) and a **majorant grid**: a dense array of the max *unscaled* density per k³-voxel cell (k = 8, dilated by one voxel for trilinear support, by max |v|·shutter for velocity blur, and floored to a minimum rate in emissive cells). An optional per-cell min is kept for residual tracking.
  - An optional **emission-power CDF** over the same cells serves light sampling.
  - Density-scale edits in IPR rescale the majorant at run time; no rebuild.
- **Integrator** (design): a new stage `traceMedia` between `intersectRays` and pass A of `shadeSurfaces`. For each ray whose [0, tHit) overlaps volumes, or which is inside an absorbing dielectric, it runs one spectral-tracking free flight against the summed majorants of the overlapping volumes. The flight:
  - collects emission (σ_a·Le, or emission per length) at each tentative collision, and the area lights it crosses at their distances;
  - ends with either a real scattering event (the ray becomes a *volume hit* at t_s), or with no event (beta carries the per-channel weight, and the ray continues to its surface or escape).
  - Light-only rays and the shadow rays of `traceShadowRays` use **ratio tracking** instead: per-channel transmittance, computed only for rays Embree found unoccluded, with RR below 0.05.
  - Volume hits are shaded in a pass C' (`shadeVolumes`): HG phase, one light chosen by power with the existing `chooseLight`, the phase pdf stored in `bsdfPdf` so the existing `bsdfSampleWeight`/`lightSampleWeight` MIS applies, a continuation sampled from the phase function on the `kBsdf` stream, Nova's RR rule, the `Volume` ray-limit class (with Karma's default 0, every volume event spawns a light-only ray: single scattering), an LPE `V` event and the clamp rules of surfaces.
- **Determinism** (design): the first free-flight uniform comes from Sobol stream `bounce(depth, kMedium)` (reserved, `sobol.h`). Everything after it comes from Nova's 32-bit PCG `Rng` seeded by hash(pixelSeed, sampleIndex, stream id), with shadow-ray tracking on its own stream block. Results stay a function of (pixel, sample, depth): thread-count independent, and bit-identical between adaptive and uniform renders at equal counts (M7 D4/D5).
- **CUDA (M10)** (design):
  - The NanoVDB buffer, majorant arrays, material PODs and the blackbody LUT are flat and pointer-free.
  - The tracking loop is `__host__ __device__` code with a small state (t, DDA state, beta, rng) that can be suspended for a "tracking step" kernel if divergence demands it.
  - Volume AABBs can become OptiX custom primitives.
- **Order of work** (design, section 6.9):
  1. M8.0: data and majorants.
  2. M8.1: absorption and emission.
  3. M8.2: scattering with NEE.
  4. M8.3: pyro material, motion blur, overlap and Karma comparisons.
  5. M8.4: emissive volumes as lights.
  6. M8.5: performance and closing.
  - Deferred: equiangular sampling, a medium stack for mesh-bounded volumes, VM volume shaders, hero-wavelength MIS.

---

## 2. Measurements (prototype `prototypes/va_volume_lab.cpp`)

### 2.1 Setup

(meas, proto)
- **Test grid**: a procedural rising, turbulent smoke plume on a 160³ index box (world size 2), stored as a NanoVDB `FloatGrid` built with Houdini's NanoVDB 32.6 (`nanovdb::build::Grid` → `createNanoGrid`, all stats).
  - 587,066 active voxels (14.3 % of the box) in 1835 leaves, 5 lower and 1 upper node.
  - Buffer 4.2 MB, built in 0.16 s.
  - Density: min 0.002, max 1.552, active mean 0.317 (max/mean 4.9, typical of smoke).
- **Flame field**: 38,013 active voxels, 0.6 MB, in the lower inner plume.
- **Extinction**: σ_t = density·σ, with σ = 8 per world unit (camera-ray optical depths: median 2.5, p90 4.75) or σ = 32 (median 10, p90 19).
- **Lookups**: trilinear via `SampleFromVoxels<ReadAccessor,1,true>`, one accessor per thread.
- **References**:
  - Optical depth is exact: trilinear interpolation is a cubic along a line within one voxel cell, and Simpson's rule per unit-cell piece is exact.
  - Emission: 4-point Gauss-Legendre on quarter cells.
  - Majorant grids: k ∈ {2, 4, 8, 16, 32} voxels per cell, plus "global" (one cell).
  - Sizes for k = 32 / 16 / 8 / 4 / 2: 5×5×4 / 9×10×8 / 17×19×15 (19 KB) / 34×38×30 (0.15 MB) / 68×76×59 (1.16 MB) cells.
  - Each grid builds in 6.1-7.6 ms, single-threaded, by scattering every stored value (leaf values and tiles) into the cells it can influence.

Reproduce: `va_build.bat <outdir>`, then `va_volume_lab.exe 160 0 <e1..e9|all> <seed> <scale>`. The seed varies the random streams of E3, E4, E5, E8 and E9; the scale multiplies the samples of E4, E5 and E8. A full `all` run takes 6.6 s on 8 threads, and E4 alone at scale 64 about 10 s.

Changes made in this session to the inherited prototype:
- It compiled and ran as left.
- Fixed:
  - The E2 tile test compared against the enclosing internal node's stats (finding 5) instead of the tile value.
  - The thread argument 0 meant zero threads.
  - E3 had no seed.
- Added:
  - Integrators D, D2 and B3.
  - Non-emissive E4 cases.
  - E9.

### 2.2 E1: transmittance estimators and majorant resolution

1500 rays (σ = 8) or 600 rays (σ = 32), 64 estimates per ray, single thread (meas). Column notes:
- Bias z = Σ(mean − T_exact)/√Σ var. For |z| < 2.5, all 36 rows pass.
- relVar = E[(T̂ − T)²]/T², over rays with T > 0.01. For delta tracking this is the binary estimator's variance and is independent of the majorant.
- Eff = 1/(relVar·time).

| σ = 8 | majorant | bias z | relVar | lookups | cells | ns/est | eff |
|---|---|---|---|---|---|---|---|
| delta (0/1) | global | 0.22 | 17.8 | 41.9 | 1.0 | 1563 | 3.6e4 |
| | k=32 | 0.15 | 18.1 | 6.4 | 4.2 | 371 | 1.5e5 |
| | k=16 | 0.16 | 18.8 | 3.8 | 7.8 | 297 | 1.8e5 |
| | **k=8** | −1.34 | 17.3 | 2.4 | 14.0 | **285** | **2.0e5** |
| | k=4 | 1.02 | 18.9 | 1.6 | 27.6 | 349 | 1.5e5 |
| | k=2 | 1.70 | 18.6 | 1.2 | 54.4 | 542 | 9.9e4 |
| ratio | global | −1.43 | 1.97 | 148.3 | 1.0 | 5090 | 1.0e5 |
| | **k=32** | 0.59 | 2.20 | 14.1 | 7.4 | 720 | **6.3e5** |
| | k=16 | 1.29 | 3.18 | 9.3 | 14.5 | 607 | 5.2e5 |
| | k=8 | −0.24 | 4.08 | 6.4 | 26.8 | 574 | 4.3e5 |
| | k=4 | 0.41 | 5.48 | 4.7 | 53.7 | 724 | 2.5e5 |
| | k=2 | −1.53 | 7.72 | 3.7 | 106.6 | 1164 | 1.1e5 |
| residual ratio | global | −1.16 | 1.80 | 148.3 | 1.0 | 5077 | 1.1e5 |
| | k=32 | 0.46 | 2.32 | 14.1 | 7.4 | 732 | 5.9e5 |
| | k=8 | −2.49 | 2.55 | 6.0 | 26.8 | 579 | 6.8e5 |
| | k=4 | 1.34 | 1.45 | 3.6 | 53.7 | 680 | 1.0e6 |
| | **k=2** | −1.61 | 0.65 | 2.0 | 106.6 | 1019 | **1.5e6** |

| σ = 32 | majorant | bias z | relVar | lookups | ns/est | eff |
|---|---|---|---|---|---|---|
| delta | global / k=16 / **k=8** / k=4 | −0.37 / −0.19 / −1.69 / 1.83 | 21-28 | 88.4 / 8.0 / 4.1 / 2.4 | 3127 / 419 / **319** / 318 | 1.5e4 / 1.2e5 / 1.2e5 / 1.2e5 |
| ratio | global / **k=32** / k=8 / k=2 | −1.31 / −0.10 / 0.38 / −0.82 | 0.59 / 1.16 / 2.77 / 7.54 | 576 / 56 / 25 / 14.5 | 19179 / 2156 / 1387 / 1596 | 8.9e4 / **4.0e5** / 2.6e5 / 8.3e4 |
| residual | global / k=32 / k=8 / **k=2** | −0.76 / 0.08 / 0.99 / 0.33 | 0.79 / 1.22 / 2.16 / 1.17 | 576 / 56 / 24 / 7.7 | 19144 / 2121 / 1316 / 1290 | 6.6e4 / 3.9e5 / 3.5e5 / **6.6e5** |

Reading (meas):
- A spatial majorant is indispensable: 5-10x for free flights over a global majorant.
- Delta tracking (free flights, used on every segment of every path) has its minimum cost at k = 8-16.
- Ratio tracking prefers looser cells, because more lookups mean lower variance per estimate.
- Residual ratio tracking prefers fine cells, where the cell minimum is a good control.
- The earlier agent's run of E1 gave identical bias and relVar columns (deterministic) and timings within 3 %.
- A trilinear lookup costs about 28 ns and a majorant-cell DDA step about 14 ns (E9 and the delta rows).

### 2.3 E2: NanoVDB node statistics as majorants

2 M uniform points in the box, trilinear density at each (meas):

| bound | samples | above the bound | worst ratio |
|---|---|---|---|
| leaf max (`getNodeInfo` level 0) | 459,036 in leaves | 1687 (0.37 %) | 17.2x |
| tile value, outside leaves | 1,540,964 | 364 (0.02 %) | |
| dilated majorant grid k=8 | 2,000,000 | **0** | |

(code) For a coordinate in a tile of an internal node, `getNodeInfo()` returns that node's min/max/avg over its whole extent (`GetNodeInfo::get(const NanoUpper&/NanoLower&, n)`, `NanoVDB.h:8197-8204`; `NANOVDB_NEW_ACCESSOR_METHODS` is defined in 32.6). It does not return the tile value. Node stats cover active values only.

Conclusion: node stats or HDDA over the tree can serve for empty-space skipping, but a majorant needs the one-voxel dilation (design).

### 2.4 E3: free-flight distribution

Delta tracking, majorant grid k=8, 200,000 flights on each of 3 rays (T(∞) = 0.118, 0.047, 0.730) and 3 seeds (meas):
- Escape share equals T(∞) within binomial noise (e.g. 0.1176 vs 0.1182).
- Max |F − F̂| over 200 quantiles of the collision distances against 1 − T(t): 0.0011-0.0024 for seeds 1 and 2, and 0.0032 once for seed 0, against a 95 % critical value of 0.0030. That is consistent with an exact sampler.

### 2.5 E4: integrators in a thermal-equilibrium furnace

**Setup** (proto):
- The test grid inside a uniform environment L = 1. The medium has σ_t,c = density·σ·kext_c, albedo_c and HG(g). Emission is σ_a·L, so in equilibrium every pixel must be exactly 1 in every channel, at unlimited depth.
- Every integrator does NEE towards the environment (uniform sphere sampling) with MIS against HG sampling, and Russian roulette.
- The non-emissive variants have no reference; the integrators must agree with each other.

**Integrators**:
- **(A)**: pbrt-v4 VolPath structure (Miller et al. 2019). Hero channel picked uniformly per path; per-channel majorants; `r_u`/`r_l` rescaled path probabilities through the whole path (including NEE); emission at every tentative collision; absorption ends the path.
- **(B)/(B2)**: one common majorant (max over channels). Spectral tracking (Kutz et al. 2017) with event probabilities ∝ max (B) or average (B2) of the throughput-weighted coefficients. Ratio-tracked NEE, direction-only MIS.
- **(B3)**: as B2, but absorption is never sampled (P_a = 0), so the null weight carries it (survival biasing), plus in-flight roulette below 0.1.
- **(C)**: the earlier agent's proposal. Hero free flights with path-level `r_u`, residual-ratio-tracked NEE, direction-only MIS.
- **(D)/(D2)**: hero and spectral MIS *per free flight* (no path state), hero uniform (D) or ∝ throughput (D2). Residual NEE.

**Size and metrics**: seed 6 at scale 64 shown (1.64 M paths for the σ = 8 cases, 410 k for the dense ones); seeds 3, 4, 5 and 7 give the same picture.
- avgVar is the mean of the per-channel per-path variances.
- µs is per path and per thread, with ±10 % timing noise between runs.
- Eff is (avgVar·µs) of A divided by that of the integrator; above 1 means better than pbrt-v4's scheme.

| case | | A | B | B2 | C | D | D2 | **B3** |
|---|---|---|---|---|---|---|---|---|
| grey furnace (σ 8, albedo 0.9, g 0.5) | mean | 1.0002 | 1.0001 | 0.9991 | 1.0007 | 1.0001 | 1.0000 | 0.9997 |
| (±0.0005; B3 ±0.0003) | avgVar | 0.355 | 0.349 | 0.348 | 0.319 | 0.347 | 0.347 | **0.191** |
| | µs | 1.74 | 1.59 | 1.53 | 1.74 | 1.69 | 1.68 | 1.77 |
| | eff | 1 | 1.11 | 1.16 | 1.11 | 1.05 | 1.06 | **1.83** |
| chromatic furnace (kext 1/0.45/0.15, albedo 0.95/0.7/0.99) | mean R | 0.9988 | 0.9991 | 0.9985 | 1.0008 | 1.0025 | 1.0007 | 1.0011 |
| (±0.0011 R, ±0.0005 G, ±0.0008 B) | avgVar | 1.087 | 0.926 | 0.815 | 1.036 | 8.02 | 1.071 | **0.589** |
| | µs (lookups/path) | 1.18 (5) | 1.41 (10) | 1.29 (9) | 1.20 (6) | 1.08 (5) | 1.41 (6) | 1.65 (10) |
| | eff | 1 | 0.98 | 1.22 | 1.03 | 0.15 | 0.85 | **1.32** |
| dense chromatic furnace (σ 32, albedo 0.99, g 0.7) | mean R/G/B | 1.000/1.000/0.997 | 1.000/1.001/0.999 | 0.996/1.002/1.000 | 0.997/1.000/0.996 | **0.89**/1.00/1.00 | 0.999/1.005/0.997 | 1.001/1.003/1.000 |
| (±0.0025-0.003 R) | avgVar | 2.03 | 2.54 | 2.08 | 1.76 | 177 | 2.47 | 1.86 |
| | µs (lookups/path) | **3.92 (32)** | 5.92 (74) | 5.63 (71) | 6.11 (70) | 3.87 (42) | 5.37 (59) | 6.08 (74) |
| | max sample (R) | 18 | 163 | 115 | 15 | 11298 | 102 | 108 |
| | eff | **1** | 0.53 | 0.68 | 0.74 | 0.01 | 0.60 | 0.70 |
| grey, no emission | mean | 0.7976 | 0.7974 | 0.7966 | 0.7981 | 0.7976 | 0.7973 | 0.7970 |
| | avgVar / µs | 0.266 / 1.92 | 0.263 / 1.86 | 0.261 / 1.56 | 0.240 / 1.84 | 0.260 / 1.73 | 0.260 / 1.77 | **0.152** / 1.75 |
| | eff | 1 | 1.04 | 1.25 | 1.16 | 1.14 | 1.11 | **1.92** |
| dense chromatic, no emission | mean R/G/B | 0.918/0.960/0.983 | 0.918/0.961/0.985 | 0.915/0.962/0.986 | 0.915/0.960/0.982 | **0.817**/0.961/0.984 | 0.917/0.964/0.983 | 0.919/0.963/0.986 |
| | avgVar / µs | 1.87 / **3.90** | 2.30 / 6.23 | 1.87 / 5.75 | 1.61 / 6.32 | 168 / 3.88 | 2.23 / 5.35 | 1.68 / 5.97 |
| | eff | **1** | 0.51 | 0.68 | 0.72 | 0.01 | 0.61 | 0.73 |

**Reading** (meas):
- **Bias**: all integrators except D are unbiased within 3σ over seeds 3-7. For B3 the largest deviation is −2.0σ.
- **D (uniform per-flight hero)**: its weights compound across flights. The resulting heavy tail produces the low means in the dense cases (0.82-0.89 instead of 0.92/1.00): technically unbiased, practically unusable.
- **B3** is the most efficient in grey and moderately chromatic media, with or without emission.
- **A** is the most efficient in dense, strongly chromatic, high-albedo media, because of its hero majorant (fewer lookups). Its tail is also the lightest (max 18 against about 100 for the common-majorant methods).
- **Combination**: survival biasing could be combined with A. Survival biasing is local to the tracking loop; A is not local (section 4.3).

### 2.6 E5: single scattering, homogeneous sphere (reference values for a unit test)

**Setup**: unit sphere at the origin, σ_t = 1.5, albedo 0.7, HG g = 0.3. Distant light of irradiance E = 1 from normalize(1, 1, 0.3). Black environment. Camera rays along +z at x = impact. Radiance from single scattering (meas):

| impact | quadrature (2e5 midpoints) | path tracer, 32 Mi samples (seed 3) | z (seeds 3 / 4) |
|---|---|---|---|
| 0.0 | 0.015118 | 0.015116 ± 0.000001 | −1.30 / 0.33 |
| 0.5 | 0.026927 | 0.026927 ± 0.000002 | 0.21 / −0.70 |
| 0.9 | 0.033980 | 0.033978 ± 0.000004 | −0.46 / 0.18 |

### 2.7 E6: point light in a homogeneous medium (single scattering)

**Setup**: ray of length 10, point light at distance h from the ray at t = 4, intensity 1. Relative variance of distance sampling, equiangular sampling, and one-sample-each MIS (power heuristic), at an equal sample count (meas):

| σ_t | h | reference | relVar distance | relVar equiangular | relVar MIS |
|---|---|---|---|---|---|
| 0.01 | 0.05 | 0.047638 | 15.2 | 1.6e-5 | 0.069 |
| 0.01 | 0.50 | 0.0044389 | 1.32 | 1.3e-4 | 0.119 |
| 0.10 | 0.05 | 0.32762 | 15.1 | 1.0e-3 | 0.072 |
| 0.10 | 0.50 | 0.028406 | 1.35 | 8.3e-3 | 0.134 |
| 1.00 | 0.05 | 0.08114 | 104 | 0.017 | 0.056 |
| 1.00 | 0.50 | 0.00466 | 10.2 | 0.144 | 0.297 |

### 2.8 E7: emission estimators (absorbing, emissive "fire")

**Setup**: σ = 8, emission σ_a·flame, 600 rays through the fire region, 64 estimates each. All five estimators are unbiased (meas):

| estimator | bias z | relVar | lookups | ns/est | eff |
|---|---|---|---|---|---|
| real collision (Le at the absorption point) | 0.18 | 9.31 | 3.1 | 334 | 3.2e5 |
| tentative collisions until absorbed (pbrt-v4) | −0.28 | 6.23 | 4.3 | 369 | 4.4e5 |
| **tentative collisions × ratio-tracked T, never absorbed** (= survival biasing) | 0.56 | 1.76 | 10.2 | 732 | **7.8e5** |
| jittered marching, 1-voxel steps, ratio-tracked T | −0.14 | 3.40 | 164 | 4538 | 6.5e4 |
| jittered marching, 4-voxel steps | 0.15 | 2.24 | 45 | 1626 | 2.8e5 |

### 2.9 E8: an emissive volume as a light

**Setup**: irradiance at points 0.1·W below the fire, centred (x = 0.5 W) and off to the side (x = 0.2 W). Absorbing emissive medium as in E7. 262,144 samples per strategy. Three strategies (meas):
- **Unidirectional**: cosine direction, then tentative-collision emission with ratio-tracked T along the ray.
- **NEE**: a cell of the k=8 power grid picked ∝ an upper bound of its σ_a·Le content, a uniform point in it, ratio-tracked T to it.
- **MIS**: both, balance heuristic in the volume measure, with p_U = pdf_dir·σ_maj/r² and p_N = P_cell/V_cell.

| point | E (unidir / NEE / MIS) | variance (unidir / NEE / MIS) | time µs | efficiency (unidir / NEE / MIS) |
|---|---|---|---|---|
| centre | 0.2884 / 0.2870 / 0.2856 | 0.908 / 0.394 / 0.295 | 0.36 / 0.21 / 0.58 | 3.1e6 / **1.2e7** / 5.9e6 |
| side | 0.0597 / 0.0587 / 0.0596 | 0.178 / 0.0132 / 0.0132 | 0.25 / 0.22 / 0.47 | 2.3e7 / **3.4e8** / 1.6e8 |

232 of 4845 cells are non-zero. In the integrator the unidirectional ray is traced anyway, so the cost that counts is NEE's, and the MIS variance is the relevant figure (design).

### 2.10 E9: lookup cost and stochastic trilinear

1 M random points along rays, single thread, 4 seeds (meas):
- Cost per lookup: trilinear **28.2-29.4 ns**; stochastic trilinear (one voxel drawn with the trilinear weights, 3 uniforms) **9.2-9.3 ns**; nearest 8.4-9.0 ns.
- Ratio tracking at k=8, σ = 8, on the same 600 rays:

| lookups | bias z (3 seeds) | relVar | ns/estimate |
|---|---|---|---|
| stochastic | −1.68, −0.93, −1.25 | 4.41, 4.82, 3.91 | 473-484 |
| trilinear | 0.18, 0.42, −0.87 | 4.21, 4.49, 3.85 | 569-587 |

The stochastic z values from these 3 seeds plus 4 earlier runs (same rays, other random streams: −0.82, −0.09, 0.98, 0.36) combine to −1.3: unbiased.

### 2.11 Blackbody (`prototypes/va_blackbody.py`)

**Method** (meas): Planck → CIE 1931 XYZ (Wyman et al. 2013 fit) → linear Rec.709, normalized to Y = 1.

| T [K] | 800 | 1000 | 1500 | 2000 | 3000 | 4000 | 6500 | 10000 |
|---|---|---|---|---|---|---|---|---|
| r | 4.865 | 4.319 | 3.175 | 2.478 | 1.755 | 1.406 | 1.042 | 0.873 |
| g | −0.039 | 0.124 | 0.461 | 0.660 | 0.849 | 0.926 | 0.984 | 0.994 |
| b | −0.092 | −0.095 | −0.066 | 0.019 | 0.271 | 0.533 | 1.035 | 1.431 |

Relative intensity from 1500 K to 3000 K under each convention:
- Stefan-Boltzmann T⁴ (Cycles' Principled Volume factor): 16x.
- Photometric luminance of Planck: 3892x.
- Luminance of pbrt-v4's peak-normalized spectrum: 122x.

The conventions differ by orders of magnitude, so the normalization must match Karma's (companion report).
- The colours below about 1900 K are out of the Rec.709 gamut (negative b).
- LUT error: 1.3e-2 (32 entries), 3.4e-3 (64), 8.6e-4 (128), 2.2e-4 (256).

---

## 3. Nova today: the integration points (question 1)

### 3.1 Wavefront stages

(code `core/integrator/wavefront.{h,cpp}`, `renderer.cpp`)
- **Waves**: one sample per pixel of a 32×32 tile (`kTileSize = 32`, `renderer.cpp:20`) or of a span of the active-pixel list (M7 D5); one `WaveQueues` per TBB thread.
- **Per depth** (`renderWave`, `wavefront.cpp:1552-1561`):
  1. `intersectRays`: batched Embree closest hit.
  2. `shadeSurfaces`, in three passes:
     - pass A `gatherLights`: Beer absorption inside media, camera-visible lights, area lights crossed by scattered rays (MIS with `bsdfPdf`), escapes to domes and distant lights; survivors are listed with their material;
     - pass B `sortByMaterial`;
     - pass C `shadeHits`: VM batches per material, then `shadeHit`: closure, emission, one NEE light, continuation.
  3. `traceShadowRays`: Embree `occluded`, fake-caustic transmittance, LPE parts.
  4. AO rays (depth 0).
- **Ray budget**: every path has at most one ray and one shadow ray per bounce.
- **Path state** (`WaveQueues`):
  - `beta` (RGB), `radiance`, `pixelSeed`, `sampleIndex`;
  - `bsdfPdf` (< 0 after a delta event), `diffuseShare` (light multipliers), `etaScale` (RR);
  - **`sigmaA`/`inMedium`**: the absorbing dielectric a path is in. Set at refraction by the authored orientation (`wavefront.cpp:1298-1303`), with no stack: leaving sets σ_a = 0, and nested dielectrics are not tracked. Applied as exp(−σ_a t) per segment in pass A (`:738-745`). "A ray that leaves the scene from inside an absorbing medium is lost."
  - `coneWidth`/`coneSpread`;
  - `eventCounts`: **a byte each for diffuse, reflect, refract and sss**;
  - `limitDither`, `lightOnly`, `afterDiffuse`, `lpeState`.
- **Ray limits** (`core/scene/ray_limits.h`):
  - Karma semantics: within the limit a full ray, past it a light-only ray that gathers lights and ends; a fractional limit is dithered.
  - `LimitClass::Volume` and `RayLimits::limit[4]` exist, and the Hydra defaults are Karma's (**volume 0**).
  - `shadeHit` skips the limit logic for `RayClass::Volume` bounces (`:1250`), which only VEX closures produce today.
  - The volume counter needs a fifth byte: widen `eventCounts` to 64 bits or add a `uint8_t` array (design).
- **MIS and lights** (`core/lights/lights.h`):
  - One light per bounce is chosen by power (`LightSampler`, `chooseLight` with the 3D stream `kLight`). Lights are analytic, never block rays, and are found by `areaLightsAlongRay`.
  - `lightSampleWeight`/`bsdfSampleWeight` use the power heuristic; delta lights and delta events get weight 1. `LightStrategy` offers MIS, LightOnly and BsdfOnly.
  - Each light has diffuse and specular multipliers, weighted by `diffuseShare`.
  - Emissive surfaces are reached only by BSDF sampling "until M9" (`wavefront.cpp:1138`).
- **RR**: `survive = clamp(maxComponent(beta)·etaScale, 0.05, 0.95)` from `rrStartDepth` (3) on, with stream `kRoulette` (`:1282-1288`).
- **Clamp**: `clampIndirect` (10) for light samples from the second event on, and for lights or emission reached after two or more events (`ShadeContext`, `:602-606`, `:1414-1415`). A NaN or Inf contribution counts as black (`addRadiance`).
- **Sampling** (`core/math/sobol.h`):
  - Owen-scrambled Sobol, stateless: a sample is f(pixelSeed, index, stream).
  - Each bounce has 8 stream slots (`bounce(depth, d)`): `kLight` 3D, `kBsdf` 3D, `kRoulette`, `kOpacity`, `kLayer`, **`kMedium = 5` (reserved for volumes)**, `kWalk`, `kTexture`.
  - Further streams live in blocks at 0x10000 (shading queries), 0x20000 (LPE split) and 0x30000 (AO).
  - `core/math/rng.h` has a 32-bit PCG-hash `Rng` "deterministic for (pixel, sample, seed)", so streams of unbounded length can be built from it.
- **LPE** (`core/film/lpe.h`, `docs/design/M6.md`): `TypeVolume` (V) exists. M6.md lists "Volume LPE events (`V`) | M8 | parsed; no volume events yet". Parts on shadow rays are multiplied by the shadow transmittance (fake caustics, `:1494-1501`), and the weight-1 sums (`unoccluded;`/`shadow;`) are kept separately.
- **Accel** (`core/accel/accel.h`): `IAccel` batched `intersect`/`occluded` (Embree now, OptiX later); `IHitFilter` is the any-hit analogue (stochastic opacity). Volumes need nothing from it (design: they are not Embree geometry).
- **Shading VM** (`core/shading/program.h`, `docs/design/M3.md`):
  - Bytecode over a register file of rows, one float per lane, run per material batch (`kShadeChunk`), with a uniform prologue per material.
  - Per-point volume shading would need one VM run per tentative collision (scalar). It would also need a majorant the program cannot provide.
  - `MaterialDesc` is surface-only.
- **Tests and gates**:
  - `tests/test_furnace.cpp`: furnaces through the full `Renderer`, uniform enclosure radiance 1, white Lambert.
  - `test_ray_limits.cpp`, `test_lpe_render.cpp`, `test_sampling.cpp`/`test_bsdf_stats.cpp` (χ² tests).
  - `docs/benchmarks.md`: per-milestone scene tables and gates such as "Opaque scenes ±3 %".
  - M7: bit-identical uniform renders at 1 and 16 threads, adaptive = uniform at equal counts.

### 3.2 What changes for volumes (summary of section 6)

| Place | Change |
|---|---|
| `renderWave` loop | new stage `traceMedia` between `intersectRays` and `shadeSurfaces`; skipped entirely if the scene has no volumes and no ray is `inMedium` |
| `gatherLights` | volume hits: no escape, lights along [0, t_s) already gathered by `traceMedia`; queued for `shadeVolumes` |
| new `shadeVolumes` (pass C') | phase, NEE, continuation, RR, Volume limit, LPE V |
| `traceShadowRays` | after `occluded`: ratio-tracked transmittance for unoccluded rays that cross volumes |
| `WaveQueues` | volume event counter (fifth limit byte); the volume id of a volume hit (reuse `hitPrim`); a flag "lights already gathered" |
| `Scene` | volumes (create/update/remove), commit builds majorants and the kernel list, light list entry for emissive volumes |
| `sobol.h` | `kMedium` used; a new block for shadow-ray tracking streams |

---

## 4. Algorithms (question 2)

### 4.1 Null-collision tracking

- **Delta (Woodcock) tracking** (lit: Woodcock et al. 1965):
  - Pad the heterogeneous σ_t with fictitious "null" matter up to a majorant σ̄, sample tentative collisions from the homogeneous σ̄, and accept each as real with probability σ_t/σ̄.
  - Unbiased for any σ̄ ≥ σ_t. Cost ∝ σ̄·length, so a tight majorant matters (E1: 5-10x).
  - In a majorant grid the flight restarts at every cell boundary; the exponential is memoryless (lit: pbrt-v4 `SampleT_maj`; proto `MajIter`).
- **Ratio tracking** (lit: Cramer 1978 in neutron transport, from memory; Novák, Selle, Jarosz 2014 in graphics): multiplies T by (1 − σ_t/σ̄) at each tentative collision instead of stopping. It is a continuous, unbiased transmittance estimator for shadow rays. Variance falls with a looser majorant at higher cost (E1: best at k=32).
- **Residual ratio tracking** (lit: Novák et al. 2014): a control σ_c ≤ σ_t (here the cell minimum) is integrated analytically, and only the residual is ratio-tracked: T = exp(−∫σ_c)·Π(1 − (σ_t − σ_c)/(σ̄ − σ_c)). It pays where the minimum is high (dense cores); for sparse smoke most cells have min 0 (E1: 1.6x at k=8, 3.5x at k=2 for σ = 8).
- **Decomposition tracking** (lit: Kutz et al. 2017): the same control idea for free flights. Sample a collision against σ_c analytically and against the residual by tracking, and take the earlier. Useful for dense, nearly homogeneous media; little gain for pyro (min ≈ 0) (design: not in M8).
- **Integral formulations and newer estimators** (lit): Georgiev et al. 2019 relate all of these (track-length, ratio, power-series, "next-flight"). Kettunen et al. 2021 give an unbiased ray-marching transmittance estimator with very low variance that needs a bound but not a strict majorant. Misso et al. 2023 handle non-bounding majorants (progressive null tracking). These are follow-ups if shadow-ray transmittance dominates; not needed for M8 (design).

### 4.2 Null-scattering path integral and spectral MIS

- **Miller, Georgiev, Jarosz 2019** (lit) write the null-collision estimators as a path integral whose vertices include null events. The path pdf then becomes explicit, so null-scattering paths can take part in MIS:
  - with each other, across wavelengths: **spectral MIS**;
  - with NEE, including the transmittance sampling along the shadow ray (`r_u`/`r_l` "rescaled path probabilities").
- **pbrt-v4's VolPathIntegrator** (lit: `cpu/integrators.cpp`, `media.h`):
  - Samples distances with the *hero* wavelength's majorant (`sigma_maj[0]`), with `SampleT_maj(ray, tMax, u, rng, lambda, callback)` iterating majorant segments (`HomogeneousMajorantIterator`, `DDAMajorantIterator`).
  - At each tentative collision it adds emission `beta·σ_a·Le/avg(r_e)`, then chooses absorb, scatter or null with probabilities σ_a/σ̄, σ_s/σ̄ and the rest, all of the hero.
  - Updates: scatter `r_u *= T_maj·σ_s/pdf`; null `r_u *= T_maj·σ_n/pdf`, `r_l *= T_maj·σ̄/pdf`.
  - Shadow rays use ratio tracking with `r_u`/`r_l` (RR q = 0.75 below Tr 0.05).
  - Surface events set `r_l = r_u/pdf_bsdf`, and every light contribution is divided by `avg(r_u + r_l)`.
  - So the spectral MIS spans the whole path and the whole integrator (proto A).
- **Spectral tracking** (lit: Kutz, Habel, Li, Novák 2017):
  - One common majorant (max over channels).
  - At each tentative collision, choose absorb, scatter or null with throughput-weighted ("history-aware") probabilities, and multiply beta by σ_x/(σ̄·P_x) per channel. Unbiased for any probabilities that are positive where the respective σ_x is.
  - The paper discusses max- and average-based variants (lit, from memory); E4 measured both (B, B2).
  - With P_a = 0, absorption becomes a weight (B3; the classic "implicit capture"/survival biasing of neutron transport, lit from memory: Lux & Koblinger 1991).
  - Local to the tracking loop: no state survives the flight except beta.
- **Hero wavelength** (lit: Wilkie et al. 2014): the origin of the one-sample spectral MIS. Nova is RGB, so the "hero" is one of three channels.
- **For Nova** (design, meas E4): **B3**.
  - It keeps the integrator's structure: beta stays an unbiased per-channel throughput, and NEE and continuation MIS use direction pdfs exactly as for surfaces (the prototype's direction-only MIS is unbiased; E4 grey and non-emissive cases).
  - In dense, strongly chromatic media it costs 1.4x against A.
  - Per-flight spectral MIS (D) must not be used. D2 is safe but no better than B3.

### 4.3 Why per-path spectral MIS is not local

(lit: Miller et al. 2019; proto A, C)
- The balance heuristic over heroes needs every channel's pdf of the *entire* path: the products of the per-segment ratios.
- Normalizing per segment instead (D) is still a valid one-sample MIS. However, a path whose flights were sampled by different heroes accumulates products of ratios that the per-path heuristic would have assigned to one channel. That is the measured heavy tail.
- Hence A requires `r_u` (and `r_l` for NEE) in the path state, and a division by `avg(r_u + r_l)` at every light contribution, surfaces included. In Nova that touches `gatherLights`, `shadeHit` emission and NEE, LPE parts and the clamp (code: section 3.1).

### 4.4 Majorants and empty-space skipping

- **pbrt-v4** (lit):
  - `GridMedium`/`RGBGridMedium`: a 16³ majorant grid over the medium bounds.
  - `NanoVDBMedium`: 64³. Each cell is mapped to index space, expanded by one voxel ("filter slop"), and set to the max of `getValue` over the covered voxels.
  - `DDAMajorantIterator` walks the grid (Amanatides & Woo 1987).
- **Cycles 5.0** (lit): an octree (`intern/cycles/bvh/octree.{h,cpp}`) built by evaluating the volume *shader* density on a dense grid (`evaluate_volume_density`). Nodes keep min/max σ; a node splits while `sigma.range()·len(size)·scale > 1.442` ("ideally only one sample per node"). It is traversed in `shade_volume.h` (`volume_octree_advance`, `volume_majorant_optical_depth`).
- **Others** (lit, from memory): Yue et al. 2010 build kd-tree partitions for adaptive majorants. Szirmay-Kalos et al. 2011 use low-resolution max grids.
- **NanoVDB's `HDDA`/`TreeMarcher`** (`util/HDDA.h`) skip empty space hierarchically (code). Their per-node stats are not trilinear majorants (meas E2).
- **For Nova** (design): a dense grid of per-cell max (and optionally min) *unscaled* density.
  - k = 8 voxels per cell, aligned to the density grid's index space (leaf-aligned).
  - Each cell covers voxels [lo, lo+k] per axis, which is the trilinear support.
  - Built by scattering the stored values of the NanoVDB tree (leaf values and non-zero tiles, the latter clamped to the active bbox) into the cells they influence: O(stored values), 6 ms for 1835 leaves single-threaded (meas), parallel over leaves in Nova.
  - k doubles until the grid has at most 128³ cells (2 M floats, 8 MB at worst; a 1024³-voxel domain still gets k = 8).
  - The run-time majorant is σ̄ = maxDensity·max_c(σ_t,c) of the volume material, plus the floors in section 6.2, summed over overlapping volumes.
  - A non-monotone density remap (a ramp) takes the max of the remap over the cell's [min, max] density.

### 4.5 Emission and blackbody

- **Conventions**: pbrt-v4 multiplies the medium's `Le` by σ_a (lit: `L += betap·σ_a·Le/avg(r_e)`). Cycles adds emission per unit length, independent of σ_a (lit: null scattering adds `throughput·emission`; `volume_emission_integrate` uses E·(1 − e^{−σ_t t})/σ_t).
- **Estimator** (design): Nova's estimator must handle both, which means emission ε(x) per length.
  - Collected at tentative collisions as `beta·ε/σ̄` (E7, unbiased).
  - This needs **σ̄ > 0 wherever ε > 0**, also where density is 0 (a flame field without density). The majorant cell takes max(σ̄_density, κ_e) in cells with emission, with κ_e = 1/voxel size: about one tentative collision per voxel, the cost of E7's jittered 1-voxel march, but unbiased.
- **Blackbody**: see 2.11. Cycles' Principled Volume intensity is `σ_SB·1e-6/π·T⁴` times a luminance-1 Rec.709 colour from a fit valid over 800-12000 K (lit). pbrt-v4's `BlackbodySpectrum` is normalized to a peak of 1 (lit). Arnold says "intensity 1 must be used" for physically correct results (lit).
- **For Nova** (design): a 128-entry LUT in 1/T of the luminance-normalized working-space colour (8.6e-4 error, meas), times an intensity law selected to match Karma (companion report). Negative out-of-gamut channels below about 1900 K are clamped to 0 or kept, again as Karma does.

### 4.6 Phase functions

- **Henyey-Greenstein** (lit: Henyey & Greenstein 1941): p(cos θ) = (1−g²)/(4π(1+g²−2g cos θ)^{3/2}), with closed-form inversion (proto `hgSample`; the pdf equals the value).
- **Dual-lobe HG**: a mixture w·HG(g1) + (1−w)·HG(g2). Sampled by choosing a lobe with the z component of the `kBsdf` 3D sample (the same pattern as lobe picking for closures, M3 K20); the pdf is the mixture (design).
- Cycles 4.x also offers Fournier-Forand, Draine and Rayleigh phase functions (lit, from memory). Not needed for Karma parity unless Karma's shaders expose them (companion report).

### 4.7 Light transport inside volumes

- **NEE from volume points** (lit: pbrt-v4 `SampleLd`; Fong et al. 2017): the same light choice as for surfaces, the phase function instead of the BSDF, and a shadow ray whose transmittance through all media is estimated by ratio tracking (per channel, common majorant) after the surface occlusion test.
- **Phase/light MIS**: power heuristic on solid-angle pdfs (proto: direction-only MIS, unbiased in E4). Nova's `lightSampleWeight`/`bsdfSampleWeight` apply as they are, with the phase pdf in `bsdfPdf`.
- **Equiangular sampling** (lit: Kulla & Fajardo 2012): samples the scattering distance along a ray ∝ 1/r² to a point light, combined with distance sampling by MIS.
  - Huge gains in thin media near point lights (E6). In heterogeneous media it needs ratio-tracked T to the sampled point and σ_s there.
  - Production use (lit): Cycles 5.0 (`volume_equiangular_sample`, `volume_direct_scatter_mis`); RenderMan PxrVolume "Equiangular Weight" (default 0.5, disabled for multi-scatter volumes since 21.5).
  - Design: defer.
- **Emissive volumes as lights** (lit: Villemin & Hery 2013, "Practical illumination from flames", importance sampling of emissive voxels; Simon et al. 2017, line integration):
  - pbrt-v4 does not light-sample emissive media; its lights and media are separate (lit, from memory). Cycles: not verified for 5.0.
  - Design: a `LightType::Volume` entry per emissive volume. Its power for light selection is 4π·Σ ε·ΔV (luminance). It is sampled by a CDF over majorant cells (cell power = Σ ε over the cell's voxels, dilated support), with a uniform point in the cell.
  - MIS uses p_N = pmf·P_cell/V_cell against p_U = pdf_dir·σ̄(y)/r² for collision emission found by continuation rays (E8; any deterministic partition of unity is unbiased).
- **Overlapping volumes** (lit: Cycles volume stack sums the shaders on the stack; 5.0 notes "removes blocky artifacts in overlapping volumes"; design):
  - σ and ε add, and majorants add over the overlap.
  - A real collision is attributed to volume i with probability σ_s,i/Σσ_s, which gives its phase function and LPE tag.
  - The DDA merges the boundaries of up to 4 overlapping volumes' cell iterators. Beyond that, the extra volumes contribute their global majorant.
- **Volumes and glass** (code, design):
  - VDB volumes are found geometrically, so a VDB inside or behind glass needs no state.
  - The dielectric's own absorption (`sigmaA`) has no scattering, so its transmittance is analytic and multiplies beta up to the event distance or the segment end. Glass Beer absorption moves from pass A into `traceMedia`.
  - Shadow rays from inside glass are blocked by the glass, as today (fake caustics aside).
  - Cycles' volume stack (lit: `volume_stack.h`, enter on front-face hit, exit on back-face, "all volumes are closed manifolds", camera stack initialized by a probe ray) is only needed for mesh-bounded volumes. Deferred (design).
- **Velocity motion blur** (lit: Karma XPU supports only the "Lookup Offset" mode, companion info; Eulerian motion blur, Kim & Ko 2007, from memory; RenderMan PxrVolume velocity "relative to the entire shutter range"):
  - Design: lookups at x − v(x)·(time − t₀)·shutter, where the ray's normalized `time` already exists in `RayQueue`.
  - The majorant cell dilates by ceil(max|v|·shutter/voxel) voxels; v_max comes from the velocity grid's stats.
  - Object transform motion uses the per-ray time like meshes.
- **Lookups**: trilinear (`SampleFromVoxels` order 1); tricubic later if Karma uses it (companion report).
  - Stochastic trilinear (E9) for density as an M8.5 optimization. Cycles 5.0: "Linear and cubic interpolation are now stochastic where possible" (lit).
  - Not for nonlinear maps (temperature → blackbody), where stochastic lookup changes the filter rather than adding noise.

### 4.8 Production practice

(lit unless marked)
- **Cycles (Blender 5.0)**:
  - The default volume algorithm is null scattering ("unbiased, removes blocky artifacts in overlapping volumes"; no step size, max steps or homogeneous settings). The old ray marcher remains under Volume > Biased.
  - Octree majorants from shader evaluation. Ratio-tracked shadows "with MIS applied" (`volume_shadow_null_scattering`). Equiangular + distance MIS.
  - Volume Scattering Probability Guiding (Xu et al. 2024). Spectral MIS citing Miller 2019 and Kutz 2017. Stochastic linear/cubic interpolation. Smoke and fire through NanoVDB.
  - Performance "depends on the scene".
- **pbrt-v4**: section 4.2, hero spectral MIS, 64³ DDA majorant for NanoVDB.
- **Arnold `standard_volume`**: ray marching ("once per ray march sample", `AtVolumeData::auto_step_size`). Blackbody intensity 1 is physical. Emission modes None, Opacity and Thermodynamic.
- **RenderMan PxrVolume**: "Max Density" is a user-supplied majorant ("must be higher than any density encountered" for unbiased rendering). Equiangular weight. Aggregate volumes with min/max density ranges (`RixVolumeServices::GetDensityRanges`). "Delta tracking" does not appear in the public docs.
- **Karma**: CPU and XPU ray-march, with the Volume Step Rate ("voxels a ray travels before another sample") and a coarser Volume Shadow Step Rate.
  - Karma XPU supports only the Pyro Preview, Karma Volume and Whitewater VOPs, and volume motion blur only in "Lookup Offset" mode.
  - Nothing public on majorants, tracking or volume light sampling.
  - Consequence (design): Nova's unbiased result equals Karma's in the limit of small step rates. Karma comparisons must use a fine step rate, or accept Karma's discretization bias as tolerance.

---

## 5. NanoVDB on the CPU now and on CUDA later (question 3)

### Version and headers

(code `$HFS/toolkit/include`)
- `NANOVDB_MAJOR/MINOR/PATCH = 32.6.0`, beside OpenVDB 13.0.0 (`openvdb/version.h`).
- 32.6 shipped with OpenVDB 11.0. OpenVDB 12.0 moved to 32.7, with the reorganization to `math/`, `tools/`, `io/` and `cuda/` and the `nanovdb::tools` namespace; 13.0 is 32.9 (lit). Houdini 22.0.466 therefore ships an older NanoVDB API than its OpenVDB.
- The grid buffer format is compatible within major version 32 in both directions (lit: `GridData::isValid` accepts both magic numbers).
- Nova can build against `$HFS` NanoVDB 32.6 now. A switch to a vendored 32.9 later would change includes and namespaces but not data.
- `NANOVDB_NEW_ACCESSOR_METHODS` and `NANOVDB_USE_SINGLE_ROOT_KEY` are defined.
- The prototype needs `/Zc:__cplusplus` under C++20: otherwise `GridBuilder.h` falls back to `std::result_of`, which was removed in C++20 (proto `va_build.bat`).

| Header (util/) | Use |
|---|---|
| `CreateNanoGrid.h` | `createNanoGrid(srcGrid, StatsMode, ChecksumMode)` from `build::Grid`, NanoGrid, or `openvdb::Grid` (the latter only with `NANOVDB_USE_OPENVDB`); `openToNanoVDB` |
| `GridBuilder.h` | `nanovdb::build::Grid<T>` (functor fill, `setTransform`) |
| `SampleFromVoxels.h` | `SampleFromVoxels<Acc, order 0-3, useCache>`; trilinear stencil at `Floor(xyz)` and `+1`, voxel centres at integer index coordinates |
| `HDDA.h` | `HDDA`, `DDA`, `ZeroCrossing`, `firstActive`, `TreeMarcher`, `PointTreeMarcher` |
| `Ray.h`, `Stencils.h`, `NodeManager.h`, `GridStats.h`, `GridChecksum.h`, `GridValidator.h`, `IO.h` (`.nvdb`), `Primitives.h`, `NanoToOpenVDB.h` | as named |
| `cuda/` | `CudaDeviceBuffer.h`, `CudaGridHandle.cuh`, `CudaPointsToGrid.cuh`, `CudaIndexToGrid.cuh`, `CudaGridStats.cuh`, `CudaGridChecksum.cuh`, `CudaNodeManager.cuh`, `CudaSignedFloodFill.cuh`, `CudaAddBlindData.cuh` |
| `PNanoVDB.h`, `CNanoVDB.h` | C, HLSL and GLSL ports of the read side |

### Layout and memory

(code, lit: Museth 2021)
- One contiguous, pointer-free buffer: GridData (incl. map), TreeData, Root (sorted tiles keyed by coordinate), Upper internal nodes (32³ children, 4096³ voxels), Lower (16³, 128³ voxels), Leaves (8³ voxels).
- Each node has a bbox, masks, min/max/avg/stddev of its active values, and children by offset.
- A float leaf stores 512 values: dense within the leaf, sparse above.
- Measured: 4.2 MB for 587 k active voxels in 1835 leaves, about 7.5 bytes per active voxel at 62 % leaf occupancy (meas).
- Quantized value types (`Fp4/8/16/FpN`) shrink the value storage 2-8x by bit width (code: the build types exist; not measured).

### Lookups and threads

- `ReadAccessor` caches the path to the last node(s) and is *mutable*: one per thread or lane, never shared. The grid is immutable after building and safe to share (code; meas E4 with 8 threads).
- Building an accessor costs a few pointers, so on the GPU each thread makes one in registers (code: everything is `__hostdev__`).
- Trilinear costs 28.5 ns per lookup, stochastic 9.2 ns (meas E9).

### Majorant grid build

- Iterate leaf values and internal or root tiles (`getFirstLeaf`/`getFirstLower`/`getFirstUpper`, `isChild`, `offsetToGlobalCoord`), and scatter each value region into cells (proto `buildMajorantGrid`, 6-7.6 ms, meas).
- Node stats alone are not conservative (E2).

### CUDA

(design; code for the APIs)
- Upload the grid buffer as-is (`GridHandle<CudaDeviceBuffer>` or a plain `cudaMemcpy`), the majorant array, the material PODs and the LUT.
- The tracking loop is the same `NOVA_HOST_DEVICE` source.
- Divergence comes from data-dependent tracking lengths (here 5-74 lookups per path). Wavefront mitigations:
  - only rays with volume intervals enter the medium kernel;
  - sort by volume;
  - optionally a tracking-step kernel that suspends after N steps and re-queues.

---

## 6. Design proposal for Nova (question 4)

### 6.1 Scope of M8 (design)

**In**:
- NanoVDB float fields from Houdini volume prims (companion report).
- Heterogeneous density with a fixed-function pyro/volume material: scattering, absorption, emission from temperature, flame or colour fields, blackbody.
- HG phase.
- Unbiased tracking with majorant grids, NEE from volume points with transmittance shadow rays (also for surface NEE through volumes), MIS.
- Overlapping volumes, volumes inside or behind glass, transform motion blur and velocity blur (lookup offset).
- Ray limits, LPE, RR and clamp integration.
- Emissive volumes as lights (MIS).
- Constant-density boxes ("fog box") as a special case of the same code: one cell, constant lookup.

**Deferred**:
- Equiangular sampling.
- Mesh-bounded uniform volumes (they need a medium stack).
- Per-point VM volume shaders.
- Hero-channel path MIS.
- Residual tracking for free flights (decomposition).
- Path guiding.
- Tricubic lookups (unless Karma uses them).

### 6.2 core/volume data structures (design)

```cpp
// Host side (Scene): one record per volume prim.
struct VolumeField {            // one NanoVDB float grid
    std::shared_ptr<const nanovdb::GridHandle<nanovdb::HostBuffer>> grid;
    Affine3f indexToObject;     // from the grid's map (companion report: voxel centres at integer indices)
};
struct VolumeMaterial {         // fixed function, POD (names follow Karma's pyro/volume shaders: companion report)
    float densityScale; Spectrum scatterColor, absorbColor;  // sigma_s = d*scale*scatter, sigma_a = d*scale*absorb
    float g1, g2, lobeMix;                                   // HG / dual-lobe HG
    uint8_t emissionMode;       // none | field*color | blackbody(temperature) [* flame]
    float emissionScale, kelvinScale, kelvinOffset; Spectrum emissionTint;
    float velocityScale;        // lookup-offset motion blur
    int8_t field[4];            // density, temperature, flame, velocity (-1: none)
};
struct VolumeDesc {
    std::vector<VolumeField> fields; VolumeMaterial material;
    std::vector<Mat4> motion;   // object-to-world keys over the shutter
    RayLimitOverrides rayLimits; std::string lpeTag; uint32_t cryptoObject; bool visible;
};

// Kernel side (POD, GPU-uploadable).
struct MajorantGrid {           // over the density grid's index space
    int32_t lo[3], res[3], k;   // cell c covers voxels [lo + c k, lo + (c+1) k] (trilinear support)
    const float *maxDensity;    // unscaled; sigma_bar = maxDensity * max_c(sigma_t scale) at run time
    const float *minDensity;    // optional (residual ratio tracking)
    const uint8_t *emissive;    // optional: cell has emission -> rate floor 1/voxel
};
struct VolumeData {
    Affine3f worldToIndex[kMaxMotionKeys]; uint32_t motionKeys; Bounds3f indexBounds;
    const nanovdb::NanoGrid<float> *grid[4]; Affine3f densityToField[4];
    MajorantGrid majorant; VolumeMaterial material;
    uint32_t lightIndex;        // its LightType::Volume entry, or kInvalidId
    const float *powerCdf;      // over majorant cells (emissive volumes)
};
```

- **Scene**: `createVolume`/`updateVolume`/`removeVolume`/`setVolumeVisible`, as for meshes.
- **`commit()`** (re)builds a volume's majorants and power CDF only when its density, velocity or emission fields change, and refreshes the kernel `VolumeData` array and the light list.
- **IPR**: material edits (colours, g, emission scale, density scale) never rebuild, because majorants are stored unscaled and power-CDF weights scale linearly. A blackbody or temperature-mapping edit rebuilds only the power CDF of that volume.

### 6.3 Integrator changes (design)

**Stage `traceMedia(scene, depth, q)`**: runs after `intersectRays`, before pass A. For each ray k with path p:
1. **Intervals**: test the ray against every visible volume's index-space bounds at the ray's time (slab test in each volume's index space; a small BVH over world AABBs once there are more than about 8 volumes), clipped to [0, tHit). No interval and not `inMedium` → nothing to do.
2. **Light-only rays and `depth == maxDepth` rays**: ratio tracking only (per-channel T into beta), the glass Beer factor, and emission (see 6.4). No events.
3. **Otherwise one spectral-tracking free flight** (B3) over the merged majorant segments:
   - the first exponential from `get1D(bounce(depth, kMedium))`, then `Rng` seeded by `hash(pixelSeed, sampleIndex, bounce(depth, kMedium))`;
   - at each tentative collision:
     - evaluate σ_s, σ_a and ε of all volumes covering the point;
     - add `beta·ε/σ̄` to the radiance (clamp and LPE as in 6.4);
     - choose scatter with probability P_s = a_s/(a_s + a_n), else null, where a_x = avg(beta·σ_x) and σ_n = σ̄ − σ_t (≥ 0 by construction). Absorption is never sampled;
     - multiply beta per channel by σ_s/(σ̄·P_s) on scatter or by σ_n/(σ̄·(1 − P_s)) on null. In a grey medium both factors equal 1 − σ_a/σ̄;
     - in-flight roulette when max(beta) falls below 0.1 of its value at the flight's start (survive 0.5, ×2).
4. **Area lights crossed at t_L < t_event** (rare: lights inside smoke): gathered *during* the flight with the current beta, using the existing `areaLightsAlongRay` on sub-intervals. A per-ray flag tells pass A that the lights are done.
5. **Glass absorption**: the analytic factor exp(−σ_a,glass·t_end) multiplies beta (moved here from pass A).
6. **Outcome**:
   - a real scatter at t_s: set `hitT = t_s`, `hitMesh = kVolumeHit`, `hitPrim = volume id`;
   - otherwise: unchanged hit or miss with the updated beta;
   - a path whose weight reached 0: dead.

**Pass A (`gatherLights`)**: volume hits skip escape handling, camera-light ties and the surface-material listing, and go to a volume list.

**Pass C' `shadeVolumes`**, per volume hit (no VM):
- **Phase**: HG or dual-lobe of the attributed volume, in a frame along the ray direction.
- **NEE**: `chooseLight(c, sampler, P, true)` at the event point. `f = phase(wo, wi)·light.<multiplier>`; which light multiplier applies to volume scattering is in the companion report. The weight is `lightSampleWeight(strategy, lightPdf, phasePdf, delta)`, and the contribution is clamped by `clampNee` as for surfaces. A shadow ray is pushed with origin P (no offset needed) and length `shadowRayLength(distance, tolerance)`.
- **Continuation**: direction from the 2D part of `bounce(depth, kBsdf)` (z picks the lobe for dual-lobe HG). Weight 1 for exact HG sampling; `bsdfPdf = phasePdf`; `diffuseShare` per the light-multiplier rule; `RayClass::Volume`.
- **Ray limits**:
  - class `Volume` with a fifth counter byte;
  - per-object overrides from the volume's `RayLimitOverrides`;
  - past the limit, a light-only ray. With Karma's default 0 that gives single scattering plus attenuated lights seen through the continuation (dome).
- **RR**: Nova's rule (`clamp(maxComponent(beta)·etaScale, 0.05, 0.95)` from `rrStartDepth`, stream `kRoulette`), the same as for surfaces.
- **LPE**: `lpeStep(t, s, TypeVolume, <scatter class per Karma>, volumeTag, 0)`; the parts machinery is unchanged.
- **AO, first-hit AOVs and alpha for volume hits**: alpha is 1 for an event; for rays that pass through, 1 − avg(beta_after/beta_before) adds to the surface or background alpha. Volume-specific first-hit AOVs follow Karma (companion report).

**`traceShadowRays`**:
- After `occluded()`, for each unoccluded ray that overlaps volumes: ratio tracking with the summed common majorant and per-channel `T *= 1 − σ_t,c/σ̄`, plus RR when max(T) < 0.05 (q = 0.75).
- Stream: `Rng` from `hash(pixelSeed, sampleIndex, kMediumShadowBase + depth)`, a new block like `kLpeSplitBase`.
- T multiplies the contribution and the LPE "lit" parts (`shadow;`), not the `unoccluded;` sums (consistent with fake caustics, `wavefront.cpp:1494-1501`).
- Surface NEE gets volume shadows the same way.
- Optional later: residual ratio tracking with the min grid (E1: 1.3-1.6x at k=8).

### 6.4 Emission, MIS for emissive volumes, clamp (design)

**Collection**:
- Collision emission `beta·ε/σ̄` is collected by every tracked ray (camera, continuation, light-only).
- For rays after a non-delta scattering event, and when the volume is an emissive light, it is weighted by `w_U = p_U²/(p_U² + p_N²)` with p_U = bsdfPdf·σ̄(y)/r² (r = distance from the ray origin) and p_N = selectPmf·P_cell/V_cell.
- Camera rays and rays after delta events use weight 1, as `bsdfSampleWeight` does.

**NEE of an emissive volume**:
- `chooseLight` picks the volume's light entry with the z of the `kLight` sample, as for every light. x then inverts `powerCdf` to pick a cell; its rescaled remainder, y and one `Rng` value give a uniform point in that cell.
- `contribution = beta·f·T(x→y)·ε(y)/r²/p_N·w_N`, with T by ratio tracking in `traceShadowRays` (the shadow ray ends at y).

**Clamp and LPE**:
- Emission seen after two or more events is clamped with `clampEscape`; direct emission is not, the same as surface emission (`wavefront.cpp:1140-1146`).
- LPE event for volume emission: Karma's (companion report; `O` with the volume's tag is the natural mapping).

### 6.5 Determinism and sampling (design)

- **Stream use**: the first free-flight decision is stratified (Sobol `kMedium`). The rest of the flight, the in-flight roulette and the scatter choices come from a PCG `Rng` seeded per (pixel seed, sample index, depth); shadow-ray tracking has its own block.
- **No dependence on scheduling**: results do not depend on thread count, wave composition or tile order. Adaptive sampling (pixel lists, per-pixel indices) stays bit-identical to uniform at equal counts.
- The proxy passes of IPR stay "first samples".
- **Gates**: render bit-identically at 1 and 16 threads (as M7.1), and adaptive threshold 0 = uniform.

### 6.6 CUDA port (M10) (design)

- Device data: `VolumeData[]`, majorant arrays, power CDFs and the blackbody LUT are flat; NanoVDB buffers upload unchanged.
- Kernels: `traceMedia` becomes one kernel over the rays with intervals, `shadeVolumes` another, and shadow transmittance runs in the shadow kernel after the OptiX occlusion query.
- No virtual calls, no VM in the tracking loop. Tracking state is about 64 bytes: t, DDA cell, tNext[3], beta, rng, segment majorant.
- If divergence hurts: split into a step-limited tracking kernel with re-queueing, and sort rays by volume.
- Volume AABBs can be OptiX custom primitives whose any-hit collects intervals, once scenes have many volumes.

### 6.7 Tests and gates (design)

Analytic and unit tests (Catch2, `tests/`):

| Test | Content | Gate |
|---|---|---|
| majorant conservativeness | E2 on a procedural grid and on `hv_pyro.vdb`: 2 M random points, trilinear ≤ majorant; dilation for velocity | 0 violations |
| transmittance unbiasedness | E1: ratio, delta and residual tracking vs exact trilinear optical depth | \|z\| < 3.5 over all rows |
| free-flight distribution | E3 KS test on 3 rays | KS < 1.63/√n (99 %) |
| homogeneous slab | absorbing box of σ_t, thickness d, in front of a uniform dome L | pixel = L·e^{−σ_t d} within 3σ |
| uniform emissive sphere | absorbing σ_a, emission ε, chord d: L = (ε/σ_a)(1 − e^{−σ_a d}); also with ε where density is 0 (rate floor) | within 3σ |
| **volume furnace** | renderer-level E4: heterogeneous, chromatic, emissive medium with ε = σ_a·L in a uniform dome L = 1, maxDepth and limits unlimited, no clamp; also albedo-1 non-emissive (σ_a = 0) → 1 | every pixel mean 1 ± 3σ; image mean within 0.2 % |
| single scattering | E5 sphere with volume limit 0 (light-only continuation) and a distant light: 0.015118, 0.026927, 0.033980 | within 0.2 % at 3σ |
| phase sampling | HG and dual-lobe χ² and pdf consistency (as BSDF tests) | χ² p > 0.01 |
| emissive-volume light | E8 irradiance: NEE+MIS vs unidirectional with BsdfOnly | agree within 3σ |
| determinism | 1 vs 16 threads bit-identical; adaptive threshold 0 = uniform | exact |
| ray limits, LPE | volume limit 0 / 1 / fractional; `C V L`, `C V+ L`, emission events; parts sum to the beauty | as test_ray_limits / test_lpe_render |
| no-volume scenes | the M7 benchmark scenes | ±2 % time, references unchanged |

Karma comparisons (husk, own Houdini instance, companion report for the scene setup):
- dome-lit smoke;
- distant-lit smoke (single scattering);
- fire only (emission plus blackbody);
- fire plus smoke lighting a ground plane;
- smoke behind glass;
- overlapping volumes.

Each at Karma's fine step rate vs Nova converged, with FLIP/RMSE thresholds set after the first measurement and the residual explained (Karma's ray-march bias, interpolation).

Performance gates (design; the numbers come from E1, E4 and E9):
- Per lookup ≤ 30 ns trilinear and per DDA cell ≤ 15 ns single-threaded (measured 28.5 / ≈ 14).
- Volume-free scenes unchanged (±2 %).
- A pyro benchmark scene in `docs/benchmarks.md` (dome + distant light, Karma default limits; plus a fire-lighting variant), with Nova's time to a fixed RMSE against Karma CPU's at its default and fine step rates, recorded at M8 and kept within ±5 % afterwards.

### 6.8 Risks (design)

| Risk | Mitigation |
|---|---|
| Large sparse domains make the dense majorant grid big | k doubles until ≤ 128³ cells; E1 shows k = 16-32 costs ≤ 25 % for free flights at σ = 8 (k = 32: 2.2x at σ = 32) |
| Majorant violated (non-monotone remaps, velocity, stochastic lookups over tiles) | max over [min, max] density for remaps, dilation by velocity, debug assert counter (E2 style) in tests |
| Heavy tails in dense chromatic media (B3 max sample ~100) | indirect clamp as for surfaces; hero MIS (A) as a later option |
| Emission where density is 0 | rate floor in emissive cells (6.2) |
| Karma's ray-marching bias differs from the exact solution | compare at fine step rates; document |
| GPU divergence | wavefront split, step-limited kernel (6.6) |

### 6.9 Sub-steps (design, in the style of docs/design/M7.md)

| Step | Content | Tests and gates |
|---|---|---|
| M8.0 Volume data | `core/volume/` records, NanoVDB fields from the companion report's path, majorant build (parallel), ray–volume intervals, Scene API, IPR rules | majorant conservativeness, build time, volume-free scenes ±2 % |
| M8.1 Absorption and emission | `traceMedia` with ratio-tracked transmittance and collision emission for all rays, glass Beer moved, shadow-ray transmittance, blackbody LUT, alpha | slab, emissive sphere, unbiasedness, determinism |
| M8.2 Scattering | B3 free flights, volume hits, `shadeVolumes` (HG, NEE, MIS, continuation), Volume limit class, RR, clamp, LPE V | furnaces, single scattering, χ², limits and LPE tests |
| M8.3 Pyro material | Karma-matching material and field conventions, dual-lobe HG if needed, overlap, transform and velocity motion blur | Karma comparisons (smoke, fire, behind glass, overlap) |
| M8.4 Emissive volumes as lights | power CDF, `LightType::Volume`, MIS | E8 test, fire-lit ground vs Karma |
| M8.5 Performance and closing | stochastic density lookups, residual ratio tracking for shadows if the profile asks for it, adaptive k; benchmarks; docs | perf gates, full ctest |

---

## 7. Open questions for the companion report or the user

1. Karma's volume conventions:
   - σ from density (scale, scatter/absorption colours, albedo vs coefficients);
   - emission per length vs σ_a·Le;
   - the blackbody intensity law and normalization;
   - temperature remapping;
   - whether flame emits where density is 0;
   - trilinear vs cubic lookups.
2. Which light multiplier (UsdLux diffuse? a Karma volume multiplier?) applies to volume scattering. Whether light-only rays collect volume emission.
3. Karma's LPE events and AOVs for volume scattering and volume emission; first-hit AOVs (P, N, depth) for volume pixels; denoiser guides (albedo, normal) for volumes.
4. Whether Karma light-samples emissive volumes (no public information found). This decides whether M8.4 is parity or an addition.
5. User decision: whether mesh-bounded uniform volumes (fog inside an arbitrary mesh) are needed in M8. They would bring a small medium stack (Cycles-style enter/exit on front/back faces) into the path state.

---

## 8. References

Verified this research (literature sub-agent, primary sources) unless marked "from memory".

- Amanatides, Woo 1987. A fast voxel traversal algorithm for ray tracing. Eurographics (from memory).
- Cramer 1978. Application of the fictitious scattering radiation transport model for deep-penetration Monte Carlo calculations. Nucl. Sci. Eng. 65 (from memory).
- Fong, Wrenninge, Kulla, Habel 2017. Production volume rendering. SIGGRAPH Courses.
- Georgiev, Misso, Hachisuka, Nowrouzezahrai, Křivánek, Jarosz 2019. Integral formulations of volumetric transmittance. ACM TOG 38(6).
- Henyey, Greenstein 1941. Diffuse radiation in the galaxy. ApJ 93:70-83.
- Kettunen, d'Eon, Pantaleoni, Novák 2021. An unbiased ray-marching transmittance estimator. ACM TOG 40(4), Art. 137.
- Kim, Ko 2007. Eulerian motion blur. Eurographics Workshop on Natural Phenomena (from memory).
- Kulla, Fajardo 2012. Importance sampling techniques for path tracing in participating media. CGF 31(4) (EGSR).
- Kutz, Habel, Li, Novák 2017. Spectral and decomposition tracking for rendering heterogeneous volumes. ACM TOG 36(4).
- Lux, Koblinger 1991. Monte Carlo Particle Transport Methods: Neutron and Photon Calculations. CRC (from memory; survival biasing / implicit capture).
- Miller, Georgiev, Jarosz 2019. A null-scattering path integral formulation of light transport. ACM TOG 38(4).
- Misso, Bitterli, Georgiev, Jarosz 2022. Unbiased and consistent rendering using biased estimators. ACM TOG 41(4).
- Misso, Li, Burley, Teece, Jarosz 2023. Progressive null-tracking for volumetric rendering. SIGGRAPH 2023 Conference Papers.
- Museth 2021. NanoVDB: A GPU-friendly and portable VDB data structure for real-time rendering and simulation. SIGGRAPH Talks.
- Novák, Selle, Jarosz 2014. Residual ratio tracking for estimating attenuation in participating media. ACM TOG 33(6).
- Novák, Georgiev, Hanika, Jarosz 2018. Monte Carlo methods for volumetric light transport simulation. CGF 37(2) (Eurographics STAR).
- Simon, Hanika, Zirr, Dachsbacher 2017. Line integration for rendering heterogeneous emissive volumes. CGF 36(4).
- Szirmay-Kalos, Tóth, Magdics 2011. Free path sampling in high resolution inhomogeneous participating media. CGF 30(1).
- Villemin, Hery 2013. Practical illumination from flames. JCGT 2(2).
- Wilkie et al. 2014. Hero wavelength spectral sampling. CGF 33(4) (EGSR).
- Woodcock, Murphy, Hemmings, Longworth 1965. Techniques used in the GEM code for Monte Carlo neutronics calculations in reactors and other systems of complex geometry. ANL-7050.
- Wyman, Sloan, Shirley 2013. Simple analytic approximations to the CIE XYZ color matching functions. JCGT 2(2).
- Xu, Herholz, Manzi, Papas, Gross 2024. Volume scattering probability guiding. SIGGRAPH Asia 2024.
- Yue, Iwasaki, Chen, Dobashi, Nishita 2010. Unbiased, adaptive stochastic sampling for rendering inhomogeneous participating media. ACM TOG 29(6).

Code and documentation:
- pbrt-v4 (github.com/mmp/pbrt-v4): `src/pbrt/media.h`, `media.cpp` (`GridMedium`, `NanoVDBMedium`, `DDAMajorantIterator`, `SampleT_maj`), `cpu/integrators.cpp` (`VolPathIntegrator::Li`, `SampleLd`), `util/spectrum.h` (`BlackbodySpectrum`).
- Blender Cycles (github.com/blender/blender): `intern/cycles/kernel/integrator/shade_volume.h`, `volume_stack.h`, `intersect_volume_stack.h`, `bvh/octree.{h,cpp}`, `scene/volume.cpp`, `kernel/svm/closure.h` (`svm_node_principled_volume`).
- Blender 5.0 Cycles release notes: developer.blender.org/docs/release_notes/5.0/cycles/
- Arnold `standard_volume` and `AtVolumeData` documentation (help.autodesk.com).
- RenderMan 25 PxrVolume and `RixVolumeServices` documentation (rmanwiki-25.pixar.com, renderman.pixar.com).
- Houdini: `rendergeometrysettings` LOP and Karma XPU documentation (sidefx.com/docs/houdini).
- OpenVDB `CHANGES` and `nanovdb/nanovdb/NanoVDB.h` at tags v11.0.0, v12.0.0, v12.1.0, v13.0.0.

Prototypes (this folder):
- `prototypes/va_volume_lab.cpp`: E1-E9.
- `prototypes/va_build.bat`.
- `prototypes/va_blackbody.py`: blackbody colour, intensity conventions, LUT error.
