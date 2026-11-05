# M4 research: ray limits, fake-caustic shadows, deferred nodes, builder nodes

Scope: user decisions Q7 (fake-caustic shadows, M4, off by default) and Q8 (per-ray-type depth limits with the
Render Settings LOP) from `docs/design/M3.md` section 10; the node items M3 deferred to M4 (`docs/design/M3.md`
2.2); and a stock-node check of the Nova Material Builder extension nodes named in `docs/PLAN.md`. `kma_*` nodes
are covered by `karma-nodes.md` and are left out here.

**Method.** Houdini 22.0.466, Karma CPU (`BRAY_HdKarma`) driven by my own `husk.exe` processes in an isolated
environment (own `HOUDINI_USER_PREF_DIR`, packages and Nova skipped). Nova used the staged build
`build/vs2022/stage`, copied to the scratch directory first. Scenes are tiny hand-written `.usda` files: beauty
`C.*[LO]` as float32, Path Traced mode, uniform oracle, colour limit off. Most probes are deterministic (emissive
furnaces, mirrors at normal incidence), so a value like `1.5000` is exact to 4 digits. Pattern probes use Karma's
box filter at scale 1e-6 with 1 spp, which samples pixel centres. The scripts are in the session scratch directory
(`.../scratchpad/m4research/nodes-limits/`, not permanent); Appendix A gives enough detail to rebuild them.

**Rules followed.** I read SideFX VEX (`$HFS/houdini/vex/include/mxstd.h`) only for behaviour; this report
describes it in words and copies none of it. MaterialX (`$HFS/houdini/materialx/libraries`, Apache-2.0) is the
semantic reference.

---

## 0. Summary

1. Karma has **no global path depth**, only five per-type limits:
   `karma:object:{diffuse,reflect,refract,volume,sss}limit`. Defaults are 1/4/4/0/1. They are floats, picked
   stochastically per sample, and set per object. The Render Settings LOP authors them on the RenderSettings prim
   as defaults; the Render Geometry Settings LOP authors `primvars:karma:object:*` per prim.
2. A limit L allows L indirect rays of that type. The vertex that would exceed the limit still does full direct
   lighting (NEE plus the MIS BSDF ray). That over-limit ray sees **lights only** (rect, dome); it never sees
   emissive geometry and never shades its hit. Nova's `maxDepth` already matches this for lights (N = L + 1), but
   Nova also counts surface emission on the over-limit ray. Furnace: `nova:maxDepth 1` = 1.5, Karma
   `diffuselimit 0` = 1.0.
3. Counters are independent per type. The limit that applies is the one on the object where the scattering
   happens. Sheen counts as **diffuse**, coat and specular as reflect, (thin-walled) subsurface as sss.
4. `thinwallatrefractlimit` is **on** by default. After the refract limit, up to L more interfaces are crossed
   straight through, unbent, so a path can cross 2·L interfaces.
5. Karma's default `causticsenable = 0` is a property of the **caster**. It makes shadow rays **and** indirect
   diffuse rays pass refractive surfaces with a BSDF-derived factor, and skips glossy/specular reflection lobes
   at hits reached after a diffuse bounce. Per-hit factor: (1−F)² on front faces, (1−F) on back faces (Schlick),
   times `transmission_color` and transmission weight; roughness is ignored. A solid glass sphere's shadow is
   (1−F)³ = 0.885 at the centre.
6. MikkT: Karma does **not** compute MikkTSpace. With the input `karma_mikkt = 1` it reads `tangentu` and `signs`
   (written by the PolyFrame SOP, style MikkT):
   N′ = normalize(x·T + y·s·(N×T) + z·N), with `signs` defaulting to −1. Without `tangentu`, Karma uses its own
   uv tangent.
7. `flake2d/3d` in Karma is the MaterialX reference algorithm (4096/4096 pixels bit-exact). Nova needs 4D cell
   noise and a native `Flake` op.
8. `hextiledimage/normalmap` in Karma differ from MaterialX GLSL in three verified ways: the hash uses a
   sign-preserving fmod, the normal map rotates the tangent by −angle, and `strength` has no effect. No new op is
   strictly required.
9. String/filename `geompropvalueuniform` picks a texture file per prim, per face and per PointInstancer
   instance in Karma. Nova needs string primvars and a texture slot chosen per binding or per hit.
10. Stock equivalents: Ramp and Triplanar already lower today. AO is the legacy `ND_ambientocclusion_float`,
    measured in Karma as cosine-weighted within a cone, output = unoccluded fraction; it needs a ray-query op.
    UserData maps to geompropvalue. RaySwitch and Curvature exist only as `kma_*` nodes.

---

## 1. Topic A: per-ray-type depth limits

### 1.1 Karma's settings

Sources: `$HFS/houdini/RenderPropertiesKA.json` (209 Karma properties, no `pathdepth` or other global depth);
`soho/parameters/BRAY_HdKarma_Global.ds` (Render Settings LOP: `xn__karmaobjectdiffuselimit_*`);
`BRAY_HdKarma_Geometry.ds` (Render Geometry Settings: `xn__primvarskarmaobjectdiffuselimit_*`); help
`nodes.zip:lop/karmarenderproperties.txt`, "Limits".

| Property | Type | Default | Notes |
|---|---|---|---|
| `karma:object:diffuselimit` | float | 1 | "stochastically picked per-sample" (3.25: 25 % of samples use 4) |
| `karma:object:reflectlimit` | float | 4 | "even with 0 you still see specular reflections of light sources" |
| `karma:object:refractlimit` | float | 4 | counts surfaces, not objects; 0 still shows direct refraction of lights |
| `karma:object:volumelimit` / `ssslimit` | float | 0 / 1 | |
| `karma:object:thinwallatrefractlimit` | bool | **true** | surfaces act thin-walled once the limits are reached |
| `karma:object:limitcomponents` / `limitcolor` | string / color | `reflect refract` / (0,0,0) | colour where a limit cuts |
| `karma:global:refractcomponents` | string[] | `refract` | "uncategorized components are reflections" |
| `karma:global:russianroulette_cutoff` | int | 2 | (Nova `rrStartDepth` 3) |

UI range 0..32. All `karma:object:*` values on RenderSettings are defaults; `primvars:karma:object:*` on a prim
overrides them.

### 1.2 Measurements (Karma CPU, plus Nova where noted)

**A1. Diffuse.** Camera inside a closed Lambert box (ρ 0.5, emission E 1 on every wall); L = E·Σ_{k≤K} ρ^k is
exact because the radiance is uniform.

| | limit / maxDepth 0 | 1 | 2 | 3 |
|---|---|---|---|---|
| Karma `diffuselimit` | 1.0000 | 1.5000 | 1.7500 | 1.8750 |
| Nova `maxDepth` | – | 1.5000 | 1.7500 | 1.8750 |

Karma sees emissive geometry through exactly L diffuse bounces (K = L); Nova through N (K = N). Karma L thus
corresponds to Nova N = L + 1 for lights (the `tests/reference` rule) but to N = L for emission.

**A2. Reflect.** Two parallel mirrors (SS metal R 0.5, smooth, E 1), orthographic camera between them at normal
incidence. Karma `reflectlimit` 0/1/2/3 gives 1.0000/1.5000/1.7500/1.8750; Nova `maxDepth` 1/2/3 gives
1.5/1.75/1.875. Fractional: 0.5 gives 1.2500 and 1.25 gives 1.5625, exactly the stochastic mixtures.

**A3. Refract.** n parallel IOR 1.5 interfaces with alternating orientation (n/2 slabs), normal incidence, an
emissive backdrop or a rect light behind.

| n | `refractlimit` needed (emissive, thinwall off) | thinwall on | light backdrop, thinwall off | transmitted |
|---|---|---|---|---|
| 1 / 2 / 3 / 4 | 1 / 2 / 3 / 4 | 1 / 1 / 2 / 2 | n = 4: 3 (thinwall on: 2) | 0.9600 / 0.9600 / 0.9228 / 0.9228 |
| 5 / 6 / 8 | – / 6 / – | 3 / 3 / 4 | | 0.8875 / 0.8875 / 0.8538 |

Read off the table:
- Emission is seen through L refractions, lights through L + 1. With thinwall on, n ≤ 2·L, and the pass-through
  hits keep the normal transmission weight and still see emission. Below the limit the result is exactly 0
  (`limitcolor` is black).
- Side fact: a smooth SS slab transmits 0.96, not 0.96². Camera rays get a Fresnel loss only when entering (front
  face); the back-face exit transmits 1.0, and there is no 1/η² radiance scaling. Nova's BTDF applies 1/η² (it
  cancels on closed objects) and Fresnel on both sides.

**A4. What the limit vertex sees.** Orthographic camera on a 45° mirror; the reflected rays go up to three strips
facing down: a rect light, an emissive quad, a Lambert quad lit by a distant light. `diffuselimit` is 0.

| | rect light | emissive quad | lit Lambert |
|---|---|---|---|
| Karma smooth, `reflectlimit 0` | 0.9914 | **0.0000** | **0.0000** |
| Karma smooth, `reflectlimit 1` (and 4) | 0.9914 | 0.9913 | 0.2231 |
| Nova smooth, `maxDepth 1` / `2` | 0.9913 / 0.9913 | **0.9913** / 0.9913 | 0.0000 / 0.2231 |
| Karma roughness 0.2, `reflectlimit 0` / `1` | 0.8926 / 0.9195 | 0.0385 / 0.9347 | 0.0011 / 0.2349 |
| Nova roughness 0.2, `maxDepth 1` / `2` | 0.9192 / 0.9193 | 0.9266 / 0.9338 | 0.0337 / 0.2347 |

A dome seen through the smooth mirror at `reflectlimit 0` gives 0.9916, the same as at 1. The limit vertex keeps
full direct lighting (NEE plus the MIS BSDF ray, also for delta lobes). The over-limit ray only gathers lights
(crossed area lights, the dome on a miss); its surface hit is neither shaded nor emits. Once the emission
difference is removed, Karma L = Nova L + 1 in every column.

**A5. Independent counters, caustics off.** A lossless smooth mirror (F ≡ 1) inside the A1 furnace; a uniform
field stays uniform, so both views should read Σρ^k.

| Karma | mirror view | wall view | expected |
|---|---|---|---|
| d 2, r 4, `causticsenable 0` (default) | 1.5725 | 1.6739 | 1.75 |
| d 2, r 4, caustics on, `causticsroughnessclamp 0` | 1.7311 | 1.7487 | 1.75 |
| d 1, r 4, caustics on, clamp 0 | 1.4841 | 1.4998 | 1.5 |
| d 2, **r 0**, caustics off or on | 0.0000 | 1.6739 | |
| Nova `maxDepth 2` | 1.3932 | 1.7117 | |

With caustics on, a mirror inside the diffuse chain uses no diffuse budget, so counters are per type (the mirror
view's −1 % comes from Karma's metal Fresnel). With caustics off, diffuse rays that hit the mirror get nothing back
(wall view 1.6739, the same as r = 0). Coated walls (SS ρ .5 + specular 1, roughness .3) show that "glossy"
includes rough lobes: caustics off/on 1.5400/1.5750 (d = r = 1) and 1.7756/1.8520 (d = r = 2); roughness 0 gives
the same off value, 1.5400.

**A6. Per object.** The A4 mirror split in two halves, the left half authoring
`float primvars:karma:object:reflectlimit = 0` (constant). Global 4: left emissive 0.0000, right 0.9913. Global 0
with the left half authoring 4: left 0.9913, right 0.0000. The limit is read from the object where the scattering
happens, at that hit.

**A7. Lobe classes.** Furnace walls with one lobe each plus E 1, testing which limit unlocks bounces (`ssslimit` 0):

| Lobe | d0 r0 t0 | d2 | r2 | t2 | counts as |
|---|---|---|---|---|---|
| sheen 1 (base 0, spec 0) | 1.0000 | **1.1267** | 1.0000 | 1.0000 | **diffuse** |
| coat 1 (base 0, spec 0) | 0.9587 | 0.9587 | **0.9999** | 0.9587 | reflect |
| specular 1, roughness .2 | 1.0000 | 1.0000 | **1.0443** | 1.0000 | reflect |
| subsurface 1, thin-walled | 1.0000 | 1.0000 | 1.0000 | 1.0000 | sss (none of d/r/t) |

**A8. `limitcolor`** set to (1,0,0). Corridor at `reflectlimit 0` gives (2,1,1); at 1 it gives (2,1.5,1.5). In A4
at 0, the emissive and Lambert strips read (1,0,0). The colour is added at the cut vertex, multiplied by the path
throughput **before** that vertex's BSDF. Default black is a no-op.

### 1.3 Karma semantics, distilled

- **K1.** Every scattering event has one type: diffuse (Lambert, Oren-Nayar, sheen), reflect (specular, coat,
  metal, glossy), refract (transmission), sss, or volume. Each type has its own counter n_t.
- **K2.** At a vertex on object o, a sampled event of type t with new count c = n_t + 1:
  - c ≤ L_t(o): a **full** ray; its hit is shaded (emission, NEE, continuation);
  - otherwise a **light-only** ray: it gathers lights (crossed area lights, the environment on a miss), the path
    ends, and `limitcolor` is added if t is in `limitcomponents`.
- **K3.** A vertex reached by a full ray always does NEE, plus MIS of the BSDF sample against lights.
- **K4.** L_t(o) comes from the object at the scattering vertex (a `primvars:karma:object:*` primvar, otherwise
  the RenderSettings value). A fractional part is decided per sample.
- **K5.** `thinwallatrefractlimit`: refract events with L ≤ n_T < 2L become straight pass-throughs. They count,
  keep the transmission weight and are full rays.
- **K6.** Caustics (section 2) also cut paths: with `causticsenable 0` on an object, its glossy reflection lobes
  are not evaluated at hits reached after a diffuse bounce.

### 1.4 Nova today

`core/integrator/wavefront.cpp` loops `depth = 0..maxDepth`; `shadeHit` adds `cl.emission` before its
`lastIteration` return, and pass A gathers area lights and escapes for every ray. Nova therefore already behaves
like K2/K3 with one total counter, except that it **adds surface emission on the light-only ray** (A1, A4).
Also: there are no per-type counters; `Lobe::diffuse` is false for sheen (Charlie, Zeltner), while Karma counts
sheen as diffuse; and `ClosureSample` reports `delta`/`transmit`/`refract` but not the sampled lobe's class.

### 1.5 Proposal

Settings use Karma's pattern. Each is an object property: its value on the RenderSettings prim is the default and a
`primvars:` copy on a prim overrides it. The values are read once per mesh at sync into `MeshDesc`.

| Setting | Type | Default | M4 |
|---|---|---|---|
| `nova:object:diffuseLimit` | float | 1 (Karma), see Q-A | yes |
| `nova:object:reflectLimit` / `refractLimit` | float | 4 / 4 | yes |
| `nova:object:sssLimit` | float | 1 | counts Nova's subsurface fallback lobe until M9 |
| `nova:object:volumeLimit` | float | 0 | parsed; used from M8 |
| `nova:object:thinWallAtRefractLimit` | bool | true (Karma) | yes |
| `nova:object:fakeCaustics` | bool | false (Q7) | yes (section 2) |
| `nova:maxDepth` | int | total safety cap, e.g. 64 | keeps its name; no longer the look control |

Integrator changes, in Karma terms:

1. **Lobe classes.** Add `Lobe::eventClass` (D/R/T/SSS) next to `Lobe::diffuse` (which still drives the light
   multipliers and ray cones) and return it in `ClosureSample`. Diffuse: Lambert, QON, EON, Charlie, Zeltner;
   Reflect: Conductor and the reflection part of Dielectric/ThinDielectric (coat included); Refract: their
   transmission part; SSS: the SSS fallback lobe. Translucent is assumed diffuse (not measured).
2. **Per-path state** in `WaveQueues` (SoA): `uint8 count[3]` (plus a 4th for SSS), `uint8 flags` (lightOnly,
   afterDiffuse), and one `float` limit-dither u drawn at camera-ray generation from a new Sobol stream. The
   integer limit is `floor(L) + (u < frac(L))` with the hit object's L, so per-object and fractional limits need
   no further state.
3. **Continuation** in `shadeHit`: apply K2/K5 with the hit mesh's limits. A light-only ray carries a `lightOnly`
   flag; in pass A it gathers area lights and the environment as today but is never queued for shading. This
   removes the extra surface emission and makes Karma limit = Nova limit (the same numbers). Thin-wall
   pass-through keeps the sampled weight, sets wi = −wo, starts from the far side and leaves the medium state.
4. The loop ends when no rays are left, or at `maxDepth` as a safety cap. RR is unchanged.
5. **Reference scenes** (`tests/reference/README.md`, "Scene rules"): replace "`maxDepth N` + `diffuselimit N−1`"
   with equal per-type numbers; set `karma:object:causticsenable = 1`, `karma:object:causticsroughnessclamp = 0`
   and the same `thinwallatrefractlimit` in Karma (Nova's default is true caustics); regenerate the affected
   references.
6. **Tests**: the probes above as Nova-vs-analytic unit tests, all deterministic with no Karma reference needed:
   A1/A2 (exact Σρ^k per limit, including fractional limits), A3 (stack counts with and without thinwall), A4
   (light-only ray rules), A6 (per-object override), A7 (sheen counts as diffuse). Add one Karma-referenced scene
   (mirror corridor plus glass stack) to `tests/reference`.

Not proposed for M4: `limitcolor`/`limitcomponents` (default black is a no-op), `causticsroughnessclamp`,
`refractcomponents`.

---

## 2. Topic B: fake-caustic (transparent) shadows

### 2.1 Karma's settings (`RenderPropertiesKA.json`, category Shading)

| Property | Default | Help text (abridged) |
|---|---|---|
| `karma:object:causticsenable` | false | "Brute-force caustics from transmissive objects. Allows evaluation of glossy BSDF that's seen by indirect diffuse bounce." |
| `karma:object:causticsroughnessclamp` | 0.2 | minimum roughness for true caustics (GGX only) |
| `karma:object:fakecausticsbsdfenable` | true | "a red bottle will automatically cast red shadows"; off means use Color |
| `karma:object:fakecausticscolor` / `fakecausticsopacity` | (1,1,1) / (1,1,1) | tint or replace the BSDF result / lighten it |
| `karma:object:fakecausticsfresneldarken` | 0 | 0 off, 1 fully dark |
| `inputs:karma:light:contributescaustics` | true | caustics need it on both the light and the object |

### 2.2 Measurements

**B1. Glass sphere shadow.** An exact sphere (a Points prim, r 0.5, centre y 1, invisible to camera rays through
`primvars:karma:object:rendervisibility = "-primary"`) over a Lambert floor, a 0.1 × 0.1 rect light at y 4, an
orthographic camera looking down; values are shadow/unshadowed ratios. Glass: SS `base 0, specular 1, IOR 1.5,
transmission 1, roughness 0`. A straight shadow ray through the sphere crosses two interfaces at the same angle θ
(cos θ = √(1−b²), b = impact parameter / R).

| b bin | Karma, default | (1−F_Schlick)³ | (1−F_exact)³ | (1−F_exact)² |
|---|---|---|---|---|
| 0.0–0.5 | 0.8847 | 0.8847 | 0.8821–0.8847 | 0.9197–0.9216 |
| 0.6–0.7 | 0.8823 | 0.8824 | 0.8674 | 0.9095 |
| 0.7–0.8 | 0.8708 | 0.8714 | 0.8415 | 0.8913 |
| 0.85–0.9 | 0.7847 | 0.7894 | 0.7410 | 0.8188 |

Bins above 0.9 are blurred by the light size. The default fake-caustic shadow is (1−F_s(θ))³ with Schlick F0 0.04.
Variants, values at the shadow centre:

| Variant | Centre | Reading |
|---|---|---|
| `transmission_color` (1, .5, .25) | (0.8847, 0.2212, 0.0553) | × colour² (once per crossing) |
| `transmission` 0.5 | 0.2212 | × 0.5² (weight per crossing) |
| roughness 0.3 | 0.880–0.886 (flat) | roughness is ignored |
| `thin_walled` | 0.8493 = 0.96⁴ | (1−F)² per crossing |
| `fakecausticsbsdfenable 0`, colour .5 | 0.2500 (flat) | colour per crossing, no Fresnel; colour 1 casts **no shadow** |
| `fakecausticscolor` .5 (BSDF on) | 0.2212 | multiplies the BSDF factor per crossing |
| `fakecausticsopacity` .5 | 0.9416 | per crossing lerp(1, t, opacity) |
| `fakecausticsfresneldarken` 1 / .5 | 0 / 0.874 at the centre falling to 0.197 at the rim | angle-dependent darkening; 1 gives an opaque shadow |
| `causticsenable 1` (d 1) | 16.5 (focus); 0.70 / 0.23 / 0.08 towards the rim; outside 1.035 | true shadow plus refracted and reflected caustics |
| SS metal / SS `opacity` .5 | 0 / 0.2500 | opaque / cut-out per crossing (Nova's filter does the same) |

**B2. Per-hit factor.** Glass planes over the floor under a vertical distant light, so both shadow-ray crossings
are at normal incidence:

| Configuration | Ratio |
|---|---|
| one plane, ray hits its back face / front face | 0.9600 (1−F) / 0.9216 (1−F)² |
| slab (front + back, either order) | 0.8847 |
| front face thin-walled; OpenPBR `transmission_weight 1` front face | 0.9216; 0.9216 |
| IOR 1.33: front face / slab | 0.9616 ≈ (1−F0)(1−0.0187) / 0.9423 (back = 0.9799 = 1−F0) |

The Karma factor at a hit is the transmitted energy along the straight line: on the front face the specular-layer
loss times the interface transmission, on the back face the interface transmission only.

**B3. Indirect diffuse through glass, caustics off.** A floor lit only by a large emissive ceiling (no lights, so
no shadow rays), with a glass pane over half of it. Under-pane/open ratio: caustics off 0.931 (roughness 0.3:
0.933); caustics on (true refraction) 0.964 (0.965). Diffuse-bounce rays pass through refractive objects with the
fake factor; A5 showed that reflection lobes are dropped after a diffuse bounce.

**B4. Which object holds the switch.** `primvars:karma:object:causticsenable = 1` on the glass gives true caustics,
identical to the global switch; on the floor it gives the default fake shadow. The flag belongs to the **caster**.

### 2.3 Karma semantics, distilled

On an object with `causticsenable = 0` (the default):
- **F1. Shadow rays** from any vertex do not stop at it. Each crossing multiplies by the factor (front (1−F)², back
  (1−F), Schlick, roughness ignored) times `transmission_color`, transmission weight, `fakecausticscolor` and the
  opacity lerp.
- **F2. Rays after a diffuse bounce** that hit it: reflection lobes (glossy and specular, any roughness) are
  skipped, transmission continues straight on with the same factor, and diffuse lobes behave normally. Because the
  factor is the same as for shadow rays, MIS between NEE and BSDF sampling stays consistent.
- **F3.** Camera rays, and rays after reflect or refract events only, see the object normally.

With `causticsenable = 1` the object blocks shadow rays, and refracts and reflects for diffuse rays too, with
roughness clamped to `causticsroughnessclamp`.

### 2.4 Proposal for Nova

`nova:object:fakeCaustics` (default false, Q7), read on the caster like Karma's switch. Nova's default stays
physically complete (shadow rays blocked, caustics from BSDF paths); with the switch on, F1 + F2 apply.
**Shadow-only transparency is not enough**: without F2, NEE through the glass and the BSDF path refracting to the
same light would both be counted, which is biased and too bright.

1. **Factor.** A closure query `transmittance(cl, wo = −dir)` = the sum of the transmissive lobes'
   `weight × (1 − F(μ))` (ThinDielectric: slab transmission; roughness ignored; tint and transmission weight
   already sit in the lobe weights). This physical form gives (1−F)² for a closed sphere. Karma's adds one more
   (1−F) on front faces, (1−F)³, which is 4 % darker at normal incidence; that choice belongs to the Q2
   Karma-compat decision.
2. **Shadow stage** (`traceShadowRays`), with a scene flag `anyFakeCaustics` and a per-mesh flag set at commit:
   - Phase 1, unchanged `occluded()`: the hit filter rejects fake-caustic meshes (they are marked filtered), so
     only opaque blockers count. This fast path is the only one in scenes without such materials.
   - Phase 2, only for unoccluded rays in such scenes: iterate closest-hit queries restricted to fake meshes (a
     filter mode in `RayBatch`, or Embree ray masks if the vcpkg build enables them), up to 8 crossings. Evaluate
     each crossing's material in chunks per material (the `evaluateSurfaceInputs` + `buildClosure` path of pass C)
     and multiply the contribution. The same stage structure maps to OptiX (any-hit program plus a loop).
3. **Path rays.** At a hit on a fake-caustic object with `afterDiffuse` set (set when a Diffuse-class or SSS event
   is sampled), filter the closure: drop the non-diffuse reflection lobes, keep the diffuse lobes, and replace the
   transmission lobes with one **null lobe** (direction −wo, weight = factor / pick, no event counted, `bsdfPdf`
   and `diffuseShare` kept for MIS).
4. **Out of M4:** Karma's extra knobs (`fakecaustics{color,opacity,fresneldarken,bsdfenable}`),
   `causticsroughnessclamp`, per-light `contributescaustics`.
5. **Tests:** the B2 planes against analytic (1−F) products (deterministic: both orientations, tint, weight); the
   B1 sphere profile against (1−F)² per bin; an MIS check (a fake-caustic floor scene rendered NEE-only and
   BSDF-only must agree, `nova:lightStrategy`); one Karma-referenced scene (Nova `fakeCaustics` on, Karma default)
   with a documented region tolerance for the (1−F) difference.

---

## 3. Topic C: nodes deferred from M3

### 3.1 MikkTSpace tangents (`karma_mikkt`)

**Where it appears.** Karma adds an integer input `karma_mikkt` (default 0) to `ND_normalmap_float`,
`ND_normalmap_vector2`, `ND_bump_vector3`, `ND_gltf_normalmap_vector3_1_0`, `ND_UsdPreviewSurface_surfaceshader`
and `ND_hmtlxpbrtextureset` (`karmaShaderNodes.json`; `python3.13libs/mtlx2karma.py` `_addNormalMapInputs`, which
also covers any nodegraph that contains a normal map). It is not a MaterialX nodedef; users add it with the recipe
"Karma MikkT Normal Map" (help `vop/mtlxnormalmap.txt`, "MikkT normal maps"). The help says Karma then uses the
`tangentu` and `signs` primvars, and without `tangentu` the map is a "Karma-specific tangent space normal map"
with `signs` defaulting to −1. The **PolyFrame SOP**, style `mikkt`, writes vertex `N` and `tangentu` (vector3);
its `signson` is **off** by default (checked with hython). Karma does **not** generate MikkTSpace itself.

**Measured.** Quad facing +z, unlit `ND_surface_unlit` showing the normal map output, constant input
v = 2·in − 1.
- With `karma_mikkt 1`: **N′ = normalize(v.x·T + v.y·s·(N × T) + v.z·N)**.
  - T is the **unnormalized, un-orthogonalized** `tangentu`: T = (2,0,1) with v = (1,0,0) gives (0.894, 0, 0.447),
    and v = (.5,.5,1) gives (0.4851, ±0.4851, 0.7276).
  - s is `signs` (−1 if missing); `scale` multiplies v.xy (v = (.5,.5,1) at scale 2 gives (1,−1,1)/√3).
- With `karma_mikkt 1` but no `tangentu` loaded: T is Karma's uv tangent and s = −1, so only the bitangent flips
  (v = (0,1,0) gives (0,−1,0), against (0,1,0) with mikkt 0).
- **Karma gotcha:** `tangentu`/`signs` were honoured only when the material also read them through
  `geompropvalue`; as plain primvars (constant, faceVarying or vertex; `st` or `uv`) they were ignored.

**Nova.**
- Lower the `karma_mikkt` input on the six nodedefs above. When it is non-zero, add `tangentu` (vector3) and
  `signs` (float) to the material's derived primvar list and build the formula with existing ops (Cross, Mul,
  Add, Normalize); keep Nova's 1.001·Ng guard as for `normalmap`.
- **New IR need:** a varying fallback when the primvar is missing (`KarmaT()`, s = −1). Either a per-binding
  "primvar present" constant (an `AttributeRef` flag) plus `Select`, or let `Primvar` take a non-uniform fallback.
- **Test:** the probe as `test_hydra_materials` cases (T = (2,0,1), (0,1,0), (1,1,0); s = ±1, missing; scale 2),
  checked against the values listed, which Karma rendered.

### 3.2 `hextiledimage` / `hextilednormalmap`

**Definitions** (`stdlib_defs.mtlx` lines 312–342 and 2889–2907). Inputs: `file`, `default`, `texcoord` (UV0),
`tiling`, `rotation` 1, `rotationrange` (0,360)°, `scale` 1, `scalerange` (.5,2), `offset` 1, `offsetrange`
(0,1), `falloff` .5; image only `falloffcontrast` .5 and `lumacoeffs` (ACEScg); normal map only `strength` 1,
`flip_g`, `normal`/`tangent`/`bitangent`. Reference: genglsl `mx_hextiledimage.glsl`,
`mx_hextilednormalmap.glsl`, `lib/mx_hextile.glsl`, `lib/mx_geometry.glsl` (Mikkelsen, JCGT 11(2) 2022).

**Algorithm (MaterialX):**
1. coord = texcoord·tiling, scaled by 2√3 and skewed into a triangle grid. The barycentric weights w1..w3 and the
   three vertex ids come from floor/fract and the triangle side s = step(0, −z).
2. Each vertex gets a tile centre (inverse skew) and two random numbers r from a hash of (id + 0.12345):
   rotation = mix(range)·r.x·rotation; scale = mix(1, mix(range, r.y), scale); offset = mix(range, r·offset).
3. Per tile, the coordinate is rotate(coord − centre)/scale + centre + offset; derivatives are rotated and scaled
   the same way.
4. Three lookups are blended with w_i ∝ lumaWeight_i·bary_i⁷, normalized, then Schlick gain if falloff ≠ 0.5.
   lumaWeight = mix(1, dot(c, lumacoeffs), falloffcontrast); alpha uses weights without luma.
5. Normal map: each sample is decoded, the frame rotated about N by the tile's angle, and the three normals
   blended with Mikkelsen's surface-gradient blend.

**Karma** (behaviour read from `mxstd.h`; points 1 and 2 confirmed black-box):
1. The hash uses a **sign-preserving fmod** (x − trunc x) instead of `fract`. Probe: a smooth periodic EXR (sin/cos
   of 2πu, 2πv), st in [−1, 1], falloffcontrast 0, compared per pixel against both models. Trunc model: median
   |Δ| 1e-4 everywhere (texture filtering). GLSL fract model: wrong in 18 % of pixels where st ≥ 0 and 99.8 % where
   st < 0 (any negative skewed id).
2. The normal map rotates T and B about N by **−angle** (GLSL: +angle), then orthonormalizes them against N, so
   **`strength` has no effect**. Probe with a constant map v = (.6, 0, .8): the "−angle, normalized T/B" model
   matches with median 9e-5 at strength 1 and 0.5; the +angle model is off by 0.82.
3. Lookups: periodic wrap, `raw` for normal maps, no v-flip, empty file gives `default`. The footprint is the
   coord derivatives × avgScale, avgScale = mix(1, mean(scalerange), scale)·0.5, **not** rotated per tile (read
   only, not measured).
4. Weights and gain are as in MaterialX, including the luma weights and separate alpha weights.

**Nova.** No mandatory new op: Floor, Trunc, Fract, Sin/Cos, Pow, Mix, Select, Dot, Cross and Normalize exist, plus
three `Tex2D` with explicit footprint operands. One node costs roughly 150–250 IR rows (estimate). If that breaks
the program row cap, add a native `HexTile` op (a = coord, imm = tile k) returning (u′, v′, angle, weight).
Rodrigues rotation and the gradient blend are plain IR. **Test:** both probes as Karma-referenced deterministic
tiles; they can join `mtl_nodes` (point filter, per-pixel tolerance) with the two generated probe EXRs.

### 3.3 `flake2d` / `flake3d`

**Definitions** (`stdlib_defs.mtlx` 1244–1274). Inputs: `size` .01, `roughness` .1, `coverage` .5, `texcoord`
(UV0) or `position` (Pobject), N/T/B (world). Outputs: `id` (integer), `rand`, `presence`, `flakenormal`.
Reference: genglsl `mx_flake2d/3d.glsl`, `lib/mx_flake.glsl` (Arvo random rotations); flake2d is flake3d at
(u, v, 0).

**Algorithm:**
1. P = position/size; a fitted rational curve maps `clamp(coverage)` to a probability.
2. Look at the 3×3×3 cells around floor(P). Skip a cell if |P − cell − 0.5|² ≥ 3d² (d = 1.5/√3) or if
   cellnoise(cell) > probability.
3. The cell's priority is cellnoise(cell, 3) (**4D**). Keep the highest priority whose flake (a cube of half-size d
   rotated by Arvo's matrix from cellnoise_vec3(cell)) contains P.
4. No flake: id 0, rand 0, presence 0, normal N.
5. Flake: n = cellnoise_vec3(cell, 2) (**4D**); rand = n.z, id = int(n.z·16777215), presence = priority; the
   normal is a GGX sample (tanθ = roughness²·√ξ1/√(1−ξ1), φ = 2πξ0) in the frame (T̂, B̂, N̂).

**Karma = MaterialX exactly.** A Python port of the GLSL (Jenkins bjmix/bjfinal 3D/4D cell noise, float32
`bits_to_01`) matched Karma's `flake2d` (size .05, coverage .5, 64², pixel centres) in **presence, rand and id for
4096/4096 pixels**. Karma normalizes N/T/B (GLSL does not for the no-flake output); irrelevant for unit inputs.

**Nova.** Add `cell4(x,y,z,w)` (len-4 hash: bjmix, then +w, then bjfinal) and `cellV3_4` (len-5 seed, b + 0/1/2)
to `core/shading/noise.h`, bit-exact like the existing cell noise. Add a native **`Flake` op** (a = position,
3-wide; b = (size, coverage); result (priority, ξ0, ξ1, rand), width 4), since a 27-cell loop with early outs is
too large for IR. In IR: id = floor(rand·16777215) (exact in float32), presence = priority, the GGX normal, and
`Select` on presence > 0. **Test:** the probe as a `mtl_nodes`-style deterministic Karma reference (including
`flakenormal` as a colour), plus a `[noise]` unit test of the 4D hashes against the same Python port.

### 3.4 String `geompropvalueuniform`

**Definitions** (`stdlib_defs.mtlx` 1580–1589): `ND_geompropvalueuniform_string` and `_filename`, uniform
`geomprop`, `default` and `out`. Houdini VOPs: `mtlxgeompropvalueuniform`, `mtlxUsdPrimvarReader`,
`usdprimvarreader`. A string output can only feed string/filename inputs, so the real use is a **texture file per
object** (the image `file` input); other string inputs (address modes, filter type) are uniform enumerations,
rare.

**Karma** (one material, `image.file` ← `geompropvalueuniform_filename("texpath")`):

| Primvar | Result |
|---|---|
| constant `string` / constant `asset` | correct file / correct file |
| `uniform` (per face) `string[]` on a 2-face mesh | per face |
| PointInstancer `string[]`, `vertex` interpolation (per instance) | per instance |
| missing | `default` |

Karma resolves it per hit, so it is not "uniform per material".

**Nova.** Read string and asset primvars (constant, uniform, instance-rate) in `hydra/primvarUtil`. At binding time
(bindings per (mesh, material) already exist), collect the distinct values, register one texture slot per value
in the binding, and store a numeric **slot-index attribute** with the same interpolation. `Tex2D` takes the slot
from an operand: a = (u, v, slot), 3-wide, or a new `Tex2DIndexed`. `UsdPrimvarReader_string` uses the same path;
non-filename consumers are specialized per binding at bind time, or warn. **Test:** the probe (four quads, a
3-instance instancer, a 2-face mesh) against Karma; deterministic (unlit constant textures), exact equality.

---

## 4. Topic D: Nova Material Builder extension nodes vs. stock nodes

Houdini 22 VOP types listed with my own hython: `mtlxramp`, `mtlxramp4`, `mtlxramplr`, `mtlxramptb`,
`mtlxramp_gradient`, `hmtlxrampc/f`, `hmtlxcubicrampc/f`, `hmtlxhuniformramp`, `mtlxtriplanarprojection`,
`mtlxambientocclusion`, `mtlxgeompropvalue(uniform)`, `mtlxUsdPrimvarReader`, `usdprimvarreader`,
`mtlxflake2d/3d`, `mtlxhextiled*`, and `kma_curvature`, `kma_rayswitch`, `kma_rampconst`, `kma_hextiled_*`,
`kma_roundededge`, `kma_tangentrotate`.

| PLAN.md node | Stock MaterialX / Houdini equivalent | Nova status | New ops needed |
|---|---|---|---|
| Ramp | `ND_ramplr/ramptb/ramp4_*`, `ND_ramp` + `ND_ramp_gradient` (1.39, up to 10 keys, linear/smooth/step), Houdini `huniformramp`/`hcatmullrom`/`huniformcubic` (behind the `hmtlxramp*`/`hmtlxcubicramp*` VOPs) | all lower via nodegraphs (M3.7 coverage test) | none; a Nova Ramp can be an HDA exporting `ND_ramp` or the Houdini ramp graph |
| Triplanar | `ND_triplanarprojection_*` (stdlib nodegraph: three `image` lookups + blend) | lowers today (M3.8) | none; hex-tiled triplanar only as `kma_hextiled_triplanar` (other report) |
| AO | `ND_ambientocclusion_float` (`coneangle` 90°, `maxdistance` 1e38): rolled back to a proposal in 1.39.2 but defined in `libraries/houdini/legacy_1_39.mtlx` and **supported by Karma** (`karmaShaderNodes.json`; VOP `mtlxambientocclusion`) | lowers to 1 with a warning (M3.md deviation 12) | `Occlusion` op, rays from the shading VM (below) |
| Curvature | none in MaterialX; `kma_curvature` | – | per Karma semantics (`karma-nodes.md`); Nova's `Scene::curvature()` (ray cones) is a possible geometric source |
| RaySwitch | none in MaterialX; `kma_rayswitch` | – | `Global::RayType` from path state: camera / diffuse / reflect / refract (the section 1.5 counters) plus shadow for opacity and fake-caustic programs |
| UserData lookup | `ND_geompropvalue_*` (numeric, incl. instance primvars), `ND_geompropvalueuniform_string/filename`, `ND_UsdPrimvarReader_*` (incl. `_string`) | numeric done (M3.5); string/filename open | string primvars + slot indexing (3.4) |

**Karma's AO, measured.** A floor under an infinite ceiling at height 1; a direction at θ from N is occluded within
`maxdistance` r iff cos θ > 1/r.

| coneangle, maxdistance | Karma out | cosine-weighted model | uniform model |
|---|---|---|---|
| 90°, 1.5 / 2 / 4 / 1e38 | 0.4444 / 0.2500 / 0.0625 / 0 | 1/r²: 0.4444 / 0.25 / 0.0625 / 0 | 0.667 / 0.5 / 0.25 |
| 45°, 1.2 | 0.3889 | (sin²α − (1 − 1/r²))/sin²α = 0.3889 | |
| 60°, 1.5 / 30°, 1.1 / 45°, 2 | 0.2592 / 0.3057 / 0 | 0.2593 / 0.3058 / 0 | |

`out` is the **unoccluded** fraction of cosine-weighted directions inside a cone of half-angle `coneangle` about the
shading normal, counting hits up to `maxdistance`. **Nova op** `Occlusion(a = (coneangle, maxdistance))`: the VM
draws one cosine-weighted cone direction per lane from a new Sobol stream and calls `IAccel::occluded` on a small
per-chunk batch. That is unbiased, and pixel samples converge it. The VM context then needs the accelerator, the
offset hit origin and the time. On the GPU it is an OptiX trace from the shading kernel, or a split stage.

---

## 5. Recommended M4 scope (minimal)

1. **Per-type limits** (1.5): `nova:object:{diffuse,reflect,refract,sss}Limit` + `thinWallAtRefractLimit` as
   RenderSettings defaults with `primvars:` overrides; per-path counters, a light-only ray flag and the lobe event
   class (sheen counts as diffuse); `maxDepth` becomes a safety cap; surface emission is no longer gathered by
   over-limit rays; `volumeLimit` parsed only (M8). Analytic tests A1–A7; migrate the reference scenes to equal
   limits plus Karma caustics on (clamp 0).
2. **Fake caustics** (2.4): `nova:object:fakeCaustics`, default off; two-phase shadow stage, the closure
   `transmittance()` query, null-lobe pass-through after diffuse with reflection lobes dropped; planes, sphere and
   MIS tests.
3. **MikkT** (3.1): the `karma_mikkt` input on the six nodedefs, derived `tangentu`/`signs` primvars, the varying
   primvar fallback.
4. **Flake** (3.3): 4D cell noise and the native `Flake` op.
5. **Hex tiling** (3.2): IR lowering with Karma's trunc-fmod hash, −angle tangent rotation and ignored strength; a
   native `HexTile` op only if the row cap is hit.
6. **geompropvalueuniform** (3.4): string/asset primvars and per-binding texture slot indexing.
7. **AO** (section 4): the `Occlusion` op for `ND_ambientocclusion_float`; cheap once the VM has accel access, and
   the base for `kma_ao` if that report wants it.

Builder extensions (D) need no new ops beyond 7 and `Global::RayType` (for `kma_rayswitch`); Ramp and Triplanar
are HDA wrappers around nodes that already lower.

Not in M4: `limitcolor`, `causticsroughnessclamp`, Karma's `fakecaustics*` artist knobs, Nova-side MikkTSpace
generation (Karma does not generate it either), a Karma-compat fake-caustic factor (Q2).

## 6. Open questions for the user

- **Q-A: default limits.** Use Karma's defaults (diffuse 1, reflect 4, refract 4, sss 1; Arnold's are similar),
  or keep a deeper default? Nova today is `maxDepth 6`, about 5 diffuse bounces in Karma terms.
  *Recommendation:* Karma's defaults, so that switching renderers in Solaris does not change the look.
- **Q-B: honour Karma's own settings?** Should Nova read `karma:object:*limit`, `thinwallatrefractlimit` and
  `causticsenable` (RenderSettings and `primvars:`) as fallbacks when no `nova:` value is authored?
  *Recommendation:* no, keep explicit `nova:` names; the Nova Render Settings LOP can offer a "match Karma"
  preset instead.

---

## Appendix A: probe scenes (to rebuild the measurements)

All probes: `husk --renderer BRAY_HdKarma --headlight none -p <spp> --lock-random 1`; RenderSettings with
`convergence_mode "Path Traced"`, `pixeloracle uniform`, `colorlimit 1e9`, `constrainmaxrough 0`, box filter,
float32 `C.*[LO]`; explicit quads (`doubleSided 1`, `subdivisionScheme none`, `st`);
SS = `ND_standard_surface_surfaceshader`.

| Id | Scene |
|---|---|
| A1 | Camera at the centre of a closed box [−1,1]³, 90° fov; walls SS base_color .5, specular 0, emission 1. Limits d ∈ 0..3 (r 10). Nova: `nova:maxDepth`. |
| A2 | Mirrors at z = ±1 (10×10, normals inward), SS metal base_color .5, specular_color 1, roughness 0, emission 1; orthographic camera at the origin looking −z, 1×1. |
| A3 | n quads at z = −0.1·i with alternating normals ±z, SS base 0, specular 1, IOR 1.5, transmission 1, roughness 0; emissive quad (or 10×10 rect light, normalize 0) at z = −3; orthographic camera at z = 5. |
| A4 | Orthographic camera (3×1) at z = 5 on a mirror at the origin, normal (0,.707,.707). At y = 3 facing down: rect light 1×2 at x = −1, emissive quad at x = 0, Lambert quad at x = 1; distant light rotateX 135°, `normalize 1`. A6 splits the mirror at x = 0 and adds `primvars:karma:object:reflectlimit`. |
| A5 | A1 box plus a smooth metal quad (base_color 1, specular_color 1) at z = −0.5, x ∈ [−1,0]; camera at z = 0.5. A5b: walls SS base .5, specular 1, roughness .3 / 0. |
| A7 | A1 box with single-lobe walls (sheen, coat, specular .2, thin-walled subsurface), each with emission 1. |
| B1 | Floor 8×8, SS base .5, specular 0; Points sphere at (0,1,0), width 1, `rendervisibility "-primary"`; rect light 0.1×0.1 at y = 4, intensity 1000, normalize 0; orthographic camera 3×3 looking down, 150²; divided by the same render with the sphere moved away (256 spp). |
| B2 | Floor plus glass quads (3×4) at y = 1 / 1.1 over x < 0; distant light straight down; shadow region x ∈ [−1.6,−0.4] over lit region. |
| B3 | Floor plus an emissive ceiling 80×80 at y = 2 (invisible to camera), glass pane over x < 0 at y = 1, diffuselimit 1, no lights. |
| C-mikkt | Unit quad facing +z; `ND_surface_unlit` ← convert ← `ND_normalmap_float` (constant `in`, `karma_mikkt`); faceVarying `primvars:tangentu` (vector3f[]) and `signs` (float[]); the material must also read them through `geompropvalue`. |
| C-flake | Unit quad, st ∈ [0,1]; unlit ← combine3(presence, rand, float(id)); `ND_flake2d` size .05, coverage .5; 64², pixel filter scale 1e-6, 1 spp. |
| C-hex | Quad with st ∈ [−1,1]; probe EXR 1024² with R = .5 + .5 sin 2πu, G = .5 + .5 cos 2πu, B = .5 + .5 sin 2πv; `ND_hextiledimage_color3`, falloffcontrast 0; 96², 1 spp. Normal map: constant EXR (.8,.5,.9), `strength` 1 and .5. |
| C-gpvu | `ND_image_color3.file` ← `ND_geompropvalueuniform_filename("texpath")`, constant-colour EXRs; quads with constant string / asset, uniform per face, none; PointInstancer with a `string[]` vertex primvar. |
| D-AO | Floor 2000² (unlit ← AO) under a grey ceiling at y = 1; orthographic camera at y = 0.5 looking down; 256 spp. |

## Appendix B: sources

- `$HFS/houdini/RenderPropertiesKA.json` (Karma property names, defaults, help texts);
  `soho/parameters/BRAY_HdKarma_{Global,Geometry}.ds`.
- Help in `$HFS/houdini/help/nodes.zip`: `lop/karmarenderproperties.txt` ("Limits"), `vop/mtlxnormalmap.txt`
  ("MikkT normal maps"), `sop/polyframe.txt`, `cop/previewmaterial.txt`.
- `$HFS/houdini/karmaShaderNodes.json`, `karmaNodeGraphs.json`, `python3.13libs/mtlx2karma.py` (Karma's extra
  `karma_mikkt` / texture inputs).
- MaterialX 1.39.5 in `$HFS/houdini/materialx/libraries`: `stdlib/stdlib_defs.mtlx` (hextiled, flake, ramp,
  geompropvalueuniform); `stdlib/genglsl/mx_hextiled*.glsl`, `lib/mx_hextile.glsl`, `lib/mx_geometry.glsl`,
  `mx_flake2d/3d.glsl`, `lib/mx_flake.glsl`, `lib/mx_noise.glsl`; `houdini/legacy_1_39.mtlx` (ambientocclusion,
  curveadjust), `houdini/huniformramp.mtlx`.
- `$HFS/houdini/vex/include/mxstd.h` (Karma's flake and hextile VEX), read for behaviour only, not copied.
- Nova: `core/integrator/wavefront.cpp`, `settings.h`, `core/bsdf/closure.h`, `surface_models.h`,
  `core/scene/scene.h`, `core/accel/accel.h`, `core/shading/{ops.h,ir.h,noise.h}`,
  `hydra/material/translator.cpp`, `tests/test_hydra_materials.cpp`, `tests/reference/README.md`.
