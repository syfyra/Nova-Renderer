# M4 research: Karma shading nodes (`kma_*`), the Karma Material Builder, and what Nova needs

Houdini 22.0.466, Karma CPU (`husk --renderer BRAY_HdKarma`). Every number below was measured in a
separate, isolated `hython`/`husk` process (own `HOUDINI_USER_PREF_DIR`, no packages, `HOUDINI_PACKAGE_SKIPLIST=nova`).
The user's Houdini session was never touched. SideFX VEX/JSON content is described in my own words, never copied.

`$HFS` = `C:\Program Files\Side Effects Software\Houdini 22.0.466`. The measurement scripts (`harness.py`, `t_*.py`,
hand-written `.usda` scenes) lived in the session scratchpad `m4research/karma-nodes/` and are not committed; every
setup is described well enough below to rebuild it.

## 0. Key facts (TL;DR)

1. Houdini 22.0.466 ships **48 Karma VOP types** (`kma_*`). Karma's own shader registry lists **55 `kma_*` shader ids**
   (`$HFS/houdini/karmaShaderNodes.json`, read by `BRAY_SdrKarmaDiscovery.dll`), and its graph library holds **287 graphs**
   (`$HFS/houdini/karmaNodeGraphs.json`, read by `libBRAY.dll`). Only **one** `kma` node is a real MaterialX nodedef on disk:
   `ND_kma_camerafalloff` (`$HFS/houdini/materialx/libraries/houdini/kma_camerafalloff.mtlx`).
2. **Karma's node set is closed.** A custom nodedef + nodegraph in `PXR_MTLX_PLUGIN_SEARCH_PATHS` fails in Karma with
   `Unhandled node type t ND_novatest_double_color3 in material` (measured). Nova-only nodedefs can **never** render in Karma.
   USD **NodeGraph prims** made of known nodes do render (that is how Houdini exports subnet HDAs like `kma_pyroshader`).
3. The **Karma Material Builder** is a plain `subnet` VOP with spare parms (`shader_rendercontextname = kma`). It writes
   `outputs:kma:surface`, `outputs:kma:displacement` (always connected, even when unused), optional `outputs:kma:volume`,
   plus an auto-generated `UsdPreviewSurface` on the universal `outputs:surface`. Default surface: **MtlX Standard Surface**
   (not OpenPBR) in 22.0.466.
4. Three hidden USD conventions matter for the translator: **(a)** `ND_dot_<type>` with `inputs:note = "ray:<var>"` is a
   **ray import** in Karma (measured), used by `ND_kma_camerafalloff`, `kma_rayimport` 1.0, the pyro shader and the quick materials;
   **(b)** `kma_material_properties` becomes `inputs:karma:<property>` on the surface shader; **(c)** `kma_aov` 2.0 becomes
   `inputs:aov:<name>` on the surface shader + a Material output + a `RenderVar` (sourceType `raw`).
5. **`kma_rayswitch` ray types (measured): 0 camera, 1 diffuse (incl. sheen), 2 glossy reflection (specular, coat, metal at
   roughness 0 and 0.6), 3 glossy transmission, 4 SSS/diffuse transmission, 5 volume, 6 shadow.** The help page's enumeration
   (1 = shadow, 2 = diffuse, ...) is wrong for 22.0.466. The hidden `shadow` input drives opacity on shadow rays (fake shadows).
6. Ray-query nodes measured analytically: `kma_ao` = **cosine-weighted** hemisphere/cone (cone angle = half angle, degrees),
   visibility = fraction of unoccluded rays within `maxdistance`; `kma_distance` = **uniform** hemisphere, mode 1 = mean
   distance over hits, mode 0 = minimum; `kma_curvature` = "thickness within `maxdistance`" probe on both sides of the surface,
   matching a closed formula to 3 digits on a 90-degree edge. `kma_roundededge` blends normals within `radius` of an edge.
7. Houdini's shipped **basic_materials.usd (120 materials)** uses `ND_kma_camerafalloff` (19x), `kma_rampconst_float` (23x),
   `kma_color_hextiling` (17x), `kma_voronoinoise3d_float` (2x); **quick_materials.usd** uses `kma_volume`, `kma_ramp_*`,
   dot-notes `ray:isshadowray`/`ray:level` and `inputs:karma:volumelimit`. In M3 Nova renders all of these with zeros.

## 1. Sources and method

| # | Source | What |
|---|---|---|
| S1 | `$HFS/houdini/karmaShaderNodes.json` | 966 shader entries (889 `ND_*`, 55 `kma_*`, USD preview/primvar/light ids); inputs, defaults, outputs |
| S2 | `$HFS/houdini/karmaNodeGraphs.json` | 287 graph definitions Karma expands itself (MaterialX stdlib graphs + 7 `kma_*`) |
| S3 | `$HFS/houdini/materialx/libraries/houdini/kma_camerafalloff.mtlx` | the only `kma` MaterialX nodedef (l. 3-12) and its graph (l. 13-49, dot-notes l. 36-46) |
| S4 | `$HFS/houdini/python3.13libs/voptoolutils.py` | builder masks l. 2462-2466, `_addMtlSubnetParms` l. 2500-2529, `_setupMtlXBuilderSubnet` l. 2531-2595 |
| S5 | `$HFS/houdini/toolbar/ExtraTools.shelf` l. 334-343 | tool `vop_karmamtlxsubnet` "Karma Material Builder" |
| S6 | `$HFS/houdini/python3.13libs/loptoolutils.py` l. 1645-1689 | Global AOV material (new in 22) |
| S7 | `$HFS/houdini/help/nodes.zip:vop/kma_*.txt`, `solaris.zip:kug/materials.txt`, `support/karma_ray_switch.txt`, `karma_xpu.txt`, `news.zip:21/*.txt, 22/*.txt` | documentation |
| S8 | `$HFS/houdini/usd/materials/{basic_materials,quick_materials}/*.usd`, `$HFS/houdini/usd/karma/kma_camera_lens.usd` | info:id statistics (pxr traversal) |
| S9 | hython: `hou.nodeTypeCategories()['Vop']`, definitions, `FunctionName` sections; 32 test materials in a Material Library LOP, `stage.ExportToString()` | inventory and USD form |
| S10 | husk / Karma CPU on hand-written `.usda` scenes (box filter scale 1e-6, `colorlimit` 1e9, 16-64 px samples, ortho cameras, unlit probes) | semantics |
| N | Nova: `hydra/material/translator.cpp` l. 507-510 (unknown node -> 0), l. 517-519 (inputs containing `:` skipped), l. 748-749 (`dot` = passthrough); `core/shading/ir.h` l. 40-44 (globals); `docs/design/M3.md` l. 67-69, 92 | current state |

## 2. Inventory (Q1)

"Kind": **native** = implemented inside Karma (opaque; VOP has render mask `karma` and a `FunctionName` section, the USD
`info:id` is the function name plus a signature suffix); **graph** = Karma expands it from S2 (re-expressible with MaterialX
nodes); **mtlx** = MaterialX nodedef on disk; **NodeGraph** = subnet HDA exported as a USD NodeGraph prim of other nodes;
**convention** = no shader id, encoded as inputs/outputs. "info:id" is what the Material Library LOP wrote in my export (S9).

### 2.1 Surface-graph nodes (pattern, utility, ray queries)

| VOP (label) | since | USD info:id | kind | I/O (defaults) |
|---|---|---|---|---|
| `kma_ao` (Ambient Occlusion) | 22.0 | `kma_ao` | native | nN, samples 4, coneangle 90, maxdistance 1e38, traceset `*`, frontface 1 -> visibility |
| `kma_distance` | 22.0 | `kma_distance` | native | mode 0, nN, samples 16, coneangle 90, maxdistance 1, traceset, frontface 1, objectspace 0, normalize 1 -> dist, dir |
| `kma_curvature` | 22.0 | `kma_curvature` | native | nN, samples 16, maxdistance 0.1, space 0, convexscale 1, convexbias 0.5, concavescale 1, concavebias 0.5 -> curvature, convex, concave |
| `kma_roundededge` | 19.5 | `kma_roundededge` | native | nN, mode 0, samples 4, radius 0.01, cuspangle 30, scope `scope:self` -> roundN, roundMask |
| `kma_rayswitch` | 21.0 | `kma_rayswitch_float` / `_color` | native | camera, diffuse, glossy_reflection, glossy_transmission, sss, volume (inputs) + `shadow` (parm only, all default 1) -> result, raytype (int) |
| `kma_rayimport::2.0` | 21.0 | `kma_rayimport_{bool,int,float,vec2,vec3,vec4,mat2,mat3,mat4}` | native | name (string), in (fallback) -> out |
| `kma_rayimport` (1.0) | 19.5 | NodeGraph containing `ND_dot_<type>` with `note = "ray:<var>"` | NodeGraph | same |
| `kma_camerafalloff` | 20.0 | `ND_kma_camerafalloff` | mtlx | bias .5, gain .5, gamma 1, invert 0, forward 1, basecolor (0,1,0), edgecolor (1,0,0) -> falloff, color |
| `kma_rampconst` | 20.0 | `kma_rampconst_color` / `_float` | native | t; vkeys/vvalues/vbasis or fkeys/fvalues/fbasis arrays -> out |
| (no VOP; "Karma Ramp Parameter" tool = `rampparm` with render mask `karma`, ExtraLopTools.shelf l. 541-557) | - | `kma_ramp_color` / `_float` | native | t, keys, values, basis -> out |
| `kma_stderiv` | 20.5 | `kma_stderiv_{float,vector2,vector}` | native | input -> derivS, derivT, derivScale |
| `kma_deriv2d` | 21.0 | `kma_deriv_2d_{float,vector2,vector}` | graph (on `kma_stderiv`) | input0, input1 (vector2) -> dIn0_dIn1x, dIn0_dIn1y |
| `kma_tangentrotate` | 21.0 | `kma_tangentrotate` | graph | amount 0, tangent (default Tworld), axis (default Nworld) -> out |
| `kma_voronoinoise2d` / `3d` | 19.5 | `kma_voronoinoise{2d,3d}_{float,vector2,vector3}` | native | texcoord/position, jitter, metric (0 euclid, 1 dist^2, 2 manhattan, 3 chebyshev), freq, offset -> distance (F1[,F2,F3]), p1..p3 |
| `kma_hextiled_texture` | 20.0 | `kma_color_hextiling` / `kma_normalmap_hextiling` | native | file, texcoord, size, scale, rand_scale, rot, rand_rot, contrast, contrast_falloff, weightexp, seed, sourceColorSpace, normal/tangent/bitangent/direction (+height, invert, space for normals) -> out (vector4), weights |
| `kma_hextiled_triplanar` | 20.0 | `kma_hextiled_triplanar_color` / `_normal` | graph (on `kma_color_hextiling`) | as above + blend, position (Pobject), normal (Nobject) |
| `kma_ocio_transform` | 19.5 | `kma_ocio_transform` | native | mode, from, fromspace, tospace, looks, display, view -> to |
| `kma_melanin` | 21.0 | `kma_melanin` | graph | melanin, redness, pheomelanin, scale, mode -> color, absorp |
| `kma_pcread` | 20.0 | `kma_pcread` | native (empty render mask) | file (.pc/.bgeo), attributes, maxpoints, radius, falloff, blendmode -> result |
| `kma_roommap` | 20.0 | `kma_roommap` (registry also has `kma_room_multimap`) | native | interior mapping -> out, normal, texcoord |
| `kma_aov` (1.0) | - | `kma_aov` (not in S1) | native passthrough | input -> output named after the AOV (`outputs:myfalloff1`) |
| `kma_aov::2.0` | 20.0 | none | convention (section 3.3) | input -> nothing in the graph |
| `kma_material_properties` | 20.0 | none | convention (section 3.3) | 31 activatable properties |
| `kma_fakecaustics` (hidden) | - | NodeGraph = `ND_dot_surfaceshader` passthrough | NodeGraph | the inner graph only passes the surface through; fake caustics are now material properties |
| `kma_nesteddielectrics` (hidden, deprecated) | 19.5 | NodeGraph = `ND_dot_surfaceshader` passthrough | NodeGraph | ndior/ndpriority not written |

### 2.2 Terminal shaders (surface on curves, volume)

| VOP | since | USD | kind | Notes |
|---|---|---|---|---|
| `kma_hair` | 19.0 | `kma_hair` on `outputs:kma:surface` | native BSDF | 23 inputs (base, baseColor, melanin, melaninRedness, ..., roughness, roughnessAzimuthal, coat, diffuse, absorption) |
| `kma_fur`, `kma_fur::2.0` | 20.0 / 21.0 | `kma_fur`, `kma_fur_2` | native BSDF | + cuticle/cortex/medulla, wetness (2.0); mixable with `mtlxmix` on CPU since 22 |
| `kma_volume` | 19.5 | `kma_volume` on `outputs:kma:volume` | native | absorption, scattering, emission, anisotropy, secondarymix, secondaryanisotropy, extinctionmodifier, contributionmodifier, modifierstartlevel |
| `kma_whitewater` | 21.0 | `kma_whitewater` on `outputs:kma:volume` | native | volume + glossy reflection layer |
| `kma_pyroshader` | 19.0 | NodeGraph on `outputs:kma:volume` | NodeGraph | ~90 inputs; inner: MaterialX nodes, `kma_ramp_*`, `kma_volume`, dot-notes `ray:isshadowray`, `ray:level`, `mtlxgeompropvalue` field reads |
| `kma_pyro{smokecolor,scattercolor,scatteremission,firecolor,fireemission,volumemask}` | 19.0 | NodeGraphs | NodeGraph | pyro building blocks |
| `kma_pyropreview` | 19.0 | `kma_pyropreview` | native | XPU preview volume |

### 2.3 Not material shaders

Light filters `kma_lfilter_{attenuation, barndoor, blocker, gel, gobo}` and `kma_light_projection` (shaderType
`kma:lightfilter`); lens shaders `kma_physicallens` (`physicallens`, CVEX, `kma_camera_lens.usd`), `kma_physicallenscore`,
`kma_roomlens`, `kma_uvlens` (CVEX `.vfl` in `$HFS/houdini/vex/VopCVex`). Registry-only ids without a VOP: `kma_pathswitch`
(int choice -> one of 16 strings). `kma_rayhitlevelfalloff` has a help page but **no node type** in 22.0.466.

### 2.4 Which are MaterialX, which are opaque

- MaterialX nodedef on disk: `ND_kma_camerafalloff` only (S3).
- Expanded by Karma from its own graph library (S2), i.e. reproducible from MaterialX stdlib nodes: `kma_tangentrotate`
  (= `rotate3d(tangent, 360 * amount, axis)` with tangent/axis defaulting to the world tangent and normal), `kma_melanin`
  (Chiang et al. 2016 / d'Eon eumelanin and pheomelanin absorption, colour = exp(-sigma), `absorp` = -ln(clamp(color))),
  `kma_deriv_2d_*` (solves the 2x2 Jacobian: derivatives of input0 and input1 with respect to s,t from `kma_stderiv`, then
  inverts), `kma_hextiled_triplanar_*` (three `kma_color_hextiling` lookups blended by |N|^weightexp).
- Exported as NodeGraph prims of other nodes (translatable once their leaves are): `kma_rayimport` 1.0, `kma_pyro*`,
  `kma_fakecaustics`, `kma_nesteddielectrics`.
- Opaque Karma natives (Nova must implement or ignore): ao, distance, curvature, roundededge, rayswitch, rayimport 2.0,
  ramp/rampconst, stderiv, voronoinoise, color/normalmap hextiling, ocio_transform, pcread, roommap, aov 1.0, hair, fur,
  volume, whitewater, pyropreview, light filters, lenses.
- XPU supports (S7 `karma_xpu.txt`): rayswitch, rayimport, voronoi, aov, light projection, ocio, roundededge, roommap,
  hextiled texture/triplanar, stderiv; hair/fur on curves; volume/whitewater/pyropreview on volumes; no MaterialX PBR nodes.

## 3. How kma materials reach USD/Hydra (Q1, Q3)

### 3.1 Karma Material Builder (S4, S5; verified in hython)

The tab tool calls `voptoolutils.createMaskedMtlXSubnet(kwargs, 'karmamaterial', KARMAMTLX_TAB_MASK, 'Karma Material Builder', 'kma')`.
The result is a **`subnet`** VOP (no HDA) with shader language `MaterialX`, the material flag set, and spare parameters
in a "Karma Material Builder" folder: `inherit_ctrl` = 2 (Material Flag), `shader_referencetype` = inherit,
`shader_baseprimpath` = `/__class_mtl__/<name>`, `tabmenumask` = `karma USD ^mtlxUsd* ^mtlxramp* ^hmtlxramp* ^hmtlxcubicramp*
MaterialX parameter constant collect null genericshader subnet subnetconnector suboutput subinput`,
`shader_rendercontextname` = `kma`, `shader_forcechildren` = 1. (The mask hides MaterialX ramps; Karma ramps are
`kma_rampconst`.) Children: `inputs` (subinput), `mtlxstandard_surface`, `mtlxdisplacement`, `material_properties`
(`kma_material_properties`), `Material_Outputs_and_AOVs` (suboutput with inputs `surface`, `displacement`, `properties`, `next`).

Exported USD (default builder, S9):

```usda
def Material "kmb_default" (apiSchemas = ["MaterialXConfigAPI"])
{
    string config:mtlx:version = "1.39"
    token outputs:kma:displacement.connect = </materials/kmb_default/mtlxdisplacement.outputs:out>
    token outputs:kma:surface.connect = </materials/kmb_default/mtlxstandard_surface.outputs:out>
    token outputs:surface.connect = </materials/kmb_default/mtlxstandard_preview.outputs:surface>
    def Shader "mtlxstandard_surface"  { uniform token info:id = "ND_standard_surface_surfaceshader" ... }
    def Shader "mtlxdisplacement"      { uniform token info:id = "ND_displacement_float" }   # no inputs authored
    def Shader "mtlxstandard_preview"  { uniform token info:id = "UsdPreviewSurface" ... }   # customData HoudiniIsAutoCreatedShader
}
```

- `material_properties` writes nothing until a property is activated.
- **Displacement is always connected** to a default `ND_displacement_float` (displacement 0). Nova must fold this to "no
  displacement" (relevant for the M4 displacement work), otherwise every Karma-builder material would trigger it.
- A shader wired to the suboutput's next slot named `volume` becomes `outputs:kma:volume` (measured with `kma_volume`,
  `kma_whitewater`, `kma_pyroshader`). Hair/fur go to `outputs:kma:surface` (no preview shader is generated).
- Since 21.0 the suboutput accepts a "Shader Render Context Name" spare parm, so one builder can also write `hview`
  (viewport-only surface); since 22.0 the Visualize VOP adds an `hvisualize` output (husk skips those materials). Nova's
  chain `nova > kma > mtlx > ""` already ignores both.
- Global AOV material (22.0, S6): a Material Library with a Karma-builder subnet holding `mtlxsurface_unlit` +
  `kma_material_properties`, referenced by the Karma Render Settings `globalaovmaterial` parm.

**Promoted parameters** (S9): promoting `base_color` as a subnet input and `specular_roughness` through a Parameter VOP
both produce Material interface inputs, and every consumer connects to them:

```usda
def Material "kmb_promoted" {
    color3f inputs:base_color = (0.8, 0.8, 0.8) (displayName = "Base Color")
    float inputs:specular_roughness = 0.2 (displayName = "Specular Roughness")
    ... mtlxstandard_surface: color3f inputs:base_color.connect = </materials/kmb_promoted.inputs:base_color>
    ... mtlxstandard_preview: color3f inputs:diffuseColor.connect = </materials/kmb_promoted.inputs:base_color>
}
```

For the subnet input only, Houdini also stores a dialog script in the Material's `customData.houdini.dialogScript`
(parameter UI, presumably for downstream material editing). Nova's M3 network source already resolves interface connections.

### 3.2 VOP -> info:id rules

- Native VOPs: `info:id` = the definition's `FunctionName` (+ signature suffix): `kma_rayswitch` -> `kma_rayswitch_color`,
  `kma_rayimport::2.0` (float) -> `kma_rayimport_float`, `kma_hextiled_texture` -> `kma_color_hextiling`,
  `kma_fur::2.0` -> `kma_fur_2`, `kma_voronoinoise3d` (float) -> `kma_voronoinoise3d_float`. Authored inputs only
  (defaults are not written), names as in S1.
- Ramp arrays are plain array inputs: `float[] inputs:vkeys`, `color3f[] inputs:vvalues`, `token[] inputs:vbasis`
  (`kma_rampconst_color` also writes the unused float set and an `int inputs:vramp` key count).
- A plain `rampparm` inside a Karma builder exports an **empty Material** (measured, no error on the LOP).

### 3.3 Conventions the translator must know

**(a) Dot-note ray imports.** `ND_dot_<type>` with `inputs:note = "ray:<var>"` returns the Karma ray variable instead of
`in`; any other note (e.g. `shadowcolor`, used inside the pyro graphs) is a plain passthrough. Measured (unlit probe,
camera at z = 5 looking down -Z at a quad at z = 0):

| note | result |
|---|---|
| `ray:hitdist` | 5.0 (in = 0.25) |
| `ray:hitN` / `ray:direction` | (0,0,1) / (0,0,-1) |
| `ray:level` | 0 |
| `hitdist` (no prefix), `ray:nonexistent` | 0.25 (= in) |
| `shadowcolor` on `ND_dot_vector3` | (0.1,0.2,0.3) (= in) |

Nova today treats every `dot` as passthrough (`translator.cpp` l. 748-749), so `ND_kma_camerafalloff` computes
`facingratio(normalize(0), normalize(0))`.

**(b) Material properties.** Activated properties become inputs on the **surface terminal shader**, not on the Material:

```usda
def Shader "mtlxstandard_surface" {
    int inputs:karma:causticsenable = 1
    float inputs:karma:diffuselimit = 3
    string inputs:karma:lpetag = "mytag"
    float inputs:karma:mtlx_imagewidth = 2
}
```

Available (22.0.466 VOP): diffuse/reflect/refract/volume/sss quality, volumesteprate, diffuse/reflect/refract/volume/sss
limits, volumeuniform (+density, samples), lpetag, dielectricpriority, mtlx_imagewidth, mtlx_imageblur,
thinwallatrefractlimit, causticsenable, causticsroughnessclamp, fakecausticsbsdfenable, fakecausticscolor,
fakecausticsopacity, fakecausticsfresneldarken, treat_as_lightsource, lightsource_samplingquality,
lightsource_diffusescale, lightsource_specularscale. Help (S7): geometry properties win over material properties.
Nova skips every input containing `:` today (`translator.cpp` l. 517-519) - correct as a default, but M4 should read the
limits and fake-caustic properties (user decisions "per-ray-type depth limits" and "fake-caustic shadows" in M4).

**(c) AOVs.** `kma_aov::2.0` with name `myfalloff` wired to the suboutput:

```usda
def Material "m_aov2" {
    color3f outputs:myfalloff.connect = </materials/m_aov2/kma_camerafalloff1.outputs:color>
    def Shader "mtlxstandard_surface" { color3f inputs:aov:myfalloff.connect = </materials/m_aov2/kma_camerafalloff1.outputs:color> }
    def Scope "Vars" { def RenderVar "myfalloff" { token dataType = "color3f"; string sourceName = "myfalloff"; token sourceType = "raw" ... } }
}
# plus a reference copy under /Render/Products/Vars/myfalloff
```

`kma_aov` 1.0 stays in the graph as `info:id = kma_aov` whose output is named after the AOV (passthrough).
Both are M6 work; M4 only has to pass the value through and ignore `aov:*` inputs.

### 3.4 What Houdini's own material libraries use (S8)

| file | materials | kma / Karma-specific content |
|---|---|---|
| `basic_materials.usd` | 120 (119 `outputs:kma:surface`) | `kma_rampconst_float` 23, `ND_kma_camerafalloff` 19, `kma_color_hextiling` 17, `kma_voronoinoise3d_float` 2, `UsdUVTexture` 32, `karma:lightsource_{diffuse,specular}scale` 1 |
| `quick_materials.usd` | 2 (one `outputs:kma:volume`) | `kma_volume` 1, `kma_ramp_color` 3, `kma_ramp_float` 2, notes `ray:isshadowray`, `ray:level`, `karma:volumelimit` |
| `kma_camera_lens.usd` | 1 | `kma_physicallens` |

## 4. Measured semantics (Q2)

All scenes: Karma CPU, unlit probes (`ND_surface_unlit`) so the value is read straight from the beauty. Means over 256-1024
pixels; standard errors are below the last printed digit unless noted.

### 4.1 `kma_rayswitch` and ray types

Setup: test quad (z = 0) seen by an ortho camera; everything else is a radius-50 sphere around the scene whose unlit
emission is the ray switch output (`camera` red, `diffuse` green, `glossy_reflection` blue, `sss` yellow,
`glossy_transmission` cyan, `volume` magenta, `shadow` grey). The quad's material decides which ray reaches the sphere.
Second run: emission = `0.1 * raytype`. Pixel = probe value x lobe albedo.

| quad material (Standard Surface) | colour seen | raytype (value / albedo) |
|---|---|---|
| probe seen directly | red = camera | 0 |
| diffuse white (base 1, specular 0) | green = diffuse | 1 |
| sheen only | green, x0.052 | 1 |
| mirror metal r 0 | blue x0.99 | 2 |
| rough metal r 0.6 | blue x0.988 | 2 |
| dielectric specular / coat only | blue x0.04 | 2 |
| thin-walled glass r 0 / r 0.5 | cyan x1.0 / x0.916 | 3 |
| thin-walled subsurface (diffuse transmission) | yellow | 4 |
| uniform volume box (`karma:object:volumeuniform`, `kma_volume` scattering 1) | magenta x0.7675 | 5 |
| shadow rays (opacity = 0.1 raytype on a blocker, transmission 0.404) | - | 6 |

- The type is the **lobe class of the scattering event that spawned the ray** (not a roughness threshold), shadow rays are
  NEE/light rays. `ray:level` was 0 for camera hits and 1 after one bounce of any type.
- Shadow input (hidden on the VOP UI, written as `inputs:shadow`): blocker opacity = rayswitch(all 1, shadow s) over a
  ground lit by a 45-degree distant light: shadow region / lit = 0.06 (s = 1), 0.57 (s = 0.5), 1.07 (s = 0); each value
  includes about +0.06 of indirect light bounced off the lit blocker, so s acts as plain shadow-ray opacity.
  `ray:isshadowray` as opacity: blocker invisible to the camera, full shadow.
- S7's guide uses `raytype` + `ifequal` + `mix` of two surface shaders; Karma 21+ blends up to 16 surface shaders through
  `mtlxmix` (S7 news 21). In Nova that needs `mix_surfaceshader` (deferred to M4 in `M3.md` l. 67).

### 4.2 Ray imports (`kma_rayimport_*`, dot-notes)

Variables (S7 `kma_rayimport-2.0.txt`): `ray:curveattribP/TanV/width`, `ray:direction`, `ray:dPdz`, `ray:hitdist`,
`ray:hitN`, `ray:hitN_facingratio`, `ray:hitNg`, `ray:hitNg_facingratio`, `ray:hitP`, `ray:hitPz`, `ray:hituv`,
`ray:isshadowray`, `ray:level`, `ray:motionvector`, `ray:objectid`, `ray:backface`, `ray:origin`, `ray:pixelsamples`,
`ray:primid`, `ray:time`, `ray:velocity`; plus `fakecaustic:*` (enable, N, ior, transmission_color, transmission, opacity, bsdf).
Measured: `ray:hitdist` = length of the **current segment** (camera hit 5.0; mirror bounce to the sphere 49.2, not 54.2);
`ray:direction` = current ray direction, world space, towards the surface (= Nova's `I`); `ray:level` = bounce count;
`ray:isshadowray` = 1 on shadow rays, 0 otherwise. All values world space (S7).

### 4.3 `kma_ao`

Setup: floor (y = 0) under a parallel ceiling at h = 1, ortho camera between them looking down, unlit floor emission =
visibility, samples 16, 64 px samples. Analytic: occluded iff the ray's cosine to the normal c > h / maxdistance.

| case | measured | uniform hemisphere | **cosine-weighted** |
|---|---|---|---|
| maxdistance 2 | 0.2500 | 0.5 | **0.25** |
| maxdistance 4 | 0.0625 | 0.25 | **0.0625** |
| maxdistance 0.9 | 1.0 | 1 | **1** |
| coneangle 60, maxdistance 4/3 | 0.4166 | 0.5 | **0.4167** ((c^2 - cos^2 a)/(1 - cos^2 a), a = 60 as half angle) |
| coneangle 30, maxdistance 4 | 0.0 | 0 | **0** (cos 30 > 0.25: every ray hits) |
| nN connected = (0.707, 0.707, 0), frontface 1 | 0.4603 | - | **0.4602** (Monte Carlo of the same rule) |

- **Cosine-weighted** directions in a cone of half angle `coneangle` (degrees) around nN; visibility = unoccluded fraction
  with hits counted up to `maxdistance`. Unconnected nN = shading normal. Constant (unconnected but authored) nN values
  were ignored (Karma used N); connected values are honoured.
- `frontface = 1` flips nN towards the incoming ray (nN = (0,-1,0) gave 0.25). `frontface = 0` gave 0.668 (nN pointing
  away from the occluder) and 0.774 (45-degree nN), which no simple model explains - treat as unknown.
- With 64 pixel samples even `samples = 1` returns exactly 0.25 per pixel: Karma stratifies AO directions across pixel samples.

### 4.4 `kma_distance`

Same scene, maxdistance 2, normalize 1:

| case | measured | model |
|---|---|---|
| mode 1, samples 16 | 0.6928 | mean of L/D over **hits only**, uniform hemisphere: ln 2 = 0.6931 (cosine would give 0.667) |
| mode 1, samples 1 | 0.8459 | miss -> 1: 0.5 + 0.5 ln 2 = 0.8466 |
| mode 1, normalize 0 | 1.3856 | 2 x 0.6928 (world units) |
| mode 0, samples 1 / 4 / 16 / 64 | 0.8459 / 0.6924 / 0.5751 / 0.534 | minimum of the samples; i.i.d. uniform minimum would give 0.847 / 0.646 / 0.533 / 0.508 |

Uniform (not cosine) hemisphere; misses count as 1 (normalized). Mode 0 = minimum, but converges more slowly than an i.i.d.
minimum (Karma's sample pattern); `dir` was not measured.

### 4.5 `kma_curvature`

Spheres (exact Points spheres, radius R), viewed head-on; plane; inside of a sphere; 90-degree cube edges. Output depends
only on x = maxdistance / R (scale invariant); `space 0` = 0..1 with 0.5 flat, `space 1` = -1..1 with 0 flat:

| x = d/R | 0.1 | 0.2 | 0.5 | 1 | 2 | 2.5 | 4 | 5 | 10 | 20 |
|---|---|---|---|---|---|---|---|---|---|---|
| measured (space 1) | 0.012 | 0.034 | 0.104 | 0.228 | 0.481 | 0.582 | 0.735 | 0.786 | 0.889 | 0.943 |
| model below | 0.025 | 0.05 | 0.125 | 0.25 | 0.5 | 0.6 | 0.75 | 0.8 | 0.9 | 0.95 |

Model that fits: **convex** = average over N uniform directions on the **-N side** (into the surface) of
max(0, 1 - L/d), L = distance to the first hit (for a solid sphere the exit chord 2R cos); **concave** = the same on the
+N side; curvature (space 1) = convex - concave, space 0 = 0.5 + 0.5 (convex - concave). Sphere: x/4 (x <= 2),
1 - 1/x (x > 2). Exact check on the 90-degree cube edge at distance t (a = t/d): convex = 1/2 [(1 - a) + a ln a]:

| t from edge (d = 0.1) | 0.022 | 0.047 | 0.072 | 0.097 |
|---|---|---|---|---|
| convex edge, measured / model (space 0) | 0.612 / 0.612 | 0.544 / 0.544 | 0.511 / 0.511 | 0.500 / 0.500 |
| concave (inner) edge, measured / model | 0.388 / 0.388 | 0.457 / 0.456 | 0.489 / 0.489 | 0.500 / 0.500 |

- The sign follows the surface's **own** normal, not the face-forwarded one (inside of an outward-normal sphere still reads
  convex; inner edge of a cube with inward normals reads concave).
- Small x reads low (0.104 vs 0.125 at x = 0.5): grazing chords shorter than a ray offset that scales with object size are lost.
- Scale/bias were left at 1 / 0.5; the exact order of scale, bias and clamp is not measured.

### 4.6 `kma_roundededge`

Unit cube, profile across the right edge of the +Z face (t = distance to the edge), samples 4/16:

| t | 0.097 | 0.072 | 0.047 | 0.022 |
|---|---|---|---|---|
| radius 0.1: roundMask | 0.32 | 0.98 | 1.0 | 1.0 |
| radius 0.1: roundN.x (angle) | 0.005 (0.3 deg) | 0.137 (7.9) | 0.34 (19.9) | 0.556 (33.8) |
| radius 0.05: roundMask / roundN.x | 0 / 0 | 0 / 0 | 0.51 / 0.014 | 1.0 / 0.371 (21.8 deg) |

- Smooth, monotonic bend towards the neighbouring face within ~radius, about 45 degrees at the edge; mask = 1 where changed.
- `mode`: 0 = both, 1 = concave only (convex edge mask 0), 2 = convex only; concave inner edge works in mode 0.
- `scope` default `scope:self` (only the same object); `cuspangle` not measured.

### 4.7 Ramps: `kma_rampconst_*`, `kma_ramp_*`

Keys (0.2, 0.5, 0.8), values black, (0.25,0.5,1), white, lookups at t = 0.1, 0.35, 0.5, 0.65, 0.9: both node families
gave identical results and **honour the per-key basis** (the help says linear only): linear 0.125/0.25/0.5 at 0.35;
constant = previous key; catmull-rom (0.078, 0.219, 0.5) at 0.35 and overshoot 1.0625 at 0.65. Clamped outside the keys.
This is VEX `spline()` behaviour with per-key bases; Nova needs an array-valued ramp op (arbitrary key count).

### 4.8 `kma_camerafalloff`

Graph (S3): `hmtlxfacingratio(normalize(-ray:direction), normalize(ray:hitN), bias, gain, gamma, invert, forward)`,
`color = mix(edgecolor, basecolor, falloff)`. "Camera" is only true for camera rays: in reflections it uses the
reflected ray's direction. Needs only dot-note support plus `hmtlxfacingratio` (already lowered by Nova).

### 4.9 Hair, fur, volumes (not measured; M8/M9)

`kma_hair`/`kma_fur` are curve BSDFs (help: melanin-parameterized hair, Chiang-style lobes, fur adds cortex/medulla);
`kma_volume` = anisotropic VDF (two HG lobes blended by secondarymix) + uniform EDF, with an extinction/contribution
modifier from `modifierstartlevel` bounces on; `kma_whitewater` = volume + glossy layer; uniform volumes come from
`primvars:karma:object:volumeuniform` on a closed mesh (verified: renders, volume ray type 5).

## 5. Houdini 21/22 changes relevant to material-workflow parity (Q5)

- 22.0: `kma_ao`, `kma_distance`, `kma_curvature`, `kma_lfilter_attenuation` (and blocker); MaterialX 1.39.5 (flake2d/3d,
  hextiledimage, fractal2d, gltf anisotropy); render-settings "MaterialX Filter Width/Blur" multipliers (and the
  `karma:mtlx_imagewidth/imageblur` material properties); Global AOV material; Karma Uniform Volume recipe uses `kma_volume`;
  hair/fur mixable with `mtlxmix` on CPU; `hvisualize` outputs; husk options to disable scene materials/lights.
- 21.0: `kma_rayswitch`, `kma_rayimport` 2.0 (+ `ray:backface`), `kma_deriv2d`, `kma_tangentrotate`, `kma_melanin`,
  `kma_fur` 2.0, `kma_whitewater`, OpenPBR VOP, up to 16 surface shaders through `mtlxmix`, hidden `karma_blur`,
  `karma_pixelblur`, `karma_width` inputs on MtlX Image, MikkT normal-map recipe, suboutput render-context override (`hview`).
- The Karma Material Builder in 22.0.466 still starts with **MtlX Standard Surface** (S4 l. 2547); OpenPBR is one tab away.
- There is no `kma_userdata*` and no `kma_layer`: user data = MaterialX `geompropvalue`/`geompropvalueuniform` or
  `UsdPrimvarReader_*` (all in S1); layering = OpenPBR/Standard Surface lobes or `mix` of surface shaders.

## 6. What Nova needs (Q4)

### 6.1 Arnold workflow -> Karma/MaterialX equivalent

| Arnold | Karma 22 / MaterialX | Nova M3 state |
|---|---|---|
| aiStandardSurface / openpbr | `ND_standard_surface`, `ND_open_pbr_surface` | native |
| aiUserData*, aiState* | `geompropvalue(uniform)`, `UsdPrimvarReader_*`; `kma_rayimport_*` | primvars native; ray imports missing |
| aiRaySwitch | `kma_rayswitch_*` | missing |
| aiAmbientOcclusion | `kma_ao` (MaterialX `ambientocclusion` is a withdrawn proposal, `M3.md` l. 92) | lowered to 1 |
| aiCurvature | `kma_curvature` | missing |
| aiRoundCorners | `kma_roundededge` | missing |
| aiFacingRatio | `hmtlxfacingratio`, `ND_kma_camerafalloff` | first ok, second broken (dot-note) |
| aiRampRgb/Float | `kma_rampconst_*`, `kma_ramp_*`, hmtlx ramps | hmtlx only |
| aiTriplanar | `ND_triplanarprojection_*`, `kma_hextiled_triplanar_*` | MaterialX one ok |
| aiCellNoise | `worleynoise*`, `kma_voronoinoise*` | worley ok |
| aiMixShader, aiLayerShader | `ND_mix_surfaceshader` | M4 item (`M3.md` l. 67) |
| aiAovWrite* | `kma_aov` 2.0 (`inputs:aov:*`) | M6 |
| aiFlakes | `ND_flake2d/3d` | M4 item (`M3.md` l. 69) |

Every Arnold-style utility has a node Karma already understands. Authoring those (instead of Nova-only nodedefs) is the only
way a Nova-built material stays renderable in Karma, because Karma rejects unknown node types (TL;DR 2).

### 6.2 Mapping plan

**(a) Map at translation time (no new VM features beyond existing ops)**
- `ND_dot_*` with `note` starting `ray:` -> the matching global (below); other notes stay passthrough.
- `ND_kma_camerafalloff` (falls out of the above), `kma_tangentrotate` (rotate3d with Tworld/Nworld defaults),
  `kma_melanin` (closed formula), `kma_deriv_2d_*` (from the stderiv implementation), `kma_aov` 1.0 (passthrough),
  `kma_fakecaustics`/`kma_nesteddielectrics` NodeGraphs (already `ND_dot_surfaceshader` passthrough - check that the
  surfaceshader-typed dot reaches the surface model), `kma_ocio_transform` when the OCIO processor is a matrix (else warn).
- Terminal-shader inputs `karma:<property>`: read `diffuselimit`, `reflectlimit`, `refractlimit`, `volumelimit`,
  `ssslimit`, the fake-caustics set, `mtlx_imagewidth`/`imageblur` (multiply `nova:textureFilterWidth`); warn-once for
  the rest. Ignore `aov:*` until M6.
- `ND_displacement_float` with nothing connected and displacement 0 -> no displacement.

**(b) Native in the VM**
- New globals/path state: **RayType** (0..6 as in 4.1; 3 bits per path, carried on the ray queues and into the opacity
  filter so shadow/camera/indirect opacity programs see it), **RayDepth** (`ray:level`), **HitDist** (segment t),
  **IsShadow** (opacity programs), **Backface** (`dot(I, Ng) > 0`), Origin (= P - t I), objectid (gprim index);
  `I`, `N`, `Ng`, `P`, `PrimId`, `Time` exist. Defer `ray:velocity/motionvector/curveattrib*/dPdz/hituv` with a warning.
- `kma_rayswitch_*`: select by RayType (7-way select; the `raytype` output is the int). Cheap.
- `kma_rayimport_*`: global read + type conversion; unknown name -> `in`.
- `kma_ramp_*`, `kma_rampconst_*`: array ramp op (keys/values in the constant pool or parameter block, per-key basis,
  VEX spline semantics incl. catmull-rom end handling), shared with the Nova Ramp node.
- `kma_stderiv_*`: Nova's `Deriv` op along the two footprint vectors, re-expressed against d(st) (2x2 inverse);
  `derivScale` = sqrt of the st-area of the footprint.
- **Ray queries**: `kma_ao`, `kma_distance`, `kma_curvature`, `kma_roundededge` (section 6.3).
- Later in M4 if time allows: `kma_voronoinoise*` (F1-F3 + cell points, 4 metrics; needs golden values), hex tiling
  (`kma_color_hextiling`, `kma_normalmap_hextiling`, shared with MaterialX `hextiledimage`; parameterizations differ).

**(c) Ignore with one warning (fallback = nodedef/registry default)**
`kma_pcread`, `kma_roommap`/`kma_room_multimap`, `kma_pathswitch`, `kma_hair`/`kma_fur`/`kma_fur_2` (M9),
`kma_volume`/`kma_whitewater`/`kma_pyropreview`/pyro NodeGraphs (M8), light filters and lens shaders (not materials).

### 6.3 Ray queries from inside shading

Nova shades in the closure integrator's pass C, chunks of 32 hits per material through the batch executor
(`docs/design/M3.md`); `IAccel` already takes ray batches. Proposal: a **`RayQuery` op family executed per chunk**, not per lane:

1. Compiler: new ops `QueryAO(n, params)`, `QueryDist(n, params)`, `QueryCurv(n, params)`, `QueryRound(n, params)` with
   immediate query kind + sample count; never folded or hoisted (depend on P/N per lane), excluded from derivative cloning
   (`Deriv` uses the base value), kept by DCE only when consumed. When a program contains one, the executor runs the
   program up to that op for the whole chunk, issues one ray batch, then continues with the results.
2. Executor: for all live lanes build `samples` rays each (origin offset along Ng, directions from a new Sobol stream
   `kShadingQuery` per path sample so Karma-like stratification across pixel samples comes for free; cosine cone for AO,
   uniform hemisphere for distance/curvature, both hemispheres for curvature), call `IAccel::occluded` (AO) or
   `intersect` (others) once per chunk, reduce per lane, write the result row, continue.
3. Filters: `scope:self`/trace sets via the existing `IHitFilter` (geomID/instance test); AO rays use the opacity filter
   with RayType = shadow; queries never shade the hit (no recursion).
4. `kma_roundededge` additionally needs the hit's interpolated shading normal (primID + barycentrics -> vertex normals;
   the shading-point code already does this) and a blending rule (weights falling to 0 at `radius`, ~45 degrees at the
   edge, cusp test against `cuspangle`); match Karma by profile (4.6), not bit-exactly.
5. GPU (M10): the same op becomes a program split - kernel 1 writes query rays, an OptiX launch traces, kernel 2 resumes.
   The CPU design keeps that boundary explicit, so no rewrite.

Complexity: compiler + executor plumbing + sampler stream ~400-600 LOC; AO + distance + curvature ~250 LOC with analytic
tests (cosine law 4.3, ln 2 in 4.4, edge formula in 4.5); roundededge ~250 LOC plus a Karma profile reference. Roughly
4-6 working days for all four. Cost at render time: `samples` extra rays per shading point and query (Karma defaults 4/16).

## Recommended M4 scope (minimal)

1. **Translator conventions** (small, unlocks Houdini's shipped libraries): dot-note ray imports; `karma:*` terminal
   inputs for per-ray-type limits, fake caustics and texture-filter multipliers; `aov:*` ignored; zero-displacement folding;
   passthrough NodeGraphs and `kma_aov` 1.0; a coverage test that lowers every material in `basic_materials.usd` and
   `quick_materials.usd` without "unknown node" warnings except the deferred ones.
2. **Ray state**: RayType (Karma's measured 0..6), RayDepth, HitDist, IsShadow, Backface in path state and opacity filter ->
   `kma_rayswitch_*`, `kma_rayimport_*`, `ND_kma_camerafalloff`; references: the 4.1 ray-type probe and shadow test vs Karma.
3. **Pattern natives**: `kma_ramp_*`/`kma_rampconst_*` (array spline op), `kma_tangentrotate`, `kma_stderiv_*`,
   `kma_deriv_2d_*`, `kma_melanin`.
4. **Ray queries (CPU)**: `kma_ao`, `kma_distance`, `kma_curvature` with analytic tests; `kma_roundededge` second.
5. **`mix_surfaceshader`** (already an M4 item) so ray-switch shader blending works.
6. **Nova Material Builder** = Karma-builder-style subnet writing the `kma` context (plus an optional `nova` override
   output), tab mask = Karma's mask + Nova presets; extensions (AO, curvature, ray switch, user data, triplanar, ramp)
   authored as the Karma/MaterialX nodes of 6.1, so Karma renders the same material.
7. Everything in 6.2 (c), voronoi and hex tiling: warning once, documented in the coverage table.

## Open questions for the user

1. **Builder authoring target.** Author Nova's extensions as Karma nodes in the `kma` context (Karma renders them, other
   MaterialX renderers do not), or pure MaterialX in `mtlx` plus Nova-only nodes in a `nova` context (portable, but
   Karma shows a fallback without AO/curvature/ray switch)? Karma cannot load custom nodedefs, so there is no third option.
2. **Fidelity of the ray-query nodes.** Reproduce Karma's estimators exactly (cosine AO, uniform distance, the curvature
   probe of 4.5, Karma-like roundededge profile) or define Nova's own and document the differences?
3. **Hex tiling and Voronoi in M4?** They are needed for 19 of Houdini's 120 basic materials (`kma_color_hextiling` 17,
   `kma_voronoinoise3d` 2) and overlap with MaterialX `hextiledimage` (already deferred to M4); otherwise they stay warnings.
4. **Default surface of the Nova Material Builder:** Standard Surface (as Karma 22.0.466) or OpenPBR?
