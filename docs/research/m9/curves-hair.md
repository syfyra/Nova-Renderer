# M9 research: curves and hair in Houdini 22.0.466, Karma's conventions and the Chiang hair BSDF

Scope: docs/PLAN.md M9, "Haare/Kurven (Embree-Curves, Chiang-Hair-BSDF) ... Feature-Parität mit dem Arnold-Kern". This report covers curves and hair only. SSS, the light BVH and light linking are covered by other reports. It answers four questions:

1. How curves (and points) reach a Hydra delegate from Houdini/Solaris: what SOP Import authors for polylines, NURBS and Bezier curves, grooms and points, and what Hydra hands the delegate.
2. Karma CPU's conventions, which are the reference: width, caps, ribbons and tubes, cubic bases, thin curves, the default shading, Karma Hair and MaterialX hair, and how hair events appear in light path expressions and ray limits.
3. The algorithms: Embree 4 curve geometry, self-intersection on curves, and the Chiang et al. 2016 model with a stand-alone prototype (furnace, sampling, chi-square).
4. Recommendations for Nova, with a test plan built on Karma reference numbers.

Sources:
- the Houdini install (`$HFS` = `C:\Program Files\Side Effects Software\Houdini 22.0.466`, `$I` = `$HFS/toolkit/include`);
- `$HFS/houdini/RenderPropertiesKA.json` and `karmaShaderNodes.json`, `karmaNodeGraphs.json` (registries);
- the help archives in `$HFS/houdini/help` and the MaterialX libraries in `$HFS/houdini/materialx` (read for semantics; described in my own words);
- Embree 4.4.1 as built by vcpkg (`%USERPROFILE%\vcpkg\packages\embree_x64-windows-static-md`, the README of the source tree in `vcpkg\buildtrees\embree`, its CMake cache);
- probes run in isolated processes (section 7), and the literature.

Every Houdini process ran isolated: hython through `tests/houdini/run_hython.ps1`, husk through `docs/research/m6/probes/husk_env.ps1`, each with its own preference directory, no user packages and the installed Nova package skipped. The user's Houdini session was not touched, and no fxhoudini tool was used. The probes contain no SideFX code; every SOP network and material is built at run time or written by hand as USD.

Each claim is tagged with its source: **(measured)** for Karma renders, **(probe)** for hython or Hydra-probe output, **(USD)** for authored USD dumped by a probe, **(header file:line)**, **(help page)**, **(registry)**, **(code file:line)** for Nova's sources, **(proto)** for the C++ prototype and **(lit)** for the literature.

Note: while this research ran, curve support appeared in the working tree (`core/geometry/curves.{h,cpp}`, `hydra/curves.{h,cpp}`, `MeshShape::Curves` in `core/scene/mesh.h:22`, curve geometry in `core/accel/embree_accel.cpp:110-161`). Section 5 reviews it against the measurements. One rule there contradicts Karma (5.1, point 3).

---

## 0. Findings in short

1. **SOP Import authors only `BasisCurves` (and `Points`), in a few fixed shapes (USD).**
   - Polylines become `linear` curves with an authored but empty `basis` token. Closed polygons become meshes.
   - Open NURBS curves (the default, "Convert to Basis Curves") become cubic `bspline` curves whose end points are written three times. Closed NURBS become nonperiodic B-splines with the first three points repeated. Bezier curves become `bezier`. **No curve is ever authored `periodic`.**
   - Only "Convert to Pinned Basis Curves" authors `wrap = pinned`. "Treat Polygons as Subdivision Surfaces" turns open polylines into B-splines with explicit reflected end points (`2P0 - P1`).
   - Widths: point `width` becomes vertex `widths`; prim width becomes uniform; detail width becomes constant; `pscale` becomes vertex `widths = 2 pscale`. Without any of them, SOP Import writes a constant `widths = 0.01` (Set Missing Widths, default 0.01).
   - `Cd` becomes `primvars:displayColor` and `uv` becomes `primvars:st`, both at the attribute's rate. `N` becomes `normals`.
   - Hair Generate writes linear curves of 9 points whose `widths` taper to 0 at the tip (0.001 at the root), uniform `id`/`skinprim`/`skinprimuv`, and no colour or normals.
   - Points become `Points` with vertex `widths` (`2 pscale`, or `width`).
2. **Hydra hands a delegate plain `basisCurves` rprims (probe).** It gets `HdBasisCurvesTopology` (type, basis, wrap, counts) and `widths`, `normals`, `displayColor`, `st` as primvars with their interpolation. `nurbsCurves` and `points` are separate rprim types that arrive only if the delegate lists them. `HermiteCurves` never reached the probe. Curves under a point instancer carry the instancer id like meshes.
3. **Karma CPU renders curves with Embree's curve primitives, one to one (measured).**
   - **Width is the diameter**, exactly, down to 1/10 pixel. There is no minimum width. `karma:object:width_scale` multiplies it.
   - **Round linear curves have hemispherical end caps** (Embree's round linear curve). **Round cubic curves are open at the ends** (Embree's sweep surface).
   - **The default `karma:object:curve_style` is Rounded Curves: authored normals are ignored** and the curve renders as a round tube with the true tube normal.
     - Style 0 renders flat ribbons facing the camera, with N toward the camera.
     - Styles 2 (Oriented) and 3 (USD default) render normal-oriented ribbons for cubic curves.
     - Linear curves cannot be oriented and fall back to camera-facing ribbons (Embree has no oriented linear type).
   - Cubic curves are intersected exactly: Bezier and B-spline coverage matches the analytic curve within the half-width bound.
   - **Pinned curves use the USD rule `P[-1] = 2 P[0] - P[1]`**, not the end-point repetition of Pixar's `HdsiPinnedCurveExpandingSceneIndex`.
   - `NurbsCurves` render as their linear control hull.
   - `ray:hituv` = (0, v), with v running from 0 to 1 over the whole curve, uniform per segment. `ray:primid` = `ray:element` = the curve's index in the prim.
   - Points render as spheres whose width is the diameter.
4. **Curves without a material get the same default surface as meshes, lit with the tube normal (measured).** It is a diffuse with about 0.81 x 0.5 albedo plus a weak glossy lobe, and it ignores `displayColor`, on curves and on meshes. Nova's default material (Lambert of `displayColor`, else 0.18) is its own choice (docs/design/M3.md K19). Curves should simply use it with the tube normal.
5. **Karma Hair (`kma_hair`) is the Chiang 2016 model in pbrt's formulation (measured, proto).**
   - White furnace on a single fibre: 1.0000 at 0, 30 and 60 degrees.
   - With absorption (0.2, 0.5, 1.0) the furnace equals the analytic mean over h of sum A_p to 4 digits: (0.6983, 0.4247, 0.2111) against (0.6984, 0.4248, 0.2113).
   - Under six distant lights, every non-negligible per-light value matches the prototype within 1.5 %. The only systematic difference: the TT peak is 0.8-1.0 % brighter in Karma.
   - The parameterisation is the same:
     - `absorption` is pbrt's sigma_a;
     - roughness r gives beta_m = beta_n = r, with v_TT = v/4 and v_TRT = 4v;
     - **`shift` s tilts by alpha = -s * 90 degrees** in a root-to-tip frame (the R highlight moves toward the root), applied to theta_i as MaterialX's GLSL does;
     - pbrt's 10-term Bessel series reproduces Karma's lobes best.
6. **Karma Hair's colour controls, measured and fitted to 0.1 % (measured).** The absorption terms add up:
   - **sigma_a = absorption + Chiang's colour inversion of (base x baseColor) with beta_n + thicknessScale * (-ln(1 - melanin)) * [(1 - redness) * 1.2085 * (0.419, 0.697, 1.37) + redness * 1.833 * (0.187, 0.4, 1.05)]**;
   - `tint` multiplies the result;
   - `diffuse` mixes in a Lambert reflection on the tube normal (diffuse = 1: albedo = diffuseColor, reflection only);
   - `coat` adds a strong glint whose model is still open (section 8).
7. **Hair events are glossy and classified by the side of the tube normal (measured, proto).**
   - Light path expressions see every hair lobe as `G`. It is `R` when the incoming direction lies on the viewer's side of the hit's tube normal and `T` otherwise; the R/T split matches that rule at all three angles (0.1270 / 0.1472 / 0.2841 predicted, 0.1276 / 0.1479 / 0.2833 measured). It does not follow the lobe identity (R+TRT against TT+TRRT).
   - The same side decides the ray limit: hair events count against the reflect or the refract limit, never the diffuse one.
   - Hair-ball furnace (limits 64): 0.939. At Karma's default limits: 0.522. With refract limit 0: 0.215. With reflect limit 0: 0.476.
8. **Karma's MaterialX `ND_chiang_hair_bsdf` is not the Chiang model (measured).**
   - A single white fibre in a white furnace returns 0.061 / 0.040 / 0.045 instead of 1. With ior 1 it returns 0.030, where Karma Hair returns 1.000.
   - The transmission side never gets direct light, and the 0-degree value equals the model's reflection side without the R lobe (0.0568 predicted, 0.0566 measured).
   - Nova should implement the node per the MaterialX spec, with the same BSDF as Karma Hair, and document that it differs from Karma here.
9. **Embree 4.4.1 from vcpkg has everything Nova needs (header, registry).**
   - Built with curves and points, minimum width OFF, curve back-face culling OFF, and the flat-curve self-intersection avoidance factor 2.0.
   - Round curves report u = segment parameter, v = 0, and an unnormalised geometric normal.
   - Round curves have no self-intersection avoidance: Nova must offset hair rays itself. A transmitted (TT) continuation must leave the far side of the fibre, which Karma Hair does: its single-fibre furnace has no second event.
10. **Prototype (`probes/ch_hair_proto.cpp`, proto).**
    - With a 24-term Bessel series, the white-furnace quadrature is within 0.18 % of 1 for beta in [0.1, 1], tilt 0/2 degrees and theta_o up to 80 degrees. pbrt's 10-term series loses up to 0.37 %.
    - The importance sampler passes chi-square tests (8 configurations, p = 0.05-0.84).
    - It predicts Karma's fibre measurements as above.
11. **Recommendation, in short (section 5).**
    - Keep curves as `MeshShape::Curves` with Embree round, flat and oriented types, but follow Karma's `curve_style` (round by default, normals ignored).
    - Add the hair BSDF to the closure model as four `Hair` lobes (R, TT, TRT, TRRT+), with every wo-dependent quantity precomputed at the hit, classified R/T by the side of the tube normal and never entering a medium.
    - Translate `kma_hair` with the measured formulas, and `ND_chiang_hair_bsdf` per the MaterialX spec.
    - Offset hair continuation and shadow rays to the fibre's exit point.

---

## 1. What Houdini's LOPs author

### 1.1 Test networks (`probes/ch_author_curves.py`, output `probes/ch_out_authored_usd.txt`)

An isolated hython builds a SOP network and imports each variant with its own SOP Import LOP under `/curves/<variant>`. The variants are:
- a wavy 6-point polyline with attributes added per variant: none, point `width` + `Cd` + `uv`, prim `width` + `Cd`, detail `width`, point `pscale`, vertex `uv`, point `N`, closed;
- the polyline converted to an order-4 NURBS curve (Convert SOP), and a 7-point Bezier;
- closed NURBS and Bezier circles (Circle SOP);
- Hair Generate on a polygon sphere without guides;
- two point clouds (`pscale`, `width`).

The NURBS curve is also imported with "Convert to Pinned Basis Curves" and with "Create NURBS Curves", and the N variant with "Treat Polygons as Subdivision Surfaces". The SOP Import defaults are `nurbscurvehandling` = basiscurves, `setmissingwidths` = 0.01 and `polygonsassubd` = 0 (probe).

### 1.2 Authored prims (USD)

| SOP source | prim | type / basis / wrap | points | widths | other |
|---|---|---|---|---|---|
| open polyline, no width | `BasisCurves` | linear / `""` (authored, empty) / nonperiodic | 6 | `[0.01]` constant (Set Missing Widths) | |
| point `width`, `Cd`, `uv` | `BasisCurves` | linear | 6 | vertex | `displayColor` vertex, `st` (texCoord2f) vertex |
| prim `width`, `Cd` | `BasisCurves` | linear | 6 | uniform | `displayColor` uniform |
| detail `width` | `BasisCurves` | linear | 6 | constant | |
| point `pscale` 0.03 | `BasisCurves` | linear | 6 | vertex **0.06** (= 2 pscale) | |
| vertex `uv` | `BasisCurves` | linear | 6 | constant 0.01 | `st` vertex |
| point `N` | `BasisCurves` | linear | 6 | vertex | `normals` vertex |
| closed polyline | `Mesh` | | | | a closed polygon is a face |
| open NURBS, order 4, clamped knots | `BasisCurves` | **cubic / bspline / nonperiodic** | **10** (P0 and P5 written 3 times) | vertex, also tripled | primvars tripled too |
| same, "Convert to Pinned Basis Curves" | `BasisCurves` | cubic / bspline / **pinned** | 6 | vertex | |
| same, "Create NURBS Curves" | `NurbsCurves` | order 4, knots (0,0,0,0,1/3,2/3,1,1,1,1), ranges | 6 | vertex | |
| closed NURBS circle (8 CVs) | `BasisCurves` | cubic / bspline / **nonperiodic** | 11 (first 3 repeated) | constant 0.01 | |
| open Bezier (7 points) | `BasisCurves` | cubic / bezier / nonperiodic | 7 | vertex | |
| closed Bezier circle | `BasisCurves` | cubic / bezier / nonperiodic | 25 (first point repeated) | constant | |
| polyline + N, "Treat Polygons as Subdivision" | `BasisCurves` | cubic / bspline / nonperiodic | 8 (phantoms `2P0 - P1` written explicitly) | vertex | `normals` vertex |
| Hair Generate (no guides) | `BasisCurves` | linear | 623 curves x 9 | vertex, 0.001 at the root tapering to **0** at the tip | uniform `id`, `skinprim`, `skinprimuv`; constant `gl_shadeopencurves`; no `Cd`, no `N` |
| points with `pscale` | `Points` | | 5 | vertex `2 pscale` | `displayColor` vertex |
| points with `width` | `Points` | | 5 | vertex `width` | |

Consequences for Nova:
- Every Houdini curve is linear, cubic Bezier or cubic B-spline. Catmull-Rom only appears if authored by hand or by the Basis Curves LOP. `periodic` is never authored by SOP Import, so Hydra's periodic path matters only for hand-authored USD.
- Linear curves from Houdini carry `basis = ""`. Hand-written linear curves without `basis` report the schema fallback `bezier` (probe). For `type = linear` the basis must simply be ignored.
- Widths can be constant, uniform or vertex; varying is legal USD (`$I/pxr/usd/usdGeom/basisCurves.h:136-200`) and must be handled.

### 1.3 Help pages (help)

- The Basis Curves LOP (`nodes/lop/basiscurves`) describes the bases, the wraps and pinned phantom points. The Karma Hair page (`nodes/vop/kma_hair`) recommends cubic B-spline + pinned for close-ups.
- The Karma render property help (`nodes/lop/karmarenderproperties`) says Karma CPU cannot render linear curves as oriented ribbons. It advises "Treat Polygons as Subdivision Surfaces" to get B-splines instead (also `RenderPropertiesKA.json`, `karma:object:curve_style`).
- **Houdini Procedural: Hair** (`nodes/lop/houdinihairprocedural`) generates or deforms hair at render time from guides and a skin with `primvars:rest`. The generated prim has render purpose and appears only when a renderer runs the procedural. husk runs procedurals through `husd/runprocedurals.py` before and after each frame (husk log). Whether this happens for a third-party delegate was not probed (section 8).
- Karma XPU supports only linear, B-spline, Catmull-Rom and Bezier curves, as round tubes only, and accepts `kma_hair` and `kma_fur` as curve terminals (`solaris/karma_xpu`).

---

## 2. What Hydra hands the delegate

### 2.1 The probe (`probes/ch_curves_probe/`, output `probes/ch_out_hydra_probe.txt`)

`hdCurvesProbe` is an HDK-free delegate built like M8's volume probe, staged as a package and run by husk on `ch_probe_hydra` (`probes/ch_karma_scenes.py`). That scene references all of section 1's prims and adds:
- a deforming curve;
- a curve prototype under a point instancer;
- a `HermiteCurves` prim;
- a Karma Hair material on the groom and a MaterialX hair material on one polyline;
- `karma:object:curve_style` / `width_scale` as render settings.

The probe lists `basisCurves`, `points`, `nurbsCurves` and `mesh` as rprim types.

| Item | What arrived (probe) |
|---|---|
| `basisCurves` rprim | `GetBasisCurvesTopology()`: type, basis, wrap, counts, no indices (as authored). Unauthored basis on a linear curve: `bezier`. SOP-imported linear: empty token. |
| primvars | `points` (vertex, role point), `widths` (constant / uniform / vertex as authored), `normals` (vertex, role normal), `displayColor` (role color), `st` (role textureCoordinate), the groom's `id`, `skinprim`, `skinprimuv` (uniform). |
| scene index | prim type `basisCurves`; data sources include `basisCurves` (topology schema), `primvars`, `displayStyle` (husk: refineLevel 8), `materialBindings`, `instancedBy` (instanced prototypes) |
| `points` rprim | primvars `points`, `widths`, `displayColor` |
| `NurbsCurves` | rprim type `nurbsCurves` (only because the probe lists it), with its primvars; Hydra does not convert it on its own |
| `HermiteCurves` | nothing arrived |
| instanced curve | the prototype `basisCurves` with `GetInstancerId() = /World/inst` |
| materials | Karma Hair: terminal `surface` with one node `kma_hair`. MaterialX: `ND_chiang_hair_bsdf` -> `ND_surface`. |
| render settings | `karma:object:curve_style` (int) and `width_scale` (float) arrive in the settings map |

Headers:
- `HdBasisCurvesTopology` lists linear and cubic types; the bases bezier, bspline, catmullRom and centripetalCatmullRom; and the wraps nonperiodic, periodic and segmented, with pinned as a token (`$I/pxr/imaging/hd/basisCurvesTopology.h:23-58`, `tokens.h:26-91`).
- The Hermite adapter "renders the points as linearly interpolated BasisCurves" in Hydra 1 (`$I/pxr/usdImaging/usdImaging/hermiteCurvesAdapter.h:21-27`). In husk's Hydra 2 path nothing arrived (probe).
- `HdsiNurbsApproximatingSceneIndex` converts `nurbsCurves` to `basisCurves` "for clients that do not natively support nurbs" (`$I/pxr/imaging/hdsi/nurbsApproximatingSceneIndex.h:17-23`). Storm registers it as a plugin (`$I/pxr/imaging/hdSt/nurbsApproximatingSceneIndexPlugin.h:14`).
- **`HdsiPinnedCurveExpandingSceneIndex` repeats the end points** (twice for B-spline, once for Catmull-Rom; `$I/pxr/imaging/hdsi/pinnedCurveExpandingSceneIndex.h:21-33`). **The USD schema defines the phantom points by reflection, `P[-1] = 2 P[0] - P[1]`** (`$I/pxr/usd/usdGeom/basisCurves.h:84-103`). Karma follows the schema (3.2), so Nova must not use the Hdsi expansion.
- Tubes vs ribbons per the schema: widths alone give tubes of radius width/2; widths plus normals give ribbons (`basisCurves.h:211-217`). Karma's default does not follow this (3.2).

Deformation blur: the deforming curve reports a single sample through the probe's default-interval `SamplePrimvar`. Nova's existing interval overload over the camera shutter (`hydra/motion.h:80-97`, code) is the right call for curve `points`, `widths` and `normals`, as for meshes. Curve motion blur was not measured in Karma.

---

## 3. Karma CPU's conventions

### 3.1 Settings and node registries (registry, help)

| Property | Default | Meaning |
|---|---|---|
| `karma:object:curve_style` | Rounded Curves (1) | 0 Flat Ribbons (camera-facing), 1 Rounded Curves, 2 Oriented Ribbons (to the normals), 3 USD Default Behaviour |
| `karma:object:curve_basis` | Geometry Default (0) | 1 Force Linear, 2 Force Bezier, 3 Force B-Spline, 4 Force Catmull-Rom (the topology must fit: >= 4 points, Bezier 4 + 3N) |
| `karma:object:width_scale` | 1 | multiplies point and curve widths |
| `karma:object:point_style` | Spheres (1) | 0 Discs (camera-facing), 1 Spheres, 2 Oriented Discs |
| `karma:global:raybias` | 0.001 | minimum distance of secondary rays |
| `karma:global:autoraybias` | true | CPU: automatic bias for everything except procedural meshes and continued rays; XPU: polymeshes only, curves and points use Ray Bias |
| `karma:global:diffusecomponents` / `refractcomponents` / `ssscomponents` / `volumecomponents` | `diffuse` / `refract` / `sss` / `volume` | other components count as reflection |
| `karma:object:geosamples` / `samplesfromstage` | 2 / true | deformation samples |

There is no minimum-width property; Mantra's curve properties (`vm_curvesurface`, `vm_curveunitst`, ...) exist only in `RenderProperties.json` (registry).

Karma Hair (`kma_hair`, output `surface`, `karmaShaderNodes.json`) and its defaults:

| Input | Default | Input | Default |
|---|---|---|---|
| base | 1 | roughness | 0.3 |
| baseColor | (1,1,1) | roughnessAzimuthal | 0.3 (used when roughnessAnisotropic = 1) |
| melanin | 0.4 | roughnessAnisotropic | 0 |
| melaninRedness | 0.1 | roughnessRandomize | 0 |
| melaninRandomize | 0 | roughness_advanced / ttscale / trtscale | 0 / 0.25 / 4 |
| thicknessScale | 1 | coat | 0 |
| tint | (1,1,1) | diffuse / diffuseColor | 0 / (1,1,1) |
| shift | 0.03 | customHairID / hairID / randSeed | 0 / 0 / 0 |
| ior | 1.55 | absorption | (0,0,0) |

- `kma_fur` (2.0) has the same inputs plus cuticle reflectance, medulla (radius 0.5, phase, opacity, pigment, diffuse, blend normals), a second R lobe, wetness (GGX, ior 1.333, roughness 0.01) and `pruneLevel`. Its help says medulla radius 0 reduces it to Karma Hair, and it cites pbrt's hair implementation, Chiang 2016 and Yan et al. (help `nodes/vop/kma_fur`).
- Both are native Karma VOPs without VEX code (HDA sections). They correspond to the VEX BSDFs `chiang(nn, tanV, R_v, R_s, TT_v, TT_s, TRT_v, TRT_s, shift, absorption, ior)` and `chiang_fur(...)`, whose `shift` maps [-1, 1] to [-90, 90] degrees (help `vex/functions/chiang`, `chiang_fur`).
- `kma_melanin` is a MaterialX nodegraph (`karmaNodeGraphs.json`):
  - artistic mode: absorption = melanin x scale x [(1 - r) x 15 x eumelanin + r x 30 x pheomelanin];
  - physical mode: melanin x 15 x scale x mix(eumelanin, pheomelanin, pheomelanin parameter);
  - eumelanin = (0.419, 0.697, 1.37), pheomelanin = (0.187, 0.4, 1.05);
  - colour = exp(-absorption), and the `absorp` output = -ln(clamp(colour)).
  - Nova already expands it (docs/research/m4/karma-nodes.md:83). It is *not* the mapping `kma_hair` uses internally (3.4).

MaterialX (`$HFS/houdini/materialx/libraries/pbrlib`):
- `ND_chiang_hair_bsdf` (bsdf "R"; `pbrlib_defs.mtlx:146-159`):
  - inputs: tints R/TT/TRT (1); ior 1.55; `roughness_R/TT/TRT` as vector2 (v, s) with defaults (0.1, 0.1) / (0.05, 0.05) / (0.2, 0.2); `cuticle_angle` 0.5; `absorption_coefficient` 0;
  - `normal` and `curve_direction` default to Nworld and Tworld.
- `ND_chiang_hair_roughness` maps (longitudinal, azimuthal, scale_TT 0.5, scale_TRT 2) to (v, s) per lobe, with Chiang's polynomials.
- `ND_chiang_hair_absorption_from_color` and `ND_deon_hair_absorption_from_melanin` (melanin' = -ln(1 - c), split by redness) are the other helpers (`pbrlib_defs.mtlx:433-463`).
- The GLSL implementation (`genglsl/mx_chiang_hair_bsdf.glsl`) gives the semantics:
  - s is the logistic scale **without** the sqrt(pi/8) factor, applied inside the trimmed logistic;
  - `cuticle_angle` maps [0, 1] to alpha in [-pi/2, pi/2] and rotates theta_i (R: theta_i + 2 alpha, TT: theta_i - alpha, TRT: theta_i - 4 alpha);
  - the residual lobe uses TRT's roughness and tint.
  - Two quirks: the residual Np is written `1.0 / 2.0 * M_PI` (= pi/2, not 1/(2 pi)), and the result is multiplied by 1/pi.
- The OSL implementation calls OSL's `chiang_hair_bsdf` closure (OSL >= 1.14), else a reflective `dielectric_bsdf` fallback (`genosl/mx_chiang_hair_bsdf.osl`).
- Houdini's VEX library implements only the three helpers (`$HFS/houdini/vex/include/mxpbr.h:1116-1171`).
- Nova lowers the three helpers already (`hydra/material/translator.cpp:1386-1418`, code) and drops `chiang_hair_bsdf` with a warning (`translator.cpp:2520-2523`, code).

### 3.2 Geometry (`probes/ch_karma_scenes.py` scenes `geo_*`, measurements in `probes/ch_out_karma_measure.txt`)

Setup: an orthographic camera looks down -z with box pixel filter 1, path traced at 64 samples, no colour limit, alpha from the beauty's fourth channel.

| Quantity | Scene | Karma | Conclusion |
|---|---|---|---|
| projected width, linear, w = 0.2 / 0.0025 (1/2 px) / 0.0005 (1/10 px) | `geo_width` | 0.20000 / 0.00249 / 0.00050 | width = diameter, exact; no minimum width |
| B-spline (pinned), w = 0.2 | `geo_width` | 0.20000 | |
| `karma:object:width_scale` = 2 (constant primvar) | `geo_width` | 0.40000 | multiplies widths |
| vertex widths 0.1 -> 0.3 along y | `geo_width` | 0.2199 / 0.1999 / 0.1798 at y = 0.4 / 0 / -0.4 (linear 0.2199 / 0.1999 / 0.1799) | linear interpolation of vertex widths on linear curves |
| point, width 0.2 | `geo_width` | alpha area 0.03142 = pi 0.1^2 | sphere of diameter = width |
| coverage beyond the end, w = 0.1 | `geo_caps` | linear: 0.003927 (hemisphere pi r^2/2 = 0.003927), centre-line extent 0.0499; B-spline: 0 | linear: round caps; cubic: open |
| projected width, normals 60 degrees from the view, w = 0.2 | `geo_ribbon` | style unset 0.2000, 0: 0.2000, 1: 0.2000, 2: **0.1000**, 3: **0.1000**, linear + 2: 0.2000 | default = round, normals ignored; 2/3 = oriented ribbons (w cos 60); linear cannot be oriented |
| `ray:hitN` across the curve | `geo_ribbon`, `geo_shade` | round: the tube normal ((-0.975, 0, 0.209) at the left edge), `Ng` the same; flat (0): (0, 0, 1) toward the camera; oriented: the authored normal (0.866, 0, 0.5) | |
| Bezier / B-spline nonperiodic, w = 0.02 | `geo_basis_*` | covered pixels within 0.0103 / 0.0100 of the exact curve (bound 0.0114); area = length x w to 0.1 % | exact cubic intersection |
| B-spline pinned | `geo_basis_bspline_pinned` | within 0.0101 of the reflected-phantom curve; 0.0154 from the end-repeat (Hdsi) curve | USD's `2 P0 - P1` |
| Catmull-Rom pinned | `geo_basis_catmullRom_pinned` | within 0.0102 of the reflected-phantom curve; the end-repeat curve is only 67 % covered | USD's `2 P0 - P1`, native Catmull-Rom |
| tight Bezier bends (w/2 : bend radius about 1 : 4) | `geo_basis_bezier_wave` | within 0.0251 (bound 0.0264) | |
| `NurbsCurves` (clamped order 4) | `geo_ids` | within 0.0101 of the linear control hull, 12 % of the true curve covered | rendered as the control polygon, as linear tubes |
| `ray:hituv` | `geo_width`, `geo_shade_side`, `geo_ids` | u = 0 everywhere; v = 0.4994 at mid-curve. A 3-segment linear curve: v = (segment + t)/3. A 2-segment B-spline: v = (x + 0.95)/1.9. At a linear end cap v = 1. | (0, v), v uniform per segment over the whole curve |
| `ray:primid`, `ray:element` | `geo_ids` | 0, 1, 2 for the three curves of one prim (2 segments each) | curve index, not segment |

All of these behaviours are those of Embree's curve types (4.1). Karma CPU evidently uses Embree's round, flat and normal-oriented curves directly: round linear curves are cones with end spheres, round cubic curves are open sweeps, and there is no oriented linear type.

### 3.3 Default shading of curves (`geo_shade`, `geo_shade_side`)

A distant light from the camera (irradiance 1) on a round tube of width 0.4 without a material gives:
- 0.1591 at the centre (= 0.5/pi);
- 0.81-0.85 x 0.5 cos(gamma)/pi off-centre;
- a mean of 0.1091 over the tube.

A tube with `displayColor` 1 gives the same (0.1094) (measured). A mesh quad with `displayColor` 1 gives 0.1592: Karma's default surface ignores `displayColor` (measured).

Lit from 60 degrees, the default surface splits into diffuse 0.0644 and glossy 0.0010 per unit irradiance. That is 0.81 x Lambert(0.5) plus a weak specular, identical on curves, flat ribbons and meshes (measured). Curves therefore use the ordinary default surface with the round tube normal.

### 3.4 Hair shading measurements

Scenes `fur_*` (white furnace: dome 1, limits 64) and `dl_*` (six normalised distant lights l0-l5 with LPE tags) use one fibre of width 0.5 along t = (0, cos a, sin a), root at -2t. An orthographic view at 200 x 40 px puts 100 px across the fibre; 256 samples. The mean radiance over the fibre (C summed / alpha summed) is the mean over h of f|cos|.

Lights, as directions the light arrives from:
- l0 (0, 0, 1), from the camera side;
- l1 (1, 0, 0), from the side;
- l2 (0, 0, -1), from behind (TT);
- l3 and l4: 10 degrees off l0 toward the tip and toward the root;
- l5 (0.5, 0.5, 0.7071).

Predictions: `ch_hair_proto predict`, Karma conventions (`CH_I0_TERMS=10 CH_TILT_WI=1`, `probes/ch_out_proto_predict_karma.txt`).

**Energy and absorption (`fur_*`, measured vs proto).**

| Material | a = 0 | a = 30 | a = 60 | Prediction |
|---|---|---|---|---|
| Karma Hair, no absorption (melanin 0) | 0.9999 | 1.0002 | 1.0001 | 1 (sum A_p) |
| Karma Hair, absorption (0.2, 0.5, 1.0) | (0.6983, 0.4247, 0.2111) | (0.6830, 0.4068, 0.2028) | (0.6543, 0.3982, 0.2403) | (0.6984, 0.4248, 0.2113) / (0.6828, 0.4066, 0.2026) / (0.6542, 0.3982, 0.2403) |
| Karma Hair defaults | (0.6400, 0.4790, 0.2524) | (0.6246, 0.4607, 0.2399) | (0.5959, 0.4422, 0.2625) | 3.5 formula |
| melanin 0.8, redness 0 | (0.2679, 0.1455, 0.0813) | (0.2554, 0.1436, 0.0905) | (0.2779, 0.2023, 0.1748) | |
| baseColor (0.8, 0.5, 0.2), melanin 0 | (0.9973, 0.9747, 0.8722) | (0.9974, 0.9732, 0.8644) | (0.9967, 0.9683, 0.8450) | Chiang eq. 9, beta_n 0.3: (0.9973, 0.9747, 0.8723) |
| diffuse 1, diffuseColor 0.5 | 0.5000 (all `RD`) | 0.5001 | 0.5001 | Lambert |
| ior 1 | 1.0001 (RG 0.0111) | | | 1 (R side 0.0107) |

**R/T split of the furnace (`RG`, `TG`; no `D`, no `S`, no indirect), Karma Hair, no absorption.**

| a | Karma RG / TG | proto: wi.N > 0 (side of the tube normal) | proto: lobes R+TRT / TT+TRRT | proto: wi.wo_perp > 0 |
|---|---|---|---|---|
| 0 | 0.1276 / 0.8724 | **0.1270** | 0.1292 | 0.0975 |
| 30 | 0.1479 / 0.8523 | **0.1472** | 0.1463 | 0.1117 |
| 60 | 0.2833 / 0.7168 | **0.2841** | 0.2753 | 0.2315 |

With absorption (0.2, 0.5, 1.0) at a = 0: RG (0.0978, 0.0799) measured, side rule (0.0976, 0.0796).

**Distant lights (`dl_kma_sa`, `dl_kma_rough`, `dl_kma_tilt`; `probes/ch_out_karma_vs_model.txt`).**

Every value above 2e-3 matches the model within 1.5 %, except l2, the TT lobe from behind, which Karma renders 0.8-1.0 % brighter (uniform across channels).

| Scene | l0 | l1 | l2 | l3 | l4 | l5 |
|---|---|---|---|---|---|---|
| `dl_kma_sa_a0` Karma | (0.0384, 0.0235, 0.0171) | (0.0403, 0.0303, 0.0214) | (1.1201, 0.6260, 0.2424) | (0.0351, 0.0207, 0.0146) | = l3 | (0.0059, 0.0042, 0.0035) |
| model | (0.0384, 0.0235, 0.0171) | (0.0401, 0.0302, 0.0214) | (1.1116, 0.6212, 0.2406) | (0.0351, 0.0207, 0.0146) | = l3 | (0.0059, 0.0042, 0.0035) |
| `dl_kma_sa_a30` Karma | (0.0094, 0.0030, 0.0005) | (0.0042, 0.0039, 0.0039) | (1.1171, 0.6044, 0.2240) | (0.0063, 0.0020, 0.0003) | (0.0138, 0.0045, 0.0009) | 0.0001 |
| `dl_kma_rough_a0` (roughness 0.15, azimuthal 0.5) Karma | (0.0724, 0.0487, 0.0383) | (0.2364, 0.1551, 0.0860) | (2.0964, 1.1940, 0.4820) | (0.0420, 0.0237, 0.0158) | = l3 | (0.0022, 0.0008, 0.0001) |
| model beta_m 0.15, beta_n 0.5 | (0.0716, 0.0483, 0.0381) | (0.2358, 0.1546, 0.0857) | (2.0926, 1.1918, 0.4810) | (0.0415, 0.0234, 0.0157) | = l3 | (0.0022, 0.0007, 0.0001) |
| `dl_kma_tilt_a0` (shift 3/90) Karma | (0.0363, 0.0221, 0.0161) | (0.0379, 0.0285, 0.0201) | (1.0506, 0.5872, 0.2274) | **(0.0328, 0.0179, 0.0116)** | **(0.0344, 0.0218, 0.0164)** | (0.0048, 0.0028, 0.0020) |
| model alpha = -3 degrees, tilt on theta_i | (0.0363, 0.0221, 0.0161) | (0.0377, 0.0284, 0.0201) | (1.0426, 0.5827, 0.2257) | (0.0328, 0.0179, 0.0116) | (0.0344, 0.0218, 0.0164) | (0.0048, 0.0028, 0.0020) |
| `dl_kma_tilt_a30` Karma | (0.0145, 0.0045, 0.0007) | (0.0025, 0.0022, 0.0020) | (1.0635, 0.5751, 0.2126) | (0.0100, 0.0031, 0.0005) | (0.0202, 0.0064, 0.0010) | 0.0001 |
| model | (0.0145, 0.0045, 0.0007) | (0.0025, 0.0022, 0.0020) | (1.0531, 0.5695, 0.2107) | (0.0100, 0.0031, 0.0005) | (0.0202, 0.0064, 0.0010) | 0.0001 |

Conventions this pins down (measured, proto):
- **Tilt.** Positive `shift` moves the R lobe toward the root, i.e. alpha = -shift x 90 degrees in a root-to-tip tangent frame. It is applied by rotating theta_i (R: theta_i - 2 alpha, TT: + alpha, TRT: + 4 alpha), not theta_o.
  - pbrt's theta_o rotation gives the same values at a = 0 for l0-l2. It misses l3/l4/l5 by 1-3 % at a = 0 and l2 by 3.9 % at a = 30.
  - `kma_def` (shift 0.03) against `kma_def_shift0` shows the same sign: l3 0.0294 < l4 0.0314; with shift 0 both are 0.0315.
- **Bessel series.** Karma's lobes match pbrt's 10-term I0 series (l0 ratio 1.000), not the more exact 24-term one (l0 0.992).
- **h convention.** Per pixel, the fibre profile under l1 and l5 has the model's asymmetry with h = N . (z x T), where z = wo projected into the normal plane (`probes/ch_out_proto_profile.txt`).
- **Diffuse.** The `diffuse` lobe is Lambert on the tube normal: `dl_kma_diffuse_a0` gives l0 0.1250 = 0.5 x (pi/4)/pi, l1 0.0398 = 0.5/(4 pi), l2 0.
- **Coat.** coat = 1 (`dl_kma_coat_a0`) makes the fibre glint at a level far above the model: C (19.35, 18.80, 18.39); l0 6.50, l1 5.81 and l2 (6.99, 6.50, 6.12) for one unit of irradiance each; l3/l4 0.022. That is an untinted, longitudinally very sharp extra lobe on both sides. Not modelled (section 8).

### 3.5 Karma Hair's colour controls (`fur_kma_mel_*`, `fur_kma_thick_*`, `fur_kma_combo`, `fur_kma_base05`, `fur_kma_tint`)

Each furnace albedo was inverted per channel to sigma_a with the model (`ch_hair_proto invert`). The inversions at a = 0 and a = 30 agree to 0.3 %. Then sigma_a was fitted as eu x (0.419, 0.697, 1.37) + pheo x (0.187, 0.4, 1.05) (residuals < 1e-3) (measured, proto):

| melanin m, redness r | eumelanin part | pheomelanin part | eu / (-ln(1-m)) |
|---|---|---|---|
| 0.1, 0 | 0.1272 | 0 | 1.207 |
| 0.5, 0 | 0.8376 | 0 | 1.2084 |
| 0.9, 0 | 2.7826 | 0 | 1.2085 |
| 0.95, 0 | 3.6208 | 0 | 1.2087 |
| 0.5, 0.25 / 0.5 / 0.75 / 1 | 0.6283 / 0.4184 / 0.2095 / 0 (= 0.8376 (1 - r)) | 0.3166 / 0.6354 / 0.9526 / 1.2713 | pheo / (r (-ln(1-m))) = 1.833 |
| 0.2, 1 and 0.3, 1 | 0 | pheo factor 1.833 | |
| thicknessScale 0.5 / 2 (m 0.5) | 0.4185 / 1.6745 (x 0.5 / x 2) | | |

So, in Karma Hair:
- sigma_melanin = thicknessScale x L x [(1 - r) x 1.2085 x eumelanin + r x 1.833 x pheomelanin], with L = -ln(1 - melanin).
- **The colour terms add.**
  - `combo` = melanin 0.5 + baseColor (0.8, 0.5, 0.2) + absorption 0.1 measured sigma_a (0.4523, 0.6970, 1.3207), against the sum of the three (0.4524, 0.6978, 1.3223).
  - `base` 0.5 adds the grey term (ln 0.5 / 5.888)^2 = 0.0139 (5.888 is Chiang's polynomial at beta_n 0.3), i.e. the colour inverted is base x baseColor.
- `tint` (0.5, 1, 1) halves the red result: 0.2702 = 0.5 x 0.5404.
- `melaninRandomize` 0.5 gave one strand an effective melanin of 0.39 instead of 0.5. It is a per-strand random value, so only statistics can match.
- Untested: whether `thicknessScale` also scales the absorption and colour terms, and which beta_n the colour inversion uses with anisotropic roughness (section 8).

### 3.6 Ray limits and light path expressions on hair (`ball_*`)

The hair ball is 2000 random straight fibres (length 0.6, width 0.01, random directions, centres uniform in a ball of radius 0.35), Karma Hair without absorption, white furnace, 128 samples, 240 x 240 px.

| Limits (diffuse / reflect / refract) | C | RG | TG | direct / indirect |
|---|---|---|---|---|
| 64 / 64 / 64 | 0.9390 | 0.1568 | 0.7821 | 0.1960 / 0.7430 |
| Karma defaults 1 / 4 / 4 | 0.5224 | 0.1039 | 0.4184 | 0.1960 / 0.3264 |
| 64 / 0 / 64 | 0.4755 | 0.0433 | 0.4322 | 0.1960 / 0.2795 |
| 64 / 64 / 0 | 0.2154 | 0.0627 | 0.1527 | 0.1960 / 0.0194 |
| 0 / 64 / 64 | 0.9390 | 0.1568 | 0.7821 | (identical to all-64) |
| Karma Hair defaults, 64 | (0.2011, 0.1270, 0.0597) | | | |
| Karma Hair defaults, Karma limits | (0.1976, 0.1266, 0.0597) | | | |
| MaterialX Chiang, 64 | 0.0435 | 0.0420 | 0.0015 | |

Hair events never count as diffuse. They count as reflect or refract by the same side rule as the LPE events (measured). Even at limit 64 the furnace keeps 0.939: in a dense ball, paths with more than 64 TT events are frequent. Nova must reproduce the classification exactly to match the default-limit image (0.522).

### 3.7 MaterialX `ND_chiang_hair_bsdf` in Karma

Single fibre, roughness from `chiang_hair_roughness(0.3, 0.3)`, cuticle 0.5 (no tilt) (measured):

| | a = 0 | a = 30 | a = 60 |
|---|---|---|---|
| furnace, no absorption | 0.0607 (RG 0.0566 direct, TG 0.0041 indirect) | 0.0396 | 0.0453 |
| furnace, absorption (0.2, 0.5, 1.0) | (0.0438, 0.0297, 0.0235) | (0.0219, 0.0078, 0.0025) | (0.0202, 0.0049, 0.0008) |
| furnace, ior 1 | 0.0298 (RG 0.0106, TG 0.0192 indirect) | | |
| six lights, absorption | C (0.0807, 0.0301, 0.0067), l2 (back) 0.0001 | C (0.1258, 0.0401, 0.0062) | |

- The transmission side never receives direct light: l2 = 0, and at ior 1 the direct part is only the reflection side (0.0106; the model gives 0.0107).
- The scattered remainder appears as a second, indirect event.
- At a = 0 the direct part equals the model's reflection side *without the R lobe* (0.0568 against 0.0566). At 30 and 60 degrees nothing simple fits.
- Karma Hair handles all of this correctly. So Karma's MaterialX hair node is not a reference; Karma Hair is.

---

## 4. Algorithms

### 4.1 Embree 4.4.1 curves and points (header, registry)

Build (vcpkg's CMake cache, `buildtrees/embree/config-x64-windows-static-md-rel-CMakeCache.txt.log`; `rtcore_config.h`): version 4.4.1, `EMBREE_GEOMETRY_CURVE` and `_POINT` ON, `EMBREE_MIN_WIDTH` OFF (`RTC_MIN_WIDTH 0`), `EMBREE_BACKFACE_CULLING_CURVES` OFF, `EMBREE_CURVE_SELF_INTERSECTION_AVOIDANCE_FACTOR` 2.0, filter functions ON, `RTC_MAX_INSTANCE_LEVEL_COUNT` 1.

From the API documentation (Embree README, sections rtcNewCurveGeometry and RTCCurveFlags):
- **Types:** `FLAT_*` (ray-facing ribbons, tessellated into quads at rate 4), `NORMAL_ORIENTED_*` (Bezier, B-spline, Hermite, Catmull-Rom; no linear), `ROUND_*` (linear, Bezier, B-spline, Hermite, Catmull-Rom), `CONE_LINEAR` (no end spheres).
- **Buffers:** vertices are float4 (x, y, z, r) per control point; the index buffer holds the first control point of each segment; Hermite needs tangents, oriented curves need normals.
  - Bezier segments start every third point; B-spline and Catmull-Rom segments use four consecutive points and start every point.
  - Catmull-Rom spans p1 to p2 with tangents (p2 - p0)/2 and (p3 - p1)/2, i.e. uniform, not centripetal.
- **Round linear** curves are a cone between a start and an end sphere. The start sphere is drawn unless the left-neighbour flag is set; the end sphere is always drawn but clipped inside the next segment. Without a flags buffer, neighbours are inferred from consecutive indices.
- **Round cubic** curves are a sweep of a circle along the curve, without end caps. The radius must stay below the curve's radius of curvature.
- **Hit data:** u = segment parameter in [0, 1]; v = 0 for round and linear, [-1, 1] across flat curves, [0, 1] for oriented ones.
  - Ng is the unnormalised surface normal for round and oriented curves, and the curve tangent for flat ones.
- **Motion:** multi-segment motion blur through `rtcSetGeometryTimeStepCount` and one vertex (and normal) buffer per step, as for triangles.
- **Points:** `SPHERE_POINT`, `DISC_POINT` (camera-facing) and `ORIENTED_DISC_POINT`, each with radius per point. These match Karma's three point styles.
- **Self-intersection:** only flat curves have built-in avoidance (hits closer than 2 x radius to the ray origin are skipped). Round curves report hits from inside the tube, since back-face culling is off.

### 4.2 Self-intersection and ray offsets on round curves

Nova offsets ray origins by a few ULPs along the geometric normal (`core/scene/ray.h:33-50`, code). That works for triangles but not for curves:
- Embree's round-curve intersector solves the sweep surface iteratively, with an error that scales with the radius, not the coordinates.
- A hair BSDF sends about 90 % of its energy into the T side (3.4). Those continuation and shadow rays start on the near side of the tube and point into it, so they hit the back wall of the same segment.

Options:
- **(a) Exit point.** Start every hair continuation and shadow ray where it leaves the fibre:
  - R side: P + eps N;
  - T side: P + t_exit d, with t_exit = chord of a cylinder of the hit radius = 2 r (-d_perp . N) / |d_perp|^2, where d_perp = d - (d.T)T, plus eps;
  - eps relative to r (about 1e-3 r) plus Nova's ULP offset. Deterministic and filter-free, and it matches the far-field model (TT is a property of the BSDF, not a second hit).
- **(b) Self-exclusion.** An Embree argument filter rejects hits on the same (instance, mesh, segment) within the chord length. That is exact for the segment, but filters cost about 5 % of the rays per second even when idle (`core/accel/embree_accel.cpp:31-34`, code), and adjacent segments of a bent strand can still be hit.
- **(c) Start on the axis.** Start the ray at the axis point C(u) with tnear = r/|sin(d, T)|. Simple, but the tnear explodes for directions along the fibre.

Karma Hair evidently does one of these: a single fibre's furnace is exactly 1 with no second event (3.4). The help also hints that Karma's continuation uses the Ray Bias (0.001): "decrease Ray Bias if strands don't look wide enough" (help `nodes/vop/kma_hair`).

### 4.3 The Chiang 2016 model (lit, proto)

Chiang, Bitterli, Tappan, Burley, "A Practical and Controllable Hair and Fur Model for Production Path Tracing" (EGSR 2016), as implemented in pbrt-v3/v4 (`HairBxDF`). The prototype `probes/ch_hair_proto.cpp` implements it from the equations:

- **Geometry:**
  - local frame x = T (root to tip), y/z span the normal plane; sin(theta) = w.x, phi = atan2(w.z, w.y);
  - h in [-1, 1] across the fibre, gamma_o = asin(h);
  - the Bravais index eta' = sqrt(eta^2 - sin^2 theta_o)/cos theta_o and gamma_t = asin(h/eta').
- **Attenuation:**
  - f = Fresnel(cos theta_o cos gamma_o, eta);
  - T = exp(-sigma_a 2 cos gamma_t / cos theta_t);
  - A_R = f, A_TT = (1-f)^2 T, A_TRT = A_TT f T, A_TRRT+ = A_TRT f T / (1 - f T).
  - With sigma_a = 0, sum A_p = 1 exactly (a telescoping sum).
- **Longitudinal M_p (d'Eon et al. 2011):** exp(-sin theta_i sin theta_o / v) I0(cos theta_i cos theta_o / v) / (2 v sinh(1/v)), with a log form for v <= 0.1.
  - Variances v_R = (0.726 beta_m + 0.812 beta_m^2 + 3.7 beta_m^20)^2, v_TT = v/4, v_TRT = v_TRRT = 4v.
  - The cuticle tilt alpha shifts the lobes by -2 alpha, +alpha and +4 alpha (Marschner 2003).
- **Azimuthal N_p:** a logistic trimmed to [-pi, pi] around Phi(p) = 2 p gamma_t - 2 gamma_o + p pi, with s = sqrt(pi/8)(0.265 beta_n + 1.194 beta_n^2 + 5.372 beta_n^22). TRRT+ is uniform, 1/(2 pi).
- **f |cos theta|** (solid angle) = sum_p M_p A_p N_p.
- **Sampling:** pick p proportional to A_p; theta_i by d'Eon's sampler of M_p; phi by inverting the trimmed logistic. pdf = sum_p (A_p / sum A) M_p N_p.
- **Colour inversion** (eq. 9): sigma_a = (ln c / (5.969 - 0.215 beta_n + 2.532 beta_n^2 - 10.73 beta_n^3 + 5.574 beta_n^4 + 0.245 beta_n^5))^2.
- **Melanin** (d'Eon 2011): sigma_a = eumelanin x (0.419, 0.697, 1.37) + pheomelanin x (0.187, 0.4, 1.05).

Prototype results (`probes/ch_out_proto_tests.txt`):
- **White furnace by quadrature.** For fixed (wo, h) the lobes separate into A_p x the integral of M_p over sin theta_i x the integral of N_p over phi, so this tests the model, not the sampler. Over beta_m, beta_n in {0.1, 0.3, 0.6, 1}, alpha in {0, 2 degrees} and theta_o in {0, 30, 60, 80}, the albedo is within **0.18 %** of 1 with a 24-term I0 series.
  - pbrt's 10-term series under-estimates I0 near x = 12, where the asymptotic form takes over. It loses up to 0.37 % (beta_m 0.3: M_TT integrates to 0.9947).
  - Karma's measured lobes follow the 10-term variant, and its furnace is still 1.0000. That is consistent with the d'Eon sampler drawing from the exactly normalised M_p, so that BSDF-sampled weights are exactly sum A_p.
- **Sampling consistency.** E[f/pdf] agrees with a uniform-sphere estimate of the integral of f (2M samples) within its noise (<= 0.8 %). The pdf integrates to 1 within 0.9 % (Monte Carlo noise of uniform sampling).
- **Chi-square.** Equal-area bins in (sin theta_i, phi_i), 24 x 48, 2M samples, low bins pooled, 8 configurations: p = 0.046-0.839, all pass at 1 %.
- **Karma.** The predictions in 3.4 and 3.5 reproduce Karma Hair; see the tables there.

Related models (lit):
- **Arnold's `standard_hair`:** its controls are base, base_color, melanin, melanin_redness, melanin_randomize, roughness (+ azimuthal, anisotropic), ior, shift, tints for specular/transmission, and diffuse. Karma Hair's parameter set mirrors it nearly one to one. Arnold documents it as a d'Eon/Chiang-style R/TT/TRT model with a residual and an optional diffuse lobe; its exact melanin mapping is not public.
- **RenderMan's `PxrMarschnerHair`:** a Marschner 2003 model (R/TT/TRT with glints, cone shifts, per-lobe gains), not energy-conserving by construction. Pixar's newer `PxrHairColor` + Chiang-style looks are the closest relatives.
- Neither changes the plan: Karma Hair is pbrt's Chiang model, and Nova should match it.

---

## 5. Recommendations for Nova

### 5.1 Data path: Hydra curves -> core curve buffers -> Embree

**Recommended: curves as a mesh shape (`MeshShape::Curves`), the shape already appearing in the working tree.** The alternative, a separate `CurvesRecord`/accel API, would duplicate the record machinery: materials, attributes, instances, LPE tags, ray limits, cryptomatte, motion and refit. A curve prim is a `MeshDesc` with per-step float4 control points (radius in w), segment start indices and a segment-to-curve table, and it reuses the BLAS-per-prim + instance-array scheme unchanged.

Rules from the measurements, with notes on the in-progress code:
1. **Expansion on the host.**
   - Pinned B-spline and Catmull-Rom: phantoms `2 P0 - P1` and `2 Pn-1 - Pn-2`, for positions, radii and vertex primvars alike. The in-progress `hydra/curves.cpp:42-98` does this (code). Do not use `HdsiPinnedCurveExpandingSceneIndex`.
   - Periodic: repeat the first 3 (cubic) or 1 (linear) points.
   - Varying primvars and widths map to segment ends.
2. **Widths.** Radius = width/2 x `width_scale`, from the render setting or the constant primvar `karma:object:width_scale`. No minimum width. Keep Embree's minimum-width feature off, as Karma has none.
3. **Style.** Read `karma:object:curve_style` (render setting and constant primvar), plus a `nova:` alias if wanted, with Karma's default **Rounded**:
   - 0: `FLAT_*`, with N = -ray direction at shading;
   - 1: `ROUND_*`, normals ignored;
   - 2 and 3: `NORMAL_ORIENTED_*` for cubic curves; linear curves fall back to `FLAT_LINEAR`.
   - **The in-progress `hydra/curves.cpp:263-275` makes every curve with normals an oriented ribbon. That contradicts Karma's default (measured round tubes) and should change.**
   - Houdini curves with `N` from SOPs would otherwise render as ribbons in Nova and as tubes in Karma.
4. **Basis.** linear -> `ROUND_LINEAR_CURVE` with neighbour flags (the in-progress code sets them, `core/accel/embree_accel.cpp:154-161`), so ends get spheres and joints do not. bezier / bspline / catmullRom -> the matching Embree types.
   - `centripetalCatmullRom` has no Embree type. Convert it to Hermite segments with centripetal tangents, or render it as uniform Catmull-Rom with a warning; Houdini never authors it.
   - `karma:object:curve_basis` (Force ...) can be honoured later; it is a topology reinterpretation.
5. **`nurbsCurves`.** Karma renders the control hull as round linear curves. Either list `nurbsCurves` and build linear curves through the CVs (exactly Karma), or register `HdsiNurbsApproximatingSceneIndex` like Storm. Its output was not inspected, so the first option is the safe one. `HermiteCurves`: nothing to do (Hydra drops them).
6. **`points`.** Add `HdPrimTypeTokens->points` with `SPHERE_POINT` (default), `DISC_POINT` and `ORIENTED_DISC_POINT` by `karma:object:point_style`; radius = width/2 x width_scale. Nova already uses `SPHERE_POINT` for UsdGeomSphere (`core/accel/embree_accel.cpp:223`, code).
7. **Motion and instancing.** Points, widths and normals are sampled over the shutter like mesh points (`hydra/motion.h:80-97`). Embree takes one float4 vertex (and normal) buffer per step. Point instancers work as for meshes (probe: `instancerId` set).
8. **Refit.** Grooms deform every frame. The mesh refit path (same counts, new points) applies to curve vertex buffers too.

### 5.2 The hit: P, normals, tangent, parameters

`resolveHit` for a curve segment (Embree u = segment parameter):
- **Normals.**
  - Round: N = Ng = normalize(Embree Ng), the true tube normal including the radius slope.
  - Flat: N = Ng = -ray direction (Karma).
  - Oriented: Ng = Embree's, N = the interpolated authored normal (Karma reported the authored normal).
- **Tangent T** = dC/du normalised, root to tip (control-point order). Hair and anisotropic shading need it, so `SurfaceHit` needs a tangent field. `core/geometry/curves.h` already evaluates centre, tangent and radius (code).
- **`uv`** = (0, v_global) with v_global = (segment index within its curve + u) / segments of the curve: Karma's `ray:hituv`. Linear end caps clamp to 0 and 1.
  - For texture footprints only the v direction exists: dvdP along T, |dC/du| x segment count.
- **Ids.** `prim` for AOVs (`ray:primid`, `ray:element`) = the curve index (`segmentCurve`), not the segment.
- **Attributes.** Constant; uniform = per curve; vertex = interpolated with the curve's basis at u (USD rule); varying = linear between the segment's ends.
- **Hair offset.** h = clamp(N . (z x T), -1, 1) with z = normalize(wo - (wo . T) T). This is the convention that reproduces Karma's per-pixel profile (3.4) and pbrt's Phi(p) signs. On flat ribbons this gives h = 0 everywhere; pbrt instead uses h = v across the ribbon. Karma's behaviour there was not measured.

### 5.3 The hair BSDF in Nova's closure model

**Recommended: four lobes of a new `LobeType::Hair` (p = R, TT, TRT, TRRT+) in the ordinary `ClosureList`.**

Why this fits:
- Nova builds closures for a fixed wo (`core/bsdf/closure.h:1-20`, code), so everything wo-dependent can be computed once at the hit. Each lobe then needs only {p, v_p, s, sin theta_o,p, cos theta_o,p (or the theta_i tilt k alpha), Phi_p + phi_o}: 5 floats.
- The lobe weight is tint_p x A_p (a Spectrum), which is exactly Nova's "weight x normalised scalar distribution" form.
- `push(..., estimate = luminance(tint_p A_p))` gives pbrt's lobe selection.
- The albedo AOV becomes sum tint_p A_p.
- The frame is (T, z x T, z) in a `ClosureList::frame` slot.

The alternative, one monolithic hair lobe that samples p internally, packs about 20 floats into the union and duplicates the mixture logic.

Required changes, all measured or derived above:
1. **Gating.** `evalClosure` gates lobes by the geometric side and by "frame agrees with geometry" (`closure.h:412-433`, code). Hair lobes reflect *and* transmit with any frame.
   - Skip both checks for `Hair`.
   - Set `transmit` in `sampleClosure` (`closure.h:490-493`) from the geometric side of the sampled wi instead of rejecting the sample.
2. **Event class.** Reflect if wi is on wo's side of Ng (the tube normal), else Refract. Never diffuse, never SSS. This is Karma's LPE and ray-limit rule (3.4, 3.6), not the lobe identity. `lpeDiffuse()` = false (G), the label from the side, and the light's *specular* multiplier.
3. **No medium.** `refract = false`, eta = 1: a TT path does not enter a medium (`closure.h` `s.refract` today means "dielectric with eta != 1").
4. **Ray cones.** Roughness about sqrt(v_p) for the spread.
5. **Fake caustics / shadows.** Hair is opaque to shadow rays; light passing through fibres is the TT lobe. `closureTransmittance` returns 0 for hair.
6. **Mp evaluation.** Use pbrt's 10-term I0 series and log form for Karma-identical lobes (3.4), and the d'Eon sampler, so BSDF-sampled weights are exactly sum A_p as in Karma's furnace.
7. **Tilt.** Apply it to theta_i (R: theta_i - 2 alpha, TT: theta_i + alpha, TRT: theta_i + 4 alpha) with alpha = -shift x pi/2 for `kma_hair`. The sampler must sample the same rotated lobe: sample theta_i' about the untilted mirror, then rotate by -k alpha.

`kma_hair` translation (native, measured):
- sigma_a = absorption + chiang_inverse(base x baseColor, beta_n) + thicknessScale x (-ln(1 - m)) x [(1 - r) 1.2085 eu + r 1.833 pheo].
- beta_m = roughness; beta_n = roughnessAzimuthal if anisotropic, else roughness. TT/TRT variance scales roughness_ttscale / trtscale (defaults 0.25 / 4, equal to Chiang's).
- ior; tint as an overall weight.
- diffuse d: hair lobes x (1 - d) + Lambert(diffuseColor) x d on the tube normal.
- melanin/roughness randomize from a hash of the curve index (statistically equivalent; Karma's hash is unknown).
- coat: warn and ignore until measured (section 8).

`kma_fur`: Chiang lobes with medulla radius 0 (the help states equivalence), warnings for medulla, wetness and the second R lobe.

MaterialX `ND_chiang_hair_bsdf`: a `LeafKind::ChiangHair` with
- per-lobe (v, s) taken as given, s x sqrt(pi/8);
- tints per lobe (TRRT+ takes TRT's);
- alpha = (cuticle_angle - 0.5) pi with the spec's sign (values above 0.5 tilt toward the root, i.e. the same direction as Karma's positive shift);
- `curve_direction` -> T; `normal` defaulting to the hit normal.

Nova will differ from Karma here (3.7). Document it, and compare against `kma_hair` with the same parameters instead. On non-curve geometry, use T = dPdu and the same h formula. The VEX `chiang()` help says it is suitable only for curves; Nova need not refuse it.

### 5.4 Ray offsets

Use option (a) of 4.2 for all rays leaving a round-curve hit:
- reflection side: P + (1e-3 r + ULPs) N;
- transmission side: the cylinder exit point along d plus the same epsilon;
- shadow rays the same, per light direction.

Flat curves keep Embree's built-in 2r avoidance and need no offset beyond the ULPs. Oriented ribbons are thin sheets: offset along +-Ng as for triangles.

Add a unit test: a single fibre in a white furnace with Karma Hair and no absorption must give 1.000 +- 0.002 with **no second scattering event**, which Nova's path statistics or an LPE `C<.G>.+L` can check.

### 5.5 AOVs and LPEs

- Beauty components: hair events are `<RG>` / `<TG>` by side; Karma Hair's diffuse is `<RD>`.
- Per-type limits: reflect / refract by side.
- First-hit AOVs on curves:
  - `ray:hitN` = `ray:hitNg` = tube normal (round) / view-facing (flat) / authored (oriented);
  - `ray:hituv` = (0, v);
  - `ray:primid` = `ray:element` = curve index;
  - `P` on the tube surface.
- Default material on curves: Nova's existing default (docs/design/M3.md K19) with the tube normal. Karma's own default ignores `displayColor` and is a 0.81 x 0.5 diffuse plus glossy (3.3); that difference already exists for meshes.

---

## 6. Test plan with Karma reference numbers

All scenes come from `probes/ch_karma_scenes.py` (orthographic, box filter, path traced). Karma values are in `probes/ch_out_karma_measure.txt` and the model in `probes/ch_out_proto_predict_karma.txt`. Tolerances are suggestions: 1 % for exact quantities, 2-3 % for sampled ones.

| Test | Scene | Quantity | Karma | Model / tolerance |
|---|---|---|---|---|
| width = diameter | `geo_width` | projected width w 0.2 / 0.0025 / 0.0005; width_scale 2 | 0.2000 / 0.00249 / 0.00050; 0.4000 | exact, 1 % |
| point | `geo_width` | alpha area | 0.03142 | pi r^2, 1 % |
| caps | `geo_caps` | area beyond the end, linear / cubic | 0.003927 / 0 | 2 % / 0 |
| styles | `geo_ribbon` | projected width unset/0/1/2/3/linear+2 | 0.2/0.2/0.2/0.1/0.1/0.2 | 1 %; N as in 3.2 |
| bases and pinned | `geo_basis_*` | max distance of covered pixels to the reflected-phantom curve; area | 0.0100-0.0103; 0.04673 / 0.02099 / 0.06205 | <= 0.0114; 1 % |
| NURBS | `geo_ids` | distance to the control hull | 0.0101 | <= 0.0114 |
| ids, uv | `geo_ids`, `geo_shade_side` | primid / element / hituv | 0,1,2 / 0,1,2 / (0, 0.1639), (0, 0.7194) | exact / 1e-3 |
| default shading | `geo_shade` | with Nova's default material: Lambert of the tube normal | (Karma differs by design) | analytic rho cos/pi |
| hair furnace | `fur_kma_s0_a{0,30,60}` | C; RG | 1.000; 0.1276 / 0.1479 / 0.2833 | 1 +- 0.002; side rule 0.1270 / 0.1472 / 0.2841, 1 % |
| absorption | `fur_kma_sa_a*` | C | (0.6983, 0.4247, 0.2111), (0.6830, 0.4068, 0.2028), (0.6543, 0.3982, 0.2403) | sum A_p, 0.3 % |
| lobes | `dl_kma_sa_a{0,30}` | per-light AOVs | table 3.4 | 1.5 % (l2 +1 %) |
| roughness map | `dl_kma_rough_a*` | per light | table 3.4 | 1.5 % |
| tilt sign | `dl_kma_tilt_a*` | l3, l4 | (0.0328..), (0.0344..); a30 (0.0100..), (0.0202..) | 1.5 % |
| colour controls | `fur_kma_def`, `_m08`, `_base`, `_mel_*`, `_combo`, `_base05`, `_tint`, `_thick_*` | C | 3.4 / 3.5 tables | 0.5 % |
| diffuse | `dl_kma_diffuse_a0` | l0, l1, l2 | 0.1250, 0.0398, 0 | 1 % |
| limits | `ball_high`, `ball_default`, `ball_refl0`, `ball_refr0`, `ball_diff0` | C | 0.9390, 0.5224, 0.4755, 0.2154, 0.9390 | 2 % (same fibres) |
| realistic | `ballkmadef_default` | C | (0.1976, 0.1266, 0.0597) | 2 % |
| MaterialX hair | `fur_mx_s0_a0` | C | Karma 0.0607, Nova ~1.000 | documented difference |
| self-intersection | `fur_kma_s0_a0` | indirect (`C[DGS].+L`) | 0.0000 | < 1e-3 |
| prototype | unit tests | furnace by quadrature, chi-square | | <= 0.2 %; p > 0.01 |

Times: Karma CPU took 3-8 s per fibre scene and 20-160 s per hair ball (`probes/ch_out_karma_times.json`).

---

## 7. Probes and files

All under `docs/research/m9/probes/`:

| File | Purpose |
|---|---|
| `ch_author_curves.py` | hython: SOP curves, NURBS, Bezier, circles, Hair Generate and points through SOP Import; prints the authored USD (`ch_out_authored_usd.txt`) and saves the stage for the Hydra probe |
| `ch_curves_probe/` (`ch_probe_delegate.cpp`, `CMakeLists.txt`, plugin files) | HDK-free Hydra delegate "CurvesProbe" logging basisCurves / points / nurbsCurves rprims, primvars, the scene-index schema and materials; built like M8's probe in a scratch directory; output `ch_out_hydra_probe.txt` |
| `ch_karma_scenes.py` | writes the calibration scenes (`geo_*`, `fur_*`, `dl_*`, `ball_*`, `probe_hydra`) and `scenes.json` |
| `ch_karma_renders.ps1` | renders them with Karma CPU through `husk_env.ps1`; times in `ch_out_karma_times.json` |
| `ch_karma_measure.py` | reads the multi-part EXRs (every subimage) and prints all measurements (`ch_out_karma_measure.txt`) |
| `ch_hair_proto.cpp` | stand-alone Chiang implementation. Modes: `tests` (`ch_out_proto_tests.txt`), `predict` (`ch_out_proto_predict.txt`; pbrt 10-term: `_i0_10.txt`; Karma conventions with `CH_I0_TERMS=10 CH_TILT_WI=1`: `_karma.txt`), `invert`, `whatif`, `profile` (`ch_out_proto_profile.txt`). `ch_out_karma_vs_model.txt` compares Karma and the model per light. Build: `cl /O2 /std:c++20 /EHsc` from the VS 2022 Build Tools x64 prompt. |

Reproduce:
1. `run_hython.ps1 -Script ch_author_curves.py <dir>`
2. `python -I ch_karma_scenes.py <dir> <dir>/ch_authored.usda`
3. `ch_karma_renders.ps1 -Dir <dir>`
4. `python -I ch_karma_measure.py <dir>`
5. Build `ch_hair_proto` and run `ch_hair_proto predict`.

---

## 8. Open questions

1. **Karma Hair `coat`.** coat = 1 adds an untinted, longitudinally very sharp lobe on both sides (l0 6.5, l2 7.0 per unit irradiance). Its model (a separate lobe? reduced v of R and TT?) needs a sweep over coat and roughness before Nova can match it. Until then: warn.
2. **Karma's melanin constants.** The factors 1.2085 (eumelanin) and 1.833 (pheomelanin) fit to 0.1 % over melanin 0.1-0.95, but their origin is unknown. Open as well: does `thicknessScale` also scale the absorption and colour terms, and which beta_n inverts the colour with anisotropic roughness on?
3. **The TT peak** is 0.8-1.0 % brighter in Karma than in the model, uniformly across channels. The cause is unknown (an M_p normalisation for small v?). It is within the test tolerance.
4. **Hair on flat ribbons (style 0) and oriented ribbons:** how Karma derives h there (0, or v across?) was not measured.
5. **Ray bias on thin strands:** Karma's help ties the visible width of thin strands to Ray Bias. A fibre narrower than 0.001 in a dense groom was not tested. Nova's exit-point offset should not need a bias.
6. **Houdini Procedural: Hair with a third-party delegate:** whether husk/Solaris run the procedural for Nova, so that generated hair reaches Nova at all, was not probed.
7. **Curve motion blur in Karma** (deformation of points and widths, velocity blur of curves) was not measured; the Hydra path is the meshes' one.
8. **`karma:object:curve_basis` overrides, `centripetalCatmullRom`, `segmented` wrap:** not measured; Houdini does not author them.
9. **Vertex primvars on cubic curves** (basis vs linear interpolation in Karma) and the `st` fallback on curves without `st` were not measured.
10. **Should Nova's `ND_chiang_hair_bsdf` follow the spec (recommended) or reproduce Karma's MaterialX behaviour (3.7)?** That is a decision for the design document.
