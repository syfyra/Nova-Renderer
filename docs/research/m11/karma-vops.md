# M11 research: Karma's shading nodes (VOPs) and material recipes in Nova

Goal (user): Nova should be "compatible with all Karma nodes". This report covers the shading side: every Karma VOP
type of Houdini 22.0.466 that is neither hidden nor deprecated, and Karma's material recipes. For each it says what
the node is, how Karma implements it, what Nova does today (with file:line evidence), what full compatibility needs,
how much work that is, and how to verify it against Karma. The render-property side (Karma Render Settings, Render
Geometry Settings, Light and Camera LOP parameters) is in `docs/research/m11/karma-properties.md`; where both touch
(material properties, lens shaders, light filters) the packages below are written to share one implementation.

No code was changed. SideFX code and help text are described in my own words, never copied. All probes ran in
separate, isolated processes (`tests/houdini/run_hython.ps1`, `refcommon.IsolatedHoudini` for husk) against the staged
build `build/vs2022/stage/Release/nova`; the user's Houdini session was not touched.

`$HFS` = `C:\Program Files\Side Effects Software\Houdini 22.0.466`.

## 0. Summary

**45 VOP types** in scope (48 Karma VOP types minus `kma_fakecaustics`, `kma_nesteddielectrics` (hidden and
deprecated) and `kma_lfilter_blocker` (hidden); versions counted separately: `kma_aov`/`::2.0`, `kma_fur`/`::2.0`,
`kma_rayimport`/`::2.0`), plus **11 recipes** (10 VOP recipes and the SOP recipe that creates a Karma Pyro Material).

| | Supported | Partial | Missing |
|---|---|---|---|
| VOP types (45) | 16 | 16 | 13 |
| Recipes (11) | 2 | 6 | 3 |

- **Supported** = renders as in Karma on CPU and XPU (within documented, measured tolerances), or matches Karma by
  construction (Karma's own graph).
- **Partial** = renders, but inputs, modes, a device (XPU) or an output (AOV) are missing or differ in a documented
  way, or parts are ignored without a warning.
- **Missing** = Nova renders the node's default outputs, a default shader, or ignores the feature (light filters,
  lens shaders).

Main findings:

1. **The pattern side is in good shape.** Ray switch, ray imports, camera falloff, ramps, ST derivatives, melanin,
   tangent rotate, ray queries (AO, distance, curvature, rounded edge on CPU), Karma Hair, `kma_volume` and the whole
   pyro shader family render, mostly verified against Karma (`tests/test_hydra_karma.cpp`,
   `tests/test_shading_queries.cpp`, `tests/husk/check_hair.py`, `tests/houdini/test_volume_karma.py`).
2. **Karma's material conventions are where Nova is silently wrong.** `kma_material_properties` writes
   `inputs:karma:<property>` on the terminal shader (29 properties, probe 2.2); Nova reads only `karma:lpetag` and
   skips the rest on purpose without a warning (`hydra/material/translator.cpp:757`,
   `tests/test_hydra_karma.cpp:252-258`). Measured: a floor whose material sets `karma:diffuselimit` 0 is 11.4 %
   darker in Karma, unchanged in Nova. This breaks Houdini's own recipes: **Pyro Fire/Explosion** (fire as light
   source with diffuse scale 10), **Cloud** (volume limit ~33 instead of 0, i.e. multiple scattering), **Uniform
   Volume** (a mesh rendered as fog; Nova renders it as an opaque Lambert surface, measured), and volume AOVs
   (`aov_fire`, `aov_scatter` of every pyro recipe stay 0).
3. **Three whole feature areas are absent:** light filters (`kma_lfilter_*`, `kma_light_projection`; no `lightFilter`
   sprim, `light:filters` ignored silently), lens shaders (`kma_physicallens` as a camera material, CVEX lens shaders
   `kma_roomlens`/`kma_uvlens`/custom; ignored silently), and mesh-bounded uniform volumes.
4. **Nodes Nova warns about and renders with defaults:** `kma_pcread`, `kma_roommap`, `kma_whitewater`,
   `kma_pyropreview`, plus 15 of the 27 ray-import variables and non-matrix OCIO transforms.
5. **Look-alike, not identical:** hex tiling and Voronoi use Nova's own hash (same look, other random layout;
   `docs/design/M4.md:300-311`). Houdini ships viewport GLSL twins of Karma's hex tiling and room map
   (`$HFS/houdini/glsl/houlib/GL32/hextiling.func`, `roommap.func`, `glsl/mx/kma_matx.def`), a lead not used in M4.
6. **Stale bookkeeping:** README's "35 of Karma's 55 `kma_*` shading nodes" counts registry shader ids, not VOPs, and
   is now 39 (hair, fur, fur 2, volume render since M8/M9; section 4). The Nova Material Builder's TAB mask still
   hides hair, fur, volume and every pyro node although Nova renders them (`houdini/package/python3.13libs/novautils.py:20-24`,
   test `tests/houdini/test_material_builder.py:46`); README:268 still says "hair, fur and point clouds: M9".

Work packages (section 6), ordered by user value: **A** Karma material conventions (M) - **B** mesh-bounded uniform
volumes (L) - **C** light filters (L) - **D** volume shading completion: volume AOVs, whitewater, pyro preview (M) -
**E** ray state completion and XPU ray queries (M) - **F** lens shaders (L) - **G** hair and fur completion (M-L) -
**H** pattern parity: hex tiling, Voronoi, room map, point clouds, OCIO, texture controls (M-L).

## 1. Sources and method

| # | Source | What |
|---|---|---|
| S1 | scratchpad `m11/karma_nodes.json` (hython, `hou.nodeTypeCategories()`) | the 48 Karma VOP types and 14 Karma "Data" recipes, hidden/deprecated flags, libraries |
| S2 | `$HFS/houdini/karmaShaderNodes.json` | Karma's shader registry: 55 `kma_*` ids with inputs, defaults, outputs; light filters typed `kma:lightfilter` |
| S3 | `$HFS/houdini/karmaNodeGraphs.json` | Karma's graph library (deriv_2d, tangentrotate, melanin, hextiled_triplanar) |
| S4 | `$HFS/houdini/materialx/libraries/houdini/kma_camerafalloff.mtlx` | the only MaterialX nodedef of a `kma` node |
| S5 | `$HFS/houdini/otls/OPlibVop.hda`, `MaterialX.hda`, `OPlibRecipe.hda` (dumped with hython: sections, function names, render masks, shader types, inner networks) | VOP definitions and recipes |
| S6 | `$HFS/houdini/vex/VopCVex/kma_roomlens.vfl`, `kma_uvlens.vfl`; `kma_physicallens` VflCode section (3.7k lines) | CVEX lens shaders |
| S7 | `$HFS/houdini/dso/usd_plugins/usdKarma/resources/generatedSchema.usda` | `Karma*LightFilter` prim schemas with `kma:lightFilter:shaderId` |
| S8 | `$HFS/houdini/soho/parameters/BRAY_HdKarma_Camera.ds`, `python3.13libs/loptoolutils.py` (`createKarmaLensMaterial`), `python3.13libs/husd/karmautils.py:126-165` (ray import menu) | camera lens properties, ray-import variables |
| S9 | `$HFS/houdini/help/nodes.zip` (`vop/kma_*`), `solaris.zip` (`karma_xpu.txt`, `support/light_filters.txt`, `support/karma_lens_shader.txt`) | documentation (paraphrased) |
| S10 | `$HFS/houdini/glsl/mx/kma_matx.def`, `glsl/houlib/GL32/{hextiling,roommap,melanin}.func` | Houdini's viewport GLSL versions of some Karma natives |
| S11 | `docs/research/m4/karma-nodes.md` | M4's measurements (ray classes, queries, ramps, dot notes); reused, not repeated |
| N | `hydra/material/translator.cpp`, `nodeDefDatabase.cpp`, `material.cpp`, `hydra/aov.cpp`, `hydra/renderDelegate.cpp`, `core/shading/{query,vm}.h`, tests | Nova's state at main `caba75c` |

### 1.1 Probes run for this report (isolated; scripts in the session scratchpad `m11/`, not committed)

1. **USD form of every node** (`probe_usd_forms3.py`, hython): one Karma Material Builder per VOP type wired to the
   right output, the recipes instantiated headless through `hrecipes.storage` (each in its own Material Library LOP:
   a recipe replaces the library's children), a Light Filter Library with all filter types assigned to a light, a
   camera with the Karma lens material and one with the UV lens VOP; the stage exported as usda. Results in 2.2.
2. **Nova's reaction** (`probe_nova_warnings.py`, husk with Nova and Karma): every exported material on its own quad
   or VDB volume, the lens cameras as render cameras. Nova warns for `kma_physicallens` (translated as a surface),
   `kma_whitewater`, `kma_light_projection`, `kma_pcread`, `kma_pyropreview`, `kma_roommap`, `ray:hituv`, the fur
   medulla. **Silent** for: material properties, the uniform volume material on a mesh, volume AOVs, light filters,
   both lens cameras, texture controls.
3. **Material properties** (`probe_props.py`, 160x120, 256 spp, both renderers): a floor half whose Standard Surface
   carries `inputs:karma:diffuselimit = 0`, a control half without it, a back wall, and a cube bound to a
   `kma_volume` with `inputs:karma:volumeuniform = 1`.

   | region mean | Karma | Nova |
   |---|---|---|
   | floor with `karma:diffuselimit` 0 | 0.3601 | 0.3967 |
   | floor without | 0.4065 | 0.3991 |
   | ratio | **0.886** | 0.994 (noise) |
   | uniform-volume cube region | 0.262 (grey fog, wall visible through it) | **0.085 (opaque Lambert cube)** |

4. **AOV nodes** (`probe_aov1.py`): `kma_aov` 2.0 (`inputs:aov:myaov` on the surface): Nova = Karma = (0.25, 0.5, 0.75)
   exactly. `kma_aov` 1.0 (in-line node, id `kma_aov`): Karma 22.0.466 prints "Unhandled node type ... kma_aov" and
   drops the surface graph; its AOV is 0 in both renderers; Nova keeps the graph (base colour passes through).
5. **Texture controls / MikkT presets** (`probe_texcontrols.py`, `hou.data.applyNodePresetRecipe`): the presets add
   spare parameters that export as `inputs:karma_blur`, `karma_width`, `karma_filter`, `karma_lerp` on `ND_image_*`
   and `inputs:karma_mikkt` on `ND_normalmap_float`.

## 2. How Karma's nodes reach USD (what a renderer must read)

### 2.1 Implementation kinds

| Kind | Nodes | How Karma runs it |
|---|---|---|
| **native** (C++ in libBRAY; VOP render mask `karma`, `FunctionName` section; id in S2) | ao, distance, curvature, roundededge, rayswitch, rayimport 2.0, rampconst (+ `kma_ramp_*` from the Karma ramp parameter), stderiv, voronoinoise 2d/3d, hextiled_texture (`kma_color_hextiling`, `kma_normalmap_hextiling`), ocio_transform, pcread, roommap (+ registry-only `kma_room_multimap`), hair, fur (both versions export `kma_fur_2`), volume, whitewater, pyropreview, light filters, light_projection, physicallens (as lens material) | opaque; behaviour only by measurement (and, for some, the viewport GLSL twin S10) |
| **Karma graph** (S3) | deriv2d, tangentrotate, melanin, hextiled_triplanar | Karma expands the graph itself; Nova loads the same graphs |
| **MaterialX nodedef** (S4) | camerafalloff | graph of stdlib nodes plus dot-note ray imports |
| **NodeGraph** (subnet HDA exported as USD NodeGraph) | pyroshader, pyrofirecolor, pyrofireemission, pyroscattercolor, pyroscatteremission, pyrosmokecolor, pyrovolumemask; rayimport 1.0 inside recipes | plain MaterialX nodes, `kma_ramp_*`, `kma_volume`, `ND_dot_*` with `note = "ray:<var>"` |
| **convention** (no shader) | aov 2.0, material_properties | inputs `aov:<name>` / `karma:<property>` on the terminal shader, plus Material outputs and RenderVar prims |
| **CVEX lens** (S6; render mask VMantra) | physicallens (VflCode), physicallenscore (building block), roomlens, uvlens | `karma:camera:lensshader` string `opdef:/Vop/<type>?...VflCode`, Karma CPU only |
| **legacy** | aov 1.0 | id `kma_aov` is no longer in Karma's registry; Karma 22.0.466 rejects it (probe 4) |

### 2.2 USD forms measured today (probe 1)

- **Material properties** (all 29 activated): `int/float/color3f/string inputs:karma:<p>` on the surface terminal
  (`causticsenable, causticsroughnessclamp, dielectricpriority, diffuselimit, diffusequality, fakecausticsbsdfenable,
  fakecausticscolor, fakecausticsfresneldarken, fakecausticsopacity, lightsource_diffusescale,
  lightsource_samplingquality, lightsource_specularscale, lpetag, mtlx_imageblur, mtlx_imagewidth, reflectlimit,
  reflectquality, refractlimit, refractquality, ssslimit, sssquality, thinwallatrefractlimit, treat_as_lightsource,
  volumelimit, volumequality, volumesteprate, volumeuniform, volumeuniformdensity, volumeuniformsamples`). For a
  **volume-only material they go on the `kma_volume`/`kma_whitewater` shader** (the recipes: `karma:volumeuniform`,
  `karma:treat_as_lightsource`, `karma:lightsource_diffusescale`, `karma:volumelimit`).
- **AOV 2.0 on volumes:** the pyro recipes put `vector3f inputs:aov:aov_fire/aov_scatter` on the inner `kma_volume`
  of the `kma_pyroshader` NodeGraph, plus Material outputs and RenderVars (`sourceType raw`, filter `ubox`).
- **Light filters:** typed prims `KarmaGelLightFilter`, `KarmaGoboLightFilter`, `KarmaBarndoorLightFilter`,
  `KarmaAttenuationLightFilter`, `KarmaBlockerLightFilter` (UsdLux light filters whose schema sets
  `kma:lightFilter:shaderId = kma_lfilter_*`, S7), with `outputs:lightfilter`, their inputs (ramps as key/value/basis
  arrays), connections to ordinary MaterialX shaders under the filter prim (a fractal noise driving the gel's
  intensity), and `rel light:filters` on the light. Hydra side: `HdPrimTypeTokens->lightFilter`,
  `UsdImagingLightFilterAdapter::GetMaterialResource`, the light's `HdTokens->filters`
  (`$HFS/toolkit/include/pxr/usdImaging/usdImaging/lightFilterAdapter.h`, `pxr/imaging/hd/tokens.h`).
- **Lens material:** the camera gets `rel material:binding` to a Material whose `outputs:kma:surface` connects a
  `kma_physicallens` shader (info:id `kma_physicallens`, registry inputs: bokeh, aberration ramp, projection incl.
  `texture_xpu` maps, OpenCV k1-k6/p1-p2, rolling shutter, tilt/shift, curvature, vignetting, focus/f-stop maps,
  exposure/tint). **CVEX lens:** `string karma:camera:lensshader = "opdef:/Vop/kma_uvlens?CVexVflCode"`,
  `bool karma:camera:use_lensshader`, `karma:camera:lensshadervop`, and a Material with `outputs:vex:cvex`.
- **Texture controls:** `inputs:karma_blur/karma_width/karma_filter/karma_lerp` on `ND_image_*`; **MikkT:**
  `inputs:karma_mikkt` on `ND_normalmap_float`.
- **Fur 1.0** exports `info:id = "kma_fur_2"` like 2.0. **Ray import 1.0** exports `kma_rayimport_<type>` when made
  fresh and a NodeGraph of `ND_dot_<type>` with a `ray:` note inside the recipes; Nova reads both.
- `kma_light_projection` in a surface exports `info:id kma_light_projection` and makes Karma's surface graph fail
  ("Error wiring nodes"): it only works inside light filter networks.

## 3. The table

Status: **S** supported, **P** partial, **M** missing. Effort: **S** = one step, a few hundred lines, days;
**M** = a design-doc step with a new runtime piece, about one to two weeks; **L** = milestone-sized (several steps,
CPU and XPU, new subsystem). Translator = `hydra/material/translator.cpp`.

### 3.1 Surface patterns and utilities

| Node | Kind | Karma implementation | Nova today | St | Gap for full compatibility | Effort | Verification against Karma |
|---|---|---|---|---|---|---|---|
| `kma_ao` | pattern, ray query | native; cosine-weighted cone of half angle `coneangle` around nN, unoccluded fraction within `maxdistance`, `traceset`, front-facing (S11 4.3) | `Op::Query` Occlusion (translator 1731-1758, `core/integrator/shading_queries`), estimator matches Karma's measured values (`tests/test_shading_queries.cpp:111`); `traceset` other than `*` warns; **XPU returns 1** (`core/shading/vm.h:162-191`, README:182; Karma XPU lacks AO too) | P | trace sets in queries; XPU queries (WP-E) | S + M | existing analytic tests; Karma probe with a trace set excluding the occluder; XPU vs CPU with the scratchpad `cmp.py` pattern |
| `kma_aov` (1.0) | legacy in-line | id not in Karma 22's registry; Karma rejects the node and drops the surface graph (probe 4) | passes its input through (translator 741-745); writes no AOV (aovs only come from `aov:*`, translator 3325-3347) | S | none for parity (AOV 0 as in Karma); optionally a "legacy node" warning | S | probe 4 (done) |
| `kma_aov::2.0` | convention | `inputs:aov:<name>` on the terminal shader + Material output + RenderVar raw | surfaces: `LowerAovs` (translator 3325-3347) + `hydra/aov.cpp:159-167`; exact vs Karma (probe 4). **Volumes: ignored silently** (`LowerAovs` only runs for surface terminals, translator 3115/3228) | P | volume-shader AOVs (WP-D) | M | pyro sim with fire and scatter on: `aov_fire`/`aov_scatter` planes vs Karma |
| `kma_camerafalloff` | pattern | MaterialX graph (S4): facing ratio of `-ray:direction` and `ray:hitN`, mix of edge/base colour | graph expanded; dot-note ray imports (translator 1648-1677); output type from the nodedef (translator 864-866); `tests/test_hydra_karma.cpp:211` | S | - | - | exact by construction |
| `kma_curvature` | pattern, ray query | native; convex/concave probes on both sides of the surface within `maxdistance` (S11 4.5) | `Op::Query` Curvature (translator 1771-1797); sphere/edge formulas (`test_shading_queries.cpp:158`); Karma's order of scale, bias, clamp was not measured (gain guessed); XPU default 0.5 | P | measure scale/bias; XPU (WP-E) | S + M | Karma sphere sweep of `convexscale`/`convexbias` |
| `kma_deriv2d` | pattern | Karma graph over `kma_stderiv`: inverts the 2x2 Jacobian (S3) | Karma's graph (`nodeDefDatabase.cpp:168-240`) over the native stderiv | S | not verified against Karma | S | plane with analytic input0 = f(s, t), input1 = a rotated/scaled st; compare both outputs |
| `kma_distance` | pattern, ray query | native; uniform cone, mode 0 minimum / mode 1 mean over hits, `normalize`, `objectspace`, `dir` | `Op::Query` Distance (translator 1759-1770); `objectspace` 1 and `traceset` warn; `dir` semantics unmeasured; XPU default | P | object space, trace sets, `dir`; XPU (WP-E) | S + M | Karma probe: scaled object (objectspace), `dir` on the parallel-plates scene of S11 4.4 |
| `kma_hextiled_texture` | texture | native `kma_color_hextiling` / `kma_normalmap_hextiling`; viewport twin `glsl/houlib/GL32/hextiling.func` | mapped onto Nova's MaterialX hex tiling core (translator 1170-1395): same look, **different random tile layout** (`docs/design/M4.md:300-305`) | P | Karma's tile hash and parameter mapping | M (risk: may stay unreproducible) | render Karma vs the GLSL algorithm first; if they agree, port it; then point-filter pixel compare as `probe_flakes.py` |
| `kma_hextiled_triplanar` | texture | Karma graph: three hex-tiled lookups blended by |N|^weightexp | Karma's graph over Nova's hex tiling | P | inherits the tile layout | with the above | same |
| `kma_light_projection` | light-filter pattern | native; uv of the light ray in the light's projection (cone angle or 45 degrees), distance `z`, `valid`; only inside light filter networks (Karma XPU: not supported per S9) | default outputs + warning (probe 2) | M | part of light filters (WP-C) | in WP-C | gel filter whose tint is a ramp of the projection's u, spotlight on a plane, vs Karma |
| `kma_material_properties` | convention | 29 activated properties become `inputs:karma:<p>` on the surface or volume terminal (2.2); geometry properties win (S9) | only `karma:lpetag` (`hydra/material/material.cpp:39-48`); all other `karma:*` inputs skipped without a warning by design (translator 757, `tests/test_hydra_karma.cpp:252-258`); measured -11.4 % miss (probe 3) | P | map to Nova's per-object overrides (section 5.1, WP-A) | M | probe 3 as a husk test; the recipe rows below |
| `kma_melanin` | pattern | Karma graph (eumelanin/pheomelanin absorption, colour = exp(-sigma)) | Karma's graph; formula test (`test_hydra_karma.cpp:666`) | S | - | - | exact by construction |
| `kma_ocio_transform` | colour | native OCIO processor: colour space, display/view and look modes | matrix conversions between spaces whose transform to scene_linear is a matrix; anything else passes through with a warning (translator 1822-1848) | P | non-matrix transforms, display/view, looks (section 5.8) | M | ramp through sRGB->ACEScg, a display/view and a look; per-pixel compare |
| `kma_pcread` | pattern, point cloud | native; reads `.pc` or geometry files, searches up to `maxpoints` within `radius` (per-point radius attribute and scale), blends an attribute with linear/smooth falloff | default outputs + warning (probe 2) | M | point-cloud loading and a search op (section 5.9) | M-L | `.bgeo.sc` with three coloured points over a plane; profile along a line vs Karma per blend mode |
| `kma_rampconst` | pattern | native `kma_rampconst_{color,float}` (and `kma_ramp_*` from the Karma ramp parameter): spline with per-key basis, clamped ends (S11 4.7) | `Ramp()` (translator 1495-1645, 1875-1886); matches Karma's probe (`tests/karma/probe_ramps.py`, `tests/data/karma_ramps.json`, `test_hydra_karma.cpp:341`) | S | - | - | existing |
| `kma_rayimport` (1.0) | ray state | `kma_rayimport_<type>` or a dot-note NodeGraph (2.2) | both forms (translator 1648-1730) | P | as 2.0 | S | as 2.0 |
| `kma_rayimport::2.0` | ray state | native `kma_rayimport_{bool,int,float,vec2,vec3,vec4,mat2,mat3,mat4}`; menu of 27 variables (S8 `karmautils.py:126-165`), XPU list adds P, D, near, far, contrib, element, hdprim, hdinst, hitvelocity, hitmotion | 12 variables: direction, hitdist, hitN, hitNg, both facing ratios, hitP, origin, isshadowray, level, backface, primid (translator 1648-1677); others return the node's input with a warning, as Karma does for unknown names (S11 4.2); matrix signatures do not lower (`test_hydra_karma.cpp:521`) | P | time, objectid, hitP/hitN/hitNg_camera, hitPz, hitN_facing, hitNg_facing, hituv, velocity, motionvector, dPdz, curveattribP/TanV/width, pixelsamples (section 5.6) | S-M | unlit probe per variable (S11 4.2 setup), plus a curve and a moving object |
| `kma_rayswitch` | ray state | native `kma_rayswitch_{float,color}`; ray classes camera 0 ... shadow 6, hidden `shadow` input = shadow-ray opacity (S11 4.1) | 7-way select on RayType (translator 1711-1723); ray-class probe tests (M4.0, `docs/design/M4.md:253`) | S | - | - | existing |
| `kma_roommap` | texture | native `kma_roommap` (+ `kma_room_multimap`): interior mapping in a window frame from Room Frame SOP primvars, cross-shaped atlas, up to 4 slices; viewport twin `glsl/houlib/GL32/roommap.func` | default outputs + warning (probe 2) | M | native lowering in IR (section 5.10) | M | Karma vs Nova on a window quad with `$HFS/houdini/pic/karma/roommap_debug.exr`, several view angles and slice settings |
| `kma_roundededge` | pattern, ray query | native; blends normals of nearby surfaces within `radius`, mode (both/concave/convex), cusp angle, scope | `Op::Query` RoundedEdge (translator 1798-1803); profile fitted to Karma within about 1 degree (`tests/karma/fit_roundededge.py`, `docs/design/M4.md:335`); scope other than `scope:self` warns; **XPU returns the unrounded normal, while Karma XPU supports this node** (S9) | P | scope; XPU (WP-E) | S + M | existing profile fit; XPU vs CPU; scope with two touching objects |
| `kma_stderiv` | pattern | native `kma_stderiv_{float,vector2,vector}`: derivatives with respect to s and t, footprint size | native (translator 1850-1873) | S | not verified against Karma | S | plane with input = a*s + b*t^2; compare derivS/derivT/derivScale with Karma |
| `kma_tangentrotate` | pattern | Karma graph: rotate the tangent by 360*amount about the axis | Karma's graph; test (`test_hydra_karma.cpp:666-683`) | S | - | - | exact by construction |
| `kma_voronoinoise2d` | pattern | native; F1-F3 distances and cell points, 4 metrics, freq/offset | native `Voronoi` op (translator 1805-1820) with MaterialX's jittered cell points: **pattern differs from Karma's** (`docs/design/M4.md:308-310`) | P | Karma's cell-point hash | M (risk: opaque) | sample Karma's p1 on a grid of positions with jitter 0 and 1; fit the hash or document |
| `kma_voronoinoise3d` | pattern | as 2d in 3D | as 2d | P | as 2d | with 2d | as 2d |

### 3.2 Surface terminals (curves, volumes-for-XPU)

| Node | Kind | Karma implementation | Nova today | St | Gap | Effort | Verification |
|---|---|---|---|---|---|---|---|
| `kma_hair` | curve BSDF | native Chiang-style fibre with melanin parameterisation | native Hair model (translator 295-315, 2359, 3145); matches Karma's calibration scenes (`tests/husk/check_hair.py`); `coat` warns (translator 3300-3313) | P | coat (model unmeasured) | M | furnace and six-light scenes of `check_hair.py` with coat 0.5/1 |
| `kma_fur` (1.0) | curve BSDF | exports `kma_fur_2` (2.2); cuticle/cortex/medulla model | rendered as Hair; medulla and second R lobe warn (translator 3314-3323); **the default `medulla_radius` 0.5 makes every default fur warn and look like hair**; `cuticle_reflectance`, `cortex_blendN`, `Ncortex`, `Nmedulla` ignored silently | P | medulla (scattering core), cuticle reflectance, blend normals (WP-G) | L | M9 research scenes (`docs/research/m9/curves-hair.md`) with medulla radius 0/0.5/1 |
| `kma_fur::2.0` | curve BSDF | as 1.0 plus a wetness layer | as 1.0; `wetness*` ignored silently | P | + wetness | L (with 1.0) | as 1.0 plus wetness 0/1 |
| `kma_pyropreview` | volume shader (XPU-oriented) | native simplified pyro: density scale, smoke/absorption/shadow colour, phase, fire from a temperature field with masks, secondary fire, scatter emission; field names as string inputs | default outputs + warning; the volume renders Karma's default volume (probe 2) | M | lower into `VolumeShader` fields (WP-D) | S-M | first confirm Karma CPU renders it on a volume; then the calibration cubes of `test_volume_karma.py` (density, fire on/off, masks) |

### 3.3 Volume shaders

| Node | Kind | Karma implementation | Nova today | St | Gap | Effort | Verification |
|---|---|---|---|---|---|---|---|
| `kma_volume` | volume shader | native: absorption, scattering, emission, two HG lobes, extinction/contribution modifiers from a start level | native `VolumeShader` (translator 2422-2425, 2791-2802); within 0.1-1.8 % of Karma (`tests/houdini/test_volume_karma.py`) | S | its material properties and AOVs: see `kma_material_properties`, `kma_aov::2.0` | - | existing |
| `kma_whitewater` | volume shader | native: a volume plus a glossy reflection whose orientation comes from a `gradient` vector (the recipe feeds the SDF/density gradient, else the hit's geometric normal) | warning; default volume (probe 2) | M | new phase function: HG mixed with a rough microfacet reflection about the gradient (WP-D) | M | slab with a constant gradient field under a distant light: angular response vs Karma; the Whitewater recipe on a FLIP whitewater cache |
| `kma_pyroshader` | volume (NodeGraph) | MaterialX nodes, `kma_ramp_*`, dot notes (`ray:isshadowray`, `ray:level`), field reads, `kma_volume`; `aov_scatter`/`aov_fire` outputs | translates (`tests/houdini/test_pyro_material.py`), renders within 0.1-1.8 % of Karma incl. fire in ramp and blackbody mode (`test_volume_karma.py`) | S | AOV outputs: see `kma_aov::2.0` | - | existing |
| `kma_pyrofirecolor` | NodeGraph block | fire colour from ramp or blackbody | translates (leaves supported) | S | - | - | covered by the pyro tests |
| `kma_pyrofireemission` | NodeGraph block | fire emission with masks | translates | S | - | - | covered |
| `kma_pyroscattercolor` | NodeGraph block | scatter colour (blackbody, ramp, hue/saturation) | translates | S | - | - | covered |
| `kma_pyroscatteremission` | NodeGraph block | scatter emission with masks | translates | S | - | - | covered |
| `kma_pyrosmokecolor` | NodeGraph block | absorption/scattering from density, colour sources, shadow colour (`ray:isshadowray`) | translates | S | - | - | covered |
| `kma_pyrovolumemask` | NodeGraph block | mask ramp | translates | S | - | - | covered |

### 3.4 Light filters

All four: native in Karma (S2 `shaderType kma:lightfilter`), USD typed prims assigned with `light:filters` (2.2), run
by Karma "on each light ray", inputs may be MaterialX networks evaluated per light ray; supported on Karma XPU (S9).
Nova: no `lightFilter` sprim (`hydra/renderDelegate.cpp:56-68`), `light:filters` ignored without a warning (probe 2).

| Node | What it does | St | Gap | Effort | Verification |
|---|---|---|---|---|---|
| `kma_lfilter_attenuation` | replaces the distance falloff: none, physical, half distance H/(H+D), quadratic polynomial 1/(a+bD+cD^2), ramp between near/far | M | WP-C | L (package) | point light over a long plane: profile vs the analytic formulas, then vs Karma |
| `kma_lfilter_barndoor` | four soft-edged flaps sliding into a spotlight's cone, invertible, plus gel-like tint/intensity/exposure/diffuse/specular | M | WP-C | (package) | spotlight on a plane: flap edge positions and soft-edge widths vs Karma |
| `kma_lfilter_gel` | tint, intensity, exposure, diffuse/specular scale, saturation, gamma, brightness, contrast | M | WP-C | (package) | constant gel values on a lit sphere; a gel driven by `kma_light_projection` + noise |
| `kma_lfilter_gobo` | projects a texture through the cone (cone angle below 90 degrees, else 45), wrap/decal with border colour, uv scale/offset, effect, invert, blur | M | WP-C | (package) | gobo of a checker on a plane, uv scale/offset/wrap modes, pixel compare |

### 3.5 Lens shaders

| Node | Kind | Karma implementation | Nova today | St | Gap | Effort | Verification |
|---|---|---|---|---|---|---|---|
| `kma_physicallens` | lens | native `kma_physicallens` as the camera's lens material (CPU and XPU); the VOP also carries a CVEX VflCode for the `karma:camera:lensshader` path | camera `material:binding` ignored silently; the lens Material is translated as a surface and fails with two warnings (probe 2) | M | lens framework + native physical lens (WP-F) | L | checkerboard through OpenCV distortion and tilt/shift (geometric, exact), vignetting profile, bokeh of out-of-focus point lights (circle/polygon/map), rolling shutter on a moving bar, chromatic aberration fringes |
| `kma_physicallenscore` | lens building block | the core lens maths as a VOP for custom CVEX lens builders | - (no CVEX lens path) | M | CVEX lens bridge (WP-F) | M | a CVEX builder lens using it vs Karma |
| `kma_roomlens` | CVEX lens | VFL: orthographic box and slice cameras to bake room-map atlases | ignored | M | CVEX lens bridge, or a small native port | S (after the framework) | bake a box room in both renderers, compare atlases |
| `kma_uvlens` | CVEX lens | VFL: UV-space baking via `uvunwrap()` on objects named `uvlowres`/`uvhighres` (cage, closest surface, UV match) | ignored | M | uv-to-surface lookup (a renderer callback Karma provides to VEX) + lens framework | L | bake a textured mesh's colour into UV space, compare images |

### 3.6 Recipes

| Recipe | What it builds (probe 1) | Nova today | St | Gap | Effort | Verification |
|---|---|---|---|---|---|---|
| Karma Pyro Material | `kma_pyroshader` + 13 field reads + `kma_aov::2.0` `aov_scatter`/`aov_fire`, no active properties | renders within 0.1-1.8 % (tests above); AOV planes stay 0 | P | volume AOVs (WP-D) | M | AOV planes vs Karma |
| Karma Pyro Smoke Material | pyro shader, smoke preset | renders | S | - | - | `test_volume_karma.py` pattern |
| Karma Pyro Fire Material | fire on; `karma:treat_as_lightsource` = Yes and `karma:lightsource_diffusescale` 10 on the `kma_volume`; AOVs | properties ignored silently: fire is not a light and lights the smoke 10x less on diffuse paths than in Karma; AOVs 0 | P | WP-A, WP-D | M | fire sim under no other light: smoke brightness and noise vs Karma |
| Karma Pyro Explosion Material | as Fire plus scatter | as Fire | P | WP-A, WP-D | M | as Fire |
| Karma Cloud Material | anisotropy 0.877, extinction falloff, `karma:volumelimit` computed by a Python expression (32.9 in the probe) | volume limit ignored: Nova renders single scattering (its default 0) where Karma bounces ~33 times | P | WP-A | S (once WP-A exists) | cloud VDB vs Karma at the recipe's limit |
| Karma Baked Cloud Material | scatter emission from a baked `occlusion` field, AOV | renders; AOV 0 | P | WP-D | M | as Pyro |
| Karma Uniform Volume Material | `kma_volume` (scattering 1, tinted by `displayColor`, shadow switch) with `karma:volumeuniform/density/samples` on the volume shader | the bound mesh renders as an opaque Lambert surface (a volume-only material yields Lambert defaults, translator 3069-3079), no warning; measured 0.085 vs Karma's fog 0.262 | M | WP-B | L | probe 3 cube; analytic exp(-density) through a unit cube; Karma Fog Box LOP scene |
| Karma Whitewater Material | `kma_whitewater` fed by `gradient`/`density` fields (`ray:hitNg` fallback), optional uniform volume | warning, default volume | M | WP-D (+ WP-B for the uniform option) | M | whitewater cache vs Karma |
| Karma Texture Controls (preset on MtlX Image) | `inputs:karma_blur`, `karma_width`, `karma_filter`, `karma_lerp` on `ND_image_*` | ignored silently (the translator keeps extra inputs as text, translator 757, but the image lowering never reads them) | M | per-image blur/width, MIP mode (section 5.7) | S-M | a high-frequency texture at blur 0/0.3 and width 1/2/4 vs Karma (point filter, deterministic) |
| Karma MikkT Normal Map (preset on MtlX Normal Map 2.0) | `inputs:karma_mikkt` | MikkT frame from `tangentu`/`signs` (translator 435-456, 2104-2115; `test_hydra_karma.cpp:406`) | S | - | - | existing |
| Create Karma Pyro Material (Pyro Bake Volume SOP recipe) | builds the Karma Pyro Material from the SOP's look settings | as Karma Pyro Material | P | as Pyro | - | - |

Out of scope (not shading): the Data recipes "Optimization AOVs" (Karma Render Settings preset), "ML Train GSplats
from Karma", "Break Material References".

## 4. Reconciling README's "35 of 55" with the 48 VOP types

The "55" are the `kma_*` shader ids of Karma's registry (S2), loaded as nodedefs by `LoadKarmaNodes`
(`hydra/material/nodeDefDatabase.cpp:110-165`). The "35" is the floor checked by the coverage test
(`tests/test_hydra_karma.cpp:511-548`, `CHECK(lowered >= 35)`), which skips a hand-kept deferred list
(`test_hydra_karma.cpp:516-522`). That list is stale: `kma_hair`, `kma_fur`, `kma_fur_2` (M9) and `kma_volume` (M8)
render now. Today's count is **39 of 55 ids**:

| Registry ids (55) | Count | Nova |
|---|---|---|
| ao, curvature, distance, roundededge, color/normalmap hextiling, hextiled_triplanar color/normal, deriv_2d x3, stderiv x3, melanin, ocio_transform, ramp x2, rampconst x2, rayimport bool/int/float/vec2/vec3/vec4, rayswitch x2, tangentrotate, voronoinoise x6 | 35 | lower (coverage test) |
| hair, fur, fur_2, volume | 4 | native models (M8, M9), still listed as deferred in the test |
| whitewater, pyropreview, pcread, roommap, room_multimap, light_projection, lfilter x5, physicallens | 12 | missing |
| pathswitch (no VOP: picks one of 16 strings by an int), rayimport mat2/mat3/mat4 (no ray variable has a matrix type) | 4 | not lowered; pathswitch as a terminal fails cleanly (`tests/test_hydra_materials.cpp:237-240`) |

VOP types with **no** registry id (13 of the 45 in scope): `kma_aov` 1.0 (legacy), `kma_aov::2.0` and
`kma_material_properties` (conventions), `kma_camerafalloff` (MaterialX), `kma_pyroshader` and the six pyro blocks
(NodeGraphs), `kma_physicallenscore`, `kma_roomlens`, `kma_uvlens` (CVEX). VOP types with **several** ids:
rayimport 2.0 (9), deriv2d/stderiv/voronoi (3 each), rampconst (2, plus 2 `kma_ramp_*` from the Karma ramp
parameter), hextiled_texture and triplanar (2 each), rayswitch (2). Both fur versions export `kma_fur_2`.

Suggested README wording: "every Karma shading node except light filters, lens shaders, point clouds, room maps,
whitewater and the XPU pyro preview; 39 of the 55 `kma_*` shader ids of Karma's registry".

## 5. Notes per gap

### 5.1 Karma material properties (WP-A)

Karma's rule (S9): the properties apply to every geometry bound to the material; a geometry property (Render
Geometry Settings) wins. Mapping onto what Nova already has:

| `karma:` input | Nova equivalent today | Action |
|---|---|---|
| `diffuselimit`, `reflectlimit`, `refractlimit`, `ssslimit`, `volumelimit` | per-object `RayLimitOverrides` from `nova:object:*Limit` (`hydra/mesh.cpp:44-48, 852-860`; volumes `hydra/volume.cpp:81`) | map |
| `thinwallatrefractlimit` | `nova:object:thinWallAtRefractLimit` | map |
| `treat_as_lightsource`, `lightsource_diffusescale`, `lightsource_specularscale`, `lightsource_samplingquality` | object primvars (`hydra/primvarUtil.cpp:110-112`, `hydra/volume.cpp:136-137`) for meshes and volumes | map (meshes and volumes) |
| `lpetag` | done (`material.cpp:39-48`) | - |
| `causticsenable`, `causticsroughnessclamp`, `fakecaustics*` | only the bool `nova:object:fakeCaustics`; Karma's per-face factor and colour/opacity/fresnel-darken controls are not modelled | map `causticsenable` = !fakeCaustics now (decision shared with `karma-properties.md`), the controls later |
| `mtlx_imagewidth`, `mtlx_imageblur` | global `nova:textureFilterWidth`/`textureBlur` only (`hydra/renderDelegate.cpp:699-702`) | per-material multipliers on the texture slots (with 5.7) |
| `volumeuniform`, `volumeuniformdensity`, `volumeuniformsamples` | none | WP-B |
| `dielectricpriority` | none (nested dielectrics) | warn once; separate feature |
| `*quality`, `volumesteprate` | no equivalent (no per-lobe splitting; unbiased volume tracking) | document as no-ops (no warning, as the render-property doc decides) |

Implementation sketch: the translator collects `karma:*` inputs of the surface terminal **and** the volume terminal
into a properties block on `HdNovaTranslatedMaterial`; the material sprim publishes it; meshes, curves, points and
volumes resolve per prim at sync: prim primvar (`nova:object:*`, then `karma:object:*`) > material property > render
setting. IPR: a property edit must dirty the bound prims (the registry's material-to-mesh index from M4.6 already
does this for displacement). CPU and XPU: data only, both already consume the per-object overrides. Also warn when a
volume-only material is bound to a mesh without `volumeuniform` (today it silently renders Lambert, translator
3069-3079), and drop hair/fur/volume/pyro from the TAB mask and the coverage test's deferred list.

Risks: precedence must match Karma when both a geometry property and a material property are authored (measure);
`causticsenable` semantics differ in more than the flag (see `karma-properties.md` B1/B2).
Verification: probe 3 as a husk check (ratio 0.886 in Karma); Fire/Explosion/Cloud recipes against Karma.

### 5.2 Mesh-bounded uniform volumes (WP-B)

Karma turns a closed mesh (or points) whose volume shader, or prim, carries `volumeuniform` into a homogeneous
medium: the surface is no longer shaded, its interior is a volume with density `volumeuniformdensity` times the
shader (on XPU only constant primvars). Used by the Uniform Volume and Whitewater recipes and the Karma Fog Box LOP.
Nova: needs media bounded by meshes. Sketch: mark such meshes "medium boundary" (invisible to camera/shadow as
surfaces); entering/leaving is tracked per path (front/back face hits push/pop a medium id, as nested media), free
flights sample a constant majorant (density times the shader's interval bound, the M8 interval executor), shadow
rays do ratio tracking over the intervals found by intersecting the boundary; merge with VDB media in the existing
`MediaWalker` overlap logic. CPU and XPU (OptiX: interval collection by any-hit or iterative closest hits).
Effort L. Risks: open or self-intersecting meshes, camera starting inside a fog box, instancing, motion blur of the
boundary, light-only rays. Verification: analytic transmittance exp(-d) through a unit cube (alpha), probe 3 cube,
a Fog Box LOP scene vs Karma, furnace inside a sphere.

### 5.3 Light filters (WP-C)

Sketch: support the `lightFilter` sprim (UsdImaging delivers its network through
`UsdImagingLightFilterAdapter::GetMaterialResource`; the Karma schemas set `kma:lightFilter:shaderId`), read
`HdTokens->filters` on lights, and compile each filter network with the existing shading compiler into a **filter
program** whose outputs are a colour multiplier plus separate diffuse and specular scales. New globals for filter
programs: the shading point in the light's frame, the distance to the light, the light's cone angle (for
`kma_light_projection`: uv in the projection, z, valid). The natives (gel, gobo, barndoor, attenuation, blocker,
projection) become IR lowerings like other `kma` nodes. Evaluate the program for every light sample (NEE) **and**
when a BSDF-sampled ray hits the light, so MIS stays consistent; apply diffuse/specular scales per lobe class (NEE
already splits per lobe for LPEs, and lights already carry diffuse and specular multipliers, `core/scene/light.h:34`,
`hydra/light.cpp:122`; a filter makes them per shading point). CPU and XPU (the VM runs in the
NEE kernel; gobo textures through the device texture path). The hidden `kma_lfilter_blocker` and the Karma Blocker
Light Filter LOP come for free with the same machinery. Effort L. Risks: cost per shadow ray, light tree importance
(unchanged, still unbiased), light linking and instanced lights, attenuation filters on infinite lights.
Verification: per filter as in 3.4, analytic where possible (attenuation, barndoor geometry), Karma pixel compares.

### 5.4 Lens shaders (WP-F)

Two Karma paths: the **lens material** (`material:binding` on the camera to a Material with `kma_physicallens`,
CPU and XPU) and the **CVEX string** (`karma:camera:lensshader`, Karma CPU only). Sketch: a camera "lens program"
stage in camera-ray generation with inputs (pixel position, sample index/seed, camera parameters, time) and outputs
(P, I, time, tint, clipping range, valid, optionally wavelength), run by the VM on CPU and in the camera-ray kernel
on XPU. `kma_physicallens` as a native lowering into that program (bokeh shapes and maps, tilt/shift, OpenCV
distortion, curvature, vignetting, rolling shutter, f-stop/focus maps, projection modes; chromatic aberration needs
per-channel or spectral sampling: three wavelengths with tint is the cheap form). CVEX lens shaders (`kma_roomlens`,
custom builders with `kma_physicallenscore`) through a new mode of the CVEX bridge on CPU; on XPU either generate
camera rays on the host per pass or fall back with a warning. `kma_uvlens` additionally needs a uv-to-surface lookup
(rasterise each named mesh's UV triangles into a grid) and is really a texture-baking feature. How Karma receives the
camera's material binding through Hydra must be confirmed first (the camera is not a gprim; probe with a scene-index
dump). Effort L (M for the CVEX bridge part). Risks: the native lens may differ from the VFL; adaptive sampling and
the film assume pinhole/thin-lens ray footprints (ray cones). Verification: 3.5.

### 5.5 Volume shading completion (WP-D)

- **Volume AOVs:** `aov:*` inputs on `kma_volume` (pyro recipes). Karma's accumulation rule along a ray (first
  scatter event, transmittance-weighted, or alpha-composited) must be measured first; then volume shaders get AOV
  outputs like surfaces (`LowerAovs` on the volume terminal) and the integrator writes them where Karma does.
- **`kma_whitewater`:** a phase function mixing HG with a rough microfacet reflection about the normalised
  `gradient` (zero gradient: probably HG only - measure); sampling and evaluation in `core/volume/phase.h`, CPU+XPU.
- **`kma_pyropreview`:** lower to `VolumeShader` fields with field reads by the string names; formulas measured on
  cubes like `test_volume_karma.py`.
Effort M overall.

### 5.6 Ray imports (WP-E)

Cheap: `time` (Global::Time), `pixelsamples`, `objectid` (the utility AOV's id as a global), camera-space variants
`hitP_camera`, `hitN_camera`, `hitNg_camera`, `hitPz` (needs the world-to-camera matrix as uniform constants of the
program), `hitN_facing`/`hitNg_facing` (measure: sign flag or clamped dot), `hituv` (the hit's parametric/barycentric
coordinates as a new global). Medium: `velocity`/`motionvector` (Nova already computes them for the motion AOVs,
M6.4), `dPdz` (measure: probably the footprint growth with depth from the ray cone), `curveattribP/TanV/width`
(closest centreline point, tangent and width of the curve hit; curves keep tangent and radius for queries,
`core/shading/query.h` QueryLane). The XPU-only names (P, D, near, far, contrib, element, hdprim, hdinst,
hitvelocity, hitmotion) are commented out in Houdini's menu; add the obvious aliases. Verification: one unlit probe
per variable (S11 4.2), plus a moving object and a curve.

### 5.7 Karma texture controls and material image controls (WP-H, small)

`karma_blur` and `karma_width` map to per-slot OIIO blur and width multipliers (CPU) and the device sampler's
footprint scale (XPU); `karma_lerp` to the MIP mode (none / one level / trilinear / anisotropic); `karma_filter`
(box, gauss, ...) has no OIIO kernel choice: map box/gauss, warn for the rest. `karma:mtlx_imagewidth/imageblur`
(material properties) multiply the same per-slot values. Effort S-M. Verification: 3.6 row.

### 5.8 OCIO transforms (WP-H)

Bake any OCIO processor (space, display/view, look) into a 3D LUT (plus a shaper for HDR ranges) at translation, as
a texture slot, sampled trilinearly on CPU and XPU; keep the exact matrix path. Effort M. Risk: HDR inputs outside
the LUT's range (use a log shaper). Verification: per-pixel compare on a wide-range ramp.

### 5.9 Point clouds (WP-H)

Load `.pc`/`.bgeo(.sc)` through Houdini's own reader (a small bridge DLL in the style of `nova_volume.dll`), build a
k-d tree (CPU) / uniform grid (XPU), and add a `PcRead` op returning the blended attribute (k nearest within the
radius, per-point radius, linear/smooth falloff, blend modes). File per material value (uniform). Effort M-L.
Verification: 3.1 row.

### 5.10 Room maps (WP-H)

Pure IR: transform the shading point and view direction into the window frame (position/tangent/bitangent inputs
from the Room Frame SOP primvars), intersect the parallax box (and up to four slice planes), pick the atlas cell of
the hit face, one `Tex2D` lookup (UDIM offset), fallback colour, optional normal mix; `kma_room_multimap` adds
per-slice files. The viewport GLSL twin is a behavioural lead; Karma's output decides. Effort M. Verification: 3.1 row.

### 5.11 Pattern parity (WP-H)

Hex tiling and Voronoi differ in their random layouts. First check whether Karma's native hex tiling equals the
viewport GLSL (render the same plane in Karma and in the Houdini GL viewport/flipbook); if so, port the hash and
mapping and the deviation disappears; otherwise keep the documented deviation. Voronoi: sample Karma's cell points
at jitter 0/1 on a grid; if the hash is VEX's, Nova's VEX-exact noise work (M3.7) may already have the pieces.

### 5.12 Hair and fur completion (WP-G)

Karma Hair coat, Fur medulla (a scattering core, typically a double-cylinder model), second R lobe, cuticle
reflectance, blend normals and the wetness layer. All unmeasured in M9; needs a measurement round like
`docs/research/m9/curves-hair.md` first. Effort M (coat, wetness) to L (medulla). Risk: Karma's model is unpublished.

### 5.13 Ray queries on XPU and trace sets (WP-E)

The M4 design already describes the split: the query op ends kernel 1 (write query rays), an OptiX launch traces,
kernel 2 resumes (`docs/design/M4.md` notes on M4.4, item 5). Priority on `kma_roundededge` (Karma XPU has it).
Trace sets/scope: filter query rays by the light-linking-style collections Nova already has for light/shadow
linking. Effort M (+S).

## 6. Work packages, ordered by user value

| WP | Content | Nodes / recipes fixed | CPU / XPU | Effort | Why this order |
|---|---|---|---|---|---|
| **A** | Karma material conventions: `karma:*` material properties on surface and volume terminals (5.1), warning for volume-only materials on meshes, `karma:mtlx_image*`; TAB mask, coverage test and README brought up to date | `kma_material_properties`; Pyro Fire, Pyro Explosion, Cloud recipes; every Karma builder material that activates a property | both (data only) | M | silent look differences in Houdini's own recipes and in every builder that uses the properties node; cheap, reuses the per-object override machinery |
| **B** | mesh-bounded uniform volumes (5.2) | Uniform Volume recipe, Whitewater's uniform option, Karma Fog Box LOP | both | L | fog/atmosphere is everyday lighting work; today a fog box renders as an opaque box |
| **C** | light filters + `kma_light_projection` (5.3) | 4 filter VOPs, light projection, hidden blocker, Karma Blocker Light Filter LOP | both | L | standard lighting tools (gobos, barn doors, gels); silently ignored today |
| **D** | volume shading completion (5.5) | `kma_aov::2.0` on volumes, `kma_whitewater`, `kma_pyropreview`; Pyro/Baked Cloud AOVs, Whitewater recipe | both | M | pyro AOVs are routine in compositing; whitewater for FLIP shots |
| **E** | ray state completion and XPU queries (5.6, 5.13) | `kma_rayimport` 1.0/2.0, `kma_ao`, `kma_distance`, `kma_curvature`, `kma_roundededge` on XPU, trace sets | both | M | closes the remaining "partial" utility nodes; rounded edges on XPU match Karma XPU |
| **F** | lens shaders (5.4) | `kma_physicallens`, `kma_physicallenscore`, `kma_roomlens`, `kma_uvlens` | both (CVEX path CPU) | L | camera looks (bokeh, distortion, rolling shutter); fewer users, large work |
| **G** | hair and fur completion (5.12) | `kma_hair` coat, `kma_fur` 1.0/2.0 | both | M-L | fur users see the warning on every default Fur material |
| **H** | pattern parity: texture controls (5.7), OCIO (5.8), point clouds (5.9), room maps (5.10), hex tiling/Voronoi hash (5.11) | Texture Controls recipe, `kma_ocio_transform`, `kma_pcread`, `kma_roommap`, `kma_hextiled_*`, `kma_voronoinoise*` | both | M-L | each node is niche or already looks right; texture controls (S-M) can be pulled into A |

Quick wins inside these: the stale bookkeeping and the volume-only-material warning (A, hours), Cloud's volume limit
(falls out of A), the cheap ray-import variables (E, S), texture controls (H, S-M).

## 7. Hidden and deprecated nodes (not in the counts)

| Node | Status in Houdini | Karma | Nova |
|---|---|---|---|
| `kma_fakecaustics` | hidden, deprecated | NodeGraph that passes the surface through (fake caustics are material properties now) | passes through (`ND_dot_surfaceshader` followed at the terminal, translator 3089-3104; `test_hydra_karma.cpp:241-250`) |
| `kma_nesteddielectrics` | hidden, deprecated | passthrough NodeGraph; priority now `karma:dielectricpriority` | passes through; priority unsupported (5.1) |
| `kma_lfilter_blocker` | hidden VOP (the Karma Blocker Light Filter LOP authors it) | native light filter: shapes with falloff in a space | missing (WP-C) |
