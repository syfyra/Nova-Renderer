# Generic MaterialX BSDF graphs in Nova (M4 research)

Scope: MaterialX 1.39.5 pbrlib closure nodes as shipped with Houdini 22.0.466, how Karma CPU renders generic
pbrlib graphs (measured), and a design for compiling such graphs onto Nova's lobe library
(`core/bsdf`). Context: `docs/design/M3.md` K1/K13 and table 2.2 defer "generic pbrlib BSDF graphs (layer/mix/add
of `*_bsdf`, `mix_surfaceshader`, `kMaxLobes` 16)" to M4. Today `Translator::Run()` rejects every surface node
except `standard_surface`, `open_pbr_surface`, `surface_unlit` and `UsdPreviewSurface`
(`hydra/material/translator.cpp:1416-1442`), so the material falls back to the next render context or the default material.

## 0. Key findings

1. **Inventory.** pbrlib 1.39.5 has 10 BSDF leaves, 3 EDF leaves plus an EDF modifier, and 2 VDFs. Its combinators are
   `layer` (BSDF and VDF), `mix`, `add` and `multiply` (F and C). The constructors are `surface`, `volume`, `light` and
   `displacement`; stdlib adds `surfacematerial` and `mix_surfaceshader` (section 2).
   - 1.39 has **no `thin_film_bsdf`**. Thin film is now an input pair on dielectric, conductor and generalized-Schlick
     leaves. Houdini still ships the 1.38 node in `libraries/houdini/legacy_1_38.mtlx`, and Nova must rewrite it itself.
2. **Every shipped "uber" graph is a weighted sum of leaves.** That covers Standard Surface (8 leaves), OpenPBR (11),
   UsdPreviewSurface (6), glTF PBR (8), Disney principled (7) and the 12 Lama nodes.
   - The layer weights are `1 − albedo(top)` at ωo; that factor depends on ωo, so the closure builder must compute it at the hit,
     not the VM.
3. **Semantics disagree on three edge cases:** `multiply` inside a layer top, `add` inside a layer top, and an
   opaque leaf with weight < 1 used as a top. The sources are the MaterialX spec text, genglsl, OSL testrender and Karma.
   - None of the five uber graphs hits these cases.
   - Houdini's Lama nodes do: `LamaLayer = layer(multiply(top, topMix), base)`.
4. **Karma CPU interprets the graph node by node. Its Standard Surface *is* that graph:** `layer(dielectric R, oren_nayar)`
   gives the same pixels as the equivalent Standard Surface (max |Δ| = 0.0).
   - Albedo scaling uses a per-ωo fitted albedo that matches its uncompensated dielectric lobe. The white furnace gives 0.992–1.000.
   - Mixes and per-pixel weights are exactly linear.
   - `mix_surfaceshader` is linear.
5. **Karma differs from MaterialX in the edge cases.** It passes the base down into the top subtree:
   - `layer(multiply(t, w), b)` gives `w·layer(t, b)`, so `w = 0` renders black.
   - `layer(add(t1, t2), b)` gives `layer(t1, b) + layer(t2, b)`, so a white furnace returns 2.0.
   - `LamaLayer` loses its base entirely.

   Other Karma deviations:
   - Generalized-Schlick layers gain up to +11 %, because the lobe is normalized while the throughput is not.
   - `burley_diffuse_bsdf` renders exactly like its `oren_nayar` (QON).
   - `conical_edf` renders as uniform on surfaces.
   - Thin film uses a plain Rec.709 XYZ→RGB matrix with a clamp (section 4).
6. **Mapping onto Nova's lobes.** Nearly every leaf maps onto an existing lobe. Missing:
   - Burley diffuse;
   - the `color82` term with arbitrary F90 and exponent (Nova's F82-tint fixes F90 = 1 and exponent 5);
   - generalized-Schlick transmission;
   - Chiang hair (M9).
7. **Design.** Three parts (section 5):
   - The VM computes all leaf parameters and the combinator factors, which do not depend on ωo.
   - A per-material POD **closure program** (post-order node list) describes the structure.
   - A builder makes two fixed loops per hit: albedos bottom-up, weights top-down. It has no recursion and is DAG-safe.

   Sizes: `kMaxLobes` 16 and up to 4 frames. On overflow the builder keeps the 16 lobes with the largest energy estimates.
   `mix_surfaceshader` becomes up to 4 weighted *segments*, each a native model or a closure program.
8. **Keep the native models.** Use the generic path for graphs nobody wrote natively (glTF, Disney, Lama, user graphs). Use it also as
   an **equivalence oracle**: Standard Surface's own nodegraph, flattened generically, must match the native closure.
   - That holds exactly in expectation, outside a short list of documented graph omissions (section 7.2).

---

## 1. Sources and method

| What | Where |
|---|---|
| pbrlib definitions | `$HFS/houdini/materialx/libraries/pbrlib/pbrlib_defs.mtlx` (465 lines; line numbers below) |
| Reference math | `pbrlib/genglsl/mx_*.glsl`, `genglsl/lib/mx_microfacet_{specular,diffuse,sheen}.glsl`, `pbrlib/genosl/pbrlib_genosl_impl.mtlx` |
| stdlib shader nodes | `stdlib/stdlib_defs.mtlx:87-92` (`surfacematerial`), `:109-116` (`surface_unlit`), `:3853-3858` (`mix_surfaceshader`), `:4769+` (`convert_*_surfaceshader`), `:5136+` (`dot_*shader`) |
| Uber graphs | `bxdf/{standard_surface,open_pbr_surface,usd_preview_surface,gltf_pbr,disney_principled}.mtlx`, `bxdf/lama/*.mtlx` |
| Legacy nodes | `libraries/houdini/legacy_1_38.mtlx` (`ND_thin_film_bsdf`, `ND_thin_surface`, `ND_multiply_surfaceshader{F,C}`), `legacy_1_39.mtlx` |
| Spec text | MaterialX PBR spec v1.39 (`documents/Specification/MaterialX.PBRSpec.md`, main branch, fetched 2026-10-13) |
| 1.39 upgrade of thin film | MaterialX `source/MaterialXCore/Version.cpp` (`upgradeVersion`, `layer` branch) |
| OSL reference path tracer | OpenShadingLanguage `src/testrender/shading.cpp` (`process_bsdf_closure`, `evaluate_layer_opacity`) |
| Karma CPU (read only, never copied) | `$HFS/houdini/vex/include/mxtypes.h:22-71`, `mxpbr.h` (signatures at :578, :586, :592, :605, :783, :830, :994, :1024, :1085, :1102, :1108) |
| Earlier Nova research | `docs/research/m3/surface-models.md` §4-6 (genglsl quotes, Karma VEX structure), `docs/research/m3/microfacet.md` |

**Karma probes (section 4).** Each probe is a unit sphere (a one-point `Points` prim, so the sphere is exact), seen orthographically in a white dome
of radiance 1. Because the sphere is convex, the pixel at radius ρ shows the closure's directional albedo at μ = √(1−ρ²). The setup is that of
`tests/reference/tools/karma_albedo_probe.py`, with these settings:
- Karma CPU 22.0.466 under husk in an isolated environment;
- 128², 512 spp, box filter at scale 1e-6 (pixel centers);
- ray limits 16, `causticsenable 1`, `causticsroughnessclamp 0`, `internalreflectenable 1`, `thinwallatrefractlimit 0`,
  `constrainmaxrough 0`, color limits 1e9.

The values are means over ten μ bins (noise about 1e-3). The networks are authored as USD `Shader` prims with `ND_*` ids under `outputs:mtlx:surface`.
The scripts (`probe_graphs.py`, `analyze.py`, `derive.py`) are in this session's scratchpad (`m4research/bsdf-graphs/`), not in the repo.

---

## 2. Inventory: MaterialX 1.39.5 pbrlib closure nodes

Notation: `w` is the `weight` input. The **throughput** T is what genglsl passes to a layer's base: `layer` scales the base by T,
so T = 1 − (directional albedo at ωo).

### 2.1 BSDF leaves (`pbrlib_defs.mtlx`)

| Node (lines) | Inputs (defaults) | `bsdf` / scatter | Semantics (spec + genglsl/genosl) |
|---|---|---|---|
| `oren_nayar_diffuse_bsdf` (26-33) | w 1, color 0.18, roughness 0, normal Nworld, `energy_compensation` false (uniform) | R | false: qualitative ON, `A + B·s/max(NL,NV)` (`lib/mx_microfacet_diffuse.glsl:8-19`). true: EON (Portsmouth et al. 2024; `:105-135`). glsl T = 0 always, **set before the weight test** (`mx_oren_nayar_diffuse_bsdf.glsl`) |
| `burley_diffuse_bsdf` (39-45) | w, color 0.18, roughness 0, normal | R | Disney diffuse `f = c/π·F_L·F_V`, `F90 = 0.5 + 2r(L·H)²` (`mx_microfacet_diffuse.glsl:140-146`). Not energy conserving: albedo 1.04–1.43 at r 1 (numeric, section 4.5). Albedo fit `:150-156`. T = 0 |
| `translucent_bsdf` (51-56) | w, color 1, normal | (R) | Lambert in the flipped hemisphere. T = 0 |
| `dielectric_bsdf` (62-75) | w, tint 1, ior 1.5, roughness **α** (0.05, 0.05), retroreflective, `thinfilm_thickness` 0 nm, `thinfilm_ior` 1.5, normal, tangent Tworld, distribution ggx, **`scatter_mode` R/T/RT** (uniform) | R, T, RT | GGX with exact dielectric Fresnel (Airy if thickness > 0), Turquin compensation `1 + F(1−E)/E` (`mx_microfacet_specular.glsl:178-182`). T = 1 − w·E(F0(ior)) · comp, **untinted** (`mx_dielectric_bsdf.glsl:46-51`). genosl: reflection tint = 0 for "T", transmission tint = 0 for "R" (`genosl/mx_dielectric_bsdf.osl`). The T response includes (1 − F) per microfacet (`lib/mx_environment_fis.glsl`: `FG = 1 − F` for refraction). Spec: "tint should stay at white"; it scales the lobes, not the Fresnel |
| `conductor_bsdf` (81-93) | w, ior (0.183, 0.421, 1.373), extinction (3.424, 2.346, 1.770) (gold), roughness α, retroreflective, thin film, normal, tangent, distribution | R | exact (n, k) Fresnel, Turquin compensation. T = 0 |
| `generalized_schlick_bsdf` (99-114) | w, color0 1, **color82 1**, color90 1, exponent 5, roughness α, retroreflective, thin film, normal, tangent, distribution, scatter_mode | R, T, RT | Hoffman Schlick: `mix(F0, F90, (1−μ)^e) − a·μ(1−μ)^6`, with `a` built from color82 (`mx_fresnel_hoffman_schlick`, `:214-223`). color82 = 1 disables the term. T = 1 − w·**avg**(E(F0, F90))·comp (grey; `mx_generalized_schlick_bsdf.glsl`). Transmission refracts with `ior = f0_to_ior(avg F0)` |
| `subsurface_bsdf` (120-127) | w, color 0.18, radius 1 (mfp), anisotropy 0, normal | R | genosl: `subsurface_bssrdf`. genglsl: screen-space approximation. T = 0 |
| `sheen_bsdf` (133-140) | w, color 1, roughness 0.3, normal, **`mode` conty_kulla / zeltner** | R | conty_kulla: Imageworks/Charlie without masking. zeltner: LTC, roughness clamped to [0.01, 1] (`mx_sheen_bsdf.glsl`). T = 1 − w·E (untinted). genosl ignores `mode` |
| `chiang_hair_bsdf` (146-159) | tint_R/TT/TRT, ior 1.55, roughness_R/TT/TRT, cuticle_angle, absorption_coefficient, normal, curve_direction | R | Chiang 2016 hair; needs curves (M9) |

Common to all leaves:
- `retroreflective` reflects V about N for the R and indirect parts.
- `distribution` only offers `ggx`.
- Roughness is **α**, not perceptual roughness; `roughness_anisotropy` and `roughness_dual` (`:384-397`) map to it.

### 2.2 Combinators

| Node | Spec (PBR spec v1.39) | genglsl | OSL testrender | Karma CPU (`mxtypes.h`, measured) |
|---|---|---|---|---|
| `layer(top, base)` BSDF (294-298) | "f_top + T_top·f_base", transmittances multiply | `R = R_t + R_b·T_t; T = T_t·T_b` (`mx_layer_bsdf.glsl:5-6`) | base pushed with `w·(1 − clamp(opacity(top), 0, 1))`; opacity is RGB (`shading.cpp`, `MX_LAYER_ID`) | implicit: the base is passed as `$base` into the top leaf's VEX function (`mxpbr_dielectric(..., base)`, `mxpbr_generalized_schlick(..., base)`, `mxpbr_sheen(..., base)`); leaves without a `base` parameter drop it |
| `layer(top, vdf)` (299-303) | surface bound to an interior medium | `R = R_t·T_vdf` | — | not measured |
| `mix(fg, bg, m)` BSDF/EDF/VDF (308-325) | `(1−m)·bg + m·fg`, transmittance alike | lerp of R and T | MIX scales the branches | lerp of F and Af (exactly linear, measured) |
| `add(in1, in2)` (330-344) | "does **not** guarantee energy conservation" | `R1 + R2; T = max(T1 + T2 − 1, 0)` | sum, opacity clamped at the layer | `Af = a + b − 1`; inside a top, the base is passed to both branches (measured: furnace 2.0) |
| `multiply(in1, w)` F/C (349-378) | weight ≤ 1 to conserve energy | `R·clamp(w, 0, 1)`; **T unchanged** | MUL scales the weight and the layer opacity | `F·w; Af = 1 − (1 − Af)·w`; no clamp; inside a top, w also scales the base (measured) |

`multiply` inside a layer top therefore has three meanings, shown for `layer(multiply(t, w), b)`:
- genglsl gives `w·t + (1 − E_t)·b`, so the base is darkened even at w = 0.
- OSL testrender and the spec's linearity give `w·t + (1 − w·E_t)·b`.
- Karma gives `w·t + w·(1 − E_t)·b`, so w = 0 renders black.

The weight semantics of opaque leaves also differ. The spec says "For opaque BSDFs, `weight` acts as statistical surface coverage. The covered fraction occludes lower layers, and the rest passes light through". genglsl and Karma instead give opaque leaves T = 0 at any weight.

### 2.3 EDF, VDF, constructors, materials

| Node | Inputs | Semantics |
|---|---|---|
| `uniform_edf` (169-172) | color | Lambertian emission `L = color` |
| `conical_edf` (178-184) | color, normal, inner_angle 60, outer_angle 0 | emission inside a cone around N (no falloff if outer < inner). **No genglsl/genosl implementation** (genmdl only) |
| `measured_edf` (190-195) | color, normal, file (IES) | IES profile. genmdl only |
| `generalized_schlick_edf` (202-208) | color0, color90, exponent, base EDF | `base·mix(c0, c90, (1−μ)^e)` with μ against the forward-facing N (`mx_generalized_schlick_edf.glsl`) |
| `absorption_vdf` (218-221), `anisotropic_vdf` (228-233) | absorption (and scattering, HG anisotropy) | genglsl: `T = exp(−absorption)` only; genosl: `anisotropic_vdf(albedo, extinction, g)` |
| `surface` (243-249) | bsdf, edf, opacity (**float**), thin_walled (uniform) | genosl clamps opacity to [0,1]. thin_walled: "closed surfaces refract, thin-walled pass straight through" |
| `volume`, `light`, `displacement` (255-285) | vdf+edf; edf, intensity, exposure; displacement float or vector3 (dPdu, dPdv, N space), scale | volumes M8; `light` scales the EDF by intensity·2^exposure; displacement is a separate M4 topic |
| `surfacematerial` (stdlib 87-92) | surfaceshader, **backsurfaceshader**, displacementshader | Nova already follows `surfaceshader` (`translator.cpp:1401-1411`) |
| `mix` surfaceshader/displacement/volume (stdlib 3853-3870) | fg, bg, mix | linear (Karma measured: residual 1e-5) |
| `convert_*_surfaceshader`, `dot_*shader` | — | nodegraphs onto `surface_unlit`; pass-through |
| legacy `thin_film_bsdf` (legacy_1_38.mtlx:113) | thickness 550 nm, ior 1.5 | 1.38 modifier, used as `layer(thin_film, X)`. MaterialX's 1.39 upgrade copies thickness and ior to **every** dielectric, conductor and generalized-Schlick node upstream of that layer, except `scatter_mode` "T", and replaces the layer by its base (`Version.cpp`; CHANGELOG 1.39.0 "automatic upgrade logic for legacy documents"). Karma keeps a `mxpbr_thinfilm(..., base)` |
| legacy `thin_surface` | front/back bsdf+edf, opacity | 1.38; Karma `mxpbr_thin_surface` |

### 2.4 How the shipped graphs use the combinators

Closure skeletons were dumped from the `.mtlx` files (section 1); the counts below are leaf BSDF nodes.

| Graph | Leaves | Layer tops | Notes |
|---|---|---|---|
| Standard Surface (`NG_standard_surface_surfaceshader_100`) | 8 | leaves (sheen, specular R, coat R) | `transmission_mix = mix(dielectric T, sheen_layer)` lies under the specular R layer. `multiply(metalness_mix, coat_attenuation)` is a **base** |
| OpenPBR | 11 | mix of two R dielectrics (thin film on/off), coat R, fuzz (zeltner); `layer(dielectric T, anisotropic_vdf)` | `multiply` only on bases (darkening, coat attenuation, thin-walled SSS factors) |
| UsdPreviewSurface | 6 | Schlick leaves | **DAG**: `transmission_mix` is the base of two layers |
| glTF PBR | 8 | mix of Schlick leaves, sheen, clearcoat | |
| Disney principled | 7 | Schlick leaves, sheen | uses `burley_diffuse_bsdf`, `subsurface_bsdf` |
| Lama (`bxdf/lama`, 12 nodedefs) | 1-2 each | **`LamaLayer = layer(multiply(top, topMix), base)`**; `LamaGeneralizedSchlick = layer(multiply(schlick R, reflectionTint), multiply(layer(schlick T, vdf), transmissionTint))` | multiply inside a top; Schlick transmission |

So the edge-case semantics (2.2) do not affect the five uber graphs, but they decide how `LamaLayer` and
`LamaGeneralizedSchlick` look.

---

## 3. Mapping onto Nova's lobe library

| pbrlib leaf | Nova lobe (`core/bsdf`) | Layering albedo at ωo (existing code) | Gap |
|---|---|---|---|
| oren_nayar, roughness 0 | `Lambert` | coverage (opaque) | — |
| oren_nayar, energy_compensation false | `Qon` | `QonLobe::albedoFit` (picks, AOV) | — |
| oren_nayar, energy_compensation true | `Eon` | `EonLobe::directionalAlbedo` | — |
| burley_diffuse | — | — | **new `Burley` lobe** (eval per MaterialX, cosine sampling, `mx_burley_diffuse_dir_albedo` fit for picks), or map to QON like Karma (Q2) |
| translucent | `Translucent` | coverage | — |
| subsurface | Lambert with albedo `color` (M3 fallback; thin-walled → translucent) | coverage | random walk M9 |
| dielectric R | `Dielectric` kReflect (+ `ThinFilm`) | `dielectricReflectionEnergy().albedo` = E_R/E_RT, film blend as in `pushDielectricLayer` (`surface_models.h:75-137`) | — |
| dielectric T | `Dielectric` kTransmit | coverage | weight `1/E_T`, E_T = E_RT − E_R (both tables exist for η > 1); see 5.2 |
| dielectric RT | `Dielectric` kBoth, `1/E_RT` (`pushTransmission`) | coverage | — |
| thin_walled + T/RT | `ThinDielectric` | coverage | — |
| conductor | `Conductor` FresnelModel::Conductor (+ film), Turquin via `msCompensation` | coverage | — |
| generalized_schlick R | `Conductor` FresnelModel::Schlick (c0 = F0, c1 = F90, exponent) | `ggxSchlickAlbedo·msCompensation` (`pushSchlickLayer`, `:385-403`), spectral | **color82** term (Hoffman `a`; one more `Spectrum` in `ConductorLobe`); the albedo table assumes exponent 5 |
| generalized_schlick T/RT | — | — | M4: a `Dielectric` lobe with η = f0_to_ior(avg F0), exact Fresnel, warning (MaterialX itself refracts with that η) |
| sheen conty_kulla | `Charlie` | `sheenAlbedo` (charlieE), capped at 1 | — |
| sheen zeltner | `Zeltner` | `zeltnerAlbedo` (MaterialX fit; Karma uses the same fit, 4.5) | — |
| chiang_hair | — | — | M9 (curves); until then dropped with a warning |
| uniform_edf / multiply / mix / add EDF | `cl.emission` | — | — |
| generalized_schlick_edf | emission × `mix(c0, c90, (1−μ)^e)` in the builder | — | — |
| conical_edf, measured_edf | uniform + warning (what Karma renders, 4.6) | — | cone and IES later |
| absorption/anisotropic_vdf under `layer` | `cl.sigmaA` (Beer, as `transmission_depth`) | — | scattering M8 |

The albedo helpers are currently inside the native builders. M4 should move them into one `leafAlbedo(kind, params, μ)`
used by both paths. That way the native and generic closures share every number, including the Charlie cap
(`surface_models.h:335-344`) and the film blend.

`Lobe` is about 88 B (estimated from the struct layout: 24 B header plus a 64 B union). `ClosureList` at 16 lobes and
4 frames is therefore about 1.6 KB on the stack. It is built and consumed per lane (K13), and `clear()` only resets `count`,
so the native models do not pay for the larger capacity.

---

## 4. Karma CPU on generic graphs (measured)

Unless noted: d = `dielectric_bsdf` R, ior 1.5, α 0.25 (Standard Surface roughness 0.5); "white"/"½" = `oren_nayar`
with color 1 / 0.5, roughness 0. Columns are the μ bins centred at 0.05 / 0.35 / 0.95.

### 4.1 Albedo scaling under a dielectric layer

| Probe | Karma | Reference |
|---|---|---|
| d over black | .1753 / .0809 / .0369 | single-scatter E_R (oracle, `gen_albedo_ref.ggx_weights`): .1850 / .0930 / .0376. Nova E_R/E_RT: .1997 / .0991 / .0379 |
| Standard Surface (base_color 0, specular 1, r .5) | identical to the line above, **max per-pixel |Δ| = 0.0** | |
| layer(d, white) | .9922 / .9988 / 1.0000 | 1 if consistent |
| layer(d, ½) = Standard Surface (base_color .5) | .5838 / .5399 / .5185, identical pixels | |
| albedo used for scaling, `1 − 2·(white − ½)` | .1833 / .0821 / .0369 | the lobe's own .1753 / .0809 / .0369 |
| layer(coat (η 1.6, α .05), layer(d, ½)) | .7690 / .6128 / .5447 | product rule from single layers: .7693 / .6129 / .5448 |

Conclusions:
- Karma's dielectric layer albedo depends on ωo. It is uncompensated: 5–17 % below the single-scatter E_R and 12–20 % below
  Nova's E_R/E_RT, consistent with M3's −15 % at r 0.5.
- The scaling uses an albedo that tracks its own lobe, so the white furnace gives 0.992–1.000.
- Nested layers multiply their throughputs.
- Karma renders Standard Surface by this same generic path.

### 4.2 Combinators

| Probe | Karma | Linear / spec prediction | genglsl prediction |
|---|---|---|---|
| add(d, ½) | .6753 / .5810 / .5370 | d + .5: .6753 / .5809 / .5369 | same |
| layer(d with w .5, white) | .9962 / .9995 / 1.0001 | .5d + (1 − .5E): .9960 / .9994 / 1.0000 | same |
| layer(mix(d, gold conductor α .1, .5), white) | (.952 .906 .784) at μ .05 | .5·layer(d, white) + .5·cond: (.951 .906 .784) | same |
| layer(multiply(d, .5), white) | .4961 / .4994 / .5000 | .5d + (1 − .5E): ≈ .996 / .999 / 1.000 | .5d + (1 − E): .904 / .958 / .982 |
| layer(multiply(d, .5), ½) | .2919 / .2699 / .2592 (= .5·layer(d, ½)) | .5d + (1 − .5E)·.5 | |
| layer(multiply(d, 0), ½) | **0** | .5 | .5(1 − E) |
| layer(add(d, d), white) | **1.984 / 1.998 / 2.000** (= 2·layer(d, white)) | 2d + max(1 − 2E, 0): .984 / .998 / 1.000 | same as spec |
| layer(oren w .5 white, oren blue) | (.5, .5, .5) | coverage (spec): (.5, .5, 1.0) | (.5, .5, .5) |
| LamaLayer(d, ½, topMix 1 / .5 / 0) | .1753… (= d) / .0876… (= .5d) / 0 | layer(d, ½) / .5d + (1 − .5E)·.5 / .5 | |

Conclusions:
- Karma handles `layer` by passing the base into the top's leaves. Under `multiply` and `add` the base is scaled or duplicated.
- An opaque top drops the base at any weight.
- `LamaLayer` loses its base entirely, probably because the base reaches the `layer` through a nodegraph interface input.
  That is a Karma defect, and nothing in Nova should imitate it.

### 4.3 Per-pixel weights

- `mix(gold conductor, ½, m)` with m = clamp(0.5 + 0.5·x) from `position` (object space).
  - Per-pixel residual against `m·cond + (1 − m)·.5`: mean 1e-5, rms 1e-3. The signal rms is 0.10–0.44, so the residual is noise.
  - The residual is flat in every quarter of m.
- `layer(d with w = m(x), ½)` against `m·layer(d, ½) + (1 − m)·.5`: residual mean 2e-5.
  - Per m-quarter at μ < 0.3, measured vs model: .5058/.5058, .5228/.5227, .5379/.5379, .5557/.5557.
- Varying weights are therefore evaluated per hit and are exactly linear. A texture input is equivalent; the Points sphere has no `st`.

### 4.4 Generalized Schlick, F82, thin film

| Probe | Karma | Reference |
|---|---|---|
| schlick color0 (.8, .1, .1), color90 1, α .25 | R .796/.799/.799, G .231/.152/.100 | single-scatter E_F: R .762/.683/.729, G .229/.130/.091; E_F/E_1: R .834/.812/.800, G .251/.155/.100 |
| layer(that, white) | **R 1.043/1.111/1.068**, G 1.003/1.022/1.008 | 1 if consistent |
| albedo used for scaling | R .753/.688/.731, G .228/.130/.092 | ≈ the single-scatter E_F (spectral) |
| schlick F0 .8, color82 .5, smooth | .5869 / .6191 / .8000 (.4508 at μ .15) | MaterialX Hoffman: .5898 / .6190 / .8000 (.4520) |
| gold conductor, smooth | (.973 .933 .798) / (.937 .798 .465) / (.944 .776 .374) | exact (n, k) Fresnel: identical |
| gold + film 400 nm, ior 1.5 | G .612/.520/.449, B .695/.358/.235, R 1.000 | Python port of `mx_fresnel_airy` (`mx_microfacet_specular.glsl:315-412`) with **plain Rec.709** XYZ→RGB plus clamp: G .614/.520/.449, B .696/.358/.235 (R unclamped 1.45–1.79). MaterialX's CIE-RGB(E) matrix: G .720/.625/.568, B .763/.432/.309. Nova's grey-normalized Rec.709 (`thin_film.h:27-31`): G .648/.548/.473, B .766/.394/.259 |
| dielectric 1.5 + film 300 nm/1.8, smooth | (.835 .655 .611) / (.321 .211 .153) / (.127 .039 .108) | plain Rec.709 port: (.833 .656 .612) / (.321 .211 .153) / (.127 .039 .108) |
| layer(that, white) | 1.000 in every bin and channel | the film throughput is spectral, 1 − F_airy(μ) |

Conclusions:
- The generalized-Schlick throughput is spectral but uses the un-normalized albedo while the lobe is normalized. That makes it gain up to +11 %;
  Nova's `pushSchlickLayer` uses one compensated albedo for both and stays at 1.
- F82 follows MaterialX exactly.
- Thin film in a layer is energy-consistent.
- Thin-film color differs between all three implementations; see Q5.

### 4.5 Diffuse and sheen leaves

| Probe | Karma | Reference |
|---|---|---|
| burley_diffuse r 1, white | .7888 / .7375 / .6308, **identical to oren_nayar r 1** (max |Δ| = 0.0) | MaterialX Burley albedo (numeric): 1.426 / 1.116 / 1.035. MaterialX QON: .820 / .774 / .650 (Karma CPU QON differs, as M3 found) |
| oren_nayar energy_compensation r 1, (.8, .5, .2) | exactly (.800, .500, .200) at every μ | EON oracle (`albedo_ref.json` `pbr_eon_r10`): R .797/.782/.759, B .197/.184/.163 |
| layer(sheen zeltner r .5, white) | 1.0002 … 1.0001 | |
| sheen albedo used, from layer over ½ | .5946 / .2624 / .0279 | MaterialX Zeltner fit: .5943 / .2624 / .0279 |
| layer(sheen conty_kulla r .3, white) | 1.000 | (Karma's conty_kulla is an albedo-weighted Lambert, M3) |

### 4.6 EDFs, surface shader mix, glass

- `uniform_edf` (.3, .6, .9) and `multiply_edfC` are exact.
- `generalized_schlick_edf` (c0 1, c90 0): .309 / .886 / 1.000 against `1 − (1−μ)^5`'s .308 / .885 / 1.000.
- `conical_edf` (inner 30°, outer 60°) gives **1.0 at every μ**: Karma ignores the cone on surface emission.
- `mix_surfaceshader(Standard Surface copper metal r .3, OpenPBR defaults, .25)` equals `.25·SS + .75·OpenPBR`
  per pixel (mean residual −1e-5).
- Glass spheres (furnace, multi-bounce):
  - smooth `scatter_mode` RT gives 1.000;
  - smooth T-only gives 1.000, so Karma's T has no Fresnel loss (M3: `fresblend 2`);
  - rough (α .25) T-only gives .561 / .823 / .945;
  - rough RT gives .638 / .831 / .943, and **`layer(R, T)` gives identical pixels to the single RT node**.
- With Karma's default `internalreflectenable 0`, smooth RT gives only .96 at normal incidence. That is a probe pitfall
  (`docs/research/m3/test-strategy.md` 1).

Karma XPU does not support MaterialX pbrlib graphs ("Does not support MaterialX PBR nodes";
`docs/research/m3/surface-models.md` 0), so generic graphs are a Karma CPU feature.

---

## 5. Design proposal

### 5.1 Split of the work

Material translation (host) produces three things:
1. **IR outputs**: every leaf parameter (weight, colors, ior, α, film, ...), every mix amount, every multiply weight
   (clamped ≥ 0 in IR), every EDF parameter, and normals/tangents that are actually connected.
   - They go into a per-hit float block, exactly like today's `SurfaceInputs` fields (`OutputBinding{field, width, uniform, index}`,
     `core/shading/program.h:54-59`).
   - Constants and parameters are uniform outputs in the material's resolved template; varying ones are copied per lane
     (`material_eval.cpp:226-236`).
   - None of these depend on ωo, so the VM computes them vectorized as today.
2. A **closure program** (structure, part of the `Program` and its structure hash): the closure nodes in post-order, leaf kinds,
   uniform enums (scatter_mode, sheen mode, energy_compensation, thin_walled), and offsets into the block.
   - Value edits (mix amounts, colors, file paths) never touch it (K6).
3. The **closure builder** (`buildGraphClosure`, header-only and `NOVA_HOST_DEVICE`) runs per hit. It evaluates every
   factor that depends on ωo (leaf albedos, `1 − A_top`, Schlick EDF factors) and pushes the lobes.

The flattened form "weight = product of mix factors × Π(1 − A_upper)" is what the builder computes. Keeping the per-node
walk instead of expanding per-leaf products at compile time has three advantages:
- it handles shared subgraphs (UsdPreviewSurface's `transmission_mix`) without path enumeration;
- it keeps one closure node per MaterialX node, so the translator stays a direct mapping;
- it costs a few scalar operations per node.

### 5.2 Semantics (proposed; edge cases are Q1)

For each node, A (spectral) is what the node blocks from a base below it, at ωo; W (spectral) is its accumulated weight.

| Node | A (bottom-up, only nodes inside a layer top) | W propagation (top-down) |
|---|---|---|
| interface leaf (dielectric R, Schlick R, sheen) | `w·E_leaf(ωo)` (dielectric: untinted; Schlick and film: spectral) | lobe weight = `W·w·(leaf factors)` |
| opaque leaf (diffuse, conductor, translucent, subsurface) and T/RT leaves | `w` (coverage, spec) | as above |
| `mix(fg, bg, m)` | `lerp(A_bg, A_fg, m)` | `W·m → fg`, `W·(1−m) → bg` |
| `multiply(in, c)` | `c·A_in` | `W·c → in` |
| `add(a, b)` | `min(A_a + A_b, 1)` | `W → a`, `W → b` |
| `layer(top, base)` | `A_t + (1 − A_t)·A_b` | `W → top`, `W·(1 − A_top) → base` |
| `layer(T leaves, vdf)` | as top | marks the T leaves' medium: `sigmaA = absorption` |

These rules are linear. They match genglsl, Karma and OSL wherever those agree (all of the five uber graphs), and they match the
spec text and OSL testrender in the edge cases. They are energy-conserving whenever the leaves are, which `add` is not.

Leaf rules:
- **Dielectric T without R in the same leaf:** Walter BTDF with (1 − F) per microfacet, normalized by 1/E_T(μ) (E_T = E_RT − E_R).
  - Alone, it transmits everything, as Karma's does (4.6).
  - Under an R layer of the same η and α: R gets weight E_R/E_RT and T gets `(1 − E_R/E_RT)·f_T/E_T = f_T/E_RT`. That is
    exactly the native RT lobe (`pushTransmission`), so Standard Surface glass expanded generically equals the native glass.
  - `transmission_extra_roughness` also matches native, since its T normalization `(1 − shareR)/eT` is the same expression.
- **Sides.** Without refractive leaves (or with `thin_walled`), every frame faces ωo, as today. With refractive leaves:
  - frames of T/RT leaves follow the authored Ng;
  - on the back side, T/RT leaves become RT lobes with 1/η and 1/E_RTinv (the table exists). R leaves whose layer base
    contains refraction get W = 0 and A = 0; everything else is evaluated normally.
  - This reproduces native Standard Surface inside a fully transmissive object. With 0 < transmission < 1, native also keeps
    the specular layer on the opaque remainder, which the generic rule drops (documented deviation).
- **`thin_walled`** (from `surface`) turns T/RT leaves into `ThinDielectric`.
- **Emission:** `Σ` over the EDF tree with the same mix/multiply/add rules. `generalized_schlick_edf` multiplies its subtree by
  `mix(c0, c90, (1−μ)^e)`. Emission is **not** attenuated by BSDF layers (MaterialX; the uber graphs model coat attenuation
  explicitly).
- **Opacity:** the `surface` float input goes to the existing `opacity` field. `mix_surfaceshader` lerps opacities in IR, so
  the M3.11 opacity program and filter are unchanged.

### 5.3 Data structures (POD, fixed size, no pointers in per-hit data)

```cpp
// core/bsdf/closure_program.h
enum class CNodeOp : uint8_t { Leaf, Mix, Multiply, Add, Layer, LayerVdf };
enum class LeafKind : uint8_t { Lambert, Qon, Eon, Burley, Translucent, Subsurface,
                                DielR, DielT, DielRT, SchlickR, SchlickT, SchlickRT, Conductor,
                                SheenCharlie, SheenZeltner };
struct CNode {                 // 8 bytes, post-order (children before parents; root last)
    CNodeOp op;
    uint8_t kind;              // LeafKind (leaves)
    uint8_t flags;             // kNeedsAlbedo (inside a layer top), kOverRefraction, kHasFilm, kAnisotropic
    uint8_t frame;             // leaves: index into ClosureProgram::frames
    uint16_t a, b;             // children: Mix fg/bg, Layer top/base, Add in1/in2, Multiply in1
    uint16_t param;            // block offset: leaf parameter record, mix amount (1), multiply weight (3)
};
enum class ENodeOp : uint8_t { Uniform, Multiply, Mix, Add, Schlick };
struct ENode { ENodeOp op; uint8_t pad; uint16_t a, b, param; };
struct FrameSlots { uint16_t normal, tangent; };       // block offsets, 0xFFFF = geometric default
struct ClosureProgram {                                  // owned by the Program, uploaded with it
    static constexpr int kMaxNodes = 48, kMaxEdf = 16, kMaxFrames = 4, kMaxAlbedo = 16;
    uint8_t nodeCount, edfCount, frameCount, flags;      // flags: thinWalled, hasRefraction
    uint8_t albedoSlot[kMaxNodes];                       // compact A index for kNeedsAlbedo nodes
    CNode node[kMaxNodes]; ENode edf[kMaxEdf]; FrameSlots frames[kMaxFrames];
};
```

Each leaf kind has a fixed record layout at `param`:

| Kind | Fields | Floats |
|---|---|---|
| Diel* | w, tint[3], ior, α[2], filmThickness, filmIor | 9 |
| Conductor | w, n[3], k[3], α[2], film[2] | 11 |
| Schlick* | w, c0[3], c82[3], c90[3], exponent, α[2], film[2] | 15 |
| diffuse/sheen | w, color[3], roughness | 5 |
| Subsurface | w, color[3], radius[3], anisotropy | 8 |
| Translucent | w, color[3] | 4 |

- Frames are deduplicated by (normal ref, tangent ref) at compile time.
- A tangent is bound only for leaves whose α may be anisotropic (connected, or authored x ≠ y), as M3.10 does for the uber models.
- Standard Surface as a graph needs about 100 floats and OpenPBR about 150, so the block becomes `float v[256]`
  (the native layout stays the first 75 floats). Per lane only the program's `blockSize` floats are copied.

### 5.4 Builder (per hit; no recursion; the switch per node does not diverge because a warp shades one material)

```
for each leaf flagged kNeedsAlbedo:  E = leafAlbedo(kind, rec, frame(wo).mu)    // existing LUT helpers, spectral
for i in 0..n-1 (post-order):        A[slot(i)] = rule(node i)   if kNeedsAlbedo (section 5.2)
W[root] = weight of this segment; W[others] = 0
for i in n-1..0 (reverse post-order = parents first, valid for DAGs):
    Mix: W[fg] += W[i]*m;  W[bg] += W[i]*(1-m)      Multiply: W[in] += W[i]*c
    Add: W[a] += W[i]; W[b] += W[i]                   Layer: W[top] += W[i]; W[base] += W[i]*(1-A[top])
    Leaf: if maxComponent(W[i]) > 1e-6: pushLeaf(kind, rec, W[i])   // lobe weight, pick = lum(weight*E), cl.albedo
emission = Σ EDF tree;  finalize()
```

Per-hit scratch is `W[48]` + `A[16]` + `E[≤16]` spectra (about 1 KB). `kLayer` stays unused: albedo scaling needs no
random numbers (M3).

### 5.5 Translator changes (`hydra/material/translator.cpp`)

- `Value` gains closure kinds: `bsdf` (closure node index), `edf`, `vdf`, `surface` (a segment list).
  - `LowerNative` handles the pbrlib closure nodes; `LowerGraph` needs no change, since closures pass through interface outputs.
  - User nodegraphs, Lama, glTF and Disney therefore work through the existing expansion.
- `Run()` lowers the terminal like any node and accepts any `surface` value. This makes `surface`, `mix_surfaceshader`,
  `convert_*_surfaceshader` and `dot` work; `surfacematerial` is already followed.
  - The native models stay native and become one-segment values.
- Legacy rewrite: `layer(thin_film_bsdf, X)` copies thickness and ior onto every dielectric, conductor and Schlick leaf of X except
  `scatter_mode` "T", then replaces the layer by X (MaterialX's upgrade rule). Nova needs this because it does not call
  `upgradeVersion`.
- Warnings once per node type:
  - `retroreflective` true, `measured_edf`, `conical_edf` (rendered uniform), VDF scattering (M8), `chiang_hair_bsdf` (dropped);
  - `backsurfaceshader` (ignored), generalized-Schlick T/RT (approximate η).
- Compile-time overflow (> 48 closure nodes, > 16 albedo slots, > 4 distinct frames) is a translation error, so the next context or
  the default material renders (K15 policy). Extra frames could instead share frame 0 with a warning (Q-free implementation detail).

### 5.6 Lobe limit and overflow

- `ClosureList::kMaxLobes` = 16 and `frame[4]`; `Lobe::frame` stays a `uint8_t`.
- Runtime overflow happens when more than 16 leaves have non-negligible weight. Today `push` keeps the first lobes
  (`closure.h:182-192`); M4 should instead replace the lobe with the smallest pick estimate if the new one is larger.
  - The dropped energy is lost (biased). The material record counts overflows, and Nova warns once per material.
- Prior art:
  - Cycles: `MAX_CLOSURE` 64 (`kernel/types.h`); `closure_alloc` returns null when full, so later closures are dropped, and
    `CLOSURE_WEIGHT_CUTOFF` culls (`kernel/closure/alloc.h`).
  - OSL testrender: traversal stacks of 16.
  - Karma XPU: at most 16 blended uber shaders.
- The five uber graphs have at most 11 leaves, so overflow needs mixed surfaces with many active lobes.

### 5.7 `mix_surfaceshader` and two different models

A surface value is a list of at most 4 **segments** `{model, blockOffset, weightSlot, frameBase}`:
- `mix(fg, bg, m)` concatenates the lists and multiplies segment weights by m and 1 − m in IR.
- Native segments call the existing builders with an initial weight `w0` (now `Spectrum w = 1` at the top of each builder,
  `surface_models.h:264`) and a frame base. `clear()` and `finalize()` move to the caller.
- Graph segments run 5.4.

Mixed quantities:
- Emission and the albedo AOV add up with the segment weights.
- `sigmaA` comes from the segment with the largest transmission weight (documented approximation).
- Opacity is lerped in IR.

Karma verifies the linearity (4.6). Building both closures this way keeps native fidelity (depth, thin-walled, fast paths),
where expanding both models into graphs would lose it.

### 5.8 Cost and GPU notes

- Per leaf in a layer top: one LUT albedo lookup at ωo. Per lobe: today's push work. Per node: a few spectral multiply-adds.
- Estimate, not measured: Standard Surface as a graph builds in 1.5–2.5× the native 150 ns/hit.
  - It has no diffuse-only fast path.
  - It computes albedos for 3 tops plus picks, and evaluates 8 leaves even when their weights are 0. The VM still computes their parameters.
- All arrays are fixed and nothing recurses. The closure program is uniform per material, and a warp shades one material (K17), so
  the node loop does not diverge. The per-hit W/A scratch (about 1 KB) lives in CUDA local memory, like `ClosureList`.
- `nova_cuda_check` must include the new header (K14).

---

## 6. Native models vs the generic path

| | Keep native (recommended) | Express natives through the generic path |
|---|---|---|
| Speed | 150 ns/hit, diffuse-only single-lobe fast path (+10–13 %, M3.9), ≤ 8 lobes, zero-weight layers skipped before any evaluation | ~1.5–2.5× build (estimate); parameters of zero-weight leaves still computed by the VM |
| Fidelity | Has what the graphs lack or get wrong: SS `transmission_depth` (the graph has no VDF), SS `thin_walled` (the graph only switches subsurface), OpenPBR thin-walled SSS without the graph's C² bug (`surface-models.md` §3), inside-glass handling with 0 < X < 1, colored opacity as mean (the SS graph uses luminance, `standard_surface.mtlx:412-423`) | inherits those |
| Code | two paths sharing the leaf helpers (`leafAlbedo`, lobe setup) | one path |

Recommendation:
- Keep the native models and route `glTF PBR`, `disney_principled`, Lama and user graphs generically.
- Add a translator switch (`NOVA_MTLX_EXPAND=standard_surface,open_pbr_surface,UsdPreviewSurface` or a `CompileOptions` flag)
  that forces expansion for tests.
- After the refactor, both paths call the same leaf functions, so any difference points to a layering bug, not to lobe math.

---

## 7. Test strategy

### 7.1 Closure-level unit tests (no rendering noise)

- **Furnace closed forms** with `leafAlbedo` as oracle:
  - `layer(d, white)` gives 1 ± 1e-4 at every μ (Nova compensates consistently);
  - `layer(d, ρ)` gives `A_d + (1 − A_d)ρ`;
  - nested layers multiply;
  - `add` sums;
  - `mix` is linear;
  - the `multiply`/opaque-weight/`add`-in-top rules give the values chosen in Q1;
  - a DAG (UsdPreviewSurface skeleton) gives the sum over paths.
- **Per-hit weights:** VM-driven mix and weight vary across lanes; the builder must equal the lerp of the two endpoint closures per lane.
- **Mixture consistency:** random trees (up to 16 leaves, random combinators and parameters) through the chi² harness
  (`tests/bsdf_stats.h`): sample vs eval/pdf, a NaN hunt, and furnace ≤ 1 + 4σ for add-free trees.
- **Overflow:** 20 active leaves keep the 16 largest estimates; the warning comes once.

### 7.2 Equivalence: native vs generic expansion of the same nodegraph

For random parameter sets of Standard Surface, OpenPBR and UsdPreviewSurface, build the native `ClosureList` and the generic one
(forced expansion of `NG_standard_surface_surfaceshader_100` etc.) at random ωo. Then compare:
- `evalClosure(ωo, ωi).fcos` over random ωi, relative 1e-4. Compare the BSDF, not the lobe lists: RT vs R+T are equal functions.
- `cl.albedo` and `cl.emission`;
- for smooth lobes, the delta weights from `sampleClosure`.

Exclude the documented graph omissions, or test them as expected differences:
- `transmission_depth` > 0;
- `thin_walled` (SS);
- colored opacity;
- 0 < transmission < 1 seen from inside;
- thin film with transmission > 0 (native has no film on the refracting interface, `surface_models.h:167`);
- OpenPBR thin-walled subsurface;
- dispersion.

A husk check then renders the same: a `mtl_spheres`-like scene natively and with `NOVA_MTLX_EXPAND`, compared with `nova_imgcmp`
(statistically equal, same tolerance tier as noise).

### 7.3 Karma references (new scene `mtl_graphs`)

The scene holds layer/mix/add over Lambert, a per-pixel mix driven by a texture, sheen layers, thin film on a conductor,
`mix_surfaceshader`, and glTF and Disney spheres. Expected regions with reasons from section 4:
- uncompensated dielectric layers (−12…−20 %);
- the Schlick throughput gain (+11 %);
- the thin-film matrix;
- Burley rendered as QON;
- EON albedo;
- edge-case semantics (`LamaLayer`, multiply or add in a top), which are not compared with Karma; the analytic tests cover them.

`karma_albedo_probe.py` gains these graph probes, so a Houdini update re-measures them.

### 7.4 Performance gate

Record closure build ns/hit for Standard Surface native vs forced-generic in `docs/benchmarks.md`. Proposed gate: generic ≤ 3× native.
Native must stay within ±3 % of M3, which bounds the cost of the larger `ClosureList`.

---

## Recommended M4 scope (minimal)

1. `kMaxLobes` 16, `frame[4]`, replace-the-smallest overflow with a once-per-material warning.
2. Refactor the native builders: shared `leafAlbedo`/leaf setup, and `w0` plus frame base so they can run as segments.
3. `SurfaceModel::LobeList` (name from M3.md K1), `ClosureProgram`, and `buildGraphClosure`. Two passes (5.4) with the 5.2 rules
   for `layer`, `mix`, `add`, `multiply` (F, C) and `layer_vdf` (absorption only).
4. Leaves:
   - `oren_nayar` (Lambert, QON, EON), `translucent`, `subsurface` (Lambert fallback);
   - `dielectric` R/T/RT with film and `thin_walled`, `conductor` with film, `generalized_schlick` R with color82 (T/RT approximate);
   - `sheen` in both modes, and a new `Burley` lobe (per Q2).
5. EDFs: `uniform`, `mix`, `add`, `multiply`, `generalized_schlick_edf`; `conical` and `measured` render uniform with a warning.
6. Surface values in the translator: `surface`, `surfacematerial`, `mix_surfaceshader` (≤ 4 segments, native or graph),
   `convert`/`dot`, and the legacy `thin_film_bsdf` rewrite.
7. Tests 7.1–7.4, including the forced-expansion equivalence for Standard Surface, OpenPBR and UsdPreviewSurface, plus the `mtl_graphs` reference.

Not in the minimal scope:
- `retroreflective`, the conical and measured EDF shapes, VDF scattering (M8), Chiang hair and random-walk SSS (M9);
- `backsurfaceshader` and legacy `thin_surface`;
- exact generalized-Schlick transmission;
- a Karma-compat switch for the edge cases.

## Open questions for the user

- **Q1 Edge-case layering semantics.** Three patterns are at stake: `multiply` inside a layer top, `add` inside a layer top, and an
  opaque leaf with weight < 1 as a top. Options:
  - (a) spec-linear (5.2; = OSL testrender, energy-conserving, gives `LamaLayer`/`LamaGeneralizedSchlick` a sensible look);
  - (b) genglsl-literal (multiply leaves the throughput unchanged; opaque tops block fully);
  - (c) Karma (base scaled or duplicated; `LamaLayer` loses its base).

  Recommendation: (a). None of the five uber graphs is affected.
- **Q2 `burley_diffuse_bsdf`.** Implement MaterialX's Burley (albedo up to 1.43 at grazing for r 1, as defined), or render it as QON
  like Karma? Recommendation: MaterialX Burley (K2). Only Disney principled and user graphs use it.
- **Q3 `dielectric_bsdf` with `scatter_mode` T.** Normalize it to unit albedo, so that `layer(R, T)` equals the native RT lobe and
  matches Karma's energy (recommended)? Or keep genglsl's per-microfacet (1 − F) under the R layer's (1 − E_R), which
  attenuates twice?
- **Q4 `multiply` weights above 1.** Clamp at 0 only (Karma, consistent with native coat_color), or to [0, 1] like genglsl?
  Recommendation: ≥ 0 only.
- **Q5 Thin-film color matrix (also affects M3's native models).** Karma uses plain Rec.709 with a clamp; MaterialX uses CIE-RGB(E);
  Nova uses Rec.709 with rows normalized to grey. They differ by up to 0.1 absolute (4.4). Options: keep Nova's choice, switch to
  MaterialX's matrix (definition-exact, K2), or use Karma's (compat)?
- **Q6 `mix_surfaceshader` of native models in M4** through segments (recommended; Houdini users blend uber shaders, and Karma
  XPU supports up to 16)? Or only mixes of generic graphs?
- **Q7 Inside view of partially transmissive generic graphs.** Is it acceptable that the generic rule (5.2) drops R layers above
  refraction on the back side, while native Standard Surface keeps the specular layer on the opaque remainder?
