# M11 research: Karma's LOP, ROP and TOP nodes and what Nova needs to be compatible with them

Goal (user): Nova should be "compatible with all Karma nodes". This report takes the node view: every Karma LOP,
ROP (Driver) and TOP node of Houdini 22.0.466, what it puts on the stage, what Nova does with that today, what is
missing, and how to verify a fix. The property view (every `karma:*` render property, one by one) is the companion
report `docs/research/m11/karma-properties.md`; this report refers to its work packages (WP-A ... WP-G) instead of
repeating property tables. No code was changed.

## 0. Summary

Inventory (from `karma_nodes.json`, hidden and deprecated types skipped): **19 node types in 30 versions**:
15 LOP types (22 versions), the Karma ROP (Driver), 2 TOP types (3 versions), 1 SOP type (4 versions), plus the
Karma (Setup) tool and the Karma parts of 7 generic LOPs (Render Settings, Render Product, Render Var, Render
Geometry Settings, Light, Camera, Light Filter Library).

Status of the 19 types plus the Setup tool in Nova today:

| Status | Count | Nodes |
|---|---|---|
| works | 1 | Karma Cryptomatte |
| works except details | 2 | Karma Standard Render Vars (per-var pixel filters ignored), Karma Render Products (raster only) |
| partial | 7 | Karma Render Settings (14 of its 161 properties read), Karma Physical Sky (sun only), Labs Karma, Labs Karma AOVs for RenderMan Denoiser, Labs ML CV Synthetics Karma ROP, Labs Quick Material (not verified end to end), Karma (Setup) |
| renders wrong or nothing | 7 | Karma Sky Dome Light (no light), Karma Sky Atmosphere (ignored), Karma Fog Box (opaque box), Karma Shadow Catcher (visible ground), Karma Background Plate (visible ground), Karma Blocker Light Filter (ignored), Karma Texture Baker (camera render instead of a bake) |
| cannot use Nova | 3 | Karma ROP (Driver), ROP Karma Render (TOP), Labs Karma Render (TOP): the renderer is fixed inside a locked asset |

Main findings (each measured or read back from the stage; section 2 has the numbers):

1. **Two Karma node families reach Nova as Hydra prim types Nova does not register.** The Karma Sky Dome Light
   arrives as the generic `light` sprim and every Karma light filter as a `lightFilter` sprim (Houdini's debug
   delegate, section 1.4). Nova supports neither, so a scene lit only by the Karma Sky Dome Light renders black
   in Nova, and a blocker that blacks out Karma's whole frame changes nothing in Nova.
2. **Holdouts and per-ray visibility are missing.** The Shadow Catcher and the Background Plate work through
   `primvars:karma:object:holdoutmode` (2, "Background") and `primvars:karma:object:rendervisibility`; the Karma ROP's
   matte and phantom objects use the same primvars. Nova renders such a ground as a normal opaque surface and a
   matte or phantom object as a normal object. Holdout LPEs (`holdouts;...`, which the Background Plate authors as two
   render vars) render black with a warning.
3. **The Fog Box covers the frame in Nova.** It binds a `kma_volume` material to a closed Cube (or Sphere) and marks
   it with `primvars:karma:object:volumeuniform`. Nova has no volume-in-a-surface path and renders the cube as an
   opaque surface (alpha 1 everywhere in the probe).
4. **Renderer selection.** The Karma Render Settings LOP's Rendering Engine menu authors `husk:default_delegate`
   (`BRAY_HdKarma` or `BRAY_HdKarmaXPU`). A USD Render ROP left at "default delegate" (what Karma (Setup) creates)
   therefore renders with Karma even when the viewport shows Nova. The Karma ROP, the ROP Karma Render TOP and the
   Labs Karma nodes pick the renderer inside locked assets (an expression over cpu/xpu or a fixed
   `BRAY_HdKarma`), so they can never render with Nova. Stock, renderer-neutral routes exist and list Nova: the USD
   Render ROP (LOP and Driver) and the USD Render File/Scene TOPs.
5. **What Karma nodes author that Nova already handles:** render products and vars (LPE and utility AOVs, M6),
   cryptomattes (M6.5), the sun of the Physical Sky (a normal DistantLight with a computed colour), the denoiser
   entry of `karma:global:imagefilter` (M7), the five global ray limits, light sampling mode and velocity-blur
   defaults. Everything else on the Karma Render Settings prim is ignored silently (companion report, WP-A).
6. **Easy wins outside the nodes themselves:** Karma applies the camera's USD exposure and `karma:camera:tint` and
   honours `inputs:shadow:enable`; Nova ignores all three (measured; same result as the companion report).

Work packages ordered by user value (section 5): K1 settings and camera fallbacks (= companion WP-A/WP-B),
K2 holdouts and ray visibility (Shadow Catcher, Background Plate, matte/phantom), K3 the Karma physical sky dome,
K4 light filters, K5 renderer selection (Nova ROP, TOP route, "switch to Nova"), K6 volumes bounded by surfaces
(Fog Box), K7 AOV pixel filters, deep and checkpoint products, K8 lens shaders and the remaining light controls,
K9 the sky atmosphere, K10 texture baking.

## 1. Method

### 1.1 Sources

- Node list: `karma_nodes.json` (an isolated hython run over all node type categories; the scratch file
  `C:\Users\Synti\AppData\Local\Temp\claude\d9a85726f5812f18\130aaf07-bd8e-41b6-877e-893f27509a27\scratchpad\m11\karma_nodes.json`).
  Skipped as hidden or deprecated: the old Karma LOP (`karma`), `karmarenderproperties` (v1), Karma Ocean, Karma
  Procedural, Karma Toon Shading, Brick Map Generator. Not covered: the `kma_*` VOPs (see
  `docs/research/m4/karma-nodes.md`) except the light-filter and lens VOPs, which belong to K4 and K8, and the
  "Data" recipes (they create ordinary nodes, e.g. Karma pyro and cloud materials, covered by M8).
- Houdini files: `$HFS/houdini/toolbar/ExtraLopTools.shelf` (Karma (Setup), Karma Texture Baker tools),
  `$HFS/houdini/soho/parameters/BRAY_HdKarma_*.ds` (the Karma tabs of the generic LOPs),
  `$HFS/houdini/dso/usd_plugins/usdKarma/resources/{plugInfo.json,generatedSchema.usda}` (Karma's USD schemas),
  `$HFS/houdini/dso/usd_plugins/hdHoudini/resources/plugInfo.json` (Houdini's imaging adapters for the Karma sky prims).
- Nova: `hydra/` and `core/` (searches for every authored name, then reading the hits), `houdini/package/`.

### 1.2 What each node authors

An isolated hython (`tests/houdini/run_hython.ps1`, own preference directory, no user packages, no installed Nova)
built a small base stage (Sphere LOP, Cube LOP, Camera LOP, Distant Light LOP), appended each Karma LOP with default
parameters and compared the composed output stage with the input stage: new and changed prims, types, applied API
schemas, attributes (value, interpolation, connections, time samples), relationships and stage metadata. Each output
stage was also exported as USDA. Nodes that author nothing useful at defaults were run again with a target set
(Shadow Catcher and Background Plate on the ground, Physical Sky in atmosphere mode, Texture Baker on a mesh with
UVs, Light Filter Library with all Karma filter VOPs). The generic LOPs were created with every `_control` menu of
their Karma tab set to "set", so the dump shows everything the tab can author.

Scripts and outputs (scratch directory `...\scratchpad\m11\`, same path as above): `usd_diff.py`, `dump_lops.py`
(+ `variants1.json`), `dump_misc.py`, `light_all.py`, `labs_check2.py`, `v1check.py`, `tops.py`, `qm_sceneimport.py`, `rop_matte.py`,
results in `out\lops.json`,
`out\variants_variants1.json`, `out\misc.json`, the stages in `out\layers\*.usda`.

### 1.3 What Nova does with it

`render_probes.py` renders probe scenes built by `make_probes.py` (sphere on a ground box, camera, distant light,
then one Karma node or one property) with husk in an isolated environment (`refcommon.IsolatedHoudini`), once with
the staged Nova (`build/vs2022/stage/Release/nova`, `HdNovaRendererPlugin`) and once with Karma CPU (`BRAY_HdKarma`),
320x180, 16 samples, uniform oracle. Statistics are region means (top rows, sphere centre, bottom rows, lower left).
**Each renderer is compared with its own base render**: unbound geometry gets each renderer's fallback material
(known open point Q1 of M3), so Karma's base is brighter than Nova's and absolute values are not comparable.

### 1.4 Which Hydra prims arrive

Houdini ships a debug delegate (`HdHDebug`, enabled by `HUSD_HDEBUG_DELEGATE`). With
`HUSD_HDEBUG_DELEGATE=sprim=on,rprim=on,bprim=on,image=off,memory=off,material=off` and
`husk --renderer HdHDebug` it prints every prim Hydra creates with its type (`hdebug.py`). This shows what any
third-party delegate receives for the Karma nodes. (It crashes on exit after printing; harmless for this use.)

## 2. Probe results

| Probe (scene) | Karma CPU (vs its base) | Nova (vs its base) |
|---|---|---|
| Karma Sky Dome Light only | blue sky light, mean RGB (0.020, 0.032, 0.051) | **black** (0, 0, 0): Hydra hands it over as a `light` sprim, which Nova does not register |
| Karma Physical Sky (sun + sky dome) | sky visible to the camera (top rows alpha 1, RGB 0.083/0.075/0.073), bluish fill on the sphere | sun only: top rows alpha 0.42, sphere centre RGB 0.0026/0.0021/0.0015 (shadow side unlit) |
| Physical Sky in atmosphere mode | the `KarmaSkyAtmosphere` prim is used as a global "atmospheric" (Karma's log names it); HdHDebug receives no prim for it | nothing from the atmosphere (only the extra sun) |
| Karma Fog Box (density 0.5, scale 3) | fog: top rows alpha 0.84, sphere visible through it | **opaque grey box**: alpha 1.0 everywhere, flat RGB 0.035 |
| Karma Shadow Catcher on the ground | ground cut out: alpha 0 where unshadowed, 0.37 in the bottom rows (shadow), RGB about 0 | ground rendered as a white diffuse surface, alpha 1, RGB 0.081 |
| Karma Background Plate (v2) on the ground | the same ground holdout as the Shadow Catcher | ground opaque; warnings: transform spaces `space:current`/`space:world` unsupported, `Mandril.pic` unreadable |
| Karma Blocker Light Filter over the sun | whole frame black (alpha unchanged) | unchanged (identical to base) |
| `holdoutmode` 1 (Matte) on the sphere | sphere alpha 0 and black, its shadow kept | unchanged |
| `rendervisibility` "-primary" on the sphere | sphere gone for the camera, shadow kept | unchanged |
| camera `exposure` 1 and `karma:camera:tint` (1, 0.5, 0.25) | x(2.0, 1.0, 0.5) per channel | unchanged |
| light `inputs:shadow:enable` false | ground brightens 0.053 -> 0.075 (no shadow) | unchanged |
| Karma Render Settings with Primary Samples 4 | as set | **64 spp** (EXR `samples` 64): Nova's own default; Karma's sample counts are not read |

"Unchanged" means the region statistics equal Nova's base render to four decimals.

## 3. Inventory

Effort: the companion report's scale, per device: **S** up to a day, **M** one to three days, **L** more (rough
size in brackets). CPU work in `core/integrator` needs its twin in `core/gpu` (XPU); Hydra-only work reaches both.

### 3.1 LOP nodes

| Node (type, versions) | What it authors (evidence: section 1.2 dumps) | Nova today | Gap | Effort | Verification idea |
|---|---|---|---|---|---|
| **Karma Render Settings** (`karmarendersettings`; the Setup tool's `karmarenderproperties::2.0` resolves to it) | `/Render/rendersettings` RenderSettings with KarmaRenderSettingsAPI, HuskRenderSettingsAPI, HuskRenderProductAPI; 87 Karma values at defaults (57 `karma:global:*`, 30 `karma:object:*`; 161 with every control set); `husk:default_delegate` = `BRAY_HdKarma`, or `BRAY_HdKarmaXPU` for engine xpu; camera, resolution, conform policy, purposes, DOF/motion-blur switches; a raster product with a `C.*[LO]` beauty var (`karma:filter` ubox); stage metadata `renderSettingsPrimPath` | Rendered as a normal settings prim (camera, resolution, products, vars, `disableMotionBlur`, `disableDepthOfField`). Reads 14 Karma names: the 5 ray limits, `light_sampling_mode`, the denoise entry of `imagefilter`, `vblur`, `instance_vblur`, `geosamples`, `instance_samples`, `curve_style`, `point_style`, `width_scale` (the last three fail on the LOP's menu-label tokens, companion report finding 5). Ignores samples, oracle, pixel filter, seed, colour limits, caustics mode, dicing, texture filtering, ... (probe: 64 spp instead of 4) | Fallback reads with Karma's defaults; a stated mapping for Karma's two convergence modes onto Nova's uniform/variance sampling; ignored-but-image-changing settings should warn once | M (Hydra; WP-A/WP-C of the companion report) | Per mapped setting: a Karma Render Settings LOP with the value changed, husk + Nova, check EXR metadata (spp, filter) and the image against Nova's base; `tests/houdini` case that builds the LOP and checks the delegate's resolved settings |
| **Karma Render Products** (`karmarenderproducts` 1.0, 2.0) | `/Render/Products/<name>` RenderProduct (HuskRenderProductAPI) with camera, productName (time-sampled), productType raster; ordered vars from the vars scope. Deep output is a separate product type (`karma:deep`) with the KarmaRenderProductAPI `karma:product:dcm*` options (seen on the Render Product LOP's Karma tab; the Karma ROP and Labs Karma have a Deep Camera Map option) | Raster products work (husk writes them) | Deep camera maps (`karma:deep`); checkpoint/resume products (`karma:checkpoint`) | deep L (1-2 weeks), checkpoint M | husk writes a DCM that `hoiiotool --info` reads as deep; checkpoint: interrupt, resume, compare with an uninterrupted render |
| **Karma Standard Render Vars** (`karmastandardrendervars` 1.0, 2.0) | RenderVars under `/Render/Products/Vars` with KarmaRenderVarAPI + HuskRenderVarAPI: LPE components (`C<RD>.*L`, `CV.*L`, ...), raw `ray:*` utility sources, primvar `st`; per var `driver:parameters:aov:karma:filter` (`ubox`, `minmax` zmin, idcover) | All standard components and utilities render (M6, Nova's own LOP uses this node inside) | `karma:filter` per var ignored (first hit at the pixel centre, M6 D9); a few raw sources (`ray:mask`, `ray:hitstack`, `ray:time`, `ray:near/far`, ...) | S-M (filters), S each source | Compare depth (zmin) and element (idcover) planes with Karma's at object edges |
| **Karma Cryptomatte** (`karmacryptomatte`) | A RenderVar per layer: source `__name`/`__materialname`/primvar, `karma:cryptomatte` on, rank, sidecar | Supported (M6.5) | Per-instance primvar cryptomattes (M6 deferral) | - | existing reference `aov_components`/crypto tests |
| **Karma Physical Sky** (`karmaphysicalsky`) | An Xform with a DistantLight `sun` (colour, intensity, angle and direction computed from solar altitude/azimuth or date, place and time; normalize on; `karma:light:renderlightgeo` on) and either a `KarmaSkyDomeLight` `sky` (sky mode) or a `KarmaSkyAtmosphere` (atmosphere mode); internal network uses the Sky Dome Light and Sky Atmosphere LOPs | The sun renders (ordinary DistantLight); the sky part is missing (see the next two rows) | Sky dome (K3) or atmosphere (K9) | as the rows below | Karma vs Nova at solar altitudes 5/30/60 degrees, turbidity 2/3/6: sky pixels and the irradiance on an upward ground |
| **Karma Sky Dome Light** (`karmaskydomelight`) | A `KarmaSkyDomeLight` prim (Karma schema, base UsdLuxNonboundableLightBase): the usual light inputs plus `solar_altitude`, `solar_azimuth`, `turbidity`, `horizon_blur`, `ground_albedo`, `ground_color` (`computegroundcolor`); Houdini's own adapter turns it into a generic `light` sprim (HdHDebug) | Not a registered sprim type: the light does not exist (probe: black) | Register `light`, recognise the Karma sky by its parameters and implement the sky model as an environment light (K3) | M (1 week incl. XPU) | As above; plus energy check: a white Lambert ground under sky only, irradiance vs Karma within 2 % |
| **Karma Sky Atmosphere** (`karmaskyatmosphere`) | A `KarmaSkyAtmosphere` gprim (Karma schema, base UsdGeomGprim) with 25 constant primvars: Rayleigh and Mie coefficients and scale heights, Mie phase, an absorption layer (altitude, width), planet centre and sea level (metres, scaled by a unit factor), ground albedo/visibility/offset/shadowing, step sizes, LUT size, colour space, IPR alpha, camera clipping | Ignored (Houdini's gprim adapter emits a type Nova does not register; Karma treats it as a global atmosphere and accepts only one) | A planetary atmosphere medium with its own light transport (K9) | L (3+ weeks) | Karma vs Nova sunset/zenith gradients and sun transmittance at several altitudes; aerial perspective on distant geometry |
| **Karma Fog Box** (`karmafogbox`) | Xform with a guide BasisCurves outline and a render Cube (or Sphere or a custom SOP shape) bound to a `kma_volume` material (MaterialX graph: density from a `density` geomprop, optional `fractal3d` noise in `ray:hitP` space, a separate density for shadow rays via `ray:isshadowray`); primvars `karma:object:volumeuniform` 1, `volumeuniformdensity`, `volumesteprate` 0.02 | The cube renders as an opaque surface with Nova's default material and hides the scene (probe: alpha 1 everywhere) | Volume shaders on closed surfaces, uniform-density media inside a gprim (K6; M8's open decision "mesh-bounded volumes") | M-L (1-2 weeks incl. XPU) | Homogeneous absorbing box: transmittance exp(-sigma d) exact; scattering box vs Karma; noise variant vs Karma |
| **Karma Shadow Catcher** (`shadowcatcher`) | On the target prims (default pattern: the last modified prims): `material:binding` to a new Standard Surface material (specular 0, `kma` context), `primvars:karma:object:holdoutmode` 2 (Background), `primvars:karma:object:rendervisibility` "primary" (with "block lights": "primary\|shadow") | Holdout and visibility primvars ignored: the ground renders lit, opaque, white | Holdout modes with shadow alpha, ray-type visibility (K2) | M+M (1-2 weeks) | Analytic: alpha in the shadow = 1 - shadowed/unshadowed irradiance; compare alpha and beauty with Karma |
| **Karma Background Plate** (`backgroundplate` 1.0, 2.0) | On the target prims: binding to a `background` material (Standard Surface whose colour is the plate image looked up in screen space, built from `ray:direction` and `ray:level` imports and a `transformvector` from `space:current` to `space:world`; plus an optional, rotatable off-screen environment texture), `holdoutmode` 2; always two RenderVars `holdout_shadow` (`holdouts;shadow;CDL`) and `holdout_diffuse` (`holdouts;CD.+L`) | Holdout ignored, holdout LPEs black with a warning, the two transform spaces unsupported (warning), `.pic` plate unreadable (warning) | K2 plus: `transformvector` spaces in the material translator, Houdini `.pic` images in the texture system | M (after K2) | Plate on a ground: reflections of the plate in a mirror sphere match Karma; holdout AOVs vs Karma |
| **Karma Blocker Light Filter** (`karmablockerlightfilter`) | A `KarmaBlockerLightFilter` prim (base UsdLuxLightFilter): shape, radius/width/height/depth, rounded corner, falloff, intensity (0 = full block), exposure, tint, invert, a `space` path; `lightFilter:shaderId` and `kma:lightFilter:shaderId` = `kma_lfilter_blocker`; a `filterLink` collection. Lights point at it with `light:filters`. Hydra: a `lightFilter` sprim | Ignored (probe: Karma black, Nova unchanged) | Light-filter framework + blocker (K4) | M (framework) + S | Blocker over half a rect light: lit fraction exact; falloff profile along a line vs Karma |
| **Light Filter Library** (`lightfilterlibrary`, generic; with the `kma_lfilter_*` VOPs) | One typed prim per filter VOP under `/lightFilters/`: `KarmaGoboLightFilter`, `KarmaBarndoorLightFilter`, `KarmaGelLightFilter`, `KarmaAttenuationLightFilter`, `KarmaBlockerLightFilter` (only non-default inputs authored, e.g. the attenuation ramp keys, values and basis), and `light:filters` on the lights it assigns to | Ignored | Gobo (texture through the light), barn doors (four edges with softness), gel (colour grading), attenuation (near/far, polynomial or ramp) (K4) | S-M each after the framework | Per filter: an analytic pattern (gobo checker, barn-door edge position) and a Karma comparison |
| **Karma Texture Baker** (`karmatexturebaker`) | `/Render/karmabake_<UDIM>` RenderSettings with about 50 `karma:global:bake_*` values (mode, low/high/cage mesh, UV set, UDIM, ray bias and max distance, per-map switches and parameters: tangent normal, alpha, AO, curvature, cavity, thickness, edges, rounded normals, position box, shader exports), a generated camera `/__HoudiniTextureBakeCamera`, one product per map and UDIM (png by default), raw vars `Nt`, `Af`, ..., an image filter that remaps normals; clears the low mesh's `rendervisibility`; no `husk:default_delegate` | Renders an ordinary camera image from the bake camera; the raw vars become empty shader AOVs | A UV-space bake mode in the integrator and the bake maps (K10) | L (3+ weeks) | Bake AO and tangent normals of the rubber toy with Karma and Nova; per-texel comparison |
| **Labs Karma** (`labs::karma` 1.0, 2.0) | An all-in-one node: the v1 Karma render properties node (`karma:global:engine` string, colour limit 10, ...), a raster product plus a `karma:checkpoint` product, Karma standard vars, an inner USD Render ROP with renderer fixed to `BRAY_HdKarma`, a TOP net (Labs Karma Render) for caching and wedging | The stage renders in Nova where Nova is chosen (viewport, own ROP), with the settings subset of the first row; its buttons always render with Karma | Renderer is fixed inside the asset; checkpoints (row 2) | S (document the route) | - |
| **Labs Karma AOVs for RenderMan Denoiser** (`labs::karma_aovs_for_prman_denoiser` 1.0) | About 20 RenderVars for RenderMan's denoiser: beauty and diffuse/specular LPEs, `export_basecolor` albedo, `ray:hitN`, `ray:motionvector` forward/backward, `ray:hitPz`, `ray:mask` alpha, `ray:pixelsamples`, and variance/MSE copies through `karma:filter` stats var/mse | Most planes render (LPE, albedo, N, motion vectors, depth, sample count); `ray:mask` is unsupported; the variance/MSE planes are plain copies | Per-var sample statistics, `ray:mask` (K7) | M | Compare the variance planes with the sample variance Nova's adaptive oracle already measures |
| **Labs ML CV Synthetics Karma ROP** (`labs::ml_cv_synthetics_karma_rop` 1.0, 1.1) | Background Plate, Karma render settings, a product with Beauty and an `ID` var (primvar `instance_id`), two inner USD Render ROPs whose renderer is `BRAY_HdKarma` (+XPU by engine) | Stage partly renders (primvar AOV supported); plate needs K2; buttons always Karma | K2, renderer fixed inside | S after K2 | - |

### 3.2 ROP, TOP, SOP and the Setup tool

| Node | What it is | Nova today | Gap | Effort | Verification idea |
|---|---|---|---|---|---|
| **Karma ROP** (Driver `karma`) | An `/out` ROP for `/obj` scenes: an inner LOP network (Scene Import for objects, lights, camera; candidate/forced/excluded objects and lights; matte objects get `holdoutmode` Matte and phantom objects `rendervisibility` "-primary" through two Render Geometry Settings nodes (read back, `rop_matte.py`); a Karma Render Settings node) and a locked inner USD Render ROP whose renderer is an expression choosing `BRAY_HdKarma` or `BRAY_HdKarmaXPU` from the engine menu (cpu/xpu only) | Cannot render with Nova. Route today: a Driver "USD Render" ROP pointing at a LOP network with Scene Import + Nova Render Settings (its renderer menu lists Nova) | A **Nova ROP** with the same object/light selection, matte/phantom (needs K2) and Nova's settings (K5) | M | hython: build `/obj` scene, render with the Nova ROP, EXR carries Nova metadata; matte/phantom objects as in Karma |
| **ROP Karma Render** (TOP `ropkarma`) | A ROP Fetch over an inner Karma ROP (engine linked to the TOP's) | Cannot render with Nova. Route today: the **USD Render File** TOP (`usdrender`) and **USD Render Scene** TOP list Nova CPU and Nova XPU in their renderer menu (checked with the staged package) | Optional Nova TOP wrapping the Nova ROP (K5) | S | TOP cook renders one frame per work item with Nova |
| **Labs Karma Render** (TOP `labs::ropkarma` 1.0, 2.0) | Cooks a Labs Karma LOP given by its LOP path | Karma only | as above | S | - |
| **Labs Quick Material** (SOP `labs::quickmaterial` 1.0-2.2) | Builds a local material network with VEX Principled Shader, MatCap and Labs PBR (VEX) shaders and assigns it with `shop_materialpath` plus a `material_override` JSON string per primitive. Through SOP Import only the primvars arrive (no material, no binding: checked) | Principled renders natively (M5.1), other VEX shaders through the CVEX bridge (M5) once the material reaches the stage (Scene Import, Karma-ROP-style import); `material_override` is not a USD concept and is ignored | End-to-end check not done (the Scene Import probe in hython imported nothing; needs a GUI-free setup with a display flag that cooks) | S | hython test: Scene Import of a Quick Material object, Nova vs Karma |
| **Karma (Setup)** (shelf tool) | Creates Karma Render Settings, a Render Pass that points at it, and a USD Render ROP for the pass (renderer left at "default delegate"; instant shutter linked to the settings' motion blur switch) | The ROP renders with `husk:default_delegate` = Karma; switching its renderer menu to Nova works (Nova then reads the subset of the first table row). Nova ships the twin **Nova (Setup)** (Nova Render Settings, Render Pass, USD Render ROP set to Nova) | A one-click "switch this setup to Nova" (K5) | S | hython: run the tool, switch, render with husk, check the EXR's renderer |

### 3.3 Karma tabs of the generic LOPs (counts; details in the companion report)

Read back with every control set (`dump_misc.py`, `light_all.py`):

| LOP tab | Karma names authored | Read by Nova | Notes |
|---|---|---|---|
| Render Settings (Karma tab = the Karma Render Settings properties) | 161 | 14 | as row 1 of 3.1 |
| Render Geometry Settings (Karma tab, `primvars:karma:object:*`) | 82 | 16 | `lpetag`, emission-as-light (4), curve/point style, width scale, velocity blur (4), and on volumes only `volumelimit`, `volumefilter`(+width), `volumevblurscale`. Not read: per-object ray limits on surfaces, `holdoutmode`, `rendervisibility`, `cull_backface`, `causticsenable`, dicing, portals, outlines, ... (companion WP-A/E/F) |
| Light LOPs (`inputs:karma:light:*`, KarmaLightAPI) | 18 | 7 | read: `renderlightgeo`, `singlesided`, `recttextureflip`, `lpetag`, `samplingquality`, `samplingmode`, `equiangular`. Not: `activeradius`, `contribs`, `contributescaustics`, `hdri_max_isize`, `illumbackground`, `lightgeocastsshadow`, `misbias`, `pointradius`, `portalmisbias`, `shader` (light shader, e.g. the projection VOP), `spread` |
| Light LOPs, generic UsdLux the Light LOP authors | ShadowAPI (5), `light:filters` | 0 | `shadow:enable`/`color`/`distance`/`falloff`/`falloffGamma` ignored (probe); filters K4 |
| Portal Light, Geometry Light LOPs | PortalLight prim; MeshLightAPI | 0 | Nova registers neither `portalLight` nor `meshLight`; Nova's emissive meshes go through `treat_as_lightsource` instead |
| Camera LOP (KarmaCameraAPI) | 5 + USD exposure (5) | 0 | `karma:camera:lensshader`/`use_lensshader`/`lensshadervop` (the Karma Lens Material tool binds a lens material, e.g. `kma_physicallens`, and writes its path here), `tint`, `window`; USD `exposure*` (probe: Karma x2 per stop) |
| Render Product (KarmaRenderProductAPI) | 3 (`dcm*`) | 0 | deep output |
| Render Var (KarmaRenderVarAPI) | 7 | 3 | cryptomatte, rank, sidecar read; `filter`, `dither`, `defaultvalue`, `utilitypathexpression` not |

## 4. Renderer selection: what the engine menus do and how Nova gets in

- **Karma Render Settings LOP:** the Rendering Engine menu (cpu/xpu) authors only `husk:default_delegate`
  (`BRAY_HdKarma` / `BRAY_HdKarmaXPU`; read back from the stage). The viewport ignores it (the viewer's renderer
  menu decides), so a user sees Nova in the viewport but gets Karma from a USD Render ROP that is left at "default
  delegate". With the ROP's renderer set to Nova or Nova XPU (the menu lists them because Nova's
  `UsdRenderers.json` has `aovsupport`, docs/research/m4/render-settings-ui.md 0), husk gets `--renderer` and the
  engine menu no longer matters.
- **Nova's own nodes** already author `husk:default_delegate` = `HdNovaRendererPlugin` or `HdNovaXpuRendererPlugin`
  (`houdini/tools/build_nova_hdas.py` `delegate()`), and Nova (Setup) sets the ROP's renderer.
- **Locked assets:** Karma ROP and ROP Karma Render (renderer expression over cpu/xpu), Labs Karma
  (`BRAY_HdKarma` as a fixed string), Labs ML CV (expression), Labs Karma Render (cooks Labs Karma). Changing them
  would mean editing SideFX assets; Nova should offer its own equivalents instead (K5).
- **Renderer-neutral stock routes that work today:** USD Render ROP (LOP), USD Render ROP (Driver `usdrender`, with a
  LOP path), USD Render File / USD Render Scene TOPs. The USD Render File TOP listed Nova CPU and Nova XPU with the
  staged package (`tops.py`); the two ROPs build the same renderer menu (`default_delegate` plus the delegates with
  `aovsupport`, read back from both ROP types), and the LOP ROP's listing of Nova was verified in M4.

## 5. Per-gap notes: what the renderer must implement

### K1. Karma settings and camera fallbacks
Covered property by property in the companion report (WP-A, WP-B, part of WP-C). Node-level additions:
- The current Karma Render Settings node is `karmarendersettings` (the Setup tool's `karmarenderproperties::2.0`
  resolves to it). `hou.Node.createNode("karmarenderproperties")` creates the hidden, deprecated v1, which Labs Karma
  still uses. Their defaults differ (read back with `v1check.py`): colour limit and indirect colour limit 20 in the
  current node, 10 in v1; v1 writes `karma:global:engine` "cpu" and `karma:global:pixelfiltersize` 2 and no
  `husk:default_delegate`; the current node writes `husk:default_delegate` and no `pixelfiltersize`. The companion
  report's section 1 dump used the v1 type name, so its "LOP default" values (colour limit 10, the unregistered
  `pixelfiltersize`) describe v1; check them against the current node before implementing WP-A/WP-C.
- Warn once per stage for Karma properties that change the image and are not honoured (today they are silent),
  so users know when a Karma setup will look different.

### K2. Holdouts, background objects and ray-type visibility (Shadow Catcher, Background Plate, matte/phantom)
- **Data:** constant primvars `karma:object:holdoutmode` (0 none, 1 Matte, 2 Background; ints on prims, the menu label
  on the render settings) and `karma:object:rendervisibility` (a small expression over ray types: `primary`,
  `shadow`, `diffuse`, `reflect`, `refract`, `volume`, `sss`, with `-` to exclude, `&`/`|`, `*` for all, empty for
  none). Hydra delivers them as ordinary primvars, so this is Hydra parsing plus core flags.
- **Visibility:** a per-mesh ray-class mask tested in the hit filters (Embree argument filters exist since M3.0;
  the per-mesh filter flags since M3.11) and with OptiX instance visibility masks or the any-hit program on XPU. The
  camera bit also governs first-hit AOVs and cryptomattes. Shadow-only objects (`primary|shadow` for "block
  lights") need the shadow bit separately from the camera bit.
- **Matte (1):** the object occludes and casts shadows but camera hits on it give colour 0 and alpha 0; measured in
  Karma (probe). What secondary rays see on a matte object must be measured before implementing.
- **Background (2):** the Shadow Catcher case: camera hits give alpha = how much light the object loses to shadows
  (probe: 0.37 in the shadowed rows, 0 elsewhere) and no colour of its own, while the object still bounces light.
  The holdout LPE prefix (`holdouts;` / `holdout;`), today black with a warning (`renderDelegate.cpp:215`,
  `core/film/lpe_compile.cpp:297`), must produce the shadow and diffuse-on-holdout planes the Background Plate
  authors. Related render settings (`use_background`, `bgoccludeshadow`, `background_iprfilter`) and the lights'
  `illumbackground` decide which lights cast onto background objects; measure each in Karma first.
- **Background Plate extras:** the translator must handle `ND_transformvector_vector3` with `space:current` ->
  `space:world` (the plate projection uses it), and the texture system must read Houdini `.pic` (and other Houdini
  image formats Karma reads through Houdini's image library); `kma_rayimport` `ray:direction`/`ray:level` are already
  in Nova (M4.0).
- Unlocks: Shadow Catcher, Background Plate (both versions), Labs ML CV, Karma ROP matte/phantom objects, the
  Render Geometry Settings visibility controls.

### K3. Karma physical sky dome
- Register the generic `light` sprim (`HdPrimTypeTokens->light`) and, when its parameters carry the Karma sky
  inputs (`solar_altitude`, `turbidity`, ... ; Houdini's adapter also knows an `isKMAskyDomeLight` flag, seen as a
  string in `USD_HD.dll`), build an environment light from the sky model; other `light` sprims are ignored with a
  warning.
- The Physical Sky LOP points Karma at `karma/hoseksolarmap.rat`, which suggests the Hosek-Wilkie sky model. Plan:
  evaluate the sky radiance into a lat-long texture at sync (resolution by `hdri_max_isize`-like setting), then reuse
  Nova's textured dome light (importance sampling, MIS, light tree, XPU) unchanged; ground below the horizon from
  `ground_color` (or computed from `ground_albedo`), blended over `horizon_blur`. The sun stays the separate
  DistantLight the LOP authors, so the sky texture must not contain the solar disc twice (check what Karma's sky
  contains at the sun position).
- Camera visibility follows `renderlightgeo` (the Physical Sky turns it on).
- Effort M; most of it is matching Karma's model and scaling (sky luminance, normalization) by measurement.

### K4. Light filters
- Register the `lightFilter` sprim; read the typed parameters (shader id `kma_lfilter_*` tells the kind) and its
  transform; read each light's `filters` (`HdLightTokens->filters`) and the filters' `filterLink` collection
  (Hydra's light-linking tokens include `filterLink`), so a filter can be limited to some geometry, as Nova's light
  linking already does for lights (M9 D15).
- Core: per light an ordered list of filter records; a filter returns a colour factor for a shading point and the
  light sample (position in the filter's or the light's space, direction, distance). Apply it wherever light is
  gathered: next-event estimation, BSDF-sampled hits on lights (MIS weights stay those of the unfiltered light) and
  camera-visible lights. Filters with factor > 1 or textures must keep the light tree's bounds conservative.
- Kinds (Karma schema inputs): blocker (shape, size, rounded corner, falloff, invert, intensity/exposure/tint);
  barn doors (four edges with per-edge softness); gobo (texture with scale/offset, wrap, blur, border, invert);
  gel (brightness, contrast, gamma, saturation, tint); attenuation (near/far ranges with falloff, polynomial
  coefficients or a ramp). The projection VOP (`kma_light_projection`) is a light shader (`karma:light:shader`), not a
  filter, and belongs to K8. Houdini also registers a barn-door API adapter for lights (`HD_HoudiniLightBarnDoorAPIAdapter`);
  check whether Light LOPs author barn doors that way as well.
- XPU: filter evaluation in the light-sampling kernels; textures go through the existing GPU texture path.
- Effort: framework + blocker M, each further kind S-M.

### K5. Renderer selection and Nova's ROP/TOP
- A **Nova ROP** (Driver) for `/obj` scenes built like the Karma ROP (Scene Import of selected objects, lights and
  camera, matte/phantom through K2's primvars, Nova Render Settings, an inner USD Render ROP set to Nova or Nova XPU).
  Generated by `build_nova_hdas.py` like the Nova LOPs, so its parameters follow `nova_settings.json`.
- TOPs: document the USD Render File/Scene route; optionally a "ROP Nova Render" TOP wrapping the Nova ROP.
- A "switch to Nova" shelf action (or a Nova (Setup) option) that sets the selected USD Render ROPs' renderer to Nova
  and, if wanted, swaps a Karma Render Settings LOP for a Nova Render Settings LOP with the mapped values (K1).

### K6. Volumes bounded by surfaces (Fog Box, Karma uniform volumes)
- A gprim (mesh or implicit) whose material has a volume output and no surface output, or that carries
  `volumeuniform`, becomes a medium bounded by its surface: the shell itself is invisible (index-matched), the inside
  is traced with Nova's existing ratio/delta tracking (M8), with the shader evaluated at points inside
  (`ray:hitP`, `geompropvalue` with a constant fallback, `ray:isshadowray` for a separate shadow density).
- Majorant: constant for uniform media; for noise graphs the interval bytecode executor of M8.1 over the box bounds.
- Entry/exit bookkeeping through overlapping or nested shells (camera inside the box is the common fog case) is the
  main design work; M8's MediaWalker overlap merge is the starting point. This is M8's open user decision
  "mesh-bounded volumes".
- Effort M-L.

### K7. AOV filters, deep and checkpoint products
- Per-var `karma:filter`: `minmax` (min/max/zmin/idcover), `stats` (var, mse), `ubox`; Nova's film would keep the
  extra per-pixel state for the vars that ask for it. Needed by Karma's own depth/id planes (edges) and the RenderMan
  denoiser vars.
- `ray:mask` and the other raw sources in the companion report's AOV section.
- Deep camera maps (`karma:deep` products, `karma:product:dcm*`) need per-sample depth lists in the film and a deep
  writer (husk leaves non-raster products to the delegate, docs/research/m6/karma-aovs.md 3): L.
- Checkpoints (`karma:checkpoint`): save and restore film state: M.

### K8. Lens shaders and the remaining light controls
- Lens shaders: the camera's `karma:camera:lensshader` names a material whose lens network (`kma_physicallens`,
  `kma_uvlens`, `kma_roomlens`, ...) decides each camera ray. Needs a camera-ray program in Nova's shading compiler
  (ray origin/direction/weight outputs) on CPU and XPU: L.
- Light shaders (`karma:light:shader`, e.g. the projection VOP), `spread`, `activeradius`, `pointradius`, `contribs`,
  `misbias`, `lightgeocastsshadow`, `contributescaustics`, IES, portal lights: companion WP-D/WP-G.

### K9. Sky atmosphere
- A global medium: spherical planet (centre, sea level), exponential Rayleigh and Mie layers with their scale heights,
  an absorbing layer at an altitude with a width, Mie phase with anisotropy, ground albedo; lit by the scene's
  distant lights. Karma accepts one atmosphere per scene and precomputes LUTs (`lutdim`) and marches with step sizes.
  For Nova: either precomputed transmittance/scattering LUTs (Bruneton-style) applied on camera and shadow rays, or a
  true heterogeneous medium with analytic density for the existing volume integrator (unbiased but slow over
  kilometre scales). Must also hand Nova a custom rprim type (find out the token the adapter emits; HdHDebug does not
  show it). Effort L.

### K10. Texture baking
- A bake mode selected by `karma:global:bake_mode` and `bake_low`: the film is the low mesh's UV space (one tile per
  UDIM); each texel is a point on the low mesh (or on a cage), and rays go from there to the high mesh (trace mode)
  within the bias and maximum distance; the shader or utility value found there is written. Maps: tangent-space
  normal (MikkT, `bake_use_mikkt`), alpha/coverage, AO and curvature/cavity/thickness/edges (Nova has AO, curvature and
  rounded-edge queries since M4.4), position in a box, world/object normals, and shader exports through the
  existing shader AOVs. Island expansion (padding) and the normal remap are image filters; the remap filter (`shift`)
  is in `karma:global:imagefilter`, which Nova currently only reads for the denoiser.
- The companion report lists the `bake_*` properties as "not planned"; for "all Karma nodes" this package is needed,
  but it is the lowest user value per effort. Effort L.

## 6. Work packages, ordered by user value

| # | Package | Unlocks (nodes) | Effort | Depends on |
|---|---|---|---|---|
| K1 | Karma settings, camera exposure/tint, ShadowAPI fallbacks; warnings for ignored image-changing properties (companion WP-A, WP-B, part of WP-C/WP-D) | Karma Render Settings, Render Geometry Settings Karma tab, Camera LOP, Light LOPs, Labs Karma stage | M-L (about 1-2 weeks) | - |
| K2 | Holdouts (Matte, Background, holdout LPEs) and ray-type visibility; `transformvector` spaces and `.pic` reading | Shadow Catcher, Background Plate 1.0/2.0, Labs ML CV, Karma ROP matte/phantom | M+M+M (about 2 weeks) | - |
| K3 | Karma sky dome (`light` sprim, sky model into the textured dome) | Karma Sky Dome Light, Karma Physical Sky | M (about 1 week) | - |
| K4 | Light filters (framework, blocker, then barn door, gobo, gel, attenuation) | Karma Blocker Light Filter, Light Filter Library with all `kma_lfilter_*` VOPs | M + S-M per kind (about 1.5 weeks) | - |
| K5 | Nova ROP, TOP route, "switch to Nova" action | Karma ROP / ROP Karma Render / Labs workflows (replacement), Karma (Setup) | M (about 1 week) | K2 for matte/phantom |
| K6 | Volumes bounded by surfaces, uniform media | Karma Fog Box, Karma uniform/cloud volume materials on meshes | M-L (1-2 weeks) | - |
| K7 | Per-var pixel filters, `ray:mask`, deep, checkpoints | Standard Render Vars details, Labs RenderMan-denoiser AOVs, Render Products deep/checkpoint, Labs Karma checkpoints | S-M + L | - |
| K8 | Lens shaders, light shaders, remaining light controls (companion WP-D/WP-G) | Camera LOP lens material, Light LOP Karma tab | L | - |
| K9 | Sky atmosphere | Karma Sky Atmosphere, Physical Sky atmosphere mode | L (3+ weeks) | K3 helps |
| K10 | Texture baking | Karma Texture Baker | L (3+ weeks) | - |

Order rationale: K1 changes how every Karma-built scene looks and is mostly Hydra work; K2 and K3 are the
compositing and outdoor-lighting tools artists use daily and today give visibly wrong frames; K4 is common in
look-dev lighting; K5 is cheap and opens the `/obj` and TOP workflows; K6 fixes a node that currently ruins the
frame but is used less; K7-K10 serve narrower pipelines.

## 7. Verification plan

- **Probe harness:** turn `make_probes.py` + `render_probes.py` into a test (`tests/husk/check_karma_nodes.py`): a
  USDA per node built once by hython (checked in, so ctest needs no hython), rendered with Nova; expected ratios and
  alphas measured once with Karma and stored in the script, as `check_lights.py` does. Pass: Nova's ratio within 3 %
  of Karma's (or exact where the case is analytic: holdout alpha 0, blocker black, exposure x2).
- **Reference scenes** for features with spatial structure (shadow catcher alpha, light filter patterns, sky,
  fog box) through `tests/reference/tools/make_references.py`, so `check_references.py` compares per plane.
- **Hydra types:** a Hydra test that inserts a `light` and a `lightFilter` sprim like Houdini's adapters do and checks
  that Nova creates the sky light and the filter.
- **Nodes in Houdini:** `tests/houdini` hython cases that create each Karma LOP with defaults and render with husk +
  Nova (catches LOP changes in future Houdini builds); the viewport check (`run_viewport_check.ps1`, own instance)
  for the sky and the filters, which users tune interactively.
- **XPU:** CPU-vs-XPU comparison per new scene and a `[gpu]` case for each integrator feature (K2, K3, K4, K6).

## 8. Open questions for the user

1. Scope of "all Karma nodes": include the Texture Baker (K10) and the Sky Atmosphere (K9), or stop after K1-K7?
2. Holdout semantics: match Karma exactly (measure and copy, including what reflections see) or define Nova's own and
   document the differences?
3. Fog Box / mesh-bounded volumes (K6) answers M8's open decision; confirm it should be done.
4. Should Nova ship its own ROP (Driver) and TOP (K5), or is documenting the USD Render ROP/TOP route enough?
5. For Karma Render Settings sample counts: map Karma's "Variance" mode (primary samples x secondary variance
   samples) onto Nova's variance oracle, or only take the total sample count?
