# Known limitations

The current limitations of Nova 0.1.3 in one place. Each entry names what happens instead and where the behaviour
comes from (the warning in the code, or the design note that deferred it). The per-milestone "Not yet" lines in
[`reference.md`](reference.md) and the "Deferred" tables in [`design/`](design/) describe the state at the end of
their milestone; where a later milestone delivered an item (holdout LPEs, light filters, mesh-bounded media,
`ray:hituv`, deep and checkpoint products), it is not listed here.

The list was compiled from the warnings the code prints (`HD_NOVA_WARN`, `warnOnce` and the core's `[Nova]`
messages) and from the design notes, each checked against the code.

## Platform and build

| Limitation | What happens instead |
|---|---|
| The Hydra delegate, the Houdini package, `nova_cli`, `nova_imgcmp` and the installer build on **Windows x64 with Houdini 22.0** only (`cmake/NovaHoudini.cmake` names Houdini's Windows import libraries). | The renderer core, its unit tests and `nova_bsdf_precompute` build without Houdini on Linux (`cmake --preset core`, see the README). |
| Nova XPU needs an **NVIDIA GPU of the RTX 20 / GTX 16 series or newer** (compute capability 7.5, Turing) with **driver 590 or newer**: OptiX 9.1 needs that driver, and that driver supports no older GPU. The CUDA driver and OptiX are loaded through Windows' loader (`core/gpu/cuda_api.cpp`). | AMD and Intel GPUs, older NVIDIA GPUs, older drivers and machines without a GPU render on the CPU, with one message that names the reason. |
| Nova XPU is **tested on an RTX 5080 only**, so every other series is **experimental**. The kernels are built as code for `sm_75` (RTX 20, GTX 16), `sm_80` (RTX 30 and RTX 40 run it) and `sm_120` (RTX 50) (`core/CMakeLists.txt`, `NOVA_GPU_ARCHS`); the code of the older two comes from another compiler of `nvcc` than the RTX 50 series' and has run, built for the RTX 5080, with the same images bit for bit (`docs/design/M10.md`, section 8), but on no card of those series. | On a card before the RTX 50 series Nova XPU renders on the CPU and says how to turn the GPU on: the installer's option in its Graphics row (shown for such a card), or `NOVA_XPU_EXPERIMENTAL_GPUS=1`. With it on, Nova XPU renders on the GPU and marks its message as experimental. |
| For GPU architectures without code of their own (data-centre cards: `sm_90`, `sm_100`) the kernels exist as PTX only. They are experimental as well. | With the experimental support on, the driver compiles the kernels at the first render, which takes several minutes once; a message says so. |
| A CPU with **AVX2** is required (the SIMD shading VM). | No fallback. |
| Debug configurations do not exist in the full build, because Houdini ships release libraries only. | Release and RelWithDebInfo; the core-only build has no such limit. |
| The husk, reference and Houdini tests need a Houdini installation, some a license or a GUI; regenerating the Karma references needs a Karma license. | Continuous integration (`.github/workflows/core.yml`) covers the core's unit tests only. |

## Nova XPU (GPU)

| Limitation | What happens instead | Source |
|---|---|---|
| GPU and CPU do not render together; one GPU is used. | The GPU renders alone (the driver's first, its fastest; `NOVA_XPU_DEVICE=<index>` names another); the CPU is the fallback. | `design/M10.md` D15, 2.2, section 8 |
| Geometry, acceleration structures, volumes and the film must fit the GPU's memory, next to the kernels' stacks, which the driver sizes by the GPU's multiprocessors (2.0 GB on an RTX 5080 with its 84, measured). Only the textures (the budget) and the waves (fewer paths at once, with a message) adapt to the memory that is free. | A scene that does not fit ends the GPU render with the device's message; the render continues on the CPU. | `core/gpu/gpu_renderer.cpp` (`planWave`), `design/M10.md` section 8 |
| Textures are not loaded on demand. | Past the GPU texture budget the finest MIP levels of the largest textures are left out, with a message. | `core/gpu/gpu_renderer.cpp` |
| VEX materials do not run on the GPU. | Grey stand-ins with one warning, as in Karma XPU. | `core/gpu/gpu_renderer.cpp` |
| Ray queries from shading (`kma_ao`, distance, curvature, rounded edges) do not trace on the GPU. | Their default values. | `design/M10.md` |
| Shadow-link categories beyond the 7th are not separated on the GPU. | Every object shadows those lights, with a message. | `core/gpu/gpu_renderer.cpp` |
| Materials and volume shaders that need more rows than the device VM has. | The GPU uses their uniform inputs (volumes: density only), with a message. | `core/gpu/gpu_renderer.cpp` |
| At most 8 volume spans along one ray segment on the GPU (a volume is one span, a medium bounded by a concave mesh may be several). | Further spans along that ray are left out; the CPU has no limit. A long row of instanced fog boxes seen end-on can reach it. | `core/gpu/kernels.cu` (`GpuSpans`) |
| The Texture Baker does not bake on the GPU. | Nova XPU bakes on the CPU. | `design/M11-P8.md` |
| The OptiX denoiser does not work on the renderer's GPU memory. | Its images go through host memory. | `reference.md` (M10) |

## Lights

| Limitation | What happens instead | Source |
|---|---|---|
| At most **64 light-link and 64 shadow-link categories** (distinct link collections) per scene. | Lights of further categories light, or are shadowed by, every object; one message. | `Scene::computeLinks`, `core/scene/scene.cpp` |
| Dome texture formats other than `latlong` (`mirroredBall`, `angular`, `cubeMapVerticalCross`). | Read as latlong, with a warning. | `hydra/light.cpp` |
| Ellipsoid lights (a non-uniformly scaled sphere light). | A sphere of the mean radius, with a warning. | `hydra/light.cpp` |
| IES profiles (`shaping:ies:file`) and `treatAsLine`. | Ignored: the light keeps its plain distribution; a line light renders as a cylinder. | not read in `hydra/light.cpp` |
| Moving or deforming emissive meshes are not light sources. | They stay plain emitters (found by BSDF sampling only), with a message. | `core/scene/scene.cpp` |
| Emission of VEX materials is never sampled as a light. | Plain emitter. | `design/M9.md` |
| Heavily instanced emissive meshes have no two-level light tree. | Flattened to one light per triangle and instance; a message above the budget. | `design/M9.md` 2.2 |
| Light filters other than Karma's blocker; filter spaces other than world. | Ignored with a warning; world space. | `hydra/lightFilter.cpp` |
| `misbias`, `activeradius`, `pointradius` and `contribs` light properties. | Ignored. | `design/M9.md` 2.2 |
| Occluders very close to a light in a scene of large extent: shadow rays stop short of a light by 32 float ulps of the scene's largest coordinate (0.04 units in a scene that reaches 10 000, 0.4 at 100 000), so that a light lying on a surface is not shadowed by it. | An object closer to a light than that does not shadow it. | `shadowRayLength`, `core/lights/lights.h` |

## Materials

| Limitation | What happens instead | Source |
|---|---|---|
| Karma Hair `coat`; Karma Fur's medulla and second R lobe. | Rendered without them, with a warning. | `hydra/material/translator.cpp` |
| `sss_traceset` (walks across objects that share a name). | Walks stay inside their object. | `design/M9.md` D10 |
| `surfacematerial`'s `backsurfaceshader`. | The front shader on both sides, with a warning. | `hydra/material/translator.cpp` |
| Retroreflective lobes. | Reflect normally, with a warning. | `hydra/material/translator.cpp` |
| Dispersion. | Ignored, with a warning (it needs spectral rendering). | `hydra/material/translator.cpp` |
| Six of MaterialX 1.39's 746 node definitions, and nodes outside it. | The node's default outputs, with a warning. | `hydra/material/translator.cpp` |
| String or filename `geompropvalueuniform` (a texture file per prim, face or instance). | The image's default, with a warning. | `design/M4.md` |
| Trace sets and object-space distances of the ray-query nodes; `scope:self` of rounded edges. | All objects, world space, with a warning. | `hydra/material/translator.cpp` |
| Principled Shader: packed colour, the `vdisp` input, noise displacement, dispersion, rounded edges, `opdef:` textures. | Ignored with a warning. | `hydra/material/principled.cpp` |
| VEX materials: AOV exports other than the mapped `export_*` names, fake caustics through them, ray-tracing VEX (`trace`, `gather`, `illuminance`), light, lens, fog and volume VEX shaders. | Opaque to fake-caustic rays; a shader that traces fails to compile and the preview material renders; the other shader kinds are ignored with a warning. | `design/M5.md` |
| UDIM sets inside packages (`.usdz`). | The texture fails to load, with an error. | `hydra/textureConvert.cpp` |

## Geometry

| Limitation | What happens instead | Source |
|---|---|---|
| `nurbsCurves` prims. | Not rendered (Houdini's SOP Import never authors them). | `design/M9.md` |
| Authored normals on subdivided meshes; subdivision of bilinear meshes and UsdGeom implicit prims. | Normals come from the limit surface; those prims stay polygonal. | `reference.md` |
| Long round linear curves (bug audit R1-11). | Reprojected onto the tube, leaving with an error-bounded offset. | `bug-audit-2026-11.md` |
| Two exactly coplanar overlapping surfaces. | Which one a ray reports follows the accelerator's traversal and can change between runs. | `bug-audit-2026-11.md` section 7 |
| A single polygon that reaches far beyond what is seen of it: rays leave a polygon by a few float ulps of its reach from the hit (Nova XPU: 10^-6 of it; the CPU half of that for polygons that are not axis-aligned, nothing for axis-aligned ones), the error of the ray-triangle tests. | Off a polygon that reaches 100 000 units the rays start up to 0.1 units above it: objects that close to it lose part of their shadow on it, and a light 5 units away lights it too brightly (a sphere on such a ground: the image 0.8 % brighter on XPU when it is axis-aligned, 1.6 % when rotated; 0.3 % on the CPU when rotated). At 10 000 units the difference is 0.1 % (XPU), below that none; from 10^6 units Nova XPU's image is 9 % and more off, the CPU's 3 % if the polygon is rotated. Split such polygons. | `leaveSurface`, `core/scene/ray.h`; `bug-audit-2026-11.md` section 12 |
| A point instancer that lists itself or one of its ancestors among its prototypes; `protoIndices` beyond the prototypes. | The process ends inside UsdImaging (a stack overflow in its prototype scene index, memory written out of bounds in its instancer data source) before the delegate sees a prim, as with any Hydra renderer. | `bug-audit-2026-11.md` section 12 |
| `motion:blurScale`, `motion:nonlinearSampleCount` and `motion:velocityScale` (UsdGeomMotionAPI). | Ignored. | `design/M8.md` |

## Volumes

| Limitation | What happens instead | Source |
|---|---|---|
| `volumefilter` other than a box of width 1; tricubic lookups. | Box/trilinear, with a warning. | `hydra/volume.cpp` |
| Karma CPU's lattice-deformation velocity blur. | Velocity blur as a lookup offset. | `design/M8.md` |
| Field prims other than VDB and Houdini volumes; VDB grids of value types other than the scalar and vector ones the bridge converts. | The field is skipped, with a warning. | `hydra/field.cpp`, `houdini/volume_bridge/bridge.cpp` |
| Cryptomatte ids for volumes. | Volumes have no cryptomatte coverage. | `design/M8.md` 2.2 |
| Karma's step-rate settings (`volumesteprate`, `volumeshadowsteprate`, `volumequality`, `volumesamplingchannel`). | Ignored: Nova tracks without bias at any setting; fire is slower than Karma at its default step rate. | `design/M8.md` D16 |

## Output and baking

| Limitation | What happens instead | Source |
|---|---|---|
| Karma render settings without a Nova equivalent. | One warning per stage that names them: "the image differs from Karma's". | `hydra/renderDelegate.cpp` |
| Utility path expressions other than `CO` (`aov:karma:utilitypathexpression`). | The values of the camera hit, with a warning. | `hydra/aov.cpp` |
| AOV sources Nova does not know; render product types other than raster, `karma:deep` and `karma:checkpoint`. | The plane keeps its clear value; the product is skipped; each with a warning. | `hydra/aov.cpp`, `hydra/renderDelegate.cpp` |
| Temporal denoising. | Each frame is denoised by itself. | `design/M7.md` D9 |
| Texture Baker: textures for trace bias, trace max distance and trace skew; bake trace sets; some image filters. | The global values; every visible object; the filter is skipped; each with a warning. | `hydra/bake.cpp` |

## Removed since this list was started

- **The core built only with MSVC against a Houdini installation.** It also builds with GCC on Linux against
  system libraries, without Houdini (`NOVA_CORE_ONLY`, `cmake/NovaStandalone.cmake`); all of its unit tests pass
  there.
- **No continuous integration.** `.github/workflows/core.yml` builds the core and runs its unit tests on every pull
  request, also under AddressSanitizer and UBSan.
- **Shading on values beyond int's range depended on the device.** A noise position past 2^31, an infinite octave
  count or a NaN hue was converted to int with undefined behaviour: INT_MIN on x86, a saturated value or 0 on the
  GPU. The conversions are defined now (`toInt`, `core/math/common.h`) and give what the CPU gave.
- **The Texture Baker lost triangles with a UV corner far outside the tile** (from u = 1e6 with a full lookup
  grid): they were in no cell of the grid.
- **A checkpoint with a short adaptive-sampling array loaded** and was then read past its end.

The last three are described in [`bug-audit-2026-11.md`](bug-audit-2026-11.md), section 9.

- **Instanced media bounded by surfaces rendered nothing.** A Fog Box under a point instancer or as a native
  instance renders at every instance, as in Karma CPU (`hydra/medium.cpp`, `design/M11-P5.md` section 7.1).
- **Indexed curves were rendered with their points in order.** Curve vertices and their vertex-rate primvars are
  read through Hydra's curve indices (`hydra/curves.cpp`).
- **A volume whose size changes over the shutter was not a light source** (bug audit R1-36). It is light-sampled
  with the cell's volume at the path's time (`core/volume/build.cpp`).

These three are described in [`bug-audit-2026-11.md`](bug-audit-2026-11.md), section 10.
