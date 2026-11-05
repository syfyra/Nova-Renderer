# Nova M3 verification strategy: Karma-referenced material scenes, analytic and furnace tests, tolerance policy

## 0. Key findings (measured on this machine, Houdini 22.0.466, Karma CPU)

Everything in this section comes from husk runs and Karma's shipped VEX headers. Scripts and results are in the scratch directory listed in §10.

1. **The same MaterialX network renders in both renderers.** Author a `Material` with `outputs:mtlx:surface` pointing to a `Shader` with `info:id = "ND_standard_surface_surfaceshader"` or `"ND_open_pbr_surface_surfaceshader"`.
   - Karma renders it correctly, including `ND_image_color3` with `<UDIM>`, `ND_noise2d`, `ND_ramplr` and the default texcoord (UV0 → `primvars:st`).
   - The repo's `tests/reference/scenes/common.usda` already uses this form.
   - Karma's own Material Builder authors `outputs:kma:surface`, which Karma ranks first (Houdini help `solaris/kug/materials.txt`). Nova currently returns only `{"nova","mtlx"}` (`hydra/renderDelegate.cpp:385-387`), so Karma Material Builder materials would be ignored. M3 needs a test for this (§8, E1).
2. **Karma's defaults change material results.** The verified recipe is in §2:
   - Turning off `constrainmaxrough`, raising the limits, turning off `thinwallatrefractlimit` and turning on caustics moves a rough glass sphere (roughness 0.3) in a furnace from 0.9832 to 0.9650.
   - Raising the limits from 16 to 64 moves it again, 0.9650 → 0.9710.
   - `karma:object:causticsenable` defaults to 0. Karma then renders "fake caustics" (a tinted transparent shadow) instead of a real path-traced caustic. `stage/caustics_0.png` vs `caustics_1.png` show the difference.
3. **Karma's material models differ from the MaterialX reference and the OpenPBR spec** (furnace albedo probe; Karma vs exact Monte Carlo integration of the reference formulas):

| Lobe | Karma behaviour (source) | Measured vs exact model |
|---|---|---|
| Dielectric reflection (SS/OpenPBR `specular`, coat, glass front face) | Schlick `F(n·v)` times a GGX lobe normalised by `ggx_albedo_curve_fit`, no Fresnel per microfacet (`$HFS/houdini/vex/include/mxpbr.h`, `mxpbr_dielectric`; `ggx_utils.h` `ggx_albedo_curve_fit_ior`) | Smooth surface at μ=0.5: Karma 0.0703, Schlick 0.0700, exact Fresnel 0.0892 (−21%). Disk mean of a specular-only sphere: −10% (r0) to −15% (r1) |
| SS metal (artistic IOR) | `F(n·v)` × GGX / E₁ fit | Within ±0.6% of fully compensated (Turquin) for r≤0.6; −1.5% at r0.8; −5.9% at r1.0 |
| OpenPBR metal (F=1) | normalised by the E₁ fit | 1.0000 (r0), **1.0018 (r0.2, gains energy)**, 0.9360 (r1.0) |
| SS diffuse_roughness (Oren-Nayar) | VEX `diffuse(...,rough)`, not `mx_oren_nayar_diffuse` | −3.1% (r0.5), −4.7% (r1.0) disk mean; up to 5.4% per μ bin |
| OpenPBR EON diffuse | energy-preserving | white 0.9999, ρ=0.5 → 0.4999 |
| Rough glass | loses energy | r0.3: 0.965 (limit 16) / 0.971 (limit 64); r0.7: 0.664; thin-walled r0.5: 0.788 |
| Smooth glass, IOR 1, smooth thin-walled, translucent | lossless | 1.0000 / 1.0000 / 1.0000 / 1.0001 |
| SS sheen (Conty-Kulla mode) | **a Lambert lobe** `clr*2*diffuse()` mixed by the Imageworks albedo fit (`mxpbr_sheen`, comment "TODO: replace diffuse with dedicated sheen BSDF") | furnace exact by construction; angular shape differs from a real sheen lobe |
| Plastic, coat, sheen or fuzz over a white base | albedo-scaling layering | 0.9987–1.0003 |
| Emission (SS and OpenPBR) | radiance = emission × colour; OpenPBR `emission_luminance` is used directly as radiance (no nits conversion) | exact (0.5, 0.25, 0.125) |
| Opacity 0.5, white diffuse sphere, black background | cut-out on both faces | value 0.7501, alpha 0.750 = 1−(1−o)² |
| `ND_ramplr` | **no clamp** of texcoord (MaterialX GLSL clamps to [0,1]) | Karma gives 1.98 at u=1.98 |
| `ND_noise2d` | identical to the MaterialX GLSL reference | max abs diff 1.7e-6 |
| `ND_image` | closest filtering exact; v=1 is the top row of the file; UDIM 1002 resolved | exact |
| Anisotropy tangent | from `st` (dP/du) when `st` exists, otherwise the polygon's own parameterisation (first edge) | highlight direction flips with swapped `st` (`aniso/aniso_mosaic.png`) |

4. **Golden values for VM/node tests can be generated offline with MaterialX's own GLSL reference.** Houdini's Python can run `MaterialX.PyMaterialXRenderGlsl.GlslRenderer.renderTextureSpace()` offscreen on this machine. Pixel (x, y) maps to texcoord ((x+0.5)/W, (y+0.5)/H); row 0 is v≈0.
5. **The chi² harness design from pbrt-v4 has a pitfall, found and fixed here.** pbrt integrates the pdf per cell with adaptive Simpson, which evaluates the pdf exactly on θ=π/2, where `cos` ≈ 6e-17 > 0. Expected mass leaks into the cells just below the horizon, and a correct GGX sampler fails at 2²² samples. An open Gauss-Legendre rule fixes it. The validated harness: correct samplers pass at 2²⁴ samples with uniform p-values; a wrong pdf or a wrong Jacobian gives p=0 (§7.2).

---

## 1. Existing test system and where M3 plugs in

- **Reference harness.**
  - `tests/reference/manifest.json` holds scenes, sample counts and the compare parameters. `refcommon.scene_config` (refcommon.py:25) merges the defaults.
  - `check_references.py` renders the scene with Nova twice (seeds 11/12, 64 spp) through husk or `nova_cli`, then runs `nova_imgcmp`.
  - `make_references.py` builds the Karma reference from two halves of 2048 spp (seeds 1/2). `check_karma_settings` (make_references.py:31) checks `convergence_mode`, the uniform pixel oracle, colour limits, `constrainmaxrough` and the primary ray count, all read from `husk:render_stats`. It then runs a self-check where Karma at the test spp must pass against its own reference.
  - **The object-level `karma:object:*` settings do not appear in `husk:render_stats`.** Only the global settings are reported. Their only protection is the scene hash, plus the evidence that they act when authored on the RenderSettings prim: the limit 16 → 64 runs changed the results.
- **Statistics.** `tools/nova_imgcmp/nova_imgcmp.cpp`:
  - Per 8×8 block and channel: `|T_b − R_b| ≤ k_b·s_b + blockRelTol·|R_b| + absTol·μ` (line 18 and line 309).
  - `k_b` is the Šidák-corrected z (line 292), widened to a Welch t.
  - Global test at line 341.
  - Inputs: T = mean of two Nova renders, R = Karma mean, σ_R = |A−B|/2.
- **Hash rule.** `refcommon.scene_inputs` hashes every referenced asset. **Editing `scenes/common.usda` or `scenes/assets/sphere.usda` invalidates all 14 M2 references.** M3 must add new files (`common_mtl.usda`, `assets/sphere_st.usda`) and leave those two alone.
- **Integrator hooks M3 will change and the tests must cover.**
  - `core/integrator/wavefront.cpp:235`: `LambertBsdf bsdf{hit.color}`.
  - `:263-264`: the NEE MIS weight.
  - `:272-286`: the BSDF sample, `q.bsdfPdf[p] = bs.pdf`.
  - `:274`: `dot(wi, ng) > 0` (reflection only).
  - `:244`: `offsetRayOrigin(hit.P, ng)` (one side only).
  - `:217/:223`: MIS at area-light and escape hits uses `q.bsdfPdf`. **Delta lobes (glass, mirror) must skip MIS there.** The "IOR-1 glass casts no shadow" and "mirror reflects a rect light at L" tests (§7.1) catch exactly this.
  - Sobol streams `kBsdf` (3D, z selects the lobe), `kOpacity` and `kLayer` are already reserved (`core/math/sobol.h:146-149`).
- **Runtime.** Each reference test takes ≈1.5–2.1 s through husk and ≈0.4–0.7 s through cli (`build/vs2022/Testing/Temporary/CTestCostData.txt`). Karma at 320×160 takes 4.3 s at 64 spp and 8.6 s at 256 spp: about 3 s start-up plus about 0.022 s per spp.

---

## 2. Settings that make Karma comparable (verified)

Put these in a **new** `tests/reference/scenes/common_mtl.usda`, so the M2 hashes stay unchanged. This is the class used for all experiments:

```usda
class "_class_MtlRefSettings"
{
    float nova:clampIndirect = 0
    int nova:maxDepth = 64
    token nova:pixelFilter = "gauss"            # "box" for furnace probes
    float nova:pixelFilterScale = 1

    token karma:global:convergence_mode = "Path Traced"
    string karma:global:pixeloracle = '["uniform",{}]'
    string karma:global:pixelfilter = "gauss"
    float karma:global:pixelfilterscale = 1
    float karma:global:colorlimit = 1e9
    float karma:global:colorlimitindirect = 1e9
    bool karma:global:colorlimitindlinked = 1
    bool karma:global:constrainmaxrough = 0     # default 1: clamps GGX roughness along paths
    float karma:object:diffuselimit = 64        # defaults 1 / 4 / 4 / 1
    float karma:object:reflectlimit = 64
    float karma:object:refractlimit = 64        # glass r0.3 furnace: 0.9650 at 16, 0.9710 at 64
    float karma:object:ssslimit = 64
    bool karma:object:causticsenable = 1        # default 0 = fake caustics (BSDF-tinted shadows)
    float karma:object:causticsroughnessclamp = 0   # default 0.2 = GGX roughness floor after diffuse bounces
    bool karma:object:internalreflectenable = 1 # default 0: no internal reflection lobe on glass backfaces
    bool karma:object:thinwallatrefractlimit = 0    # default 1: paths escape at the limit (adds energy)
    rel products = [</Render/Products/beauty>]
}
```

Notes and evidence:
- **Defaults are in `$HFS/houdini/RenderPropertiesKA.json`.** Karma limits are stochastic floats. `karma:object:limitcolor` is (0,0,0), i.e. a path that hits the limit returns black.
- **Bounce classes in Karma.** `diffusecomponents = ["diffuse"]` and `refractcomponents = ["refract"]`; everything else (`reflect`, `coat`, ...) counts as reflect. Karma's Standard Surface sheen is a `diffuse()` lobe, so its bounces count against the **diffuse** limit.
  - **Recommendation:** use limits large enough not to matter (64) on both sides. Test limit semantics separately once Nova has per-type limits.
- **Limits in Nova.** Nova currently has only `nova:maxDepth` (lit events). Use 64 with Russian roulette. Karma's RR cutoff 2 is also unbiased.
- **`internalreflectenable` hardly changes furnace energy** (glass r0: 1.0001 vs 1.0000) but changes how the glass looks. Nova always does internal reflection, so set it to 1.
- **Caustics.** With caustics on, smooth glass over a diffuse plane under a 1 m² rect light is very noisy at 256 spp (`stage/caustics_1.png`). **Material scenes must avoid diffuse receivers of specular light.** Use floating spheres, emissive (unlit) backdrops, and a constant dome plus rect lights.
- **No textured domes in material scenes.** `misbias = -1` (needed against Karma's biased dome MIS, M2 finding) makes Karma's dome BSDF-sampled only, which speckles diffuse spheres badly (first stage render).
- **Unchanged from M2:** `husk --headlight none`, `-p <spp> --lock-random <seed>`, the float32 beauty product with `C.*[LO]`, and `fStop = 0`.
- **Karma reference cost.** About 35 s per 4096-spp half at 192×96, about 90 s at 320×160.
- **Reference noise.** From two 256-spp seeds of the verified stage scene (§5): the median standard error of an 8×8 block mean is **0.05% for a reference at 2×4096 spp**. Nova at 2×64 spp is 0.42% median, 1.4% p90 and 4.9% max (glass blocks). So model differences ≥2% are detectable per block.

---

## 3. Authoring materials so that Karma and Nova read the same network

```usda
def Material "metal_r02"
{
    token outputs:mtlx:surface.connect = </Looks/metal_r02/surface.outputs:out>
    def Shader "surface"
    {
        uniform token info:id = "ND_standard_surface_surfaceshader"
        float inputs:base = 1
        color3f inputs:base_color = (0.95, 0.64, 0.54)
        float inputs:metalness = 1
        float inputs:specular = 1
        float inputs:specular_roughness = 0.2
        float inputs:coat = 0
        float inputs:sheen = 0
        float inputs:transmission = 0
        float inputs:emission = 0
        float inputs:subsurface = 0
        float inputs:diffuse_roughness = 0
        color3f inputs:base_color.connect = </Looks/metal_r02/tex.outputs:out>   # optional texture
        token outputs:out
    }
    def Shader "tex"
    {
        uniform token info:id = "ND_image_color3"
        asset inputs:file = @./tex.<UDIM>.exr@      # <UDIM> verified in Karma
        string inputs:filtertype = "closest"
        color3f outputs:out
    }
}
```

Rules:
- **Only `outputs:mtlx:surface`.** Karma's priority is `kma` > `mtlx` > `vex` > universal. A material with a second `kma` output would make Karma render a different network.
  - Add **one** test scene that uses only `outputs:kma:surface` with a pure MaterialX Standard Surface (that is what Karma Material Builder materials look like). Nova must pick it up, which needs `"kma"` in `GetMaterialRenderContexts`.
- **Author every input that matters.** Two definitions ship side by side (`houdini/materialx/libraries/bxdf/standard_surface.mtlx:6/14`): `ND_standard_surface_surfaceshader` v1.0.1 (base 1.0, base_color 0.8) and `_100` v1.0.0 (base 0.8, base_color 1.0).
  - Add one "all defaults" sphere (only `info:id`), so Nova's default resolution is tested against the 1.39.5 library Houdini ships.
  - Versions: MaterialX 1.39.5 (`MaterialXCore/Generated.h`), OpenPBR nodedef 1.1.1, OIIO 2.5.18.
- **Explicit meshes with `primvars:st`.** Use a new asset `assets/sphere_st.usda` (160×80 UV sphere, vertex normals, `st` with u along φ; generator in `gen_furnace.py: sphere_asset`). `st` defines Tworld for anisotropy (verified) and is the default UV0 for image nodes (verified).
- **Displays colour** keeps going through `ND_geompropvalue_color3` (existing `/Looks/displayColorLambert`).
- **Thin-walled transmission is not in the MaterialX 1.39.5 Standard Surface nodegraph.** `thin_walled` only selects translucent vs subsurface (standard_surface.mtlx:244-251). Karma implements it anyway (smooth thin glass = 1.0000). Nova must take it from the Autodesk spec, not from the nodegraph. Test it.

---

## 4. Expected Nova–Karma differences and how to handle each

The rule: **exactness comes from analytic tests (§7); Karma comparisons check translation and integration.** Each difference gets one of three handlings: avoid, design around, or tolerate per region.

| Effect | Size (measured) | Handling |
|---|---|---|
| Schlick vs exact Fresnel on dielectric reflection | up to −21% of the specular part at μ≈0.5 | Use bright bases (ρ ≥ 0.6) and soft lights. In a uniform dome the pixel error is ΔE_spec·(1−ρ) ≤ 0.02·(1−ρ), ≤ 1.3% for ρ=0.6. Highlight regions get a region tolerance (≤10%). Smooth glass seen *through*: front reflection error ≈ 0.009 × reflected radiance, ≤ 1.5%. |
| Rough metal r ≥ 0.8 (Karma −1.5…−6%) | see table §0 | Only r ≤ 0.6 in strict scenes. r 0.8/1.0 in Nova-only unit tests. |
| Rough glass (Karma −3% at r0.3, −34% at r0.7) | | Strict scenes use smooth or r ≤ 0.2 glass. Rough glass only analytically (Nova-only furnace, model-specific expectation). |
| SS Oren-Nayar (−3…−5%) | | In the tolerant scene (rel_tol 0.06). OpenPBR EON in the strict scene. |
| SS sheen = Lambert-shaped in Karma | furnace identical, shape differs | Furnace (constant 1) is strict. Lit sheen sphere gets rel_tol 0.10. |
| `ramplr` clamp | node level | Karma probes only use texcoords in [0,1]. MaterialX GLSL golden values cover u > 1. |
| Texture filter width / mip selection | per-pixel sharpness | Compare 8×8 block means (invariant to any mean-preserving filter). Pixel-exact only for `closest` at magnification. |
| Normal handling at silhouettes (Karma `_mx_normal_clamp(..., 0.01)` on reflection; shading vs geometric normals) | grazing pixels | Erode region masks by 1.5 px; furnace probes use r < 0.97 only. |
| Fireflies / caustics | | Avoided by scene design (§5). |

**Optional, not required for M3: a test-only Karma-compatibility closure variant.** A uniform flag (like the existing `nova:lightStrategy` debug setting) would switch dielectric reflection to Schlick-albedo-normalised GGX, conductor Fresnel to `F(n·v)`, and SS sheen to a Lambert shape. With it, plastic, coat and sheen scenes could become strict too.
- Implement it only from MaterialX (Apache-2.0) sources and black-box measurements. **Do not copy `mxpbr.h`/`ggx_utils.h`: they are marked "PROPRIETARY INFORMATION" by SideFX.**
- Decide this after the first tolerant-tier results.

---

## 5. Karma-referenced material scenes (design) and the verified prototype

**The prototype that was rendered:** `stage/mtl_stage.usda`, generator `gen_stage.py`.
- Geometry and camera: 3×6 floating spheres (radius 0.5, 1.25 apart), no ground, camera at z=12 (50 mm, 36×18 aperture), 320×160.
- Lights: constant dome (0.35, colour (0.85, 0.9, 1)), rect key (6, 2×1.2 m), warm rect rim (4, 3×0.8 m).
- Rows:
  - Lambert red, SS Oren-Nayar r1, plastic r0.2 / r0.5, sheen, coat-over-red.
  - Metal r0 / r0.2 / gold r0.5, anisotropic metal, thin-film metal, emission.
  - Glass r0, glass r0.3, tinted glass, thin glass, opacity 0.4, IOR-1 glass.
- Result: Karma renders all materials (`stage/mtl_stage.png`): 4.3 s at 64 spp, 8.6 s at 256 spp. The noise numbers are in §2.

**Scenes to add (192×96, 18 spheres in a 6×3 grid, explicit `sphere_st.usda`, `common_mtl.usda`, test 64 spp, reference spp_half 4096):**

| Scene | Content | Lighting | Compare |
|---|---|---|---|
| `furnace_materials` | 16 spheres, all energy-conserving in Karma: Lambert white, OpenPBR EON r1, plastic r0.2/r0.6, SS coat r0/r0.3, OpenPBR coat r0, sheen-over-white, fuzz-over-white, smooth glass, IOR-1 glass, thin smooth glass, translucent (thin, subsurface 1), OpenPBR metal F=1 r0, emission 0 on white Lambert, "all-defaults" Lambert-free plastic replaced by white plastic r0.4 | constant dome L=1, `karma:light:renderlightgeo = 1` (background = 1) | `constant [1,1,1,1]`, `karma_check: true`, `block_rel_tol 0`, `global_rel_tol 5e-4` (Karma measured within 3e-4 for every listed material) |
| `mtl_diffuse` | Lambert colours, OpenPBR EON r0.5/r1, cut-out opacity 0.4, emission sphere, UDIM-textured base_color (st), all-defaults SS sphere | dome 0.35 + key rect + rim rect | strict: `block_rel_tol 0.01` |
| `mtl_metal` | SS metal r0/0.2/0.4/0.6 (silver, copper, gold), OpenPBR metal F82 r0.3, anisotropy 0.8 with st and with swapped st, smooth thin-film metal | same | `block_rel_tol 0.02` (Karma vs exact per-μ-bin ≤ 2.1% for r ≤ 0.4) |
| `mtl_glass` | smooth glass IOR 1.5 and 1.33, tinted transmission, smooth thin-walled, IOR 1, r0.15 glass, in front of an **emissive checker backdrop** (base 0, emission 1, so no caustic paths) | dome + key rect | `block_rel_tol 0.02` |
| `mtl_layered` | plastic r0.2/r0.5 on bright bases, SS coat, SS sheen, OpenPBR fuzz, smooth thin-film dielectric, SS Oren-Nayar r1 | dome + soft rect away from the mirror direction | `block_rel_tol 0.03`, regions: sheen 0.10, Oren-Nayar 0.06, plastic highlights 0.10 (with a `reason` field each) |
| `mtl_textured` | tilted plane + spheres: UDIM base_color, roughness map (raw colour space), `ND_normalmap` from st, checker `.tx` minified at grazing angles | dome + rect | strict 0.01 (block means do not depend on filter width) |
| `mtl_kma_context` (can be a husk check instead) | the same sphere with `outputs:kma:surface` only | dome | strict |

---

## 6. Statistics and tolerance policy

1. **Keep the block z-test.** It has Šidák FWER α=1e-3 plus a Welch–Satterthwaite t, and the global test.
2. **Add region tests to `nova_imgcmp`** (per-sphere means with confidence intervals and per-region tolerances). Use repeated CLI arguments, so no JSON parser is needed:

```cpp
// nova_imgcmp: --region name,cx,cy,r,relTol   (pixels, row 0 = top; repeatable)
struct Region { std::string name; double cx, cy, r, relTol; };
// (a) per-pixel tolerance map: blockRelTol outside regions, region relTol inside
std::vector<float> tol(size_t(W) * H, float(a.blockRelTol));
for (const Region &g : a.regions)
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x)
        if (sq(x + 0.5 - g.cx) + sq(y + 0.5 - g.cy) <= g.r * g.r) tol[size_t(y) * W + x] = float(g.relTol);
// block test: allowed = k*sqrt(v) + (sum_p tol_p*|R_p|)/n + absTol*mu         (replaces line 309)
// (b) region-mean test, Sidak over regions x channels (alphaFwer split with the blocks):
//   T_g = mean_p T, R_g = mean_p R, s_g = sqrt(sum_p (vT_p + vR_p)) / n_g
//   pass_g: |T_g - R_g| <= z_sidak * s_g + relTol_g * |R_g|
// JSON: "regions": [{"name", "ref", "test", "rel_diff", "z", "pass"}]   (diagnosis: "sheen +7.3%")
```

   - `gen_scenes.py` computes the circles analytically from the camera (pinhole projection of each sphere centre, radius ≈ f·R/d in pixels, minus 1.5 px).
   - `manifest.json` → `compare.regions: [{name, circle:[cx,cy,r], rel_tol, reason}]`; `check_references.py` forwards them.
3. **Tiers.**
   - **Analytic** (constant or closed form): `block_rel_tol 0`, `global_rel_tol ≤ 5e-4`.
   - **Karma-agreeing:** ≤ 0.02, justified by the measured Karma-vs-exact table.
   - **Tolerant:** per region, ≤ 1.5 × measured deviation, capped at 0.10. Each scene still needs a strict test on its unflagged regions.
   - Every tolerance above 0.01 carries a `reason` in the manifest.
4. **Re-measuring.** Keep the albedo-probe scripts as `tests/reference/tools/karma_albedo_probe.py` (port of `run_karma.py` + `compare.py`). On a Karma version change (recorded in `images/<scene>.json`), re-run the probe and re-check the deviation table before regenerating references.

---

## 7. Analytic, furnace and statistical tests that need no Karma

### 7.1 Render-level furnace tests (`tests/test_material_furnace.cpp`, through `Renderer`, like `test_furnace.cpp`)

Setup: orthographic camera, a 128×64 sphere, uniform dome L=1, `maxDepth 64`, `clampIndirect 0`. Use the disk r < 0.97 only, because of the interpolated-normal loss at grazing angles (`test_render.cpp:75`).

| Case | Expected | Tolerance |
|---|---|---|
| Lambert ρ (any colour) | ρ per channel | 2e-3 |
| White plastic r0.2/0.6, coat, sheen/fuzz over white, smooth/IOR-1/thin glass, translucent, F=1 mirror | 1.0 (if Nova's layering conserves energy; otherwise the model's own E table) | 3e-3 |
| Emission only (base 0, spec 0), dome off | (0.5, 0.25, 0.125) exact; OpenPBR luminance 1 → same | 1e-5 |
| Opacity 0.5 white, dome invisible to the camera | colour 0.75, alpha 0.75 | 3e-3 |
| Transmissive sphere | alpha 1 (transmission must not reduce alpha) | exact |
| **IOR-1 sphere above a Lambert plane in a uniform dome** | plane pixel = ρ (no shadow). Fails if MIS weights a delta-lobe path or if `wi·ng > 0` rejects transmission | 1% |
| **F=1 mirror reflecting a rect light (L=3)** | reflected light pixels = 3.0 (MIS weight 1 after a delta bounce) | 0.5% |
| Albedo probe per lobe (radial profile, 10 μ bins) | from `tests/data/albedo_ref.json` | 0.3% abs |

`albedo_ref.json` is produced by `tools/gen_albedo_ref.py`: an independent numpy implementation of the formulas, i.e. MaterialX GLSL `mx_ggx_smith_G2`, `mx_fresnel_dielectric`, `mx_oren_nayar_diffuse`, the conductor Fresnel, and Nova's chosen compensation. `scratch/analytic.py` is the seed. Two independent implementations must agree. Values computed here (orthographic disk μ ≥ 0.25):

| α (SS r) | E₁ avg (F=1, single scatter) | SS white metal, single scatter | Turquin-compensated | Dielectric 1.5, single scatter | Turquin |
|---|---|---|---|---|---|
| 0.04 (0.2) | 0.9941 | 0.9882 | 0.9917 | 0.0691 | 0.0691 |
| 0.16 (0.4) | 0.9407 | 0.9368 | 0.9912 | 0.0608 | 0.0613 |
| 0.36 (0.6) | 0.8033 | 0.7934 | 0.9896 | 0.0453 | 0.0463 |
| 1.0 (1.0) | 0.4092 | 0.3872 | 0.9856 | 0.0189 | 0.0209 |

Also measured: MaterialX Oren-Nayar disk mean 0.8503 (r0.5) and 0.7062 (r1.0); E(μ=1) = 0.7855 / 0.6253. These are Nova's targets if it implements `mx_oren_nayar_diffuse`; Karma differs (see §4).

### 7.2 Closure statistics (`tests/test_closures.cpp`, header `tests/bsdf_stats.h`)

The harness is validated (scratch `chi2/bsdf_stats.h`, `chi2/main.cpp`; MSVC 14.44 /W4 clean). Interface:

```cpp
struct LobeUnderTest {   // local frame, +Z = shading normal
    std::function<bool(V3 wo, V3 u, V3 &wi, double &pdf, double &weight, bool &delta)> sample; // weight = f|cos|/pdf
    std::function<double(V3 wo, V3 wi)> pdf;
    std::function<double(V3 wo, V3 wi)> evalCos;
};
Chi2Result chi2Test(const LobeUnderTest&, V3 wo, Pcg32&, int sampleCount = 1<<20, int thetaRes = 48,
                    double minExpFrequency = 5, double significance = 0.01, int runsInTestCase = 5);
AlbedoPair albedoEstimates(const LobeUnderTest&, V3 wo, Pcg32&, int n = 1<<18); // importance vs uniform-sphere
```

- **Design** (pbrt-v4 `bsdfs_test.cpp`):
  - Constants: θ×φ = 48×96 over the whole sphere, 2²⁰ samples, cells with expectation < 5 pooled, significance 0.01 with Šidák correction over runs.
  - Two deliberate changes: (a) the per-cell pdf integral uses an **open 8×8 Gauss-Legendre rule on 2×2 sub-cells**, not adaptive Simpson (the horizon leak, §0.5); (b) the p-value uses the Wilson–Hilferty approximation `z = ((χ²/k)^{1/3} − (1−2/9k))/√(2/9k)` (dof is in the thousands).
- **Validation results:** Lambert and GGX (α 0.5, 0.1, aniso 0.6/0.15) pass; GGX at 2²⁴ samples gives p=0.39. A wrong-pdf Lambert and a wrong-Jacobian GGX give p=0, with pdf mass 1.89 instead of 0.83. About 150 ms per run (1M samples).
- **Further checks per lobe config:**
  - `|pdfMass − acceptedFraction| < 2e-3`.
  - For 10⁴ samples: `sample().pdf == pdf(wo, wi)` and `weight == evalCos/pdf` to 1e-4 relative.
  - Delta lobes: `pdf()` and `eval()` of the sampled direction are 0.
  - `|E_importance − E_uniform| < 4σ`, only for α ≥ 0.3 (uniform sampling is useless for sharp lobes: ±0.0125 at α=0.1).
- **Configs:**
  - Lambert; Oren-Nayar r0.5/1; EON.
  - GGX conductor α 0.05 (with thetaRes 96), 0.2, 0.6, aniso.
  - Dielectric R+T for η 1.5 and 1/1.5, α 0.3/0.6, at a grazing wo (μ=0.1).
  - Thin-walled transmission; Charlie/Conty-Kulla sheen; Zeltner fuzz.
  - Coat-over-base mixture (tests lobe selection on `kBsdf.z`); five random full Standard Surface and OpenPBR parameter sets.
- **Budget:** 2¹⁹ samples, θ 32 except where noted, 3 wo each → about 5 s CPU, one `TEST_CASE` per lobe so ctest parallelises.
- **Reciprocity:** `f(wo,wi) = f(wi,wo)` to 1e-5 relative for single lobes (Lambert, Oren-Nayar, GGX reflection, sheen). For refraction: `f(wo→wi)/η_o² = f(wi→wo)/η_i²` (Veach). **Not for layered closures:** albedo-scaling layering, both MaterialX's and Karma's, is not reciprocal by construction.
- **Energy fuzzing:** 512 random parameter sets (UI ranges plus edges 0/1, roughness 1e-8, IOR 1, 0.5, 3), 4 wo, 2048 samples. Require `E ≤ 1 + 4σ` for colours ≤ 1. ≈ 0.5 s.
- **NaN/Inf hunt:** 10⁶ random (params, wo, wi, u), including wo·n = ±1e-7, wo = n exactly, shading normal on the far side of the geometric normal, tangent ∥ normal, IOR 0 (UI min is 0), anisotropy 1. Require finite outputs, pdf ≥ 0, |wi| = 1 ± 1e-4, weight ≥ 0. The renderer currently hides non-finite samples (`wavefront.cpp` drops them in `renderWave`), so this test is the only place they surface.
- **Energy-compensation tables** (if Nova uses E(μ,α) tables): table vs brute force ≤ 2e-3 absolute; white metal E(μ) ∈ [0.995, 1.002] for all α.

### 7.3 Texture tests (`tests/test_textures.cpp`, wherever OIIO is linked; core stays OIIO-free)

- **UDIM:** tile = 1001 + ⌊u⌋ + 10⌊v⌋ (Karma-verified). A missing tile returns the MaterialX `default` input. Test both `<UDIM>` and `<UVTILE>` (u1_v1).
- **Orientation:** v=1 is file row 0 (Karma-verified).
- **Address modes** periodic / clamp / mirror / constant at u = −0.25 and 1.25 with `closest`: exact texel values.
- **Bilinear at magnification:** exact linear interpolation between texel centres.
- **Mip selection:** write a texture whose mip level k is a distinct constant colour (OIIO `ImageOutput` with `AppendMIPLevel`). A lookup with footprint 2^k texels must return level k (or the trilinear blend fraction). Same result for `.tx` (`hoiiotool in.exr -otex out.tx`, or `imaketx`) and for an untiled EXR with automip.
- **Minified 1-texel checker:** mean 0.5 ± 0.005 and per-pixel standard deviation < 0.02, which only holds with mipmapping.
- **Colour space:** 8-bit PNG is sRGB → linear (as `husk_textures`); `colorSpace = "raw"` keeps data maps linear. Verify this rule once with a Karma unlit probe before encoding it (not verified here).

### 7.4 VM differential tests (`tests/test_shading_vm.cpp`)

- **Opcodes vs a scalar reference** (MaterialX GLSL semantics) on edge values (±0, denormals, ±inf, NaN, 1e±30). Exact ops bit-identical; transcendentals ≤ 2 ulp vs a double reference.
- **Random graphs:** a seeded DAG generator over the supported node set, type-correct connections, random constants and inputs. Compare the unoptimised IR (tree-walking evaluator) with the optimised bytecode (constant folding, DCE, CSE); scalar vs 64-lane SoA batch; divergent `ifgreater`/`switch` masks. Bit-identical if folding uses float in the same order. 2000 graphs ≈ 0.3 s.
- **Folding:** an all-constant graph folds to a single constant; DCE removes unreachable nodes (check instruction count).
- **Determinism:** the same IR gives the same bytecode bytes (hash) across runs and threads.
- **MaterialX golden values:** `tools/gen_mtlx_golden.py` (offline; Houdini Python + OpenGL) renders about 30 stdlib graphs (noise2d/3d, fractal, cellnoise, worley, ramp, checker, math, image with address modes, colour conversion) with `GlslRenderer.renderTextureSpace(Vector2(0,0), Vector2(1,1))` at 64×64 into `tests/data/mtlx_golden/*.exr`. The VM evaluates the same graphs at texcoord ((x+0.5)/64, (y+0.5)/64). Tolerance 2e-5 abs + 1e-4 rel.
  - API facts confirmed here: `r.setImageHandler(...)` before rendering; `img = Image.create(W, H, 4, BaseType.FLOAT); img.createResourceBuffer(); r.captureImage(img)`; `getTexelColor(x, y)` row 0 is v≈0.
- **End-to-end through Hydra:** a husk unlit probe (emission-only quad, orthographic, `nova:pixelFilter = "point"` / `karma:global:pixelfilter = "point"`, 1 spp) for nodes where Karma equals MaterialX (noise2d verified to 1.7e-6; image `closest`). Compare against the golden EXR, not Karma, so no licence is needed.

---

## 8. Concrete additions, expected values, runtime

| # | File / test | Checks (expected) | Est. wall (-j4) |
|---|---|---|---|
| U1 | `tests/bsdf_stats.h` + `tests/test_closures.cpp` | chi², pdf mass, sample/eval/pdf consistency, reciprocity, energy fuzz, NaN fuzz, `albedo_ref.json` | ~2 s (5–8 s CPU) |
| U2 | `tests/test_material_furnace.cpp` | §7.1 table | ~1.5 s |
| U3 | `tests/test_shading_vm.cpp` + `tests/data/mtlx_golden/` | §7.4 | ~0.5 s |
| U4 | `tests/test_textures.cpp` | §7.3 | ~0.5 s |
| H1 | `tests/husk/check_materials.py` (Houdini Python) | unlit probes: UDIM tile colours exact, v orientation, noise2d vs golden ≤ 2e-5, `kma` context picked up, all-defaults sphere = 0.8 albedo + spec, missing-texture warning on stderr | ~5 s (one test, ~6 husk runs) |
| R1 | `furnace_materials` (constant 1, karma_check) | 1.0 everywhere, global 5e-4 | 2 s husk + 0.6 s cli |
| R2–R6 | `mtl_diffuse`, `mtl_metal`, `mtl_glass`, `mtl_layered`, `mtl_textured` (Karma) | §5 tolerances | 5 × (2 s + 0.6 s) |
| T1 | `nova_imgcmp --region` | region JSON output | – |
| T2 | `tests/reference/tools/karma_albedo_probe.py`, `gen_albedo_ref.py`, `gen_mtlx_golden.py` | offline tools (not in ctest) | – |

- **Totals:** about +32 s serial, **about +9–10 s wall at -j4** (suite ≈ 40 s).
- **Offline cost:** Karma references 6 × 2 × ~35 s ≈ 7 min; golden generation < 1 min.
- **Scene generation:** `gen_scenes.py` gets a `material(name, shader_id, inputs)` helper (as in `gen_furnace.py`) and writes `common_mtl.usda` and `assets/sphere_st.usda` as new files.
- **Nova-side testability hooks** (recommended to the shading/integrator topics):
  - Closure entry points callable on the host without a Scene: `NOVA_HOST_DEVICE` `evalClosure(prog, frame, wo, wi) → {f, pdf}` and `sampleClosure(prog, frame, wo, Vec3f u) → {wi, weight, pdf, flags (delta | transmission | lobe id)}`.
  - A `ShadingPoint` built from (P, N, Ng, T, uv).
  - The delta flag must reach `q.bsdfPdf` handling (`wavefront.cpp:217/223`).

---

## 9. Pitfalls

1. **Karma defaults:** `constrainmaxrough=1`, caustics off (fake caustics), `thinwallatrefractlimit=1`, limits 1/4/4/1, colour limit 20, variance oracle, `internalreflectenable=0`. Object-level settings are not in `husk:render_stats`, so they cannot be checked automatically.
2. **Never change `scenes/common.usda` or `assets/sphere.usda`.** Every M2 reference hash breaks.
3. Standard Surface v1.0.0 and v1.0.1 have different defaults; OpenPBR defaults differ from Standard Surface (specular_roughness 0.3, coat_ior 1.6). Author explicitly and test the "all defaults" case once.
4. Karma Material Builder → `outputs:kma:surface`; Nova ignores it today.
5. Anisotropy tangent: from `st` if present, else polygon parameterisation (Karma). Always author `st`.
6. MaterialX image: v=1 is the top row of the file. ramplr: Karma does not clamp.
7. OpenPBR `emission_luminance` is used as radiance in both the MaterialX reference and Karma; no photometric conversion.
8. Transmission does not reduce alpha; geometric opacity does, on every face crossed.
9. chi²: closed quadrature rules leak pdf mass across the horizon. Use open rules.
10. Uniform-sphere albedo estimators are useless for α < 0.3. Do not use them as a check there.
11. Furnace profiles: exclude r > 0.97 (faceted silhouette normals, filter mixing with the background, `_mx_normal_clamp`).
12. Textured domes in Karma material references: `misbias = -1` is needed against bias but makes diffuse surfaces speckle. Use constant domes and rect lights.
13. Any diffuse surface that sees glass or metal under a small light creates caustic paths that are high-variance in both renderers. Use floating spheres and emissive backdrops.
14. `mxpbr.h` and `ggx_utils.h` are proprietary SideFX code. Use them only as documentation of behaviour, never copy them into Nova.
15. Karma's Standard Surface sheen is a diffuse lobe for Karma's bounce limits. With large limits this does not matter; with per-type limits it does.

---

## 10. Scratch artifacts

All paths are under `C:/Users/Synti/AppData/Local/Temp/claude/C--Users-Synti-Desktop-renderEngine/ac6add8e-dc52-40d3-838a-501e34988043/scratchpad/m3research/test-strategy/`.

| Path | What it is |
|---|---|
| `gen_furnace.py` | furnace probe scenes, 64 cases + Karma settings class + `sphere_asset()` with st |
| `run_karma.py` | isolated husk/Karma runner + radial profile |
| `analytic.py`, `compare.py` | exact GGX albedos via VNDF Monte Carlo; Karma vs exact per μ bin |
| `furnace/main/results_256.json`, `furnace/variants.log`, `furnace/on/results_256.json`, `compare_256.json` | all numbers quoted above |
| `gen_stage.py`, `stage/mtl_stage.usda`, `stage/mtl_stage.png`, `stage/caustics_0.png`, `stage/caustics_1.png` | the verified lit stage scene and the caustics comparison |
| `lpe/` | reflection-only LPE (`C<R.>L`) renders: the Schlick finding |
| `texprobe.py`, `texprobe/` | UDIM / orientation / ramplr / noise unlit probes |
| `aniso/aniso_mosaic.png` | tangent convention |
| `glsl_golden.py`, `glsl/noise2d_glsl.npy` | MaterialX GLSL texture-space golden values (feasibility) |
| `chi2/bsdf_stats.h`, `chi2/main.cpp`, `chi2/pdist.cpp`, `chi2/dbg3.cpp` | validated chi² / albedo harness and its validation programs |