# Reference image tests

Nova renders every scene in `scenes/` through husk and is compared with a
Karma CPU reference in `images/` (or with an analytic value). Karma is set up
to compute the same expectation as Nova, so the comparison is statistical:
both images are noisy, and the test asks whether the difference is larger than
the noise allows.

| File | |
|---|---|
| `manifest.json` | scenes, sample counts, tolerances (hand-edited) |
| `scenes/common.usda` | `_class_RefSettings` (Nova and Karma settings), float32 beauty product, `/Looks/displayColorLambert` |
| `scenes/*.usda` | written by `tools/gen_scenes.py`; edit the script, not the files |
| `scenes/assets/` | `sphere.usda` (48x24 UV sphere), `sky_soft.exr`, `rect_pattern.exr` (hoiiotool commands in `gen_scenes.py`) |
| `images/<scene>.exr` | Karma mean of two independent renders, half float, RGBA |
| `images/<scene>.sigma.exr` | per pixel standard error of that mean, `|A - B| / 2` |
| `images/<scene>.json` | provenance: input hashes, Karma version, samples, Karma's noise floor |
| `images/<scene>/<plane>.exr`, `.sigma.exr` | AOV planes of scenes with `aovs` (M6.3), built like the beauty |
| `karma_albedo.json` | Karma's directional albedos against the oracle (`tools/karma_albedo_probe.py`) |
| `../data/albedo_ref.json` | the oracle: per-lobe albedos from `tools/gen_albedo_ref.py` (numpy), checked by `nova_tests "[albedo]"` |

## Running

`ctest -L reference` (one test per scene, needs the staged package and husk).
Each test renders the scene twice with Nova (seeds 11 and 12, 64 spp by
default) and runs `nova_imgcmp`; results, FLIP maps and the renders go to
`build/<preset>/tests/reference/<config>/`. Scenes marked `pending` are skipped.

A reference that no longer matches its scene files (hash in
`images/<scene>.json`) fails the test: regenerate it.

## Regenerating references (needs a Karma license)

    python tests/reference/tools/make_references.py --hfs "%HFS%" ^
        --imgcmp build/vs2022/tools/nova_imgcmp/Release/nova_imgcmp.exe ^
        --refdir tests/reference --work %TEMP%/nova_refs [--only cornell_rect]

The script checks Karma's effective settings from the EXR metadata
(`husk:render_stats`), and Karma rendered at the test sample count must pass
against its own new reference before the files are moved from `--work` into
`images/` (the JSON last). `--spp-half N` makes a dry run that stays in
`--work` (add `--force` to install it anyway); the test checks that the
image's `nova:spp` matches its JSON and the manifest.

## Albedo probes

Material references compare against Karma, and Karma's material models
differ from the MaterialX reference in places (rough metals, Oren-Nayar, no
compensation of dielectric layers). The probe measures that: it renders the
lobe configurations of `tests/data/albedo_ref.json` on a sphere in a white
dome with Karma and compares them with the independent oracle, the same
measurement `nova_tests "[albedo]"` makes for Nova. Re-run it after a Houdini
update, before regenerating references, and recheck the region tolerances:

    "%HFS%/python313/python.exe" tests/reference/tools/karma_albedo_probe.py --hfs "%HFS%"

`gen_albedo_ref.py` (about four minutes) only needs re-running when a probe
or a compensation changes.

## Scene rules

- Sublayer `common.usda`; the RenderSettings prim inherits `_class_RefSettings`
  and sets the same ray limits for both renderers (docs/design/M4.md 3.2, the
  semantics are Karma's): `nova:object:diffuseLimit = karma:object:diffuselimit
  = N - 1` for N lit events, reflect and refract limits likewise where a scene
  sets them (both renderers default to 4). Karma renders true caustics
  (`causticsenable 1`, `causticsroughnessclamp 0`), as Nova does.
- Explicit meshes only, `subdivisionScheme = "none"` unless subdivision is
  tested (the USD default is Catmull-Clark), `doubleSided = 1`, albedo through
  `primvars:displayColor`. `implicit_spheres` tests Nova's analytic
  `UsdGeomSphere`; Karma tessellates those (about 7 % smaller silhouettes), so
  its variant `implicit_spheres_karma.usda` has one-point `Points` prims, which
  Karma renders as exact spheres (also under a non-uniform scale).
- AOV scenes (`aov_components`): the product's first var is the beauty `C`,
  the planes listed in the manifest's `aovs` follow (part names =
  `driver:parameters:aov:name`). Each plane is extracted with `hoiiotool
  --subimage` and compared on its own; `compare.aovs.<plane>` overrides the
  scene's settings (its `regions` replace the scene's). Planes with rare paths
  (a small emitter seen in reflections) need a larger `abs_tol`: Karma fails
  its own 64-spp check there with the defaults.
- `mtl_nodes` is deterministic (unlit tiles, Nova's point filter, Karma's box
  filter at scale 1e-6) and compares per pixel (`compare.pixel_tol`, 32-bit
  reference via `reference.float`).
- Textured domes: `inputs:karma:light:misbias = -1` (Karma's default is biased
  by -0.6 to -1 %); keep bright features soft and out of view.
- No surface inside a sphere or cylinder light; camera `fStop = 0` unless depth
  of field is tested (Karma's depth of field is on by default).
- Rect light textures wrap around at the light's border in both renderers
  (`light_rect_textured` tests exactly this).
- Karma shades infinitely sharp creases differently from the OpenSubdiv limit
  surface (about 10 % brighter or darker next to the crease; Karma also differs
  from Houdini's own Subdivide SOP there). `subdiv` therefore has a Karma
  variant (`reference.file` = `scenes/subdiv_karma.usda`) with the same limit
  surface in split form: the creased cube becomes an open box plus its top
  quad, both with `interpolateBoundary = "edgeOnly"` (the crease rules are the
  boundary rules). Nova renders the creased original; both files are hashed.
