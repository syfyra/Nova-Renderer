# Nova M3 research: microfacet and BSDF building blocks for Standard Surface and OpenPBR

I read the current design (`core/integrator/wavefront.{h,cpp}`, `core/bsdf/lambert.h`, `core/math/{sobol,sampling,frame,vec,spectrum}.h`, `core/lights/lights.h`, `core/scene/scene.h`), the MaterialX 1.39.5 libraries and Karma's MaterialX VEX implementation shipped with Houdini 22.0.466, and the reference code of pbrt-v4, Cycles (main), Mitsuba 3, OpenPBR 1.1.1, the EON paper and the bounded-VNDF paper.

All proposed code was compiled and checked numerically with MSVC: pdf normalisation, chi-square tests, albedos and LUT error. Karma's behaviour was measured with one husk render (isolated prefs, `HOUDINI_PACKAGE_SKIPLIST=nova`).

**One rule slip.** A `curl -o` without an absolute path wrote `bsdfs_test.cpp` into the project root. I moved it to the scratch folder right away, so the project files are unchanged. `film.obj` and `renderBuffer.obj` were already in the root; I did not create them.

**Scratch artefacts** (under `C:/Users/Synti/AppData/Local/Temp/claude/C--Users-Synti-Desktop-renderEngine/ac6add8e-dc52-40d3-838a-501e34988043/scratchpad/m3research/microfacet/`):
- `verify/bsdf_sketch.h`: the verified code the snippets below are adapted from.
- `verify/verify.cpp`, `verify2.cpp`, `verify3.cpp`: the numerical tests.
- `scripts/gen_karma_probe.py`, `karma/probe.usda`, `karma/probe.exr`: the Karma probe.
- `dl/`: downloaded reference sources (pbrt, Cycles, Mitsuba, OpenPBR, EON, bounded VNDF, Imageworks sheen and Kulla–Conty slides).

---

## 0. Decisions at a glance

| Topic | Recommendation |
|---|---|
| NDF / masking | Anisotropic GGX. Height-correlated Smith in the stable form `V = 1/(2(t_o|μ_i| + t_i|μ_o|))`, `t(ω) = |(α_x ω_x, α_y ω_y, ω_z)|`. |
| Sampling | Spherical-cap VNDF (Dupuy & Benyoub 2023). For reflection-only lobes use the **bounded** cap (Eto & Tokuyoshi 2023): at α = 0.8, μ = 0.95 the share of valid samples rises from 62 % to 97 %. Dielectric transmission always uses the full cap. |
| Dielectric | Walter 2007 with VNDF. R/T choice by F(ω_o·m). Work in a flipped frame with ω_o.z > 0 and η inverted. In radiance mode the BTDF is divided by η². |
| Fresnel | Exact dielectric, exact conductor (n, k), generalized Schlick, F82-tint (Hoffman / MaterialX form) clamped ≥ 0, Gulbrandsen artistic IOR, OpenPBR `specular_weight` IOR modulation. |
| Multiple scattering | Turquin-style lobe scaling `1 + F_ms (1−E)/E` with F_ms from Kulla–Conty's average-Fresnel series. Dielectric R+T is scaled by `1/E_RT(μ_o, r, η)` (two 32³ LUTs, η > 1 and η < 1). No extra lobe and no pdf change. |
| LUTs | 32 × 32 (r, √μ) and 32³ (+ z = ((η−1)/(η+1))^½), ~420 KB in total. Generated offline by `tools/nova_bsdf_precompute` from the **same** eval/sample code, with Nova's Sobol sampler. |
| Thin film | Belcour & Barla Airy summation evaluated per microfacet (MaterialX `mx_fresnel_airy`). The DC term must stay grey. **Karma has a measured red cast of R/G = 1.27** caused by the linear-sRGB matrix. |
| Sheen | Standard Surface: `sheen_bsdf mode=conty_kulla`, MaterialX form (Imageworks D, no G, Neubelt denominator). OpenPBR fuzz: `mode=zeltner` LTC. **Karma renders conty_kulla as Lambert × albedo fit (measured).** |
| Diffuse | OpenPBR: EON with CLTC + uniform sampling (26× lower variance at grazing). Standard Surface: QON in the MaterialX form (Karma measured equal). Also Burley, translucent. |
| Layering | A MaterialX BSDF value = (closure list, throughput T(ω_o)). `layer` multiplies the base weights by `T_top(ω_o)`. Weights are constants per hit, so the result is a flat weighted closure list. |
| Uber sampling | One-sample MIS: pick lobe j with p_j ∝ lum(w_j·Ê_j(ω_o)) (ω_o-only, with a floor); pdf = Σ p_k pdf_k; f = Σ w_k f_k. Lobes return **f·\|cos\|**, because coat and base normals differ. |
| Robustness | Cycles' `ensure_valid_specular_reflection` normal bend (Karma does the same with `_mx_normal_clamp`). Reflect/transmit classification by N_g in sampling. Delta lobes below α = 1e-3 with an explicit delta flag for MIS. NaN guards. |

---

## 1. Ground truth in this install

**Versions**
- MaterialX 1.39.5 (`$HFS/toolkit/include/MaterialXCore/Generated.h:9-11`).
- OIIO 2.5.18.
- MaterialX data libraries: `$HFS/houdini/materialx/libraries/{stdlib,pbrlib,bxdf}`. Further copies are in `$HFS/bin/usd_plugins/usdMtlx/resources/libraries` and `$HFS/python313/lib/site-packages/MaterialX/...`.
- `open_pbr_surface.mtlx` declares OpenPBR v1.1.1 (`ND_open_pbr_surface_surfaceshader`, line 6). `standard_surface.mtlx` declares 1.0.1.

**MaterialX BSDF reference code** (`pbrlib/genglsl/lib/`)

`mx_microfacet_specular.glsl`:

| Lines | Content |
|---|---|
| 34 | `mx_ggx_NDF` |
| 42-62 | spherical-cap VNDF |
| 65 | reflection pdf |
| 72 | `G1` |
| 82 | `G2` (height-correlated) |
| 91-108 | analytic albedo fit |
| 178-182 | Turquin compensation `1 + Fss(1−Ess)/Ess` |
| 214-222 | Hoffman F82 |
| 225-238 | exact dielectric Fresnel |
| 241-283 | polarized dielectric and conductor Fresnel |
| 286-412 | thin film (Airy) |
| 486-512 | albedo per Fresnel model |

Other files:
- `mx_microfacet_sheen.glsl`: Imageworks sheen 5-25, its fit 28-37, Zeltner 97-187.
- `mx_microfacet_diffuse.glsl`: QON 8-18, EON (named "compensated") 87-136, Burley 140-156.
- BSDF nodes: `mx_dielectric_bsdf.glsl`, `mx_conductor_bsdf.glsl`, `mx_generalized_schlick_bsdf.glsl`, `mx_sheen_bsdf.glsl`, `mx_oren_nayar_diffuse_bsdf.glsl`.
- Layering semantics: `mx_layer_bsdf.glsl` (`response = top + base·top.throughput; throughput = top.T·base.T`), `mx_add_bsdf.glsl` (`T = max(T1+T2−1, 0)`), `mx_mix_bsdf.glsl` (lerp), `mx_multiply_bsdf_*.glsl` (scales the response by `clamp(w, 0, 1)`; **throughput unchanged**).

**Graphs**
- `pbrlib_defs.mtlx:138`: `sheen_bsdf` has `mode` default `conty_kulla`.
- `standard_surface.mtlx:259`: Standard Surface leaves `mode` at the default, so it uses conty_kulla.
- `open_pbr_surface.mtlx:555-561`: OpenPBR fuzz uses `mode="zeltner"`.
- `open_pbr_surface.mtlx:197-203`: OpenPBR diffuse is `oren_nayar_diffuse_bsdf energy_compensation=true` (EON).
- `standard_surface.mtlx:222`: Standard Surface diffuse uses `false` (QON).
- `open_pbr_surface.mtlx:428`: OpenPBR metal is `generalized_schlick_bsdf` with `color82 = specular_color` (F82-tint).
- `standard_surface.mtlx:314-318`: Standard Surface metal is `artistic_ior` → `conductor_bsdf` (exact n, k).

**Karma's implementation** (`$HFS/houdini/vex/include/mxpbr.h`, `ggx_utils.h`, `mxtypes.h`)

| Function | Behaviour |
|---|---|
| `mxpbr_dielectric` (mxpbr.h:605) | R lobe = `ggx(F0=F90=specular_color, fresblend=0)`, i.e. **a white GGX lobe without per-microfacet Fresnel**, times `1/ggx_albedo_curve_fit(μ, α, 1, 1)`, mixed as `mix_bsdf(f, base, 1 − alb)` with `alb = ggx_albedo_curve_fit_ior` (ggx_utils.h:91). |
| Backface | R is skipped, `f = base`. |
| `mxpbr_conductor` (783) | `1/alb11` normalisation (full white compensation). |
| `mxpbr_sheen` (994) | conty_kulla: `mix_bsdf(base, clr·2·diffuse(), w·albedo_fit)` with a modified fit at r < 0.02 (290-309). Zeltner: VEX `sheen()` × fit (984). |
| `_mx_normal_clamp` (536) | Bends N so that reflect(I, N) lies ≥ 0.01 above the N_g plane (not applied on backfaces). |
| `_mx_fresnel_airy` (433) | Evaluated at the **macro** angle, 2 iterations, `xyztorgb` (linear sRGB). |
| `_mx_sss_parameterization` (255) | OpenPBR 2025 random-walk albedo inversion (relevant for M9). |

**Karma probe** (`scripts/gen_karma_probe.py` → `karma/probe.usda`, Karma CPU 256 spp)

Each quad is viewed head-on (μ = 1, orthographic camera) under a uniform white dome, so the pixel value equals E(μ=1). Per-pixel σ is ~1e-4; the means are over 256 px.

| Quad | Karma | Explanation (computed) |
|---|---|---|
| SS spec, IOR 1.5, r 0.3 (α 0.09); OpenPBR spec same | 0.041588 | Exactly `alb_fit_ior · E_ss11 / fitE11` = 0.041588. The physical value (exact microfacet Fresnel) is 0.03971, so Karma is +4.7 %. |
| SS spec + thin film 3000 nm, film IOR 1.33 | (0.02808, 0.02210, 0.02118) | R/G = 1.270 and B/G = 0.958. Predicted from `M_sRGB·(1,1,1)_XYZ` = (1.2054, 0.9483, 0.9100): R/G 1.2711, B/G 0.960. Grey level 0.0233 = incoherent film reflectance. |
| SS sheen r 0.3 / r 0.8 | 0.051978 / 0.134954 | Equal to the MaterialX fit (0.0520 / 0.13494). The true integral of the MaterialX lobe is 0.0430 at r 0.3. |
| SS metal, white, r 0.8 | 0.98553 | artistic IOR clamps r to 0.99; compensated by 1/fitE11. |
| OpenPBR metal, white, r 0.8 | 0.99527 | F = 1; residual is Karma's fit error. |
| SS diffuse, σ = 1 | 0.624124 | QON A(1) = 1 − 0.5/1.33 = 0.624060, so Karma's Standard Surface diffuse is QON. |
| OpenPBR diffuse, σ = 1 | 0.999926 | EON (passes the furnace). |
| OpenPBR fuzz r 0.5 | 0.024870 | Zeltner fit R(1, 0.5) = 0.0249, so VEX `sheen()` is normalised to albedo 1. |

---

## 2. (a) GGX: distribution, masking, VNDF sampling, pdfs, rough dielectric

**Conventions.** Local frame with +Z = shading normal. ω_o = view, ω_i = light, both pointing away from the surface. μ = cos θ. α = r² (Standard Surface and OpenPBR).

**Anisotropy mappings differ between the two models:**
- MaterialX `mx_roughness_anisotropy` (Standard Surface): `r2 = clamp(r², ε, 1)`; for a > 0, `aspect = sqrt(1 − min(a, 0.98))`, `α_x = min(r2/aspect, 1)`, `α_y = r2·aspect`.
- OpenPBR (spec §Microfacet model; graph lines 609-646): `α_t = r²·sqrt(2/(1+(1−a)²))`, `α_b = (1−a)·α_t`.

**Formulas** (Walter et al. 2007; Heitz 2014 JCGT 3(2) eq. 72/99; pbrt-v4 `util/scattering.h:109-210`)

- D(m) = 1 / (π α_x α_y (m_x²/α_x² + m_y²/α_y² + m_z²)²) for m_z > 0.
- t(ω) = sqrt(α_x²ω_x² + α_y²ω_y² + ω_z²).
- Λ(ω) = (t − |ω_z|) / (2|ω_z|).
- G1 = 2|ω_z| / (t + |ω_z|). This avoids tan θ and the 1 − cos² cancellation.
- Height-correlated G2 = 1/(1 + Λ_o + Λ_i), hence G2/(4|μ_o||μ_i|) = **1 / (2(t_o|μ_i| + t_i|μ_o|))**.
- Reflection BRDF: f_r = F(ω_o·m)·D(m)·V.
- VNDF: D_ωo(m) = G1(ω_o)·max(0, ω_o·m)·D(m) / μ_o.
- Reflection pdf: p(ω_i) = D_ωo(m)/(4 ω_o·m) = **D(m) / (2(t_o + μ_o))**.
- Bounded cap: p = **D(m) / (2(k μ_o + t_o))** with k = (1−a²)s² / (s² + a²μ_o²), a = min(α_x, α_y, 1), s = 1 + |ω_o.xy|. This is Eto & Tokuyoshi 2023, Listing 2. For μ_o < 0 their stable form is `D·(t−μ_o)/(2|αω_o.xy|²)`, which is identical to Tokuyoshi 2021's unbiased normalisation for back-facing shading normals.
- Spherical-cap sampling (Dupuy & Benyoub 2023; MaterialX `mx_ggx_importance_sample_VNDF`, lines 42-62):
  - ω_h = normalize(α_x ω_x, α_y ω_y, ω_z), φ = 2πu₁, z = (1−u₂)(1+b) − b with b = ω_h.z (bounded: k·ω_h.z).
  - c = (√(1−z²) cos φ, √(1−z²) sin φ, z), h = c + ω_h, m = normalize(α_x h_x, α_y h_y, max(h_z, 0)).
  - This produces the same distribution as Heitz 2018 (Cycles `microfacet_ggx_sample_vndf`, `bsdf_microfacet.h:304`; pbrt `Sample_wm`, `scattering.h:163`) but needs no basis construction.

**Rough dielectric** (Walter 2007; pbrt `bxdfs.cpp:77-262`)
- Let η = n_inside/n_outside. If μ_o < 0, flip z of both directions and use 1/η, so the lobe always sees μ_o > 0.
- Generalized half vector: m = normalize(ω_o + η′ω_i), η′ = 1 for reflection and η for transmission, then face-forwarded to +z.
- Reject back-facing microfacets: (ω_o·m) ≤ 0 or (ω_i·m)·μ_i ≤ 0.
- f_t = (1−F)·D·G2·|ω_i·m||ω_o·m| / (|μ_i||μ_o|(ω_i·m + ω_o·m/η)²) · **1/η²** (radiance transport).
- p_t = D_ωo(m)·|ω_i·m| / (ω_i·m + ω_o·m/η)² · P_T.
- Refraction: ω_i = −ω_o/η + (cos_o/η − cos_t)·m with cos_t = √(1 − (1−cos_o²)/η²). TIR (only when exiting) gives F = 1.
- R/T choice: reflect with probability F (or tinted F for R-only tint), else transmit.

Verified: chi² p = 0.68 / 0.79 / 0.99 / 0.87 for η ∈ {1.5, 1/1.5} × α ∈ {0.1, 0.5}. Generalised reciprocity holds: `f(ω_o,ω_i)/f(ω_i,ω_o) = (n_o/n_i)² = 0.4444`.

**Single-scatter loss of R+T** (MC, verify.cpp) shows why compensation is essential:

| Case | μ | E_R + E_T |
|---|---|---|
| η = 1.5, α = 1 | 1 | 0.893 |
| η = 1.5, α = 1 | 0.2 | 0.704 |
| η = 1/1.5, α = 1 | 1 | **0.418** |
| η = 1/1.5, α = 0.3 | 1 | 0.900 |

**Thin-walled** (OpenPBR §Thin-walled; Cycles `bsdf_microfacet.h:1373-1550`)
- With front Fresnel R, T and normal-incidence absorption colour c (Beer: c_θ = c^(1/cos_t)):
  - T′ = c_θ·T² / (1 − (R·c_θ)²)
  - R′ = R + T′·R·c_θ
- Reflection lobe: GGX with constant colour R′ (`fresnel_constant`).
- Transmission lobe: the *mirrored* reflection lobe (ω_i reflected through the plane), no bending and no IOR change, with roughened α_t = saturate(α·√(3.4(η−1)(η−0.5)²/η³)) (Kulla–Conty 2017 p. 40; Cycles notes that "3.7" in the slides is a typo).
- At α_t ≈ 0 the transmission is a pass-through delta.

```cpp
// core/bsdf/microfacet.h   (adapted from verify/bsdf_sketch.h, all checks passed)
struct GgxDistribution {
    float ax, ay;  // clamp both to >= 1e-4 when the lobe is not delta (pbrt scattering.h:116-122)

    NOVA_INLINE float D(Vec3f m) const {
        if (m.z <= 0.0f) return 0.0f;
        const float x = m.x / ax, y = m.y / ay, d = x * x + y * y + m.z * m.z;
        return 1.0f / (kPi * ax * ay * d * d);
    }
    NOVA_INLINE float t(Vec3f w) const { return std::sqrt(sqr(ax * w.x) + sqr(ay * w.y) + sqr(w.z)); }
    NOVA_INLINE float G1(Vec3f w) const { const float z = std::fabs(w.z); return 2.0f * z / (t(w) + z); }
    NOVA_INLINE float vis(Vec3f wo, Vec3f wi) const {   // G2 / (4 |mu_o| |mu_i|)
        return 0.5f / (t(wo) * std::fabs(wi.z) + t(wi) * std::fabs(wo.z));
    }
    NOVA_INLINE float G2(Vec3f wo, Vec3f wi) const { return 4.0f * std::fabs(wo.z) * std::fabs(wi.z) * vis(wo, wi); }
    NOVA_INLINE float capK(Vec3f wo, bool bounded) const {          // Eto & Tokuyoshi 2023 eq. 5/6
        if (!bounded || wo.z <= 0.0f) return 1.0f;
        const float a = saturate(std::fmin(ax, ay)), s = 1.0f + std::sqrt(sqr(wo.x) + sqr(wo.y));
        const float a2 = a * a, s2 = s * s;
        return (1.0f - a2) * s2 / (s2 + a2 * wo.z * wo.z);
    }
    NOVA_INLINE Vec3f sampleVisible(Vec3f wo, Vec2f u, bool bounded) const {   // Dupuy & Benyoub 2023
        const Vec3f wh = normalize(Vec3f{ax * wo.x, ay * wo.y, wo.z});
        const float b = capK(wo, bounded) * wh.z, phi = kTwoPi * u.x;
        const float z = (1.0f - u.y) * (1.0f + b) - b, s = std::sqrt(saturate(1.0f - z * z));
        const Vec3f h = Vec3f{s * std::cos(phi), s * std::sin(phi), z} + wh;
        return normalize(Vec3f{ax * h.x, ay * h.y, std::fmax(h.z, 0.0f)});
    }
    NOVA_INLINE float pdfVisible(Vec3f wo, Vec3f m) const {
        const float om = dot(wo, m);
        return om > 0.0f ? G1(wo) * om * D(m) / wo.z : 0.0f;
    }
    NOVA_INLINE float pdfReflect(Vec3f wo, Vec3f wi, bool bounded) const {   // solid angle of wi
        return D(normalize(wo + wi)) / (2.0f * (capK(wo, bounded) * wo.z + t(wo)));
    }
};
```

---

## 3. (b) Fresnel

```cpp
// core/bsdf/fresnel.h
// Exact dielectric; cosI >= 0 on wo's side, eta = n_t/n_i; returns 1 on TIR and writes cosT.
NOVA_INLINE float fresnelDielectric(float cosI, float eta, float &cosT) {
    const float s2t = (1.0f - cosI * cosI) / (eta * eta);
    if (s2t >= 1.0f) { cosT = 0.0f; return 1.0f; }
    cosT = std::sqrt(1.0f - s2t);
    const float rs = (cosI - eta * cosT) / (cosI + eta * cosT), rp = (eta * cosI - cosT) / (eta * cosI + cosT);
    return 0.5f * (rs * rs + rp * rp);
}
// Exact conductor per channel (Lagarde; MaterialX mx_microfacet_specular.glsl:259-283)
NOVA_INLINE float fresnelConductor(float c, float n, float k) {
    const float c2 = c * c, s2 = 1.0f - c2, n2 = n * n, k2 = k * k, t0 = n2 - k2 - s2;
    const float a2b2 = std::sqrt(t0 * t0 + 4.0f * n2 * k2), t1 = a2b2 + c2;
    const float a = std::sqrt(std::fmax(0.5f * (a2b2 + t0), 0.0f)), t2 = 2.0f * a * c;
    const float Rs = (t1 - t2) / (t1 + t2), t3 = c2 * a2b2 + s2 * s2, t4 = t2 * s2;
    return 0.5f * (Rs + Rs * (t3 - t4) / (t3 + t4));
}
// Generalized Schlick + F82 tint (Hoffman 2023 "Generalization of Adobe's Fresnel model";
// MaterialX mx_fresnel_hoffman_schlick :214; OpenPBR metal for p = 5, F90 = 1). Per channel.
NOVA_INLINE float fresnelF82Tint(float mu, float F0, float F82tint, float F90, float p) {
    constexpr float kMuBar = 1.0f / 7.0f, kDen = 0.0566416f;     // muBar (1 - muBar)^6
    const float x = saturate(mu);
    const float a = lerp(F0, F90, std::pow(1.0f - kMuBar, p)) * (1.0f - F82tint) / kDen;
    return std::fmax(lerp(F0, F90, std::pow(1.0f - x, p)) - a * x * pow6(1.0f - x), 0.0f);  // clamp REQUIRED
}
NOVA_INLINE float fresnelF82TintAvg(float F0, float F82tint, float F90, float p) {   // 2 int F mu dmu
    constexpr float kMuBar = 1.0f / 7.0f, kDen = 0.0566416f;
    const float a = lerp(F0, F90, std::pow(1.0f - kMuBar, p)) * (1.0f - F82tint) / kDen;
    return std::fmax(lerp(F0, F90, 2.0f / ((p + 1.0f) * (p + 2.0f))) - a / 126.0f, 0.0f);
}
// Gulbrandsen 2014 artistic IOR (MaterialX mx_artistic_ior; Standard Surface metal)
NOVA_INLINE void artisticIor(float refl, float edge, float &n, float &k) {
    const float r = std::fmin(std::fmax(refl, 0.0f), 0.99f), sr = std::sqrt(r);
    n = lerp((1.0f + sr) / (1.0f - sr), (1.0f - r) / (1.0f + r), edge);
    k = std::sqrt(std::fmax((sqr(n + 1.0f) * r - sqr(n - 1.0f)) / (1.0f - r), 0.0f));
}
// OpenPBR specular_weight IOR modulation (spec eq. modulated_ior; graph lines 335-371)
NOVA_INLINE float openPbrModulatedEta(float etaS, float specWeight) {
    const float Fs = sqr((etaS - 1.0f) / (etaS + 1.0f));
    const float eps = std::copysign(std::sqrt(std::fmin(std::fmax(specWeight * Fs, 0.0f), 0.99999f)), etaS - 1.0f);
    return (1.0f + eps) / (1.0f - eps);
}
```

Further formulas:
- **Generalized Schlick:** F = lerp(F0, F90, (1−μ)^p). Its hemispherical average is lerp(F0, F90, 2/((p+1)(p+2))).
- **Average conductor Fresnel without quadrature** (Cycles `bsdf_util.h:275-281`): fit F82 through F0 = F_c(1) and F82 = F_c(1/7), with b = 7/(6/7)⁶·(Schlick(1/7) − F82), then F_avg = lerp(F0, 1, 1/21) − b/126.
- **Average dielectric Fresnel:** the Kulla–Conty fit `(η−1)/(4.08567+1.00071η)` (η > 1) and `0.997118+0.1014η−0.965241η²−0.130607η³` (η < 1) is **inaccurate near η = 1**. Measured: η = 1.1 → 0.0193 vs exact 0.0252 (−23 %); η = 1.33 → −8 %. Use a 64-entry 1D LUT on z = ((η−1)/(η+1))^½ instead.

**F82 pitfall (measured).** With `F82tint = 0` the raw curve goes negative (min −0.127 at F0 = 0), and so does the averaged formula (−0.017). Clamp both: Cycles saturates `fresnel_f82` (`bsdf_util.h:178`); MaterialX GLSL does not. Checked exactly: `F(1/7) = tint·Schlick(1/7)`, and the average formula matches quadrature to 4 digits.

**OpenPBR conventions:**
- `specular_color` tints only the first reflection from above (R lobe). MaterialX uses it as `dielectric_bsdf tint`; T and the throughput are untinted.
- With a coat present, η_s = lerp(n_b, n_b/n_c, C). Where n_c > n_b the ratio is inverted (TIR fix; graph lines 330-343, spec §Total internal reflection).

---

## 4. (c) Multiple-scattering energy compensation and LUTs

**Kulla & Conty 2017** (slides 13-15, 29-32; reciprocal lobe):
- f_ms = (1−E(μ_o))(1−E(μ_i)) / (π(1−E_avg)) · F_avg²E_avg / (1 − F_avg(1−E_avg)).
- E_avg = 2∫E(μ)μ dμ, F_avg = 2∫F(μ)μ dμ.
- The slide's first version had F_avg instead of F_avg²; Turquin's errata (the extra F_avg removes the double-counted k = 0 term) is noted on the slide.
- For glass the transmission split needs x·(1−F_avg(η))/(1−E_avg(1/η))... matched with η² (slide 32).

**Turquin 2019** (as used by MaterialX `:178` and Cycles `microfacet_ggx_preserve_energy`, `bsdf_microfacet.h:544-600`):
- f = f_ss·(1 + F_ms·(1 − E(μ_o))/E(μ_o)).
- Cycles uses F_ms = F_avg·E_avg / (1 − F_avg(1−E_avg)): one F_avg is already inside f_ss.
- MaterialX uses F(VdotH) in place of F_ms; Karma uses F_ms = 1 (plain 1/E).

**Recommendation.**
- Conductors, F82 and generalized Schlick: f = f_ss(F)·(1 + F_ms(1−E)/E) with Cycles' F_ms, F_avg from §3, E and E_avg from the LUT. Same pdf, no extra lobe; white metal gives exactly 1.
- Dielectrics (R+T): f ← f/E_RT(μ_o, r, η), with separate tables for η > 1 and for 1/η (exiting). Since R+T is lossless, white glass gives 1.
- Dielectric R-only (coat, specular over diffuse): scale R by the **same** 1/E_RT and use **E_R^comp = E_R/E_RT** as the layering albedo. Then `E_R^comp + (1 − E_R^comp)·1 = 1` exactly, and the R/T split stays physically correct (§8).
- K-C reciprocal lobes are only worth adding for a later bidirectional integrator.

**LUT accuracy (verify3.cpp, F = 1, axes x = r = √α, y = √μ, bilinear):**

| Option | Max abs error | Mean |
|---|---|---|
| 32 × 32 LUT | **0.0020** | 0.0002 |
| 16 × 16 LUT | 0.0088 | 0.0007 |
| MaterialX analytic fit (`:91`), F = 1 | 0.039 | |
| MaterialX analytic fit, F0 = 0.04 | 0.010 | |

So the fit would fail a 1 % white furnace; use LUTs.

**Table set** (all float32; one blob uploaded once). Axes: r = sqrt(sqrt(α_x α_y)) (Cycles `alpha_to_roughness_index`, `bsdf_microfacet.h:142`), y = √μ, z = sqrt((η−1)/(η+1)), which covers η ∈ [1, ∞) with resolution where it matters.

| Table | Dims | Content | Samples/cell |
|---|---|---|---|
| `ggxE` | 32 r × 32 y | E_ss, F = 1 | 2²² |
| `ggxEavg` | 32 r | 2∫E μ dμ | 2²⁴ (u_μ random) |
| `ggxSchlickABC` | 32 × 32 × 3 | A = ∫f₁cos·(1−s), B = ∫f₁cos·s, C = ∫f₁cos·μ_h(1−μ_h)⁶ with s = (1−μ_h)⁵, giving E_F82 = F0·A + F90·B − b·C exactly | 2²² |
| `dielE_RT` | 32 z × 32 y × 32 r | E_R + E_T, η > 1 (energy units, i.e. without 1/η²) | 2²⁰ |
| `dielE_RT_inv` | same | for 1/η | 2²⁰ |
| `dielE_R` | same | E_R single scatter (layering E_R/E_RT) | 2²⁰ |
| `fresnelAvgDiel` | 64 z | exact 2∫Fμ dμ | quadrature |
| `charlieE` (opt.) | 32 × 32 | albedo of the MaterialX Charlie form | 2²⁰ |

Total ≈ 420 KB. Cycles uses the same sizes and axes (`scene/shader.tables`: ggx_E[1024], glass_E[32768]...). The generator is §12.

Reference values for tests (MC, ≤ 0.002 noise):

| Quantity | Value |
|---|---|
| E(μ=1, α=1) | 0.307 |
| E(0.5, 0.5) | 0.698 |
| E(0.05, 0.5) | 0.913 |
| E_RT(η=1.5, α=0.3, μ=1) | 0.989 |
| E_RT(1/1.5, 1, 1) | 0.418 |
| E_R(1.5, α=0.05, μ=1) | 0.0403 |

```cpp
// core/bsdf/energy.h (sketch)
struct BsdfTables {               // POD of device pointers; never reference host statics from kernels
    const float *ggxE, *ggxEavg, *ggxABC, *dielRT, *dielRTinv, *dielR, *fAvgDiel, *charlieE;
};
NOVA_INLINE float lut2(const float *t, int nx, int ny, float x, float y) { /* clamp + bilinear */ }
NOVA_INLINE float lut3(const float *t, int nx, int ny, int nz, float x, float y, float z) { /* trilinear */ }
NOVA_INLINE float roughIndex(float ax, float ay) { return std::sqrt(std::sqrt(ax * ay)); }
NOVA_INLINE float iorIndex(float eta) { return std::sqrt(std::fabs((eta - 1.0f) / (eta + 1.0f))); }
// Turquin x Kulla-Conty: multiply f_ss(F) by this
NOVA_INLINE Spectrum msCompensation(float E, float Eavg, Spectrum Favg) {
    const Spectrum Fms = Favg * Eavg / (spectrum(1.0f) - Favg * (1.0f - Eavg));
    return spectrum(1.0f) + Fms * ((1.0f - E) / std::fmax(E, 1e-4f));
}
```

---

## 5. (d) Thin-film interference (Belcour & Barla 2017)

Algorithm as in MaterialX `mx_fresnel_airy` (`mx_microfacet_specular.glsl:315-412`) and Karma `_mx_fresnel_airy` (`mxpbr.h:433`); outside IOR 1:
- η₂ = max(film_ior, 1); cos θ_t = √(1 − (1−cos²θ)/η₂²).
- R12 polarized (1 → η₂); T121 = 1 − R12.
- R23 per channel: conductor-polarized with (η₃/η₂, κ₃/η₂). For Schlick/F82 bases, R23 = ½·F_Hoffman(cos θ_t) for both polarizations and η₃ = IOR from F0.
- Phases:
  - φ21 = (θ < θ_Brewster ? 0 : π, π).
  - φ23: from the conductor phase formula (`:286-298`); for Schlick bases π where η₃ < η₂, else 0.
- OPD = 2η₂ cos θ_t·d (d in m; Standard Surface thickness in nm, OpenPBR in μm ×1000 in the graph).
- Per polarization:
  - I += R12 + Rs with Rs = T121²R23 / (1 − R12R23).
  - C₀ = Rs − T121.
  - For m = 1..M: C_m = C_{m−1}·√(R12R23), I += 2C_m·S(m·OPD, m(φ23 + φ21)).
- Average the two polarizations, then convert XYZ → RGB and clamp to [0, 1].
- S is a sum of Gaussians in Fourier space (`mx_eval_sensitivity`, `:301-311`), normalised so that S(0) = (1, 1, 1)_XYZ (verified: 1.0005, 1.0002, 1.0011).
- M: MaterialX uses `AIRY_FRESNEL_ITERATIONS`, Karma 2, Cycles 3.

**Colour-space issue (measured).** Belcour's normalisation is equal-energy white, and MaterialX GLSL uses CIE-RGB(E) (`XYZ_TO_RGB` at `:318`), which maps (1,1,1) to (1,1,1). Karma's `xyztorgb` is linear sRGB (D65 matrix), which maps the grey DC term to (1.205, 0.948, 0.910). The probe shows exactly this cast (R/G 1.270).

Recommendation for Nova:
- Keep the DC term grey: convert with the working-space matrix and normalise per channel by `M·(1,1,1)`.
- Better and cheaper: a Cycles-style FFT CMF table of 512 × 6 (`shader.cpp:957-1016`, `bsdf_util.h:456-498`) built at scene load for the working colour space.
- Evaluate per microfacet (VdotH), as MaterialX does. Karma uses the macro angle, which differs only at high roughness.
- Transmission uses 1 − R_airy (OpenPBR requires fringes in T).
- Albedo of a film-coated R lobe for layering: MaterialX `mx_ggx_dir_albedo(fd.airy)` (`:488-497`), i.e. `lerp(F_airy(μ_o), E(F0_airy, 1), √α)`. It cannot be tabulated (four extra dimensions).
- For Karma reference scenes with thin film, expect +27 % R / −4 % B differences, or add a `nova:compat:karmaThinFilm` switch.

---

## 6. (e) Sheen and fuzz

**Imageworks "Charlie"** (Estevez & Kulla 2017, slides `s2017_pbs_imageworks_sheen.pdf`)
- D(m) = (2 + 1/r)·sin^(1/r)θ_m / (2π).
- Λ(μ) = e^{L(μ)} for μ < 0.5, else e^{2L(0.5) − L(1−μ)}, with L(x) = a/(1 + b x^c) + d x + e.
- Coefficients P = (1−r)²·P₀ + (1 − (1−r)²)·P₁:
  - r = 0: (25.3245, 3.32435, 0.16801, −1.27393, −4.85967)
  - r = 1: (21.5473, 3.82987, 0.19823, −1.97760, −4.32054)
- G = 1/(1 + Λ_o + Λ_i), with terminator softening Λ̃_i = Λ_i^{1 + 2(1−μ_i)⁸} (non-reciprocal).
- f = D·G / (4μ_oμ_i). Sampling: uniform hemisphere, as the authors recommend.

**What MaterialX `sheen_bsdf` conty_kulla actually is** (`mx_microfacet_sheen.glsl:5-25`):
- f = D / (4(μ_i + μ_o − μ_iμ_o)): **no G**, Neubelt–Pettineo denominator, D clamped at r ≥ 0.005.
- Albedo: rational fit (`:28-37`).
- The forms differ substantially (quadrature):

| r, μ | E (MaterialX form) | E (original form with G) |
|---|---|---|
| 1.0, 0.1 | 0.494 | 0.721 |
| 0.1, 0.1 | 0.781 | 0.540 |

- The fit approximates the **MaterialX form** with ≤ 0.036 abs error (r 0.1, μ 0.1); at μ = 1, r = 0.1 it gives 0.0105 vs a true 0.0022.
- Nova: implement the MaterialX form. Use `charlieE` (LUT, exact furnace) or the fit (Karma/MaterialX parity) for layering and selection. Never combine the original-G lobe with the MaterialX fit.
- Karma renders this lobe as Lambert × fit (measured), so only uniform-lighting energy matches Karma; directional highlights differ.

**Zeltner, Burley & Chiang 2022 LTC sheen** (OpenPBR fuzz)
- μ_i·f = F_color·R(μ_o, r)·D_LTC(ω_i).
- LTC frame: X = projection of ω_o.
- D_LTC = (o_z/π)·(a⁻¹/|o|²)² with o = (a⁻¹w_x + b⁻¹w_z, a⁻¹w_y, w_z).
- Sampling: cosine sample c → w = normalize((c_x − b⁻¹c_z)/a⁻¹, c_y/a⁻¹, c_z); **pdf = D_LTC**, so the throughput weight is exactly R·F_color.
- Fits a⁻¹, b⁻¹ and R: MaterialX `:97-114` (identical in Karma `mxpbr.h:984`). Cycles uses a 3 × 32 × 32 table (`bsdf_sheen.h`).
- Clamp r ∈ [0.01, 1]; **clamp R ≥ 0** (the fit returns −0.0000 at r 0.1, μ ≥ 0.5); skip the lobe when |a⁻¹| < 1e-5 or R < 1e-5 (Cycles).
- Layering does not tint the base (OpenPBR): base × (1 − F·R).
- Verified: pdf integral = 1.0000 at every tested (r, μ); chi² p = 0.70 / 0.83.

```cpp
// core/bsdf/sheen.h (Zeltner part; constants from mx_microfacet_sheen.glsl:97-114)
NOVA_INLINE void ltcBasis(Vec3f wo, Vec3f &X, Vec3f &Y) {
    const float l2 = wo.x * wo.x + wo.y * wo.y;
    X = l2 > 0.0f ? Vec3f{wo.x, wo.y, 0.0f} * (1.0f / std::sqrt(l2)) : Vec3f{1.0f, 0.0f, 0.0f};
    Y = {-X.y, X.x, 0.0f};
}
NOVA_INLINE float zeltnerAInv(float x, float y) { return (2.58126f * x + 0.813703f * y) * y / (1.0f + 0.310327f * x * x + 2.60994f * x * y); }
NOVA_INLINE float zeltnerBInv(float x, float y) { return std::sqrt(1.0f - x) * (y - 1.0f) * y * y * y / (0.0000254053f + 1.71228f * x - 1.71506f * x * y + 1.34174f * y * y); }
NOVA_INLINE float zeltnerAlbedo(float x, float y) {
    const float s = y * (0.0206607f + 1.58491f * y) / (0.0379424f + y * (1.32227f + y));
    const float m = y * (-0.193854f + y * (-1.14885f + y * (1.7932f - 0.95943f * y * y))) / (0.046391f + y);
    const float o = y * (0.000654023f + (-0.0207818f + 0.119681f * y) * y) / (1.26264f + y * (-1.92021f + y));
    return std::fmax(std::exp(-0.5f * sqr((x - m) / s)) / (s * std::sqrt(kTwoPi)) + o, 0.0f);
}
NOVA_INLINE float zeltnerPdf(Vec3f wo, Vec3f wi, float r) {          // == f*cos / albedo
    Vec3f X, Y; ltcBasis(wo, X, Y);
    const Vec3f w{dot(wi, X), dot(wi, Y), wi.z};
    const float aI = zeltnerAInv(wo.z, r), bI = zeltnerBInv(wo.z, r);
    const Vec3f o{aI * w.x + bI * w.z, aI * w.y, w.z};
    const float l2 = dot(o, o);
    return std::fmax(o.z, 0.0f) * kInvPi * sqr(aI / l2);
}
NOVA_INLINE Vec3f zeltnerSample(Vec3f wo, float r, Vec2f u) {
    const Vec3f c = sampleCosineHemisphere(u);
    const float aI = zeltnerAInv(wo.z, r), bI = zeltnerBInv(wo.z, r);
    const Vec3f w = normalize(Vec3f{(c.x - c.z * bI) / aI, c.y / aI, c.z});
    Vec3f X, Y; ltcBasis(wo, X, Y);
    return X * w.x + Y * w.y + Vec3f{0.0f, 0.0f, w.z};
}
```

---

## 7. (f) Diffuse

- **Lambert:** f = ρ/π, cosine sampling. **Translucent:** Lambert in the −N hemisphere.
- **QON** (MaterialX `mx_microfacet_diffuse.glsl:8-18`; Karma measured identical):
  - f = ρ/π·(A + B·max(0, s)/max(μ_i, μ_o)), s = ω_i·ω_o − μ_iμ_o.
  - A = 1 − 0.5σ²/(σ²+0.33), B = 0.45σ²/(σ²+0.09), with **σ = diffuse_roughness directly**.
  - Albedo fit: `:21-28`. Cosine sampling.
- **EON** (Portsmouth, Kutz & Hill 2024, arXiv 2410.18026, Listings 1-4; MaterialX `:87-136`; Cycles `bsdf_oren_nayar.h`):
  - f = f_FON + f_ms.
  - f_FON = ρ/π·A_F·(1 + r·s/t) with s/t = s/max(μ_i, μ_o) for s > 0, else s.
  - A_F = 1/(1 + (½ − 2/(3π))r).
  - Ê_F(μ) = A_F + (r·A_F/π)·G(μ), G = sinθ(θ − sinθcosθ) + (2/3)((sinθ/cosθ)(1 − sin³θ) − sinθ), with the polynomial approximation from Listing 1.
  - ⟨Ê_F⟩ = A_F·(1 + (2/3 − 28/(15π))r).
  - ρ_ms = ρ²⟨Ê⟩/(1 − ρ(1 − ⟨Ê⟩)).
  - f_ms = ρ_ms/π·(1 − Ê(μ_o))(1 − Ê(μ_i))/(1 − ⟨Ê⟩). Reciprocal.
  - **Sampling:** CLTC (LTC matrix [[a,0,b],[0,c,0],[d,0,1]], clipped half-disk + half-ellipse) plus a uniform lobe as defensive one-sample MIS with P_u = r^0.1·(0.162925 − 0.372058μ + 0.538233μ² − 0.290822μ³).
  - Verified: albedo = 1.0000 exact, approx ≥ 0.9999; ∫pdf = 1.0000; chi² p = 0.74 / 0.30. Weight variance at r = 1, μ = 0.05 is **0.036 vs 0.93 with cosine sampling** (26×); at μ = 0.5, 0.022 vs 0.21.
- **Burley** (MaterialX `:140-156`): f = ρ/π·F_D(μ_i)·F_D(μ_o), F_D(μ) = 1 + (F90 − 1)(1−μ)⁵, F90 = 0.5 + 2r(ω_i·h)². Albedo fit `:150-156`.

```cpp
// core/bsdf/diffuse.h — EON (Listings 1-4), returns f*cos; checked in verify.cpp
constexpr float kFon1 = 0.5f - 2.0f / (3.0f * kPi), kFon2 = 2.0f / 3.0f - 28.0f / (15.0f * kPi);
NOVA_INLINE float eFonApprox(float mu, float r) {
    const float mc = 1.0f - mu;
    const float g = mc * (0.0571085289f + mc * (0.491881867f + mc * (-0.332181442f + mc * 0.0714429953f)));
    return (1.0f + r * g) / (1.0f + kFon1 * r);
}
NOVA_INLINE Spectrum eonFcos(Spectrum rho, float r, Vec3f wo, Vec3f wi) {
    if (wi.z <= 0.0f || wo.z <= 0.0f) return spectrum(0.0f);
    const float s = dot(wi, wo) - wi.z * wo.z, st = s > 0.0f ? s / std::fmax(wi.z, wo.z) : s;
    const float AF = 1.0f / (1.0f + kFon1 * r), avg = AF * (1.0f + kFon2 * r);
    const Spectrum fss = rho * (kInvPi * AF * (1.0f + r * st));
    const Spectrum rhoMs = rho * rho * avg / (spectrum(1.0f) - rho * (1.0f - avg));
    const float k = std::fmax(1e-7f, 1.0f - eFonApprox(wo.z, r)) * std::fmax(1e-7f, 1.0f - eFonApprox(wi.z, r))
                  / std::fmax(1e-7f, 1.0f - avg);
    return (fss + rhoMs * (kInvPi * k)) * wi.z;
}
NOVA_INLINE Spectrum eonAlbedo(Spectrum rho, float r, float mu) {
    const float AF = 1.0f / (1.0f + kFon1 * r), avg = AF * (1.0f + kFon2 * r), E = eFonApprox(mu, r);
    return rho * E + rho * rho * avg / (spectrum(1.0f) - rho * (1.0f - avg)) * (1.0f - E);
}
// ltcCoeffs, cltcSample, cltcPdf, eonPdf, eonSample: verbatim from EON Listings 2-4 (see
// verify/bsdf_sketch.h). Pass the lobe-selection remainder as the uniform-vs-CLTC choice
// instead of reusing u1 (paper), to keep the Sobol xy pair intact.
```

---

## 8. (g) Albedo-scaling layering, coat absorption, darkening

**Operator** (OpenPBR §Layering eq. non-reciprocal-albedo-scaling; Standard Surface; MaterialX `mx_layer_bsdf`):
- f = f_top + (1 − E_top(ω_o))·f_base. It conserves energy and preserves the furnace.
- In a path tracer ω_o is fixed per hit, so all layer factors are **scalars or colours per hit**. Layering therefore reduces to closure-weight arithmetic; Cycles does the same with `closure_layering_weight`, `svm/closure.h:117-146`.

**VM semantics** (from MaterialX GLSL): a BSDF register = {lobe slice, throughput T(ω_o) Spectrum}.

| Op | Lobes | Throughput |
|---|---|---|
| leaf, R-only (dielectric R, generalized Schlick R, sheen) | {w·tint·lobe} | T = 1 − w·E(ω_o) (E untinted) |
| leaf, opaque (diffuse, conductor, translucent) | {w·lobe} | T = 0 |
| `layer(top, base)` | top ∪ base × T_top | T_top·T_base |
| `mix(fg, bg, m)` | fg × m ∪ bg × (1−m) | lerp(T_bg, T_fg, m) |
| `add(a, b)` | a ∪ b | max(T_a + T_b − 1, 0) |
| `multiply(a, c)` | a × clamp(c, 0, 1) | T unchanged |

Karma's `mul_bsdf` instead scales the albedo: `Af = 1 − (1−Af)·b`.

**How the top albedo is obtained at runtime:**

| Top lobe | Albedo source |
|---|---|
| Dielectric R | `dielE_R/dielE_RT` 3D LUT (exact furnace). The MaterialX fit F0·A + F90·B is within 0.01 at F0 = 0.04. |
| Thin film | MaterialX airy-albedo approximation (§5) |
| F82 / Schlick | ABC LUT × compensation |
| Sheen | `charlieE` or the fit |
| Fuzz | Zeltner R |

**Dielectric base with an exact split** (resolves MaterialX's `layer(R, mix(T, opaque))` without double-counting Fresnel):
- R lobe: microfacet Fresnel F(ω_o·m)·specular_color, compensated by 1/E_RT.
- T lobe from outside: the BTDF with (1 − F(m)), **normalised by 1/E_T^ss(μ_o)** (E_T = E_RT − E_R from the LUTs).
- After the MaterialX layer factor (1 − E_R^comp) = E_T/E_RT the T energy becomes exactly E_T/E_RT, i.e. the compensated physical split. Opaque base: E_R^comp + (1 − E_R^comp)·ρ_diffuse.
- Furnace exact for T = 1 and for T = 0 with white diffuse.
- Backface of a non-thin-walled dielectric (Karma `mxpbr.h:697-718` does the same): R-mode nodes emit nothing (T = 1); T-mode nodes emit the full R+T lobe with 1/η (TIR inside) and 1/E_RT_inv.

**OpenPBR mixture weights** (spec §Reduction to a mixture of lobes, as the MaterialX graph implements it; F, C, M, T, S = fuzz, coat, metalness, transmission and subsurface weights):
- k₁ = 1 − F·R_fuzz(μ_o); fuzz lobe weight = F·fuzz_color.
- Coat R lobe weight k₁·C (η = coat_ior).
- k₂ = k₁·(1 − C·E_coat^comp(μ_o))·lerp(1, coat_color, C)·lerp(1, Δ, C·δ). The graph applies the coat attenuation and darkening as `multiply` nodes.
- Δ = (1 − K)/(1 − E_b·K) with K = 1 − (1 − F0_coat)/n_c² (graph lines 466-512).
  - The spec prefers K = lerp(K_s, K_r, r_b) with K_r using the hemispherical F_avg.
  - E_b = lerp(lerp(base_color, subsurface_color, S), base_color·specular_weight, M).
- Metal: k₂·M·specular_weight × F82 lobe (F0 = base_weight·base_color, tint = specular_color).
- Dielectric R: k₂(1 − M). T: k₂(1−M)(1 − E_R)·T·tint (tint = transmission_color when depth = 0). Diffuse: k₂(1−M)(1−E_R)(1−T)(1−S)·EON(base_weight·base_color). SSS: k₂(1−M)(1−E_R)(1−T)·S → M9; until then diffuse with subsurface_color.
- Thin film: one lobe with F = lerp(F, F_airy, thin_film_weight). This equals MaterialX's mix of two lobes because D and G are the same.
- Coat roughening of the base: r′ = lerp(r, min(1, r⁴ + 2r_c⁴)^¼, C).

**Coat absorption:**
- MaterialX graph and Standard Surface: constant `lerp(1, coat_color, C)`.
- OpenPBR spec: T_coat^{(1/μ_i^t + 1/μ_o^t)}, where coat_color = T² at normal incidence and μ^t = √(1 − (1−μ²)/n_c²).
- Cycles approximates both directions with ω_o: `coat_color^(n/√(n² − sin²θ_o))` (`slab_color_at_angle`, `bsdf_microfacet.h:1555`).
- For a per-hit constant weight use Cycles' form. The spec form is ω_i-dependent and would make the coat weight per direction (still GPU-friendly, but evaluated inside `eval`).

**Standard Surface specifics:**
- coat_affect_color: base_color^(1 + coat·coat_affect_color).
- coat_affect_roughness: r′ = lerp(r, 1, coat_affect_roughness·coat·coat_roughness).
- transmission_extra_roughness.
- Metal: exact conductor via artistic IOR, F_avg via the F82 fit (§3).

---

## 9. (h) Closure list, one-sample MIS, robustness

```cpp
// core/bsdf/closure.h (sketch; GPU: fixed size, switch on type, no virtuals)
enum class LobeType : uint8_t { Lambert, Translucent, Qon, Eon, Burley, ConductorNK, F82, DielectricR,
                                DielectricT, DielectricRT, ThinDielectricR, ThinDielectricT, Charlie, Zeltner };
struct Lobe {
    LobeType type; uint8_t frame; uint8_t flags;   // frame: index into ClosureList::frames (base/coat normal)
    Spectrum weight;                               // layering factors already applied (depend on wo only)
    Spectrum c0, c1;                               // F0 / n / rho ; F82 tint / k / tint
    float ax, ay, eta, extra;                      // roughness, IOR, film thickness, etc.
};
constexpr uint32_t kMaxLobes = 8;
struct ClosureList {
    Frame frames[2]; uint32_t count;
    Lobe lobe[kMaxLobes];
    float cdf[kMaxLobes];                          // selection, filled by prepare()
    uint32_t nonDelta;                             // bitmask
};
struct BsdfSample { Vec3f wi; Spectrum fcos; float pdf; uint32_t flags; }; // fcos = sum w f |cos|

// prepare: p_j proportional to lum(weight_j * albedoEstimate_j(wo)), floored so no lobe with weight > 0 gets p = 0
// eval   : sum_k w_k fcos_k(wo, wi), pdf = sum_{k non-delta} p_k pdf_k; lobes whose R/T class != geometric class -> 0
// sample : j from u.z; uc = (u.z - cdf[j-1]) / p_j feeds the lobe's discrete choice (R vs T, EON uniform vs CLTC)
//          delta lobe -> fcos = w_j * value / p_j, pdf flagged delta (no mixture)
//          otherwise  -> classify by N_g (reflect iff dot(wi,Ng)*dot(wo,Ng) > 0), must equal the lobe's class,
//                        then full mixture eval at wi (balance heuristic over lobes)
```

**Rules that keep it unbiased and robust:**
1. **Selection probabilities may depend only on ω_o**, never on the sampled microfacet. Mitsuba's `lobe_probs` (`principled.cpp:609-648`) notes this explicitly because the pdf is re-evaluated at ω_i.
2. Per-lobe cosine: return f·|ω_i·n_lobe| (coat and base normals differ). Nova's integrator becomes `beta *= s.fcos / s.pdf`, and NEE uses `evalClosures(...)` directly (replacing `bsdf.eval(wiLocal) * cosI` at `wavefront.cpp:263-264`).
3. **Delta lobes** (max(α_x, α_y) < 1e-3, pbrt `EffectivelySmooth`):
   - They are excluded from NEE and from the mixture pdf.
   - Store an explicit delta flag with `q.bsdfPdf` (`wavefront.cpp:286`). `bsdfSampleWeight` (`lights.h:411`) must then return 1 regardless of strategy; passing ∞ gives NaN in `powerHeuristic`, and Cycles' `pdf *= 1e6` is a hack.
   - If the list contains only delta lobes, skip the shadow ray.
4. **Ray origin** for transmission: `offsetRayOrigin(hit.P, transmit ? -ng : ng)` (currently always +ng, `wavefront.cpp:244`). Entering vs exiting comes from the *authored* `hit.Ng` (`dot(wo, hit.Ng) > 0`), before `faceForward`. Thin-walled: always entering, never bends.
5. **Shading-normal problem.** Bend each closure normal with Cycles' `ensure_valid_specular_reflection(Ng, wo, N)` (`bsdf_util.h:322-397`): threshold t = min(0.9·(ω_o·N_g), 0.01) and a quartic root solve for N′ in the (N_g, N) plane. This guarantees reflect(ω_o, N′)·N_g ≥ t and ω_o·N′ > 0. Karma's equivalent is `_mx_normal_clamp` with bias 0.01 (`mxpbr.h:536`).
   - In eval, test only the lobe's shading hemisphere. In sampling, also reject N_g/N disagreement (Cycles `bsdf_microfacet.h:1007-1016`); this avoids shadow-terminator artefacts.
   - For bump or normal maps on diffuse lobes, add Cycles' `bump_shadowing_term` (`bsdf.h:94-155`: G1_GGX with α² = 0.125·tan²θ_d; zero if `cos(Ns,I)·cos(Ns,N)·cos(N,I) < 0`).
   - Schüssler et al. 2017 (microfacet-based normal mapping) is the energy-conserving alternative but costs 2-3 lobe evaluations; keep it optional.
   - Tokuyoshi 2021 corrects VNDF normalisation for back-facing normals; already covered by the bounded-pdf form.
6. **NaN and Inf guards:** reject the sample when `!(pdf > 0) || !isFinite(fcos)`. Clamp μ to ≥ 1e-6 in compensation lookups. Note `vis()` has no tan and stays finite for μ → 0 because f·μ_i ≤ F·D/(2t_o).
7. **Sobol streams:** `stream::kBsdf` (`sobol.h:146`) is already 3D (xy direction, z lobe). The remapped z remainder provides the inner discrete choice. `kLayer` is not needed for albedo-scaling layering and can stay reserved.
8. The first-hit albedo AOV becomes Σ w_j·Ê_j (denoiser guide).

---

## 10. Reference implementations compared

| Aspect | pbrt-v4 | Cycles (main) | Mitsuba 3 principled | MaterialX GLSL 1.39.5 | Karma (VEX) |
|---|---|---|---|---|---|
| VNDF | Heitz 2018 disk | Heitz 2018 | Heitz VNDF | Dupuy-Benyoub cap | Heitz/d'Eon slope sampling (`ggx_utils.h:115-185`) |
| G | height-correlated | height-correlated | separable G1·G1 | height-correlated (avg α) | internal `ggx()` |
| MS compensation | none | Turquin + K-C F_ms, LUTs 32²/32³ | none | Turquin with F(VdotH), analytic fit | 1/E (fit) |
| Dielectric Fresnel in R | per microfacet | per microfacet | per microfacet | per microfacet | **curve-fit scalar at macro angle** |
| Metal | conductor n, k | F82-tint, conductor | Schlick | F82 (OpenPBR) / n, k (SS) | n, k (macro) / F82 |
| Sheen | none | Zeltner LTC table | Disney | Charlie (no G) / Zeltner | Lambert × fit / Zeltner |
| Diffuse | Lambert | EON | Disney | QON / EON | QON / EON |
| Thin film | none | Airy with FFT CMF LUT (working space) | none | Airy Gaussian fit, CIE-RGB(E) | Airy Gaussian fit, sRGB (cast) |
| Lobe selection | single BxDF | sample_weight ∝ albedo | ω_o-only budgets | n/a | n/a |

---

## 11. Proposed `core/bsdf/` (header-only, `NOVA_INLINE`, float only, POD)

| File | Contents |
|---|---|
| `bsdf_common.h` | `sqr`, `saturate`, `lerp` (float/Spectrum), `pow5`, `pow6`, `reflect`, `refract(wo, m, eta, cosT)`, `safeSqrt` (none exist in `core/math` today) |
| `microfacet.h` | `GgxDistribution` (§2), `alphaMtlx(r, aniso)`, `alphaOpenPbr(r, aniso)`, `isDelta(ax, ay)` |
| `fresnel.h` | §3 functions, polarized dielectric and conductor plus conductor phase (for thin film), `iorToF0`, `f0ToIor`, F82 `b` from (F0, F82), `conductorFavg` |
| `thin_film.h` | `fresnelAiry(cosθ, base{n3,k3 | F0,F82,F90,p}, filmIor, thicknessNm, const float xyzToRgb[9])`, normalised DC, `airyAlbedoApprox` |
| `energy.h` | `BsdfTables`, `lut2`, `lut3`, `msCompensation`, `dielectricCompensation`, `specRAlbedo`, `schlickAlbedo(F0, F90, b, μ, r)` |
| `dielectric.h` | R (bounded VNDF, tint, film), T (full cap, 1/E_T normalisation), RT (verified `RoughDielectric`), smooth delta R/T, thin-walled R′/T′ |
| `conductor.h` | microfacet reflection with Fresnel kind {nk, F82/generalized Schlick, constant}, bounded VNDF |
| `diffuse.h` | Lambert, translucent, QON + albedo fit, EON + CLTC/uniform, Burley |
| `sheen.h` | Charlie (MaterialX form, uniform sampling, albedo LUT or fit), Zeltner LTC |
| `shading_frame.h` | `ensureValidSpecularReflection`, `bumpShadowingTerm` |
| `closure.h` | `Lobe`, `ClosureList`, `prepareClosures`, `evalClosures`, `sampleClosures`, `albedoEstimate` |
| `tables/bsdf_tables.inc` | generated arrays + hash |
| `lambert.h` | kept for tests, or deleted once `closure.h` lands |

**Integration** in `shadeSurfaces` (`wavefront.cpp:229-290`):
1. Replace `LambertBsdf bsdf{hit.color}` (`:235`) with a closure list produced first by a native OpenPBR / Standard Surface parameter builder, and later by the bytecode VM.
2. NEE evaluates the mixture.
3. The BSDF sample stores pdf + delta flag.
4. Offset the origin by side.
5. Keep Russian roulette on `maxComponent(beta)`.

`WaveQueues` needs a per-path `uint8_t flags` (last bounce delta, inside dielectric) next to `bsdfPdf`.

**GPU (M10).**
- Tables travel as a pointer struct in the launch parameters.
- `ClosureList` (~0.6 KB with 8 lobes) lives in per-thread local memory and is created and consumed within one shade kernel, so it never goes into SoA queues.
- `std::sqrt/pow/exp` are fine in device code (already used throughout `core/math`). Prefer `pow5`/`pow6` multiplications and `exp2f(log2f(x)·k)` for Charlie.

---

## 12. LUT generator `tools/nova_bsdf_precompute`

Model it on Cycles `app/cycles_precompute.cpp`:
- C++ program linking only `core/` headers, run as `nova_bsdf_precompute <table>`, writes `core/bsdf/tables/bsdf_tables.inc`.
- Per cell: `Sampler{hash(x, y, z), i}.get3D(0)` with Nova's own Owen-scrambled Sobol, sample counts as in §4, `tbb::parallel_for`.
- Each sample calls the **runtime** sample function with F = 1 or the table's Fresnel model and accumulates f·cos/pdf in double.
- Convert axes to parameters: r = x/(N−1) clamped to [1e-2, 1] (α = r²); μ = max((y/(N−1))², 1e-4); η = ior_from_F0(z⁴) clamped to z ∈ [1e-4, 0.99].
- E_avg: draw μ from the third dimension and accumulate 2μE.
- ABC: one pass, three channels.
- Commit the output; regenerate with a CMake custom target `nova_bsdf_tables`, not in the default build (minutes of CPU time). Store parameters and the generator source hash in the `.inc` header.

---

## 13. Tests (Catch2 `[bsdf]` plus render tests)

1. **Normalisation:** ∫D(m)m_z dm = 1 and ∫D_ωo dm = 1 by (μ, φ) midpoint quadrature (both pass to 1e-4). The bounded and full reflection pdfs have mass ≤ 1 over H⁺, and quadrature agrees with the MC valid-sample fraction to 1e-3.
2. **Chi² per lobe:** pbrt `bsdfs_test.cpp:32-53, 185-358` style (80 × 160 bins, 10⁶ samples, 5 random ω_o, expected counts by 2D integration of `pdf()`, pooling below 5, α = 0.01 with Šidák). Cover GGX aniso (full and bounded), dielectric η ∈ {1.5, 1/1.5} × α ∈ {0.05, 0.5}, thin-walled T, EON r ∈ {0.5, 1}, Zeltner, Charlie, Burley, and the full closure mixture.
3. **Sample/eval consistency:** `sample()` returns fcos and pdf equal to `eval()` at the sampled ω_i within 1e-4 relative.
4. **Per-lobe white furnace** (MC, 2¹⁸ samples), all to within 0.5 %, for μ ∈ {0.05, 0.2, 0.5, 1}, r ∈ {0, 0.25, 0.5, 1}:
   - compensated metal with F = 1 → 1;
   - dielectric RT, η ∈ {1.33, 1.5, 2, 1/1.5} → 1 (energy units);
   - R + (1 − E_R^comp)·Lambert → 1;
   - EON ρ = 1 → 1;
   - coat over white EON → 1;
   - fuzz over white → 1.
5. **Energy bound:** 10⁴ random parameter sets (colours ≤ 1) give E ≤ 1 + 3σ, and no NaN or Inf over 10⁷ random (ω_o, ω_i) pairs, including grazing μ = 1e-6, back-facing pairs and α = 1e-4.
6. **Reciprocity:** f(ω_o,ω_i) = f(ω_i,ω_o) for Lambert, QON, EON, GGX R without Turquin, and Charlie without softening. BTDF in radiance mode: f(ω_o,ω_i)/f(ω_i,ω_o) = (n_o/n_i)² (measured 0.4444 for η = 1.5).
7. **Fresnel:** exact dielectric at μ = 1 equals ((η−1)/(η+1))²; TIR when η < 1 beyond critical; conductor with k = 0 equals dielectric; F82(1/7) = tint·Schlick(1/7); F82 average formula against quadrature; F ≥ 0; artistic IOR round trip (F_c(1, n, k) = r).
8. **LUT regression:** 16 random cells per table recomputed at 2¹⁶ samples agree within 4σ; spot values from §4; E(μ, r → 0) → 1 for F = 1; monotone in r for fixed μ ≥ 0.2.
9. **Integrator furnace** (extend `tests/test_furnace.cpp`): white dome, sphere, maxDepth 64, the OpenPBR §"White furnace testing" configurations, pixel = 1 ± noise.
10. **Analytic head-on probe** (port `gen_karma_probe.py` to `tests/reference/tools/gen_scenes.py`): each quad value equals the Nova closed form or LUT at μ = 1 (EON 1, QON A(σ), F82 metal, Zeltner R(1, r), ...).
11. **Karma comparisons:** add the probe scene and material spheres (dome + rect light) to `tests/reference/manifest.json`. The known structural deviations need per-scene tolerances or `pending`:
    - dielectric spec at μ = 1, α 0.09: Nova ≈ 0.0398 vs Karma 0.0416;
    - Standard Surface sheen lobe shape (Lambert in Karma);
    - thin-film colour cast;
    - Karma uses the MaterialX fits for E (white metal 0.985 / 0.995 vs Nova 1.0).

    Uniform-dome scenes compare energy and should agree to ≤ 1-2 % except thin film.

---

## 14. Pitfalls (each measured or traced to source)

- Bounded VNDF for transmission is **biased**: the cap excludes microfacets that refract. Use it for reflection-only lobes only.
- MaterialX's analytic GGX albedo fit is off by up to 0.039 (grazing, α = 1). Do not use it for compensation if furnace tests must pass.
- The MaterialX Charlie albedo fit only matches the no-G MaterialX form; with the original G form, energy differs by up to ~45 %.
- The Zeltner albedo fit goes negative; clamp it and skip degenerate LTCs.
- F82 with low tint goes negative; clamp F and its average.
- The Kulla–Conty F_avg fit is 23 % off at η = 1.1; use the 1D LUT.
- The thin-film colour space produces Karma's measured 1.27 R/G cast; keep the DC term grey in Nova.
- Karma's dielectric spec has no microfacet Fresnel (curve-fit scalar). Expect a few % of intensity difference and a different grazing falloff.
- Lobe selection must not depend on the microfacet; the pdf mixture would be wrong.
- The BTDF 1/η² belongs to radiance transport only; light tracing (future) must not apply it.
- Thin-walled surfaces must never refract or push an IOR stack; the transmission lobe is the mirrored, roughened reflection.
- OpenPBR and Standard Surface use different roughness-anisotropy mappings, and the anisotropic LUT index is r = (α_xα_y)^¼.
- MaterialX `multiply` keeps throughput while Karma's `mul_bsdf` scales the albedo. Follow MaterialX in the VM unless Karma parity is required.
- With a coat IOR above the base IOR, apply OpenPBR's ratio inversion; otherwise a bright grazing TIR ring appears.
- Current Nova code to change: NEE (`wavefront.cpp:259`) requires `dot(ng, wi) > 0` and the BSDF sample (`:274`) requires `dot(wi, ng) > 0`. Both must become class-aware (reflect vs transmit) or transmission never receives direct light.

---

## 15. Sources

- **Local:**
  - `$HFS/houdini/materialx/libraries/pbrlib/genglsl/{lib/*.glsl, mx_*_bsdf.glsl}`, `bxdf/{open_pbr_surface,standard_surface}.mtlx`, `pbrlib/pbrlib_defs.mtlx`.
  - `$HFS/houdini/vex/include/{mxpbr.h, ggx_utils.h, mxtypes.h}`.
  - `$HFS/houdini/help/vex.zip` (`functions/ggx.txt`, `sheen.txt`, `diffuse.txt`, `xyztorgb.txt`).
- **pbrt-v4** (master): `src/pbrt/util/scattering.h`, `src/pbrt/bxdfs.{h,cpp}`, `src/pbrt/bsdfs_test.cpp` — https://github.com/mmp/pbrt-v4
- **Cycles** (main): `intern/cycles/kernel/closure/{bsdf_microfacet.h, bsdf_util.h, bsdf_sheen.h, bsdf_oren_nayar.h, bsdf.h}`, `kernel/svm/closure.h`, `scene/{shader.cpp, shader.tables}`, `app/cycles_precompute.cpp` — https://github.com/blender/blender
- **Mitsuba 3:** `src/bsdfs/principled.cpp`, `include/mitsuba/render/microfacet.h` — https://github.com/mitsuba-renderer/mitsuba3
- **OpenPBR 1.1.1:** https://academysoftwarefoundation.github.io/OpenPBR/ (source `index.html` in its GitHub repo).
- **EON:** Portsmouth, Kutz, Hill 2024 — https://arxiv.org/html/2410.18026
- **Bounded VNDF:** Eto & Tokuyoshi 2023 — https://gpuopen.com/download/Bounded_VNDF_Sampling_for_Smith-GGX_Reflections.pdf
- **Back-facing VNDF:** Tokuyoshi 2021 — https://gpuopen.com/download/publications/Unbiased_VNDF_Sampling_for_Backfacing_Shading_Normals.pdf
- **Imageworks sheen:** Estevez & Kulla 2017 — https://fpsunflower.github.io/ckulla/data/s2017_pbs_imageworks_sheen.pdf
- **Kulla & Conty 2017** slides — https://fpsunflower.github.io/ckulla/data/s2017_pbs_imageworks_slides_v2.pdf
- **Cited through the code listed above** (not downloaded): Turquin 2019; Belcour & Barla 2017; Zeltner et al. 2022; Hoffman 2019/2023; Gulbrandsen 2014; Walter et al. 2007; Heitz 2014/2018; Dupuy & Benyoub 2023; Schüssler et al. 2017.