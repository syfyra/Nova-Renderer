# M4 research: displacement (Houdini 22.0.466, Karma CPU, MaterialX 1.39.5)

Scope: user decision Q3 in docs/design/M3.md section 10 (displacement belongs to M4). The report covers how displacement reaches a
Hydra delegate, what Karma does (measured), and a minimal production design for Nova.

Sources are files of this repo, files of the Houdini install (`$HFS` = `C:\Program Files\Side Effects Software\Houdini 22.0.466`),
the HtoA 6.5.3.4 install (Arnold 7.5.3.3, queried with `kick -info`), a few web pages (linked), and experiments.
Every Karma number below was measured with my own `husk.exe --renderer BRAY_HdKarma -V 4` and `hython.exe` processes.
Each process had its own `HOUDINI_USER_PREF_DIR`, ran with `HOUDINI_PACKAGE_SKIPLIST=nova`, and had the package/HSITE/PXR variables removed.
The user's Houdini session was not touched.

Probe scenes, textures and analysis scripts are in the session scratchpad: `.../scratchpad/m4research/displacement/`.
- `gen*.py` writes `scenes/*.usda`.
- `gentex.py` writes `tex/`.
- `run.py` renders into `out/*.exr` and `*.log`.
- `ana*.py` reads the EXR parts (C, P, N) with OIIO.
- `probe_authoring.py` and `probe_authoring.txt` hold the hython authoring probe.

Probe conventions:
- The camera is orthographic and looks down −Z.
- The pixel filter is a box with scale 1e-6, so each pixel is a point sample.
- Surfaces are emissive `standard_surface`s with `emission_color = (s, t, 0)`. Each pixel therefore carries the surface parameter of the hit (C), the displaced world position (P) and the normal (N).
- Unless noted, the frame is 600 px across 3 units (200 px/unit), or 300 px across 1.5 units for the plane tests (also 200 px/unit).

---

## 0. Findings and decisions in short

1. **Authoring.** In USD a Houdini material carries `outputs:kma:displacement` (Karma Material Builder) or `outputs:mtlx:displacement` (USD MaterialX Builder). That output is connected to `ND_displacement_float` or `ND_displacement_vector3`; the vector version is the VOP's "Vector 3" signature.
   - UsdPreviewSurface uses `outputs:displacement` on the Material, connected to the preview shader's `displacement` output.
   - Hydra delivers all of these as the terminal `displacement` of `HdMaterialNetwork2`.
   - **Every Karma Material Builder wires a displacement node with value 0 by default.** Karma skips dicing for a constant 0 (measured), and Nova must do the same.
2. **Semantics, measured on Karma; all match the MaterialX doc strings.**
   - **Scalar:** `P += displacement · scale · N̂`.
   - **Vector:** `P += scale · (x·T̂ + y·B̂ + z·N̂)`.
     - T̂ is the normalized dP/ds of the `st` set.
     - B̂ is the part of dP/dt orthogonal to T̂, normalized, and it keeps dP/dt's side for mirrored uvs. This is the rule Nova's `KarmaB()` already uses.
   - **Space:** the offset is in **object space**, so the prim's transform scales it. A `scale 2` sphere with d = 0.1 renders at world radius 2.2.
3. **Karma dices polygon meshes as well as subdivision surfaces when they are displaced.** A single quad becomes 100×100 micropolygons.
   - The micropolygon edge is **1/dicingquality pixels**.
   - The default `karma:object:dicingquality` in 22.0 is **0.5**, which means 2-px micropolygons.
   - `dicingdepthmin` = `dicingdepthmax` = d gives exactly 2^d segments per face.
   - **Karma has no displacement-bound property.** It bounds the diced geometry itself.
4. **Shading normals after displacement are smooth and come from the diced surface.** Their error against the analytic normal falls as O(h²): 0.50° at 2 px, 0.14° at 1 px.
5. **Displacement textures** are looked up with a footprint of about half a micropolygon edge: MIP level = log2(texels per micropolygon edge) − 1.08, trilinear (measured with a probe texture whose MIP levels hold different constants). 1-channel 8-bit PNGs are read linear.
6. **Motion blur:** Karma runs the displacement shader **once, at the shutter-open sample**, and adds the **same object-space offset vector** to every motion sample. With 2 or 4 geometry samples the shader-call and micropolygon counts equal the static ones. Cycles does the same (`mesh_displace.cpp`).
7. **Nova design (§5).**
   - Displacement is a geometry step in `HdNovaMesh::Sync`:
     1. Compute per-edge tessellation rates from the dicing camera, then run OpenSubdiv Bfr. Bfr already supports per-edge rates (`bfr/tessellation.h:108-148`). Subdivision meshes use their scheme; polygon meshes use the **bilinear** scheme on the authored faces.
     2. Run the material's displacement program in the shading VM with one lane per tessellated vertex.
     3. Add the offset to every motion step.
     4. Recompute vertex normals per step by averaging the displaced triangle normals.
   - No ray-time cost.
   - Estimate: about 12–15 working days. The main risks are memory and the dicing camera in the viewport.

---

## 1. How displacement arrives in USD and Hydra

### 1.1 MaterialX definitions (`$HFS/houdini/materialx/libraries`)
- **`pbrlib/pbrlib_defs.mtlx:276-285`**
  - `ND_displacement_float`: input `displacement` (float, 0), documented as "Scalar displacement amount along the surface normal direction". Input `scale` (float, 1), "Scale factor for the displacement vector".
  - `ND_displacement_vector3`: input `displacement` (vector3, 0,0,0), documented as "Vector displacement in (dPdu, dPdv, N) tangent/normal space". Input `scale` (1).
  - Both output `displacementshader`.
- **Reference implementations do not fix the space.**
  - `pbrlib/genglsl/mx_displacement_float.glsl` and `mx_displacement_vector3.glsl` only fill `{vec3 offset; float scale}` (float: `offset = vec3(disp)`).
  - genosl returns `vector(displacement * scale)`; `pbrlib_genosl_impl.mtlx:62-63`.
  - The renderer interprets the result.
- **Related nodes.**
  - `stdlib_defs.mtlx:87-92`: `ND_surfacematerial` has a `displacementshader` input.
  - `:3859-3864`: `ND_mix_displacementshader` (fg, bg, mix). Only genmdl implements it (`stdlib_genmdl_impl.mtlx:628`).
  - `ND_dot_displacementshader` is a pass-through.
- **UsdPreviewSurface.** `displacement` is a float with default 0, "displacement in the direction of the normal" ([UsdPreviewSurface spec](https://openusd.org/release/spec_usdpreviewsurface.html)). Houdini's nodedef also declares it (`bxdf/usd_preview_surface.mtlx:23`).

### 1.2 Houdini authoring (hython probe, `probe_authoring.txt`)
- **Builder subnets.**
  - The Karma Material Builder tool runs `voptoolutils.createMaskedMtlXSubnet(kwargs, 'karmamaterial', KARMAMTLX_TAB_MASK, ..., 'kma')` (`$HFS/houdini/toolbar/ExtraTools.shelf:334-343`).
  - `_setupMtlXBuilderSubnet` (`python3.13libs/voptoolutils.py:2531-2590`) always creates an `mtlxdisplacement` VOP and wires it to output 2, named `displacement`.
- **Materials authored by the Material Library LOP.**
  - Karma builder: `token outputs:kma:displacement.connect = </World/Looks/karmamaterial/mtlxdisplacement.outputs:out>` and `outputs:kma:surface`.
  - MaterialX builder: `outputs:mtlx:displacement` and `outputs:mtlx:surface`.
  - Both also get `outputs:surface` from an auto-created UsdPreviewSurface.
  - The shader prim is `info:id = "ND_displacement_float"`. `inputs:displacement` is authored only when it is non-zero. An unchanged builder therefore exports a **wired displacement terminal with value 0**.
- **Signatures.** The `mtlxdisplacement` VOP's `signature` parm has menu items `default` (Float) and `vector3` ("Vector 3"). `vector3` authors `ND_displacement_vector3`. Its DialogScript shows `shadertype displacement`, `rendermask mtlx`.
- **Help pages.**
  - `nodes/vop/mtlxdisplacement.txt` repeats the MaterialX semantics.
  - `solaris/materialx.txt:77-87`: the MtlX Surface Material VOP, or a Collect VOP, is where a displacement network is wired.
- **Geometry properties are primvars.** The Render Geometry Settings LOP stores Karma's object properties as constant primvars. For example, the parm `xn__primvarskarmaobjectdicingquality_8sbfg` authors `primvars:karma:object:dicingquality`.
  - The probes authored `primvars:karma:object:{dicingquality,truedisplace,dicingdepthmin,dicingdepthmax,geosamples}` by hand, and Karma honored all of them (§2).
  - Nova can use the same mechanism for `primvars:nova:*`; `nova:subdivLevel` already works this way.

### 1.3 Hydra and Nova today
- **Terminal tokens.** `HD_MATERIAL_TERMINAL_TOKENS` contains `surface` and `displacement` (`$HFS/toolkit/include/pxr/imaging/hd/tokens.h:248-256`).
  - Hydra 2 resolves each terminal per render context: Karma uses kma > mtlx > universal, and Nova's chain is nova > kma > mtlx > "" (M3.6).
  - `HdMaterial::DirtyDisplacement = 1<<5` exists (`hd/material.h:32`), but every Hydra 2 material edit arrives as AllDirty and never re-syncs bound meshes (docs/research/m3/hydra-materials.md §2.4).
- **Nova.** `hydra/material/networkSource.cpp:74-80` keeps all terminals, but `Translator::Run()` reads only `surface` (`hydra/material/translator.cpp:1389`).
  - **The displacement terminal is ignored silently today.** There is no warning, contrary to the "one warning" in docs/design/M3.md:71.

---

## 2. Karma measurements

### 2.1 Spheres: 128×64 polygon UV sphere, r = 1, 8 192 faces, vertex normals, `subdivisionScheme none`

`|P|` is the distance of each hit from the sphere centre, taken over fully covered pixels. Undisplaced chords make it slightly smaller than r.
"Diced" and "calls" are Karma's `Polygons (Diced)` and `Shader Calls: Displacement` statistics.

| scene | change | \|P\| mean (min / max) | diced / calls |
|---|---|---|---|
| sph_none | no displacement | 0.99965 (0.99940 / 1.00000) | 0 / 0 |
| sph_zero | `ND_displacement_float`, displacement 0 | identical to sph_none | **0 / 0** |
| sph_const | displacement 0.1 | **1.09965** (1.09939 / 1.09999); bbox ±1.1 | 152 320 / 233 984 |
| sph_const_nonormals | same, no authored normals | identical to sph_const | same |
| sph_scale2 | 0.1, scale 2 | 1.19965 | same |
| sph_kma | `outputs:kma:displacement` | identical to sph_const | same |
| sph_preview | UsdPreviewSurface, displacement 0.1 | identical to sph_const | same |
| sph_subdiv | same cage, catmullClark | 1.09928 (limit surface + 0.1) | 152 320 / 233 984 |
| sph_bump | `primvars:karma:object:truedisplace = 0` (bump) | 0.99965 (silhouette unchanged) | 0 / 0 |
| sph_disabled | `truedisplace = 2` | 0.99965 | 0 / 0 |
| sph_coarse | 8×4 polygon sphere, no normals | 0.914 → 1.015 mean; coverage radius 0.9489 → 1.0491 | 134 656 |
| sph_vec_n | `ND_displacement_vector3` (0, 0, 0.1) | 1.0996; D·N̂ = 0.0996 | same |
| sph_vec_y | vector (0, 0.2, 0) | D·T̂ = 0, **D·(dP/dt)^ = 0.2000** (sd 1e-4), D·N̂ = −0.0004 | same |
| sphx_scale2 | prim `xformOp:scale 2`, 0.1 | **2.1993**; bbox ±2.2 | same |
| sphx_scale211 | scale (2, 1, 1), 0.1 | bbox x ±2.2, y ±1.1 | 86 000 |
| sphx_scale2_vec | scale 2, vector (0, 0.2, 0) | 2.0389 ≈ √(2² + 0.4²) = 2.0396 | |

Conclusions:
- **Direction and amount.** The scalar displacement moves the point exactly `d·scale` along the unit normal (+0.10000 at every pixel), in object units.
- **Vector displacement is tangent space.**
  - On the sphere, `|dP/dt| = π`, yet the measured amount is 0.2, so the tangent frame is normalized.
  - The direction is the `st` primvar's t direction. The polygons' parametric v runs along s, so Karma uses `st` and not the parametric uv.
  - With object-space displacement the component along dP/dt would vary with latitude, but its sd is 1e-4.
- **Karma does not smooth the polygon base mesh before dicing.**
  - The displaced coarse sphere keeps its facets: |P| min 0.963.
  - Karma interpolates its own vertex normals: for meshes without normals it uses `karma:object:cuspangle`, default 60° (`RenderPropertiesKA.json:1738`).
  - The coarse sphere's 45° dihedral angles are below that, so its normals are smooth.
- **Implicit spheres.** `usdsphere_disp` is a UsdGeomSphere with r = 1. Karma renders it as a 100-face *subdivision mesh* (|P| 0.953 → 1.053) and dices it like any subdivision mesh.
  - `points_disp` is a Points prim with width 2. It makes 1 displacement call and the whole sphere *moves* +0.1 towards +Z, so the bbox z range is −0.9…1.1.

### 2.2 Vector displacement on planes (1×1 plane of 8×8 quads, `ana.py plane_vec`, `ana2.py`, `ana3.py`)

D(st) is the world displacement at a surface parameter, computed as hit P minus the undisplaced point with the same st (read from C).

| scene | geometry and st | displacement | D (mean; sd ≤ 1e-4) |
|---|---|---|---|
| plane_vec | st ∈ [0, 2] (\|dP/ds\| = 0.5), prim rotated 90° about Z | (0.2, 0, 0) | (0, 0.2, 0) = 0.2·normalize(dP/ds) |
| | | (0, 0.2, 0) | (−0.2, 0, 0) = 0.2·normalize(dP/dt) |
| | | (0, 0, 0.2) and float 0.2 | (0, 0, 0.2) |
| plane_skew | s = x+½, t = x+y+1, so dP/ds = (1, −1, 0) and dP/dt = (0, 1, 0) | (0.2, 0, 0) | (0.1414, −0.1414, 0) = 0.2·T̂ |
| | | (0, 0.2, 0) | **(0.1414, 0.1414, 0)**: dP/dt made orthogonal to T̂, not dP/dt itself |
| plane_mirror | s = x+½, t = ½−y (mirrored) | (0, 0.2, 0) | **(0, −0.2, 0)**: B̂ keeps dP/dt's side (N̂×T̂ would give +0.2) |

The frame is exactly Nova's `KarmaT()` and `KarmaB()` (`hydra/material/translator.cpp:329-337`), evaluated in object space with the object-space normal.

### 2.3 Dicing rate: z = 0.05·sin(8πs) on a single 1×1 quad, 200 px/unit (`ana.py sin_field`)

"N err" is the angle between the rendered N and the analytic height-field normal.

| setting | diced | grid per face | edge (px) | max \|z − z_ref\| | N err mean / max |
|---|---|---|---|---|---|
| `dicingquality` 0.25 | 2 500 | 50² | 4 | 1.48e-3 | 1.86° / 2.98° |
| 0.5 (default) | 10 000 | 100² | 2 | 2.99e-4 | 0.50° / 0.67° |
| 1 | 40 000 | 200² | 1 | 1.01e-4 | 0.14° / 0.21° |
| 2 | 160 000 | 400² | 0.5 | 4.0e-6 | 0.007° / 0.034° |
| 4 | 640 000 | 800² | 0.25 | 4.7e-6 (AOV float floor) | 0.003° / 0.028° |
| default, image at 400 px/unit | 40 000 | 200² | 2 | 7.8e-5 | 0.13° / 0.17° |
| `dicingdepthmin = max` = 2 / 4 / 6 | 16 / 256 / 4 096 | 4² / 16² / 64² | – | 5.0e-2 / 1.05e-2 / 8.7e-4 | 36° / 15.7° / 1.1° (mean) |
| 16×16 base quads, default | 12 544 | 7² per face | 1.8 | 3.1e-4 | 0.37° / 0.55° |
| one catmullClark quad, default | 10 404 | 102² | ~2 | 3.7e-4 | 0.46° / 0.74° |

- **Rate rule.** `segments per face edge = ceil(raster length × dicingquality)` (12.5 px × 0.5 = 6.25 gave 7). Counts are not powers of two unless a depth is forced.
  - The raster measure is per object. A sphere half outside the frame gets the full 152 320.
  - A sphere entirely off screen gets 26 660, i.e. 0.175×, from `offscreenquality` 0.1 applied "smoothly" by angle.
- **Polygons are diced:** the polygon quad gives 10 000 micropolygons.
- **Normals are recomputed smoothly from the diced surface.**
  - Flat micropolygon normals would err by about A·k²·h/2 ≈ 9° at 2 px. The measured error is 0.5° and falls about 4× per halving of h, which is the O(h²) of interpolated vertex normals.
- **Shader calls are per diced vertex, including duplicates.** 10 816 = 104² for a 100² grid, because Karma shades sub-grids of at most 25 segments with shared borders.

### 2.4 Polygon cube with hard edges (`cube_const`, `ana_cube.py`)
- **Setup.** A 6-quad cube, half-size 0.6, shared vertices, no normals. The dihedral angle of 90° exceeds the 60° cusp angle, so the faces get flat normals. Displacement is 0.1.
- **Faces translate flat.** The height in object space is 0.7000 from the face centre all the way to the original edge (|u| < 1).
- **No holes.** The coverage scan finds no background pixels inside the silhouette.
- **Bridged edges.** The gap at each 90° edge is filled by a rounded strip; heights fall from 0.686 to 0.673 beyond the old edge. Corner |P| goes from 1.0378 to 1.1388, i.e. along the diagonal.
- Karma therefore cusps hard edges and stitches the cracks. This is not cheap to replicate (§5.4).

### 2.5 Textures in displacement (`gentex.py`, `ana.py tex`)
- **Linear EXR.** `ND_image_float` of a float EXR ramp (value = s), scale 0.1, gives z = 0.10000·s + 1e-6 (rms 1.7e-6). The lookup is bilinear, with no gamma.
- **8-bit PNG.** The same ramp as a 1-channel 8-bit PNG gives z = 0.10039·s − 2e-4: it is read linear (rms 2.2e-2 against an sRGB decode). Nova already applies this rule to surface textures (1-channel → linear).
- **MIP selection.** A tiled MIP-mapped EXR stores level L as the constant 0.1·L, so the rendered height reports the level read.

  | case | texels per micropolygon edge | level read |
  |---|---|---|
  | q 0.5 | 1024/100 = 10.24 | 2.28 |
  | q 2 | 2.56 | 0.28 |
  | q 0.25 | 20.48 | 3.28 |
  | q 0.5 at 400 px/unit | 2.56 | 0.28 |

  So **level = log2(texels per micropolygon edge) − 1.08** in every case. The fractional values are a trilinear blend, and the footprint is about 0.47 micropolygon edges. For comparison, surface lookups use log2(texels/px) − 2 (M3.9).

### 2.6 Motion blur (`gen4.py`–`gen6.py`)
| scene | motion over the shutter | displacement | Karma result |
|---|---|---|---|
| sph_mb2 / sph_mb4 | points scaled 1 → 1.2, `geosamples` 2 / 4 | 0.1 | bbox ±1.3; diced 152 320 and calls 233 984, **the same as static** |
| sph_mbx_obj / _world | points translated +2 in x | 0.1·(P.x + 1), object or world P | bbox max x **3.2**: the open sample's offset (0.2) was added at close. Re-evaluating at close would give 3.4 |
| sph_mbx_center | same, frame 1.5, shutter [−0.5, 0.5] | same | bbox −1 … 3.2: evaluated at **shutter open**, not at the frame time (that would give −1.1 … 3.3) |
| sph_mbrot | squashed sphere (y·0.5) rotating 90° about Z | 0.1 | bbox y ±**1.0177**: the open offset vector is not re-oriented at close. Applying it along each step's normal would give ±1.1 |

### 2.7 Cost
- **Dicing time.** "Dicing" is 0.01 s of a 1.7 s run for 152 k micropolygons.
- **Memory.** Peak memory is 298 MiB undisplaced and 380.5 MiB displaced (sph_none vs sph_const). That is +82 MiB for 304 k triangles, about 280 B per triangle including Karma's caches.

---

## 3. Karma's controls

All values are defaults from `$HFS/houdini/RenderPropertiesKA.json`. The Render Geometry Settings LOP and the Karma Render Properties LOP show the same defaults (hython probe).

| property | default | meaning (help text, plus measurements) |
|---|---|---|
| `karma:object:dicingquality` (`:1576`) | **0.5** | "1 = approximately one micropolygon per pixel". Measured edge = 1/q px |
| `karma:global:dicingqualityscale` (`:697`) | 1 | global multiplier (viewport scope) |
| `karma:object:truedisplace` (`:1596`) | 1 "True Displacement" | 0 bump only (no dicing); 2 shader disabled; 3 "bump added to displacement" (diced and bumped) |
| `karma:object:dicingdepthmin` / `max` (`:1634`, `:1654`) | −1 | 2^d rows/cols per face; min = max gives a fixed parametric rate (measured) |
| `karma:object:dicingpredisplace` (`:1674`) | off | trial displacement before the raster measure |
| `karma:object:dicingzimportance` (`:1686`) | 1 | below 1, the projected area is reduced by the incidence angle |
| `karma:global:dicingcamera` (`:669`) | "" (the render camera; the stats JSON reports `"dicing_camera":"/World/cam"`) | fixed camera to avoid re-dicing artifacts when the camera moves |
| `karma:global:offscreenquality` (`:679`) | 0.1 | quality factor outside the frustum, by angle |
| `karma:global:ipr_continuous_dicing` (`:716`) | **off** | "dice ... on the first render, but not if the camera tumbles" |
| `karma:object:cuspangle` (`:1738`) | 60 | normals of meshes without normals |

- **No displacement bound in Karma.** Mantra's `vm_displacebound` (`RenderProperties.json:829-836`) has no Karma counterpart. Karma bounds the diced geometry itself; its log prints a scene bbox of ±1.1.
- **XPU limits.** Karma XPU supports dicing quality, displacement style (only `mtlxstandard_surface` / `mtlxopen_pbr_surface` with float displacement), dicing depths and predisplace (`solaris/karma_xpu.txt:167-172`).

---

## 4. How other renderers do it (brief)

- **Cycles.**
  - The displacement method is per material: Bump only, Displacement only, or both.
  - Geometry density comes from adaptive subdivision. Defaults (`intern/cycles/blender/addon/properties.py`):
    - `dicing_rate` 1.0 for final renders, `preview_dicing_rate` 8.0 (the 4.3 manual defines it as the micropolygon size in pixels; current `main` describes it as a multiplier on the per-object size);
    - `offscreen_dicing_scale` 4.0;
    - `max_subdivisions` 12;
    - an optional `dicing_camera`.
  - [Manual](https://docs.blender.org/manual/en/4.3/render/cycles/render_settings/subdivision.html): it was an experimental feature in 4.x.
  - [`scene/mesh_displace.cpp`](https://github.com/blender/blender/blob/main/intern/cycles/scene/mesh_displace.cpp):
    - The shader runs **once per unique vertex**, at the corner of the first triangle that reaches it (a `done[]` array).
    - The offset is added in object space and **the same offset is added to every motion step**.
    - Vertex normals are then recomputed by accumulating face normals; flat triangles get the displaced face normal.
    - With "both", undisplaced data is kept for the bump.
- **Arnold 7.5.3.3** (`kick -info polymesh`, `kick -info options` in HtoA 6.5.3.4).
  - polymesh subdivision: `subdiv_type` (none / catclark / linear), `subdiv_iterations` 1, `subdiv_adaptive_error` 0 (0 = off, otherwise in pixels), `subdiv_adaptive_metric` auto, `subdiv_adaptive_space` raster, `subdiv_frustum_ignore`.
  - polymesh displacement: `disp_map`, `disp_padding` 0 (the bound, set by hand), `disp_height` 1, `disp_zero_value` 0, `disp_autobump` false.
  - options: `max_subdivisions` 255, `subdiv_dicing_camera`, `subdiv_frustum_culling`, `subdiv_frustum_padding`.
  - Displacement only moves the vertices that exist after subdivision, so polygon meshes need `subdiv_type linear` to get dicing.
- **RenderMan.**
  - A **displacement bound must be authored**, e.g. `Attribute "displacementbound" "float sphere"`. RenderMan warns if it is too small (holes) or more than 10× too large ([rmanwiki](https://rmanwiki.pixar.com/x/m4FxCQ), [rmanwiki](https://rmanwiki.pixar.com/x/6Q_6Ag)).
  - Dicing is by micropolygon length; the dicing strategy defaults to `instanceprojection`, and `worlddistance`/`objectdistance` are alternatives ([rmanwiki](https://rmanwiki.pixar.com/x/NRm6Ag)).
  - Displacement has been in object space since RenderMan 22.
- **Takeaway for Nova.** Karma and Cycles are the closest models: pre-tessellate in raster space, evaluate the shader once per vertex, add the offset to all motion steps, recompute smooth normals. Pre-tessellation also removes the need for a user-set bound (Arnold padding, RenderMan bound).

---

## 5. Design for Nova (minimal, production-usable)

### 5.1 Translation: a displacement program
- **Where it is compiled.** `Translator::Run()` also compiles the `displacement` terminal into the material's IR as one more output: the **object-space offset**, 3 floats. The compiler then derives `Program::displacement`.
  - This works like `Program::opacity` (K10): the graph is dead-code-eliminated down to that output and shares the material's parameters and texture slots.
  - The surface program never contains it, so displacement costs nothing at ray time.
- **Node lowering.** (`ObjN` is the object-space unit normal at the vertex. `T̂`/`B̂` are `KarmaT`/`KarmaB` built from object-space `DPdu`/`DPdv`/`N`.)
  - `ND_displacement_float`: `offset = displacement · scale · ObjN`.
  - `ND_displacement_vector3`: `offset = scale · (x·T̂ + y·B̂ + z·ObjN)`.
  - UsdPreviewSurface's `displacement` input: `offset = d · ObjN`.
  - `ND_mix_displacementshader`: `lerp(bg, fg, mix)` of the offsets. `ND_dot_displacementshader` and a `surfacematerial.displacementshader` input are followed through.
  - Unknown displacement nodes get one warning, and the material is not displaced.
- **Zero detection.** If folding leaves a constant (0, 0, 0) offset, as with the default Karma builder and `scale 0`, the material has **no displacement**. Its meshes are not tessellated (Karma parity, sph_zero).
- **Context.** The terminal comes from the same context chain as the surface (nova > kma > mtlx > ""); this is already per-terminal in the network source.

### 5.2 Where it runs: `HdNovaMesh::Sync`, before `AcquireSceneForEdit()`
`_BuildMesh` gets one more stage when any material slot of the mesh has a displacement program and the style is not `off`:

1. **Control mesh and motion steps.** Unchanged (`hydra/mesh.cpp:350-480`).
2. **Rates** from the dicing view (§5.3).
3. **Tessellation.** Use `tessellateSubdiv` with **per-edge rates**.
   - Subdivision meshes keep their scheme.
   - Polygon meshes (`none`, and also `bilinear`) go through the same Bfr path with `SubdivScheme::Bilinear` on the authored faces. Bfr's bilinear limit surface *is* the polygon, n-gons included.
   - The output is watertight by construction, primvars are interpolated (vertex, varying, faceVarying, uniform), and the per-step points already exist.
4. **Evaluation** of the displacement program on the tessellated vertices of step 0 (§5.5).
5. **Apply** the offset to every step, then recompute normals per step (§5.6).

### 5.3 Tessellation rates (Karma-like dicing quality)
- **Per-edge rate.**
  - For every control edge e: `rate_e = clamp(ceil(L_px(e) · q · qScale · f_off), 1, 2^dmax)`.
  - Inputs: `q` is `primvars:nova:dicingQuality` (default 0.5 = Karma), `qScale` is the render setting `nova:dicingQualityScale` (default 1), and `dmax` is 10 (Karma's range, 1 024 segments).
  - `L_px` measures the edge between its two limit/corner points:
    - **perspective:** `L_world · (res_y / 2) / (tan(fov_y / 2) · max(dist, near))`, where `dist` is the distance to the camera position. A spherical measure keeps rates finite behind the camera and off screen.
    - **orthographic:** `L_world · res_x / width`.
- **Bfr API.** `Bfr::Tessellation(param, N, edgeRates)` infers the inner rates (`tessellation.h:116-148`). An edge shared by two faces gets one rate, so both faces emit the same edge points.
  - `subdiv.cpp` pass 1 already shares edge points through an owner face. It needs `perEdge = rate_e − 1` per edge instead of one global `rate − 1` (`subdiv.cpp:332`).
- **Off screen.**
  - `f_off` = 1 inside the frustum (the mesh bbox intersects it). Outside, it blends to `nova:offscreenQuality` (0.1) over the angle to the frustum edge.
  - Karma's numbers: whole-mesh off screen gives 0.175× the micropolygons. A partially visible mesh dices at the full rate, so a per-mesh test is enough.
- **Fixed rate.** If `primvars:nova:dicingDepth` = d is authored, every edge gets 2^d (Karma's `dicingdepthmin = max`). The fixed rate is also the **fallback when no camera is known**.
- **Budget.**
  - Estimate triangles as Σ over faces of ≈ (mean edge rate)² (quads ×2).
  - If the estimate exceeds `nova:subdivMaxTriangles` (8 M default, `renderDelegate.cpp:87`), scale q by √(budget/estimate) and warn once per mesh. This is the same policy as subdivision today.
  - Memory estimate for Nova: points + normals + st ≈ 32 B per vertex, indices 12 B per triangle, Embree BVH ≈ 50–80 B per triangle, so about 110–140 B per triangle. 8 M triangles ≈ 1–1.1 GiB (an estimate, not measured).
- **Default density.** Karma's default q = 0.5 dices to about 0.5 triangles per screen pixel². The 600² sphere gave 304 k triangles; a full-HD frame filled with displaced surfaces, plus back faces, is a few million.

### 5.4 Displacement direction
- **Subdivision meshes:** the limit normal (Du × Dv), already computed per point.
- **Polygon meshes:**
  - authored vertex/varying normals → evaluated bilinearly through Bfr, then normalized;
  - otherwise → an **angle-weighted average of the face normals at each control vertex**, including faceVarying normals averaged per vertex. The same normal is used by every face around a vertex, so the result is **watertight**.
- **Difference to Karma.** A hard-edged mesh (the cube) becomes pillow-shaped instead of Karma's flat faces with a stitched rounded strip (§2.4).
  - Cusped normals plus crack stitching is a later refinement. Displacement is normally used on smooth or subdivision assets.
- **Subdivision crease copies.** `subdiv.cpp`'s `splitVertices`, the infinitely sharp copies, take the offset of their copy source (`copySource[]`), so creases cannot open.

### 5.5 Evaluation: lanes = vertices
- **Entry point.** Add `nova::evaluateDisplacement(...)` in `core/integrator/material_eval.cpp`, next to `fillAndRun`.
  - Unique tessellated vertices are grouped by the material slot of their **owner triangle**, the first triangle that references them (Cycles' rule; it also settles GeomSubset borders).
  - Chunks of 32 run through `execute<32>`, with `tbb::parallel_for` over chunks inside a mesh.
- **Rows filled from per-vertex arrays.** No `SurfaceHit`, no `Scene` lookup, because the mesh is not in the scene yet. Factor `Scene::commit`'s primvar-binding resolution into a function both paths call.
- **Globals.**

  | global | value at a vertex |
  |---|---|
  | `Pobject`, `Nobject` | tessellated point and direction normal (step 0) |
  | `P`, `N`, `Ng` | the same through the mesh transform at step 0 (`Ng` = `N`) |
  | `I` | −N |
  | `DPdu`, `DPdv` | `dualBasis` of the owner triangle's tangent-uv gradients (as `tangentDerivatives`) |
  | `ObjX`…`ObjT` | transform at step 0 |
  | `Frame`, `Time` | as for surfaces |
  | `PrimId` | the authored face (`_triangleFaces`) |
  | `InstanceId` | 0 |

- **Primvars.**
  - vertex/varying rate: the value at the vertex;
  - corner (faceVarying) rate: the owner triangle's corner;
  - uniform rate: the authored face;
  - constant: as is;
  - **instance primvars are unavailable**, because one BLAS serves all instances. This is the same limit as any pre-tessellating renderer.
- **Footprints (derivative clones, K7).** `PDx`/`PDy` = P + ½·(the owner triangle's two edges at the vertex). A shifted primvar is the value halfway along the same edge. See §6 for what this does to texture lookups.
- **Deterministic.** No stochastic texture filtering here (`rnd = null`), so a re-sync reproduces the same geometry.

### 5.6 Applying, normals, motion
- **Offsets.** `P'_s = P_s + offset` (object space) for **every** step s, with offsets from step 0.
  - This is Karma parity, measured with 2/4 samples, translation, rotation and a centred shutter, and it is Cycles' rule.
  - Evaluation cost does not depend on the step count, and procedural displacement does not swim within the shutter.
  - Known artifact, shared with Karma: under rotating deformation the offset does not rotate (sph_mbrot).
- **Normals.** Recompute per step from the displaced triangles: area-weighted vertex normals, the same as Cycles' `compute_vertex_normals`. Karma's measured error behaves like this, O(h²).
  - Copies at sharp creases average only over their own side's triangles.
  - Uniform-rate and constant primvars are untouched; vertex and corner primvars stay as tessellated.
- **No ray-time changes.** The displaced mesh is an ordinary `MeshDesc` (Embree BLAS, motion steps, attributes). Shading sees the displaced P and the recomputed N.
  - "Bump added to displacement" would need the undisplaced P and N at the hit (Cycles keeps them as attributes), so it is left out of the minimal scope.

### 5.7 Special prims
- **Analytic spheres (M3.6b) with a displacing material.**
  - Fall back to a tessellated sphere: a 6-face cube cage, Bfr bilinear per-edge rates, points projected onto the exact sphere, uv from Nova's sphere parametrization. Then displace like any mesh.
  - Karma itself renders UsdGeomSphere as a 100-face subdivision cage with limit radius 0.92–0.985. Karma references with spheres keep that known offset; implicit_spheres already handles it with a Points variant, and points cannot be used for displacement.
- **Instancers.** Displacement runs once per prototype.
- **Points and curves.** Out of scope. Karma only offsets a point sphere as a whole.

### 5.8 IPR invalidation
| event | action |
|---|---|
| material edit that changes the displacement program, its parameters or a texture it reads (compare a displacement hash) | re-sync the meshes bound to that material, which re-tessellates them |
| material edit that changes only surface outputs | nothing geometric (today's path) |
| binding or GeomSubset change, from or to a displacing material | geometry rebuild (`materialsDirty` alone is not enough) |
| material removed | rebuild without displacement |
| camera tumble in IPR | **no re-dicing** (Karma: `ipr_continuous_dicing` off); optional `nova:continuousDicing` |
| `nova:dicingQuality*`, `nova:dicingCamera`, resolution change | re-dice the displaced meshes (like the subdiv-settings resync) |
| shutter change | existing resample path (displacement included) |
| texture file changed on disk | existing mtime path → material invalidation → re-displace |

- **Mesh re-sync.** Hydra never re-syncs meshes on material edits (M3 research §2.4). Two pieces are needed:
  - the material registry tracks `material path → mesh paths`; meshes register in `Acquire`;
  - `HdNovaResyncSceneIndex::DirtyPrims` is extended to take a **path list**. Today it dirties all prims of a type (`hydra/resyncSceneIndex.h:33`).
  - Both pieces were already proposed for primvar unions in M3.
- **Dicing camera.**
  - **husk:** `renderCameraPath`, `resolution` and the shutter arrive with the initial settings (`renderDelegate.cpp:99-112`, `renderParam.h:53`), and sprims sync before rprims. The first sync can therefore look up the HdCamera and dice correctly.
  - **Viewport:** the camera is known only in `HdNovaRenderPass::_Execute`.
    1. Meshes synced before then use the fixed fallback and are flagged.
    2. The first Execute stores the view in the render param and re-dirties the flagged meshes once, the same pattern as the shutter resync (`renderPass.cpp:234-281`).

### 5.9 Settings (proposal)
| name | where | default |
|---|---|---|
| `nova:dicingQuality` | constant primvar per prim | 0.5 |
| `nova:dicingDepth` | constant primvar (fixed 2^d per edge) | unset |
| `nova:displacement` | constant primvar: `true` / `off` | `true` |
| `nova:dicingQualityScale` | render setting | 1 |
| `nova:dicingCamera` | render setting (camera path) | "" (render camera) |
| `nova:offscreenQuality` | render setting | 0.1 |
| `nova:continuousDicing` | render setting | off |
| `nova:subdivMaxTriangles` | existing, also caps displaced meshes | 8 000 000 |

### 5.10 Effort and risks
**Effort** (working days, with tests):

| step | content | days |
|---|---|---|
| 1 | translator: displacement output, node lowering, zero folding | 1–1.5 |
| 2 | vertex-lane evaluator and attribute-binding refactor | 2 |
| 3 | per-edge Bfr rates, the polygon bilinear path, the sphere fallback, the dicing view and rates | 3–4 |
| 4 | apply, normals per step, crease copies, budget | 1.5 |
| 5 | Hydra plumbing: material→mesh index, path-list resync, dicing camera / first Execute, settings, IPR rules | 2–3 |
| 6 | unit tests, husk checks, the Karma reference scene, docs | 2–3 |
| | **total** | **≈ 12–15** |

**Risks:**
1. **Memory and time on large screens or close-ups.**
   - Mitigation: the budget, off-screen falloff and `dmax`.
   - Each displaced mesh rebuilds its BLAS on every displacement edit, so IPR latency exceeds 100 ms for heavy meshes; Karma re-dices too.
2. **Viewport double sync** of displaced meshes at the first frame (bounded: only flagged meshes).
3. **Parallel mesh syncs** read programs and textures. Programs are immutable `shared_ptr`s and the OIIO TS is thread-safe. Lock the registry only for the lookup.
4. **Cracks.**
   - Watertight by construction: per-edge rates are shared, offsets are evaluated once per shared vertex (owner rule), crease copies follow their source.
   - The remaining discontinuities are GeomSubset borders between *different* displacement materials, which are watertight but kinked (Karma does not measure better there).
5. **Parity gaps with Karma:**
   - hard-edged polygons (pillow instead of cusp and stitch);
   - Nova's polygon meshes without normals are flat-shaded, while Karma uses cusp-60° smooth normals. The displacement direction must be smooth even when shading is flat.
   - implicit spheres;
   - Karma's per-mesh raster measure with sub-grids vs Nova's per-edge measure (rates differ by rounding, not by scale).

---

## 6. Texture lookups and globals at vertices

- **Footprint.** There are no ray cones at tessellation time. The footprint is the tessellation itself: derivatives = ½ × the uv change along the owner triangle's two edges at the vertex.
  - With OIIO's full-width derivative convention, the MIP level is **log2(texels per edge) − 1**.
  - Karma measures −1.08 (§2.5), a footprint 5 % narrower; that is not visible in practice.
  - Because the footprint is tied to the dicing rate, displacement maps lose detail finer than the micropolygon size **by design**: those frequencies cannot be represented by the mesh. A later "bump added" mode restores them.
- **Mechanism.** It reuses the M3.9 compiler machinery unchanged. Derivative clones read `PDx`/`PDy` and shifted primvars, and the evaluator fills them from the triangle edges instead of ray cones.
- **Filter and decoding.** Trilinear MIP (Karma blends fractional levels), bilinear within a level. The texture decoding rules are those of surface textures: OCIO file rules, 1-channel linear, .tx/.rat conversion. Karma's PNG ramp matched linear.
- **Globals that must be valid at vertices.**
  - Required: `P`, `Pobject`, `N`, `Nobject`, `Ng`, `DPdu`, `DPdv` (vector displacement, normal-map-like nodes), primvars (`st` and any `geompropvalue`), `Frame`, `Time`, `Obj*`, `PrimId`, `PDx`, `PDy`, `PobjectDx`, `PobjectDy`.
  - Placeholders: `I` = −N; `InstanceId` = 0.
  - No ray-dependent nodes: `kma_rayswitch`-like and ray-import nodes evaluate to their camera-ray defaults.
- **Space conventions.** MaterialX `position`/`normal` nodes default to object space; this matches the object-space offset. World-space reads use step 0's transform (Karma: the shutter-open sample).

---

## 7. Tests

**Analytic (Catch2, core only; MemorySampler for textures):**
1. **Sphere.** A 128×64 polygon sphere and a catmullClark cage, constant 0.1.
   - |P| = 1.1 within the chord sagitta. `scale 2` gives 1.2. A transform scale of 2 gives world radius 2.2 (object space).
   - A constant 0 or `scale 0` program produces **no** tessellation (the triangle count is unchanged).
2. **Height field.** A plane with `0.05·sin(8πs)`:
   - z is within the chord error A·k²·h²/8 for the chosen rate;
   - the normal error falls about 4× per rate doubling (O(h²));
   - the rate follows `ceil(L_px·q)`: a 200-px quad at q 0.5 gives 100 segments, as in Karma's table.
3. **Vector frame.** The §2.2 cases with the expected D values: rotated plane, skewed st (0.1414, ±0.1414), mirrored st (0, −0.2).
4. **Watertightness.**
   - Displaced closed meshes (subdivided cube cage, polygon cube, sphere fallback) leak no rays: 10 000 rays from inside must all hit.
   - Crease copies keep the same positions.
5. **Textures.** The MIP-probe texture (level L = 0.1·L) gives level log2(texels/edge) − 1 ± 0.05. A linear ramp is reproduced.
6. **Motion.**
   - A translating sphere with P-dependent displacement reaches max x = 3 + d(step 0) (Karma 3.2).
   - The rotating squashed sphere reaches max y = 1.0177 if Karma parity is chosen.
   - Per-step normals are unit length and match the displaced triangles.
7. **Budget.** A tiny `nova:subdivMaxTriangles` triggers one warning and a proportionally reduced rate.

**Hydra / IPR (driver tests like `test_hydra_materials.cpp`):**
- A displacement parameter edit rebuilds only the bound meshes; a surface-only edit rebuilds none.
- Binding to and from a displacing material rebuilds.
- Material removal undisplaces.
- A camera move in IPR does not re-dice.
- husk dices on the first sync, without a second sync.

**Karma references (husk, `tests/reference`):**
- `disp_analytic`: the §2.1/§2.2 scenes with P/N AOVs, compared numerically. Nova needs a P AOV or depth; depth exists.
- **`disp_textured`.**
  - Geometry: a catmullClark cube cage, 2 subdivision levels, uv-mapped, and a 1×1 polygon plane.
  - Displacement: a float EXR height map (`ND_image_float` → `ND_displacement_float`, scale 0.08) and one vector3 case.
  - Lighting: a grazing rect light plus a dome; perspective camera; `primvars:karma:object:dicingquality` = `primvars:nova:dicingQuality` = 1.
  - Comparison: `nova_imgcmp` with a region tolerance on silhouettes; dicing differs by rounding.
  - Tune the tolerances on the Karma P/N AOVs first, then the beauty.

---

## Recommended M4 scope (minimal)

1. **Translator.** Displacement terminal → `Program::displacement`, an object-space offset.
   - Nodes: float, vector3 (KarmaT/KarmaB frame), mix, UsdPreviewSurface `displacement`.
   - Constant-zero folding means no tessellation. Every unsupported node is warned once, which replaces today's silent ignore.
2. **Tessellation.** Per-edge Bfr rates in `core/geometry/subdiv.cpp`. Subdivision meshes keep their scheme; polygon meshes use the bilinear path. Rate = `ceil(L_px · q)` (q 0.5) with the off-screen factor, the `nova:dicingDepth` fixed mode and the `nova:subdivMaxTriangles` cap.
3. **Vertex-lane evaluation.** Globals from §6, owner-triangle rule, footprints from the tessellation edges.
4. **Apply.** Offsets from step 0 added to all motion steps (Karma/Cycles parity), smooth per-step normals.
5. **Analytic spheres** with displacement → tessellated fallback.
6. **Hydra.** material→mesh index with path-list resync; dicing camera from husk settings, or from the first Execute in the viewport; no re-dice on camera moves in IPR; settings from §5.9.
7. **Tests and references** from §7.
8. **Not in M4:** bump-only and bump-added modes, cusp-angle stitching of hard edges, predisplacement, continuous dicing, dicing z-importance, displacement on points/curves/volumes.

## Open questions for the user

1. **Karma compatibility of object properties.** Should Nova also read `primvars:karma:object:{dicingquality, truedisplace, dicingdepthmin/max}` as fallbacks when no `nova:` value is authored?
   - Scenes set up for Karma would then dice the same in Nova.
   - The cost is Nova behaviour depending on another renderer's namespace.
2. **Motion-blurred displacement.** Choose between:
   - **Karma/Cycles parity** (recommended): evaluate at step 0 and add the same offset vector to every step.
   - **Re-orient** the offset in each step's local frame. This is more correct for rotating deformation, still one shader evaluation, but it differs from Karma.
3. **Default dicing quality.** Choose between:
   - **0.5** (Karma 22 default, 2-px micropolygons; recommended for parity);
   - **1.0** (1-px micropolygons as in Cycles and RenderMan; 4× memory, sharper silhouettes).
4. **Bump modes** ("displacement as bump", "bump added to displacement"): in M4, or later (recommended later)? Bump-only is cheap with the existing `Deriv`/heighttonormal machinery. "Bump added" needs the undisplaced P and N stored per vertex.
