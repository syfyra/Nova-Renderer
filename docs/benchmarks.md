# Benchmarks

Rays per second per milestone (docs/PLAN.md, "Performance"). "Rays" counts
every ray Embree traces: camera rays, continuation rays and shadow rays.

Machine: Ryzen 7 9800X3D (8 cores, 16 threads, all used), Windows 11, MSVC
14.44 Release build, Embree 4.4.1 (static, AVX2). Idle machine, two runs per
scene (the table gives the mean; runs agree within 2 %).

Command, from the repository root with `$HFS/bin` on `PATH`:

```
build/vs2022/tools/nova_cli/Release/nova_cli.exe tests/reference/scenes/<scene>.usda -o out.exr --res 960x540 --spp 64
```

The scenes are the Karma reference scenes (`tests/reference/scenes`) with their own
path lengths and lights.

## M2 (2026-10-12)

| Scene | Content | Time | Mrays/s |
|---|---|---|---|
| `cornell_rect` | Cornell box, rect light, maxDepth 8 | 4.90 s | 24.3 |
| `instancer` | point instancer, dome + distant light, maxDepth 3 | 4.52 s | 25.1 |
| `subdiv` | Catmull-Clark meshes, dome + rect light, maxDepth 3 | 3.31 s | 29.3 |
| `motion_blur` | transform and deformation blur, dome + distant light, maxDepth 2 | 1.67 s | 52.2 |
| `dome_sky` | textured dome (importance sampled), maxDepth 4 | 2.38 s | 37.8 |

Shading is Lambert only in M2; M3 adds the material VM and closures. The M3
gate (docs/design/M3.md, section 8): an untextured Standard Surface version of
these scenes at most 1.35x the M2 wall time.

## M3.4 (2026-10-13): closures, default material

The default material (Lambert with the display color) now runs through the
closure path, and shading uses three passes per bounce (lights and escapes,
sort by material, closures). Same command and scenes.

| Scene | Time | Mrays/s | vs M2 |
|---|---|---|---|
| `cornell_rect` | 5.18 s | 23.1 | -5 % |
| `instancer` | 5.05 s | 22.5 | -10 % |
| `subdiv` | 3.35 s | 28.5 | -3 % |
| `motion_blur` | 2.01 s | 42.7 | -18 % |
| `dome_sky` | 2.87 s | 31.6 | -16 % |

Gate (docs/design/M3.md, M3.4): `cornell_rect` within 10 % of M2. The
recorded M2 numbers come from another machine state, so the comparison was
also made side by side with the M2 build of hdNova under husk. That build
renders in 4.73 s and M3.4 in 5.12 s (+8 % time; husk startup of 1.15 s
excluded).

What it took. With stage timers in the wavefront loop, shading was 74 % of
the thread time on `cornell_rect`, and the rect light's sampling was 40 % of
shading. Closures over an M2-style inline Lambert cost about 11 % at first.
The fixes:

- Closures with a single Lambert lobe build without copying the inputs and
  skip the mixture evaluation.
- The generic lobe and surface-model dispatch stays out of line
  (`NOVA_OUTLINE`).
- The Embree argument-filter scene flags are set only while a mesh is
  filtered.
- Rect lights precompute their edge frame (`RectFrame`). The spherical
  rectangle uses the closed-form edge normals.

The low-depth scenes without rect lights (`motion_blur`, `dome_sky`) still
pay the per-hit closure cost: shading is cheap there, so it weighs more. That
is work for the M3.12 performance step (section 8 gates).

## M3.9 (2026-10-13): Standard Surface materials, textures, ray cones
Since M3.6 the reference scenes render their own material: `displayColorLambert` is a
Standard Surface with `specular 0`, not the default material. M3.9 adds a single-lobe
build for such diffuse-only surfaces (before it: `cornell_rect` 19.6, `dome_sky` 27.2,
`motion_blur` 34.5 Mrays/s). Texture footprints cost nothing on untextured materials.
Same command and scenes.

| Scene | Time | Mrays/s | vs M3.4 (default material) |
|---|---|---|---|
| `cornell_rect` | 5.48 s | 21.8 | -6 % |
| `dome_sky` | 3.12 s | 29.8 | -6 % |
| `motion_blur` | 2.24 s | 39.0 | -9 % |

Side by side under husk with the M2 build of hdNova (`dist/nova`), the run before the
single-lobe build needed 1.29x M2's render time on `cornell_rect` (gate for Standard
Surface: 1.35x).

## M3 (2026-10-13): final
Same command and scenes; the reference scenes render their Standard Surface
`displayColorLambert` (single-lobe build), opaque scenes pay nothing for texture
footprints or opacity support. Two runs each, repeated where the machine was busy.

| Scene | Time | Mrays/s | vs M2 |
|---|---|---|---|
| `cornell_rect` | 5.57 s | 21.5 | -12 % |
| `instancer` | 5.60 s | 20.0 | -20 % |
| `subdiv` | 3.68 s | 26.6 | -9 % |
| `motion_blur` | 2.30 s | 38.5 | -26 % |
| `dome_sky` | 3.14 s | 29.5 | -22 % |

M3 gates (docs/design/M3.md section 8), measured with `nova_hydra_tests "[.bench]"`:

| Gate | Target | Measured |
|---|---|---|
| Standard Surface vs M2 Lambert wall time | ≤ 1.35x | 1.14x (`cornell_rect`, 5.57 s vs 4.90 s) |
| VM per instruction component and lane, N = 32 | 0.15-0.25 ns | 0.152-0.155 ns |
| Closure build, default Standard Surface | ≤ 150 ns/hit | 149-152 ns/hit (three runs) |
| Translation + compile, Standard Surface + 3 textures | ≤ 2 ms | 0.09 ms |
| IPR value edit, engine side | ≤ 10 ms | 0.12 us |
| Opaque scenes with opacity support | ±3 % | within 1 % of M3.9 |

The recorded M2 times come from another machine state; side by side under husk with
the M2 build of hdNova (`dist/nova`, husk startup measured at 16x9 and subtracted):
`motion_blur` 2.01 s vs 1.59-2.06 s (at most 1.27x), `dome_sky` 2.92 s vs 2.22 s
(1.32x). All five scenes stay inside the 1.35x gate.

Where the time goes (stage timers, `motion_blur` / `dome_sky`): intersection 24 / 22 %,
closures, light samples and continuation rays 41 / 41 %, camera rays 17 / 12 %,
hit resolve 7 / 5 %, the material VM 4 / 3 %, shadow rays 5 / 7 %. Camera ray generation
(double-precision unprojection, three per ray) is unchanged since M2; a later
performance step can precompute it per tile.

## M4 (2026-10-13): final

Same machine and command as above (`nova_cli`, 960x540, 64 spp, the reported render
time, minimum of five runs). M4.1 changed the scenes' limits to Karma's: the diffuse limit
is the M3 `maxDepth` minus one, so the paths are as long as in M3, and a light-only ray
leaves the last vertex. The times are therefore not comparable with the M3 table; the
rays per second are (gate, docs/design/M4.md 4: within ±5 % of M3 at equal limits).

| Scene | M4.4 | M4.5 | M4 final | Mrays/s | vs M3 Mrays/s |
|---|---|---|---|---|---|
| `cornell_rect` | 4.10 s (24.2 Mrays/s) | 4.17 s | 4.22 s | 23.5 | +9 % |
| `instancer` | | | 5.16 s | 21.1 | +6 % |
| `subdiv` | | | 3.60 s | 26.4 | -1 % |
| `motion_blur` | 2.25 s | 2.27 s | 2.26 s | 37.6 | -2 % |
| `dome_sky` | 2.94 s | 3.00 s | 3.00 s | 29.4 | -0 % |
| `mtl_spheres` | 2.61 s | 2.62 s | 2.58 s | 21.0 | |
| `mtl_graphs` (M4.9) | | | 2.79 s | 19.3 | |
| `disp_textured` (M4.9) | | | 2.79 s | 19.9 | |
| `limits_caustics` (M4.9) | | | 5.18 s | 26.3 | |

No scene is slower per ray than in M3. The two above +5 % gain from the cheap light-only
rays at the limit (no shading at their hit), which `cornell_rect` (diffuse limit 7) and
`instancer` trace most often.

M4 gates (docs/design/M4.md 4), `nova_hydra_tests "[.bench]"` on the final build:

| Gate | Target | Measured |
|---|---|---|
| Native closure build (16 lobes, 4 frames, larger input block) | ±3 % of M3 | 147-152 ns/hit (M3: 149-152) |
| Generic graph build, default Standard Surface expanded (16 nodes) | ≤ 3x native | 374-382 ns/hit (2.5-2.6x) |
| Opaque scenes | ±3 % | within 2 % of M4.4 |
| Rays/s of the M3 scenes at equal limits | ±5 % | -2..+9 % (above) |

Displacement (risk table, docs/design/M4.md 5), `disp_textured` at 960x540 and dicing
quality 1: the plane dices into 339 201 triangles, the Catmull-Clark cube into 340 384, the
vector-displaced sphere into 169 564. Dicing, displacing and the larger BVH add 82 ms to the
wall time of a 1-spp render (0.30 s vs 0.22 s with `nova:displacement` off, best of five). An IPR
edit of a displacement re-dices only the bound meshes, so that is the upper bound of its
latency here.

## M5 (2026-10-13): final

Same machine and command (`nova_cli`, 960x540, 64 spp, the reported render time, minimum
of five runs).

| Scene | M4 final | M5 final | Mrays/s |
|---|---|---|---|
| `cornell_rect` | 4.22 s | 4.11 s | 24.3 |
| `instancer` | 5.16 s | 5.07 s | 21.8 |
| `subdiv` | 3.60 s | 3.49 s | 27.2 |
| `motion_blur` | 2.26 s | 2.23 s | 38.3 |
| `dome_sky` | 3.00 s | 2.97 s | 30.1 |
| `mtl_spheres` | 2.58 s | 2.57 s | 21.1 |
| `mtl_graphs` | 2.79 s | 2.73 s | 19.7 |
| `disp_textured` | 2.79 s | 2.73 s | 20.3 |
| `limits_caustics` | 5.18 s | 5.04 s | 27.1 |
| `vex_principled` (M5.1, native) | | 2.93 s | 19.4 |
| `vex_builder` (M5.4, CVEX bridge) | | 3.68 s | 16.0 |

The opaque scenes are unchanged (gate docs/design/M5.md M5.3: ±3 %; all within -3..0 %).
`vex_builder` runs all seven of its spheres through the CVEX bridge; the same layout with
native Principled materials (`vex_principled`) takes 2.93 s.

Bridge cost per shading point (`nova_cvex_tests "CVEX bridge cost per shading point"`, one
run of 256 lanes on one thread, best of three times 400 runs; it includes eval_bsdf at the
light direction, albedo, sample_bsdf and eval_bsdf at the sample):

| Shader | ns per shading point |
|---|---|
| VEX `diffuse` | 109 |
| VEX metal (`get_bsdf("ggx")`) | 199 |
| VEX plastic (diffuse + ggx) | 308 |
| Stock VEX Material Builder: Principled Shader Core with a noise (Houdini-generated, 26 KB of VFL) | 844 |

For comparison, Nova's native Standard Surface closure build is about 150 ns per hit (M4 gates
above), plus the lobes' evaluation and sampling. A shading point of the stock builder costs about
three to four native ones; `vex_builder`'s own shaders are the simpler mixtures of the first rows.

## M6 (2026-10-13): cost of light path expressions

`tests/reference/tools/bench_lpe.py` renders `cornell_rect`, `mtl_spheres` and `mtl_glass` with
`nova_cli --settings /Render/ref` (the scenes' own settings prim: their ray limits and the
product, so the times are not comparable with the tables above), 960x540, 64 spp, the reported
render time, minimum of five runs with the four sets interleaved. The sets:

- 0: the beauty only;
- 1: the beauty and `C<RD>.*L`;
- 20: Karma's standard set (the beauty and 19 vars: 18 distinct expressions and the beauty's
  `unoccluded;`/`shadow;` variants);
- 74: Karma's full table (26 LPE vars) and its 16 lighting rules split for three light tags.

| Scene | 0 LPEs | 1 | 20 (Karma's set) | 74 |
|---|---|---|---|---|
| `cornell_rect` | 6.00 s | 6.11 s (+1.9 %) | 6.35 s (+5.9 %) | 6.49 s (+8.2 %) |
| `mtl_spheres` | 2.43 s | 2.56 s (+5.7 %) | 2.75 s (+13.2 %) | 2.83 s (+16.8 %) |
| `mtl_glass` | 2.99 s | 3.05 s (+2.1 %) | 3.27 s (+9.5 %) | 3.34 s (+11.7 %) |

Before the M6.6 changes (lazy copies, no per-lobe evaluation for closures that are one event)
`cornell_rect` cost +5.0 / +9.7 / +15.2 %; the other two scenes are unchanged within the noise
of these runs (about 2 %).

With the beauty only, no LPE code runs; husk renders `cornell_rect` as fast as the M5 package
(M6.2). The design's gate of at most 5 % with Karma's standard set is not met (docs/design/M6.md,
notes on M6.6). The cost is bookkeeping per event:

- the parts of each light sample;
- the deposits into the film per output;
- the weight-1 sums of `unoccluded;` and `shadow;`;
- the `A` events at every hit;
- for layered materials (`mtl_spheres`), the per-lobe evaluation and class choice.

Measured with `nova_cli --profile` (a sampling profiler; RelWithDebInfo build). Found on the way,
for M7, in the beauty-only profile of `cornell_rect`:

- `fmaxf`/`fminf` run as CRT calls (about 4 % of the samples);
- `asinf` in spherical-rectangle sampling takes about 5 %;
- small `NOVA_INLINE` helpers such as `normalize` are not always inlined.

## M7 (2026-10-14): per-ray speed, IPR, adaptive sampling, denoising

**Per-ray speed (M7.0).** `nova_cli`, 960x540, 64 spp, minimum of five runs, alternating with the M6 binary on the same
machine (`std::fmin`/`std::fmax` inline instead of CRT calls, an inline arcsine for spherical rectangles, forced
inlining of the vector helpers):

| Scene | M6 | M7.0 | |
|---|---|---|---|
| `cornell_rect` | 4.09 s | 3.87 s | -5.2 % |
| `instancer` | 5.04 s | 4.88 s | -3.1 % |
| `subdiv` | 3.48 s | 3.33 s | -4.4 % |
| `motion_blur` | 2.25 s | 2.10 s | -6.7 % |
| `dome_sky` | 2.96 s | 2.77 s | -6.1 % |
| `mtl_spheres` | 2.56 s | 2.31 s | -9.9 % |
| `mtl_graphs` | 2.77 s | 2.54 s | -8.1 % |
| `disp_textured` | 2.71 s | 2.56 s | -5.5 % |
| `limits_caustics` | 5.10 s | 4.70 s | -8.0 % |
| `vex_principled` | 2.92 s | 2.68 s | -8.1 % |
| `vex_builder` | 3.78 s | 3.76 s | -0.5 % (CVEX-bound) |

**IPR latency (M7.2).**
- Method: `nova_hydra_tests "[ipr-latency]"`, at 1280x720, on a scene of 980 000 triangles with 20 000 instances. Each value is the time from the USD edit to the first new image, the median of six edits; docs/research/m7/ipr.md describes the method.
- Before M7.2, every edit took 183-256 ms (the first full pass).
- With proxy passes (one pixel per 4x4 block first) and refits:

| Edit | Total | Sync | Restart | First (proxy) pass |
|---|---|---|---|---|
| camera move | 23 ms | 0.7 | 4.4 | 18.3 |
| light intensity | 25 ms | 1.5 | 1.9 | 21.6 |
| light transform | 24 ms | 1.7 | 1.9 | 20.1 |
| material value | 29 ms | 1.9 | 1.9 | 24.7 |
| object transform (cube) | 50 ms | 1.6 | 2.0 | 45.9 |
| object transform (1M triangles) | 40 ms | 6.6 | 2.3 | 31.4 |
| instancer: one position | 55 ms | 2.7 | 9.7 | 42.5 |
| deform points (1M triangles) | 89-94 ms | 20-26 | 27-42 | 26-44 |
| visibility (cube) | 48 ms | 2.2 | 2.1 | 43.5 |
| add / remove a prim | 36 ms | 1.6 | 2.4 | 31.7 |

All edits stay below PLAN.md's 100 ms.
- The refit of the deformed 1M-triangle mesh takes 25 ms in Embree, against 39 ms for the rebuild.
- After every edit that commits Embree (transforms, visibility, geometry), the first pass takes about 20 ms longer than after a camera or shading edit, probably because Embree's own threads are still busy after the commit.

**Adaptive sampling (M7.3).**
- Method: `tests/reference/tools/bench_adaptive.py`. It renders each scene's settings prim at the scene's resolution.
- The error is the relMSE against Nova's own converged image: the mean of two 4096-spp renders, with their noise subtracted.
- The uniform time at the oracle's error comes from a log-log fit of uniform renders at 32-1024 spp.
- The oracle runs with a maximum of 1024, threshold 0.01 and the automatic minimum (32).

| Scene | Uniform time at equal error | Variance oracle | Speed-up | Gate |
|---|---|---|---|---|
| `mtl_glass` | 1.49 s | 0.46 s (relMSE 6.1e-4) | 3.25x | >= 2x |
| `cornell_rect` | 0.55 s | 0.52 s (1.4e-3) | 1.06x | >= 0.85x |
| `dome_sky` | 0.17 s | 0.18 s (4.8e-4) | 0.97x | >= 1x: 10 ms short, within the runs' noise |

The first implementation, with whole 32x32 tiles as work items, reached only 1.4x / 0.77x / 0.87x on these small images:
- the last active pixels sat in a few tiles, so most of the 16 threads idled;
- work items are now chunks of the tiles' active pixels, sized to keep every thread busy.

**Denoising (M7.4, M7.5).**
- Method: `nova_cli` with `nova:denoise` at 16 spp, with relMSE against the same converged images.

| Scene | Noisy (16 spp) | Denoised | |
|---|---|---|---|
| `cornell_rect` | 1.34e-2 | 8.85e-4 | 15x lower (gate >= 10x) |
| `mtl_glass` | 1.39e-2 | 1.79e-3 | 7.8x |
| `dome_sky` | 2.05e-3 | 2.59e-4 | 7.9x |

- Alpha stays exact.
- husk with Karma's `karma:global:imagefilter` (the Karma Render Settings LOP's Denoiser menu) gives the same image as `nova:denoise`.
- The time of a denoise on the RTX 5080 (docs/research/m7/denoising.md 3.9) is 2.5 ms at 960x540, 8 ms at 1080p and 31 ms at 4K. The first call in a process adds the OptiX context, about 0.1-0.3 s.

## M8 (2026-11-03): volumes

**Volume-free scenes.** husk wall time against the installed pre-M8 package (`dist/`), alternating, minimum of 2 runs, through `refcommon.IsolatedHoudini` with either package on `HOUDINI_PATH`:

| Scene | Samples | pre-M8 | M8 | |
|---|---|---|---|---|
| `cornell_rect` | 4096 | 21.09 s | 20.95 s | -0.7 % |
| `mtl_glass` | 4096 | 9.85 s | 9.85 s | +0.1 % |
| `mtl_spheres` | 2048 | 4.04 s | 4.02 s | -0.4 % |
| `dome_sky` | 2048 | 5.48 s | 5.54 s | +1.1 % |
| `instancer` | 2048 | 8.53 s | 8.52 s | -0.1 % |

All within the M8 gate (±2 %): without active volumes the integrator skips `traceMedia` and pass C'.

**Pyro.** The research's sim (`tests/data/volumes/pyro.vdb`, the reference scenes' key light and camera) at 400x600, 256 samples per pixel for both renderers (Karma path traced, uniform), husk wall time including about 1.5 s of startup, minimum of 2 runs:

| Scene | Nova | Karma CPU | |
|---|---|---|---|
| `pyro_bare` (no material, volume limit 0) | 4.54 s | 12.90 s | 2.8x |
| `pyro_bare_vl4` (volume limit 4) | 4.74 s | 19.84 s | 4.2x |
| `pyro_fire` (Karma Pyro Material, fire on) | 46.98 s | 107.59 s (step rate 4) | 2.3x |
| | | 14.83 s (default step rate 0.25) | Karma's default is 6-10 % bright on absorbing emitters (research H 4.8) |

- Fire is the expensive case: every cell whose emission bound is above 0 gets a collision rate of at least one per voxel (research A 4.5), so camera rays and the continuations that collect emission evaluate the Pyro Material (67 instructions, two fields) about once per voxel across the whole hot plume.
- Shadow rays and light-only rays collect no emission of non-light volumes: they track on the extinction bound alone (`VolumeRecord::extinctionMajorant`). That took `pyro_fire` from 59.1 s to 47.0 s.
- At equal accuracy Nova is faster than Karma on every pyro scene; stochastic lookups and residual ratio tracking (design 5) were not needed.

## M9 (2026-11-04): hair, SSS, light tree, emissive-mesh lights

**Unchanged scenes.** husk wall time of the reference scenes against the installed pre-M9 package (`dist/`), alternating, minimum of 2 runs (light tree on, the default):

| Scene | Samples | pre-M9 | M9 | |
|---|---|---|---|---|
| `cornell_rect` | 4096 | 18.74 s | 18.62 s | -0.7 % |
| `mtl_glass` | 4096 | 9.43 s | 9.75 s | +3.4 % |
| `mtl_spheres` | 2048 | 3.74 s | 3.74 s | -0.0 % |
| `dome_sky` | 2048 | 5.27 s | 5.27 s | -0.1 % |
| `instancer` | 2048 | 7.89 s | 7.92 s | +0.4 % |

- The first M9 build was 2.3-4.6 % slower on every scene with identical ray counts (the same paths). The growth of `wavefront.cpp` had stopped MSVC from inlining small hot functions: the Sobol sampler, the pixel filter's sampling and the camera's double-precision unprojection now force inlining (the unprojection multiplies by reciprocals), and closures of delta lobes skip the light choice and the MIS vertex. `nova_cli` on `mtl_glass` at 1280x720: 18.88 s → 17.90 s.
- `mtl_glass` keeps +3.4 % at its reference size of 160x80 pixels, where the threads mostly wait (16 % of the profile's samples are busy): a fixed cost per pass, not per ray. At 1280x640 (64 samples) it is 5.60 s against 5.60 s, `dome_sky` 5.75 s against 5.71 s (+0.7 %).
- The light tree costs 1 % on `mtl_glass` (uniform selection: +2.3 % instead of +3.4 % at 160x80).

**The M9 scene** of the Solaris check (`-Only m9`: 600 hair fibres with Karma Hair, a subsurface sphere, 40 sphere lights, an emissive quad treated as a light), 1280x720, 128 samples per pixel for both renderers, husk wall time: **Nova 21.9 s, Karma CPU 102.0 s** (4.7x).

**Commits with many emissive triangles** (`nova_tests "[.bench]"`, 1 M triangles): first commit 0.85 s, an intensity edit of another light 0.13 s (before the parallel build, the power-only refit and keeping the emitters: 4.0 s and 3.9 s).

## M10 (2026-11-04): Nova XPU

GPU: GeForce RTX 5080 (driver 617.42, CUDA 12.9, OptiX 9.1). `nova_cli` with `--device cpu` and `--device xpu`, the command above (960x540, 64 samples, the reference settings), the faster of two runs; "converged in" includes the GPU's setup (driver context, OptiX pipelines, the scene's upload), not the process start. Every image agrees with the CPU's: every pixel within 1 % except `mtl_textured` (80.5 %, the worst 16 x 16 block 1.7 %: hardware trilinear texture filtering against OIIO's stochastic filtering, M10 G2).

| Scene | CPU | Mrays/s | XPU | Mrays/s | |
|---|---|---|---|---|---|
| `cornell_rect` | 5.42 s | 22.3 | 0.75 s | 176 | 7.2x |
| `mtl_spheres` | 2.19 s | 23.8 | 0.45 s | 147 | 4.9x |
| `mtl_glass` | 2.79 s | 22.0 | 0.59 s | 116 | 4.8x |
| `mtl_textured` | 3.78 s | 21.9 | 0.61 s | 177 | 6.2x |
| `dome_sky` | 3.13 s | 29.2 | 0.67 s | 165 | 4.6x |
| `instancer` | 5.27 s | 21.2 | 0.68 s | 178 | 7.7x |
| `implicit_spheres` | 3.75 s | 24.9 | 0.67 s | 170 | 5.6x |
| `motion_blur` | 2.30 s | 38.1 | 0.53 s | 175 | 4.3x |
| `limits_caustics` | 4.82 s | 28.3 | 0.85 s | 142 | 5.7x |
| `pyro_bare` | 1.35 s | 32.7 | 0.70 s | 67 | 1.9x |
| `furnace_materials` | 2.53 s | 20.7 | 0.93 s | 63 | 2.7x |

**Per sample, without the setup.** Nova XPU at 64 and 1024 samples (the faster of two runs each): the difference gives the time per sample at 960x540 and the setup (0.26-0.42 s per render: the device context, the OptiX pipelines, the scene's upload). The CPU's time per sample is its 64-sample time over 64 (its setup is small: `cornell_rect` takes 84.35 s for 1024 samples, 82.4 ms per sample against 84.7 ms from 64; `mtl_glass` 41.93 s, 40.9 against 43.7 ms).

| Scene | CPU per sample | XPU per sample | | XPU setup |
|---|---|---|---|---|
| `cornell_rect` | 84.7 ms | 7.35 ms | 11.5x | 0.27 s |
| `mtl_spheres` | 34.2 ms | 2.24 ms | 15.3x | 0.26 s |
| `mtl_glass` | 43.7 ms | 4.65 ms | 9.4x | 0.29 s |
| `mtl_textured` | 59.0 ms | 3.75 ms | 15.7x | 0.36 s |
| `dome_sky` | 48.8 ms | 5.26 ms | 9.3x | 0.33 s |
| `instancer` | 82.3 ms | 6.18 ms | 13.3x | 0.35 s |
| `implicit_spheres` | 58.5 ms | 5.25 ms | 11.1x | 0.33 s |
| `motion_blur` | 36.0 ms | 3.97 ms | 9.1x | 0.33 s |
| `limits_caustics` | 75.3 ms | 8.47 ms | 8.9x | 0.30 s |
| `pyro_bare` | 21.0 ms | 5.27 ms | 4.0x | 0.42 s |
| `furnace_materials` | 39.5 ms | 9.62 ms | 4.1x | 0.32 s |

- The research measured the GPU at 31-68x the CPU on its probes (research cw); Nova XPU reaches 9-16x on surfaces. Nsight Systems on `cornell_rect` (960x540, 128 samples): the shade kernel takes 89.5 % of the GPU time (0.68 ms per depth), the OptiX launches 4 %, generation 4 %, shadows 2 %. The shade kernel uses 217 registers and a 16.7 KB stack per thread (closures, surface inputs, the VM's rows, LPE parts): few warps per SM and local-memory traffic. Capping it at 128 registers (`__launch_bounds__(256, 2)`) made it slower (`cornell_rect` 1024 samples 10.2 s against 7.8 s): the spills cost more than the occupancy gains. Splitting it into material evaluation and scattering (research cw 11.1, M10's risk table) is the later work; material sorting made no difference (M10 G8).
- Volumes (`pyro_bare`, 4x) and many layered materials (`furnace_materials`, 4x) gain least: the majorant DDA and the material programs run per thread with divergent loops.
- Material divergence (`nova_tests "XPU material divergence"`, 64 spheres with 64 different per-hit programs against one): 3.85 ms per pass in both.
- IPR (`nova_tests "XPU edit latency"`, 4.2M triangles, 640x360): the first pass 183 ms, a pass after an edit of a light, a material or a transform 7 ms (52 ms before uploads by changed mesh), a pass alone 2 ms.
