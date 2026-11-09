# Nova reference

The details behind the [README](../README.md): how Nova is used in Houdini, every setting and per-object property,
what each milestone added (with the measured differences to Karma), the tools and the test suite.

## Use in Houdini

The simplest way: run `dist/NovaSetup-<version>.exe` (built by `tools/installer/build.ps1`, see
`tools/installer/README.md`). It installs Nova for the current user, registers it with Houdini 22.0 and adds an
uninstaller to Apps & features. By hand:

1. Close Houdini, because a loaded `hdNova.dll` cannot be overwritten.
2. Make the package visible to Houdini. Either:
   - copy `dist/nova.json` to `%USERPROFILE%\Documents\houdini22.0\packages\`, or
   - set `HOUDINI_PACKAGE_DIR` to the `dist` directory.
3. **Solaris:** in the viewport renderer menu, choose **Nova CPU** or **Nova XPU** (an NVIDIA GPU with OptiX; without one it renders on the CPU and says so).
4. **husk:** `husk --renderer Nova scene.usda -o out.exr`, or `--renderer "Nova XPU"`. The plugin ids `HdNovaRendererPlugin` and `HdNovaXpuRendererPlugin` also work.

Render settings. In Solaris (TAB menu, Rendering > Nova), Nova's nodes follow Karma's:
- **Nova (Setup)** builds what Karma (Setup) builds: a Nova Render Settings LOP, a Render Pass that uses it and a USD Render ROP set to Nova;
- the **Nova** LOP is the all-in-one node, like the Karma LOP: Render to Disk, Render to MPlay, Render to Disk in Background, the frame range, and all the settings of Nova Render Settings;
- the **Nova Render Settings** LOP authors a complete setup, laid out like the Karma Render Settings LOP:
  - render settings prim, output picture, camera, resolution, the Rendering Engine (CPU or XPU: the renderer husk and the render buttons use, authored as `husk:default_delegate` as Karma's engine menu does) and samples on top;
  - **Rendering**: Sampling, Limits, Camera Effects (motion blur, depth of field), Geometry and Shading;
  - **Image Output**: AOVs in the groups of Karma's Standard Render Vars (the same render vars Karma authors), Utility, Cryptomatte;
  - **Advanced**: Textures, Debug;
- the **Render Settings** LOP has a **Nova** tab (sparse: only values set to "Set or Create" are authored);
- the **Render Geometry Settings** LOP has a **Nova** tab for per-object properties;
- the viewport's display options (Render tab) carry the interactive subset.

A hip file saved before the package was installed needs *Initialize Parameters > Rebuild Renderer-Specific Parameters* once. The settings are attributes on a RenderSettings prim:

| Setting | Default | |
|---|---|---|
| `int nova:samples` | 64 | Samples per pixel. |
| `token nova:pixelFilter` | `gauss` | The pixel reconstruction filter. (`gauss`, `box`, `triangle`, `blackman`, `blackmanharris`, `point`) |
| `float nova:pixelFilterScale` | 1 | Multiplies the filter's width (0: point sampling). |
| `int nova:seed` | 0 | Varies the sample pattern between renders of the same frame. |
| `float nova:clampIndirect` | 10 | Clamps indirect light per sample against fireflies (0: off). |
| `int nova:maxDepth` | 64 | A safety cap on the scattering events of a path; the per-type limits decide the look. |
| `float nova:object:diffuseLimit` | 1 | Diffuse bounces (fractions dither between the neighbouring integers). Past the limit, rays only collect light. |
| `float nova:object:reflectLimit` | 4 | Glossy and mirror reflection bounces. |
| `float nova:object:refractLimit` | 4 | Refraction bounces. |
| `float nova:object:sssLimit` | 1 | Subsurface and translucency bounces. |
| `float nova:object:volumeLimit` | 0 | Volume scattering events. |
| `bool nova:object:thinWallAtRefractLimit` | 1 | Past the refraction limit (up to twice it), refractions continue straight on instead of stopping. |
| `bool nova:object:fakeCaustics` | 0 | Shadows and diffuse rays pass through this object's transmission (transparent shadows) instead of needing caustic paths. |
| `int nova:maxMotionSteps` | 8 | The most shutter samples of moving geometry, transforms and cameras. |
| `int nova:subdivLevel` | -1 | Uniform subdivision level of subdivision surfaces (-1: from the display complexity). |
| `int nova:subdivMaxTriangles` | 8000000 | Triangle budget of a subdivided or displaced mesh; above it, coarser tessellation and a warning. |
| `float nova:dicingQualityScale` | 1 | Multiplies every object's displacement dicing quality. |
| `float nova:offscreenQuality` | 0.1 | Dicing quality of displaced objects outside the camera's view. |
| `bool nova:textureAutoTx` | 1 | Converts images to tiled, MIP-mapped, linear .tx files in the texture cache directory. |
| `string nova:textureCacheDir` | "" | Where converted textures go (empty: $HOUDINI_TEMP_DIR/nova_tx). |
| `float nova:textureBlur` | 0 | Extra blur of every texture lookup. |
| `float nova:textureFilterWidth` | 0.25 | Texture footprint relative to the pixel's ray cone (0.25 as Karma; 0: the finest MIP level). |
| `bool nova:abortOnMissingTexture` | 0 | Stops the render when a texture file cannot be read. |
| `int nova:textureStats` | 0 | OpenImageIO texture statistics level printed at the end of a render. |
| `token nova:lightSamplingMode` | `lighttree` | How a light sample picks its light: the light tree, or `uniform` (by power alone). While at the default, Karma's `karma:global:light_sampling_mode` applies. |
| `token nova:lightStrategy` | `mis` | Debugging aid: which estimators sample direct light. (`mis`, `light`, `bsdf`) |
| `token nova:pixelOracle` | `uniform` | `variance`: adaptive sampling; Samples is then the maximum. |
| `int nova:oracleMinSamples` | -1 | Samples before a pixel may stop (-1: max(16, square root of Samples)). |
| `float nova:oracleThreshold` | 0.01 | The variance oracle's noise level. |
| `bool nova:denoise` | 0 | Denoises the final frame (OptiX). |
| `string nova:denoiseAovs` | `C` | The AOVs it denoises, by name. |
| `bool nova:viewportDenoise` | 0 | Denoises the viewport (a toggle on the viewport toolbar). |
| `int nova:denoiseStartPasses` | 4 | Passes the viewport shows noisy before denoising. |
| `int nova:iprProxyDepth` | 2 | Viewport proxy passes after an edit (0: none). |
| `token nova:device` | (the renderer's) | `cpu` or `xpu`: where Nova renders. Unset, Nova renders on the CPU and Nova XPU on the GPU. Not authored by the Nova LOPs. |
| `int nova:gpuTextureBudget` | 0 | Nova XPU's texture memory in MB (0: half the GPU memory free at the first texture upload); past it finer MIP levels are left out. |

The `nova:object:*` settings are the defaults of per-object properties. Constant primvars override them on a prim and its descendants:

| Primvar | Default | |
|---|---|---|
| `float primvars:nova:object:diffuseLimit` | 1 | Diffuse bounces (fractions dither between the neighbouring integers). Past the limit, rays only collect light. |
| `float primvars:nova:object:reflectLimit` | 4 | Glossy and mirror reflection bounces. |
| `float primvars:nova:object:refractLimit` | 4 | Refraction bounces. |
| `float primvars:nova:object:sssLimit` | 1 | Subsurface and translucency bounces. |
| `float primvars:nova:object:volumeLimit` | 0 | Volume scattering events. |
| `bool primvars:nova:object:thinWallAtRefractLimit` | 1 | Past the refraction limit (up to twice it), refractions continue straight on instead of stopping. |
| `bool primvars:nova:object:fakeCaustics` | 0 | Shadows and diffuse rays pass through this object's transmission (transparent shadows) instead of needing caustic paths. |
| `int primvars:nova:object:lightSource` | 0 | Efficient Emission Sampling: an emissive mesh or volume as a light source (0 no, 1 yes, 2 auto). Unset: Karma's `karma:object:treat_as_lightsource` (with `lightsource_diffusescale`, `_specularscale`, `_samplingquality`). M8's `nova:object:volumeLight` still works for volumes. |
| `int primvars:nova:object:curveStyle` | 1 | Curves: 0 ribbons facing the ray, 1 round tubes, 2 ribbons facing their normals, 3 ribbons where normals exist. Unset: Karma's `karma:object:curve_style`. |
| `float primvars:nova:object:widthScale` | 1 | Multiplies curve and point widths. Unset: Karma's `karma:object:width_scale`. |
| `int primvars:nova:object:pointStyle` | 1 | Points: 0 discs facing the ray, 1 spheres, 2 discs facing their normals. Unset: Karma's `karma:object:point_style`. |
| `int primvars:nova:object:velocityBlur` | 0 | Meshes, curves, points: 0 no velocity blur (time samples), 1 velocity blur, 2 acceleration blur. Unset: Karma's `karma:object:vblur` (also from the render settings). |
| `int primvars:nova:object:instanceVelocityBlur` | 0 | The same for a point instancer's instances (velocities, accelerations, angular velocities). Unset: Karma's `karma:object:instance_vblur`. |
| `int primvars:nova:object:accelerationSamples` | 2 | Shutter samples of acceleration blur (at most `nova:maxMotionSteps`). Unset: Karma's `karma:object:geosamples`, on point instancers `instance_samples`. |
| `int primvars:nova:subdivLevel` | -1 | Uniform subdivision level of subdivision surfaces (-1: from the display complexity). |
| `float primvars:nova:dicingQuality` | 0.5 | Displacement micropolygons per pixel edge (0.5: 2-pixel micropolygons, Karma's default). |
| `int primvars:nova:dicingDepth` | -1 | A fixed 2^depth segments per edge instead of the camera's rate (-1: off). |
| `bool primvars:nova:displacement` | 1 | Applies the material's displacement. |

The parameter files and the LOP are generated from one table, `houdini/tools/nova_settings.json` (`gen_nova_ds.py`, `build_nova_hdas.py`, run with `tests/houdini/run_hython.ps1`). `tests/houdini/test_solaris_ui.py` checks them against the delegate.

Motion blur and depth of field follow the render camera: its `shutter:open`/`shutter:close`, `fStop`, `focusDistance` and `focalLength`. The UsdRender settings `disableMotionBlur` and `disableDepthOfField` switch them off (husk does not pass the deprecated `instantaneousShutter`, so it has no effect there, as with Karma).

husk crop windows and overscan (`dataWindowNDC`, also beyond [0, 1]), `pixelAspectRatio` and `aspectRatioConformPolicy` are supported. A crop reproduces the matching pixels of the full-frame render exactly. As in Karma, a film back offset (`horizontalApertureOffset`/`verticalApertureOffset`) scales with the conformed aperture in final renders; the Solaris viewport conforms like Houdini's own drawing.

Render products: Nova renders the render vars Karma's LOPs author (see AOVs below) and Hydra's own AOV names. husk and `nova_cli` write them as multi-part EXRs; the Solaris viewport shows any of them through the viewer's output menu.

Light properties (on UsdLux lights, Karma's names; Nova's own `inputs:nova:light:*` win where both are authored): `inputs:karma:light:samplingquality` (`samplingQuality`) multiplies the light's share of light samples; `inputs:karma:light:equiangular` (`equiangular`) adds equiangular sampling along rays in volumes for point, spot, sphere and area lights. Light linking and shadow linking use UsdLux's `collection:lightLink` / `collection:shadowLink` (the Light Linker LOP authors them).

## Features by milestone

After M10 a bug audit fixed 71 verified defects across the renderer, Hydra, the bridges and the installer; the findings, fixes, tests and remaining limitations are in `docs/bug-audit-2026-11.md`.

Karma's Solaris nodes (M11, `docs/design/M11.md`, one design note per package `docs/design/M11-P1.md` ... `M11-P9c.md`): scenes built with the twelve visible Karma LOP types render in Nova as in Karma, on CPU and XPU, each feature measured against Karma CPU first and checked by a `tests/husk/check_karma_*.py` test:

- **Karma Render Settings:** every parameter with a Nova equivalent is read as a fallback (Nova's own setting wins only if it is authored): sampling and the variance oracle, seed, pixel filter, ray limits, fake caustics with Karma's factor and controls, internal reflection, roughness constraint, colour limits, dicing, texture settings, motion blur and DOF switches, curve/point styles, emission as light. Also outlines (contour, depth and crease lines with their AOVs), the look-dev overrides (headlight, dome, emissive only, simplified shading with AO and depth-cue fog), nested dielectric priorities, light portals and path guiding (Nova's own guiding field on CPU and XPU; Karma XPU ignores guiding). Settings Nova does not support warn once per stage.
- **Karma Shadow Catcher, Background Plate:** holdouts (Matte, Background) with Karma's shadow alpha and holdout LPE planes, per-ray-type render visibility on surfaces and volumes, the background plate composite (`use_background`), `.pic` images, `transformvector` spaces.
- **Karma Sky Dome Light, Physical Sky, Sky Atmosphere:** Karma's sky model as a textured dome, the sun, and the planetary atmosphere (Rayleigh, Mie, ozone, ground) as a medium in the path tracer.
- **Karma Blocker Light Filter, Fog Box:** light filters on every light sampling path; media bounded by closed surfaces.
- **Karma Render Products, Standard Render Vars, Cryptomatte:** per-var sample filters (`minmax`, `stats`, `ubox`, ...), raw ray sources, checkpoints with resume, deep EXR output, per-instance primvar cryptomattes.
- **Karma Texture Baker:** UV-space bakes (low/high/cage, UDIMs) of the tangent normal (MikkTSpace), displacement, occlusion, cavity, curvature, thickness, rounded normals and edges, position and shader exports, with the image filters; Nova XPU bakes on the CPU.
- Renders are reproducible run to run (stable object keys from prim paths, `docs/bug-audit-2026-11.md` section 7).

Nova XPU (M10, `docs/design/M10.md`):

- **Nova on the GPU:** CUDA wavefront kernels and OptiX 9 ray tracing on NVIDIA GPUs, chosen as **Nova XPU** in the viewport's renderer menu, with `husk --renderer "Nova XPU"`, with the Nova LOPs' Rendering Engine menu (CPU, XPU) or with `nova:device`.
- **The same renderer:** the bytecode VM, the closures, the lights and the light tree, light path expressions, volumes and the random walk run from the same code on both devices with the same random numbers, so a GPU render matches the CPU's image up to floating-point rounding (the reference scenes within 1 % in every pixel).
- **Everything renders on the GPU** but VEX materials (grey stand-ins with one warning, as in Karma XPU) and ray queries from shading (`kma_ao`, distance, curvature, rounded edges: their default values): meshes, subdivision and displacement, curves of every basis (normal-oriented ribbons tessellated), points, spheres, instancing, motion blur, depth of field, every native and MaterialX material, textures and UDIMs, every light with the light tree, emissive meshes as lights, light and shadow linking, AOVs, cryptomattes, volumes, SSS, hair, opacity, fake caustics, adaptive sampling and IPR proxy passes.
- **Speed:** per sample 9-16x the 16-thread CPU on the reference scenes (volumes and heavy layered materials 4x) after a setup of about 0.3 s per render (docs/benchmarks.md); an edit of a 4M-triangle scene shows in about 7 ms (only changed meshes are uploaded again).
- **Fallback:** without a usable NVIDIA GPU, or after a device error, Nova XPU renders on the CPU and says so once (`NOVA_XPU_DISABLE=1` forces it).
- Not yet: GPU and CPU together, multiple GPUs, demand-loaded textures, the denoiser on GPU memory.

Hair, curves, SSS and many lights (M9, `docs/design/M9.md`):

- **Curves and points:** `basisCurves` of every basis and wrap (linear, cubic Bézier, B-spline, Catmull-Rom) as Embree curves, and `points` as spheres or discs, with Karma's conventions: widths and `width_scale`, `curve_style` (camera-facing ribbons, round tubes, oriented ribbons), `point_style`, round caps, the face AOV per curve. Motion blur and instancing as for meshes.
- **Hair:** Karma Hair (`kma_hair`), Karma Fur (no medulla) and MaterialX `chiang_hair_bsdf` on Chiang et al.'s fibre model with Karma's measured conventions (melanin, absorption, colour, tilt, roughness controls, diffuse); a white fibre reflects and transmits everything. Coat, medulla and the second R lobe warn.
- **Subsurface scattering:** random walks (Standard Surface, OpenPBR, the Principled Shader, `subsurface_bsdf`), Karma's parameterization: matches Karma's 47 calibration scenes within 0.7 %.
- **Light tree:** every bounded light in a BVH over light bounds and emission cones (pbrt-v4's), lights at infinity by importance; 100x less variance than selection by power on a street of 2000 lights. `nova:lightSamplingMode` / Karma's `light_sampling_mode` switches back to uniform selection.
- **Light linking and shadow linking** through UsdLux collections; **instanced lights** (lights under point instancers) are one light per instance.
- **Emissive meshes as lights:** `treat_as_lightsource` turns every emitting triangle into a two-sided light: the same image as without, with 15-24x less noise far from the emitter (Karma: 12-18x), `L` events also for the camera, the light multipliers. Moving meshes and VEX materials stay plain emitters.
- **Equiangular sampling** per light in volumes (opt-in), MIS-combined with free flights.
- Not yet: Karma Hair coat, fur medulla, `sss_traceset`, two-level light trees for heavily instanced emitters. (Light filters: the Karma blocker since M11.)

Volumes (M8, `docs/design/M8.md`):

- **Volume prims and fields** from every Houdini path: VDB and native volumes, `.vdb` and `.bgeo.sc` files and in-memory SOP volumes (`op:`, the viewport), through Houdini's own resolver (`nova_volume.dll` next to `hdNova.dll`), converted once to NanoVDB float grids; vector fields as three components. Field edits (a SOP recook, a frame change) reload only the edited fields.
- **Volume materials** through the material pipeline: `kma_volume`, MaterialX `volume` with `anisotropic_vdf` / `absorption_vdf` / `uniform_edf` and their mixes, and the **Karma Pyro Material** from Houdini's recipe (smoke, fire in ramp and blackbody colour, scatter, shadow colour). Fields are read through `geompropvalue` / primvar reads; volumes without a material are Karma's default (white, isotropic, extinction = density). Karma's blackbody (power-scaled) is measured and tabulated.
- **Unbiased rendering:** majorant grids from interval execution of the shader bytecode (a transfer table or a sampled estimate where intervals give no bound, with a warning), spectral tracking with survival biasing for free flights, ratio tracking for shadows and alpha, emission at every tentative collision. No step rate to tune.
- **Scattering:** Henyey-Greenstein and Karma's two-lobe mix, light samples with MIS, the Volume ray limit (`nova:object:volumeLimit`, or Karma's `karma:object:volumelimit`), Karma's extinction and contribution modifiers with Karma's level semantics, the lights' diffuse multiplier.
- **Emissive volumes as lights:** `karma:object:treat_as_lightsource` (or `nova:object:lightSource`) light-samples a volume's emission (by its cells' emission bounds) and makes it an `L` event, with `lightsource_diffusescale` / `specularscale`.
- **Motion blur:** transform blur, and velocity blur by lookup offset (`nova:object:velocityBlur`, else `karma:object:vblur` on the volume or the render settings, `volumevblurscale`, the `vel` or `v` field).
- **Overlapping volumes** are one medium; volumes render inside and behind glass.
- **AOVs:** alpha is the volumes' opacity; light path expressions see volume scattering as `V` (no scattering class, as in Karma), volume emission as `O` (`L` for volume lights), `A` after `V`. Volumes stay transparent to the first-hit AOVs (Karma averages its sampled volume events into P and depth).
- **Matches Karma CPU** on the calibration cubes, furnaces, single scattering, the modifiers, volume lights and the research's pyro sim with and without the Pyro Material, fire included, within 0.1-1.8 % (`tests/houdini/test_volume_karma.py`; emitters against Karma at a fine step rate: at its default step rate Karma renders absorbing emitters up to 10 % bright). One documented difference: smoke lit by a volume light at volume limit 0 is 4.5 % darker than Karma's.
- **Speed:** on the pyro sim 2.3-4.2x faster than Karma CPU at equal accuracy (docs/benchmarks.md); scenes without volumes are unchanged.
- Mesh-bounded volumes (uniform volume materials on closed meshes, the Karma Fog Box) since M11 P5.
- Not yet: Karma's lattice-deformation velocity blur (renders as lookup offset), `volumefilter` other than box of width 1 (warns).

Look-dev and IPR (M7, `docs/design/M7.md`):

- **NVIDIA OptiX denoiser.**
  - Final frames in husk and `nova_cli`: `nova:denoise` with `nova:denoiseAovs` (default `C`), or the Karma Render Settings LOP's Denoiser menu (Karma's `karma:global:imagefilter`, separate `_denoise` AOVs included).
  - The viewport: the Denoise toggle on the viewport toolbar (`nova:viewportDenoise`), from `nova:denoiseStartPasses` passes on.
  - The beauty and the listed LPE AOVs denoise together, with the first-hit albedo and normal as guides. Alpha stays exact.
  - Without an NVIDIA driver Nova renders as before and warns once: the CUDA and OptiX DLLs are loaded at run time.
- **Adaptive sampling** (`nova:pixelOracle` = `variance`, `nova:oracleThreshold`, `nova:oracleMinSamples`; Samples is then the maximum).
  - Each pixel stops once its noise estimate is low enough, decided at power-of-two checkpoints, so renders stay deterministic.
  - At equal error it is about 3x faster on scenes with mixed noise (glass, docs/benchmarks.md) and about even on uniformly noisy ones.
  - The sample count per pixel is an AOV (Karma's `ray:pixelsamples`).
- **Fast IPR.**
  - After an edit the viewport first renders one pixel per 4x4, then per 2x2 block (`nova:iprProxyDepth`). Those samples count towards the image.
  - On a million-triangle scene at 1280x720, every measured edit shows its first image within 23-94 ms. Deforming meshes refit their BVH.
- **Faster rays:** 3-10 % on every benchmark scene compared with M6.


AOVs (M6, `docs/design/M6.md`). Nova speaks Karma's AOV vocabulary: the render vars of the Karma Render Settings, Karma Standard Render Vars, Cryptomatte and LPE Tag LOPs and of `kma_aov` render in Nova under their names, so one Solaris setup renders in both renderers.

- **Light path expressions** in Karma's dialect of OSL's: events `R`/`T` × `D`/`G`, light `L`, emission `O`, albedo `A`, background `B`, positional tags and labels (`<RD'wall'>`, `<L.'key'>`, `<...'coat'>`), and the prefixes `unoccluded;` and `shadow;` and `!`. Karma's measured conventions hold (`C.*` = `C.*[LO]`, coats labelled `coat`, SSS `sss`, sheen a plain `D` event, `shadow;` the occluded fraction).
  - All bound expressions compile into one automaton, and each path carries a 16-bit state.
  - A closure whose lobes fall into different components splits. Light samples split exactly per lobe; continuations choose one component in proportion to its share. The components therefore sum to the beauty per sample, and every component is unbiased.
  - Tags: `inputs:karma:light:lpetag` on lights, the constant primvar `karma:object:lpetag` on objects, `karma:lpetag` on a material's surface shader.
  - Karma's standard components match Karma CPU within the documented model differences (reference `aov_components`, 23 planes). Light groups and every partition add up to the beauty.
  - Not yet: `holdout;` (black, with a warning).
- **Utility AOVs** at the pixel centre's first hit, with Karma's measured conventions:
  - `P`, `P_camera`, `N`/`Ng` (as authored), camera-space normals, the facing ratios, depth, `hitdist`;
  - `primid`/`element` (the polygon index), `objectid`;
  - motion vectors and velocity over the shutter;
  - any primvar (`st`, ...);
  - material AOVs (`kma_aov`, a surface shader's `aov:<name>` inputs), `export_basecolor`;
  - ambient occlusion;
  - the first-hit albedo, now also of VEX materials.
  - Ids into float images are written as floats (husk's `husk:format float`).
  - Differences: unfiltered (Karma filters P/N with ubox); meshes without `st` give 0 (Karma: parametric coordinates); VEX `export_*` parameters are not AOVs yet.
- **Cryptomatte** for objects, materials and any string or int primvar. Names, MurmurHash3 ids, manifests (in the header or as a sidecar file), ranks and coverage are as in Karma (the manifests match Karma's entry for entry), through husk and `nova_cli`.
- **Cost:** none with the beauty only. Karma's standard set of LPEs costs about 6-13 % of the render time, a single component 2-6 % (`docs/benchmarks.md`). Buffers other than the beauty are copied from the film only when they are read or the render ends.

VEX materials (M5, `docs/design/M5.md`):

- **Principled Shader** (Houdini's VOP, `principledshader::2.0`): rendered natively. Its parameters, textures, bump and normal maps, emission, opacity and displacement map onto Nova's Standard Surface with the Principled Shader's own rules (Disney sheen and clearcoat, Schlick metal, reflectivity as an effective IOR, its texture and UV conventions). Not supported, with a warning: packed colour, noise displacement, dispersion, rounded edges, `opdef:` textures.
- **VEX Material Builders and other VEX code** (`info:sourceCode` in the `vex` context): run through the CVEX bridge, `nova_cvex.dll`, next to `hdNova.dll`. The surface function is rewritten into a CVEX function, compiled once per code with VCC, and run in batches of 256 shading points on a CVEX context per thread; its BSDF stays in VEX and is evaluated and sampled there (`eval_bsdf`, `sample_bsdf`). Supported:
  - parameters from the material's values, from primvars of the parameters' names, or from a connected UsdPrimvarReader;
  - `Of` (cut-out and fractional opacity, evaluated per hit only when it varies), `Ce` (emission);
  - `displace` code (the VEX displacement terminal), diced like any displacement;
  - IPR: value edits rebind without compiling, code edits compile the new code.
  - Code that calls `trace`, `gather`, `illuminance` or other ray-tracing functions renders the material's preview, with one warning, as in Karma XPU.
  - In husk, hython and `nova_cli` VEX's BSDFs are sampled exactly. Inside Houdini itself, Houdini's native GGX lobe faults in `sample_bsdf`; Nova catches the fault, warns once and samples VEX materials from a fixed mixture for the rest of the session (unbiased, but glossy lobes are noisier and perfect mirrors and refraction are not sampled). `NOVA_CVEX_SAFE_SAMPLING=1` forces this mode.
  - Not yet: VEX AOV exports (`export_*` parameters), fake caustics through VEX materials, light/fog/volume VEX shaders.
- A shading point of a stock builder costs about three to four native ones (`docs/benchmarks.md`).

Material workflow in Solaris (M4, `docs/design/M4.md`):

- **Karma's conventions:** materials of the Karma Material Builder (`kma` context) render as in Karma. Supported:
  - 35 of Karma's 55 `kma_*` shading nodes: ray switch and ray imports, ramps, hex tiling, Voronoi, curvature, AO, distance, rounded edges, melanin, ...;
  - the `dot`-note ray imports and `karma_mikkt` normal maps.
  - Not yet: room maps (hair, fur and point clouds: M9).
- **Ray queries from shading:** `kma_ao` and MaterialX `ambientocclusion`, `kma_distance`, `kma_curvature` and `kma_roundededge` trace rays from the shading point, with Karma's measured estimators.
- **Generic MaterialX surfaces:** any pbrlib BSDF graph (layer, mix, add, multiply over every 1.39 leaf, including Burley and the generalized Schlick with F82), glTF PBR, Disney principled, Lama, and mixes of surface shaders (`mix_surfaceshader`, native models included). Expanded through their own nodegraphs, Standard Surface, OpenPBR and UsdPreviewSurface give the native BSDFs.
- **Displacement:** float, vector (Karma's tangent frame) and UsdPreviewSurface displacement in object space. Meshes and spheres are diced from the camera at Karma's dicing quality (`primvars:nova:dicingQuality`, 0.5 = 2-pixel micropolygons), subdivision surfaces on their limit surface, polygon meshes bilinearly. The dicing is watertight, motion-blurred like Karma's, and IPR edits of a displacement re-dice only the bound meshes.
- **Per-ray-type limits** (diffuse, reflection, refraction, SSS, volume; fractional, per object; Karma's defaults 1/4/4/1/0), light-only rays past them, thin walls at the refraction limit, and **fake caustics** (`nova:object:fakeCaustics`: transparent shadows through glass).
- **Solaris UI:** Nova tabs on the Render Settings and Render Geometry Settings LOPs, the viewport's display options, a **Nova Render Settings** LOP, and the **Nova Material Builder** (TAB menu of material networks, under Nova).

Materials (M3, `docs/design/M3.md`):

- **MaterialX** from Solaris and husk: Standard Surface 1.0.1/1.0.0, OpenPBR 1.1, `surface_unlit` and UsdPreviewSurface (also the auto-generated preview of materials Nova cannot read yet), in Karma's render-context order (`kma`, `mtlx`, `vex` since M5, universal). Every MaterialX 1.39 node with a numeric output (740 of 746 definitions, all stdlib/pbrlib/Houdini nodegraphs, hex tiling and flakes since M4) runs through Nova's shading compiler and VM, with Karma's semantics (VEX safe math, noise bit-exact to Karma).
- **BSDFs:** exact layering per MaterialX/OpenPBR with energy compensation; measured differences to Karma are documented per reference scene (`tests/reference/manifest.json`).
- **Textures** through OpenImageIO: UDIM, address modes, color spaces decided as Karma does, automatic conversion to linear tiled MIP-mapped EXRs, `.rat` through `imaketx`, ray-cone footprints with Karma's MIP choice, stochastic filtering.
- **Normals:** normal maps and bump (`heighttonormal`) with Karma's tangent frame from `st`, anisotropy and its rotation.
- **Opacity:** cut-out and fractional, in camera, shadow and indirect rays (stochastic, alpha included).
- **Spheres:** UsdGeomSphere prims (the Sphere LOP) render as exact spheres, also ellipsoids, instanced and moving.
- **IPR:** value edits never recompile; a structural edit recompiles one material in about 0.1 ms.

Geometry, lights and sampling (M2):

- Meshes; UsdGeom implicit prims other than spheres are converted to meshes.
- Unbound geometry renders Lambert from `primvars:displayColor` in any interpolation (constant, per face, per point, per face-vertex).
- UsdLux lights, with the semantics measured on Karma CPU (`tests/karma/compare_lights.py`):
  - distant (soft angle), dome (constant or latlong texture), sphere (`treatAsPoint` = point light), rect (optional texture), disk and cylinder lights;
  - `intensity` (negative values subtract light), `exposure`, `color`, color temperature, `normalize` (world-space area), `diffuse`;
  - as in Karma: distant-light angles above 180 degrees act as 180; sphere lights also light points inside them; rect-light textures wrap around at the border;
  - ShapingAPI cone, softness, focus and focus tint (a cone angle of exactly 90 degrees means no cone, as in Karma);
  - Karma's light properties `karma:light:renderlightgeo` (camera visibility; lights are invisible to the camera by default), `karma:light:singlesided` and `karma:light:recttextureflip`. A camera-visible light is opaque, also in the first-hit AOVs. A light lying on a surface (within float precision) shows in front of it if the camera sees its emitting side, otherwise the surface shows.
  - Not yet: other dome texture formats, IES profiles, `treatAsLine` (rendered as a cylinder), ellipsoids (a non-uniformly scaled sphere light renders as a sphere of the mean radius, with a warning), mesh lights, portals, light linking.
- Textures are read with Houdini's OpenImageIO (EXR, HDR, PNG, JPEG, TIFF, ...; `.rat` files, such as Houdini's HDRIs in `$HFS/houdini/pic/hdri`, through Houdini's `imaketx`) and converted to the OCIO `scene_linear` space (Linear Rec.709 in Houdini's config, ACEScg in ACES studio configs), deciding the source color space as Karma does: 1- and 2-channel images are linear; otherwise the config's file rules apply (Houdini's: PNG, JPEG, TGA and TIFF are sRGB, EXR linear, `*ACEScg*` names ACEScg), then the file's metadata (a `.rat` file's stored color space), then the extension (`.tx` files are sRGB, as Houdini's `imaketx` writes them). As in Karma, `colorSpace` metadata on a dome or rect light's `inputs:texture:file` is ignored (it applies to material textures). Without `$OCIO` (nova_cli) `scene_linear` is taken to be Linear Rec.709. NaN and infinite texels are set to 0.
- Warnings (unreadable textures, unknown settings) also go to stderr, because husk does not print Hydra's diagnostics.
- Sampling: Owen-scrambled Sobol sequences, pixel filters by filter importance sampling, one light per bounce chosen in proportion to its power, and multiple importance sampling of light and BSDF samples (textured domes with MIS compensation).
- AOVs: `color`, `depth`, `primId`, `instanceId`, `normal`, `albedo`. The first-hit AOVs come from a ray through the pixel center at the frame time (clamped into the shutter) and are not filtered; depth is measured from the camera at that time.
- Native instancing: point instancers (positions, `orientations`/`orientationsf`, scales, `invisibleIds`, nesting) and USD native instances, as Embree instance arrays (one transform per instance, 48 bytes). `instanceId` is the flattened instance index (outermost instancer slowest), as in Hydra picking. Editing an instancer moves the instances without rebuilding the prototype geometry. Per-instance `displayColor` (point instancer primvars, colors on native instances) applies where the prototype has no color of its own; the innermost level wins.
- Motion blur over the camera shutter, from the authored time samples (`tests/scenes/motion*.usda`):
  - transform blur of meshes, cameras, point instancers and their instances (positions and scales linear, orientations slerped as in USD, nested levels); transforms are decomposed so that rotations follow the arc;
  - deformation blur of points and normals (constant topology);
  - arrays whose length changes within the shutter (meshes changing topology, particles born or dying) hold their value at the frame without blur, as USD holds them (Karma does the same);
  - everything is resampled to uniform steps that resolve the finest authored spacing within the shutter, at most `nova:maxMotionSteps`;
  - velocity and acceleration blur (docs/design/M8.md D19, `tests/scenes/motion_velocity.usda`): meshes, curves and points move by their `velocities` (Houdini's `v`, units per second) and `accelerations` instead of their time samples, point instancers' instances by their velocities, accelerations and `angularVelocities`, all as Karma decides: Karma's `karma:object:vblur` / `instance_vblur` (0 none, the default; 1 velocity; 2 acceleration) on the object, inherited or on the render settings, Nova's `nova:object:velocityBlur` / `instanceVelocityBlur` winning; acceleration blur samples the shutter `geosamples` / `instance_samples` times (2 by default, as Karma) or `nova:object:accelerationSamples`. Velocities convert with the stage's time codes per second. Through pxr's velocity-resolving scene index, so the viewport re-blurs live when a setting changes, CPU and XPU alike (volumes: velocity blur of their fields, M8).
- Depth of field: thin lens with a planar focus plane, aperture radius `focalLength / (2 fStop)` as in Karma.
- Subdivision surfaces (Catmull-Clark and Loop) with OpenSubdiv 3.7 from Houdini: the limit surface is tessellated on the host into a watertight triangle mesh with exact limit positions and normals, for every motion step (`tests/scenes/subdiv.usda`):
  - all USD subdivision tags (creases, corners, holes, boundary and face-varying rules, triangle rule, orientation) with the conventions of `PxOsdRefinerFactory`; infinitely sharp creases keep flat shading on both sides;
  - as in Karma, meshes are subdivided from render complexity "high" on (refine level 3; husk's default "veryhigh" gives level 3, "high" level 2) and render as their control hull below; `primvars:nova:subdivLevel` (constant int) overrides the level per mesh; displaced meshes are diced instead (see above); faces with fewer than 3 vertices are ignored (with a warning);
  - authored normals are ignored on subdivided meshes; bilinear meshes and UsdGeom implicit prims (cylinders, cones, ...) stay polygonal.

## Tools

- `nova_cli scene.usda [--settings /Render/rs] [-o out.exr] [--camera /cam] [--frame F] [--res WxH] [--spp N] [--threads N] [--timeout S] [--purposes ...] [-q] [--profile report.txt]` renders a scene without husk, the HDK or a Houdini license (`build/<preset>/tools/nova_cli/<config>/`; needs `$HFS/bin` on `PATH`). It reads the RenderSettings, products and render vars like husk and writes the same multi-part EXR layout, cryptomattes and sidecar manifests included. `--profile` writes a sampling profile of the render (functions and source lines; build the RelWithDebInfo configuration for symbol names).
- `tests/reference/tools/bench_lpe.py --cli <nova_cli>` measures the cost of 0, 1, 20 and 74 LPE render vars (`docs/benchmarks.md`).
- `nova_imgcmp` compares noisy renders against a reference (8x8 block z-tests with a Sidak-corrected error rate, a global mean test, HDR-FLIP and relMSE as diagnostics); `--make-ref` turns two independent reference renders into a mean and a standard-error image.
- `tests/reference/tools/make_references.py` regenerates the Karma references (needs a Karma license).
- `tests/houdini/run_viewport_check.ps1 -Out <dir>` checks the installed package in the Solaris viewport: a separate Houdini instance renders `tests/scenes/basic.usda` with Houdini VK, Nova (also after live edits: a prim, a MaterialX material, a Nova Material Builder material with Karma's AO and its displacement, a Principled Shader, a VEX Material Builder and an edit of it) and Karma CPU, saves the captures and quits (needs a GUI and a license; never touches a running session). `-Only xpu` checks Nova XPU in the viewport.

## Tests

Tests (`ctest --preset release`):

| Label | Contents |
|---|---|
| (none) | Catch2 unit tests of `nova_core`: math, sampling, lights, acceleration structure (incl. the hit filter for cut-out opacity), analytic render checks (white furnace, distant and dome light), determinism, and the statistical BSDF harness (`tests/bsdf_stats.h`: chi-square, albedo estimators) |
| `hydra` | The render pass and delegate driven like an interactive viewport: convergence after camera/setting changes, host stop and buffer replacement; MaterialX library loading and the OIIO TextureSystem inside hdNova |
| `husk` | husk renders of `tests/scenes` with the package staged in the build tree (`build/<preset>/stage/<config>/nova`): beauty values, crop windows, AOVs, instancing, motion blur, depth of field and subdivision against analytic coverage, texture color spaces |
| `cli` | The same scenes rendered with `nova_cli` (no husk, no license): pixel values, crops, AOV layout |
| `reference` | `tests/reference`: one test per scene of `manifest.json`. Nova renders the scene twice and `nova_imgcmp` compares it statistically with a Karma CPU reference (or an analytic value), see `tests/reference/README.md` |
| `cvex` | The CVEX bridge (`nova_cvex_tests`): the VEX rewrite, compiling and running surface and displacement code, VEX's BSDF conventions, furnace and MIS checks through the renderer (also with the sampling-free fallback, `cvex_safe_sampling`), opacity and emission |
| `houdini` | hython in an isolated environment: the Solaris UI, the Nova Material Builder, and a stock VEX Material Builder rendered by husk with Nova and Karma |

The husk and reference tests run Houdini with an isolated, empty preference directory, so installed packages never interfere.
Unit tests include furnace tests (white sphere in a uniform dome, area-light furnace boxes, MIS furnaces with complementary dome textures) and the emitted power of every light type.
