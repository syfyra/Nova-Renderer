# Known limitations

The current limitations of Nova 0.2.1 in one place. Each entry names what happens instead and where the behaviour
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
| `filtertype` cubic of a MaterialX image. | Bilinear on the GPU (the CPU is bicubic, as Karma). | `core/gpu/gpu_renderer.cpp` |

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
| A light with UsdLux ShadowAPI settings (shadows off, a shadow colour, a shadow distance) is sampled by its own light samples alone: a ray that continues the path stops at the first surface and would shadow it as any light. | Its light is the same; a large area light with such settings reflected in a glossy surface close to it is noisier than without them. A dome's BSDF samples become shadow rays of their own, at one more ray per bounce. | `LightData::ownShadows`, `core/lights/lights.h`; `bug-audit-2026-11.md` 20 |
| Geometry inside a sphere light (a bulb mesh, a light sunk into a wall). | Every surface inside is lit with the light's full radiance whatever it faces; Karma lights it by how it faces the centre. | `bug-audit-2026-11.md` 19 (L-0) |
| A cylinder light scaled non-uniformly across its axis; a dome light scaled non-uniformly. | A circular cylinder of the mean radius; a dome that is turned and mirrored but not stretched. A warning each. | `hydra/light.cpp` |
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
| `primvars:displayOpacity` of instance rate (per instance of a point instancer) on geometry without a material. | Opaque; the primvar on the prototype itself is read. | `HdNovaHasDisplayOpacity`, `hydra/primvarUtil.cpp` |
| A mesh without `st` under a textured material. | The texture is read at (0, 0): one colour. Karma gives every face its own parameters (a quad (0, 0) to (1, 1) from its first vertex). | `bug-audit-2026-11.md` 20 (T-5) |
| UsdPreviewSurface between metallic 0 and 1, and its specular workflow with a coloured `specularColor`. | MaterialX's graph of the shader, which is less saturated than Karma's own conversion of the USD shader. | `bug-audit-2026-11.md` 19 (MA-1) |
| Rough coloured metal and rough glass against Karma. | Nova compensates multiple scattering (coloured for metals, energy-preserving for glass); Karma's rough metal keeps the base colour and its rough glass loses energy. From roughness 0.6 on the two differ visibly. | `bug-audit-2026-11.md` 19 (MA-4, MA-5) |

## Geometry

| Limitation | What happens instead | Source |
|---|---|---|
| `nurbsCurves` prims. | Not rendered (Houdini's SOP Import never authors them). | `design/M9.md` |
| Karma's `instance_samples` for instances without velocity blur. | Instances follow every authored time sample; Karma samples them `instance_samples` times (2: a straight segment). | `bug-audit-2026-11.md` 19 (MO-2) |
| Authored normals on subdivided meshes; subdivision of bilinear meshes. | Normals come from the limit surface; bilinear meshes stay polygonal. | `reference.md` |
| Cylinders, cones and capsules are meshes of 96 sides, not analytic shapes. | They are the prim to 0.05 % of its radius. Karma renders them 6 % narrower (it subdivides UsdImaging's ten-sided cage), and spheres 8 % smaller, where Nova's are exact. | `hydra/implicitSceneIndex.h` |
| Long round linear curves (bug audit R1-11). | Reprojected onto the tube, leaving with an error-bounded offset. | `bug-audit-2026-11.md` |
| Two exactly coplanar overlapping surfaces. | Which one a ray reports follows the accelerator's traversal and can change between runs. | `bug-audit-2026-11.md` section 7 |
| A single polygon that reaches far beyond what is seen of it: rays leave a polygon by a few float ulps of its reach from the hit (Nova XPU: 10^-6 of it; the CPU half of that for polygons that are not axis-aligned, nothing for axis-aligned ones), the error of the ray-triangle tests. | Off a polygon that reaches 100 000 units the rays start up to 0.1 units above it: objects that close to it lose part of their shadow on it, and a light 5 units away lights it too brightly (a sphere on such a ground: the image 0.8 % brighter on XPU when it is axis-aligned, 1.6 % when rotated; 0.3 % on the CPU when rotated). At 10 000 units the difference is 0.1 % (XPU), below that none; from 10^6 units Nova XPU's image is 9 % and more off, the CPU's 3 % if the polygon is rotated. Split such polygons. | `leaveSurface`, `core/scene/ray.h`; `bug-audit-2026-11.md` section 12 |
| A point instancer that lists itself or one of its ancestors among its prototypes; `protoIndices` beyond the prototypes. | The process ends inside UsdImaging (a stack overflow in its prototype scene index, memory written out of bounds in its instancer data source) before the delegate sees a prim, as with any Hydra renderer. | `bug-audit-2026-11.md` section 12 |
| `motion:blurScale`, `motion:nonlinearSampleCount` and `motion:velocityScale` (UsdGeomMotionAPI). | Ignored. | `design/M8.md` |
| Scenes very far from the world's origin. A hit point is a float, rounded to 2^-23 of its largest coordinate (0.008 units at 100 000), and a ray leaves the surface by 8 times that along the normal (10^-6 of the distance from the origin). | Up to 10 000 times the size of the detail nothing shows (unit-sized objects 10 000 units out: no 20 x 20 pixel block off by more than 2 %). At 100 000 times, a few blocks differ (4 to 11 of 144 by more than 5 %, the image's mean within 0.5 %; Karma: none); at a million times the image is wrong (12 % dark with displacement; Karma 1 % off). Rendering relative to the camera would lift this; it is not done. | `offsetRayOrigin`, `core/scene/ray.h`; `bug-audit-2026-11.md` section 15 |
| In the viewport displacement is diced from the first view: neither a camera move nor another frame dices again (Karma's default, continuous dicing off). | The tessellation of the first view stays until the render is restarted. Final renders (husk, `nova_cli`) dice every frame from that frame's own camera. | `hydra/renderPass.cpp`, `design/M4.md` |

## Volumes

| Limitation | What happens instead | Source |
|---|---|---|
| `volumefilter` other than a box of width 1; tricubic lookups. | Box/trilinear, with a warning. | `hydra/volume.cpp` |
| Karma CPU's lattice-deformation velocity blur. | Velocity blur as a lookup offset. | `design/M8.md` |
| Field prims other than VDB and Houdini volumes; VDB grids of value types other than the scalar and vector ones the bridge converts. | The field is skipped, with a warning. | `hydra/field.cpp`, `houdini/volume_bridge/bridge.cpp` |
| Cryptomatte ids for volumes. | Volumes have no cryptomatte coverage. | `design/M8.md` 2.2 |
| Instanced VDB volumes. | Every instance builds its own bounds (the grids are shared): many instances of a large VDB cost build time and memory per instance. | `hydra/volume.cpp` |
| Field prims with a transform of their own or outside their Volume prim. | Placed by the field prim's transform (the UsdVol schema); Karma places every field by the Volume's. | `bug-audit-2026-11.md` 19 (V-7) |
| Karma's step-rate settings (`volumesteprate`, `volumeshadowsteprate`, `volumequality`, `volumesamplingchannel`). | Ignored: Nova tracks without bias at any setting; fire is slower than Karma at its default step rate. | `design/M8.md` D16 |

## Output and baking

| Limitation | What happens instead | Source |
|---|---|---|
| Karma render settings without a Nova equivalent. | One warning per stage that names them: "the image differs from Karma's". | `hydra/renderDelegate.cpp` |
| Utility path expressions other than `CO` (`aov:karma:utilitypathexpression`). | The values of the camera hit, with a warning. | `hydra/aov.cpp` |
| AOV sources Nova does not know; render product types other than raster, `karma:deep` and `karma:checkpoint`. | The plane keeps its clear value; the product is skipped; each with a warning. | `hydra/aov.cpp`, `hydra/renderDelegate.cpp` |
| Temporal denoising. | Each frame is denoised by itself. | `design/M7.md` D9 |
| Render passes (`husk --pass`): the pass's collections name paths of the stage, and geometry inside a native instance has its prototype's path in Hydra. | Such geometry is not pruned, hidden or made a matte by itself; a collection that holds the whole instance works. Geometry under a point instancer follows the instancer's path. | `hydra/renderPassSceneIndex.h` |
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

- **Scenes far from the origin, and meshes rotated in their own space.** Rays left a surface by Wächter and Binder's offset: 256 float ulps
  of each coordinate scaled with the normal's component along that axis. A mesh rotated in its own space and rotated back by its
  transform shadowed itself from 1 000 units on (a flat plane lost 27 % of its light), unit-sized objects 100 000 units out
  rendered 32 % dark with displacement, and objects of 0.0001 units 1 % bright. The offset is now 8 ulps of the hit point's
  largest coordinate along the normal; what is left is in the list above ("Geometry").

This one is described in [`bug-audit-2026-11.md`](bug-audit-2026-11.md), section 15.

- **Concave polygons covered what they do not.** Hydra's triangulation is a fan from a face's first vertex: a U covered up to 20
  units of area for 11, and more with displacement. Such faces are triangulated by their shape (`hydra/meshTriangulate.cpp`).
- **`husk --pass` rendered the whole scene for every pass.** The active render pass's collections (prune, renderVisibility,
  cameraVisibility, matte) are applied (`hydra/renderPassSceneIndex.cpp`).
- **A rotated DomeLight_1 with its pole at z pointed elsewhere than in Karma** (32 % in the image's mean). The pole axis turns the
  dome after the light's transform, as in Karma.

- **Cylinders, cones and capsules showed ten sides at every silhouette.** They were the cage UsdImaging makes of them, with
  smooth normals. They are meshes of 96 sides at the prim's size (`hydra/implicitSceneIndex.cpp`).

These four are described in [`bug-audit-2026-11.md`](bug-audit-2026-11.md), section 16.

- **Quads were split along the other diagonal than in Karma.** Hydra fans a face from its first vertex, Karma from its
  last: a quad 0 1 2 3 was 0 1 2 and 0 2 3 where Karma renders 0 1 3 and 1 2 3, so a quad that is not flat folded the
  other way, and a primvar or `st` that is not affine over a face was interpolated otherwise. Every face of more than
  three vertices has Karma's triangles (`hydra/meshTriangulate.cpp`).
- **UsdLux ShadowAPI was ignored.** `inputs:shadow:enable`, `inputs:shadow:color`, `inputs:shadow:distance`,
  `inputs:shadow:falloff` and `inputs:shadow:falloffGamma` are rendered as Karma renders them, on every light type and
  both devices (`shadowReach`, `shadowPass`, `core/lights/lights.h`).
- **Geometry without a material ignored `primvars:displayOpacity`.** It is see-through by the primvar, as in Karma, and
  its shadows let the rest of the light through.
- **Nova XPU rendered a cut-out opaque when its opacity was a `geompropvalue` itself**, with nothing computed from it:
  the opacity program then has no instruction, which the kernels took for no program.

- **A volume shader that gives something without its fields filled cells of 8 voxels** (a constant emission or
  medium on a VDB, a density with an offset). It runs where a lookup reaches a voxel of the fields it reads, the
  voxels and half a voxel around them, as in Karma.
- **Diffuse light passed the diffuse limit on surfaces that also have a glossy lobe** (and glossy light a reflect
  limit): a bounce took the ray class of the lobe that was sampled and the light of all of them. It takes one class
  with that class's light (the lights its own ray meets, which a ray of every class gathers, keep the whole).

These six are described in [`bug-audit-2026-11.md`](bug-audit-2026-11.md), section 20.
