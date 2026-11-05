# Standard Surface 1.0.1 and OpenPBR 1.1.1 in Nova (M3): definitions, Karma behaviour, design, tests

## 0. Key findings

- **Versions shipped with Houdini 22.0.466.** MaterialX is 1.39.5 (`$HFS/toolkit/include/MaterialXCore/Generated.h`).
  - The Standard Surface definition `ND_standard_surface_surfaceshader` is version **1.0.1**. It inherits `ND_standard_surface_surfaceshader_100`, changes only the defaults (`base=1.0, base_color=0.8` instead of `0.8 / 1.0`), and uses the same nodegraph (`standard_surface.mtlx:6-12,107-108`).
  - The OpenPBR definition `ND_open_pbr_surface_surfaceshader` is version **1.1.1** (`open_pbr_surface.mtlx:6`).
  - Houdini's OpenPBR graph equals the OpenPBR v1.1.1 reference graph, plus clamps on `subsurface_color` and `emission_luminance` (diffed against `OpenPBR/v1.1.1/reference/open_pbr_surface.mtlx`).
- **Both models reduce to a weighted sum of a few lobes**, with weights that depend on the outgoing direction ωo:
  - layer: `top + (1-E_top(ωo))·base`
  - mix: lerp of the two inputs
  - All of this is linear in the leaf lobes.
- **Karma CPU interprets the pbrlib nodegraph generically.** `libBRAY.dll` holds one VEX snippet per pbrlib node (`mxpbr_dielectric(...,$base)` etc.).
  - The layer node is folded into the top BSDF; the DLL contains the comment `// Implicit handling of ND_layer_bsdf`.
  - Implementation: `$HFS/houdini/vex/include/mxpbr.h`, `mxtypes.h`, `ggx_utils.h`.
- **Karma XPU implements both shaders natively.** The help states "Does not support MaterialX PBR nodes" and allows at most 16 blended Standard Surface/OpenPBR shaders (`help/solaris/karma_xpu.txt:240-326`).
- **Measured Karma behaviour.** These come from direct-light-only albedo probes I rendered with husk (section 5.3):
  - Layering under the specular lobe uses the **untinted** albedo (MaterialX semantics, not the Autodesk spec's tinted one).
  - The Standard Surface sheen is an **albedo-weighted Lambert lobe** on both CPU and XPU, not the Charlie microfacet lobe.
  - Rough glass is **not** energy-compensated: furnace value 0.87–0.997.
  - Coat darkening uses MaterialX's `K = 1-(1-F0)/η²`.
  - CPU and XPU differ for Standard Surface `diffuse_roughness > 0` (XPU matches the MaterialX qualitative Oren-Nayar).
- **Recommendation: a native uber-closure in Nova.** Use one bytecode op per surface model (or one op with a model flag). It fills a canonical `SurfaceInputs` POD and emits ≤8 lobes from a shared lobe library, with energy compensation from lookup tables.
  - This is the Cycles SVM "principled" pattern.
  - Generic pbrlib node graphs (M4) emit into the same closure list.
- **M3 scope.** Everything except random-walk subsurface (diffuse fallback), volumetric `transmission_scatter`, and dispersion.

---

## 1. Sources (local, ground truth)

| What | Path |
|---|---|
| Standard Surface definitions and graph | `$HFS/houdini/materialx/libraries/bxdf/standard_surface.mtlx` (431 lines). An identical copy is in `$HFS/bin/usd_plugins/usdMtlx/resources/libraries/` |
| OpenPBR definition and graph | `$HFS/houdini/materialx/libraries/bxdf/open_pbr_surface.mtlx` (678 lines) |
| pbrlib definitions | `$HFS/houdini/materialx/libraries/pbrlib/pbrlib_defs.mtlx` |
| pbrlib GLSL (reference math) | `.../pbrlib/genglsl/*.glsl`, `.../pbrlib/genglsl/lib/mx_microfacet*.glsl` |
| Translation graphs | `.../bxdf/translation/standard_surface_to_open_pbr.mtlx`, `open_pbr_to_standard_surface.mtlx` |
| Karma CPU MaterialX BSDFs | `$HFS/houdini/vex/include/mxpbr.h`, `mxtypes.h`, `ggx_utils.h`; snippets in `$HFS/bin/libBRAY.dll` |
| Houdini help | `help/nodes.zip: vop/mtlxstandard_surface.txt`, `vop/mtlxopen_pbr_surface.txt`; `help/solaris.zip: karma_xpu.txt`, `support/sss.txt` |
| Example presets | `$HFS/houdini/materialx_resources/Materials/Examples/{StandardSurface,OpenPbr}/*.mtlx` |
| Specs | autodesk.github.io/standard-surface (`index.html` of github Autodesk/standard-surface); OpenPBR v1.1.1 `index.html` (tag v1.1.1; branch dev_1.2 holds the next version) |
| EON reference code (MIT) | github.com/portsmouth/EON-diffuse `main.glsl` |
| Design reference (BSD-3/Apache) | Blender `intern/cycles/kernel/svm/closure.h`, `closure/bsdf_microfacet.h`, `closure/bsdf_util.h` |

---

## 2. Standard Surface 1.0.1 (`ND_standard_surface_surfaceshader`)

### 2.1 Parameters (`standard_surface.mtlx:8-99`; 1.0.1 overrides at lines 8-11)

| Input | Type | Default | UI range | Notes |
|---|---|---|---|---|
| base | float | **1.0** (1.0.0: 0.8) | 0–1 | diffuse weight; metal F0 = base·base_color |
| base_color | color3 | **0.8** (1.0.0: 1.0) | 0–1 | |
| diffuse_roughness | float | 0 | 0–1 | qualitative Oren-Nayar σ, **no** energy compensation |
| metalness | float | 0 | 0–1 | |
| specular | float | 1 | 0–1 | scales the dielectric R lobe; metal edge = specular·specular_color |
| specular_color | color3 | 1 | 0–1 | |
| specular_roughness | float | 0.2 | 0–1 | α = r² |
| specular_IOR | float | 1.5 | min 0, soft 1–3 | also the transmission IOR |
| specular_anisotropy | float | 0 | 0–1 | |
| specular_rotation | float | 0 | 0–1 | ×360° |
| transmission | float | 0 | 0–1 | |
| transmission_color | color3 | 1 | | |
| transmission_depth | float | 0 | soft 0–100 | **ignored by the MaterialX graph**; Karma applies Beer's law |
| transmission_scatter | color3 | 0 | | ignored by the graph and by Karma |
| transmission_scatter_anisotropy | float | 0 | 0–1 | ignored |
| transmission_dispersion | float | 0 | soft 0–100 | **Abbe number V** (0 = off); ignored by the graph |
| transmission_extra_roughness | float | 0 | −1–1 | added to r for the T lobe |
| subsurface | float | 0 | 0–1 | |
| subsurface_color | color3 | 1 | | |
| subsurface_radius | color3 | 1 | | mean free path (scene units) |
| subsurface_scale | float | 1 | soft 0–10 | |
| subsurface_anisotropy | float | 0 | −1–1 | HG g |
| sheen | float | 0 | 0–1 | (the spec table says 0.8; the MaterialX default is 0) |
| sheen_color | color3 | 1 | | |
| sheen_roughness | float | 0.3 | 0–1 | used directly (not squared) |
| coat | float | 0 | 0–1 | |
| coat_color | color3 | 1 | | |
| coat_roughness | float | 0.1 | 0–1 | |
| coat_anisotropy, coat_rotation | float | 0, 0 | 0–1 | |
| coat_IOR | float | 1.5 | soft 1–3 | |
| coat_normal | vector3 | Nworld | | |
| coat_affect_color | float | 0 | 0–1 | |
| coat_affect_roughness | float | 0 | 0–1 | |
| thin_film_thickness | float | 0 | soft 0–2000 | **nm** |
| thin_film_IOR | float | 1.5 | soft 1–3 | |
| emission | float | 0 | soft 0–1 | |
| emission_color | color3 | 1 | | |
| opacity | color3 | 1 | | the graph uses `luminance` with ACEScg coefficients (0.2722287, 0.6740818, 0.0536895) |
| thin_walled | boolean | false | | the graph uses it only for subsurface→translucent; Karma also makes the transmission non-refractive |
| normal, tangent | vector3 | Nworld, Tworld | | |

### 2.2 MaterialX graph `NG_standard_surface_surfaceshader_100` (`standard_surface.mtlx:113-429`)

Derived quantities. Clamping of C is my recommendation; the graph clamps C only for the coat gamma.

```
C  = coat
k  = coat_affect_roughness · coat · coat_roughness                       (117-124)
r_m = lerp(specular_roughness, 1, k)                                     (125-129)
r_t = lerp(clamp(specular_roughness + transmission_extra_roughness,0,1), 1, k)   (135-146)
(αx,αy)  = A_ss(r_m, specular_anisotropy)      (130-133)
(αtx,αty)= A_ss(r_t, specular_anisotropy)      (147-150)
(αcx,αcy)= A_ss(coat_roughness, coat_anisotropy) (345-348)
A_ss(r,a): r2=clamp(r²,ε,1); a>0 ? (min(r2/√(1-min(a,.98)),1), r2·√(1-min(a,.98))) : (r2,r2)
           (genglsl/mx_roughness_anisotropy.glsl; x is the tangent axis)
T_main = specular_anisotropy>0 ? normalize(rot(T, normal, 360°·specular_rotation)) : T   (153-170)
T_coat = coat_anisotropy>0     ? normalize(rot(T, coat_normal, 360°·coat_rotation)) : T  (173-190)
rot(v,a,θ) = v cosθ + (v×a) sinθ + a(a·v)(1-cosθ)        (stdlib genglsl mx_rotate_vector3.glsl)
γ  = 1 + clamp(C,0,1)·coat_affect_color                 (193-203)
ρd = max(base_color,0)^γ ; ρs = max(subsurface_color,0)^γ   (204-219)
metal: r = clamp(base·base_color, 0, .99); g = specular·specular_color   (306-317)
   n = lerp((1+√r)/(1-√r), (1-r)/(1+r), g);  k = √max(0, ((n+1)² r − (n−1)²)/(1−r))   (mx_artistic_ior.glsl)
F0c = ((1−coat_IOR)/(1+coat_IOR))²                       (365-384)
```

Lobe tree, from the bottom of the graph up:

```
diffuse     = oren_nayar(weight=base, color=ρd, σ=diffuse_roughness)               (222-227)
sss_sel     = thin_walled ? translucent(color=ρs) : subsurface_bsdf(ρs, radius·scale, g)  (228-251)
base_mix    = mix(bg=diffuse, fg=sss_sel, subsurface)                               (252-256)
sheen_layer = layer(top = sheen_bsdf(weight=sheen, color, roughness), base_mix)    (259-268)  [mode conty_kulla]
trans_mix   = mix(bg=sheen_layer, fg=dielectric_bsdf T(tint=transmission_color, ior=specular_IOR, αt), transmission) (271-285)
spec_layer  = layer(top = dielectric_bsdf R(weight=specular, tint=specular_color, ior, α, thin film), trans_mix) (288-303)
metal_mix   = mix(bg=spec_layer, fg=conductor_bsdf(n,k, α, thin film), metalness)  (318-333)
attenuated  = metal_mix · lerp(1, coat_color, C)                                    (336-344)
coat_layer  = layer(top = dielectric_bsdf R(weight=coat, tint 1, ior=coat_IOR, αc, coat_normal, T_coat), attenuated) (349-362)
edf         = mix(bg=uniform(emission·emission_color),
                  fg=generalized_schlick_edf(color0=1−F0c, color90=0, exp 5, base=uniform·coat_color), C)  (385-409)
surface(bsdf=coat_layer, edf, opacity=luminance(opacity))                           (413-424)
```

### 2.3 Closed form Nova must implement (graph semantics, identical to Karma)

All E values are evaluated at ωo and are **untinted scalars**. E_X includes the X lobe's own multiple-scattering compensation.

```
f =  C·f_coat
   + (1 − C·E_coat)·t_c·[ M·f_metal
       + (1−M)·( s·c_s·f_specR
           + (1 − s·E_specR)·( X·c_t·f_T(αt)
               + (1−X)·( h·c_h·f_sheen
                   + (1 − h·E_sheen)·( (1−S)·b·ρd·f_ON(σ) + S·(thin ? ρs·f_transl : ρs·f_sss) ) ) ) ) ]
t_c = lerp(1, coat_color, C),  s = specular, c_s = specular_color, X = transmission, c_t = transmission_color,
h = sheen, c_h = sheen_color, S = subsurface, b = base, M = metalness
L_e = presence · emission·emission_color · [ (1−C) + C·coat_color·(1−F0c)·(1 − (1−μo)^5) ]
```

How this differs from the Autodesk spec (`index.html` "Specular reflection"):
- The spec weights the base by `(1 − specular·specular_color·reflectance)`, i.e. tinted. The MaterialX graph and Karma use the untinted albedo. I measured this: `specular_color=(1,0,0)` over white diffuse gives G = 0.9585 at μ=1 in Karma, which is 1 − E_R.
- The spec's coat formula is `lerp(1, coat_color·(1−E_coat), C)`; MaterialX uses `(1−C·E_coat)·lerp(1,coat_color,C)`. They are identical for a white coat.
- The spec's diffuse/subsurface table lists 1.0.0 defaults.

---

## 3. OpenPBR 1.1.1 (`ND_open_pbr_surface_surfaceshader`)

### 3.1 Parameters (`open_pbr_surface.mtlx:8-89`, spec "Parameter reference")

| Input | Type | Default | Range (soft) | Notes |
|---|---|---|---|---|
| base_weight | float | 1 | 0–1 | scales diffuse and metal F0 |
| base_color | color3 | 0.8 | 0–1 | |
| base_diffuse_roughness | float | 0 | 0–1 | **EON** σ (energy compensated) |
| base_metalness | float | 0 | 0–1 | |
| specular_weight | float | 1 | 0–∞ (0–1) | **modulates the IOR** (v1.1.1); scales the metal lobe |
| specular_color | color3 | 1 | 0–1 | dielectric R tint; metal F82 tint |
| specular_roughness | float | 0.3 | 0–1 | |
| specular_ior | float | 1.5 | (1–3) | |
| specular_roughness_anisotropy | float | 0 | 0–1 | no rotation input; rotate `geometry_tangent` upstream |
| transmission_weight | float | 0 | 0–1 | |
| transmission_color | color3 | 1 | 0–1 | tint if depth = 0, else Beer's law |
| transmission_depth | float | 0 | (0–1) | |
| transmission_scatter | color3 | 0 | 0–1 | μs = S/depth |
| transmission_scatter_anisotropy | float | 0 | −1–1 | |
| transmission_dispersion_scale | float | 0 | 0–1 | V_d = abbe/scale |
| transmission_dispersion_abbe_number | float | 20 | (9–91) | |
| subsurface_weight | float | 0 | 0–1 | |
| subsurface_color | color3 | 0.8 | 0–1 | observed multiple-scattering albedo |
| subsurface_radius | float | 1 | (0–1) | mean free path scale |
| subsurface_radius_scale | color3 | (1, 0.5, 0.25) | 0–1 | |
| subsurface_scatter_anisotropy | float | 0 | −1–1 | |
| fuzz_weight / fuzz_color / fuzz_roughness | float / color3 / float | 0 / 1 / 0.5 | 0–1 | Zeltner LTC sheen |
| coat_weight | float | 0 | 0–1 | |
| coat_color | color3 | 1 | 0–1 | **square** of the normal-incidence transmittance |
| coat_roughness, coat_roughness_anisotropy | float | 0, 0 | 0–1 | |
| coat_ior | float | **1.6** | (1–3) | |
| coat_darkening | float | 1 | 0–1 | |
| thin_film_weight / thin_film_thickness / thin_film_ior | float | 0 / **0.5 µm** / **1.4** | | |
| emission_luminance / emission_color | float / color3 | 0 / 1 | (0–1000) | nits; color may exceed 1 |
| geometry_opacity | float | 1 | 0–1 | |
| geometry_thin_walled | bool (uniform) | false | | connected to `surface.thin_walled` |
| geometry_normal, geometry_coat_normal, geometry_tangent, geometry_coat_tangent | vector3 | Nworld/Tworld | | |

### 3.2 Model structure (spec "Model")

```
M_PBR          = mix(ambient, M_surface, α=geometry_opacity)
M_surface      = layer(M_coated-base, S_fuzz, F)
M_coated-base  = layer(M_base-substrate, S_coat, C)
M_base-substr. = mix(M_dielectric-base, S_metal, M)
M_dielectric   = mix(M_opaque-base, S_translucent-base, T)
M_opaque-base  = mix(M_glossy-diffuse, S_subsurface, S)
M_glossy-diff. = layer(S_diffuse, S_gloss)
```

Reduction to lobes (spec "Reduction to a mixture of lobes", boxed equation; transcribed in 3.4).

### 3.3 Exact formulas (spec and MaterialX graph line references)

**Anisotropy** (spec eq. openpbr-anisotropy-formula; graph `open_pbr_anisotropy` at 640-676; no clamp):

```
α_t = r²·√(2/(1+(1−a)²)),   α_b = (1−a)·α_t      (α_t² + α_b² = 2r⁴)
```

**Coat roughening** (spec "Roughening"; graph 99-127):

```
r' = lerp(r_s, min(1, r_s⁴ + 2 r_c⁴)^{1/4}, C)
```

**Specular IOR modulation with coat TIR fix** (spec eqs. specular_ior_ratio, specular_ior_ratio_with_tir_fix, modulated_ior; graph 306-371):

```
ratio = n_b/n_c;  ratio_fix = ratio>1 ? ratio : 1/ratio
η_s   = lerp(n_b, ratio_fix, C)          (n_ambient = 1)
F_s   = ((η_s−1)/(η_s+1))²
ε     = sgn(η_s−1)·√clamp(ξ_s F_s, 0, 0.99999)     ξ_s = specular_weight
η'    = (1+ε)/(1−ε)       → used for dielectric R and T, the refraction direction too (v1.1.1)
```

OpenPBR dev_1.2 decouples the refraction direction from `specular_weight` (merged PR #247; refraction uses the unmodulated η_ti, plus Stokes-relation handling for η<1). Store separate `etaFresnel` and `etaRefract` in the lobe so this needs no rework later.

**Metal, F82-tint** (spec "Metal"; graph 424-455 via `generalized_schlick_bsdf` with color82; `mx_fresnel_hoffman_schlick`, `mx_microfacet_specular.glsl:214`):

```
F_Schlick(μ) = F0 + (1−F0)(1−μ)^5,  F0 = base_weight·base_color
F82(μ) = F_Schlick(μ) − μ(1−μ)^6/(μ̄(1−μ̄)^6)·(F_Schlick(μ̄) − specular_color·F_Schlick(μ̄)),  μ̄ = 1/7
F_metal = specular_weight·F82
```

**EON diffuse** (spec eqs. EON_brdf/FON_brdf/EON_comp; `mx_microfacet_diffuse.glsl:87-137`; EON `main.glsl`):

```
s = ωi·ωo − μiμo ;  s/t = s>0 ? s/max(μi,μo) : s
A = 1/(1 + (1/2 − 2/(3π))σ),  f_ss = ρ/π · A · (1 + σ·s/t)
E_FON(μ) = A + (σA/π)·[ Si(acos μ − Si μ) + (2/3)(Si μ(1+Si+Si²)/(1+Si) − Si) ],  Si = √(1−μ²)
⟨E⟩ = A(1 + (2/3 − 28/(15π))σ)
ρ_ms = ρ²⟨E⟩/(1 − ρ(1−⟨E⟩)),  f_ms = ρ_ms/π · (1−E(μi))(1−E(μo))/(1−⟨E⟩)
```

The spec text puts an extra 1/π (and 1/(1−⟨E⟩)) inside ρ_ms. With that version a white furnace gives (1−E)/π. Use the EON paper code above, which the MaterialX GLSL also follows.

**Glossy-diffuse.** `f_dielectricR + (1 − E_R(ωo))·f_EON(ρ = base_color)` (spec eq. under "Glossy-diffuse"). base_weight multiplies f_diffuse (graph 197-203).

**Translucent base** (spec "Translucent base"; graph 219-296):

```
μt = −ln(T)/d,  μs = S/d,  μa = μt − μs;  if min(μa)<0: μa −= min(μa)
T lobe tint = d>0 ? 1 : transmission_color
dispersion: V_d = abbe/scale;  n(λ) = A + B/λ²,  B = (n_d−1)/(V_d(λF⁻²−λC⁻²)), A = n_d − B/λd²
            λF = 486.1, λd = 587.6, λC = 656.3 nm
```

**Subsurface parameter mapping** (spec "Subsurface"; Karma `mxpbr.h:255-280` uses the same mapping for both models):

```
μt = 1/mfp
s  = 4.09712 + 4.20863C − √(9.59217 + 41.6808C + 17.7126C²)
α  = (1−s²)/(1 − g s²)
```

**Thin-walled subsurface** (spec eq. thin_wall_subsurface):

```
f_R = ½ C (1−g) f_+ ,   f_T = ½ C (1+g) f_−      (f_± are albedo-1 diffuse lobes; lobe shape from base_diffuse_roughness)
```

**Coat darkening** (spec "Darkening"):
- Spec version: `Δ = (1−K)/(1−E_b K)`, `K = lerp(K_s, K_r, r_b)`, `K_r = 1 − (1−E_F(η_c))/η_c²`, `K_s = F(ωo,η_c)`.
- d'Eon fit: `E_F(η) ≈ ln((10893η−1438.2)/(−774.4η²+10212η+1))`.
- Applied factor: `lerp(1, Δ, C·coat_darkening)`.
- **The MaterialX graph (459-524) and Karma use** `K = 1 − (1−F0c)/η_c²` (no E_F, no K_s) and `E_b = lerp(lerp(base_color, subsurface_color, S), specular_weight·base_color, M)`.

**Coat absorption.**
- MaterialX: `lerp(1, coat_color, C)` (527-535).
- Spec (view-dependent, optional): `T_coat^{1/μ_i^t+1/μ_o^t}` with `μ^t = √(1−(1−μ²)/η_c²)`.
- Cycles applies only the ωo half: `slab_color_at_angle`, `pow(color, η/√(η²−(1−μ²)))`.

**Fuzz** (spec "Fuzz"; graph 555-565, sheen_bsdf mode zeltner):

```
μi f = F·E_fuzz(μo,r)·D_LTC(ωi|ωo,r)
weighted layer: F·f_fuzz + (1 − F·E_fuzz)·f_coated   (untinted)
```

**Emission** (graph 568-612):

```
L_e = max(lum,0)·emission_color · [ (1−C) + C·coat_color·(1−F0c)(1−(1−μo)^5) ]
```

Nits are used as radiance 1:1 by MaterialX and Karma. The spec's reduction uses `lerp(1, T_coat, C)·E`.

**Thin film** (graph 299-302, 403-417, 436-450): `thickness_nm = 1000·thin_film_thickness`. The R lobe is `mix(non-tf, tf, thin_film_weight)` for both the dielectric R lobe and the metal. That is linear in F, so Nova can blend `F = lerp(F, F_airy, w)` inside one lobe.

### 3.4 Closed form (graph semantics)

```
f =  F·c_f·f_fuzz + (1 − F·E_fuzz)·[ C·f_coat + (1 − C·E_coat)·t_c·d·(
       M·f_metal(F82, ξ_s)
     + (1−M)·( c_s·f_specR(η') + (1 − E_specR(η'))·(
            T·tint_T·f_T(η')
          + (1−T)·( S·sss + (1−S)·b·f_EON(ρ=max(base_color,0)) ) ) ) ) ]
t_c = lerp(1,coat_color,C),  d = lerp(1, Δ, C·coat_darkening)
sss = thin ? ½(1−g)C·f_ON + ½(1+g)C·f_transl : f_SSS(C, mfp, g)
```

Bug in Houdini's graph (`open_pbr_surface.mtlx:141-178`): it multiplies the thin-walled lobes by C **and** gives the lobes color C, so the effective weight is C². Upstream MaterialX `main` fixed this: it now uses `mix(R(C), T(C), 0.5+0.5g)`. Implement the spec form (C). For C=0.5, thin-walled, direct light only, Karma gave CPU 0.141 vs XPU 0.313. Do not use this configuration as a Karma reference.

---

## 4. pbrlib semantics as implemented in genglsl (quoted)

Closure struct: `BSDF {response, throughput}`, where throughput = 1 − directional albedo, evaluated at V (= ωo).

```glsl
// mx_layer_bsdf.glsl:5-6
result.response = top.response + base.response * top.throughput;
result.throughput = top.throughput * base.throughput;
// mx_mix_bsdf.glsl:5-6
result.response = mix(bg.response, fg.response, mixValue);
result.throughput = mix(bg.throughput, fg.throughput, mixValue);
// mx_add_bsdf.glsl:5,13
result.response = in1.response + in2.response;
result.throughput = max(in1.throughput + in2.throughput - 1.0, 0.0);
// mx_multiply_bsdf_color3.glsl:5-7   (tint clamped to [0,1]; throughput unchanged)
vec3 tint = clamp(in2, 0.0, 1.0); result.response = in1.response * tint; result.throughput = in1.throughput;
// mx_dielectric_bsdf.glsl:46-51 (reflection)
vec3 comp = mx_ggx_energy_compensation(NdotV, avgAlpha, F);          // 1 + F(1-Ess)/Ess
vec3 dirAlbedo = mx_ggx_dir_albedo(NdotV, avgAlpha, F0, 1.0) * comp; // Schlick-type fit, untinted
bsdf.throughput = 1.0 - dirAlbedo * weight;
bsdf.response = D * F * G * comp * safeTint * closureData.occlusion * weight / (4.0 * NdotV);
// mx_generalized_schlick_bsdf.glsl:50-51: throughput = 1 - avg(E(color0,color90))*weight
// mx_sheen_bsdf.glsl:42/59: throughput = 1 - dirAlbedo*weight   (untinted)
// mx_conductor/oren_nayar/translucent/subsurface: throughput = 0 (opaque)
```

Building blocks in `lib/mx_microfacet_specular.glsl`:

| Line | Function | Note |
|---|---|---|
| 34 | GGX NDF | anisotropic |
| 42 | VNDF sampling | Dupuy & Benyoub 2023 spherical caps |
| 72 | Smith G1 | |
| 82 | height-correlated G2 | isotropic, uses `avgAlpha = √(αxαy)` even for anisotropic lobes |
| 91 | `mx_ggx_dir_albedo_analytic` | rational fit, `E = F0·A + F90·B` |
| 178 | energy compensation | `1 + Fss(1−Ess)/Ess`, Turquin 2019 |
| 214 | Hoffman/F82 | |
| 225 | exact dielectric Fresnel | |
| 259 / 278 | conductor Fresnel | |
| 315-463 | `mx_fresnel_airy` | Belcour-Barla 2017; `AIRY_FRESNEL_ITERATIONS` = `GenOptions::hwAiryFresnelIterations = 2` (`MaterialXGenShader/GenOptions.h:91`); assumes η=1 above the film; XYZ→"CIE 1931 RGB, illuminant E" matrix |
| 486-510 | Airy albedo approximation | `mix(F_airy(μ), E(μ,α,F_airy(1),1), √α)` |

Other lobes:
- Sheen (`lib/mx_microfacet_sheen.glsl`):
  - Imageworks/Charlie (5-26): `D = (2+1/r)·sin^{1/r}θh/(2π)`, `f = D/(4(NL+NV−NL·NV))`; albedo rational fit at 28-37.
  - Zeltner: albedo fit at 97-104, LTC `aInv`/`bInv` fits at 106-115, eval at 135-156, sampling at 158-188. Roughness is clamped to [0.01, 1].
- Diffuse (`lib/mx_microfacet_diffuse.glsl:8-19`), the qualitative Oren-Nayar used by Standard Surface:

  ```glsl
  float s = LdotV - NdotL*NdotV;  float stinv = (s > 0.0) ? s / max(NdotL, NdotV) : 0.0;
  float sigma2 = roughness*roughness; float A = 1.0 - 0.5*(sigma2/(sigma2+0.33)); float B = 0.45*sigma2/(sigma2+0.09);
  return A + B*stinv;      // f = color/π · this
  ```
- Translucent: `color/π` in the flipped hemisphere.
- Subsurface: screen-space approximation (rasteriser only).
- `anisotropic_vdf`: `exp(−absorption)` only.

**Measured error of the MaterialX albedo fit.** I integrated numerically (VNDF, 512² samples; `cpp/albedo.cpp`). The fit is off by up to 3.5%: r=0.3, μ=0.125 gives an exact E_ss of 0.8876 vs a fit of 0.9226. Do not use the fit in Nova; use tables (6.4).

---

## 5. Karma

### 5.1 Karma CPU (VEX, generic graph)

- **Node snippets.** `libBRAY.dll` maps each pbrlib node definition to a snippet, e.g. `mx_bsdf out_$NODE_ID = mxpbr_dielectric($weight,$tint,$ior,$roughness,$normal,$tangent,$distribution,$scatter_mode,$thinfilm_thickness,$thinfilm_ior,'$bsdflabel',__outside_ior,__thin_walled,__transmission_dispersion,ndior,__internalreflect,__openpbr_transmissiontint,$retroreflective,$base)`. The `layer` node is implicit: its base is passed as `$base`.
- **Closure state.** `mx_bsdf {bsdf F; vector Af}` (`mxtypes.h:22`). `mix_bsdf` lerps F and Af; `add_bsdf` sets Af = a+b−1; `mul_bsdf` sets Af = 1−(1−Af)·b (`mxtypes.h:35-71`).
- **Dielectric R** (`mxpbr.h:605-722`):
  - The lobe is a GGX tinted with `clr`, normalised by `1/ggx_albedo_curve_fit(μ,α,1,1)`.
  - It is mixed with the base as `alb·f + (1−alb)·base`, where `alb = ggx_albedo_curve_fit_ior(μ, α, ior)` (`ggx_utils.h:91`). That is the MaterialX fit, forced to Schlick as α→0, with an extra `pow(…, smooth(1,1.4,ior)^−0.2)`.
  - The albedo is untinted.
  - With thin film, `alb = _mx_fresnel_airy(μo)`.
- **Dielectric T** (`scatter_mode "T"`): a full GGX with `fresblend 2` (pure refraction except under TIR, so no Fresnel weighting). Weight is `w·clr·openpbr_transmissiontint`. The R layer above supplies the (1−E_R) factor.
- **Conductor** (`mxpbr.h:783-816`): exact n,k with `CONDUCTOR_FRESNEL_FIX` defined, times `1/E_ss(F=1)`.
- **Sheen** (`mxpbr.h:994-1022`):
  - zeltner: `mix(base, clr·sheen(N,r), w·R_zeltner)`.
  - conty_kulla (Standard Surface): `mix(base, clr·2·diffuse(N), w·E_imageworks_robust)`, i.e. a Lambert lobe weighted by the sheen albedo.
- **Subsurface:** `randomwalk` with the OpenPBR α(C,g) mapping (`mxpbr.h:1024-1041`).
- **Transmission depth, thin-walled tint** (VEX prologue in `libBRAY.dll`):

  ```
  if (!shadow && !thin && transmission>0 && depth>0 && color!=1): tint=1; absorption = −ln(clamp(color,1e−5,1))/depth
  else: tint=color; if thin: tint = pow(color, 1/√(1 − (1/ior)²(1−(N·I)²)))
  ```
- **Dispersion:** for OpenPBR, `__transmission_dispersion = abbe/scale`. VEX dispersion = `clamp(0.28/(V·η²), 0, 1)` with η = 1/ior (`mxpbr.h:181-193`). Thin-walled disables it.
- **Fake caustics.** Shadow rays use `Of = f(Af)`, so thin translucent and glass walls pass light in shadow rays. This confounds direct-light probes of thin-walled materials.

### 5.2 Karma XPU

Native Standard Surface and OpenPBR. Documented limits (`karma_xpu.txt:312-326`): transmission scatter and its anisotropy are ignored; "Unlike the MaterialX standard, Thin Walled will work as expected"; SSS only on meshes and points; blending SSS or absorption is not supported. The help says Karma CPU and XPU give "nearly identical" results for OpenPBR (`mtlxopen_pbr_surface.txt`).

### 5.3 Measured directional albedo E(μo), Karma 22.0.466

Setup (husk, isolated environment): unit sphere mesh 160×80 with exact normals; white dome with intensity 1; orthographic camera (aperture 20, i.e. 2 units); box filter; 256 spp; `diffuselimit = reflectlimit = refractlimit = 0` (direct light only). A pixel at radius ρ gives E at μo = √(1−ρ²). Values are bin means; bins are centred at μ ≈ 0.98, 0.88, 0.70, 0.50, 0.30, 0.12.

| Case | CPU | XPU | Interpretation |
|---|---|---|---|
| SS Lambert (base_color 1, specular 0) | 1.000 everywhere | – | probe exact |
| SS white diffuse + spec (r 0.2) | 1.000 … 0.996 | – | ≈ furnace-preserving |
| SS diffuse 0.5 + spec (r 0.3, η 1.5) | .5204 .5197 .5216 .5370 .5832 .6834 | identical | exact albedo scaling: .5199 .5207 .5257 .5434 .5882 .6552 |
| SS specular_color (1,0,0) over white | R≈1, G=B .9585 .9603 .9572 .9281 .8317 .7107 | identical | **untinted** layering |
| SS diffuse_roughness 1 | .6261 .6416 .6703 .7086 .7480 .8019 | .6386 .6721 .7086 .7475 .7837 .8305 | MaterialX QON (numeric) .6418 .6722 .7086 .7471 .7837 .8126; **XPU = MaterialX, CPU differs** |
| SS metal, base 1, r 0.2 / 0.7 | .988–1.003 / .986 … .916 | r 0.7 identical | F0 clamped to 0.99; no proper compensation at high roughness |
| SS coat 1 (r 0.1) over white | 1.000 | – | |
| SS sheen 1 over white | 1.000 | 1.000 | |
| OpenPBR defaults, base_color 1 | 1.000 … .988 | – | |
| OpenPBR EON σ=1, white | 1.000 | 1.000 | EON exact |
| OpenPBR metal white r 0.3 / 0.7 | .99–1.013 / .996 … .922 | r 0.7 identical | |
| OpenPBR coat 1 (η 1.6, r 0) over diffuse 0.5, specular_weight 0 | .3089 .3089 .3105 .3310 .4260 .6978 | identical | MaterialX K with exact Fresnel: .3089 .3099 .3175 .3468 .4340 .6434 (μ=1 exact; Karma uses Schlick at grazing). Spec E_F-based K: .2980 … |
| OpenPBR fuzz white | 1.000 … .994 | same | |

Multi-bounce white furnaces (limits 64):

| Case | CPU | XPU |
|---|---|---|
| Smooth glass, η 1.5 | 1.000 | – |
| **Rough glass r 0.3 (SS = OpenPBR)** | .997 .994 .982 .946 .894 .869 | .990 … .862 |
| Coated rough metal | .998 … .968 | .998 … .968 |

Back-lit velvet (Standard Surface sheen, normalised distant light at 150°): the velvet/Lambert(0.05) ratio is 8.3 and 12.6 in the μ≈0.3 and 0.12 bins, on both CPU and XPU. Predicted ratios (`cpp/sheen.cpp`): albedo-weighted Lambert 8.4 / 12.3; MaterialX Charlie lobe 3.2 / 8.5. So **Karma's Standard Surface sheen is albedo-weighted Lambert** on CPU and XPU.

---

## 6. Recommendation for Nova

### 6.1 Native uber-closure vs. generic pbrlib interpretation

| | Native op per model, shared lobe library (recommended) | Generic pbrlib graph (Karma CPU style) |
|---|---|---|
| Faithfulness | Matches the graph semantics exactly where intended (2.3/3.4). Can add what the graph drops: transmission depth, thin-walled refraction-free glass, glass energy compensation, tables instead of fits, spec-correct thin-walled subsurface | Inherits graph bugs and omissions (C², depth ignored, Standard Surface thin_walled half-done, fits) |
| Speed | One op evaluates about 40 inputs. Lobes with weight < 1e-5 are culled and never emitted. Albedos come from tables | 10–15 BSDF nodes per shader, each computing albedo/throughput; zero-weight nodes still run |
| GPU | Fixed ≤8-lobe POD list, `switch` dispatch, same function on CUDA (Cycles SVM pattern) | Dynamic node count, deep closure tree; harder to bound registers |
| Custom BSDF graphs | M4: pbrlib nodes emit into the same closure list | native |

Every pbrlib operator reduces to scaling leaf weights:
- `mix(fg,bg,m)`: emit(fg, w·m), emit(bg, w·(1−m))
- `add`: concatenate
- `multiply(c)`: emit(in, w·clamp(c,0,1))
- `layer(top,base)`: emit(top, w); then `w' = w·(1 − Σ_top weight_i·E_i(ωo))`; emit(base, w')

So the generic path in M4 needs only one extra op, `CLOSURE_ALBEDO(range) → register`. Both paths share the lobe library and the integrator code.

### 6.2 Files and structures

```
core/bsdf/
  closure.h        Lobe, ClosureList, LobeType/flags (POD, NOVA_HOST_DEVICE)
  microfacet.h     GGX (D, Λ, G1, G2, VNDF sample/pdf), Fresnel (dielectric, conductor, F82, Airy)
  lobes.h          eval/sample/pdf/albedo per LobeType (switch, no virtual calls)
  tables.h         BsdfTables view (pointers + sizes), bilinear/trilinear lookup
  tables_data.inc  generated (tools/nova_bsdf_tables), constexpr float arrays
  surface.h/.cpp   SurfaceInputs + buildStandardSurface()/buildOpenPBR() → ClosureList
core/shading/      (VM topic) terminal op EMIT_SURFACE(model) writes SurfaceInputs, calls build*()
tools/nova_bsdf_tables/  offline integrator (VNDF stratified, double precision) → tables_data.inc
```

```cpp
// core/bsdf/surface.h : what the bytecode produces (one block per shading point)
enum class SurfaceModel : uint8_t { StandardSurface = 0, OpenPBR = 1 };
struct SurfaceInputs {                       // linear rendering-space RGB, world-space vectors
    SurfaceModel model; uint8_t thinWalled, pad[2];
    float baseWeight;  Spectrum baseColor;  float diffuseRoughness;  float metalness;
    float specularWeight; Spectrum specularColor; float specularRoughness; float specularIor;
    float specularAnisotropy; float specularRotation;            // rotation: SS only (OpenPBR 0)
    float transmissionWeight; Spectrum transmissionColor; float transmissionDepth;
    Spectrum transmissionScatter; float transmissionScatterAnisotropy;
    float abbe;                                                  // effective V_d, 0 = none
    float transmissionExtraRoughness;                            // SS only
    float subsurfaceWeight; Spectrum subsurfaceColor; Spectrum subsurfaceMfp; float subsurfaceAnisotropy;
    float sheenWeight; Spectrum sheenColor; float sheenRoughness;            // SS sheen | OpenPBR fuzz
    float coatWeight; Spectrum coatColor; float coatRoughness, coatAnisotropy, coatRotation, coatIor;
    float coatDarkening;                                         // OpenPBR
    float coatAffectColor, coatAffectRoughness;                  // SS
    float thinFilmWeight, thinFilmThicknessNm, thinFilmIor;
    Spectrum emission;                                           // SS: emission*color, OpenPBR: max(lum,0)*color
    float opacity;                                               // SS: luminance(opacity)
    Vec3f normal, coatNormal, tangent, coatTangent;
};
```

How the VM fills it: each material carries a constant `SurfaceInputs` template. Translation time resolves node-definition defaults and folds constants into it. A short patch list `(fieldOffset ← register)` covers the varying inputs. `EMIT_SURFACE` copies the template, applies the patches, normalises the vectors, and calls `buildOpenPBR()` or `buildStandardSurface()`. This is cheap and works the same on GPU.

```cpp
// core/bsdf/closure.h
enum LobeType : uint8_t {
    kLobeDiffuse,      // mode 0 Lambert, 1 QON (SS), 2 EON (OpenPBR); ax = sigma, c0 = rho
    kLobeTranslucent,  // Lambert BTDF; c0 = color
    kLobeSheen,        // SS sheen; mode 0 Charlie (MaterialX), 1 albedo-Lambert (Karma compat); ax = r
    kLobeFuzz,         // Zeltner LTC; ax = clamp(r, 0.01, 1)
    kLobeConductor,    // mode 0 F82 (c0 = F0, c1 = B, p = specular_weight), mode 1 n,k (c0 = n, c1 = k)
    kLobeDielectric,   // c0 = reflection tint, c1 = transmission tint (0 = no T)
    kLobeSubsurface,   // M3: Lambert(c0); M9: random walk (c1 = mfp, ax = g)
};
enum : uint8_t { kLobeSingular = 1, kLobeTransmit = 2, kLobeThinWalled = 4, kLobeThinFilm = 8, kLobeSplitAlpha = 16 };

struct Lobe {                      // POD, 80 bytes
    Spectrum weight;               // includes mix/layering/coat tint/darkening, evaluated for wo
    float    pick;                 // selection probability (normalized over the list)
    uint8_t  type, flags, frame, mode;
    float    ax, ay;               // GGX alphas (R)
    float    axT, ayT;             // T alphas (SS transmission_extra_roughness), else = ax, ay
    float    etaF, etaT;           // dielectric: Fresnel / refraction IOR ratio (equal in v1.1.1)
    float    energy;               // MS compensation (1 + Fms (1-E)/E) or 1/E_glass
    Spectrum c0, c1;
    float    tfThicknessNm, tfIor, tfWeight;
};
struct ClosureList {
    static constexpr int kMaxLobes = 8;   // worst case: fuzz, coat, metal, dielectric, sss (2 if thin), diffuse = 7
    Lobe lobe[kMaxLobes];
    Frame frame[2];                       // 0 base (t = anisotropy tangent), 1 coat
    Spectrum emission, albedo, sigmaA;    // sigmaA: interior absorption for transmission_depth > 0
    float presence;                       // opacity (stochastic pass-through, also in shadow rays)
    uint8_t count, thinWalled, hasTransmission, frontFacing;
};
NOVA_HOST_DEVICE Spectrum evalClosures(const ClosureList&, Vec3f woW, Vec3f wiW, float &pdf); // f*|cos|, non-singular lobes
NOVA_HOST_DEVICE bool     sampleClosures(const ClosureList&, Vec3f woW, Vec3f u, BsdfSample&);  // u = get3D(kBsdf)
```

Changes to `core/integrator/wavefront.cpp`:

| Line | Change |
|---|---|
| 235 | Replace `LambertBsdf bsdf{hit.color}` with `SurfaceInputs in = runMaterial(...)` and `ClosureList cl = build…(in, geom, wo, tables)` |
| new | Add `beta·cl.emission` with weight 1 (no emissive-mesh light sampling in M3) |
| new | Opacity: if `u = get1D(bounce(d, kOpacity)) ≥ presence`, pass the ray through without a scattering event. This needs a per-path bounce counter instead of the wave-global `depth`, so streams use the path's own index |
| 263-264 | NEE: `evalClosures` returns f·\|cos\| and the mixture pdf over non-singular lobes. Accept ωi on both sides when `cl.hasTransmission`, with the consistency test `sign(dot(ng,wi)) == sign(dot(ns,wi))` |
| 272 | `sampleClosures(cl, wo, sampler.get3D(bounce(depth, kBsdf)))`: u.z selects the lobe, then is rescaled for the R/T choice. Stream 1 is already reserved as 3D in `sobol.h:146` |
| 244, 274 | Offset the origin to `-ng` for transmission; flip the `dot(wi,ng)>0` test |
| 286 | `bsdfPdf`: use a sentinel (`< 0`) after a singular sample. `bsdfSampleWeight()` (`lights.h:411`) must then return 1. Today `powerHeuristic(0, lightPdf)` returns 0, so light reached through mirrors and glass would vanish |
| path state | New: `uint8_t flags` (inside medium, last bounce singular) and `Spectrum sigmaA`. At the next hit, `beta *= exp(−sigmaA·hitT)` |

### 6.3 Lobe math (ready to adapt)

```cpp
// --- roughness mappings -------------------------------------------------------
NOVA_INLINE Vec2f alphaSS(float r, float a) {                 // mx_roughness_anisotropy.glsl
    const float r2 = fminf(fmaxf(r * r, 1e-8f), 1.0f);
    if (a > 0.0f) { const float asp = sqrtf(1.0f - fminf(a, 0.98f)); return {fminf(r2 / asp, 1.0f), r2 * asp}; }
    return {r2, r2};
}
NOVA_INLINE Vec2f alphaOpenPBR(float r, float a) {            // open_pbr_surface.mtlx:640-676
    const float i = 1.0f - a, at = r * r * sqrtf(2.0f / (1.0f + i * i));
    return {at, i * at};
}
// --- GGX (anisotropic Smith; MaterialX uses isotropic avg alpha in G2, this is more correct) ---
struct GGX { float ax, ay;
  NOVA_INLINE float D(Vec3f m) const { float t = sq(m.x/ax) + sq(m.y/ay) + sq(m.z); return 1.0f / (kPi*ax*ay*t*t); }
  NOVA_INLINE float lambda(Vec3f w) const { return 0.5f * (-1.0f + sqrtf(1.0f + (sq(ax*w.x) + sq(ay*w.y)) / sq(w.z))); }
  NOVA_INLINE float G1(Vec3f w) const { return 1.0f / (1.0f + lambda(w)); }
  NOVA_INLINE float G2(Vec3f o, Vec3f i) const { return 1.0f / (1.0f + lambda(o) + lambda(i)); }
  NOVA_INLINE float Dv(Vec3f o, Vec3f m) const { return G1(o) * fabsf(dot(o, m)) * D(m) / fabsf(o.z); }
  NOVA_INLINE Vec3f sampleVisible(Vec3f o, Vec2f u) const {      // Dupuy & Benyoub 2023, needs o.z > 0
    const Vec3f h0 = normalize(Vec3f{ax*o.x, ay*o.y, o.z});
    const float phi = kTwoPi*u.x, z = (1.0f - u.y)*(1.0f + h0.z) - h0.z, s = sqrtf(fmaxf(0.0f, 1.0f - z*z));
    const Vec3f h = Vec3f{s*cosf(phi), s*sinf(phi), z} + h0;
    return normalize(Vec3f{ax*h.x, ay*h.y, fmaxf(h.z, 0.0f)});
  }
};
NOVA_INLINE float fresnelDielectric(float c, float eta) {     // eta = n_t/n_i; c<0: from the other side
    if (c < 0.0f) { eta = 1.0f / eta; c = -c; }
    const float g2 = eta*eta - 1.0f + c*c; if (g2 < 0.0f) return 1.0f;   // TIR
    const float g = sqrtf(g2), A = (g - c)/(g + c), B = (c*(g + c) - 1.0f)/(c*(g - c) + 1.0f);
    return 0.5f*A*A*(1.0f + B*B);
}
// --- F82-tint (OpenPBR metal), avg for MS compensation (Cycles bsdf_util.h:140-160) ---
NOVA_INLINE Spectrum f82B(Spectrum F0, Spectrum tint) {
    const float f = 6.0f/7.0f, f5 = f*f*f*f*f;
    return (F0 + (spectrum(1) - F0)*f5) * (7.0f/(f5*f)) * (spectrum(1) - tint);
}
NOVA_INLINE Spectrum f82(float c, Spectrum F0, Spectrum B) {
    const float s = fminf(fmaxf(1.0f - c, 0.0f), 1.0f), s5 = s*s*s*s*s;
    return clamp01(F0 + (spectrum(1) - F0)*s5 - B*(c*s5*s));
}
NOVA_INLINE Spectrum f82Avg(Spectrum F0, Spectrum B) { return lerp(F0, spectrum(1), 1.0f/21.0f) - B*(1.0f/126.0f); }
// --- artistic IOR (SS metal), mx_artistic_ior.glsl ---
NOVA_INLINE void artisticIor(Spectrum refl, Spectrum edge, Spectrum &n, Spectrum &k) {
    const Spectrum r = clamp(refl, 0.0f, 0.99f), rs = sqrt(r);
    n = lerp((spectrum(1) + rs)/(spectrum(1) - rs), (spectrum(1) - r)/(spectrum(1) + r), edge);
    k = sqrt(max((sq(n + 1.0f)*r - sq(n - 1.0f))/(spectrum(1) - r), 0.0f));
}
// --- OpenPBR modulated specular IOR incl. coat TIR fix (open_pbr_surface.mtlx:306-371) ---
NOVA_INLINE float openpbrEta(float nb, float nc, float C, float xi) {
    const float ratio = nb / nc, fix = ratio > 1.0f ? ratio : 1.0f / ratio;
    const float es = lerp(nb, fix, C), F = sq((es - 1.0f)/(es + 1.0f));
    const float e = copysignf(sqrtf(fminf(fmaxf(xi*F, 0.0f), 0.99999f)), es - 1.0f);
    return (1.0f + e) / (1.0f - e);
}
```

Dielectric R+T, Walter 2007 with tints. Adapted from pbrt-v4 `DielectricBxDF::f/PDF` (Apache-2.0):

```cpp
// wo, wi local (+z = side the normal points to); eta = n_inside/n_outside; tR, tT tints (tT = 0: no T)
NOVA_INLINE Spectrum evalDielectric(const GGX &d, float eta, Spectrum tR, Spectrum tT, Vec3f wo, Vec3f wi, float &pdf) {
    pdf = 0.0f; const float co = wo.z, ci = wi.z;
    if (co == 0.0f || ci == 0.0f) return {};
    const bool refl = co * ci > 0.0f;
    const float etap = refl ? 1.0f : (co > 0.0f ? eta : 1.0f / eta);
    Vec3f m = wi * etap + wo; if (dot(m, m) == 0.0f) return {};
    m = normalize(m); if (m.z < 0.0f) m = -m;
    if (dot(m, wi) * ci < 0.0f || dot(m, wo) * co < 0.0f) return {};
    const float F = fresnelDielectric(dot(wo, m), eta);
    const float pR = F * maxComponent(tR), pT = (1.0f - F) * maxComponent(tT);
    if (pR + pT <= 0.0f) return {};
    const Vec3f woU = co > 0.0f ? wo : -wo;
    if (refl) {
        pdf = d.Dv(woU, m) / (4.0f * fabsf(dot(wo, m))) * pR / (pR + pT);
        return tR * (d.D(m) * d.G2(wo, wi) * F / fabsf(4.0f * ci * co));
    }
    const float den = sq(dot(wi, m) + dot(wo, m) / etap);
    pdf = d.Dv(woU, m) * fabsf(dot(wi, m)) / den * pT / (pR + pT);
    const float ft = d.D(m) * (1.0f - F) * d.G2(wo, wi) * fabsf(dot(wi, m) * dot(wo, m) / (den * ci * co));
    return tT * (ft / sq(etap));                     // radiance transport (camera paths)
}
```

- **Sampling:** draw m from the VNDF of `woU`; choose R with probability pR/(pR+pT); otherwise refract with `etap`. If refraction is impossible, return no sample.
- **Singular case** (α < 1e-4): choose R with probability ∝ F·max(tR), else T. The returned weight is f·cos/pdf = tR·F/P_R or tT·(1−F)/(P_T·etap²).
- **Energy:** multiply both parts by `energy = 1/E_glass(μo,α,η)` from the table.
- **SS split α** (`transmission_extra_roughness ≠ 0`):
  - R uses `ax,ay`; T uses `axT,ayT` with its own Walter BTDF scaled by `(1−E_R(αR))/(1−E_R(αT))`.
  - Choose R/T by table albedos; the pdf is the mixture.
- **SS `specular < 1`:** tR = s·c_s and tT = X·c_t·(1−s·E_R)/(1−E_R). This keeps total energy as in the graph; s=0 gives full transmission.
- **Thin-walled** (both models, matching Karma and the spec): no refraction.
  - Sheet Fresnel (Cycles `bsdf_thin_glass_fresnel`): `c = T_color^{1/|cosθt|}`; `T' = c·t1·t2/(1−(r2 c)²)`; `R' = r1 + T'·r2·c`, with r=F(μo), t=1−F.
  - The R lobe is the GGX reflection weighted by R'. The T lobe is the same GGX shape mirrored to the lower hemisphere (`wi.z → −wi.z`), weighted by T'.

Other lobes:
- **Conductor:** `f = D·G2·F(wo·m)/(4|ci co|)·energy`; pdf = Dv/(4|wo·m|).
  - F is F82 (mode 0) times specular_weight, or exact n,k (mode 1, `mx_fresnel_conductor`).
  - Thin film: `lerp(F, F_airy, tfWeight)`. For F82 metals the Airy base IOR comes from Gulbrandsen's n,k (as Cycles does).
- **EON:** use `f_EON`, `E_EON`, `sample_EON`, `pdf_EON` verbatim from EON `main.glsl` (MIT): CLTC sampling plus uniform sampling, mixed by the fitted P_u.
- **QON:** `f = ρ/π·(A + B·stinv)` (4.); cosine sampling. Albedo for lobe selection: the MaterialX rational fit (`mx_microfacet_diffuse.glsl:21-28`).
- **Charlie:** `D = (2+1/r)·sin^{1/r}θh/(2π)`, `f = c·D/(4(NL+NV−NL·NV))`. Sample the uniform hemisphere (pdf 1/2π). Take the albedo from a table, not the fit.
- **Zeltner:** `f·μi = c·R(μo,r)·D_LTC(ωi)`. LTC sampling is exact, so the sample weight is c·R. Port `mx_microfacet_sheen.glsl:97-188` 1:1.
- **Translucent:** `c/π` for ωi in the hemisphere opposite the forward-facing N; cosine sampling.

### 6.4 Tables (offline tool, double precision, VNDF stratified ≥1024²)

| Table | Dimensions | Use |
|---|---|---|
| `E_ggx(μ, r)` | 32×32 | GGX albedo, F=1 |
| `Eavg_ggx(r)` | 32 | Kulla-Conty E_avg |
| `E_dielR(μ, r, z)` | 16 z × 32 × 32 | dielectric reflection albedo, exact Fresnel, z = √F0 ∈ [0,1) ↔ η ∈ [1, ∞); layering weights for the dielectric and coat |
| `E_glass(μ, r, η)`, `E_glassInv` | 2 × 16³ | R+T albedo for glass compensation (`energy = 1/E`) |
| `S(μ, r)` (3 channels) | 32×32×3 | ∫f₁, ∫f₁(1−c)⁵, ∫f₁·c(1−c)⁶, with c=ωo·h. Exact single-scatter albedo of any Schlick/F82 Fresnel: `F0·S0 + (1−F0)·S1 − B·S2`; also the Airy albedo approximation |
| `E_charlie(μ, r)` | 32×32 | |

- **Multiple-scattering compensation** (Cycles `microfacet_ggx_preserve_energy`, `bsdf_microfacet.h:544-600`):

  ```
  E = E_ggx(μo, √√(αxαy)) ;  Fms = Fss·Eavg / (1 − Fss(1−Eavg))
  f_comp = f_ss · (1 + Fms·(1−E)/E)                 (F ≡ 1 → albedo exactly 1)
  Fss: F82 → f82Avg ; n,k → f82Avg(fit F82 at 0° and acos(1/7)) ; dielectric R → (η−1)/(4.08567+1.00071η)
  ```
- **Layering albedo of a compensated dielectric R lobe:** `E_R = E_dielR(μo, r, η)·(1 + Fms(1−E)/E)`.
- **Thin film** (no table possible): the MaterialX approximation `lerp(F_airy(μo), F_airy(1)·S0 + S1, √α)`.

Size is under 300 KB. On GPU this goes in texture or global memory with `__ldg`; constant memory is limited to 64 KB.

Numeric anchors from my VNDF integration (`cpp/albedo.cpp`), useful as table tests:

| r \ μ | 0.98 | 0.875 | 0.7 | 0.5 | 0.3 | 0.125 |
|---|---|---|---|---|---|---|
| E_ggx r=0.2 | 0.9980 | 0.9976 | 0.9971 | 0.9957 | 0.9888 | 0.9468 |
| E_ggx r=0.3 | 0.9904 | 0.9891 | 0.9850 | 0.9752 | 0.9460 | 0.8876 |
| E_ggx r=0.5 | 0.9142 | 0.9050 | 0.8853 | 0.8573 | 0.8379 | 0.8765 |
| E_ggx r=0.7 | 0.6967 | 0.6924 | 0.6906 | 0.7040 | 0.7490 | 0.8413 |
| E_ggx r=1.0 | 0.3108 | 0.3331 | 0.3789 | 0.4507 | 0.5601 | 0.7253 |
| E_dielR,ss η1.5 r≈0 | 0.0400 | 0.0413 | 0.0509 | 0.0892 | 0.2078 | 0.5000 |
| E_dielR,ss η1.5 r=0.3 | 0.0398 | 0.0414 | 0.0515 | 0.0868 | 0.1759 | 0.3088 |
| E_dielR,ss η1.5 r=0.6 | 0.0337 | 0.0352 | 0.0410 | 0.0549 | 0.0811 | 0.1216 |
| Zeltner R, r=0.5 | 0.0259 | 0.0341 | 0.0651 | 0.1497 | 0.3152 | 0.5249 |
| QON albedo σ=1 | 0.6418 | 0.6722 | 0.7086 | 0.7471 | 0.7837 | 0.8126 |

### 6.5 Build order (top → bottom, ωo-dependent weights; Cycles `principled_bsdf_emission` pattern)

```cpp
void buildOpenPBR(const SurfaceInputs &p, const ShadingGeom &g, Vec3f wo, const BsdfTables &T, ClosureList &cl) {
  Spectrum w = spectrum(1); cl.presence = clamp01(p.opacity);
  const float C = clamp01(p.coatWeight), M = clamp01(p.metalness), X = clamp01(p.transmissionWeight),
              S = clamp01(p.subsurfaceWeight), F = clamp01(p.sheenWeight);
  // 1 fuzz: normal lerp(N, Nc, C) (spec; MaterialX uses N)
  if (F > kCut) { float Ef = zeltnerR(muF, clamp(p.sheenRoughness, .01f, 1.f));
                  push(kLobeFuzz, w*F*p.sheenColor); w *= 1.0f - F*Ef; }
  // 2 coat
  if (C > kCut) { Vec2f ac = alphaOpenPBR(p.coatRoughness, p.coatAnisotropy);
                  push(kLobeDielectric, w*C, frame 1, eta = p.coatIor (oriented), tR = 1, tT = 0);
                  w *= 1.0f - C*E_R(muC, ac, p.coatIor);
                  w *= lerp(spectrum(1), p.coatColor, C);
                  float F0c = sq((p.coatIor-1)/(p.coatIor+1)), K = clamp01(1 - (1-F0c)/sq(p.coatIor));  // MaterialX
                  Spectrum Eb = lerp(lerp(p.baseColor, p.subsurfaceColor, S), p.specularWeight*p.baseColor, M);
                  w *= lerp(spectrum(1), (1-K)/(spectrum(1) - Eb*K), C*p.coatDarkening); }
  cl.emission = cl.presence * p.emission * lerp(spectrum(1), p.coatColor*((1-F0c)*(1-pow5(1-muC))), C);
  // 3 roughness (roughened by coat)
  float rs = lerp(p.specularRoughness, powf(fminf(1, pow4(p.specularRoughness) + 2*pow4(p.coatRoughness)), .25f), C);
  Vec2f as = alphaOpenPBR(rs, p.specularAnisotropy);
  // 4 metal
  if (M > kCut) { Spectrum F0 = p.baseWeight*p.baseColor; push(kLobeConductor/F82, w*M*p.specularWeight, F0, f82B(F0, p.specularColor), as); }
  w *= 1 - M;
  // 5 dielectric base (R tinted by specular_color, T untinted except tint_T)
  float eta = openpbrEta(p.specularIor, p.coatIor, C, p.specularWeight);
  Spectrum tT = X * (p.transmissionDepth > 0 ? spectrum(1) : p.transmissionColor);
  push(kLobeDielectric, w, tR = p.specularColor, tT, as, eta, thin film);
  if (X > 0 && p.transmissionDepth > 0 && !thin) cl.sigmaA = -log(clamp(p.transmissionColor, 1e-5f, 1.f)) / p.transmissionDepth; // = mu_t (Karma); M8: split mu_s
  w *= (1 - X) * (1 - E_R(mu, as, eta));
  // 6 opaque base
  if (S > kCut) { if (thin) { push(kLobeDiffuse/QON-or-EON, w*S*0.5f*(1-g)*Css); push(kLobeTranslucent, w*S*0.5f*(1+g)*Css); }
                  else push(kLobeSubsurface, w*S*Css /* M3 Lambert */, mfp = radius*radiusScale, g); }
  push(kLobeDiffuse/EON, w*(1-S)*p.baseWeight, rho = max(p.baseColor, 0), sigma = p.diffuseRoughness);
  normalize picks: pick_i ∝ luminance(weight_i)·E_i(wo)
}
```

`buildStandardSurface` follows 2.3 with these differences:
- coat_affect_color power and coat_affect_roughness lerp;
- `alphaSS` and tangent rotation;
- conductor mode n,k from `artisticIor(base·base_color, specular·specular_color)`;
- dielectric tR = s·c_s and tT = X·c_t·(1−s·E_R)/(1−E_R), with split α;
- sheen = `kLobeSheen` (Charlie);
- diffuse = QON (mode 1), ρ = base_color^γ, weight = base;
- coat factor `(1−C·E_c)·lerp(1,coat_color,C)`, no Δ;
- emission formula from 2.3.

### 6.6 Mapping table (canonical block ↔ models)

| Canonical field | OpenPBR 1.1.1 | Standard Surface 1.0.1 | Notes |
|---|---|---|---|
| baseWeight, baseColor | base_weight, base_color | base, base_color (^γ for diffuse) | same role (diffuse weight, metal F0 = w·c) |
| diffuseRoughness | base_diffuse_roughness (EON) | diffuse_roughness (QON, uncompensated) | lobe mode by model |
| metalness | base_metalness | metalness | |
| specularWeight | specular_weight (IOR modulation, metal scale) | specular (R scale, metal edge factor) | semantics differ |
| specularColor | dielectric tint, F82 tint | dielectric tint, metal edge color | metal Fresnel: F82 vs Gulbrandsen n,k |
| specularRoughness | specular_roughness | specular_roughness | roughening: OpenPBR `(r⁴+2r_c⁴)^¼`; SS `lerp(r,1,C·car·r_c)` |
| specularIor | specular_ior | specular_IOR | OpenPBR adds coat ratio and TIR fix |
| specularAnisotropy / Rotation | specular_roughness_anisotropy / – | specular_anisotropy / specular_rotation | α mapping differs (6.3) |
| transmission* | transmission_weight/color/depth/scatter/scatter_anisotropy | transmission/... | SS graph ignores depth; Karma uses Beer for both |
| abbe | abbe_number/scale | transmission_dispersion (V) | |
| transmissionExtraRoughness | – | transmission_extra_roughness | |
| subsurfaceWeight/Color | subsurface_weight/color | subsurface/subsurface_color (^γ) | |
| subsurfaceMfp | subsurface_radius·radius_scale | subsurface_radius·subsurface_scale | |
| subsurfaceAnisotropy | subsurface_scatter_anisotropy | subsurface_anisotropy | |
| sheen* | fuzz_weight/color/roughness (Zeltner) | sheen/sheen_color/sheen_roughness (Charlie) | MaterialX translation uses fuzz_r = sheen_r^0.4 |
| coat* | coat_weight/color/roughness/roughness_anisotropy/ior (1.6) | coat/coat_color/coat_roughness/coat_anisotropy/coat_IOR (1.5), coat_rotation | |
| coatDarkening | coat_darkening | – (coat_affect_color/roughness instead) | the official translation maps coat_affect_roughness → coat_darkening; do not copy that |
| thinFilm* | weight, thickness µm, ior 1.4 | weight = thickness>0, thickness nm, IOR 1.5 | |
| emission | max(lum,0)·emission_color | emission·emission_color | |
| opacity | geometry_opacity | luminance(opacity) | |
| normals/tangents | geometry_normal, coat_normal, tangent, coat_tangent | normal, coat_normal, tangent (coat tangent = tangent rotated by coat_rotation about coat_normal) | |

### 6.7 M3 vs. deferred

- **M3:**
  - All parameters of both models; every lobe above, including thin film (Airy), anisotropy and rotation, coat in all variants, thin-walled glass and thin-walled subsurface;
  - emission; stochastic opacity (camera and indirect rays plus an Embree occlusion filter for shadow rays);
  - transmission tint, transmission_depth as absorption-only Beer (single-medium path state);
  - energy-compensation tables.
  - **Subsurface fallback:** a Lambert lobe with albedo C. With albedo scaling this gives exactly the spec's `E_multi = (1−E_spec)·C`, and it is the spec's own "Flexibility" recommendation.
- **M8:** `transmission_scatter` and its anisotropy (interior scattering medium). M3 uses σa = μt like Karma.
- **M9:** random-walk subsurface (μt = 1/mfp, α(C,g) from 3.3, as Karma does).
- **Later:** dispersion (needs spectral/hero wavelength; stream `kWavelength` is reserved; M3 warns); nested dielectrics and outside IOR; transparent/fake-caustic shadows; OpenPBR 1.2 (haze, retroreflectivity, decoupled IOR); generic pbrlib graphs (M4); closure-space mix of several surface shaders (M4, kMaxLobes 16).

---

## 7. Pitfalls

1. **Standard Surface version defaults.** 1.0.0 uses base 0.8 / base_color 1; 1.0.1 uses 1 / 0.8. Products are identical except under coat_affect_color. Resolve unauthored inputs from the matching node definition (Sdr/usdMtlx); check that `inherit` resolves.
2. **Untinted specular albedo for layering** in MaterialX and Karma (measured), unlike the Autodesk spec. specular_color does not tint the energy left for lower layers.
3. **Do not use MaterialX's analytic albedo fits** (errors up to 3.5%, numbers in 4.). Karma's dielectric layering uses Schlick-like fits, so expect up to ~8% differences in the layered base at grazing angles (μ<0.2) against Karma.
4. **Karma deviations** to loosen or exclude in reference tests:
   - rough glass not compensated (−13% at grazing);
   - metals at r ≥ 0.7 (−8% at grazing);
   - Standard Surface sheen = albedo-weighted Lambert (Nova's Charlie lobe differs by 30–60% under back light; offer `mode 1` as a compat switch);
   - CPU qualitative Oren-Nayar ≠ MaterialX (XPU matches);
   - OpenPBR thin-walled subsurface (graph C² bug, CPU ≠ XPU);
   - transmission scatter ignored.
5. **EON:** the spec's ρ_ms formula has a typo; use the EON paper code. Clamp ρ ≤ 1 in ρ_ms, because the denominator `1−ρ(1−⟨E⟩)` can reach zero for base_color > 1.
6. **Coat darkening:** MaterialX K uses F0 and the spec uses E_F. At η=1.6, ρ=0.5, μ=1 this is 0.3089 vs 0.2980. Karma uses the MaterialX form.
7. **OpenPBR v1.1.1 refraction:** η' (with specular_weight and the coat TIR-fix ratio) also drives refraction, so specular_weight=0 means no bending. Version 1.2 decouples this; keep `etaF`/`etaT` separate.
8. **Units:**
   - thin film: nm (Standard Surface) vs µm (OpenPBR);
   - IOR defaults: thin film 1.5/1.4, coat 1.5/1.6;
   - lengths (mfp, depth) in scene units, as Karma does;
   - emission nits treated as radiance 1:1.
9. **Opacity:** Standard Surface opacity is a color but the graph uses ACEScg luminance; OpenPBR is scalar. Cutout opacity requires opacity evaluation in shadow rays (Embree occlusion filter; OptiX any-hit later).
10. **Thin film and the layering albedo:** MaterialX dielectric throughput ignores the film (uses F0 from IOR). The soap-bubble preset (IOR 1.0 + film) shows the energy error; Nova uses the Airy albedo approximation.
11. **Sanitising inputs:**
    - the Velvet preset has `specular_IOR=0` (F0=1);
    - clamp α ≥ 1e-4, with α < 1e-4 treated as singular (Chrome preset r=0);
    - OpenPBR anisotropy 1 gives α_b = 0;
    - clamp negative colors to 0; clamp weights to [0,1];
    - `multiply_bsdf` clamps tints to [0,1].
12. **Tangent rotation:** MaterialX `rotate3d` gives `T' = T cosθ + (T×N) sinθ` (`mx_rotate_vector3.glsl`).
    - The Tworld default comes from the texcoord u-derivative; Karma uses `tangentu` if bound, otherwise st, then the primitive tangent.
    - Nova needs tangents from primvars:st with a stable fallback.
    - OpenPBR has no rotation input.
13. **Singular lobes and MIS:** `bsdfSampleWeight` must return 1 after singular samples (sentinel pdf). NEE never evaluates singular lobes.
14. **Inside/outside:** orient η by the authored `Ng·wo`, not by the forward-facing normal; flip the ray offset side for transmission; keep the absorption state (σa) per path.
15. **Non-reciprocal layering** (albedo scaling at ωo): run chi-square and reciprocity tests per lobe core, not on the whole layered list. Bidirectional methods are not supported, as in the Autodesk spec's discussion.
16. **Official translation graphs** (`standard_surface_to_open_pbr.mtlx`) have quirks:
    - SS `transmission_dispersion` (an Abbe number) is fed to OpenPBR `dispersion_scale`;
    - `coat_darkening ← coat_affect_roughness`;
    - coat is zeroed when metalness > 0;
    - `fuzz_roughness = sheen_roughness^0.4`.

    Do not use them as Nova's mapping.

---

## 8. Test plan

### 8.1 Unit tests (`tests/test_bsdf.cpp`, no rendering)

Run per lobe and per parameter set, with r ∈ {0.05, 0.2, 0.5, 1}, anisotropy ∈ {0, 0.5, 0.9}, η ∈ {1.33, 1.5, 2.4, 1/1.5}, μo ∈ {0.05, 0.3, 0.7, 1}:

1. **pdf normalisation:** ∫pdf dω = 1 (VNDF reflection: equals the fraction of samples above the horizon). Monte Carlo over the uniform sphere, tolerance 0.5%.
2. **sample ↔ eval consistency:** `sample()` returns f and pdf equal to `eval(wi)` and `pdf(wi)` within 1e-4 relative.
3. **Chi-square** (pbrt/Mitsuba style): 32×64 bins in (cosθ, φ), integrated pdf vs 1M samples, Pearson with Sidak correction at α=1e-3. Cover every lobe, the R+T dielectric with tints, and the full closure list at fixed ωo (mixture pdf).
4. **Reciprocity** of the uncompensated cores: `f(i,o) = f(o,i)` for EON, QON, Lambert, GGX conductor and dielectric R; Charlie; for dielectric T, `f(i,o)/η_o² = f(o,i)/η_i²`.
5. **Albedo:** MC estimate of `mean(f·|cos|/pdf)` vs the 6.4 tables, the EON analytic albedo, Zeltner R, and the QON numbers. Tolerance 0.3%.
6. **Per-lobe white furnace (E = 1 ± 0.3%):**
   - EON with ρ=1 at any σ;
   - conductor F0=1, tint 1, at any α (F82 and n,k);
   - glass R+T (η, 1/η, any α) with energy compensation;
   - thin glass R'+T' = 1 with T_color = 1;
   - thin-walled subsurface with C=1: R+T = 1.
7. **Layered closure lists, white configurations** (OpenPBR spec "White furnace testing"; Standard Surface equivalents). Total albedo must be 1 ± 0.5% for all μo and roughness values:
   - metal (base 1, specular_color 1); diffuse; subsurface (fallback) white; translucent with transmission_color 1;
   - any mix of these; each with coat (white) and with fuzz/sheen (white);
   - with thin film, anisotropy, normal maps, opacity.
8. **Exact non-white anchors:**
   - glossy-diffuse ρ=0.5, r 0.3, η 1.5: `E = E_R + (1−E_R)·0.5` from the table;
   - OpenPBR coat η 1.6 smooth over ρ 0.5 with specular_weight 0, MaterialX K: 0.3089, 0.3099, 0.3175, 0.3468, 0.4340, 0.6434 at μ = 0.98 … 0.125;
   - Standard Surface specular_color (1,0,0) over white: G = 1 − E_R(μ).

### 8.2 Render tests (husk + nova_cli, `tests/reference` infrastructure)

- **Furnace scenes:** unit sphere mesh in a uniform dome (intensity 1, `renderlightgeo 1`), unlimited depth, Russian roulette on. Every pixel must equal 1 (mean ±0.2%). One scene per white configuration from 8.1.7, plus rough glass r 0.3 (Karma: 0.87–0.997 measured; Nova must reach 1).
- **Albedo probe:** the 5.3 setup with `nova:maxDepth = 1`, compared radially to the analytic E(μo) from 8.1 and to the Karma table in 5.3. The table holds measured values; deviations are explained in 7.3–7.4.
- **Karma comparison grid** (`nova_imgcmp`, Karma CPU reference): one material ball per preset, lit by (a) the white dome, (b) `sky_soft.exr`, (c) a rect key light.

  | Preset | Parameters | Status for M3 |
  |---|---|---|
  | SS Gold | base_color (0.944, 0.776, 0.373), specular_color (0.998, 0.981, 0.751), r 0.02, metal | compare |
  | SS Copper | metal, white base, specular 0, coat 1, coat_color (0.965, 0.376, 0.258), coat r 0.2 | compare |
  | SS Chrome | r 0 | singular path |
  | SS Glass | IOR 1.52, r 0.01, transmission 1, base 0 | compare, smooth |
  | SS Glass tinted | r 0.15, IOR 1.54, transmission_color (0.2, 0.1, 1) | loosen at grazing (no compensation in Karma) |
  | SS Plastic | blue base, r 0.325 | compare |
  | SS Car paint | base 0.5, r 0.4, anisotropy 0.5, coat r 0 | needs tangents |
  | SS Velvet | sheen 1, specular 0, IOR 0 | use sheen compat mode or exclude |
  | SS Thin film | IOR 2.5, film 550 nm, r 0.02 | compare |
  | SS Metal brushed | anisotropy 0.65, specular_color 0 | compare |
  | SS Jade, OpenPBR Ketchup, OpenPBR Pearl | subsurface | exclude until M9 |
  | SS Brass tiled | textures | after the M3 texture work |
  | SS Greysphere | 0.18, r 0.7 | compare |
  | OpenPBR aluminum_brushed | anisotropy 0.9 | compare |
  | OpenPBR carpaint | coat η 1.6 / spec 1.6 | compare |
  | OpenPBR default | defaults | compare |
  | OpenPBR glass | Abbe 64, scale 1 | exclude, dispersion |
  | OpenPBR honey | depth 2 + scatter | absorption only; Karma also ignores scatter |
  | OpenPBR lightbulb | emission 10000 nits | radiance 1:1 |
  | OpenPBR soapbubble | thin-walled, IOR 1, film | compare |
  | OpenPBR velvet | fuzz | compare |

  Tolerances: the global relative tolerance from `manifest.json`. Add a silhouette mask (μ < 0.2) or a looser block tolerance for layered dielectrics (7.3).
- **Regression:** Standard Surface and OpenPBR graphs with the same mapped parameters (6.6). Where the semantics coincide (no coat, no sheen/fuzz, specular_weight 1, roughness 0, white specular), Nova must give identical images for both.

---

I did not change any project files. Probe scenes, scripts and outputs are in `C:/Users/Synti/AppData/Local/Temp/claude/C--Users-Synti-Desktop-renderEngine/ac6add8e-dc52-40d3-838a-501e34988043/scratchpad/m3research/surface-models/`:
- `karma/gen.py` writes the scenes (`*.usda`) and `karma/run.py` renders them; results are in `karma/results_cpu.txt` and `karma/results_xpu.txt` (the later furnace, velvet and thin-walled probes were printed to the console and are quoted in 5.3 and 3.4);
- `cpp/albedo.cpp`, `cpp/coat.cpp`, `cpp/sheen.cpp` are the numeric checks;
- `web/` holds the downloaded spec and reference sources.