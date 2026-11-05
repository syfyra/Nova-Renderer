# M7 research: denoising with the OptiX denoiser (Houdini 22.0.466, OptiX 9.1, RTX 5080)

Scope: docs/PLAN.md M7, "OptiX-Denoiser (Viewport-IPR + Final)". The user wants the OptiX denoiser, not OIDN. Adaptive sampling is covered by a separate report.
The report answers five questions from measurements on this machine:
1. How Houdini's husk image filters (`TIL_DenoiseOptix7.dll`, `TIL_DenoiseIntel.dll`) are configured, what they read and write, and whether they work for a third-party delegate (tested with Nova and with Karma).
2. Whether the Solaris viewport offers a denoiser to third-party delegates, and how Karma does viewport denoising.
3. The OptiX 9.1 denoiser API: initialization without an SDK, the CUDA driver API it needs, models, guides, formats, intensity, tiling, memory and alpha. A standalone prototype measures time and quality.
4. CUDA and OptiX inside the Houdini process.
5. A recommended architecture for final frames and for the viewport, and the guides Nova must provide.

Sources:
- the Houdini install (`$HFS` = `C:\Program Files\Side Effects Software\Houdini 22.0.466`, `$I` = `$HFS/toolkit/include`);
- the help archives in `$HFS/houdini/help` (extracted with 7-Zip into a temp directory; described here, not copied);
- the OptiX 9.1 headers from github.com/NVIDIA/optix-dev in `build/vs2022/_deps/nova_optix_headers-src/include` (BSD-3-Clause), with CUDA 12.9's `cuda.h`;
- `dumpbin` and a strings scan of Houdini's denoiser DLLs, only to see their imports and option names;
- one NVIDIA developer-forum thread (marked "web").

Every Houdini process ran isolated: hython through `tests/houdini/run_hython.ps1`, husk and hoiiotool through `docs/research/m6/probes/husk_env.ps1` (own preference directory, no user packages, the installed Nova package skipped, the staged Nova from `build/vs2022/stage/Release/nova` on `HOUDINI_PATH`). The user's Houdini session was not touched.
Machine: RTX 5080 (16 GB, sm_120, driver API 13040), Ryzen 7 9800X3D, Windows 11.
Probes and the prototype are in `probes/` and `prototypes/` (section 7). They contain no SideFX code.

Each claim is tagged with its source: **(measured)**, **(header)** with file:line, **(help)** with archive/page, **(imports)**/**(strings)** for the binary scans, **(probe)** for hython output, **(web)**.

---

## 0. Findings in short

1. **Houdini has three final-frame denoise paths, and all three run the same plug-in.** The plug-in is a `TIL_RasterFilter` (`$I/TIL/TIL_RasterFilter.h`, a public HDK interface) loaded from `$HOUDINI_DSO_PATH/img_filter`.
   - Karma's built-in image filter, `karma:global:imagefilter = [["denoise", {...}]]`, runs inside Karma (`BRAY_Denoise.dll` imports `libTIL`).
   - **husk's COP filter lists**: `husk:orderedImageFilters` points at `HoudiniImageFilterList` prims that hold `HoudiniImageFilter` children (`type = "denoiseai"`, `filterparm:denoiser = "optix"`). husk runs them after the render on the buffers it mapped from the delegate.
   - `--slap-comp`, `idenoise` and the Denoise AI COPs.
2. **The husk filter-list path works with Nova unchanged (measured).**
   - `husk --renderer Nova` with an OptiX filter list cut the relMSE of a 16 spp Cornell box from 8.98e-3 to 1.82e-4 (49x).
   - The guides were Karma's vocabulary, as Nova already renders it: `albedo` = lpe `CDA`, `N` = raw `ray:hitN`.
   - The vars the Karma Render Settings LOP auto-creates for filter lists (`husk:bypassSave = 1`) also work: they are rendered, used and not written.
   - Karma's built-in `karma:global:imagefilter` is delegate-internal: **Nova ignores it**, and the output was bit-identical to the undenoised render.
3. **Houdini's OptiX filter has fixed choices that a native implementation can avoid (measured).**
   - It always denoises alpha, which doubles the cost and turned an exact alpha of 1 into 0.9977.
   - It tiles at 2048 px with an 8 px overlap; OptiX asks for 128 px. Above 2048 px this leaves a measurable seam (a column step 2.6x the untiled image's) and a 4.6 % deviation in the last column.
   - It hands OptiX the image bottom-up, which costs up to 14 % relMSE on a textured scene.
   - With "All Color AOVs" it also denoises the albedo guide plane.
   - It runs only in husk: not in the viewport, not in `nova_cli`.
4. **The viewport has no generic denoiser for Hydra delegates (help, files).**
   - The Image Filter page warns that for third-party delegates filters run in husk only.
   - The viewport's "Denoise if Needed" is Houdini's own Vulkan lighting.
   - Karma denoises inside the delegate. It declares a viewport toolbar toggle `karma:hydra:denoise` ("Enable Denoising", icon `VIEW_display_denoise`) in `$HFS/houdini/soho/parameters/BRAY_HdKarma*_Viewport.ds`, uses `karma:global:imagefilter`, and waits `karma:global:ipr_proxy_for_imagefilter` (default 2) passes before the first denoise.
   - Nova can declare the same kind of toggle in its own `HdNovaRendererPlugin_Viewport.ds`.
5. **OptiX 9.1 works without an SDK and without `cuda.lib` (measured, prototype).**
   - `optixInit()` from `optix_stubs.h` loads `nvoptix.dll` from the driver store. `nvcuda.dll` can be loaded with `LoadLibrary`, resolving about 30 driver entry points by name; `cuda.h` is still needed at compile time.
   - A cold process took 0.16-0.28 s from start to a ready OptiX context.
   - Steady-state denoise with the AOV model, albedo + normal guides, float RGBA, pinned host buffers:

     | Resolution | Upload + intensity + invoke + download | Invoke (GPU) | Peak VRAM |
     |---|---|---|---|
     | 960x540 | 2.45 ms | 1.85 ms | 128 MB |
     | 1920x1080 | 8.0 ms | 5.7 ms | 474 MB |
     | 3840x2160 | 31 ms | 22.6 ms | 1.85 GB |

   - Creating and setting up a denoiser costs about 10 ms. The first invoke in a process takes 6 ms at 960x540.
6. **Guides.**
   - On a textured scene at 4 spp, an albedo guide matters. Nova's Hydra `albedo`, the total first-hit albedo, beat Karma's diffuse-only `CDA`: relMSE 0.0140 against 0.0184; 0.0213 without guides.
   - On untextured scenes the guides changed relMSE by at most about 5 %, and pixels at edges were not better with them.
   - World-space versus camera-space normals made no difference within 0.6 %.
   - Images must be passed top-down.
7. **In-process coexistence works (measured, hython).**
   - Houdini's own OptiX filter (own CUDA context) and the prototype (the device's primary context) ran in either order in one hython process.
   - In Houdini the primary context was already active, so retaining it cost 0.02 ms and an OptiX context 27-51 ms.
8. **Recommendation** (section 6):
   - a native OptiX denoiser in `core/denoise` (HDK-free, CUDA driver API loaded at run time), used by the delegate for final frames and the viewport and by `nova_cli`;
   - the husk filter-list path stays usable as a Karma-compatible extra;
   - Nova honours Karma's `karma:global:imagefilter` denoise entry, so the Karma Render Settings LOP's Denoiser menu works for Nova.

---

## 1. Final frames through husk

### 1.1 The denoise paths and the plug-ins behind them

| Path | Configured by | Runs in | Evidence |
|---|---|---|---|
| Karma built-in image filter | RenderSettings `string karma:global:imagefilter`, a JSON list such as `[["denoise", {"engine": "optix", ...}]]` | Karma (delegate): log lines "Adding image filter module: denoise" and "Running image filters" | measured; `BRAY_Denoise.dll` exports `newBRAYImageFilter` and imports `libTIL.dll` (imports) |
| COP image filter lists | RenderSettings `rel husk:orderedImageFilters` → `HoudiniImageFilterList` prims | husk, after rendering: it runs `$HFS/houdini/python3.13libs/husd/slapcomputils.py --prim <list> --aovs "<all>" --caovs "<color>"`, builds a `copnet_filterlist` under `/img` and cooks it on the mapped AOVs | measured (husk log), help `nodes/lop/imagefilter`, `nodes/lop/rendersettings` (Ordered Image Filters) |
| `--slap-comp <graph or cpio>[?filterlist=...]` | husk command line (USD Render ROP Slap Comp tab) | husk | `husk --help` (measured); help `ref/utils/husk` |
| `idenoise`, Denoise AI COP (Copernicus and COP2), Render View IPR | tool options / node parameters | Houdini processes | help `ref/utils/idenoise`, `nodes/cop/denoiseai`, `render/optixdenoiser` |

Every path ends in a `TIL_RasterFilter` plug-in:
- `$HFS/houdini/dso/img_filter/TIL_DenoiseOptix7.dll` (registered as `optix`);
- `TIL_DenoiseIntel.dll` (`oidn`).

The interface (header, `$I/TIL/TIL_RasterFilter.h`):
- a factory registered by `newRasterFilter()` in DSOs found in `$HOUDINI_DSO_PATH/img_filter`;
- `setOptions(UT_Options)`, `setAuxPlane()` for the albedo/normal/motion-vector/previous-frame tokens, and `apply(PXL_Raster*)`, or a layer variant with an upscale and a first-frame flag;
- raster data as a `PXL_Raster`, a Vulkan buffer or a raw CUDA pointer (`RDT_RAW_CUDA`, with `matchesDevice(uuid)`);
- `isInteractive()`.

The OptiX plug-in (strings, imports):
- imports `nvcuda.dll` directly (`cuCtxCreate_v2`, `cuCtxPushCurrent_v2`/`PopCurrent`, ...);
- statically links the CUDA runtime (per-thread-stream `cuda*_ptsz` names);
- finds `nvoptix.dll` through the same registry lookup as `optix_stubs.h`;
- reads options `blendfactor`, `tilesize`, `tileoverlap` and `denoisealpha`;
- supports the model kinds AOV, TEMPORAL(_AOV) and UPSCALE2X;
- calls `UT_MemoryResource::requestMemoryFree` (`$I/UT/UT_MemoryResource.h`, Houdini's shared GPU-memory arbitration).
- `HOUDINI_DEBUG_OPTIX_DENOISE=4` (help `ref/env`) makes it print its configuration, memory and OptiX's own log.

Karma's `--filters` list (measured) contains the image filters `Denoiser-Optix`, `denoise`, `ocio`, `tonemap`, `uv_enlarger`, `background`, `range`, `shadow` and `shift`.

### 1.2 What the LOPs author (hython probes `probe_krs_denoise.py`, `probe_imagefilter.py`; dumps in `probes/results/`)

**Karma Render Settings LOP, Image Output > Filters.**

COP image filter lists:
- `cop_createaovs` (default on);
- the multiparm `cop_filter_enable_#` / `cop_filter_#` (Filter Prim) / `cop_filter_create_#`.

Karma Built-in Image Filters:
- `denoiser` ∈ {`off`, `optix`, `oidn`};
- `denoise_aovs` (default `C`), `denoise_usealbedo` (on), `denoise_useN` (on), `denoise_separate_aovs` (off), `denoise_cpu_only` (off);
- the tone-map and OCIO parameters.

What these author (diffed against the default):

| Setting | Authored |
|---|---|
| `denoiser = optix` | `karma:global:imagefilter = '[["denoise", {"engine": "optix", "use_albedo_input": true, "use_n_input": true, "forcecpu": false, "aovs": ["C"]}]]'` (default `'[]'`) |
| `denoiser = oidn` | the same with `"engine": "oidn"` |
| + `denoise_separate_aovs`, `denoise_cpu_only`, `denoise_aovs = "C diffuse"` | the list names `["C_denoise", "diffuse_denoise"]`, `forcecpu: true`; a new RenderVar `Vars/beauty_denoise` (`husk:name = "C_denoise"`, the same LPE `C.*[LO]`, `color4f` / `color4h`) is appended to `orderedVars`. A `diffuse` var did not exist in the probe, so it was reported missing |
| a filter prim in `cop_filter_1` | `rel husk:orderedImageFilters = [</Render/ImageFilter/...>]` |
| the above with `cop_createaovs = 1` | three extra vars appended to `orderedVars`, see below |

The three auto-created vars, all with `HuskRenderVarAPI`, `husk:bypassSave = 1`, `multiSampled = 0`, `clearValue = 0`:

| Prim | `husk:name` | Source | `dataType` / `husk:format` |
|---|---|---|---|
| `_internal_cop_filterlist_depth` | `depth` | raw `ray:hitPz` | — |
| `_internal_cop_filterlist_albedo` | `albedo` | lpe `CDA` | `color3f` / `color3f` |
| `_internal_cop_filterlist_N` | `N` | raw `ray:hitN` | `normal3f` / `half3` |

The help names the JSON keys differently from what the LOP authors (help `solaris/support/filters`: `albedo`, `use_N_input`, default aovs `["Cf"]`). The authored keys are `use_albedo_input`, `use_n_input`, `forcecpu` and `aovs`; those are the ones that run.

**Image Filter LOP** (`imagefilter`, new in 22.0) authors `/Render/ImageFilter/<node>` as `HoudiniImageFilterList`:
- `uniform token AOVs` ∈ {`color` (default: "All Color AOVs"), `all`, `none`, `list`};
- `uniform string[] AOVlist` (default `["C", "color"]`);
- `uniform string albedoscope = "albedo"`, `normalscope = "N"`, `depthscope = "depth"`;
- `rel orderedFilters`.

A Denoise AI filter in its internal `copnet_filterlist` becomes a child `HoudiniImageFilter`:
- `uniform string type = "denoiseai"`, `uniform bool bypass = 0`;
- `custom string filterparm:denoiser = "optix"`, `filterparm:auxareclean`, `filterparm:prefilteraux`, `filterparm:onlycpu`.

The Denoise AI COP has the inputs `source`, `normal`, `albedo`, `motionvec`, `prevframe` (probe). The filter-list prim has no scope for motion vectors or a previous frame (probe), so the husk filter-list path cannot run OptiX's temporal mode.

### 1.3 Planes read and written

| Path | Denoised planes | Albedo guide | Normal guide | Output |
|---|---|---|---|---|
| Karma built-in | `aovs` of the JSON (LOP default `["C"]`) | the plane named `albedo` (`use_albedo_input`) | `N`, `Ng` or `normal` (`use_n_input`; help) | in place; with "Create Separate Denoised AOVs" into `<name>_denoise` vars |
| husk filter list | `AOVs`/`AOVlist`: `color` = every var whose husk format is a colour type (husk passes them as `--caovs`) | the var named by `albedoscope` | the var named by `normalscope` | in place, before husk writes the files |

Measured with Nova (scene 1, `probes/denoise_scene.usda`):
- `list` with `["C"]` changes only `C`: the `albedo` and `N` parts stayed bit-identical.
- `color` also denoises the **albedo guide plane** (log: two denoiser configurations; the albedo part differs from the undenoised render).
- A product without the guide vars still runs the filter, now without guides (log: ABuff/NBuff 0).

### 1.4 Test renders: Nova and Karma through husk

**Scenes** (all in `probes/`, sublayering the reference scenes; the guide vars use Karma's sources):
- `denoise_scene.usda` (scene 1): the Cornell reference box at 512², 16 spp, guides `albedo` (lpe `CDA`, `color3f`) and `N` (raw `ray:hitN`, `normal3f`). Filter lists for OptiX `list`, OptiX `color` and OIDN; a settings prim with only Karma's built-in filter; a LOP-style variant whose guide vars carry `bypassSave`; a 4096 spp truth.
- `denoise_scene2.usda` (scene 2): the eight `mtl_spheres` materials, a ground plane, a sphere light and the soft sky, 960x540, 16 spp. The camera is rotated (-20.2°, 30.8°), so camera space ≠ world space. Truth: 2048 spp.
- `denoise_scene3.usda` (scene 3): the `mtl_textured` reference (checker ground, UDIM card, roughness-mapped metal ball), 960x480 at **4 spp**. Truth: 2048 spp.

**Command:**

```
husk --renderer Nova --headlight none -V 4 -s /Render/<settings> -o <out>.exr denoise_scene.usda
```

Karma ran the same with `--renderer BRAY_HdKarma -p 16 --lock-random 1`.

**Metric** (`probes/dn_analyze.py`): relMSE = mean((x − ref)² / (ref² + 0.01)) over RGB.
- "detail" = pixels next to an albedo or normal edge of the truth (dilated by 1 px);
- "flat" = the rest;
- "specular" = geometry whose diffuse albedo is < 0.02 (metals, glass).

Scene 1 (Cornell, 16 spp; relMSE against the Nova truth):

| Render | relMSE | detail | rmse |
|---|---|---|---|
| Nova, no filter | 8.98e-3 | 1.96e-2 | 0.0197 |
| Nova + husk OptiX filter (`list C`, guides `albedo` + `N`) | **1.82e-4** | 2.24e-3 | 0.0093 |
| Nova + husk OptiX filter, LOP-style `bypassSave` guides | 1.81e-4 (output has only part `C`) | 2.24e-3 | 0.0093 |
| Nova + husk OptiX filter, product without guides | 1.75e-4 | 2.15e-3 | 0.0088 |
| Nova + husk OIDN filter | 1.83e-4 | 1.94e-3 | 0.0194 |
| Nova + `karma:global:imagefilter` (Karma's built-in) | 8.98e-3: **bit-identical to "no filter"** | | |
| Karma CPU, no filter | 1.33e-2 | 2.50e-2 | 0.0217 |
| Karma + husk OptiX filter | 1.62e-4 | 1.69e-3 | 0.0082 |
| Karma + built-in `karma:global:imagefilter` | identical to the husk filter (mean \|Δ\| 1e-12) | | |

Karma is compared against Nova's truth, so its numbers include the small model differences between the renderers. The denoiser itself is the same: Karma's built-in filter and the husk filter list give the same pixels.

The answer to question 1: **the husk image filters work for a third-party delegate** as long as the delegate renders the vars the filter list names. Nova already renders Karma's `CDA`/`ray:hitN` and Hydra's `albedo`/`normal`, so a Solaris setup with the Karma Render Settings LOP and an Image Filter LOP denoises Nova's husk renders today. Karma's built-in filter does not: it lives inside Karma.

Cost in husk (measured, scene 2 at 960x540):
- one frame: 2.6 s → 3.0 s wall, which includes building the COP network through hou and initializing CUDA;
- 10 frames: 13.05-13.34 s → 14.01-14.08 s, i.e. about 0.07-0.1 s per frame after the first.
- At 3840x2160: 18.6 s → 19.2 s.

### 1.5 What Houdini's OptiX filter does internally, and what that costs

From the debug log (`HOUDINI_DEBUG_OPTIX_DENOISE=4`, measured):
- configuration `blendfactor 0`, `tilesize 2048`, `tileoverlap 8`, `denoisealpha 1`; "AOV Mode";
- OptiX's own log: fp16 buffers, `xmma-jit convolution`, and **two** networks per frame: one with 9 input channels (RGB + albedo + normal), one with 3 for alpha;
- memory at 512²: scratch 43.7 MB, state 22.5 MB;
- at 3840x2160 it created its layers at 2064x2064 (2048 + 2 × 8 overlap), with 721 MB scratch.

The prototype (section 3) reproduces its output when fed the same configuration:
- the vertically flipped image (bottom-up rows), alpha denoising on, and for 4K tiles of 2048 with 8 px overlap;
- mean \|Δ\| 1.7e-9 against Houdini's output on scene 3;
- the same relMSE to six digits on scenes 1 and 2.

That gives four measurable consequences:

| Behaviour | Measured effect |
|---|---|
| alpha always denoised | scene 2 has alpha = 1 everywhere; after the filter its minimum is 0.99774. OptiX's alpha network doubles the invoke time: 3.52 ms instead of 1.86 ms at 960x540, 40.7 ms instead of 22.6 ms at 4K |
| bottom-up rows (Houdini/OpenGL order) | relMSE against top-down: scene 3 with the total-albedo guide 0.0160 vs 0.0140 (+14 %), with `CDA` 0.01864 vs 0.01824 (+2 %); scenes 1 and 2 ±1 % |
| tiles of 2048 with 8 px overlap above 2048 px | at 3840x2160 compared with a whole-frame invoke: the step between columns 2055 and 2056 is 2.65x the untiled step; columns 2047-2050 deviate 0.44 % (18x the median column); the last column deviates 4.6 %. With OptiX's reported overlap (128 px) the tiled result equals the whole frame within 0.02 % and has no step (ratio 0.999). On the flat ground of this scene the seam was not visible in a crop |
| "All Color AOVs" | also denoises the `albedo` guide plane (1.3) |

---

## 2. Viewport (IPR) denoising in Solaris

### 2.1 No generic denoiser for third-party delegates

- **Help, `solaris/support/image_filters`.** Image filters preview in the viewport only when the viewport renders with Karma CPU or XPU, and for third-party delegates they work only in husk. The Image Filter LOP page (`nodes/lop/imagefilter`) says most filters also work in viewport rendering; that refers to Karma.
- **Help, `ref/windows/displayopts_3d`, `basics/viewporteffects`, HOM `GeometryViewportSettings.setUseDenoising`.** "Denoise if Needed" belongs to Houdini's own Vulkan viewport: ray-traced lighting, AO, environment lights. `GR_CommonDispOption` marks it "Vulkan only" (`$I/GR/GR_CommonDispOption.h:955`).
- **Help, `ref/views/ipr`, `render/optixdenoiser`.** The Render View's denoise button is the Mantra-era IPR viewer. It uses a `basecolor` plane as albedo.
- **`$HFS/houdini/UsdRenderers.json`** has no denoise key for any delegate (Karma's entries list restart settings, stats and metadata only).

### 2.2 How Karma does it (files and help; not observed in a GUI)

- **`$HFS/houdini/soho/parameters/BRAY_HdKarma_Viewport.ds` and `BRAY_HdKarmaXPU_Viewport.ds`** declare:
  - the parameter `xn__karmahydradenoise_q3af`, i.e. the setting `karma:hydra:denoise`: label "Enable Denoising", toggle, default 0, `uiscope "viewport toolbar"`, `uiicon VIEW_display_denoise`, help "Use the NVIDIA OptiX denoiser";
  - `karma:global:ipr_proxy_for_imagefilter` ("Denoise Bucket Size", default 2, range 0-6, `uiscope "viewport"`);
  - `karma:global:imagefilter` with a menu of denoise/OCIO/UV-enlarger JSON presets.
- **The Karma IPR settings** (help `nodes/lop/rendersettings`; `husk --properties` measured, defaults `ipr_proxy_for_imagefilter 2`, `ipr_proxydepth 2`, `ipr_reserve_threads 1`):
  - IPR starts with downsampled passes;
  - `ipr_proxy_for_imagefilter` is the number of those passes rendered before the denoise filter is first applied.
- **The generic Render Settings LOP** has `karma:global:imagefilter` with the same presets and `husk:orderedImageFilters` (probe `probe_ipr_parms.py`).
- **Karma XPU's help** lists "Enable Denoising" among the supported render settings (`solaris/karma_xpu`).

Karma's viewport denoise is therefore delegate-internal:
- a viewport-toolbar render setting declared in Karma's `*_Viewport.ds`;
- Karma's own image filter (`BRAY_Denoise` → `TIL_DenoiseOptix7`);
- a "first N passes undenoised" rule.

### 2.3 The hook Nova can use

- **The toolbar toggle.** The mechanism is documented in docs/research/m4/render-settings-ui.md 2.6:
  - `<RendererPluginName>_Viewport.ds` parameters with `uiscope "viewport"` appear in the viewport's Display Options; with `toolbar` they appear on the viewer's side toolbar, with `uiicon`;
  - their values reach the delegate as render settings.
  - Nova already generates `soho/parameters/HdNovaRendererPlugin_Viewport.ds` from `houdini/tools/nova_settings.json` (stage, measured). A toggle with `uiscope "viewport toolbar"` and `uiicon VIEW_display_denoise` gives Nova the same button as Karma.
- **Telling the viewport from husk.** husk sends `houdini:renderer = "husk"` (measured in husk's settings dump). Houdini sends `houdini:interactive` ∈ {`normal`, `flipbooking`, `playing`, `scrubbing`, `editing`, `tumbling`, `husk:mplay`} and `houdini:viewport` (m4 research, HDK page).

---

## 3. The OptiX 9.1 denoiser API

### 3.1 Initialization without an SDK install

- **The function table** (header, `optix_function_table.h:20`, `optix_stubs.h:72-78`, `optix_function_table_definition.h`).
  - The `optix*` calls are stubs over a function table, `g_optixFunctionTable_118`; the ABI version 118 is part of the symbol name.
  - Exactly one translation unit defines the table by including `optix_function_table_definition.h`.
  - `OPTIX_ENABLE_SDK_MIXING` makes the stubs `static`, so several OptiX versions can live in one binary (`optix_stubs.h:54-66`).
  - On Windows DLL symbols are not shared, so hdNova's table cannot collide with Houdini's plug-in (measured: both ran in one process, 4.2).
- **Finding `nvoptix.dll`** (header, `optix_stubs.h:88-202`, `218-258`).
  - `optixInit()` loads `nvoptix.dll` from the system directory.
  - Failing that, it enumerates display adapters (`CM_Get_Device_ID_List`, class GUID `{4d36e968-...}`), reads each adapter's registry value `OpenGLDriverName` and loads `nvoptix.dll` from the same directory.
  - It then calls `optixQueryFunctionTable(OPTIX_ABI_VERSION, ...)`.
  - `optix_stubs.h` links `Cfgmgr32.lib` by pragma. The registry calls also need **`advapi32.lib`**, which the prototype had to add.
- **On this machine** (measured): the loaded file was `C:\WINDOWS\System32\DriverStore\FileRepository\nv_dispsi.inf_amd64_…\nvoptix.dll` (reached through the registry path), and OptiX logged runtime `rt v13040`.
- **Errors.** A missing driver gives `OPTIX_ERROR_LIBRARY_NOT_FOUND`; a driver that lacks ABI 118 gives `OPTIX_ERROR_UNSUPPORTED_ABI_VERSION` (header `optix_types.h` result codes). Houdini's plug-in prints "maybe old driver?" in that case (strings).
- **Requirements.** No SDK is needed, only the BSD-licensed headers already fetched by `cmake/NovaOptiX.cmake`.
- **The CUDA header** (header, `optix_host.h:40`, `optix_function_table.h:29`). `optix_host.h` and `optix_function_table.h` include `<cuda.h>`, so the CUDA toolkit's headers are needed at compile time even when nothing is linked.

### 3.2 The CUDA driver API

The prototype resolves these entry points by name from `nvcuda.dll`. The `cuda.h` macros map the unversioned names to `_v2` exports; the prototype stringizes the macro expansion.
- `cuInit`, `cuDriverGetVersion`;
- `cuDeviceGetCount`, `cuDeviceGet`, `cuDeviceGetName`, `cuDeviceGetAttribute`;
- `cuDevicePrimaryCtxRetain`, `cuDevicePrimaryCtxRelease_v2`, `cuDevicePrimaryCtxGetState`;
- `cuCtxPushCurrent_v2`, `cuCtxPopCurrent_v2`;
- `cuStreamCreate`, `cuStreamDestroy_v2`, `cuStreamSynchronize`;
- `cuMemAlloc_v2`, `cuMemFree_v2`, `cuMemAllocHost_v2`, `cuMemFreeHost`, `cuMemcpyHtoDAsync_v2`, `cuMemcpyDtoHAsync_v2`, `cuMemsetD8Async`, `cuMemGetInfo_v2`;
- `cuEventCreate`, `cuEventRecord`, `cuEventSynchronize`, `cuEventElapsedTime`, `cuEventDestroy_v2`;
- `cuGetErrorName`.

Ways to bind them:

| Option | Behaviour | Trade-off |
|---|---|---|
| link `cuda.lib` (what `Nova::OptiX` does today: `CUDA::cuda_driver`) | `nvcuda.dll` becomes a load-time import of `hdNova.dll` | on a machine without an NVIDIA driver `hdNova.dll` fails to load, so Nova disappears from Houdini even for CPU rendering (Windows loader behaviour, not measured here). Simplest code. Houdini's `TIL_DenoiseOptix7.dll` links this way, but it is an optional plug-in DSO |
| `/DELAYLOAD:nvcuda.dll` | loads at the first call | a missing DLL surfaces as an SEH exception at the first call unless probed first (`__HrLoadAllImportsForDll`); versioned names keep working |
| **`LoadLibrary` + `GetProcAddress`** (prototype) | Nova decides at run time | about 30 pointers. `LoadLibrary("nvcuda.dll")` took 0.5-1 ms, once 41 ms (measured). The versioned export names must be chosen explicitly; `cuGetProcAddress` is the driver's own versioned resolver. OIDN's CUDA device and the CUDA runtime load the driver the same way at run time |

Recommendation: dynamic loading, with CMake using only the CUDA include directory.

### 3.3 Models (header, `optix_types.h:1823-1845`, `optix_host.h:1092-1168`)

| Model kind | Use | Notes |
|---|---|---|
| `OPTIX_DENOISER_MODEL_KIND_AOV` | single image | beauty in `layers[0]`; further layers are AOVs denoised with the same predicted kernels ("kernel prediction", header 1123-1129), so the AOVs stay consistent with the beauty. Each layer has a type (`BEAUTY`, `SPECULAR`, `REFLECTION`, `REFRACTION`, `DIFFUSE`) |
| `TEMPORAL_AOV` | sequences | needs a flow guide (2D, pixel motion from the previous to the current frame), `previousOutput` per layer, and two internal guide layers in `OPTIX_PIXEL_FORMAT_INTERNAL_GUIDE_LAYER` (16 bytes per pixel measured; previous contents zero on the first frame), with `temporalModeUsePreviousLayers` = 0 on the first frame. Optional `flowTrustworthiness` (0..1) and `flowMulX/Y` |
| `UPSCALE2X`, `TEMPORAL_UPSCALE2X` | denoise and upscale 2x | output layers 2W x 2H; supports AOVs |
| `HDR`, `LDR`, `TEMPORAL` | deprecated | HDR/LDR are mapped to AOV internally (header). Measured: HDR cost equals AOV (1.87 vs 1.85 ms) |

### 3.4 Guide layers

- **Enabling them.** `OptixDenoiserOptions::guideAlbedo/guideNormal` must be set at `optixDenoiserCreate`, and the images are then required (header 1862-1872, 1105-1109).
- **Albedo.** 3 components RGB (header 1879-1880).
- **Normal: the header contradicts itself.**
  - The struct comment says (X, Y) camera space for the legacy HDR/LDR models and **(X, Y, Z) world space for all other models** (1882-1885).
  - The `optixDenoiserInvoke` comment says 3D **camera** space for the temporal models and only X, Y used otherwise (1141-1143).
  - (web) NVIDIA forum thread 262875 quotes the OptiX 8.0 release notes: for the AOV model, camera-space normals still work but world-space normals give better results. An NVIDIA engineer adds that only LDR/HDR need camera space and that world space is better on R535+ drivers. Camera-space components are in [-1, 1].
- **Measured, scene 2, rotated camera, AOV model** (relMSE / detail):
  - world 3.51e-4 / 1.76e-3;
  - camera (Houdini's +Z-forward `worldToCamera`) 3.53e-4 / 1.75e-3;
  - camera with Z flipped (USD's -Z-forward) 3.52e-4 / 1.77e-3.
  - The space does not matter measurably. World space is what Nova's film already stores (`Film::normal`, world-space shading normal).
- **Which guides help** (measured, prototype on Nova's noisy renders, AOV model, top-down):

| Guides | Scene 1 relMSE | Scene 2 relMSE / detail | Scene 3 (textured, 4 spp) relMSE / detail / flat |
|---|---|---|---|
| none | 1.82e-4 | 3.59e-4 / 1.57e-3 | 0.0213 / 0.0476 / 2.3e-4 |
| `CDA` albedo (diffuse only, filtered) | 1.89e-4 | 3.58e-4 / 1.71e-3 | 0.0184 / 0.0409 / 4.3e-4 |
| `CDA` + `ray:hitN` | 1.84e-4 | 3.51e-4 / 1.76e-3 | 0.0182 / 0.0403 / 5.3e-4 |
| `CDA` + Karma's pixel-filtered N | — | 3.52e-4 / 1.80e-3 | — |
| Hydra `albedo` (total first-hit albedo) | — | 3.70e-4 / 1.93e-3 | **0.0141** / 0.0311 / 4.7e-4 |
| Hydra `albedo` + `ray:hitN` | — | 3.65e-4 / 1.97e-3 | **0.0140** / 0.0309 / 4.0e-4 |
| (noisy input) | 8.98e-3 | 1.92e-2 / 3.20e-2 | 0.0785 / 0.1264 / 0.0401 |

  - Guides decide texture detail. On scene 3 the total albedo cut relMSE by 34 % against no guides and by 23 % against `CDA`.
  - On untextured scenes they hardly change relMSE (±5 %), and pixels next to edges were slightly better without them.
  - The flat regions of scene 3 are better without guides (2.3e-4 against 4.0e-4), but they contribute little to the total.
  - Crops of scene 3: without guides the checker texture is blurred; with guides it is kept.
- **Anti-aliasing of the guides** (measured, `probes/guide_aliasing.py`, share of edge pixels holding a blend of their neighbours):

  | Plane | Nova | Karma |
  |---|---|---|
  | `CDA` | 0.86 (LPE, accumulated per sample) | 0.87 |
  | `ray:hitN` | 0.04 (one ray through the pixel centre, `Film` first-hit AOVs, `core/integrator/wavefront.cpp:417-516`) | `N` filtered with `ubox`: 0.86 |
  | Hydra `albedo` (`Film::albedo`) | 0.33 (scene 2) / 0.45 (scene 3). Same pixel-centre ray, so point-sampled at silhouettes by construction; the share is inflated by albedo that varies smoothly within surfaces (Fresnel, filtered textures) | — |

  Replacing Nova's point-sampled N with Karma's filtered N changed nothing measurable (3.51e-4 vs 3.52e-4).
- **Denoised means** are 0.1-0.5 % darker than the truth on scenes 2 and 3 (measured `mean_rel_diff`). The denoiser is close to, but not exactly, energy-preserving.

### 3.5 Pixel formats (header, `optix_types.h:1780-1818`)

- **Formats.** `HALF1-4`, `FLOAT1-4`, `UCHAR3/4`, and `INTERNAL_GUIDE_LAYER` for the temporal internal layers.
- **Strides.** `OptixImage2D` has unsigned row and pixel strides, so a bottom-up film cannot be described with a negative stride: flip rows while copying.
- **Precision.** OptiX logs "buffers: fp16" internally (measured). Half input gave the same relMSE as float (3.51e-4 on scene 2) and the same invoke time. It halves the upload: 2.88 vs 5.79 ms for the 4K beauty + guides.

### 3.6 hdrIntensity and hdrAverageColor

Header (`optix_host.h:1170-1219`, `optix_types.h:1944-1961`):
- `optixDenoiserComputeIntensity` returns 0.18 / exp(mean of log luminance) over pixels with luminance > 1e-8 (Rec. 709 weights).
- `optixDenoiserComputeAverageColor` returns the per-channel log average, used by the AOV model.
- Passing null pointers lets the invoke compute them automatically per invocation. With tiling, they must be computed once for the whole image so that all tiles get the same exposure.

Measured:
- computing both costs 0.04 ms at 960x540 and 0.35-0.43 ms at 4K;
- the result with null pointers was bit-identical (scene 2) for a whole-frame invoke.

So compute them explicitly only when tiling.

### 3.7 Alpha (header, `optix_types.h:1850-1857`)

`OPTIX_DENOISER_ALPHA_MODE_COPY` copies alpha from the input; `DENOISE` denoises it. The beauty is passed as a 4-channel layer either way.

Measured:
- `DENOISE` doubles the invoke time (3.52 vs 1.86 ms at 960x540; 40.7 vs 22.6 ms at 4K) and adds 11 MB state;
- on an all-opaque frame it changed alpha (min 0.99774).

Use `COPY`. Alpha noise (motion blur, DOF, fractional opacity) is better reduced by sampling. `DENOISE` could be an option.

### 3.8 Tiling and overlap

Header (`optix_host.h:1043-1103`, `optix_denoiser_tiling.h:120-197`, `199-`):
- size the memory for the tile with `optixDenoiserComputeMemoryResources(tileW, tileH)`;
- `optixDenoiserSetup` with tile + 2 × `overlapWindowSizeInPixels`;
- invoke each tile with the input offsets, zero where the tile touches the image border;
- `optixUtilDenoiserInvokeTiled` does this sequentially, reusing one scratch buffer.

Measured:
- `overlapWindowSizeInPixels` = **128** for every configuration;
- 4K whole frame: invoke 22.6 ms, 1392 MB scratch, VRAM +1854 MB;
- tiles of 1024: 40.2 ms, VRAM +734 MB;
- tiles of 512: 68.8 ms, VRAM +560 MB;
- with the reported overlap the tiled image equals the whole frame within 0.02 %; with 8 px it seams (1.5).

### 3.9 Timing and memory on the RTX 5080

Prototype `prototypes/optix_denoise_spike.cpp`, `--bench`, 30 frames per case; full output in `probes/results/spike_bench_rtx5080.txt`.
- Images are scene 2 resampled.
- "frame" = upload of beauty + guides + intensity + invoke + download of the beauty, wall-clock with stream synchronization; median after the first frame.
- "GPU" = invoke by CUDA events.
- VRAM = drop of `cuMemGetInfo` free memory for the denoiser and all its device buffers.

| Case | Resolution | Invoke (GPU) ms | Frame ms | First frame ms | State / scratch MB | VRAM MB |
|---|---|---|---|---|---|---|
| AOV, albedo + normal, float4, pinned | 240x135 | 0.63 | 0.81 | 1.45 | 11.2 / 11.2 | +24 |
|  | 480x270 | 0.88 | 1.12 | 2.16 | 11.2 / 23.0 | +42 |
|  | 960x540 | 1.85 | 2.45 | 6.05 (first in process); later denoisers 2.5-9.6, typically 3-5 | 11.2 / 87.0 | +128 |
|  | 1920x1080 | 5.65 | 7.97 | 7.84 | 11.2 / 348 | +474 |
|  | 3840x2160 | 22.55 | 31.1 | 31.0 | 11.2 / 1392 | +1854 |
| same, half4/half3 | 960x540 | 1.85 | 2.19 | | | +116 |
|  | 3840x2160 | 22.63 | 27.0 (upload 2.9) | | | +1630 |
| float, pageable host memory | 960x540 | 1.87 | 3.06 | | | |
|  | 3840x2160 | 22.20 | 46.3 (upload 16.1, download 6.3) | | | |
| no guides | 960x540 | 1.69 | 2.05 | | 11.2 / 69.1 | +98 |
| albedo only | 960x540 | 1.80 | 2.42 | | 11.2 / 75.0 | +110 |
| alpha DENOISE | 960x540 | 3.52 | 4.13 | | 22.5 / 87.0 | +140 |
| intensity/average null (auto) | 960x540 | 1.95 | 2.58 | | | |
| beauty + 1 / 3 / 7 AOV layers | 960x540 | 1.98 / 2.18 / 2.62 | 2.73 / 3.21 / 4.26 | | | +144 / +176 / +240 |
| beauty + 3 AOV layers | 3840x2160 | 26.69 | 42.6 | | | +2622 |
| tiled 1024 / 512 (overlap 128) | 3840x2160 | 40.17 / 68.84 | 49.2 / 78.3 | | 11.2 / 175 / 44 | +734 / +560 |
| TEMPORAL_AOV (zero flow) | 960x540 | 2.73 | 3.46 | 12.1 (create 16.9) | 11.4 / 132 | +192 |
|  | 3840x2160 | 32.30 | 41.1 | | 11.4 / 2109 | +2890 |
| UPSCALE2X (input → 2x output) | 960x540 → 1920x1080 | 5.12 | 6.26 | | 11.2 / 276 | +342 |
|  | 1920x1080 → 3840x2160 | 19.43 | 23.8 | | 11.2 / 1105 | +1326 |

Denoiser lifetime per configuration: `optixDenoiserCreate` 5.4-7.6 ms (one outlier 16 ms; 16.9 ms temporal), `ComputeMemoryResources` < 0.2 ms, `optixDenoiserSetup` 2.4-6.4 ms (up to 20 ms at 4K).

Process start, cold standalone process, from 6 runs:

| Step | ms |
|---|---|
| `LoadLibrary("nvcuda.dll")` | 0.4-1.0 (once 41) |
| `cuInit` | 13-26 |
| primary context retain + first use | 52-73 |
| `optixInit` | 1.5-12 |
| `optixDeviceContextCreate` | 65-100 |
| process start → OptiX ready | 160-280 |

- **Inside Houdini (hython, 4.2):** `cuInit` 0, retain 0.02 ms, `optixDeviceContextCreate` 27-51 ms.
- **OptiX's disk cache** (`%LOCALAPPDATA%\NVIDIA\OptixCache`) is not used by the denoiser. With an empty cache directory (`OPTIX_CACHE_PATH`) the cache stayed at 0 bytes, and with the cache disabled (`OPTIX_CACHE_MAXSIZE=0`) the times were unchanged.

---

## 4. CUDA and OptiX inside the Houdini process

### 4.1 Who uses CUDA in Houdini 22.0.466 (imports)

| Module | CUDA context |
|---|---|
| `houdini/dso/img_filter/TIL_DenoiseOptix7.dll` | creates its own (`cuCtxCreate_v2`), push/pop around use (strings `CudaContextScope`) |
| `houdini/dso/gputrace/GT_GPUTraceOptix.dll` (viewport GPU trace; debug level `HOUDINI_DEBUG_OPTIX_GTGPUTRACE`) | creates its own (`cuCtxCreate_v2`), push/pop |
| `bin/OpenImageDenoise_device_cuda.dll` (OIDN on GPU) | **primary context** (`cuDevicePrimaryCtxRetain`/`Release_v2`), push/pop |
| Karma XPU | not resolved here (no static `nvcuda` import found in `BRAY_AccelGpu.dll`). The help lists its device controls (`KARMA_XPU_DEVICES`, `KARMA_XPU_DISABLE_OPTIX_DEVICE`) and its participation in Houdini's GPU-memory coordination (`KARMA_XPU_OPTIX_ENABLE_MEMORY_RESOURCE`, help `ref/env`) |

Device selection for Houdini's OptiX denoiser: `HOUDINI_NVIDIA_OPTIX_DEVICENUMBER` (help `ref/env`, `render/optixdenoiser`; `hgpuinfo -o` lists the devices).

### 4.2 Coexistence test (measured, `probes/inproc_probe.py`)

Isolated hython. The prototype was built as a DLL (`build_spike.bat <dir> dll`) and called through ctypes.

1. A COP2 `aidenoise` with `denoiser = optix` cooked first (263 ms). That loaded `nvcuda.dll`, `nvoptix.dll`, `TIL_DenoiseOptix7.dll` and `OpenImageDenoise.dll` into the process.
2. The prototype in the same process. The primary context was **already active** (`cuDevicePrimaryCtxGetState`: active 1), so it was in use by another module. Retain 0.02 ms, `optixDeviceContextCreate` 27.8 ms. Denoise 960x540 in 2.44 ms per frame, the same as standalone. Free VRAM after context: 14757 MB, against 14985 MB in a standalone process, i.e. about 230 MB held by Houdini's contexts.
3. Houdini's `aidenoise` cooked again and still worked (72 ms). The prototype ran a second time (OptiX context 27 ms).

### 4.3 Rules for Nova (from the measurements and the CUDA/OptiX API contracts)

- **Context.**
  - Use the device's primary context (`cuDevicePrimaryCtxRetain` once, `Release` at shutdown). It shares with OIDN and with anything else in the process that uses the CUDA runtime, and it does not cost a context's memory a second time.
  - Never call `cuDevicePrimaryCtxReset`, `cuDevicePrimaryCtxSetFlags` or `cudaDeviceReset`: other modules hold the same context.
- **Current context per thread.**
  - Push the context before and pop it after every block of CUDA/OptiX calls (`cuCtxPushCurrent`/`cuCtxPopCurrent`), as Houdini's modules do.
  - Never leave it current on a thread Nova does not own (TBB workers, Houdini's main thread).
- **Threads.** One denoise thread owns the denoiser objects. An `OptixDenoiser` and its state/scratch buffers must not be used by two invocations at once; the header requires state memory to stay valid between setup and invoke, and scratch to be exclusive per invoke.
- **Streams.** Use own streams created with `CU_STREAM_NON_BLOCKING`, so that work does not serialize with the legacy default stream other modules may use. Synchronize only Nova's own stream or event, never the device.
- **Host memory.** Pinned staging buffers (`cuMemAllocHost`) halve the 4K frame time against pageable memory (31 vs 46 ms). Keep a few per resolution; do not pin the film itself. Karma XPU also notes OS trouble with too much pinned memory (`KARMA_XPU_OPTIX_DISABLE_HOST_PINNED`).
- **GPU memory.**
  - 4K needs about 1.9 GB.
  - Query `cuMemGetInfo` before setup and fall back to tiles (always with the reported overlap) or to the CPU-only "no denoise" path when it does not fit.
  - Houdini's `UT_MemoryResource` (`$I/UT/UT_MemoryResource.h`) lets clients register and ask others to free GPU memory; the OptiX plug-in uses it. Optional for Nova; it would be HDK-side (hydra), not in `core`.
- **Device choice.** Honour `HOUDINI_NVIDIA_OPTIX_DEVICENUMBER` (users already set it for Houdini's denoiser), plus a Nova setting. `CUDA_VISIBLE_DEVICES` applies anyway.
- **Errors.** A missing driver, an old driver (ABI 118) or an out-of-memory must give one warning and an undenoised image, never a failed render.
- **Driver ABI.** Building against the OptiX 9.1 headers requires a driver with ABI 118. NVIDIA's minimum driver for OptiX 9.1 was not checked. Older OptiX headers (8.x) would widen driver support, at the cost of newer API fields.

---

## 5. What Nova renders today that a denoiser can use (code read, measured)

- **Beauty.** `Film::colorSum`/`alphaSum`: per pixel, 1 spp per pass, filtered by filter importance sampling. Rows are **bottom-up** (`core/film/film.h:51-52`).
- **First-hit guides.**
  - `Film::albedo` (`ClosureList::albedo`: weight × albedo of all lobes at wo, VEX materials included) and `Film::normal` (world-space shading normal, face-forwarded) come from one extra ray through each pixel centre, traced with the first pass (`wavefront.cpp:417-516`).
  - They exist after pass 1, cost nothing later, and are exposed as Hydra `albedo`/`normal`.
- **LPE albedo** `CDA`: accumulated per sample, diffuse only.
- **Copies into the render buffers.** The render loop (`hydra/renderDelegate.cpp:266-338`) copies the beauty into its render buffer after every pass under the framebuffer lock. The other AOVs are copied after pass 1, at convergence, or when mapped.
- **Seeds.** For a static scene Nova renders **the same noise on every frame**: frames 1 and 10 of a 10-frame husk render were bit-identical (measured). Temporal flicker of the noise is therefore absent today. A temporal denoiser would see the same input twice, which is no help. This is relevant to the temporal-denoising and adaptive-sampling decisions.

---

## 6. Recommendation

### 6.1 One native denoiser module

**Placement.**
- `core/denoise/`: HDK-free, as PLAN.md's architecture already lists.
- It depends only on `cuda.h` types and the OptiX headers. `nvcuda.dll` is loaded at run time and `nvoptix.dll` through `optixInit`.
- CMake: `Nova::OptiX` keeps the CUDA include directory but stops linking `CUDA::cuda_driver`.
- When `NOVA_HAS_OPTIX` is off, or at run time when the driver is missing, the module reports "unavailable" and nothing else changes.

**Interface** (a sketch):
- a `Denoiser` that owns the OptiX device context, a non-blocking stream and per-resolution state;
- `denoise(const Inputs&, Outputs&)`: inputs are the beauty RGBA (float or half), an optional albedo and normal, and optional AOV layers; host or device pointers.
- Host pointers are staged through pinned buffers.
- In M10 the GPU film passes device pointers and skips the copies.
- The same module serves the delegate (viewport and husk) and `nova_cli`.

**Defaults** (all measured above):
- AOV model;
- guides albedo + normal;
- alpha COPY;
- half buffers, which halve uploads with no loss;
- rows flipped to top-down during staging;
- intensity and average colour null (automatic) for whole frames, computed explicitly when tiling;
- a whole-frame invoke when free VRAM allows, else tiles with `overlapWindowSizeInPixels` (never smaller);
- `blendFactor` as a user setting (Houdini exposes it as `blendfactor`).

**Guides.**
- Albedo = the **total** first-hit albedo (Nova's `Film::albedo`), not Karma's diffuse-only `CDA`. It was the best guide on the textured scene and is free. Normal = world-space `Film::normal`.
- Both are point-sampled today. The measurements found no loss from that (filtered N made no difference), so keep them.
- For final frames, an M7 test should decide whether per-sample accumulation of the two guides (6 floats per sample, only when denoising) improves edges on a scene with fine geometric detail. Neither test scene here had such detail.

### 6.2 (a) Final frames

- **Primary path: denoise inside the delegate** when the render settings ask for it, and write the result through husk's normal render buffers. This path is independent of the scene's AOVs and avoids the measured problems of Houdini's filter (alpha, orientation, 8 px tile overlap, guide plane denoised). It also works for `nova_cli`.
  - Nova setting `nova:denoise` (off/optix), `nova:denoiseAovs` (default `C`), output mode: replace in place, or Karma-style separate `<name>_denoise` AOVs.
  - Run after convergence, before the final copy.
  - Beauty plus the listed LPE AOVs go into one invoke as AOV layers, so the components denoise with the beauty's kernels. Cost per extra layer at 960x540 is 0.1-0.15 ms GPU plus upload.
- **Honour Karma's built-in setting.** If `karma:global:imagefilter` contains a `denoise` entry with engine `optix`, map its `aovs` and the albedo/N flags onto Nova's denoiser. Then the Karma Render Settings LOP's Denoiser menu, and its "Create Separate Denoised AOVs" with `<name>_denoise` vars, work for Nova as everything else Karma-authored does (README "Current scope"). For `engine = oidn`, warn and use OptiX, or skip, per the user's no-OIDN decision.
- **Keep the husk filter-list path working.** It already does (1.4). Document that it is husk-only and has Houdini's fixed behaviour.
- **Temporal denoising** for sequences needs a frame-to-frame flow guide in pixels from the previous to the current frame (Nova's `ray:motionvector` is shutter-interval motion, a different quantity) and the previous denoised output. It only pays off once noise differs per frame (5). Defer it until the seed varies with the frame and a flicker test shows a need. 4K temporal costs 32 ms GPU and 2.9 GB VRAM (measured).

### 6.3 (b) Viewport IPR

- **Switch.** A viewport toolbar toggle in `HdNovaRendererPlugin_Viewport.ds` (via `nova_settings.json`): `uiscope "viewport toolbar"`, `uiicon VIEW_display_denoise`, default off, mirroring `karma:hydra:denoise`. Plus a "start after N passes" setting mirroring Karma's `ipr_proxy_for_imagefilter`, default 2-4 passes of Nova's 1 spp.
- **Buffers.** The film keeps accumulating the noisy beauty. What changes is the copy step. When denoising is on and `houdini:renderer` is not `husk`, the beauty's render buffer receives the latest *denoised* frame instead of the resolved noisy one. Guides come from the film (available after pass 1). Other AOVs stay undenoised unless the viewer shows one that is in the denoise list.
- **Timing.**
  - Denoise asynchronously on the denoise thread.
  - After each pass the render thread resolves the beauty into a pinned half-float staging buffer (top-down) and signals; it never waits on the GPU.
  - The denoise thread uploads, invokes and downloads (2.2 ms at 960x540, about 8 ms at 1080p, 27-31 ms at 4K).
  - Under the framebuffer lock it publishes into the render buffer if no newer pass has finished; otherwise it drops the result.
  - The display then lags at most one pass. Against Nova's pass times (Cornell 960x540 is about 64 ms per spp, `docs/benchmarks.md` M4) the denoiser is about 4 % of a pass at the same resolution, so it can run every pass.
  - Rate-limit only at very high resolutions or very fast passes, for example at most one denoise per 50 ms.
  - Always denoise the converged final pass.
- **Interaction.** During `houdini:interactive = tumbling/scrubbing/playing` the first passes are too noisy for the denoiser to help (Karma's own default skips 2 passes). Either show the noisy passes until N passes exist, or use UPSCALE2X on a half-resolution render later (960x540 → 1080p costs 6.3 ms). That is an IPR-performance decision for M7.
- **Lifetime.**
  - Create the OptiX context lazily when the toggle is first enabled (0.16-0.28 s cold, 30-50 ms inside Houdini) and keep it for the session.
  - Recreate denoiser state only when the viewport resolution changes (create + setup about 10 ms). Allocate once per resolution.
  - On resolution changes during interactive resizing, skip denoising until the size is stable.

### 6.4 AOVs and guides Nova must provide

- Final and viewport guides: `Film::albedo` (total) and `Film::normal` (world space) internally. No user AOVs are needed.
- For the husk filter-list path, the vars the Karma LOP auto-creates (`CDA` → `albedo`, `ray:hitN` → `N`, `ray:hitPz` → `depth`). Nova renders these already, so no work.
- Possible additions for M7 tests: the guide accumulation option (6.1) and a frame-dependent seed (5).

### 6.5 Open points to settle during M7

- Whether per-sample guides beat pixel-centre guides on geometry-heavy scenes (hair in M9, thin geometry); add such a scene to the denoise tests.
- The minimum driver for ABI 118, and whether to build against older OptiX headers.
- Whether the AOV kernel-prediction layers should be the LPE components the user lists, or a fixed diffuse/specular split.
- Multi-GPU device choice: the default is device 0, or `HOUDINI_NVIDIA_OPTIX_DEVICENUMBER`.

---

## 7. Probe and prototype files

| File | What |
|---|---|
| `probes/probe_krs_denoise.py` → `results/krs_filter_parms.txt`, `results/krs_denoise_dump.txt` | Karma Render Settings LOP filter parameters and what each denoiser setting authors |
| `probes/probe_imagefilter.py` → `results/filterlist_info.txt`, `results/imagefilter_flat.usda` | Image Filter LOP with a Denoise AI (OptiX) filter; the Karma Render Settings LOP referencing it (auto-created guide vars) |
| `probes/probe_ipr_parms.py` | IPR/denoise parameters on the generic Render Settings and Karma Render Properties LOPs |
| `probes/denoise_scene.usda`, `denoise_scene2.usda`, `denoise_scene3.usda` | test scenes (section 1.4) with filter lists, guide vars and truth settings |
| `probes/dn_analyze.py` | relMSE / detail / flat / specular metrics, guide-plane identity |
| `probes/guide_aliasing.py` | are guide planes anti-aliased? |
| `probes/seam_check.py` | tile-seam measurement against a whole-frame denoise |
| `probes/crop_strip.py`, `probes/same_pixels.py` | visual crops; bit-identity check |
| `probes/dll_strings.py` | `strings` for the DLL scan (output kept in the scratch directory) |
| `probes/inproc_probe.py` | Houdini's OptiX filter and the prototype DLL in one hython process |
| `prototypes/optix_denoise_spike.cpp`, `prototypes/build_spike.bat` | the standalone OptiX denoiser: dynamic `nvcuda.dll`, `optixInit`, primary context, EXR in/out via Houdini's OIIO, `--bench` table; `dll` build for the in-process probe |
| `probes/results/spike_bench_rtx5080.txt` | the `--bench` output behind the table in 3.9 |

Re-run (PowerShell, repo root; `$S` = a scratch directory, `$P` = `docs\research\m7\probes`):

```
# husk renders (Nova; Karma with --renderer BRAY_HdKarma -p 16 --lock-random 1)
powershell -NoProfile -ExecutionPolicy Bypass -File docs\research\m6\probes\husk_env.ps1 -Tool husk.exe `
  -Stage C:\Users\Synti\Desktop\renderEngine\build\vs2022\stage\Release\nova -Log $S\h.log `
  -CmdLine "--renderer Nova --headlight none -s /Render/optix -o $S\nova_optix.exr <repo>\$P\denoise_scene.usda"
# metrics
& "$env:HFS\python313\python.exe" -I $P\dn_analyze.py --ref $S\nova_truth.exr $S\nova_noisy.exr $S\nova_optix.exr
# prototype
cmd /c docs\research\m7\prototypes\build_spike.bat $S\spike
$env:PATH = "$env:HFS\bin;" + $env:PATH
& $S\spike\optix_denoise_spike.exe --in $S\nova_s2_noisy.exr --bench
& $S\spike\optix_denoise_spike.exe --in $S\nova_s3_noisy_hd.exr --albedo albedo_hd --out $S\dn.exr
# hython probes
powershell -ExecutionPolicy Bypass -File tests\houdini\run_hython.ps1 -Script $P\probe_krs_denoise.py -Stage <stage> $S\krs
cmd /c docs\research\m7\prototypes\build_spike.bat $S\dll dll
powershell -ExecutionPolicy Bypass -File tests\houdini\run_hython.ps1 -Script $P\inproc_probe.py $S\dll\optix_denoise_spike.dll $S\nova_s2_noisy.exr $S\inproc optix
```

Environment variables used: `HOUDINI_DEBUG_OPTIX_DENOISE=4` (Houdini's filter log), `OPTIX_CACHE_PATH` / `OPTIX_CACHE_MAXSIZE` (cache test).

Web source: NVIDIA developer forum, "[OptiX 8.0] Denoiser: Camera space vs World space?", https://forums.developer.nvidia.com/t/optix-8-0-denoiser-camera-space-vs-world-space/262875
