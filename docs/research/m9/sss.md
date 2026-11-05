# M9 research: random-walk subsurface scattering, Karma's conventions and the design for Nova (Houdini 22.0.466)

Scope: docs/PLAN.md M9, "SSS Random Walk ... Feature-Parität mit dem Arnold-Kern". This report answers three questions:

1. **Algorithms.** How production renderers do random-walk SSS: albedo inversion, mean free path, entry and exit boundary, Dwivedi guiding, anisotropy, chromatic mean free paths, step caps and thin geometry. A standalone lab measures what matters for Nova.
2. **Parametrization and Karma's behaviour.** The MaterialX/OpenPBR parameters and Karma's own properties, and Karma CPU renders that pin down its model: inversion, boundary, size and radius dependence, anisotropy, limits, LPEs, thin walls. Nova renders the same scenes.
3. **Recommendations for Nova.** Algorithm, inversion, boundary model, how the walk fits the wavefront integrator, sampling streams, ray limits, LPE events and tests with reference values.

Sources:
- the Houdini install (`$HFS` = `C:\Program Files\Side Effects Software\Houdini 22.0.466`): MaterialX libraries (`$HFS/houdini/materialx/libraries`), Karma's VEX MaterialX implementation (`$HFS/houdini/vex/include/mxpbr.h`), the property and node registries (`$HFS/houdini/RenderPropertiesKA.json`, `karmaShaderNodes.json`) and the help archives (`$HFS/houdini/help/*.zip`, described in my own words);
- Nova's code (read only);
- Karma CPU and Nova renders through husk;
- the prototype `probes/sss_lab.cpp`;
- the literature, partly from memory (marked).

Tags: **(measured)** Karma or Nova renders, with the scene name; **(lab E#)** the prototype's experiments; **(header file:line)**; **(registry)**; **(help page)**; **(code file:line)** Nova; **(lit)** literature or open-source code; **(lit, from memory)** not re-checked against the source in this research; **(inference)** a conclusion drawn from measurements, not observed directly; **(design)** a proposal.

Every Houdini process ran isolated: husk through `docs/research/m6/probes/husk_env.ps1` (own preference directory, no user packages, the installed Nova package skipped). The user's Houdini session was not touched. Nova was the staged package `build/vs2022/stage/Release/nova`, read only, not rebuilt. Machine: AMD Ryzen 7 9800X3D, 8 cores. Lab built with MSVC 14.44, `/O2 /arch:AVX2`. Probes are in `probes/` (section 9). They contain no SideFX code, and all scenes are generated as plain USDA at run time.

---

## 0. Findings in short

1. **Karma's MaterialX subsurface is a random walk with the OpenPBR 2025 parametrization** (header `mxpbr.h:255-279`, `1024-1040`).
   - `ND_subsurface_bsdf` becomes a `cvex_bsdf("randomwalk_eval", "randomwalk_sample", "label", "sss", "absrp", σa, "scatr", σs, "g", anisotropy)`.
   - The extinction is σt = 1 / mfp per channel, with mfp = `subsurface_radius · subsurface_scale` (Standard Surface) or `subsurface_radius_scale · subsurface_radius` (OpenPBR).
   - The single-scattering albedo comes from the exact inverse of van de Hulst's half-space albedo, with a similarity correction for the HG anisotropy g: s = 4.09712 + 4.20863·A − √(9.59217 + 41.6808·A + 17.7126·A²), α* = 1 − s², α = α* / (1 − g(1 − α*)). A is the subsurface colour.
   - Chiang et al. 2016's fit is present but disabled (`#if 0`, `mxpbr.h:262-269`).
2. **Karma's boundary model is: diffuse (cosine) entry, an index-matched walk without internal reflection, and a Lambertian exit.** The lab model with exactly these choices reproduces every Karma measurement to within 0.1-0.4 %, and a normal-incidence (refracted) entry is ruled out (measured, lab E1/E5/E6):
   - **White furnace, thick sphere, A = 0.2 / 0.5 / 0.8:** Karma 0.2028 / 0.5019 / 0.8001 (`f_a1`). Lab, cosine entry: 0.2025 / 0.5016 / 0.8015. Lab, normal entry: 0.1935 / 0.4896 / 0.7941.
   - **Transmission through slabs** of 0.5-8 mean free paths: Karma equals the lab's (T + unscattered)/π, for example 0.0877 / 0.1187 / 0.1234 against 0.0881 / 0.1192 / 0.1239 at 2 mfp (`slab_d0p1`).
   - **Entry direction:** at 60° incidence, transmission scales exactly with the irradiance, 0.500 × the normal value (`slab_a60`). An emissive card behind a thin slab shows no ballistic image (`ball_d05`).
   - **Exit:** under a light from the camera's direction the radiance follows A·μ/π: Karma equals Nova's exact Lambert render of the same scene within 2 % in every μ bin, so the exit is Lambertian (`head`).
   - **The interface:** a specular or coat layer scales the SSS by (1 − E_spec) on the viewing side only, 0.944 for specular 1. There is no Fresnel at the walk's far exit (`f_spec`, `f_coat`, `slab_spec`).
3. **The inversion makes the colour the furnace albedo of a thick object, within 0-2.4 %, but only with cosine entry** (lab E1). It holds for g from −0.5 to 0.9 thanks to the similarity correction: R/A is 0.997-1.045. With normal entry R/A drops to 0.59-0.997, and with a Fresnel boundary of IOR 1.5 to 0.35-0.99. In Karma: g = +0.5 gives 0.2006 / 0.5016 / 0.7966 and g = −0.5 gives 0.2094 / 0.5047 / 0.8015 (`f_gp5`, `f_gm5`).
4. **Karma kills walks after about 256 segments, and high albedos lose energy.**
   - A furnace with A = 0.95 / 0.99 renders 0.9118 / 0.9202 in Karma (`f_a2`). The lab's half space, cut after 256 segments, gives 0.9101 / 0.9184. Uncut, it gives 0.9501 / 0.9897.
   - The loss is a property of the paths, not of the sampling: Dwivedi-guided walks lose exactly the same energy at the same cap (lab E4).
   - It is 0.1 % at A = 0.8 and g = 0, but 11 % at A = 0.8 and g = 0.9 (Karma 0.7207, `f_gp9`).
   - Cycles' `BSSRDF_MAX_BOUNCES` is also 256 (lit, from memory).
5. **Size and radius behave as a physical medium.**
   - In the furnace a sphere of radius 1 with A = 0.2 renders 0.2028 / 0.2106 / 0.2335 / 0.3273 / 0.6197 / 0.8445 at radius/mfp = 1000 / 33 / 10 / 3.3 / 1 / 0.33. The lab gives 0.2025 / 0.2108 / 0.2334 / 0.3272 / 0.6195 / 0.8443 (measured `f_s*`, lab E5).
   - Light bleeds into the shadowed half of a sphere lit from the side. At mfp 0.2 the dark side still reads 0.004-0.04 within 0.5 units of the terminator; at mfp 0.01 it is black 0.13 units past it (`term_*`, section 4.4).
6. **One walk is one SSS event** (measured `room_*`, `f_lim*`). A camera inside a closed room of emissive SSS walls (walk albedo A') measured:

   | `ssslimit` | radiance | `C<TD>O` | `C<TD><TD>O` |
   |---|---|---|---|
   | 0 | 1 | | |
   | 1 | 1 + A' (0.2514 / 0.4998 / 0.7466) | A' | 0 |
   | 2 | 1 + A' + A'² | A' | A'² |

   - Past the limit the walk still runs; its exit gathers lights only, so a furnace is unchanged at `ssslimit` 0.
   - The diffuse limit has no effect, so the exit's continuation is in the SSS class.
   - In LPEs a walk is a single `<TD>` event labelled `sss`: `C<...'sss'>.*[LO]` and `C<TD>.*[LO]` hold all of it, and `C<RD>.*` and `C<TG>.*` hold none.
   - `C<TD'sss'>L` is 0, because the third position is the object tag. The label is written `C<TD.'sss'>L`, as Nova's M6 dialect already does.
7. **Thin-walled subsurface is a Lambert translucency with the subsurface colour and no walk** (0 probe rays). Radius, scale and anisotropy are ignored: the transmitted radiance is color·E/π, 0.1592 / 0.2546 / 0.3024 (`slab_thin`).
8. **Open meshes act as a half space** (measured `open_quad`). A probe ray that misses means no boundary in that direction, and the walk goes on. A single SSS quad renders exactly like a thick slab: 0.1596 / 0.2541 / 0.2894 against 0.1596 / 0.2541 / 0.2893.
9. **Karma's properties** (registry):
   - `karma:object:ssslimit`: 1.
   - `karma:object:sssquality`: 1, a multiplier on secondary samples; no effect in path-traced mode.
   - `karma:object:sss_traceset`: "". Without a traceset a walk traces only its own object; with one, every object that shares the name. In XPU this is at most 4 tracesets per scene.
   - `karma:global:ssscomponents`: "sss".
   - `sss` also appears in `exportcomponents`, `guiding_components`, the light `contribs` menu and the `rendervisibility` menu.
   - There is no `kma_*` SSS node. The only SSS terminals are MaterialX `subsurface_bsdf` (inside Standard Surface, OpenPBR and Disney principled) and LamaSSS.
10. **Karma's walk is cheap but noisy** (measured, inference).
    - It traces about 7 probe rays per walk for A = (0.2, 0.5, 0.8) (`f_a1`), against 35 for a grey 0.8 (`f_mix`). The lab's analog walk needs 46 segments for 0.8.
    - The noise per sample of Karma's 0.8 channel in `f_a1` is 4.3x the lab's (relative standard deviation 1.6 against 0.38), but normal in `f_mix`.
    - Both fit Russian roulette on an average of the channels' throughput. Chromatic radii raise the noise again (`f_chroma`: relative standard deviation 1.1-2.2), consistent with one walk and spectral MIS rather than three walks.
11. **Lab results that decide the design.**
    - **Dwivedi guiding towards the entry surface** pays 2-110x in efficiency in a half space (α 0.5-0.999), but costs up to 50x in a sphere of radius 3 mfp and still loses at 30 mfp for α = 0.999 (lab E2). Guiding needs Cycles' thickness-aware forward/backward choice; it is an optimization, not a requirement.
    - **Chromatic mean free paths in one walk:** per-step channel choice by throughput (Cycles) and a per-walk hero channel with path-level MIS are both unbiased and about equally good. They are 4-7x less efficient than three separate walks in a half space and with skin-like radii, and 50-100x in a small object from which almost all light escapes. A uniform per-step choice is unusable: its standard deviation reaches 39 (lab E3).
12. **Nova today** (code `core/bsdf/surface_models.h:386-389, 623-640`) renders subsurface as a Lambert lobe with the subsurface colour. Its SSS class, limit counting and LPE events (`sss`, `C<TD>O`, `C<TD><TD>O`) already equal Karma's in the room probe.
    - **Where it matches:** thick objects at moderate albedo (−1.4 % to 0 at A 0.2-0.8).
    - **Where it differs** (measured, section 4.7):
      - +4.2 / +7.6 % at A 0.95 / 0.99 and +11 % at g 0.9, where Karma's cap darkens;
      - small objects in the furnace are 11-76 % too dark;
      - nothing passes through slabs, and no light bleeds past the terminator.
13. **Recommendation in short** (section 5): Karma's exact model.
    - A cosine entry, an index-matched walk with HG phase, σt = 1/mfp and the van de Hulst/OpenPBR inversion, and a Lambertian exit.
    - A cap of 256 segments, killing the walk when it is reached.
    - One walk per path, with the channel chosen per step by throughput and balance-heuristic spectral MIS (an exact single-channel walk when the radius is achromatic).
    - Closest-hit queries against the hit mesh instance's own BLAS, through a new `IAccel::intersectLocal`.
    - A walk stage between shading and shadow rays that runs each walk to completion, the shape of a single CUDA kernel in M10.
    - The first uniforms from stream `kWalk`, the rest from a PCG stream.
    - One SSS event per walk with Karma's limit semantics and LPE `<TD.'sss'>`.
    - Dwivedi guiding and an optional higher cap later.

---

## 1. Nova today (code)

| Where | What |
|---|---|
| `core/bsdf/surface_models.h:315, 386-389` | Standard Surface: the subsurface share `S` is `pushLambert(w·S, subsurface_color^γ, thin)` with `l->sss = true` (thin-walled: a translucent lobe). Radius, scale and anisotropy are read but unused. |
| `core/bsdf/surface_models.h:623-640` | OpenPBR: thick objects get the same Lambert fallback; thin-walled objects get OpenPBR's diffuse pair ½(1−g)C·f_R + ½(1+g)C·f_T. |
| `core/bsdf/surface.cpp:31-33, 46-61` | Defaults; `subsurfaceRadius × subsurfaceScale` is the mean free path in both models (OpenPBR's `radius_scale` is stored as `subsurfaceRadius`). |
| `core/bsdf/closure_graph.h:31`, `graph_closure.h:199` | The generic graph leaf `Subsurface` keeps only weight and colour (radius and anisotropy are dropped). |
| `hydra/material/principled.cpp:408-412` | The Principled Shader maps its SSS onto Standard Surface: colour = base colour, radius = `ssscolor`, scale = `sssdist`. |
| `core/bsdf/closure.h:34, 53, 59, 163-168, 186-187, 202-203` | `RayClass::Sss`, `LobeLabel::Sss` (name `sss`), the `sss` flag, the event class, and the LPE convention (SSS is `T` on either side). |
| `core/scene/ray_limits.h:16-21` | `LimitClass::Sss`, Karma's default 1 in the delegate. |
| `core/math/sobol.h:152` | `stream::kWalk = 6`, reserved per bounce for M9. |
| `core/integrator/wavefront.cpp:1327-1588` (`shadeHit`) | NEE with one light (1422), then one closure sample (1493). The class's limit decides between a full ray, a thin-wall pass-through and a light-only ray (1508-1530). Then the next ray is pushed (1579). There is no queue between shading and the next bounce other than the shadow queue. |
| `core/accel/accel.h`, `embree_accel.cpp` | One BLAS (`RTCScene`) per mesh, instanced into the TLAS by an Embree instance array whose geomID is the MeshId. The `IAccel` interface has only scene-wide `intersect`/`occluded`. |
| `core/integrator/shading_queries.cpp:258` | The rounded-edge query keeps "the object itself" by discarding other objects' closest hits. That is not usable for a walk, because another object inside the SSS mesh would stop it. |

Nova's fallback already reproduces Karma's event semantics. In the room the Lambert lobe is one `Sss` event per bounce with the `sss` label, which is exactly the walk's count (section 4.6).

---

## 2. Algorithms (question 1)

### 2.1 The random walk

A random walk treats the inside of a closed mesh as a homogeneous medium: extinction σt, single-scattering albedo α (σs = α·σt, σa = σt − σs) and a phase function, usually Henyey-Greenstein with g.

A walk proceeds in four steps:
1. It enters at the shading point in a direction chosen by the **entry model**.
2. It repeatedly samples a free flight t ~ σt·e^(−σt t).
3. It traces a closest-hit ray **against the same object only** up to t. If the ray hits the surface first, the walk exits there; otherwise it scatters (direction from the phase function, weight ×α).
4. At the exit the **exit model** decides the outgoing radiance. The path then continues from the exit with NEE and a new direction.

No diffusion profile, no BSSRDF table and no probe-ray importance sampling is involved. The walk is exact for the medium it models, also on thin and concave geometry, at the cost of noise and ray queries. (lit: Chiang et al. 2016; Wrenninge et al. 2017)

### 2.2 Production variants

| Renderer | Entry / exit | Parametrization | Notes |
|---|---|---|---|
| Disney Hyperion (Chiang, Kutz, Burley 2016) | (boundary not re-checked) | albedo α(A) = 1 − e^(−5.09406A + 2.61188A² − 4.31805A³), σt = 1/(d·s(A)), s = 1.9 − A + 3.5(A − 0.8)², so that the walk matches Burley's normalized diffusion with the same "d" | Karma ships this fit disabled (header `mxpbr.h:262-269`). (lit, from memory; header) |
| Pixar RenderMan PxrSurface "path traced" (Wrenninge, Villemin, Hery 2017) | | Chiang/Burley-style inversion. Anisotropic HG, and "non-exponential" free flights that compensate the missing low-order scattering of an exponential medium | Also offers the diffusion models (dipole, better dipole, Burley, multiple mfp). (lit, from memory) |
| Arnold `randomwalk` and `randomwalk_v2` | `randomwalk_v2` accounts for the specular boundary (internal reflections) and is advertised as more accurate on thin and concave shapes but slower | `subsurface_radius·subsurface_scale` as the mean free path, `subsurface_color` as the target albedo. The two versions use different albedo mappings | Plus `diffusion` (empirical BSSRDF). (lit, from memory; not verified here) |
| Blender Cycles `random_walk` and `random_walk_skin` (3.0+) | `random_walk`: cosine entry into the surface, Lambert exit. `random_walk_skin`: a mix of diffuse and refracted entry with the subsurface IOR | σt = 1/(r(1 − g)) and a fitted α(A, g) (two arctangent terms blended by A^0.25) | Local closest-hit queries restricted to the object (`scene_intersect_local`). Per-step channel choice by throughput·albedo with spectral MIS. Dwivedi guiding (Meng et al.) with a forward/backward choice from a thickness probe and one-sample MIS. At most 256 bounces (`BSSRDF_MAX_BOUNCES`), then the walk is dropped. (lit, from memory) |
| pbrt-v4 | `SubsurfaceMaterial` is a tabulated BSSRDF (photon beam diffusion), sampled by probe segments between two points of the same material | `SubsurfaceFromDiffuse` inverts the table's effective albedo (ρ_eff) for a given mean free path | pbrt-v4 has no random-walk BSSRDF. The "volumetric" alternative is a dielectric interface with a homogeneous medium inside, through its volumetric path tracer: exact, with Fresnel at both ends and refraction, but slower and with a different look. (lit, from memory) |
| **Karma CPU 22.0** | **cosine entry, index-matched walk, Lambert exit** (measured, section 4) | σt = 1/mfp; α from the van de Hulst inverse with the OpenPBR 2025 similarity correction (header `mxpbr.h:270-279`) | Walk only within the object or its `sss_traceset`. About a 256-segment cap. One SSS event per walk. (measured) |

The OpenPBR 2025 course notes (cited in `mxpbr.h:271`) define `subsurface_color` as the multiple-scattering albedo, `subsurface_radius` and `radius_scale` as the mean free path, and the van de Hulst inversion. Karma follows them literally. (header; lit)

### 2.3 Albedo inversion (lab E1)

The lab (`probes/sss_lab.cpp`, output `probes/sss_out_lab.txt`) traces walks in an index-matched half space, so σt only sets the length scale and R depends only on α, g and the entry model. Each row inverts the target A to α and measures the total diffuse reflectance R (scale 0.5: 2 M walks per entry, standard errors ≤ 0.0003):

| A | α (vdH/OpenPBR) | R, normal entry | R, cosine entry | R, cosine entry, Fresnel exit η 1.5 | R, dielectric diffuse illumination η 1.5 |
|---|---|---|---|---|---|
| 0.05 | 0.218489 | 0.0390 (0.780 A) | 0.0512 (1.023 A) | 0.0177 (0.353 A) | 0.0138 (0.276 A) |
| 0.20 | 0.613357 | 0.1616 (0.808) | 0.2021 (1.010) | 0.0816 (0.408) | 0.0653 (0.326) |
| 0.50 | 0.911709 | 0.4373 (0.875) | 0.5008 (1.002) | 0.2767 (0.553) | 0.2328 (0.466) |
| 0.80 | 0.990589 | 0.7587 (0.948) | 0.7996 (1.000) | 0.6216 (0.777) | 0.5478 (0.685) |
| 0.95 | 0.999505 | 0.9378 (0.987) | 0.9500 (1.000) | 0.8899 (0.937) | 0.8025 (0.845) |
| 0.99 | 0.999981 | 0.9874 (0.997) | 0.9901 (1.000) | 0.9768 (0.987) | 0.8859 (0.895) |

- **The van de Hulst formula is the half-space albedo under diffuse illumination of an index-matched medium.** It is accurate to 2.3 % at low albedo and 0.1 % above A = 0.5. The formula itself against the measured R with cosine entry: α 0.3 gives 0.0729 against 0.0745, α 0.9 gives 0.4772 against 0.4778, α 0.999 gives 0.9297 against 0.9296. So the inversion is exact enough only for cosine entry. (lab E1)
- **Anisotropy.** With the similarity correction α = α*/(1 − g(1 − α*)) and σt = 1/mfp, R with cosine entry stays within −0.3 % and +4.5 % of A for g ∈ {−0.5, 0, 0.5, 0.9} and A ∈ {0.2, 0.5, 0.8}. The worst case is g = −0.5 at A = 0.2 (1.045). (lab E1)
- **Chiang 2016's fit** (with σt = 1/(d·s)) gives R/A = 1.032 / 1.013 / 1.005 / 0.994 / 0.949 / 0.931 at A = 0.05-0.99 with cosine entry. It was fitted to match Burley's diffusion, not the furnace albedo. (lab E1)
- **Cycles' fit as recalled** reproduces A within 1.6 % at g = 0 and 1.2-5 % at g = 0.5. At g = −0.5 my recollection gives α = 0, and at g = 0.9 R/A is 1.24, so either the recollection or the fit's range is off there. It is not used further. (lab E1)

### 2.4 Dwivedi guiding (lab E2)

**The method.** Zero-variance theory for a half space suggests direction sampling biased towards the surface normal: p(μ) ∝ 1/(ν₀ − μ), where ν₀ solves α·ν₀·atanh(1/ν₀) = 1. Free flights are then sampled with the stretched extinction σt(1 − μ/ν₀). (Křivánek & d'Eon 2014.)

Meng, Hanika and Dachsbacher 2016 give a closed-form ν₀. It agrees with the exact root to 6·10⁻⁵ (α 0.5) and to better than 10⁻⁵ above α 0.9 (lab E2).

Pure guiding is biased towards short paths and has unbounded weights for paths that go deep. The lab therefore uses one-sample MIS with probability p_g for the guided strategy, per segment, for both direction and distance (lab, proto).

| Geometry | α | plain: relStd, segments | Dwivedi p_g 0.5 / 0.75 / 0.9: efficiency vs plain |
|---|---|---|---|
| half space | 0.5 | 1.34, 4.2 | 2.1 / 3.0 / 3.4 |
| | 0.9 | 0.74, 12.8 | 4.5 / 12.5 / 21 |
| | 0.99 | 0.38, 44.9 | 4.9 / 21 / 66 |
| | 0.999 | 0.21, 145 | 5.0 / 23 / 110 |
| sphere radius 3 mfp, guided to the entry normal | 0.5 | 1.00, 3.5 | 0.92 / 0.70 / 0.43 |
| | 0.9 | 0.31, 5.0 | 0.66 / 0.31 / 0.17 |
| | 0.99 | 0.042, 5.0 | 0.07 / 0.04 / 0.02 |
| sphere radius 30 mfp | 0.9 | 0.69, 11.9 | 4.3 / 10.3 / 15.8 |
| | 0.99 | 0.31, 32 | 5.6 / 11.5 / 3.3 |
| | 0.999 | 0.088, 41 | 1.3 / 0.42 / 0.24 |

(lab E2; efficiency = 1/(variance × time); cosine entry; 1 M walks per row)

Guiding towards the entry plane is excellent where the object is thick compared with the diffusion length. It is harmful where light should go through or around the object (small or thin objects, high albedo), because the MIS weights then carry the variance. Cycles therefore probes the thickness along −N first and mixes "backward" and "forward" guiding (lit, from memory).

For Nova this is an optimization with a scene-dependent sign. It belongs after the correct walk. (design)

### 2.5 Chromatic mean free paths (lab E3)

With per-channel σt, one walk cannot sample the free flights of all three channels. The options:

| Strategy | Half space, mfp (1, .5, .25), A .8: standard deviation per walk | Sphere R 2, mfp (1, .5, .25), A .8 | Sphere R 2, skin mfp (1, .15, .1), A (.9, .6, .4) | Efficiency vs three walks (the four cases) |
|---|---|---|---|---|
| three mono walks (reference) | 0.300 / 0.300 / 0.299 | 0.025 / 0.054 / 0.117 | 0.006 / 0.314 / 0.331 | 1 |
| one walk, channel per step by throughput, balance heuristic (Cycles) | 0.876 / 0.656 / 0.860 | 0.877 / 0.550 / 0.844 | 1.247 / 0.562 / 0.501 | 0.26 / 0.02 / 0.18 / 0.18 |
| one walk, hero channel per walk, path-level balance heuristic | 0.875 / 0.656 / 0.860 | 0.877 / 0.550 / 0.844 | 1.277 / 0.584 / 0.498 | 0.19 / 0.01 / 0.15 / 0.14 |
| one walk, uniform channel per step | 5.4 / 1.5 / 14.2 | 1.3 / 0.41 / 39 | 25 / 3.1 / 0.87 | ≤ 0.03 |

(lab E3; cosine entry; 500 k walks; every strategy unbiased: the means agree within 2 standard errors; the fourth case, a slab of thickness 1 with skin mfp, behaves like the third)

- Per-step uniform choice multiplies per-step ratios of up to 3 over the whole walk, so its weights explode. Throughput-driven choice and path-level MIS keep them bounded, but they degenerate into "one channel at a time" when the channels' mean free paths differ.
- The worst case is a channel that barely scatters inside the object (red in skin): three walks give it almost no noise (standard deviation 0.006), one walk gives it the MIS noise of the others (1.25).
- Three walks would mean three exit points and three continuations. That is a path split, which the wavefront path state does not have.
- Karma's measured noise in `f_chroma` (relative standard deviation 1.1 / 0.9 / 2.2 per sample) is at the single-walk level, and its probe count does not grow with chromatic radii (7.07 against 6.99 probes per walk). So Karma does one walk (inference).

### 2.6 Step counts and caps (lab E4)

Half space, cosine entry. The mean and 99th percentile of segments per walk, and the reflectance kept when walks are killed after N segments:

| A | g | mean segments (plain / Dwivedi .75) | p99 | R | cap 64 | 128 | 256 | 512 | 1024 |
|---|---|---|---|---|---|---|---|---|---|
| 0.5 | 0 | 13.7 / 6.1 | 56 | 0.5008 | 0.5008 | 0.5008 | 0.5008 | 0.5008 | 0.5008 |
| 0.8 | 0 | 46 / 17 | 437 | 0.8003 | 0.7725 | 0.7929 | 0.7993 | 0.8003 | 0.8003 |
| 0.95 | 0 | 209 / 70 | 5743 | 0.9501 | 0.8344 | 0.8795 | **0.9101** | 0.9299 | 0.9417 |
| 0.99 | 0 | 664 / 320 | 17034 | 0.9897 | 0.8379 | 0.8851 | **0.9184** | 0.9421 | 0.9587 |
| 0.8 | 0.5 | 92 | 874 | 0.8002 | 0.7294 | 0.7725 | 0.7928 | 0.7992 | 0.8001 |
| 0.8 | 0.9 | 454 | 4341 | 0.8001 | 0.5102 | 0.6275 | **0.7102** | 0.7616 | 0.7881 |
| 0.5 | 0.9 | 126 | 538 | 0.5002 | 0.4252 | 0.4791 | 0.4974 | 0.5001 | 0.5002 |

(lab E4; 2 M walks per row; the "plain" histogram limit of 65536 segments truncates the α ≥ 0.99998 rows slightly)

- A cap removes the physical contribution of paths with more scattering events than the cap. The guided rows at a cap equal the plain rows to 4 digits, so guiding does not change the truncation.
- Karma's 0.9118 / 0.9202 (`f_a2`) and 0.9089 (`slab_refl`) sit at the 256 column. Karma's 0.7207 at g = 0.9 (`f_gp9`) is 1.5 % above the 256 column and far below the 512 column. Karma's whole model reads about 0.5-1.5 % high at g = 0.9 in all three channels, so the cap is about 256 there as well (measured, inference).
- Karma seems to kill the walk at the cap; other choices would leave the measured deficit unexplained (inference). Possible alternatives:
  - exit at the cap position (biased bright);
  - a diffusion-based estimate of the remaining escape (biased);
  - no cap (unbounded cost at α → 1).

### 2.7 Thin geometry and exits

- **Exits need closest-hit queries against the walk's own object.** Each segment is one ray of length t: no hit means the walk scatters at t, a hit means the walk crosses the surface there. Any-hit is not enough, because the exit position and normal are needed.
- Other objects must be invisible to these rays: Karma traces only the object itself unless a traceset groups objects (help `solaris/support/sss`, registry).
- **Thin and small objects are where the walk matters.** The exit may be on the far side (transmission) or around a corner.
  - Furnace values grow towards 1 as radius/mfp shrinks, because fewer walks are absorbed: A = 0.2 goes from 0.20 (thick) to 0.62 at radius 1 mfp (lab E5, `f_s*`).
  - Transmission through a slab of 1 mfp at A = 0.8 is 0.54 of the incident flux (lab E6).
- **An open mesh** (no closing surface below the entry) behaves like a half space in Karma: misses do not end the walk (measured `open_quad`).
- **Back faces and entry from inside** (a camera inside an SSS object, a ray that refracted into it) were not probed. A walk that starts from a back face would walk outside the mesh (open question).

### 2.8 Anisotropy

HG in the walk is straightforward: sample the new direction relative to the old one. Similarity theory says that, after many events, (σs(1 − g), α') behaves like an isotropic medium, which is the basis of both the eq. 50 correction and Cycles' σt = 1/(r(1 − g)).

**The cost of forward scattering.** Forward scattering makes walks longer: at A = 0.8, 46 segments at g = 0, 92 at 0.5 and 454 at 0.9. The 256 cap then loses 11 % at g = 0.9 (lab E4). Karma's g = 0.9 furnace shows this loss: 0.7207, against the lab's 0.7102 at the cap (`f_gp9`).

**The effect on transmission** (measured `slab_gp5`, `slab_gm5`; lab E6):

| g | transmission at 2 mfp, A = (0.5, 0.8, 0.95) |
|---|---|
| +0.5 | 0.1484 / 0.1720 / 0.1749 |
| 0 | 0.0877 / 0.1187 / 0.1234 |
| −0.5 | 0.0566 / 0.0890 / 0.0953 |

Positive g roughly doubles transmission through thin slabs, and negative g halves it. Both match the lab to 0.4 %.

---

## 3. Parametrization (question 2, part 1)

### 3.1 MaterialX and the uber models

| Node / model | Inputs (defaults) | Semantics | Source |
|---|---|---|---|
| `ND_subsurface_bsdf` | `weight` 1, `color` 0.18, `radius` (1,1,1), `anisotropy` 0, `normal` Nworld | R-only BSDF ("bsdf=R"); radius is the mean free path per channel | `pbrlib_defs.mtlx:120-127`; help `nodes/vop/mtlxsubsurface_bsdf` |
| genosl | `subsurface_bssrdf(N, albedo, radius, anisotropy)` (OSL ≥ 1.14) | the renderer's BSSRDF closure; albedo is the colour | `pbrlib/genosl/mx_subsurface_bsdf.osl` |
| genglsl | screen-space Burley diffusion approximation from curvature for direct light; indirect is plain diffuse with the colour | a preview, not a reference | `pbrlib/genglsl/mx_subsurface_bsdf.glsl`, `lib/mx_microfacet_diffuse.glsl:194-199` |
| Standard Surface 1.0.1 | `subsurface` 0, `subsurface_color` 1, `subsurface_radius` (1,1,1), `subsurface_scale` 1, `subsurface_anisotropy` 0; `coat_affect_color` exponent on the colour | radius·scale = mfp; `thin_walled` switches subsurface to `translucent_bsdf(color)`; the subsurface replaces the diffuse with weight `subsurface` | `bxdf/standard_surface.mtlx:50-59, 212-253` |
| OpenPBR 1.1 | `subsurface_weight` 0, `subsurface_color` 0.8 (clamped to [0, 1]), `subsurface_radius` 1, `subsurface_radius_scale` (1, 0.5, 0.25), `subsurface_scatter_anisotropy` 0 | radius_scale·radius = mfp; thin-walled: `oren_nayar(color)·color(1−g)` mixed 50/50 with `translucent(color)·color(1+g)` (the graph's colour-squared error, M3) | `bxdf/open_pbr_surface.mtlx:40-49, 135-210` |
| Disney principled, LamaSSS | `subsurface`, `subsurfaceDistance`; LamaSSS `sssRadius`, `sssScale`, `sssMode`, `sssIOR`, `sssAnisotropy`, ..., `albedoInversionMethod` | not probed | registry `karmaShaderNodes.json` (ND_disney_principled, ND_lama_sss) |

The help (`solaris/support/sss`) describes the parameters the same way, in my words:
- radius × scale is the mean free path per colour channel in scene units, and scale exists only for convenience;
- the colour is the target colour of the effect;
- anisotropy is HG, with values between −0.5 and 0.5 recommended;
- thin-walled subsurface uses only the colour and is fully forward scattering;
- SSS AOVs come from the `sss` label;
- tracesets join split objects.

### 3.2 Karma's implementation (header, VEX placeholders)

- `mxpbr_subsurface` (`mxpbr.h:1024-1040`) converts (colour, mfp, anisotropy) to σa and σs with `_mx_sss_parameterization` (`mxpbr.h:255-279`, OpenPBR 2025 branch). It returns `cvex_bsdf("randomwalk_eval", "randomwalk_sample", "label", "sss", "absrp", σa, "scatr", σs, "g", g) · w`. Thin-walled translucency is `cvex_bsdf("translucency_eval", ..., "label", "sss")` (`mxpbr.h:592-602`).
- `$HFS/houdini/vex/CVex/randomwalk_{eval,sample}.vfl` are stubs: "only supported in Karma", and they return nothing in Mantra. The sample stub's interface has inputs `absrp` (0.0131), `scatr` (25.425), `g` and `eta` (1) and exports `ssstype`, `ssabsrp`, `ssscatr`, `ssg`, `sseta`. So the walk itself is native Karma code driven by these values. `eta` exists but `mxpbr_subsurface` does not pass it, so it stays 1. That is consistent with the measured index-matched walk (header, inference).

### 3.3 Karma's settings (registry `RenderPropertiesKA.json`; help `karmarendersettings`, `rendergeometrysettings`, `karma_xpu`)

| Property | Default | Meaning |
|---|---|---|
| `karma:object:ssslimit` | 1 (range 0-32, float, stochastic) | SSS bounces, like the diffuse limit (measured semantics: section 4.6) |
| `karma:object:sssquality` | 1 | multiplier of min/max secondary samples for the SSS component (no effect with path-traced convergence and fixed samples) |
| `karma:object:sss_traceset` | "" | without: the walk traces only its own object; with: every object sharing the name; on instancers it must be set on the prototype. Karma XPU: at most 4 tracesets per scene (shared with custom tracesets) |
| `karma:global:ssscomponents` | "sss" | component labels that behave as SSS bounces (limit, reflection scope) |
| `karma:global:exportcomponents` | "diffuse reflect coat refract volume sss" | per-component exports |
| `karma:global:guiding_components` | "diffuse volume sss" | path guiding applies to SSS |
| `inputs:karma:light:contribs` menu | includes `sss`, "Any diffuse" = `diffuse\|volume\|sss` | per-light contribution selection |
| `karma:object:rendervisibility` menu | "Invisible to indirect rays" = `-diffuse&-reflect&-refract&-volume&-sss` | ray-type visibility |
| Karma Standard Render Vars | `sss` = `C<...'sss'>.*[LO]`, colour3 | help `karmastandardrendervars` |
| Karma XPU | SSS only on meshes and points; mixes of uber shaders do not mix SSS | help `karma_xpu` |

---

## 4. Karma's behaviour, measured (question 2, part 2)

### 4.1 Setup (`probes/sss_scenes.py`, `sss_renders.ps1`, `sss_measure.py`)

- **Scenes.** 47 scenes, written as USDA by `sss_scenes.py` (plain Python, no Houdini):
  - MaterialX Standard Surface 1.0.1 with `base` 0, `specular` 0 and `subsurface` 1, unless a scene says otherwise;
  - polygon geometry: UV spheres of 128×64 with smooth normals, boxes, a room shell and quads;
  - orthographic cameras.
- **Render settings.** One RenderSettings prim serves both renderers. Karma: path traced, fixed samples (256-1024 per pixel), box filter, colour limits 1e9, true caustics. Nova: the same samples, box filter, no indirect clamp.
- **Statistics.** `sss_measure.py` reads every part of the multi-part EXRs with OpenImageIO and prints the means (± standard error over pixels), μ bins or x profiles. Raw outputs: `probes/sss_out_karma.txt`, `sss_out_nova.txt`.
- **Run time.** Each Karma render took 2-4 s.
- **LPE AOVs.** Every scene also writes LPE AOVs (`sss`, `C<TD>.*[LO]`, `C<RD>.*[LO]`, `C<TD>L`, `C<TD'sss'>L`, `C<TD><TD>L`, `C<TG>.*`, `C<RG>.*`, and the `O` variants in the room).

### 4.2 Furnace: colour, inversion, size, anisotropy, layering (sphere of radius 1 in a dome of radiance 1; mean over the disc r < 0.95)

| Scene | Parameters | Karma | Lab (E1/E5, cosine entry, uncapped) |
|---|---|---|---|
| `f_a1` | A (0.2, 0.5, 0.8), mfp 0.001 | 0.2028 / 0.5019 / 0.8001 | 0.2025 / 0.5016 / 0.8015 (normal entry: 0.1935 / 0.4896 / 0.7941) |
| `f_opbr` | the same as OpenPBR | identical to `f_a1` (same samples) | |
| `f_a2` | A (0.05, 0.95, 0.99) | 0.0519 / 0.9118 / 0.9202 | 0.0512 / 0.9500 / 0.9901; cap 256: – / 0.9101 / 0.9184 |
| `f_gp5` | g = 0.5 | 0.2006 / 0.5016 / 0.7966 | 0.2000 / 0.5005 / 0.8004 (cap 256: 0.7928) |
| `f_gm5` | g = −0.5 | 0.2094 / 0.5047 / 0.8015 | 0.2090 / 0.5043 / 0.8001 |
| `f_gp9` | g = 0.9 | 0.2021 / 0.5044 / 0.7207 | 0.1994 / 0.5004 / 0.8003; cap 256: – / 0.4974 / 0.7102 |
| `f_s003` .. `f_s3` | mfp 0.03 / 0.1 / 0.3 / 1 / 3 (radius/mfp 33 / 10 / 3.3 / 1 / 0.33) | A=0.2: 0.2106 / 0.2335 / 0.3273 / 0.6197 / 0.8445; A=0.5: 0.5213 / 0.5730 / 0.7182 / 0.8913 / 0.9616; A=0.8: 0.8320 / 0.9006 / 0.9606 / 0.9876 / 0.9957 | 0.2108 / 0.2334 / 0.3272 / 0.6195 / 0.8443; 0.5209 / 0.5725 / 0.7171 / 0.8911 / 0.9617; 0.8319 / 0.8993 / 0.9599 / 0.9876 / 0.9958 |
| `f_chroma` | A 0.8, mfp 0.3 × (1, 0.5, 0.25) | 0.9618 / 0.9250 / 0.8812 | 0.9599 / 0.9256 / 0.8792 |
| `f_spec` | + specular 1 (IOR 1.5, roughness 0.2) | 0.2476 / 0.5300 / 0.8117, of which `C<RG>.*` 0.0560 | SSS part = `f_a1` × 0.944 = 1 − 0.056 |
| `f_coat` | + coat 1 | 0.2470 / 0.5296 / 0.8114, `C<RG>.*` 0.0554 | the same layering |
| `f_mix` | subsurface 0.5 over base colour 0.2, SSS colour 0.8 | 0.5004, `C<RD>.*` 0.1000 | 0.5 · 0.8 + 0.5 · 0.2 |
| `f_lim0`, `f_lim0d0`, `f_d0`, `f_lim4` | ssslimit 0 / 0 + diffuse 0 / diffuse 0 / 4 + 4 | all identical to `f_a1` | see 4.6 |

Conclusions (measured, lab):
- Karma's colour is the furnace albedo of a thick object with cosine entry and an index-matched walk, uncapped at A ≤ 0.8. At A ≥ 0.95 the walk cap darkens it by 4-7 %.
- Small objects are brighter in the furnace, as physics says.
- Specular and coat layers sit on top with MaterialX albedo scaling at the viewing direction. The walk's entry is not a glossy transmission (`C<TG>.*` = 0).

### 4.3 Slabs (4 × 4 × d, distant light of irradiance 1 from behind at normal incidence; camera in front; mean over the central 1 × 1)

| Scene | d / mfp (A = 0.5 / 0.8 / 0.95) | Karma | Lab (E6), (T + unscattered)/π with cosine entry |
|---|---|---|---|
| `slab_d0p025` | 0.5 | 0.2116 / 0.2227 / 0.2240 | 0.2116 / 0.2229 / 0.2242 |
| `slab_d0p05` | 1 | 0.1534 / 0.1731 / 0.1757 | 0.1538 / 0.1733 / 0.1759 |
| `slab_d0p1` | 2 | 0.0877 / 0.1187 / 0.1234 | 0.0881 / 0.1192 / 0.1239 |
| `slab_d0p2` | 4 | 0.0317 / 0.0693 / 0.0776 | 0.0317 / 0.0695 / 0.0776 |
| `slab_d0p4` | 8 | 0.0043 / 0.0311 / 0.0442 | 0.0043 / 0.0311 / 0.0442 |
| `slab_gp5` / `slab_gm5` | 2, g ±0.5 | 0.1484 / 0.1720 / 0.1749; 0.0566 / 0.0890 / 0.0953 | 0.1487 / 0.1721 / 0.1752; 0.0568 / 0.0893 / 0.0955 |
| `slab_a60` | 2, light at 60° (irradiance 0.5) | 0.0438 / 0.0593 / 0.0617 | 0.5 × normal: 0.0440 / 0.0596 / 0.0619 (a straight entry at 60° would give 0.0362 / 0.0519 / 0.0543) |
| `slab_chroma` = `slab_opbr_chroma` | A 0.8, mfp 0.1 × (1, .5, .25): d/mfp 1 / 2 / 4 | 0.1731 / 0.1187 / 0.0690 | 0.1733 / 0.1192 / 0.0695 |
| `slab_spec` | 2, specular 1 | 0.0839 / 0.1136 / 0.1181 = 0.957 × `slab_d0p1` | one interface factor (viewing side) only |
| `slab_refl` | thick, lit from the camera side | ×π: 0.5014 / 0.7983 / 0.9089 | 0.5008 / 0.7993 / 0.9101 (cap 256) |
| `slab_thin` | thin-walled quad | 0.1592 / 0.2546 / 0.3024 = colour/π | translucent Lambert |
| `open_quad` | single quad, not thin-walled, lit from the camera side | 0.1596 / 0.2541 / 0.2894 | = `slab_refl` |

The light from behind passes through the walk without any ballistic component. The light that crosses the slab unscattered is also emitted diffusely at the back, because entry and exit are Lambertian.

### 4.4 Ballistic light, angular behaviour, terminator

- **`ball_d05`** (emissive card of radiance 1 at x < 0 behind a slab of 0.5 mfp, A 0.5 / 0.8 / 0.95). The profile across x falls smoothly from 0.4496 (x = −0.49) through 0.3044 (x = 0) to 0.1717 (x = 0.45), red channel. There is no step at the card's edge; a straight walk would show 0.61 = e^(−0.5) on the card side.
  - All of it is one `<TD>` event followed by `O`.
  - With specular 1 it is 0.957× lower (`ball_d05_spec`).
- **`head`** (thick sphere, distant light from the camera direction).
  - Karma's L(μ) divided by Nova's Lambert render of the same scene is 1.013 / 1.005 / 1.002 at μ 0.95 (Karma 0.0614 / 0.1521 / 0.2429) and 1.02 / 1.00 / 0.985 at μ 0.17. The exit is Lambertian. (Both are 5 % below A·μ/π in the outermost bin, where pixels straddle the silhouette of the faceted sphere.)
  - Lab E6: a straight entry with a straight index-matched exit would be 0.70x this value at μ = 1 and 2.3x at μ = 0.2 (A 0.2), a retro-reflective lobe.
- **`term_s0p01`, `term_s0p05`, `term_s0p2`** (sphere lit from +x, A 0.5 / 0.8 / 0.95, mfp 0.01 / 0.05 / 0.2): the radiance along the centre line (averaged over |y| < 0.275) gives reference values for bleeding into the shadow.

  | x (dark side ← terminator → lit) | −0.54 | −0.27 | −0.13 | 0.01 | 0.15 | 0.42 |
  |---|---|---|---|---|---|---|
  | mfp 0.01 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0.0001 | 0.0017 / 0.0035 / 0.0046 | 0.0236 / 0.0376 / 0.0430 | 0.0683 / 0.1082 / 0.1228 |
  | mfp 0.05 | 0 / 0.0006 / 0.0017 | 0.0001 / 0.0018 / 0.0047 | 0.0005 / 0.0040 / 0.0076 | 0.0048 / 0.0122 / 0.0174 | 0.0252 / 0.0425 / 0.0505 | 0.0716 / 0.1126 / 0.1272 |
  | mfp 0.2 | 0.0041 / 0.0113 / 0.0131 | 0.0083 / 0.0189 / 0.0215 | 0.0125 / 0.0253 / 0.0282 | 0.0209 / 0.0367 / 0.0400 | 0.0386 / 0.0596 / 0.0639 | 0.0826 / 0.1156 / 0.1214 |

### 4.5 Ray statistics (husk's counters; `probes/sss_out_karma_rays.txt`)

Karma counts the walk's segments as **Probe** rays.

| Scene | probes per camera hit | Note |
|---|---|---|
| `f_a1` | 6.99 | A (0.2, 0.5, 0.8) |
| `f_mix` | 35.5 per walk (27.2 M probes over half of 1.53 M hits) | grey 0.8 |
| `f_a2` | 30.8 | |
| `f_gp9` | 45.8 | |
| `f_s003` .. `f_s3` | 6.62 / 5.73 / 4.10 / 2.26 / 1.44 | shrinking objects: shorter walks |
| `f_chroma` | 7.07 | chromatic radius |
| `slab_d0p025` .. `slab_d0p4` | 1.99 / 2.94 / 4.70 / 7.82 / 12.5 | |
| `slab_thin` | 0 | no walk |

- After the walk Karma does one occlusion query and one light-geometry hit per walk in the furnace, NEE and the continuation from the exit. Nothing is "Indirect": the exit's continuation is counted as light geometry when it hits the dome.
- The per-sample noise (pixel variance × samples) in `f_a1` is a relative standard deviation of 1.26 / 0.98 / 1.60, against about 1.2 / 0.7 / 0.38 for the lab's plain mono walks (E2, E3). In `f_mix` (grey) it is 0.45.
- Walks that are short when the other channels are dark, and noise moved into the brightest channel, fit Russian roulette on the channels' mean throughput rather than their maximum (inference).

### 4.6 Ray limits and light path expressions (`room_*`, camera inside a closed shell of SSS walls 2 units thick, mfp 0.003, A (0.25, 0.5, 0.75), emission 1)

| Scene | C | `CO` | `C<TD>O` | `C<TD><TD>O` | `C<TD><TD><TD>O` | `C<RD>O`, `C<TD><RD>O`, `C<RD>.*` |
|---|---|---|---|---|---|---|
| `room_l0` (ssslimit 0) | 1 / 1 / 1 | 1 | 0 | 0 | 0 | 0 |
| `room_l1` | 1.2514 / 1.4998 / 1.7466 | 1 | 0.2514 / 0.4998 / 0.7466 | 0 | 0 | 0 |
| `room_l2` | 1.3146 / 1.7495 / 2.3032 | 1 | same | 0.0632 / 0.2496 / 0.5567 (= A'²) | 0 | 0 |
| `room_l3` | 1.3306 / 1.8743 / 2.7229 | 1 | same | same | 0.0160 / 0.1249 / 0.4197 | 0 |
| `room_l8` (+ diffuse 8) | 1.3360 / 1.9953 / 3.6956 | 1 | same | same | same | 0 |
| `room_l1d0`, `room_l1d4`, `room_l2d0` | equal to `room_l1` / `room_l1` / `room_l2` | | | | | |

Semantics (measured; Karma's K1-K4 of docs/research/m4/nodes-and-limits.md extended to SSS):
- **Counting.** A walk is one event of class SSS, counted at the vertex where it starts. Event n ≤ `ssslimit` is a full event: the exit's continuation hits surfaces that are shaded (emission, the next walk).
- **Past the limit.** The event past the limit still walks. The exit then gathers lights only: there is no surface emission (the room at limit 0 is 1) but light from the dome (the furnace at limit 0 equals the furnace at 1).
- **The diffuse limit** neither restricts walks nor counts the exit's continuation. The exit's continuation counts as SSS: `ssslimit` 1 with diffuse 4 still stops after one walk.
- **LPE.** One `<TD>` event per walk, labelled `sss`: `C<...'sss'>.*[LO]` = `C<TD>.*[LO]` = the beauty in every SSS-only scene. The exit adds no event, and the walk's entry is not `G`.

### 4.7 Nova today against Karma (Nova's renders of the same scenes; `probes/sss_out_nova.txt`)

Nova renders thick SSS as Lambert(colour), so every furnace reads exactly the colour, and nothing passes through a slab.

| Scene | Karma | Nova | Nova / Karma |
|---|---|---|---|
| `f_a1` (thick, A 0.2/0.5/0.8) | 0.2028 / 0.5019 / 0.8001 | 0.2000 / 0.5000 / 0.7999 | 0.986 / 0.996 / 1.000 |
| `f_a2` (A 0.05/0.95/0.99) | 0.0519 / 0.9118 / 0.9202 | 0.0500 / 0.9499 / 0.9899 | 0.963 / 1.042 / 1.076 |
| `f_gp9` (g 0.9) | 0.2021 / 0.5044 / 0.7207 | 0.2000 / 0.5000 / 0.7999 | 0.990 / 0.991 / 1.110 |
| `f_s01` (radius/mfp 10) | 0.2335 / 0.5730 / 0.9006 | 0.2 / 0.5 / 0.8 | 0.857 / 0.873 / 0.888 |
| `f_s1` (radius/mfp 1) | 0.6197 / 0.8913 / 0.9876 | 0.2 / 0.5 / 0.8 | 0.323 / 0.561 / 0.810 |
| `f_s3` (radius/mfp 0.33) | 0.8445 / 0.9616 / 0.9957 | 0.2 / 0.5 / 0.8 | 0.237 / 0.520 / 0.803 |
| `f_chroma` | 0.9618 / 0.9250 / 0.8812 | 0.8 / 0.8 / 0.8 | 0.832 / 0.865 / 0.908 |
| `f_spec`, `f_coat` | 0.2476 / 0.5300 / 0.8117; 0.2470 / 0.5296 / 0.8114 | 0.2506 / 0.5315 / 0.8124; 0.2507 / 0.5315 / 0.8123 | +1.2 % / +0.3 % / +0.1 % (the layering is already right) |
| `f_mix`, `f_opbr` | 0.5004; = `f_a1` | 0.5000; 0.2000 / 0.4999 / 0.7999 | |
| `slab_*`, `ball_d05` (transmission) | section 4.3 / 4.4 | 0 everywhere | no transmission |
| `slab_refl`, `open_quad` | 0.1596 / 0.2541 / 0.2894 | 0.1592 / 0.2546 / 0.3024 | 0.997 / 1.002 / 1.045 |
| `slab_thin` | 0.1592 / 0.2546 / 0.3024 | 0.1592 / 0.2546 / 0.3024 | exact |
| `head` | μ profile | Lambert | Karma / Nova 0.985-1.02 per μ bin |
| `term_s0p2`, dark side (x = −0.13) | 0.0125 / 0.0253 / 0.0282 | 0 | no bleeding; the lit side is also slightly different near the terminator |
| `room_l1` / `room_l2` / `room_l3` | 1.2514 / 1.4998 / 1.7466; 1.3146 / 1.7495 / 2.3032; ... | 1.25 / 1.5 / 1.75; 1.3125 / 1.75 / 2.3125; 1.3281 / 1.875 / 2.7344 | the same event counting, and the same `sss`, `C<TD>O`, `C<TD><TD>O` and zero `C<RD>.*` |

Conclusions:
- Nova's limits and LPE semantics need no change.
- Nova's energy matches Karma only for thick objects at A ≤ 0.8. The differences are:
  - the cap at A ≥ 0.95 and g = 0.9: +4 to +11 %;
  - small objects, which are too dark (down to 0.24×);
  - no transmission or bleeding at all.

---

## 5. Recommendations for Nova (question 3)

### 5.1 Algorithm

| Option | For | Against |
|---|---|---|
| A. **Random walk with Karma's boundary model** (cosine entry, index-matched, HG, Lambert exit, van de Hulst/OpenPBR inversion, 256-segment cap) | Matches every Karma measurement in section 4 to 0.1-0.4 % in the lab. It is the OpenPBR 2025 reference parametrization and needs no tables. It is exact for thin and concave geometry, and the same code runs on CUDA. | Noise and ray cost on high albedos. The cap's energy loss (−4 to −7 % at A ≥ 0.95) is Karma's too, so parity keeps it. |
| B. Refracting entry/exit with Fresnel and internal reflection (Arnold `randomwalk_v2`, Cycles skin, pbrt-v4's volumetric way) | Physically closer for dielectrics | A Fresnel boundary darkens the result by up to 3.6x at low albedo unless the inversion is refitted (lab E1). It differs from Karma in every scene of 4.2-4.4. |
| C. Diffusion BSSRDF (Burley normalized, tabulated PBD) with probe rays | Cheap and noise-free at high albedo | Wrong on thin geometry and not what Karma renders. It needs disk sampling with probe rays and MIS over axes. |
| D. Keep the Lambert fallback | Free | Fails on thin objects, transmission, bleeding and high albedo (4.7) |

**Recommended: A.** M9's parity goal is Karma CPU. Arnold-like behaviour (a `randomwalk_v2`-style boundary) can be an option later. (design)

### 5.2 Parameters and inversion

- **Mean free path.** mfp_c = max(radius_c · scale, 1e-5) (Karma's clamp, `mxpbr.h:273`); OpenPBR uses radius_scale_c · radius. Nova already stores both products. σt = 1/mfp.
- **Colour.** A = clamp(colour, 0, 1), after `coat_affect_color` (SS: the colour^γ already computed in `surface_models.h:315`; OpenPBR clamps in its graph).
- **Inversion.**
  - α = α*/(1 − g(1 − α*)), α* = 1 − s², s = 4.09712 + 4.20863A − √(9.59217 + 41.6808A + 17.7126A²), g = clamp(anisotropy, −1, 1). It is evaluated once per closure, three times.
  - A = 1 gives α = 1. Clamp α to at most 1 − 1e-7 so that walks still end by RR.
- **Thin-walled.** Unchanged: Standard Surface's translucent lobe and OpenPBR's spec pair. There is no walk and no use of radius or anisotropy (measured `slab_thin`).
- **Lobe.** Add a `LobeType::Subsurface` with {σt[3], α[3], g}. It has no BSDF value at the entry (it is never part of NEE or of the mixture density). Selected for a continuation, it yields a "walk" sample of class `Sss`.
  - Its pick estimate and the albedo AOV use weight × A.
  - The generic graph leaf `LeafKind::Subsurface` must carry radius and anisotropy (8 floats, as planned in docs/research/m4/bsdf-graphs.md).
  - Mixes of several subsurface lobes in one closure (`mix` of two SSS surfaces): pick one lobe, walk with its medium (Karma XPU does not mix SSS at all, help `karma_xpu`).

### 5.3 Boundary and exit model

| Option | |
|---|---|
| A. **Karma's**: entry direction cosine-distributed about −N_g (geometric normal of the hit, oriented against the incoming ray), walk index-matched, exit when a segment crosses the surface; at the exit a Lambert lobe about the exit's geometric normal (facing out of the medium), weight = walk throughput; no specular factor at the exit | the measured model (4.2-4.4) |
| B. As A, but the exit's Lambert lobe on the interpolated shading normal (on the geometric side, as Nova's Lambert lobes are framed) | The exit's NEE is cosine-weighted, so on coarse smooth-shaded meshes the shading normal avoids faceting. Karma's choice was not separated in these probes (open question; the 128×64 sphere looks the same with either). |

**Recommended: A, with the exit's Lambert lobe on the shading normal (B).** Check it against Karma on a coarse smooth-shaded mesh during M9.

**The specular layer.** The interface stays the closure's job: the SSS lobe's weight is already scaled by (1 − E_spec(μ_o)) at the entry (MaterialX layering, measured 0.944 in Karma, which Nova's models already compute). The exit gets no second factor.

### 5.4 Fitting the walk into the wavefront integrator

The flow per bounce becomes: Generate → Intersect → **traceMedia** → Shade → **Walk** → Shadow → next. (design)

1. **Shade (pass C, `shadeHit`).**
   - NEE at the entry uses the closure without its SSS lobe: its f is 0, as for delta lobes. MIS densities exclude it (`nonDeltaMask` without the SSS bit).
   - The continuation picks a lobe as now. If it picks the SSS lobe (probability p), the path goes to the **walk queue** with these fields: entry point, oriented geometric normal, mesh, instance, primitive, time, σt[3], α[3], g, and the weight w_SSS/p.
   - The SSS event is counted (class `Sss`, limit as now), and the LPE state steps by `<TD.'sss'>`.
   - Nothing goes to `nextRays` for that path in this pass.
2. **Walk stage (new).**
   - For each queued path, run the walk to completion: segments of closest-hit queries against the hit's own mesh instance, HG scattering, spectral MIS and Russian roulette. A miss means no boundary in that direction (the open-mesh rule).
   - At most 256 segments; then the walk ends with weight 0. Count those walks for the render statistics.
   - At the exit, resolve the hit (`Scene::resolveHit`: P, N, Ng; no material evaluation).
     - **NEE:** one light by `chooseLight` with a Lambert lobe; shadow ray into the existing shadow queue; MIS density cos/π.
     - **Continuation:** a cosine direction into `nextRays`, with `bsdfPdf` = cos/π, `diffuseShare` 1, class `Sss`, cone spread as after a diffuse event, `afterDiffuse` = 1, and RR on the post-walk throughput with `kRoulette`.
   - If the SSS event was past its limit, the exit still does NEE but its continuation is light-only (section 4.6; that is Nova's existing `lightOnly` mechanism).
3. **Queries.**

   | Option | |
   |---|---|
   | a. **`IAccel::intersectLocal(rays, meshIds, instances, hits)`**: transform each ray into the instance's object space (its transform at the ray time, from `MeshRecord::instances` or `motionKeys`) and traverse that mesh's own BLAS (`rtcIntersect1` on `Blas::scene`) | **Recommended.** It is the cheapest, ignores every other object, and handles instanced and deforming meshes. On OptiX (M10): `optixTrace` on the GAS traversable of that mesh with the object-space ray, the way Cycles' local intersection works. |
   | b. Ray masks / visibility masks: one bit per traceset | Embree has 32 bits and OptiX 8 (Karma XPU allows 4 tracesets). It suits `sss_traceset`, not "own object". |
   | c. Filter callback rejecting other meshes | It runs for every candidate hit in the TLAS. It is slow, and it forces `RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS` on everything. |
   | d. Per-object TLAS built on demand | Memory and build time per SSS object; only for tracesets. |

   - `sss_traceset`, later: a small TLAS per traceset name built at commit, in which the walk uses (a) against that TLAS instead of the BLAS (design).
   - Hit filters (cut-out opacity) on SSS meshes: ignored inside the walk (open question for Karma).
4. **CPU now, CUDA later.** Running each walk to completion in one thread is the simplest. One `rtcIntersect1` per segment matches what the shade stage does per ray, and it maps to one CUDA kernel whose threads loop over `optixTrace` (Cycles' `intersect_subsurface` kernel). A queue per segment would only pay off if divergence becomes a problem on the GPU; the walk state is small (position, direction, throughput[3], rng, count). (design)

### 5.5 Chromatic mean free paths

| Option | |
|---|---|
| a. **One walk; achromatic mfp: exact single-channel distances with per-channel weights α_c; chromatic: channel per segment by throughput, balance-heuristic weights** (Cycles' scheme) | **Recommended.** Unbiased, one exit, no path split, Karma's cost model. It costs 4-7x efficiency against separate walks for strongly chromatic radii in the lab, and 50-100x in small objects that light mostly escapes (E3). Karma shows the same noise (`f_chroma`). |
| b. Hero channel per walk with path-level MIS | Equal variance to (a) in the lab, more state (log pdfs per channel). |
| c. Three walks, one continuation | The best variance for the walk itself, 3x walk cost, three exit NEEs. The continuation must pick one exit (colour noise in indirect light after SSS). Worth a try if chromatic skin turns out too noisy (later). |

Use the max-channel throughput for Russian roulette inside the walk (the lab does), not the mean, so that bright channels do not inherit the dark channels' noise (section 4.5).

### 5.6 Step cap, Russian roulette, guiding

- **Cap.** 256 segments, kill at the cap. That is Karma's value (and Cycles', from memory) and keeps parity at high albedo and g = 0.9. Expose it as `nova:sssMaxSteps` (default 256) only if a user needs high-albedo energy, and document the loss (lab E4: at A = 0.95 the cap keeps 0.9101 of 0.9501; 1024 would keep 0.9417).
- **Russian roulette.** Weight the throughput by α per scattering. Apply RR when the max-channel throughput falls below 0.1, as in the lab: unbiased, and it keeps the analog behaviour for α ≈ 1.
- **Dwivedi guiding.** Later (M9 polish or M10), with Cycles' thickness probe and p_g ≈ 0.5. On its own, p_g 0.5 brings 2-5x on thick objects but still loses up to 14x in a sphere of 3 mfp at α 0.99 (lab E2). The thickness probe must switch guiding off, or turn it forward, there (not measured). Guiding does not change the cap's energy loss.

### 5.7 Sampling (determinism as in docs/design/M8.md D18)

| Decision | Source |
|---|---|
| the lobe choice at the entry | `bounce(depth, kBsdf).z` (unchanged) |
| entry direction (2D) and first free flight (1D) | **Sobol `bounce(depth, kWalk)`** (3D) |
| further distances, directions, channel choices, RR inside the walk | a PCG `Rng` seeded by hash(pixelSeed, sampleIndex, `bounce(depth, kWalk)`) |
| exit NEE (3D) | a new block `stream::walkLight(depth)` (e.g. base `0x60000`), because `kLight` of this depth was used at the entry |
| exit continuation direction (2D) | `bounce(depth, kBsdf).xy`, unused when the SSS lobe was picked |
| RR after the walk | `bounce(depth, kRoulette)`, unused at the entry |

Results stay a function of (pixel, sample, depth), independent of threads and of adaptive sampling.

### 5.8 Ray limits

- **Class and limit.** Class `Sss` with the existing per-object limit (`primvars:nova:object:sssLimit`, else the render setting `nova:object:sssLimit`). The limit is read at the entry's mesh.
- **Past the limit.** The walk still runs; the exit does NEE and a light-only continuation.
- **Diffuse limit.** Unaffected (measured 4.6).
- **Thin-walled translucency** stays class `Sss`, as now and as M4 measured (`docs/research/m4/nodes-and-limits.md` A7).

### 5.9 Light path expressions and AOVs

- **The event.** `<TD.'sss'>` once per walk at the entry, with Nova's `LobeLabel::Sss` and `lpeTransmit` = true.
- **The exit adds no event.** Its NEE parts carry the state after `<TD.'sss'>`.
- **No lobe splits for the walk.** `perLobe` light-sample parts exclude the SSS lobe at the entry (its light is collected at the exit).
- **The albedo AOV** at the first hit: weight × A of the SSS lobe, as now. Karma's albedo AOV for SSS was not probed.

### 5.10 Tests (design; numbers from section 4 and the lab)

1. **Unit, core (no renderer).** A walk function on analytic geometry, the lab's scenes, against `probes/sss_out_lab.txt`:
   - half space, cosine entry: R = 0.2021 / 0.5008 / 0.7996 for A = 0.2 / 0.5 / 0.8;
   - with the 256 cap: 0.9101 / 0.9184 for A = 0.95 / 0.99;
   - g = ±0.5: within 1 % of the E1 rows;
   - sphere furnace: radius/mfp = 1: 0.6195 / 0.8911 / 0.9876;
   - slab of 2 mfp: (T + unscattered) = 0.2768 / 0.3745 / 0.3892 for A 0.5 / 0.8 / 0.95;
   - each at 1 M walks, at 5σ.
2. **Inversion.** α(A, g) against the closed form (exact), plus the van de Hulst round trip.
3. **Chromatic estimator.** Mean of the one-walk estimator equal to separate walks (E3 cases) at 5σ.
4. **Determinism.** Identical images across thread counts and adaptive/uniform at equal counts.
5. **husk against Karma** (scenes of `sss_scenes.py`, regenerated at test time; 2-4 s each in Karma). Tolerances from Karma's standard errors (± 0.0002-0.001):

   | Test | Karma reference |
   |---|---|
   | `f_a1` | 0.2028 / 0.5019 / 0.8001 |
   | `f_a2` | 0.0519 / 0.9118 / 0.9202 (the cap) |
   | `f_gp5`, `f_gm5`, `f_gp9` | section 4.2 |
   | `f_s01`, `f_s1` | 0.2335 / 0.5730 / 0.9006; 0.6197 / 0.8913 / 0.9876 |
   | `f_chroma` | 0.9618 / 0.9250 / 0.8812 |
   | `f_spec` | 0.2476 / 0.5300 / 0.8117 |
   | `f_mix` | 0.5004 |
   | `slab_d0p05`, `slab_d0p1`, `slab_d0p4`, `slab_a60`, `slab_chroma`, `slab_gp5` | section 4.3 |
   | `slab_thin`, `open_quad` | section 4.3 |
   | `ball_d05` | the profile, no step |
   | `head` | Karma / Nova's Lambert = 0.985-1.02 per μ bin |
   | `term_s0p05` | dark-side profile |
   | `room_l0` .. `room_l3` | limits and `C<TD>O`, `C<TD><TD>O`, `sss` |

6. **Furnace in Nova's unit tests.** A thick sphere in a white dome has R = A to the lab's ratios (1.010 at 0.2). `ssslimit` 0 leaves the furnace unchanged.
7. **Solaris.** A Standard Surface SSS preset (wax from the help page: colour 0.5/0/0, radius 1/0.3/0.2, scale 0.02, anisotropy 0.35) in the Nova viewport next to Karma, through `run_viewport_check.ps1`.

### 5.11 Order of work (design)

1. **M9.SSS.0.** `IAccel::intersectLocal` (Embree BLAS traversal in object space) with unit tests (instanced, moving, deforming meshes).
2. **M9.SSS.1.**
   - The inversion and the `Subsurface` lobe in both models and the graph leaf.
   - The walk function in `core/bsdf` or `core/integrator`, with the lab tests.
3. **M9.SSS.2.** The walk stage in the wavefront: exit NEE, continuation, limits, LPE, streams; husk tests against Karma (5.10).
4. **M9.SSS.3.** Chromatic MIS, the cap statistic, `sss_traceset`, performance (Dwivedi guiding if the budget allows).

---

## 6. Not measured / open

- **Exit normal.** Karma's exit normal (shading or geometric) and its entry normal on smooth-shaded low-poly meshes.
- **Back faces and nesting.** A walk entering from a back face (camera inside, after refraction into the object); SSS objects inside glass or nested SSS objects.
- **Opacity.** Cut-out opacity on SSS meshes, inside the walk.
- **`sss_traceset`.** Not rendered; described from the help and the registry.
- **Light multipliers and AOVs.** Lights' `diffuse` multiplier and `contribs` = `sss` on the exit's NEE; Karma's albedo AOV of an SSS material.
- **Karma internals.** Karma's exact cap and Russian-roulette rule: the 256 and the mean-throughput roulette are inferences from energy and ray counts. Karma XPU was not probed.
- **Other renderers.** Arnold's `randomwalk_v2` boundary, RenderMan's non-exponential flights and Cycles' fitted coefficients are from memory, not verified here.
- **Lab detail.** In the α ≥ 0.99998 rows of E4 a few walks reach the 65536-segment histogram limit.

## 7. Lab summary (`probes/sss_lab.cpp`)

- **E1:** albedo inversions × entry models × anisotropy.
- **E2:** Dwivedi guiding.
- **E3:** chromatic strategies.
- **E4:** segment counts and caps.
- **E5:** sphere furnace.
- **E6:** slabs and headlight.

All estimators are unbiased: the means agree across strategies within 2-3 standard errors. Each walk is weighted by α per scattering, with Russian roulette below a throughput of 0.1. `sss_lab.exe all 0 0.5 1` takes 452 s on 8 threads.

## 8. References

- Chiang, Kutz, Burley 2016. "Practical and Controllable Subsurface Scattering for Production Path Tracing". SIGGRAPH Talks. (from memory, and via the comment in `mxpbr.h:263`)
- Burley 2015. "Extending the Disney BRDF to a BSDF with Integrated Subsurface Scattering". SIGGRAPH Course. (from memory)
- OpenPBR Surface 1.1 specification; "OpenPBR: Novel Features and Implementation Details", SIGGRAPH 2025 Physically Based Shading course notes v1.2, §3.1.2 (cited by `mxpbr.h:271`).
- van de Hulst 1980. *Multiple Light Scattering*, Academic Press (the half-space albedo formula).
- Křivánek, d'Eon 2014. "A Zero-variance-based Sampling Scheme for Monte Carlo Subsurface Scattering". SIGGRAPH Talks.
- Meng, Hanika, Dachsbacher 2016. "Improving the Dwivedi Sampling Scheme". Computer Graphics Forum (EGSR).
- Wrenninge, Villemin, Hery 2017. "Path Traced Subsurface Scattering using Anisotropic Phase Functions and Non-Exponential Free Flights". Pixar Technical Memo 17-07.
- Wilkie et al. 2014. "Hero Wavelength Spectral Sampling". EGSR. Miller, Georgiev, Jarosz 2019. "A Null-Scattering Path Integral Formulation of Light Transport" (spectral MIS).
- Blender Cycles source, `kernel/integrator/subsurface_random_walk.h`, `kernel/closure/bssrdf.h` (from memory).
- pbrt-v4, `src/pbrt/bssrdf.h`, `materials.h` (`SubsurfaceMaterial`, `TabulatedBSSRDF`, `SubsurfaceFromDiffuse`) (from memory).
- Autodesk Arnold user guide, Standard Surface "Subsurface" (`randomwalk`, `randomwalk_v2`) (from memory).

## 9. Probe files (`docs/research/m9/probes/`, prefix `sss_`)

| File | Purpose |
|---|---|
| `sss_lab.cpp`, `sss_build.bat` | the prototype lab (E1-E6), standalone C++20; `sss_build.bat <outdir>` |
| `sss_out_lab.txt` | its full output (`sss_lab.exe all 0 0.5 1`) |
| `sss_scenes.py` | writes the 47 USDA scenes, `common.usda` (LPE render vars) and `scenes.json` (what to measure) into a scratch directory; plain Python |
| `sss_renders.ps1` | renders them with Karma (`-Renderer BRAY_HdKarma`) or Nova (`-Renderer Nova -Stage build\vs2022\stage\Release\nova`) through `husk_env.ps1` |
| `sss_measure.py` | reads every EXR part (OpenImageIO, Houdini's Python) and prints the statistics |
| `sss_out_karma.txt`, `sss_out_nova.txt` | the measurements (scratch paths removed) |
| `sss_out_karma_rays.txt` | husk's ray counters per Karma scene |

Reproduce:

```
python sss_scenes.py <dir>          (Houdini's python313)
powershell -NoProfile -ExecutionPolicy Bypass -File docs\research\m9\probes\sss_renders.ps1 -Dir <dir>
powershell ... sss_renders.ps1 -Dir <dir> -Renderer Nova -Stage build\vs2022\stage\Release\nova
python sss_measure.py <dir> BRAY_HdKarma Nova
```
