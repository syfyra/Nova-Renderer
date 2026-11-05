# M9 research: many lights, emissive geometry as lights, light linking (Houdini 22.0.466)

Scope: the light part of docs/PLAN.md M9 ("Light-BVH, Light-Linking ... Feature-Parität mit dem Arnold-Kern") and what earlier milestones deferred to it:
- light sampling of emissive meshes (docs/design/M3.md, 2.2);
- per-light sampling quality and the light tree (docs/design/M7.md, 2.2);
- equiangular sampling for point and spot lights in thin media (docs/design/M8.md, 2.2; docs/research/m8/volume-algorithms.md E6).

Hair, curves and SSS are covered by other M9 reports. This report feeds the binding M9 design document. It implements nothing in the renderer.

It answers five questions:
1. A light BVH (light tree): the literature, production practice, a prototype lab that compares it with Nova's power-proportional alias table, and how it fits MIS, infinite lights, instances, volume lights and IPR.
2. Emissive geometry as lights: sampling, power estimates, and Karma's "Efficient Emission Sampling" with its LPE and multiplier behaviour, measured.
3. Light linking: the USD semantics, what Houdini authors, what reaches a third-party Hydra delegate, and Karma's measured behaviour.
4. Equiangular sampling for Nova's M8 volume integrator.
5. Recommendations: data structures, the integrator, the linking data path, and tests with Karma numbers.

Sources:
- the Houdini install (`$HFS` = `C:\Program Files\Side Effects Software\Houdini 22.0.466`, `$I` = `$HFS/toolkit/include`). The USD in Houdini 22.0.466 is 26.05 (`$I/pxr/pxr.h:45-48`, `PXR_VERSION 2605`);
- the property registry `$HFS/houdini/RenderPropertiesKA.json` and the plugin registries `$HFS/houdini/dso/usd_plugins/*/resources/plugInfo.json`;
- probes, all in isolated processes. Hython ran through `tests/houdini/run_hython.ps1`, and husk was a subprocess of that isolated hython. The user's Houdini session was not touched, and no node network or HDA was saved: every probe builds what it needs at run time;
- a stand-alone C++ prototype, `probes/lt_light_lab.cpp`. It ran on an AMD Ryzen 7 9800X3D (8 cores), built with MSVC 14.44 (`/O2 /arch:AVX2`);
- the literature and open-source code. pbrt-v4 `lightsamplers.h` and Cycles `kernel/light/tree.h` / `scene/light_tree.h` were read for this report. Other items are marked "from memory".

Every claim carries its source:
- **(measured)**: Karma renders or prototype numbers;
- **(probe)**: hython or probe-delegate output;
- **(USD)**: authored USD read back by a probe;
- **(header file:line)**: a header under `$I`;
- **(registry)**: `RenderPropertiesKA.json` or a `plugInfo.json`;
- **(help)**: the registry's help text, paraphrased;
- **(lit)**: literature or source code read for this report; **(lit, from memory)**: not re-checked;
- **(code file:line)**: Nova's code;
- **(design)**: a proposal.

Probe and prototype files are in `probes/` with the prefix `lt_` (section 9).

---

## 0. Findings in short

1. **A light BVH beats Nova's power-proportional alias table by 22x to 1764x in variance at equal sample counts (measured, E1).**
   - Scenes and N (1k / 10k / 100k lights):
     - "street": small sphere lights over a ground plane: 107x / 485x / 1764x;
     - "tris": one-sided emissive triangles, randomly oriented: 83x / 399x / 659x;
     - "cloud": lights all around receivers with random normals: 22x / 36x / 46x.
   - The power table is barely better than uniform selection (1.0-1.7x).
   - The price is 370-900 ns per selection against 20-40 ns. Counting a 1 µs shadow ray per sample, the gain is still 15x-983x.
   - The BVH was unbiased in every test (|z| < 2.7, E1/E7). Its PMF matched the sampling frequencies (χ²/dof 0.95-1.03 over 3.7k-10k lights, E2).
2. **Splitting (Conty & Kulla's adaptive tree splitting) did not pay (measured, E7).** It took several light samples per estimate and reached only 0.23-0.83x of the single-sample BVH's efficiency per light sample. Cycles also picks one light per shading point (lit). Recommended: one sample, no splitting.
3. **The MIS PMF of a light hit by a BSDF ray costs 385-787 ns with pbrt-v4's bit trail (measured, E1).** It needs the previous vertex's position and normal on the path.
4. **The same tree answers the BSDF rays' "which lights does this ray cross" query (measured, E4).**
   - Nova's brute-force loop over all area lights costs 2.3 µs per ray at 1k lights, 23 µs at 10k and 0.23-1.07 ms at 100k.
   - A traversal of the light tree's boxes costs 0.10-0.22 µs, and 0.28-1.9 µs when the rays start inside the cloud. That is within 1.0-1.25x of a dedicated SAH ray BVH, except cloud 10k (1.8x).
5. **IPR: refit after intensity edits, rebuild after moves (measured, E3).**
   - After intensity and colour edits a bottom-up refit costs 0.36 ms (10k) / 3.7 ms (100k) against a 70 / 940 ms single-threaded build, with the same variance.
   - After a few lights move far, the variance of a refitted tree explodes: 2254 against 2.9 with 1 % moved at 10k.
6. **Lights at infinity: pick them by importance at the shading point, not with pbrt's fixed 1/(n+1) (measured, E5).**
   - pbrt's 1/(n+1) is 2.0-2.3x worse than the oracle when the distant light gives 10 % of the light.
   - Nova's capped power share is within 1-7 % of the oracle at 10-50 %, but 2.2-4.9x worse at 90 %.
   - An importance ratio against a 16-node cut of the tree stays within 1.04-1.43x of the oracle everywhere.
7. **The receiver bound must be one-sided for opaque receivers (measured, E8).** pbrt-v4's |cos| gives 16-23 % more variance and spends samples on lights behind the surface.
8. **Karma uses a light tree by default (registry).**
   - `karma:global:light_sampling_mode` defaults to "Light Tree" (the alternative is "Uniform"), and `light_sampling_quality` defaults to 1.
   - Per light: `karma:light:samplingquality` 1, `samplingmode` Auto/Uniform/Light Tree, and `equiangular` false.
   - The help names the lights that stay out of Karma's tree: dome, distant, point, light-filtered and shaped (spot) lights.
9. **Karma's emissive meshes as lights (`treat_as_lightsource`) behave as its volume lights do (measured; research H 4.8).**
   - With 1 (Yes) or 2 (Auto) the emission becomes an **L** event everywhere: the camera sees `CL` instead of `CO`, and the ground gets `CDL` instead of `CDO`.
   - The means are unchanged (0.0998 against 0.1004).
   - At 16 samples the ground beside the emitters is 2-18x less noisy in relative standard deviation.
   - `lightsource_diffusescale` scales the diffuse receivers (2x, or 0), `lightsource_specularscale` the glossy ones (`C<R[GS]>L` 0.0020 → 0), and the camera's view stays plain.
   - **Auto turned every tested emitter into a light:**
     - sizes 0.05-0.6 and emission 0.001-20;
     - next to a sun;
     - a noise-driven emission;
     - a mesh of 32,768 faces.
   - The registry default is No (0), although the help text calls Auto the default.
10. **Karma's mesh emission is two-sided.** A single quad facing away from the ground lit it like a sphere did: 0.1023 against 0.1004 near it (measured).
11. **Light linking semantics, measured in Karma.**
    - `lightLink` decides only the direct light at the receiver: light samples, and also BSDF rays of the receiver that reach the light. A metal sphere excluded from a light loses that light's highlight (max 16.4 → 0).
    - Light reflected off other, linked objects still arrives.
    - An object excluded from a light still casts that light's shadow.
    - `shadowLink` removes occlusion completely, for small and large lights alike: an excluded sphere's shadow region reads 0.15904 against 0.15909 for no shadows at all. So the BSDF-sampled part of MIS is not blocked either.
    - Domes can be linked.
    - Native instance prims link per instance, and point instancers as a whole. Instance-proxy paths in a collection are ignored.
12. **USD collection gotcha (measured, probe).** UsdLux's `includeRoot` fallback is true. A `membershipExpression` therefore only takes effect when `includeRoot` is blocked or false, and the Light Linker LOP authors that block. Unblocked, Karma ignored the expression and the light lit everything.
13. **A third-party delegate gets no light linking by default (probe, husk and in-process).**
    - `GetLightParamValue(lightLink | shadowLink)` is empty and `GetCategories()` is empty.
    - Houdini's own `HD_LightLinkingSceneIndexPlugin` is registered only for "Houdini VK", "Karma CPU" and "Karma XPU" (registry), and registering it by id for another renderer had no effect (probe).
14. **Appending pxr's public `HdsiLightLinkingSceneIndex` (`pxr_hdsi`) gives a delegate exactly Karma's semantics (probe).**
    - Category tokens on lights (empty means everything).
    - `GetCategories` on rprims.
    - Per-instance categories on native instancers.
    - Instancer-level categories on point instancers.
    - Proxy exclusions ignored.
    - It must not list `instancer` among its geometry types, otherwise per-instance categories are lost (probe).
15. **Lights under instancers arrive as one light sprim with an instancer id (probe).** This holds for point instancers and native instances. Karma renders every instance (measured: two instanced sphere lights each lit their spot, 0.166). Nova's `HdNovaLight` ignores the instancer today (code `hydra/light.cpp:89-180`).
16. **Equiangular sampling: implement per light, opt-in (Karma's `karma:light:equiangular`, default off).** Use MIS weights computed from a deterministic proxy of the free-flight density. Such weights stay unbiased with Nova's spectral-tracking free flights. Research A (M8) E6 measured 70x-10⁶x gains in thin media near point lights.
17. **Recommendation, in short (section 6).**
    - A pbrt-v4-style light tree over all bounded lights: area, point and spot lights, emissive triangles and volume lights. One light per leaf, a flat 32-48-byte node array, a bit trail for the MIS PMF, and the same tree for BSDF-ray light queries.
    - Infinite lights outside the tree, chosen against a tree cut.
    - Receiver-set masks in the tree nodes (Cycles' scheme) for light linking, plus a shadow-link occlusion filter.
    - Lights with a shadow link sampled by NEE only.
    - Emissive triangles as tree emitters, with emission evaluated at unoccluded shadow-ray ends.
    - Instanced lights expanded per instance.

---

## 1. Nova today

- **Light selection**:
  - Walker's alias table over all lights by estimated power (code `core/lights/light_sampler.{h,cpp}`; `LightSamplerView::sample`, `light_sampler.h:22`). The header says M9 replaces it.
  - Power is luminance-weighted flux times the larger of the diffuse/specular multipliers (code `lights.cpp:257-299`).
  - Lights at infinity count the flux through a disk of the scene radius, and are capped at the local lights' total when they exceed it (code `scene.cpp:645-686`).
- **NEE**: `chooseLight` draws one light from the `stream::kLight` 3D dimension (z selects the light, xy the point) and samples it (code `wavefront.cpp:1126-1136`). Volume lights (`LightType::Volume`, M8 D13) are sampled by `sampleVolumeLight` over their majorant cells (code `wavefront.cpp:1090-1124`). The MIS weights are `lightSampleWeight` / `bsdfSampleWeight` with power-heuristic solid-angle PDFs times the selection PMF (code `lights.h:419-450`).
- **BSDF rays reaching lights**: every scattered ray tests **every** area light (`areaLightsAlongRay`, code `lights.h:581-610`, "the light BVH of M9 has to answer this all-crossings query"), and every escaped ray every light at infinity (`escapedRadiance`, `lights.h:478`). The volume tracker does the same with `areaLightCrossings` (code `wavefront.cpp:771`).
- **Emissive surfaces** are reached by BSDF sampling only, with weight 1 (code `wavefront.cpp:1400-1408`). The emission is computed per hit by the material's program (`cl.emission`). `OutputBinding::uniform` already tells whether each output is uniform (code `core/shading/program.h:53-58`). The opacity, displacement and volume sub-programs show the pattern for an emission-only sub-program (code `program.h:72-91`).
- **Hydra lights**:
  - `HdNovaLight::Sync` reads UsdLux and Karma light parameters through `GetLightParamValue` (code `hydra/light.cpp:89-180`).
  - It ignores the instancer id (finding 15) and `lightLink`/`shadowLink`.
  - The delegate supports the six UsdLux types, not `meshLight` (code `renderDelegate.cpp:55-60`).
  - Nova already has a scene-index plugin, `HdNova_ImplicitSurfaceSceneIndexPlugin` (code `hydra/implicitSurfaceSceneIndexPlugin.cpp`), where a light-linking scene index can be appended.
- **Sampler**: per bounce 8 decision slots, all assigned (`kLight` 0 ... `kTexture` 7, code `core/math/sobol.h:146-153`). New per-bounce random numbers come from the path's PCG streams as in M8 (D18).

---

## 2. Light BVH

### 2.1 Literature and production practice

- **Conty Estevez & Kulla 2018** (HPG, "Importance Sampling of Many Lights with Adaptive Tree Splitting") (lit, from memory).
  - A binary BVH over lights. Each node stores a box, the total power and an orientation bound: a cone axis, the normals' spread θ_o and the emission falloff θ_e.
  - The build uses a surface-area-orientation heuristic (SAOH).
  - The node importance at a shading point is power × the clamped receiver cosine × the clamped emitter cosine / d². Both angles are reduced by the box's angular radius θ_u.
  - Traversal picks a child in proportion to importance.
  - "Adaptive splitting" visits both children where a variance estimate of the node's contribution exceeds a threshold, so one shading point can take several light samples.
- **pbrt-v4 `BVHLightSampler`** (lit, read).
  - `LightBounds`: box, power φ, cone axis w with cos θ_o and cos θ_e, and a two-sided flag.
  - Interior nodes are unions; the cone union is pbrt's `DirectionCone::Union`.
  - The build is a SAOH with 12 buckets per axis and the cost φ·M_Ω·K_r·area, with **one light per leaf**.
  - `Importance(p, n)`:
    - d² is clamped from below by half the box diagonal. As written the clamp compares d² with an unsquared length.
    - θ' = max(0, θ_w − θ_o − θ_b), where θ_b is the box's angular extent seen from p (everything when p is inside).
    - The node gets 0 if θ' ≥ θ_e, else φ·cos θ'/d².
    - The receiver factor is the clamped |cos| of the incidence angle reduced by θ_b, and is skipped when n = 0 (media).
  - `Sample`: infinite lights get probability n_inf/(n_inf + 1), uniformly among themselves. The rest descends the tree, reusing one uniform number rescaled at every level, and returns nothing when both children have importance 0.
  - `PMF(light)`: re-walks the path stored as a **bit trail** (one bit per level) and multiplies the child probabilities.
  - Node: a quantized box (6×16 bits relative to the scene box), an octahedral cone axis, 15-bit cos θ_o and cos θ_e, φ as a float, and a 31-bit child or light index with a leaf bit, aligned to 32 bytes.
- **Cycles light tree** (Blender 3.5+) (lit, read).
  - Based on the same paper and pbrt's build: SAOH, 12 buckets, multithreaded above 4096 emitters, and up to a few emitters per leaf.
  - It keeps a **min and max importance** per child: the max uses the near distance bound, the min the far bound and is 0 outside the emission cone. A child's probability is the mean of the two ratios. A leaf picks an emitter by the same blend.
  - **No splitting**: one light per shading point.
  - Emissive triangles are emitters. A mesh is a **subtree** in object space, referenced by **instance nodes** with their own object transform.
  - Distant lights and the world are special leaves with their own importance.
  - Volume points use the **ray segment** (closest point to the cluster, the angle the segment subtends), and the min importance is 0 there.
  - `light_tree_pdf` re-walks the bit trail, through the mesh subtree for triangles.
  - **Light linking**: every node carries a 64-bit mask of the receiver sets its emitters illuminate, and each receiver set has its own root (shared subtrees where the membership is uniform).
  - Emission sampling for meshes: Auto (default) uses a heuristic estimate of the emitted intensity to decide whether a mesh is a light. Other modes take front, back or both sides (lit, Blender 3.5 release notes).
- **Moreau, Pharr & Clarberg 2019** ("Dynamic many-light sampling for real-time ray tracing", HPG) (lit, from memory). A two-level light BVH: per-mesh bottom-level trees of emissive triangles, a top level over instances, and refits for animation. This is the GPU-oriented variant.
- **Karma** (registry, help).
  - The light tree is the default (`karma:global:light_sampling_mode` = "Light Tree"). The help says it can be much faster for many lights.
  - Dome, distant, point, filtered and shaped (spot) lights stay outside Karma's tree and are sampled separately.
  - Per light: `samplingquality` (a multiplier on the global quality that gives the light more samples) and `samplingmode` (Auto, or force Uniform / Light Tree).
- **Arnold and RenderMan** also ship tree-based many-light sampling (lit, from memory, not re-checked).

### 2.2 The prototype lab (`probes/lt_light_lab.cpp`)

The lab is stand-alone (no Nova or Houdini code) and was built with `probes/lt_build.bat`. It measures **light selection alone**: the irradiance E(x) = ∫ L cos⁺ dω at receivers, with no occlusion (proto).

- **Lights**:
  - Sphere lights of radius 0.05 with uniform radiance, sampled by their solid-angle cone.
  - One-sided triangles (edge about 0.2), sampled by area. Their power is π·L·area.
  - Radiances are log-uniform in [10, 100].
- **Scenes**:
  - **street**: spheres 0.5-1.5 above a ground plane at a constant density (one per 4 m²). Receivers on the ground, normal up.
  - **cloud**: spheres uniformly in a cube. Receivers inside the cube with random normals.
  - **tris**: randomly oriented one-sided triangles above a ground plane. Receivers on the ground.
  - 256 receivers per scene.
- **Reference**: every light, 16 samples each (64 in E7).
- **Metric**: relVar is the mean over receivers of MSE/E² for one-sample estimates. It uses 4096 estimates per receiver; bias z = Σ(mean − E)/√Σ(var/K).
- **Times** are single-threaded, selection plus shape sample plus evaluation, without the shadow ray.
- **Strategies**:
  - uniform;
  - **power**: Nova's alias table, the same code as `LightSampler::build`;
  - **bvh**: pbrt-v4's sampler with the importance and build of 2.1, one light per leaf, and a one-sided receiver bound (E8);
  - **split**: the BVH with both children visited while a node's bounding sphere subtends more than τ (sine of the half-angle), up to a budget of samples.

### 2.3 E1: variance and cost

Results (measured; full output in `probes/lt_out_light_lab.txt`). Gain is the power table's relVar divided by the BVH's, at equal sample counts. "+1 µs" counts a 1 µs shadow ray per sample.

| scene | N | relVar uniform | relVar power | relVar bvh | ns/sample power / bvh | gain | gain +1 µs | build alias / bvh [ms] | depth | PMF eval [ns] |
|---|---|---|---|---|---|---|---|---|---|---|
| street | 1k | 141 | 124 | **1.16** | 26 / 409 | 107 | 78 | 0.03 / 6.1 | 13 | 385 |
| street | 10k | 1578 | 1454 | **3.00** | 28 / 575 | 485 | 317 | 0.15 / 75 | 17 | 485 |
| street | 100k | 18,200 | 11,630 | **6.59** | 35 / 858 | 1764 | 983 | 1.0 / 949 | 20 | 623 |
| cloud | 1k | 39.5 | 36.9 | **1.66** | 33 / 521 | 22 | 15 | 0.03 / 6.2 | 13 | 478 |
| cloud | 10k | 97.8 | 68.4 | **1.88** | 34 / 475 | 36 | 26 | 0.19 / 86 | 16 | 487 |
| cloud | 100k | 172 | 102 | **2.21** | 42 / 853 | 46 | 26 | 1.1 / 954 | 20 | 598 |
| tris | 1k | 222 | 220 | **2.66** | 24 / 372 | 83 | 62 | 0.02 / 8.2 | 12 | 474 |
| tris | 10k | 2268 | 2222 | **5.56** | 26 / 668 | 399 | 246 | 0.13 / 95 | 16 | 582 |
| tris | 100k | 12,160 | 7,499 | **11.4** | 34 / 901 | 659 | 358 | 1.1 / 1280 | 20 | 787 |

- The BVH's z was within ±1.4 in all nine cases. The z values of uniform and power selection reached ±3. They are heavy-tailed estimators (relVar up to 10⁴), for which the sample variance underestimates the error (measured).
- Cost: about 40 importance evaluations per selection (two per level). That is 20-40 ns per level, and grows with log N (measured).
- Builds are single-threaded and unoptimized: O(N log N), 12 buckets × 3 axes per node, with full `LightBounds` unions per bucket (proto).

### 2.4 E2 / E6: PMF consistency and the reused random number

E2 used 10k lights, 4 receivers per scene and 20 M draws each (measured):
- The PMF computed from the bit trail matched the PMF accumulated while sampling to 1e-6 to 2e-5 (float).
- The sampling frequencies matched the PMF with χ²/dof 0.95-1.03 over 3.7k-10k lights with at least 20 expected draws. The maximum |z| was 3.4-4.8 over that many lights.
- **Σ PMF over all lights** is 1.0000000 on the street. It is 0.58-0.92 for cloud and 0.88-0.97 for tris.
- The missing mass is the probability that a traversal reaches a node whose children both have importance 0, for example everything below the horizon. Such draws return no light. They cost no shadow ray and do not bias the estimate, because the importance bound is conservative (2.8). With pbrt's |cos| receiver bound every draw returns a light, but more of them contribute nothing (E8).

E6 (street, 100k lights, depth 20) asked whether the reused float uniform loses precision. It is rescaled at every level (u ← (u − p₀)/(1 − p₀)). Results (measured):
- χ²/dof was 1.002-1.008 with float and 1.003-1.006 with double.
- Lights expected at least 5 times but never drawn: 7-21 with float, 12 with double. That is noise.
- So float reuse is fine at this depth. Clamp u below 1 after each rescale (proto).

### 2.5 E3: IPR edits, refit against rebuild

Street scene, single thread (measured):

| N | build | edit | refit | relVar after refit | relVar after rebuild |
|---|---|---|---|---|---|
| 10k | 70 ms (relVar 2.97) | all intensities × U[0.5, 2] | 0.36 ms | 3.02 | 2.92 |
| | | 1 % of the lights moved anywhere | 0.36 ms | **2254** | 2.91 |
| | | 10 % moved | 0.35 ms | **6117** | 2.87 |
| 100k | 942 ms (6.42) | all intensities | 3.7 ms | 6.44 | 6.52 |
| | | 1 % moved | 3.8 ms | **1146** | 6.47 |
| | | 10 % moved | 3.8 ms | **5.4·10⁶** | 6.42 |

- Refit is exact and practically free for colour, intensity, exposure and multiplier edits.
- After lights move away from their clusters, the boxes of their old ancestors span the scene. Lights near a receiver then sit under nodes whose importance is computed for a huge box, and are drastically under-sampled.
- Rule (design): refit for value edits and small transform edits (a light that stays inside its parent's box, or moves by less than a fraction of it). Rebuild otherwise, or when the summed box growth exceeds a threshold.
- A 100k-light rebuild is about 1 s single-threaded (proto). A parallel build (Cycles: tasks above 4096 emitters) and a background rebuild keep IPR responsive.

### 2.6 E4: BSDF rays finding the lights they cross

Cosine-distributed rays from the receivers collect **every** light they cross: lights do not block, as in Nova (code `lights.h:19-23, 577-610`). Results (measured):

| scene | N | brute force (Nova today) | SAH ray BVH (4 per leaf) | the selection BVH (SAOH, 1 per leaf) | hits/ray |
|---|---|---|---|---|---|
| street | 1k / 10k / 100k | 2.3 / 22.6 / 235 µs | 98 / 136 / 183 ns | 102 / 150 / 202 ns | 0.003-0.004 |
| cloud | 1k / 10k / 100k | 2.3 / 23.4 / 234 µs | 272 / 662 / 1522 ns | 279 / 1194 / 1899 ns | 0.010-0.048 |
| tris | 1k / 10k / 100k | 10.7 / 107 / 1065 µs | 134 / 173 / 206 ns | 112 / 172 / 217 ns | 0.008-0.009 |

- Build times for the SAH ray BVH: 0.8 / 8.5-9.0 / 100-132 ms.
- Brute force is already 20-90x too slow at 1k lights and 10³-10⁴x at 100k.
- The selection tree, traversed with its boxes (slab test, stack, every leaf hit), costs at most 1.25x the dedicated ray BVH, except cloud 10k (1.8x).
- So one structure serves selection, the MIS PMF and the BSDF-ray query (design).

### 2.7 E5: lights at infinity next to the tree

A distant light (irradiance E_s at normal incidence, direction 17 degrees from the zenith) next to N street lights, with its share of the mean irradiance set to 10 / 50 / 90 %. The probability q of picking the distant light (relVar, measured):

| N | share | pbrt q = ½ | Nova capped power | importance vs root | importance vs 16-node cut | oracle q = E_sun/E |
|---|---|---|---|---|---|---|
| 1k | 0.1 | 2.61 | **1.15** | 1.40 | 1.27 | 1.13 |
| 1k | 0.5 | 0.72 | **0.68** | 1.13 | 0.86 | 0.63 |
| 1k | 0.9 | 0.69 | 0.69 | 0.29 | **0.20** | 0.14 |
| 10k | 0.1 | 5.37 | **2.68** | 2.92 | 2.79 | 2.68 |
| 10k | 0.5 | 1.67 | **1.57** | 2.07 | 1.69 | 1.51 |
| 10k | 0.9 | 0.74 | 0.74 | 0.54 | **0.39** | 0.34 |

How the q values were formed:
- "Importance" is q = I_sun/(I_sun + I_tree), with I_sun = 4π·E_s·cos⁺. The factor 4π brings an irradiance onto the scale of φ/d² for sphere emitters.
- I_tree is the root's importance, or the sum over the 16 nodes four levels down.

Results:
- No rule wins everywhere.
- The capped power share is optimal when local light dominates. It is 2.2-4.9x off when the sun dominates, because the cap holds it at one half.
- The cut-based importance is never worse than 1.4x of the oracle.
- The root alone is too coarse: the receiver lies inside the root box.

### 2.8 E7 / E8: splitting, and the receiver bound

**E7** (64-sample reference, 3 seeds; measured):

| scene | bvh | τ 0.5, budget 8 | τ 1, b 4 | τ 2, b 4 | τ 2, b 2 |
|---|---|---|---|---|---|
| street 10k: relVar [samples] | 3.01 [1] | 1.45 [8] | 1.43 [4] | 2.04 [2.31] | 2.48 [1.61] |
| efficiency per light sample vs bvh | 1 | 0.26 | 0.53 | 0.64 | 0.76 |
| cloud 1k | 1.66 [1] | 0.73 [8] / 0.29 | 0.68 [4] / 0.61 | 0.92 [2.51] / 0.72 | 1.22 [1.63] / 0.83 |
| tris 10k | 5.54 [1] | 3.00 [8] / 0.23 | 2.90 [4] / 0.48 | 3.86 [2.24] / 0.64 | 4.54 [1.58] / 0.77 |
| tris 100k | 11.4 [1] | 6.11 [8] / 0.23 | 6.07 [4] / 0.47 | 7.72 [2.41] / 0.61 | 9.29 [1.68] / 0.73 |

- Splitting removes at most half the variance and always costs more samples than it saves.
- Spending the same samples on more independent paths (or on the pixel) is better. This agrees with Cycles' choice (lit).
- All 60 z values lay within ±2.7. The tris 10k rows share a slightly positive tendency in every column, because the same reference is used for all rows. So there was no evidence of bias.

**E8** (cloud, receivers with random normals; measured):

| N | relVar, one-sided receiver bound | relVar, pbrt's \|cos\| | draws that yield a light (one-sided / \|cos\|) |
|---|---|---|---|
| 1k | **1.66** | 2.04 | 0.80 / 1.00 |
| 10k | **1.88** | 2.18 | 0.79 / 1.00 |
| 100k | **2.21** | 2.70 | 0.78 / 1.00 |

- The one-sided bound (the angle to n reduced by the box's angular radius, zero beyond the horizon) is 16-23 % better.
- It also skips 20 % of the shadow rays.
- Transmissive closures and volumes need the two-sided form, or none (n = 0), as Cycles and pbrt do (lit).

### 2.9 Design questions

- **MIS PMF for a light hit by a BSDF ray** (lit, measured).
  - Re-walk the light's bit trail with the importances at the **previous vertex** (its position, its normal, and whether its closure transmits) and multiply the child probabilities: 385-787 ns.
  - Nova's queues must keep that vertex per path: the offset origin of the continuation ray is already `rays.org`, so add the normal and a flag. Cycles stores the same.
  - An alternative is a per-light parent index (walk up, then down). The bit trail needs 8 bytes per light and supports depth ≤ 64.
- **Lights at infinity** (dome with a distribution, distant lights): keep them outside the tree, in a small alias table among themselves, and choose that table with q from E5 (design, 6.2).
  - A constant dome stays BSDF-sampled only (Nova today).
  - pbrt's PMF for infinite lights is 1/(n_inf + 1) (lit), which is the weakest choice in E5.
- **Instances.**
  - Lights under instancers: expand per instance into world-space entries.
  - Emissive meshes under instancers: flatten world-space triangles per instance up to a budget. A two-level tree (Cycles / Moreau et al.: per-mesh subtree in object space, instance nodes with transforms) is the later answer for heavily instanced emitters (design).
- **Volume lights** (`LightType::Volume`): one emitter with the volume's world box, φ = Nova's existing power (4π·Σ ε·ΔV, flux units like the others), θ_o = π, θ_e = π/2. For volume receivers (phase-function NEE) pass n = 0, so there is no receiver factor (pbrt does the same) (design).
- **Textured lights**:
  - Rect textures: φ already includes the mean luminance (code `lights.cpp:287`).
  - Emissive triangles with varying emission: estimate φ per triangle (6.5).
  - Any non-negative estimate is unbiased. A zero estimate only removes that triangle from light sampling, and MIS then gives its BSDF hits weight 1 (code `lights.h:436`).
- **Mixed emitter types.** pbrt's importance uses flux φ. In one direction an omnidirectional emitter (sphere, point, volume) radiates φ/(4π), a one-sided planar one up to φ/π.
  - The tree therefore prefers spheres over planar emitters by up to 4x at equal irradiance. This costs variance, not correctness.
  - Normalizing each leaf's φ by its cone's M_Ω (pbrt's own cost measure: π for a one-sided emitter, 4π for an omnidirectional one) makes the importance about the radiant intensity, which is irradiance-comparable.
  - It also gives the infinite lights' q a common unit, so the 4π of E5 is not needed (design; not measured).
- **Negative intensities**: φ = |φ| (Nova's convention).
- **Spot lights**: θ_o = 0 about the axis and θ_e = the cone angle (pbrt). Karma keeps them out of its tree (registry); nothing prevents including them.
- **GPU layout** (design).
  - A flat array of 32- or 48-byte nodes in depth-first order: the first child implicit, then the second child index or the light index, and a leaf bit.
  - Bounds quantized to 16 bits against the scene box, the cone axis octahedral, cos θ_o / cos θ_e as 15 bits, φ as a float.
  - The light-link mask is 8 bytes more (6.6).
  - A per-light bit trail (8 bytes).
  - Traversal is a loop without recursion or stack, so the same code runs in CUDA. The ray query needs a small stack.

---

## 3. Emissive geometry as lights

### 3.1 Sampling and power (literature)

- **Points on a triangle** (lit).
  - Uniform area sampling (pdf = d²/(A·cos θ_l)) is cheap and good for small solid angles.
  - Spherical-triangle sampling (Arvo 1995) samples the solid angle uniformly and removes the 1/d² and cos θ_l variance near large emitters.
  - pbrt-v4 switches between them by solid angle: area sampling below 3·10⁻⁴ sr and above 6.22 sr. In between it samples the spherical triangle, warped bilinearly by the receiver cosine at the corners (lit, from memory).
  - Nova's rect lights already follow the same pattern with spherical rectangles (code `core/lights/shapes.h:114-245`).
- **Power estimates for textured or procedural emission.**
  - pbrt-v4's `DiffuseAreaLight::Phi` averages the image over its texels (lit, from memory).
  - Cycles estimates the emission of a shader from its node graph, and uses that estimate both for the Auto decision and for the light tree's energy (lit; Blender 3.5 notes).
  - Any estimate leaves MIS unbiased as long as the BSDF-hit weight uses the same PMF.

### 3.2 Karma's "Efficient Emission Sampling" (registry, help)

- `karma:object:treat_as_lightsource`: 0 No, 1 Yes, 2 Auto. The **registry default is "No"**, but the help text calls Auto the default.
  - The help explains it as making an emissive object behave like an explicit light, which samples its light far more efficiently. The cost is memory and update time.
  - Auto lets Karma decide by an internal heuristic.
  - The property is marked `material_vop: true`, so it can also be set on materials.
- `karma:object:lightsource_samplingquality`, `lightsource_diffusescale` and `lightsource_specularscale` all default to 1 and are invisible in the UI. The Pyro "Create Render Stage" sets them (research H 1.3).
- `karma:global:override_lighting` has a mode "Emissive Objects", which turns off all lights and keeps only geometry lights.

### 3.3 Measurements (`probes/lt_karma_emit.py`, output `probes/lt_out_karma_emit.txt`)

Scene "pair":
- A grey diffuse ground (base 0.5, specular 0) seen from above (orthographic, 15 px per unit).
- An emissive polygon sphere (32 × 16, radius 0.5, centre 1 above the ground).
- An emissive quad (1 × 1 at height 1, **normal pointing away from the ground**).
- Both emitters: `standard_surface` base 0, specular 0, emission 1, colour (2, 1, 0.5).
- No lights. Karma CPU, 256 path-traced samples, box filter, no colour limit.
- Red means; "rel. std" is the pixel standard deviation over the mean at 16 samples (measured).

| treat_as_lightsource | ground near sphere | far | near quad | far | camera on emitters | LPE | rel. std @16 (near / far, sphere; quad) |
|---|---|---|---|---|---|---|---|
| 0 | 0.1004 | 0.0202 | 0.1023 | 0.0148 | 2.000 = `CO` | ground = `C<RD>O` | 0.46 / 3.13; 0.43 / 2.14 |
| 1 | 0.0998 | 0.0185 | 0.1021 | 0.0147 | 2.000 = `CL` | ground = `C<RD>L` | **0.23 / 0.26; 0.13 / 0.12** |
| 2 (Auto) | identical to 1 | | | | `CL` | `C<RD>L` | identical to 1 |
| 1, diffuse scale 2 | 0.2009 | 0.0381 | 0.2049 | 0.0295 | 2.000 | | |
| 1, diffuse scale 0 | 0 | 0 | 0 | 0 | 2.000 | | |
| 0, glossy ground (specular 1, roughness 0.3) | `C<RD>O` 0.0963 + `C<R[GS]>O` 0.0024 | | | | | | |
| 1, glossy ground | `C<RD>L` 0.0951 + `C<R[GS]>L` 0.0020 | | | | | | |
| 1, glossy ground, specular scale 0 | `C<RD>L` 0.0963, `C<R[GS]>L` **0** | | | | | | |

- **The emission is two-sided**: the quad faces away from the ground and still lights it like the sphere does, with and without light treatment (measured).
- **Light treatment changes noise, not the mean.** The far ground's relative noise drops 12x (sphere) and 18x (quad), so the variance drops by 150-320x at equal samples (measured).
- **LPE**: treated emitters are **L** events for the camera and every receiver, otherwise **O** events. This is the same as Karma's volume lights (research H 4.8).
- **Multipliers**: the diffuse scale multiplies diffuse receivers, the specular scale glossy ones, and the camera's direct view is not scaled.

Scenes "auto", "auto_sun" and "auto_varied" all used `treat_as_lightsource` 2 and checked whether the camera sees `CO` or `CL` (measured):
- nine spheres of radius 0.05 / 0.25 / 0.6 with emission 0.1 / 1 / 20;
- the same with emission 0.001 / 0.1 / 20 next to a distant light of irradiance 3;
- a noise-driven emission (`ND_noise3d`);
- a 256 × 128 sphere (32,768 faces).

**Every one became a light (`CL`, `CO` = 0).** Within this range Auto behaves like Yes. The internal heuristic was never seen to decline.

### 3.4 What this means for Nova

Nova must provide (design, details in 6.5):
- `treat_as_lightsource` 1 and 2 on meshes;
- emissive triangles as tree emitters with two-sided emission;
- L events;
- the light multipliers from the two scales;
- no change of the mean.
The test values are in section 7.

---

## 4. Light linking

### 4.1 USD semantics and what Houdini authors (`probes/lt_link_authoring.py`, output `probes/lt_out_link_authoring.txt`)

**UsdLux collections** (USD, probe).
- Every light has the collections `collection:lightLink` and `collection:shadowLink`. Houdini's light LOPs apply `CollectionAPI:lightLink` and `CollectionAPI:shadowLink` explicitly.
- `includeRoot` resolves to **true** when unauthored, so a light lights everything and every object shadows it.
- Membership comes in one of two forms:
  - relationship mode: `includes` / `excludes` relationships, with `expansionRule` (Houdini authors `expandPrims`, all descendants);
  - pattern mode: a `membershipExpression`.
- `lightLink` decides which objects the light illuminates; `shadowLink` decides which objects occlude it.

**The Light Linker LOP** (`lightlinker`, probe).
- Each rule has a prim, includes, excludes, a type (`light` or `shadow` link), a "reversed" flag (the rule is geometry-centric), and "Pattern Is USD Path Expression".
- What it authored:
  - "key lights sphA" (light-centric): `includeRoot = false`, `includes = [sphA]`.
  - "key shadow excludes sphB": `shadowLink:excludes = [sphB]`, with includeRoot left at its fallback.
  - "ground is lit only by the dome" (reversed): it **adds ground to the `lightLink:excludes` of every other light** and puts it in the dome's `includes`, leaving the dome's includeRoot true.
  - A path-expression rule alone: `membershipExpression = /World/sph*` with **`includeRoot` blocked** (resolved `None`); membership was then the spheres only.
  - The same rule combined with other rules was flattened into explicit `includes` with includeRoot true.
- The Light LOP itself has no linking parameters (probe).

**Pitfall, measured with Karma.** A light with only `membershipExpression = "/World/sphA"` and includeRoot unauthored (fallback true) lit **everything**: the expression is ignored. With `includeRoot = None` it lit only sphA. UsdImaging converts the unblocked case to the expression `~//*.*`, which means everything (probe). Hand-written test scenes must block includeRoot.

### 4.2 What reaches a third-party delegate (`probes/lt_link_probe/`, `probes/lt_link_hydra.py`)

The probe delegate "LinkProbe" is HDK-free (pxr libraries only) and logs, per light sprim and rprim:
- `GetLightParamValue(lightLink | shadowLink | filters)` and the Karma light parameters;
- `GetCategories`, the instancer's `GetCategories` / `GetInstanceCategories` and the instancer id;
- the scene-index prim's `light` schema, `collections` and `categories`.

It ran in husk and in-process through `UsdImagingGL.Engine` (as in the Solaris viewport), with the same results. The scene:
- meshes A, B, other and ground;
- a point instancer of boxes and two native instances of a box;
- a mesh with `MeshLightAPI`;
- a point instancer of sphere lights and two native instances of a sphere light;
- lights with relationship-mode, expression-mode and instance-proxy links;
- a linked dome.

Results (probe; outputs `probes/lt_out_link_hydra_*.txt`):

| LT_LINK_SI | light `lightLink` / `shadowLink` params | rprim `GetCategories` | native instancer | point instancer |
|---|---|---|---|---|
| none (default) | empty `VtValue` | `[]` | `[]`, no instance categories | `[]` |
| houdini (Houdini's `HD_LightLinkingSceneIndexPlugin` registered by id for "LinkProbe") | empty | `[]` | `[]` | `[]` |
| hdsi (pxr's `HdsiLightLinkingSceneIndex` appended in the delegate's scene-index plugin) | category tokens: `group_0`, ...; `""` for collections that include everything | `['group_0', 'group_1', ...]`, the collections that contain the prim | per-instance categories: ni1 with group_0, ni2 without | instancer-level categories |

Details:
- **Registration.**
  - Houdini's plugin is declared with `loadWithRenderer` ["Houdini VK", "Karma CPU", "Karma XPU"] (registry `hdHoudini/resources/plugInfo.json`). The same file restricts `HD_MeshLightSceneIndexPlugin`, `HD_PortalLightSceneIndexPlugin` and `HD_MotionBlurSceneIndexPlugin` to those renderers.
  - Registering `HD_LightLinkingSceneIndexPlugin` by id for another renderer appended nothing that changed the data (probe).
  - pxr's scene index is public: `$I/pxr/imaging/hdsi/lightLinkingSceneIndex.h`, library `libpxr_hdsi`, CMake `Houdini::Dep::pxr_hdsi`.
  - Its arguments are `lightPrimTypes`, `lightFilterPrimTypes` and `geometryPrimTypes` (header `lightLinkingSceneIndex.h:25-28`).
- **The collections arrive anyway** as path expressions in the light's `collections` data source, even without the scene index (probe):
  - includes become `(/World/sphA// - //*.*) + ...`;
  - excludes become `~//*.* - /World/sphB//`.
  So a delegate could evaluate them itself. The Hdsi scene index already does this, and it invalidates lights and geometry when membership changes (header `lightLinkingSceneIndex.h:43-60`).
- **Instancing.**
  - With `instancer` in `geometryPrimTypes`, the native instancer got instancer-level categories and **no** per-instance categories, so ni1's link was lost.
  - Without it, `GetInstanceCategories` returned one category list per instance, matching Karma (4.3).
  - The prototype rprims themselves carry no categories. A delegate takes the union of the rprim's categories, the instancer's, and the instance's (probe).
- **Instance proxies.** Excluding `/World/ni2/m` (inside a native instance) had no effect, both in the categories and in Karma (4.3). The header says that instance proxies and nested instancing are not supported yet (header `lightLinkingSceneIndex.h:67-70`).
- **Lights under instancers** arrive as **one** light sprim per prototype, with `GetInstancerId` set (`/World/lpi` and the `UsdNiInstancer` of native instances) and a prototype-local transform (probe). `HdLight::DirtyInstancer` exists (header `light.h:87`). The delegate must replicate the light per instance from the instancer's transforms.
- **Mesh lights.** `MeshLightAPI` geometry arrived as a mesh rprim only, with no `meshLight` sprim (probe). Houdini's mesh-light scene index is Karma/VK-only (registry).
- **Karma light parameters** arrive through `GetLightParamValue` with their attribute names (`inputs:karma:light:samplingquality = 2 (float)`), as Nova already reads `renderlightgeo` (probe; code `hydra/light.cpp:24-30`).
- **Registration side note.** A scene-index plugin type declared with `loadWithRenderer` in plugInfo was appended for that renderer **without** an explicit `RegisterSceneIndexForRenderer` call. With the call as well, it was appended once (probe, first probe build). Nova's `HdNova_ImplicitSurfaceSceneIndexPlugin` does both; this is worth a check (section 8).

### 4.3 Karma's behaviour (`probes/lt_karma_link.py`, output `probes/lt_out_karma_link.txt`)

Scene:
- A top view (orthographic, 8 × 8 units, 15 px per unit) of a grey diffuse ground.
- Grey diffuse spheres A (x = −1.5) and B (x = +1.5), radius 0.7.
- Boxes: two native instances (ni1, ni2) and a point instancer (pi0, pi1).
- A 1 × 1 rect light "key" (intensity 20) at (0, 2, −4), aimed at the origin, so shadows fall to +z beside the spheres.
- A dome "dome" of radiance 0.3. Karma's dome is invisible to the camera, so the background reads 0.
- Karma CPU, 256 samples, box filter.

The red channel of region means, with per-light LPEs (measured):

| configuration | A top (key direct) | B top | shadow of A | shadow of B | open ground | other notes |
|---|---|---|---|---|---|---|
| base | 0.02938 | 0.02946 | 0 | 0 | 0.02268 | dome on the ground 0.146 |
| key `lightLink` includeRoot 0, includes A | 0.02943 | **0** | 0 | 0 | **0** | **indirect key light on the ground** `C<RD>.+<L.'key'>` 0.00109 between the spheres (base 0.00204) |
| key `lightLink` excludes A | **0** | 0.02945 | **0 (A still casts the shadow)** | 0 | 0.02268 | |
| key `shadowLink` excludes B | 0.02939 | 0.02955 | 0 | **0.01787** | 0.02268 | |
| key `shadowLink` includeRoot 0 (no shadows) | 0.02951 | 0.02956 | 0.01787 | 0.01787 | 0.02268 | |
| 3 × 3 key, base | 0.2961 | 0.2965 | 0.00597 | 0.00719 | 0.1978 | |
| 3 × 3 key, `shadowLink` excludes B | 0.2968 | 0.2982 | 0.00603 | **0.15904** | 0.1979 | |
| 3 × 3 key, no shadows | 0.2980 | 0.2978 | 0.15910 | 0.15909 | 0.1979 | |
| metal B (roughness 0.15) | | key glossy on B: max 16.42 | | | | |
| metal B, key `lightLink` excludes B | | key glossy on B: **max 0** (highlight gone; the dome reflection remains) | | | | |
| dome `lightLink` includes A and B only | dome on spheres 0.150 | 0.150 | dome 0.0012 | 0.0010 | **dome 0.0012** (base 0.146; the rest is light reflected off the spheres) | |
| key `lightLink` includes ni1 and pi | 0 | 0 | | | 0 | ni1 **0.13571**, ni2 **0**, pi0 0.37018, pi1 0.37028 |
| key `lightLink` excludes `/World/ni2/m` (instance proxy) | as base | | | | | ni2 0.13569: **ignored** |
| `membershipExpression` /World/sphA, includeRoot unauthored | as base (expression ignored) | 0.02946 | | | | |
| `membershipExpression` /World/sphA, includeRoot `None` | 0.02943 | **0** | | | 0 | = the includes form |
| point instancer of two sphere lights (radius 0.05, intensity 50, 0.6 above the shadow regions) | | | 0.16627 | 0.16635 | | both instances light (analytic about 0.17 at the centre) |

Reading:
1. **lightLink acts at the receiver only.** An unlinked object gets none of the light directly: neither from light samples nor from its own BSDF rays reaching the light (the metal highlight is gone). It still receives the light **indirectly** via linked objects (0.00109 on the ground between the spheres).
2. **Illumination and occlusion are independent.** An object excluded from `lightLink` still casts that light's shadow; `shadowLink` decides occlusion.
3. **shadowLink removes the occluder completely for that light.** With the 3 × 3 light the excluded sphere's shadow region reads 0.15904 against 0.15909 with no shadows at all: 0.03 %, within noise. The BSDF-sampled part of MIS is not blocked either. Karma presumably traces its BSDF light samples as separate rays to the light. Nova reuses the continuation ray for them, which needs care (6.6).
4. **Domes link like other lights.** Excluded geometry keeps only the dome light reflected off linked objects.
5. **Instancing**: native instances link per instance, point instancers as a whole, and instance-proxy paths are ignored. This is the same as `HdsiLightLinkingSceneIndex` with per-instance categories (4.2).
6. Karma ignores a `membershipExpression` unless includeRoot is blocked or false (4.1).

### 4.4 Karma light properties related to sampling and linking (registry; meanings from the help, paraphrased)

| Property | Default | Meaning |
|---|---|---|
| `karma:global:light_sampling_mode` | Light Tree (0 Uniform, 1 Light Tree) | Light tree or uniform sampling. Dome, distant, point, filtered and spot lights are never in the tree. |
| `karma:global:light_sampling_quality` | 1 | A global multiplier on every light's quality: direct light and shadow samples. |
| `inputs:karma:light:samplingquality` | 1 | Per-light multiplier on the global quality: more samples for this light relative to the others. |
| `inputs:karma:light:samplingmode` | Auto (0 Auto, 1 Uniform, 2 Light Tree) | Keep a light out of the tree (uniform sampling), at a speed cost. |
| `inputs:karma:light:misbias` | 0 (−1 ... 1) | Shifts MIS between BSDF and light samples. |
| `inputs:karma:light:equiangular` | false | "Improve Volume Sampling (Equiangular MIS)". For lights inside or near thin volumes. The help warns of a slight hue bias with spectral opacity. Not for dome and distant lights. |
| `inputs:karma:light:activeradius` | −1 | Clamps the light's reach (no effect beyond the radius). |
| `inputs:karma:light:pointradius` | 0 | Softens the 1/d² singularity of point lights. |
| `inputs:karma:light:contribs` | "*" | Which BSDF components the light affects (diffuse, reflect, coat, sss, volume, combinations). |
| `inputs:karma:light:category`, `shadowtraceset` | "" | Karma's legacy categories / trace sets (no help text). |
| `karma:object:lightcategories`, `tracesets` | [] | The object side of the legacy categories. |
| `inputs:karma:light:lightgeocastsshadow` | false | Light geometry casts shadows. |
| `inputs:karma:light:contributescaustics` | true | Caustics need this on the light and on the object. |
| `karma:object:treat_as_lightsource` (+ `lightsource_*`) | No (0 / 1 / 2) | Section 3.2. |
| `karma:global:override_lighting` | Off (Emissive Objects / Headlight / Dome Light) | Lighting overrides for look development. |

Linking itself comes only from the UsdLux collections. No Karma-specific linking property is needed.

---

## 5. Equiangular sampling

- **Method** (lit: Kulla & Fajardo 2012, "Importance sampling techniques for path tracing in participating media").
  - For a point light at distance D from the ray, at parameter t₀ along it, sample the scattering distance t ∝ 1/(D² + (t − t₀)²) over the segment [a, b]: the angle θ is uniform in [θ_a, θ_b], with t = t₀ + D·tan θ.
  - pdf = D / ((θ_b − θ_a)(D² + (t − t₀)²)).
  - It cancels the 1/r² of the light exactly, which distance sampling cannot do in thin media.
  - For area lights, pick the point on the light first, then sample equiangularly towards it.
  - The paper combines it with distance sampling by MIS. In heterogeneous media the transmittance to the sampled point is then estimated separately.
- **Cycles** samples per volume shader with Distance, Equiangular or Multiple Importance (one-sample MIS of both) for direct light. In 5.0's null-scattering volumes it keeps equiangular plus distance MIS (lit, from memory; volume-algorithms 4.8).
- **Karma** has it per light, opt-in: `karma:light:equiangular`, default false (registry).
- **RenderMan**'s PxrVolume has an "Equiangular Weight" (lit, from memory; volume-algorithms 4.7).
- **Gains measured in M8** (volume-algorithms E6: homogeneous medium, point light): relative variance falls from 1.3-104 (distance sampling) to 1.6·10⁻⁵ - 0.14 (equiangular), and to 0.06-0.30 with one-sample MIS. The thinner the medium and the closer the light, the larger the gain.
- **The difficulty for Nova** (design).
  - Nova's free flights use spectral tracking with survival biasing (M8). The density with which a real scattering event lands at t is not available in closed form in heterogeneous media, because it involves the true transmittance.
  - MIS weights only need to be a **deterministic partition of unity** to stay unbiased. That is the same argument as for the volume lights' collision MIS (volume-algorithms E8).
  - So evaluate both strategies' weights with a deterministic proxy of the free-flight density: p̂_ff(t) = lum(σ_s(t))·exp(−∫₀ᵗ σ̄_avg), where σ̄_avg is a per-cell mean extinction stored with the majorant grid. In homogeneous media the proxy is exact.
  - The equiangular estimator then works like this:
    - for a segment through a medium and one light chosen by the tree (with the segment as the reference, as Cycles does), sample t equiangularly;
    - ratio-track T(0, t);
    - evaluate σ_s(t) and the phase function;
    - trace a shadow ray with ratio-tracked transmittance;
    - weight by p_ea²/(p_ea² + p̂_ff²).
  - The free-flight NEE at a real event at t, towards the same light, takes the complementary weight.
  - Unbiased: yes. Optimal: only where the proxy is close.

---

## 6. Recommendations for Nova

### 6.1 The light tree

| Option | For | Against |
|---|---|---|
| (a) keep the alias table (+ cap) | O(1), 30 ns | 22-1764x more variance (E1); BSDF rays still test every light |
| **(b) pbrt-v4-style light BVH, one light per leaf, one sample** | 22-1764x less variance; exact PMF by bit trail; the same boxes answer BSDF-ray queries (E4); simple, GPU-friendly | 0.4-0.9 µs per selection and per PMF; a build of about 1 s for 100k lights single-threaded |
| (c) Cycles-style min/max importance, multi-emitter leaves | sharper near clusters (lit) | more code; not measured here |
| (d) (b) + adaptive splitting | less variance per estimate | 0.23-0.83x efficiency per light sample (E7) |

**Recommended: (b)** (design). Details:
- **What goes in.** Everything bounded:
  - sphere, rect, disk and cylinder lights;
  - point lights (θ_o = π) and spot lights (cone);
  - volume lights (2.9);
  - emissive triangles (6.5);
  - lights expanded per instance (6.7).
  Domes and distant lights stay outside (6.2).
- **Importance.**
  - pbrt-v4's function with Nova's receiver bound: one-sided for closures without transmission (E8), two-sided when they transmit (Nova knows this from the closure's lobes), and none (n = 0) for volume events.
  - The d² clamp: use (half diagonal)², squared, not pbrt's unsquared form (proto note 2.1).
  - Store φ/M_Ω, the intensity, instead of φ (2.9).
  - Multiply by the light's selection weight: the larger of the diffuse/specular multipliers, as today (code `lights.cpp:295`), times `karma:light:samplingquality` (6.9).
- **Selection.**
  - Reuse `stream::kLight`'s z component, rescaled per level and clamped below 1 (E6).
  - A traversal that dead-ends (both children 0) returns no light. This replaces `chooseLight`'s alias draw; `LightSamplerView` becomes a view of the node array and the outside table.
- **Build and edits.**
  - Parallel SAOH build (TBB, 12 buckets).
  - Refit in `Scene::commit` for value edits and small moves; rebuild on added or removed lights, large moves (E3), or a changed receiver-set layout (6.6).
  - A 10k-light rebuild is about 70 ms single-threaded, well inside the IPR budget once parallel.
- **Layout.** As in 2.9, with the bit trail per light (8 bytes) and the link mask per node (8 bytes).

### 6.2 Lights at infinity

| Option | E5 |
|---|---|
| (a) pbrt: q = n_inf/(n_inf + 1) | 1.0-2.3x the oracle; worst when local light dominates |
| (b) Nova today: the power share, capped at ½ | 1.00-1.07x when the distant light gives ≤ 50 %; 2.2-4.9x when it gives 90 % |
| **(c) per receiver: q = I_inf/(I_inf + Σ_cut I_node)**, over a cut of 16 tree nodes | 1.04-1.43x everywhere |

**Recommended: (c)** (design), with I_inf in the same units as the tree's intensity-based importance (2.9):
- distant lights: E·cos⁺(n, ω);
- textured domes with a distribution: the irradiance of their light-sampled part, from the distribution's integral (`lightPower` already has it, code `lights.cpp:270-275`).
- Among the infinite lights, keep Nova's power alias table.
- The cut costs 16 importance evaluations (about 0.3 µs) per NEE, and the BSDF-hit PMF of an infinite light needs the same q.
- (b) is the fallback if profiling objects.

### 6.3 BSDF rays finding lights

| Option | For | Against |
|---|---|---|
| (a) brute force (today) | none | 2.3 µs per ray at 1k lights, 0.2-1 ms at 100k (E4) |
| **(b) traverse the light tree's boxes** (all crossings, slab test, stack) | 0.1-0.2 µs per ray (street, tris), 0.3-1.9 µs (rays inside a cloud); one structure; CUDA-portable | about 1.0-1.25x a dedicated BVH (1.8x in one case) |
| (c) a separate SAH ray BVH over light shapes | slightly faster (E4) | a second structure to build and update |
| (d) Embree user geometry in a separate light scene, collecting hits with a filter callback; OptiX custom primitives later | Embree's traversal | callbacks per crossing, a second scene, and different code on the GPU |

**Recommended: (b)** (design), used by `areaLightsAlongRay`, `areaLightCrossings` (volumes) and `nearestCameraLight`. The query returns (light, t, front) triples; the existing per-light code then applies the MIS weight with the tree PMF at the previous vertex (6.4).
- Lights still never block rays.
- **Emissive triangles need no query**: a BSDF ray finds them as the ordinary surface hit.

### 6.4 MIS and the integrator (design)

- **New path state.** The previous scattering vertex's normal (oct-encoded) and a "transmits" bit next to `bsdfPdf`. `rays.org` already holds the vertex's offset position.
- **The tree's PMF at that vertex replaces `c.lightSampler.pmf[i]`** in:
  - `areaLightsAlongRay` and `escapedRadiance` (infinite lights: q × the table PMF);
  - the volume trackers' light crossings and volume-light emission gathering (M8 `EmissionGather`);
  - the new emissive-triangle hits (6.5).
- **Unchanged.** Weights use the power heuristic; lights with selection probability 0 keep weight 1 (code `lights.h:436-450`). Delta events keep weight 1.
- **The external (VEX) pre-pass** draws the same light, because `chooseLight` stays deterministic per path (M5 D7).
- **LPE** (M6 D4): each light keeps its own event; emissive triangles of a treated mesh share one light id (the mesh's).

### 6.5 Emissive meshes as lights

- **Hydra.**
  - Read `karma:object:treat_as_lightsource` (0/1/2), `lightsource_diffusescale`, `lightsource_specularscale` and `lightsource_samplingquality` as constant primvars on meshes (as `hydra/volume.cpp:118-130` does for volumes).
  - Add Nova's own `nova:object:lightSource` with the same values, the counterpart of `nova:object:volumeLight`.
  - 2 (Auto) means "a light when the material emits", which matches every Karma measurement (3.3).
  - Karma also allows the property on materials (registry `material_vop`): optional.
- **Which triangles.** Those whose material slot emits:
  - the emission outputs are uniform and non-zero, or
  - they vary: an emission sub-program `Program::emission`, built like `Program::opacity`, is not identically zero.
- **Emitters.**
  - Per triangle and per instance, world space.
  - φ = π·lum(E)·A·2: two-sided, as Karma measured (3.3).
  - Bounds: two-sided cone about the face normal (θ_o = 0, θ_e = π/2, two-sided).
  - For varying emission, φ from the sub-program evaluated at the centroid and the three corners at commit. Any estimate is unbiased (2.9).
  - Warn above a budget (for example 2 M emitter triangles); a two-level tree comes later.
- **NEE.**
  - Area sampling below a solid-angle threshold, spherical-triangle sampling above (pbrt-v4's thresholds as for rects).
  - The light sample stores (mesh, instance, primitive, barycentrics) with the shadow ray.
  - **Emission is evaluated only for unoccluded shadow rays**, in a short pass after `traceShadowRays`, batched per emitting material. Uniform emission skips the VM. The MIS weight needs only PDFs, so pass C can finish the weight before the emission is known.
- **BSDF hits** on a treated emitter: the hit's emission is MIS-weighted with the tree PMF at the previous vertex times the triangle's solid-angle PDF.
- **Untreated emitters** keep weight 1, as today.
- **LPE**: treated emitters are L events everywhere, including the camera's direct view (Karma: `CL`), with the object's lpetag as the light tag. Untreated ones stay O events (code `wavefront.cpp:1400-1408`).
- **Multipliers**: the two scales act as the light's diffuse/specular multipliers on receivers. The camera's view stays unscaled, as for volume lights (M8 D13).
- **Light-only rays** (past ray limits) gather treated emitters as lights, as for volume lights (research H 4.8).

### 6.6 Light linking

**Hydra** (design):
- Append `HdsiLightLinkingSceneIndex` in Nova's scene-index plugin, linking `pxr_hdsi`.
  - `lightPrimTypes`: Nova's six light types.
  - `geometryPrimTypes`: mesh, sphere, volume (curves and points when M9 adds them). **Not `instancer`** (4.2).
- `HdNovaLight` reads `GetLightParamValue(HdTokens->lightLink / shadowLink)`: a category token, empty for everything. It re-syncs on `DirtyCollection`.
- Rprims and instancers read `GetCategories` and `GetInstanceCategories` on `DirtyCategories`.
- A Nova object instance's categories are the union of its rprim's, its instancer chain's and its own instance entry (probe 4.2).

**Core: receiver sets** (design, after Cycles):
1. Every category token used by any light gets a small id.
2. Every object instance (mesh or volume instance) maps its category set to a **receiver set**, numbered by distinct combination, with 0 = no categories. Light *i* illuminates receiver set *r* if the light's `lightLink` category is empty or contained in *r*'s categories.
3. Per light, a 64-bit mask of the receiver sets it illuminates. Per tree node, the OR of its emitters' masks.
4. Importance of a child = 0 if `(child.mask >> r) & 1` is 0. Selection and the PMF then only ever see linked lights: nothing is wasted, and the PMF stays exact.
5. Beyond 64 receiver sets, fall back to selection without masks plus a rejection test: unbiased, only wasteful.

The receiver set id per instance goes next to `MeshRecord::instances` (and `VolumeRecord`). The hit gives (mesh, instance) → r.

**Where the integrator checks** (design):

| Path event | Check |
|---|---|
| NEE at a surface or volume event | Selection with the receiver's set *r* (the mask). |
| BSDF/phase ray reaching an area light, escaping to a dome or distant light, hitting a treated emitter, or gathering volume-light emission | Contribution 0 unless the light's mask contains the **previous vertex's** set. Store *r* per path, 1 byte. Karma measured: the metal highlight is gone. |
| Shadow rays | An occluder *o* blocks light *i* only if *o* is in *i*'s `shadowLink` set. Shadow rays carry the light's shadow category; objects excluded from some shadow set get an occlusion-filter flag (like Nova's `filtered` meshes), and their filter ignores the hit for that category. Volumes: skip the transmittance of excluded volumes. |
| **BSDF rays and shadow-linked lights** | Karma's result has no residual shadow (4.3, item 3). Nova's continuation ray would stop at the excluded object and miss the light behind it. **Recommended: lights with a non-trivial `shadowLink` are NEE-only:** `bsdfSampleWeight` 0 and `lightSampleWeight` 1 for them. This is unbiased and costs only some variance on glossy receivers. **Delta events** keep weight 1, so a mirror's reflection of such a light is still occluded by excluded objects. Document it, or trace a light-only probe ray past excluded hits (later). |
| Camera rays seeing lights (`renderlightgeo`) | No linking: the camera is not a receiver (not measured). |
| Indirect light | Nothing: linking acts only on the light's direct contribution at the receiver (Karma: indirect key light reaches unlinked objects). |
| Light-only rays past ray limits | As BSDF rays: the stored receiver set of the vertex that spawned them. |

### 6.7 Instanced lights (design)

- `HdNovaLight::Sync` checks `GetInstancerId(id)`. If one is set, it computes the instance transforms through Nova's instancer (`HdNovaInstancer`, as meshes do) and creates one `LightDesc` per instance (world transform = instance × light).
- Re-sync on `DirtyInstancer`.
- Instance-rate light primvars are optional.
- Karma lights every instance of a point-instanced light (measured 4.3). Native instances arrive the same way (probe 4.2).

### 6.8 Volume lights and equiangular sampling (design)

- Volume lights join the tree as emitters (2.9). Their `sampleVolumeLight` and MIS against collision emission stay as built in M8.
- **Equiangular**: per light, opt-in through `inputs:karma:light:equiangular` (default false, as Karma) and a Nova override.
  - Point, spot, sphere and small area lights; not domes or distant lights.
  - The estimator and the deterministic-proxy MIS of section 5.
  - Its own PCG stream (`stream::bounce(depth, ...)` is full, so use the M8 D18 scheme).
- Do it after the tree and linking. The test is M8 E6's homogeneous single-scattering cases against the closed form.

### 6.9 Karma light properties to honour (design)

- **Read.**
  - `inputs:karma:light:samplingquality` (default 1): multiplies the light's selection weight in the tree or the outside table.
  - `karma:global:light_sampling_quality` (default 1): multiplies every light's weight equally, which changes nothing in one-sample selection. Read it, document it as a no-op, or later map it to extra NEE samples at camera hits; E7 says extra samples per vertex do not pay.
  - `inputs:karma:light:samplingmode` Uniform (1): put the light into the outside alias table with the infinite lights.
  - `karma:global:light_sampling_mode` Uniform (0): no tree, which keeps today's alias table as a debugging mode.
- **Later.** `inputs:karma:light:misbias` (shift the power-heuristic weights), `activeradius`, `pointradius`, `contribs` (per-lobe multipliers, as Nova already has diffuse/specular).
- **Ignore with a warning.** `category`/`shadowtraceset` and `karma:object:lightcategories`: Karma's legacy linking, superseded by the collections.

### 6.10 Order of work (design, in the style of docs/design/M7.md)

1. **M9.L1 Light tree**: build, refit, importance, selection, PMF, the outside table with q against a cut, `chooseLight`, MIS through the previous vertex, debug mode Uniform. Tests: 6.11 (1)-(4).
2. **M9.L2 Ray queries through the tree**: `areaLightsAlongRay`, `areaLightCrossings`, `nearestCameraLight`. Tests: identical images to brute force (bit-exact or within noise); timing at 10k lights.
3. **M9.L3 Instanced lights** (6.7). Test: the Karma `light_instancer` values.
4. **M9.L4 Light linking** (6.6): Hydra scene index, receiver sets, masks, shadow filter, NEE-only shadow-linked lights. Tests: section 7.1 against Karma.
5. **M9.L5 Emissive meshes as lights** (6.5). Tests: section 7.2 against Karma, variance gain.
6. **M9.L6 Volume lights in the tree, equiangular, Karma light properties** (6.8, 6.9).

### 6.11 Tests (design)

1. **Unbiasedness of selection**: on the lab's three scenes (without occlusion, as `nova_tests` unit tests with Nova's light types), the tree's one-sample estimate equals uniform selection's mean within 3σ (z as in E1), for spheres, rects, disks, cylinders, points, spots and emissive triangles.
2. **PMF consistency**: Σ PMF ≤ 1, and equal to the fraction of draws that return a light; frequency test χ²/dof within [0.9, 1.1] (E2); the PMF from the bit trail equals the sampled PMF to 1e-5.
3. **Variance gain** on street 10k: relVar ≤ 1/100 of the power table's (E1: 1/485), as a regression gate on the selection code.
4. **MIS**: furnace-style images with LightOnly / BsdfOnly / MIS strategies agree within noise on scenes with 1k sphere and rect lights (the existing `LightStrategy` debug modes, code `light_strategy.h`).
5. **IPR**: an intensity edit refits (no rebuild); moving a light across the scene rebuilds; images equal a fresh render within noise.
6. **Karma comparisons**: section 7, through husk at 256 samples, red-channel region means within 2 % or 3σ.

---

## 7. Test scenes with Karma reference values

All are generated at run time by the probe scripts (hand-written USDA, Karma CPU, 256 path-traced samples, box filter, colour limits 10⁹). Values are red-channel region means (measured).

### 7.1 Light linking (`probes/lt_karma_link.py`)

| Configuration | Region | Karma | LPE |
|---|---|---|---|
| base | A top / B top / open ground | 0.02938 / 0.02946 / 0.02268 | `C<RD><L.'key'>` |
| key lightLink = {A} | B top / open ground / A top | 0 / 0 / 0.02943 | same |
| | ground between spheres, indirect | 0.00109 (base 0.00204) | `C<RD>.+<L.'key'>` |
| key lightLink excludes A | shadow of A / A top | 0 / 0 | same |
| key shadowLink excludes B | shadow of B | 0.01787 (no shadows at all: 0.01787) | same |
| 3 × 3 key, shadowLink excludes B | shadow of B | 0.15904 (no shadows: 0.15909; base 0.00719) | same |
| metal B, key lightLink excludes B | key on B (max) | 0 (unlinked: 16.42) | `C<R[GS]><L.'key'>` |
| dome lightLink = {A, B} | ground under the dome | 0.00116 (base 0.14614) | `C.*<L.'dome'>` |
| key lightLink = {ni1, pi} | ni1 / ni2 / pi0 / pi1 | 0.13571 / 0 / 0.37018 / 0.37028 | `C<RD><L.'key'>` |
| key lightLink excludes /World/ni2/m (proxy) | ni2 | 0.13569 (ignored) | same |
| membershipExpression with includeRoot blocked | B top | 0 | same |
| point-instanced sphere lights | shadow regions A / B | 0.16627 / 0.16635 | `C.*<L.'pil'>` |

### 7.2 Emissive meshes (`probes/lt_karma_emit.py`)

| Configuration | Ground near sphere / far / near quad / far | Camera on the emitters | Notes |
|---|---|---|---|
| treat 0 | 0.1004 / 0.0202 / 0.1023 / 0.0148 | `CO` 2.000 | rel. std at 16 spp: 0.46 / 3.13 / 0.43 / 2.14 |
| treat 1 or 2 | 0.0998 / 0.0185 / 0.1021 / 0.0147 | `CL` 2.000 | rel. std: 0.23 / 0.26 / 0.13 / 0.12 |
| treat 1, diffuse scale 2 / 0 | 0.2009 / 0.0381 / 0.2049 / 0.0295, or 0 | 2.000 | |
| treat 1, glossy ground, specular scale 1 / 0 | glossy `C<R[GS]>L` 0.0020 / 0 | | |
| Auto on every tested emitter | | `CL` | 3.3 |

Nova's targets:
- the same means;
- the same L/O split;
- at least 5x lower relative noise at the far regions with treatment 1 than with 0, at 16 samples.

---

## 8. Open questions

- **Karma's tree internals.** Leaf size, importance and splitting cannot be observed. The registry says spots and point lights stay outside its tree; Nova includes them (6.1). The images converge to the same result.
- **Auto heuristic.** Every tested emitter became a light (3.3). Whether Karma ever declines (very many emitters, zero emission in parts of a texture) is unknown. Nova's "when the material emits" rule matches every observation.
- **Linking of emissive-mesh lights and volume lights**, and volumes as linked receivers, were not probed in Karma. Expected: the same semantics, through the object's categories. Test once Nova implements them.
- **Shadow-linked lights and delta paths.** Karma's mirror reflection of a light behind a shadow-excluded object was not measured. Nova's NEE-only rule (6.6) differs there if Karma does something else.
- **The camera's view of linked lights** (`renderlightgeo` on a light linked away from everything) was not measured.
- **Light filters** (`lightFilterLink`, Karma blocker filters) are out of scope.
- **Scene-index registration.** A plugInfo `loadWithRenderer` entry alone appended the probe's plugin (4.2). Check whether hdNova's implicit-surface plugin, which also registers explicitly, runs once, and with which arguments. For example, count `_AppendSceneIndex` calls under `TF_DEBUG`.
- **Mixed emitter normalization** (φ/M_Ω, 2.9) and the min/max blend of Cycles were not measured. They are a lab extension if M9.L1's tests show poor selection on mixed scenes.
- **Two-level trees for instanced emissive meshes**: flattening is the M9 plan. Measure memory and build time on a heavily instanced emitter before deciding.

## 9. Files (`docs/research/m9/probes/`)

| File | Purpose |
|---|---|
| `lt_light_lab.cpp`, `lt_build.bat` | The stand-alone light-selection lab (E1-E8). Build: `lt_build.bat <outdir>`; run: `lt_light_lab.exe [e1..e8|all] [threads]`. E1 takes about 6 min, all experiments about 15-20 min on 8 threads. |
| `lt_out_light_lab.txt` | Output of E1-E8 (the E4 numbers are from a quiet re-run) |
| `lt_link_authoring.py`, `lt_out_link_authoring.txt` | What the Light Linker LOP authors (4.1); isolated hython |
| `lt_link_probe/` | HDK-free probe delegate "LinkProbe" (CMake, plugInfo, UsdRenderers.json). Build: `cmake -S docs/research/m9/probes/lt_link_probe -B <dir> -G "Visual Studio 17 2022" -A x64` and `cmake --build <dir> --config Release`; staged package in `<dir>/pkg`. `LT_LINK_SI` = none / hdsi / houdini selects the scene index. |
| `lt_link_hydra.py`, `lt_out_link_hydra_{husk,inprocess}_*.txt` | The scene for the probe delegate and its logs in husk and in-process (4.2): `run_hython.ps1 -Script lt_link_hydra.py -Stage <dir>/pkg <outdir> husk`, or `inprocess` with `LT_LINK_SI` set |
| `lt_karma_link.py`, `lt_out_karma_link.txt` | Karma light/shadow linking measurements (4.3, 7.1); about 1 min |
| `lt_karma_emit.py`, `lt_out_karma_emit.txt` | Karma emissive meshes, treat_as_lightsource, Auto (3.3, 7.2); about 1.5 min |

The probe outputs replace scratch paths with `<out>` / `<scratch>`. No SideFX code, help text or Houdini-generated network is committed: the scenes are hand-written USDA, and the Light Linker network is built and discarded at run time.

## 10. References

- A. Conty Estevez, C. Kulla. Importance Sampling of Many Lights with Adaptive Tree Splitting. Proc. ACM Comput. Graph. Interact. Tech. (HPG) 1(2), 2018.
- M. Pharr, W. Jakob, G. Humphreys. Physically Based Rendering, 4th ed., and pbrt-v4 `src/pbrt/lightsamplers.{h,cpp}` (BVHLightSampler, LightBounds, CompactLightBounds).
- Blender Cycles: `src/kernel/light/tree.h`, `src/scene/light_tree.{h,cpp}`; Blender 3.5 release notes (light tree, emission sampling), Blender 4.0 (light linking).
- P. Moreau, M. Pharr, P. Clarberg. Dynamic Many-Light Sampling for Real-Time Ray Tracing. HPG 2019.
- J. Arvo. Stratified Sampling of Spherical Triangles. SIGGRAPH 1995.
- C. Ureña, M. Fajardo, A. King. An Area-Preserving Parametrization for Spherical Rectangles. EGSR 2013.
- C. Kulla, M. Fajardo. Importance Sampling Techniques for Path Tracing in Participating Media. EGSR 2012.
- OpenUSD 26.05: UsdLux (LightAPI collections lightLink/shadowLink), UsdCollectionAPI (includeRoot, expansionRule, membershipExpression), `pxr/imaging/hdsi/lightLinkingSceneIndex.h`, `pxr/imaging/hd/{categoriesSchema,instanceCategoriesSchema,lightSchema,collectionsSchema}.h`.
- Nova: `docs/research/m8/volume-algorithms.md` (E6 equiangular, E8 volume lights), `docs/research/m8/houdini-volumes.md` (4.8 volume lights in Karma).
