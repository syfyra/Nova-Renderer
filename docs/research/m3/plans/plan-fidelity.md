# Nova M3 implementation plan: Shading core

ShaderGraph IR, bytecode VM, Standard Surface / OpenPBR, OIIO textures. Milestone result per `docs/PLAN.md`: "MaterialX-Standard-Surface aus Solaris rendert korrekt".

## 0. Key decisions

| # | Decision | Reason |
|---|---|---|
| K1 | **Where semantics come from, in this order:** (1) the MaterialX 1.39.5 graphs in `$HFS/houdini/materialx/libraries`; (2) where a graph has a known bug or drops a documented parameter, the OpenPBR 1.1.1 / Autodesk spec, which matches Karma here; (3) every analytic fit is replaced by an exact integral (LUT). | The emphasis is fidelity. The graph is what Houdini users see. The fits are off by up to 3.5–3.9 %, so they fail furnace tests. |
| K2 | **Pattern nodes follow Karma by default.** `nova:mtlxSemantics = "karma"` (default) or `"spec"` (MaterialX 1.39.5 genglsl). The switch affects only the ops where the two were measured to differ. | Karma CPU is the plan's material reference. For pattern nodes the differences are differences in meaning (ramptb orientation, safe math, the `constant` address mode), and they show up directly in look-dev. |
| K3 | **BSDF math follows K1, not Karma's approximations.** Measured Karma deviations get documented, per-region tolerances in the reference tests. | Karma's deviations are approximation errors: Schlick-fit dielectric, uncompensated rough glass, sheen rendered as a Lambert lobe, a red cast on thin film. Karma XPU differs from CPU too. |
| K4 | **Standard Surface, OpenPBR, UsdPreviewSurface and surface_unlit are intrinsic closures.** Their nodegraphs are not expanded. Each emits at most 8 lobes from one shared lobe library. | Lobe-exact importance sampling, a fixed POD closure list, and GPU-friendly code (the Cycles SVM "principled" pattern). |
| K5 | **All other MaterialX nodes are expanded at run time** with Houdini's MaterialX C++ API: a per-material `mx::Document` with `setDataLibrary(sharedLib)`, then `flattenSubgraphs` (0.18 ms per material). This yields about 60 width-generic native opcodes. | Karma expands exactly the same graphs (`karmaNodeGraphs.json` has identical node counts). User libraries work without an extra step. |
| K6 | **One bytecode, two executors.** A per-lane executor (`NOVA_HOST_DEVICE`) for CUDA, Embree filter callbacks and as test oracle. An SoA batch executor with 32 lanes (CPU) in the Shade stage. | Measured: 0.15 ns per instruction per lane (batch) vs 3.0–3.8 ns (per lane). A 32-lane CPU batch corresponds to one CUDA warp. |
| K7 | **Parameter values never fold.** Each material has a uniform block. Parameter-only subgraphs are hoisted into a host prologue. A value edit with the same structure hash means no recompile. | Required for IPR, under 100 ms per the plan. Same idea as MDL class compilation. |
| K8 | **Hydra side:** read `HdMaterialSchema` directly from the terminal scene index; `GetMaterialResource` is only the fallback. Context chain `nova → kma → mtlx → ""`. | Without `kma`, every Karma Material Builder material is ignored. The scene index keeps colorSpace/typeName separate, keeps terminal output names, and keeps `nodeTypeInfo`, which M5 needs. |
| K9 | **Textures:** a private OIIO `TextureSystem` on the shared `ImageCache` (`flip_t=1`, `gray_to_rgb=1`, `stochastic=3`). Color is baked by converting to half-float, linear, tiled, mipmapped EXR in a disk cache. Linear gamut changes are a 3×3 matrix applied after the lookup. Footprints come from ray cones with a 0.25 pixel scale. | OIIO 2.5.18's `colortransformid` is wrong for untiled files and automip levels, and it quantizes 8-bit data. Karma's MIP choice was measured as log2(texels per pixel) − 2. |
| K10 | **Cut-out opacity is decided stochastically in accelerator filter callbacks**, for both closest-hit and shadow rays. The decision is deterministic per (ray random, mesh, prim, instance). | One mechanism with the same structure as an OptiX any-hit program. No per-path transparent-depth state is needed. |

## 1. Scope

### 1.1 What M3 delivers

- **Core shading:** ShaderGraph IR, optimizer, register allocation, 16-byte bytecode, batch and lane VM, about 60 opcodes, MaterialX noise bit-exact to Karma and the MaterialX reference.
- **BSDF lobe library:** GGX (anisotropic, VNDF / bounded VNDF), exact dielectric and conductor Fresnel, F82-tint, artistic IOR, Lambert, translucent, QON, EON, Charlie (MaterialX form), Zeltner LTC, rough dielectric R/T/RT, thin-walled dielectric, thin-film Airy. Energy-compensation LUTs.
- **Standard Surface 1.0.1 and 1.0.0, all parameters:**
  - thin_walled transmission per the Autodesk spec and Karma;
  - `transmission_depth` as Beer absorption;
  - subsurface as a Lambert fallback (see 1.2).
- **OpenPBR 1.1.1, all parameters except dispersion and transmission scatter:** coat darkening, F82 metal, IOR modulation including the TIR fix, thin-walled subsurface per the spec.
- **UsdPreviewSurface** (scope addition, D5) and **`ND_surface_unlit`**, which the node probes need.
- **MaterialX patterns:** all 495 stdlib primitives listed as Tier 1 in mtlx-nodes §5.4, plus all 198 stdlib nodegraphs and Houdini's `hmtlx*`/cmlib graphs through expansion.
- **`kma` render context:** MaterialX nodes inside Karma Material Builder networks render. `kma_*` natives get a warning and their default output until M4.
- **Material sprim and IPR:** structure-hash program cache, value-only fast path, network diffing, GeomSubsets, all numeric primvars (mesh and instance level), Karma tangent frame from `st`, normal maps, bump via `heighttonormal`.
- **Textures:** OIIO TS, UDIM (`<UDIM>`, `<UVTILE>`), address modes, filter types, auto-conversion, `.rat` through `imaketx`, color-space rules as in Karma, ray-cone footprints, stochastic filtering, missing-texture policy.
- **Integrator:** closure-based Shade stage sorted by material, transmission-aware NEE/MIS, delta lobes, emission, cut-out opacity, single-medium Beer absorption, Russian roulette with an `etaScale` term.
- **Verification:** chi², furnace and energy suites; VM differential tests; VEX and MaterialX GLSL goldens; Karma node probes; 8 new Karma/analytic reference scenes.

### 1.2 Deferred

| Item | Goes to | Note |
|---|---|---|
| Random-walk subsurface | M9 (plan) | M3 uses Lambert(subsurface color). With albedo scaling that is exactly OpenPBR's "Flexibility" fallback `(1−E_spec)·C`. Scenes with SSS stay out of the strict Karma comparisons until M9. |
| `transmission_scatter`, scatter anisotropy (interior medium) | M8 (volumes) | Karma ignores them too. M3 uses σa = μt, as Karma does. |
| Dispersion (`transmission_dispersion`, Abbe) | no milestone in PLAN.md, see Q8 | Needs hero wavelengths (stream `kWavelength` is reserved). Warns once per material. |
| Generic pbrlib BSDF graphs (user mix/layer of `dielectric_bsdf`…), closure-space `mix` of surface shaders | M4 | Flattened into the same lobe list with an albedo op. `kMaxLobes` becomes 16. M3 warns and renders the `bg` input. |
| `kma_*` natives (rayswitch, ao, curvature, ramps…), MikkT tangents (`karma_mikkt`), `hextiledimage`, `flake2d/3d`, matrix ops, string/filename `geompropvalueuniform` (per-prim texture paths) | M4 | Mapping tables plus Nova Material Builder. |
| VEX networks (`vex` context) | M5 | `"vex"` joins the context list only then, otherwise it would hide the universal preview fallback. |
| `aov:*` inputs on terminals, LPE classes | M6 | M3 already records the lobe class per path (flags) for later LPE use. |
| Primvar filtering (upload only what materials use), ray-cone curvature refinements, kernel specialization | M7 | |
| Emissive surfaces as light-sampled sources (mesh lights, light BVH) | M9 | M3 adds emission only on hits (MIS weight 1). |
| Displacement terminal | see Q4 | M3 ignores it with one warning per material. |

### 1.3 Deviations from docs/PLAN.md

- **D1:** "OpenPBR/Standard Surface komplett" excludes dispersion, transmission scatter and random-walk SSS (see 1.2). These need spectral rendering, volumes and M9 random walks, which the plan schedules later.
- **D2:** The plan names Karma CPU as the material reference. For BSDF math Nova follows the MaterialX graphs and specs (K1/K3), and Karma comparisons carry documented per-region tolerances. Pattern nodes follow Karma (K2).
- **D3:** M3 already reads the `kma` context. The plan puts `kma_*` mapping in M4; without `kma` in M3, Solaris' default material builder would not render at all. Only the MaterialX content renders in M3.
- **D4:** The plan's translator list ("MaterialX-Standardnodes") includes pbrlib closure nodes. Generic closure graphs move to M4 together with the Nova Material Builder. Standard Surface and OpenPBR are intrinsic in M3.
- **D5:** UsdPreviewSurface (plus `UsdUVTexture`, `UsdPrimvarReader_*`, `UsdTransform2d` through their MaterialX graphs) is added in M3. The universal context is the fallback for every material, including husk's auto-generated previews. Low effort, because it uses the same lobes.
- **D6:** The plan lists "Reziprozität" under verification. Albedo-scaling layering (MaterialX, OpenPBR, Karma) is non-reciprocal by definition, so reciprocity is tested per lobe only.
- **D7:** Light textures (dome, rect) keep the full-image loader `HdNovaTextureCache`, because importance maps need the whole image. They adopt the shared color-space rules; the OIIO TS serves surfaces only.
- **D8:** The `IAccel` interface gains an opacity filter hook. The plan describes only intersect/occluded; this is required for cut-outs, and OptiX any-hit has the same structure.

## 2. Architecture

### 2.1 File layout (N = new, C = changed)

```
core/
  math/simd8.h                 N  AVX2 vfloat8/vint8 wrapper (noise kernels); scalar fallback on CUDA
  math/sobol.h                 C  stream::kSpare(7) -> stream::kTexture (1D); kOpacity used; kLayer stays reserved
  scene/attributes.h           N  MeshAttribute, InstanceAttribute, well-known attribute ids
  scene/mesh.h                 C  attributes, triangleFace; displayColor/cornerColors/hasDisplayColor removed
  scene/material.h             N  MaterialId, MaterialDesc, MaterialRecord, MaterialTable
  scene/scene.h/.cpp           C  materials, mesh material slots, attributes, ShadingPoint, shading view
  geometry/subdiv.h/.cpp       C  generic SubdivPrimvar (Bfr vertex/varying/faceVarying), base-face ids
  accel/accel.h, embree_accel.cpp  C  RayBatch::opacityRnd, IOpacityEvaluator, setMeshOpacity()
  bsdf/bsdf_common.h           N  sqr, saturate, lerp, pow5/6, reflect, refract, safeSqrt
  bsdf/microfacet.h            N  GgxDistribution (D, t, G1, vis, VNDF + bounded cap, pdfs), alphaMtlx, alphaOpenPbr, isDelta
  bsdf/fresnel.h               N  dielectric (exact, polarized), conductor (n,k, phase), F82-tint (+avg), artisticIor, openPbrEta
  bsdf/thin_film.h             N  fresnelAiry (Belcour-Barla, 2 iterations), grey-DC normalization, airyAlbedo
  bsdf/diffuse.h               N  Lambert, translucent, QON (+ albedo fit), EON (+ CLTC/uniform sampling)
  bsdf/sheen.h                 N  Charlie (MaterialX no-G form), Zeltner LTC (MaterialX fits)
  bsdf/conductor.h, dielectric.h  N  microfacet R; dielectric R / T / RT / smooth delta / thin-walled R'T'
  bsdf/energy.h                N  BsdfTables (POD pointers), lut2/lut3, msCompensation, dielectric albedos
  bsdf/tables.cpp + tables/bsdf_tables.inc  N  generated arrays (committed) + generator hash
  bsdf/shading_frame.h         N  ensureValidSpecularReflection (Cycles), Karma tangent frame, normal-map helpers
  bsdf/closure.h               N  LobeType, Lobe, ClosureList, BsdfSample, prepare/eval/sample
  bsdf/surface.h               N  SurfaceModel, SurfaceField, SurfaceInputs, buildStandardSurface/OpenPBR/Preview/Unlit
  bsdf/lambert.h               removed (replaced by diffuse.h; BsdfSample moves to closure.h)
  shading/ir.h/.cpp            N  IrType, IrRef, IrNode, IrGraph, IrBuilder (C++ test/graph construction)
  shading/ops.h                N  scalar op semantics (karma/spec variants), own polynomial transcendentals
  shading/mx_noise.h, mx_noise_simd.h  N  Perlin/fBm/cell/Worley (scalar, verified port) + 8-wide AVX2
  shading/program.h            N  Op, Instr (16 B), Global, OutputBinding, Program, ProgramView
  shading/compiler.h/.cpp      N  lower -> fold -> CSE -> fold -> DCE -> uniform hoist -> derivatives/shift clones
                                  -> Sethi-Ullman -> first-fit rows -> emit + structure hash
  shading/vm_lane.h            N  NOVA_HOST_DEVICE per-lane executor
  shading/vm_batch.h/.cpp      N  CPU batch<8|16|32> executor
  shading/shade_context.h      N  per-lane hit references for Primvar/Global/Tex2D/TransformSpace
  texture/texture.h            N  TextureId, TexWrap, TexFilter, TexSampler, TexQuery, ITextureSampler
  texture/ray_cone.h           N  cone transfer, footprint ellipse, uvGradients, curvature, scatterSpread
  texture/memory_sampler.h/.cpp N in-memory bilinear+MIP reference sampler (unit tests, no OIIO)
  integrator/shade.h/.cpp      N  hit sorting, material evaluation per chunk, closure gather/build
  integrator/wavefront.h/.cpp  C  path state (flags, etaScale, sigmaA, cone), closure NEE/sampling, delta MIS
  integrator/settings.h        C  mtlxSemantics, textureFilterWidth, textureBlur, frame, fps
hydra/
  material/material.h/.cpp        N  HdNovaMaterial sprim
  material/networkSource.h/.cpp   N  scene-index reader + GetMaterialResource fallback, context chain
  material/mtlxSearchPaths.h/.cpp N  library location (env vars, then MaterialXCore.dll location)
  material/nodeDefDatabase.h/.cpp N  the only file that includes MaterialX; library + POD nodedef records
  material/translator.h/.cpp      N  pure function: network -> TranslatedMaterial (IR, params, textures, attrs, hash)
  material/lowering.h/.cpp        N  (category, type) -> IR emission table, defaultgeomprop lowering
  material/usdAliases.h           N  UsdPreviewSurface/UsdUVTexture/UsdPrimvarReader/UsdTransform2d ids; legacy renames
  material/registry.h/.cpp        N  SdfPath->MaterialId, ProgramCache, attribute registry, warn-once
  textureSystem.h/.cpp            N  OIIO TS, ITextureSampler implementation, texture registry
  textureConvert.h/.cpp           N  make_texture auto-tx, imaketx for .rat, usdz extraction, disk cache
  colorSpace.h/.cpp               N  shared source-color-space decision (surfaces + light textures)
  textureCache.cpp                C  uses colorSpace.cpp
  mesh.h/.cpp, meshSubdiv.cpp     C  attributes, material slots, GeomSubsets, DirtyMaterialId, BLAS-preserving rebinds
  instancer.h/.cpp                C  generic instance-rate primvars
  renderDelegate.h/.cpp           C  material sprim, contexts, new settings, IsParallelSyncEnabled(material)
  renderParam.h, renderPass.cpp   C  registry + texture system; settings; re-translate on semantics change
  diagnostics.h                   C  TF_DEBUG HDNOVA_MATERIAL (network dump, chosen context)
cmake/NovaHoudini.cmake           C  Nova::MaterialX (MaterialXCore, MaterialXFormat imported targets)
tools/nova_bsdf_precompute/       N  LUT generator (core headers + TBB), custom target nova_bsdf_tables
tools/nova_imgcmp/                C  --region name,cx,cy,r,relTol (repeatable), region JSON output
tests/                            see section 6
```

### 2.2 Key data structures

**Attributes (primvars).** Replaces `cornerColors`. Every numeric primvar is uploaded (see Q9).

```cpp
// core/scene/attributes.h
using AttributeId = uint32_t;                 // global name id, assigned by the host registry
constexpr AttributeId kAttrDisplayColor = 0, kAttrSt = 1, kAttrDisplayOpacity = 2; // fixed by convention
struct MeshAttribute {                        // object-space values, no motion
    AttributeId name; uint8_t components;     // 1..4 (int/bool stored as exact floats)
    PrimvarInterp interp;                     // Constant | Uniform | Vertex (incl. Varying) | FaceVarying
    std::vector<float> data;                  // Constant: c; Uniform: per authored face (via triangleFace);
};                                            // Vertex: per point; FaceVarying: 3*c per triangle corner
struct InstanceAttribute { AttributeId name; uint8_t components; std::vector<float> data; }; // per instance
// MeshDesc gains: std::vector<MeshAttribute> attributes; std::vector<uint32_t> triangleFace;
// MeshRecord gains: std::vector<InstanceAttribute> instanceAttributes (replaces instanceColors),
//                   material slots + per-(slot, program-attribute) remap (filled at commit)
```

Lookup order for the `Primvar` op: the mesh attribute in any interpolation, then the innermost instance attribute (the existing displayColor rule, generalized), then the op's default operand.

**Shading point.** This is today's `resolveHit`, extended. Optional fields are computed only when the material's `globalMask` asks for them.

```cpp
struct ShadingPoint {
    Vec3f P, Ng, N;            // world; Ng oriented by authored winding; N interpolated, NOT face-forwarded
    Vec3f Pobject, Nobject;    // prototype space (verified Karma semantics for instances)
    Vec2f bary; MeshId mesh; uint32_t prim, instance, face;
    Vec2f st; Vec3f dPdu, dPdv;            // from the triangle's st corners (stDerivs), if st exists
    Vec3f T, B;                            // Karma TB: T = norm(dPdu - N(N.dPdu)), B = norm(NxT)*sign((Txdpdv).N);
                                           // no st: first-edge parameterization; degenerate: makebasis
    Vec3f dPdx, dPdy;                      // ray-cone footprint axes x textureFilterWidth (2.3.6)
    Affine3f objectToWorld;                // instance transform at ray time
};
```

**Program (bytecode).** POD and uploadable. Adopted from the validated shader-vm prototype.

```cpp
namespace nova::shading {
enum class Op : uint16_t { End, Mov, Add, Sub, Mul, Div, Mod, Mad, Mix, Clamp, Min, Max, Abs, Sign, Floor, Ceil,
    Round, Fract, Sqrt, Pow, Exp, Ln, Sin, Cos, Tan, Asin, Acos, Atan2, Smoothstep, Gt, Ge, Eq, Select, And, Or, Not,
    Dot, Cross, Normalize, Extract, Combine2, Combine3, Combine4, Swizzle, Remap, Luminance, RgbToHsv, HsvToRgb,
    Rotate2D, Rotate3D, TransformSpace, Blend /*plus|minus|difference|burn|dodge|screen*/, Comp4 /*over|in|mask|
    matte|out|disjointover|premult|unpremult*/, RampLR, RampTB, SplitLR, SplitTB, Global, Primvar,
    Noise, CellNoise, Fractal, Worley, Tex2D, NormalMap, HeightToNormal, JumpIfUniform, JumpIfAllZero, Count };
constexpr uint8_t kUniformA = 1, kUniformB = 2, kUniformC = 4, kSplatA = 8, kSplatB = 16, kSplatC = 32,
                  kDual = 64 /*operands/result carry d/dx, d/dy rows: 3*width rows*/, kExt = 128 /*ext words follow*/;
struct Instr { Op op; uint8_t width, flags; uint16_t dst, a, b, c; uint32_t imm; };    // 16 B
static_assert(sizeof(Instr) == 16);
// imm: op variant (karma/spec semantics bit, noise dim/outW, worley style/metric, blend mode, space from/to,
//      Global id + shift index 0/1/2, Primvar slot, Tex2D sampler index); >3 operands: ext words {u16 op, u8 flags}
enum class Global : uint8_t { P, Pobject, N, Ng, Nobject, T, B, I, St, Time, Frame, RayDepth, PrimId, InstanceId, Count };
struct OutputBinding { uint16_t field; uint8_t width; uint8_t uniform; uint16_t index; };  // -> SurfaceInputs float
struct Program {                       // immutable, shared by structure hash
    std::vector<Instr> prologue;       // uniform-only code, run on the host when params change
    std::vector<Instr> code;           // varying code (empty = "varyingFree")
    std::vector<Instr> opacityCode;    // DCE'd to the opacity output; empty = constant opacity
    std::vector<float> constants;      // folded literals (uniform slots 4..)
    std::vector<UniformInit> uniformInit;          // slot <- {zero | const k | param k | hoisted}
    std::vector<OutputBinding> outputs;            // SurfaceInputs bindings
    std::vector<TexSampler> samplers;              // per Tex2D instruction (texture SLOT, not id)
    std::vector<ParamDesc> params;                 // layout: key, type, offset (values live in MaterialDesc)
    uint32_t textureSlots, attributeSlots, rowCount, globalMask;
    SurfaceModel model; uint32_t flags;            // emissive, hasOpacity, needsTangents, needsDerivatives, thinWalled
    uint64_t structureHash;
};
}
```

- **Uniform block layout:** slots 0–3 are zero, then constants, then params, then hoisted results.
- **Row cap:** 1024 on the CPU (16-bit operands). The compiler records `rowCount`. Above 256 it emits a "would not fit M10 GPU limit" diagnostic. If compilation fails, the material falls back to the default material with a warning.

**Surface inputs.** The canonical parameter block covers both models (surface-models §6.6). Fields hold the authored values in the model's own units; the `build*` functions apply each model's semantics.

```cpp
enum class SurfaceModel : uint8_t { Lambert /*default material*/, StandardSurface, StandardSurface100,
                                    OpenPBR, PreviewSurface, Unlit };
enum SurfaceField : uint16_t {        // float offsets into SurfaceInputs::v; (3) = color/vector
  kBaseWeight=0, kBaseColor=1/*3*/, kDiffuseRoughness=4, kMetalness=5, kSpecularWeight=6, kSpecularColor=7/*3*/,
  kSpecularRoughness=10, kSpecularIor=11, kSpecularAnisotropy=12, kSpecularRotation=13,
  kTransmissionWeight=14, kTransmissionColor=15/*3*/, kTransmissionDepth=18, kTransmissionScatter=19/*3*/,
  kTransmissionScatterAniso=22, kTransmissionDispersion=23, kTransmissionExtraRoughness=24,
  kSubsurfaceWeight=25, kSubsurfaceColor=26/*3*/, kSubsurfaceRadius=29/*3*/, kSubsurfaceScale=32, kSubsurfaceAniso=33,
  kSheenWeight=34, kSheenColor=35/*3*/, kSheenRoughness=38,          // SS sheen | OpenPBR fuzz
  kCoatWeight=39, kCoatColor=40/*3*/, kCoatRoughness=43, kCoatAnisotropy=44, kCoatRotation=45, kCoatIor=46,
  kCoatDarkening=47, kCoatAffectColor=48, kCoatAffectRoughness=49,
  kThinFilmWeight=50, kThinFilmThickness=51, kThinFilmIor=52,
  kEmissionWeight=53, kEmissionColor=54/*3*/, kOpacity=57/*3*/,
  kNormal=60/*3*/, kCoatNormal=63/*3*/, kTangent=66/*3*/, kCoatTangent=69/*3*/,
  kThinWalled=72, kOpacityThreshold=73, kUseSpecularWorkflow=74, kCount=76 };
struct SurfaceInputs { float v[kCount]; SurfaceModel model; uint8_t pad[3]; };   // 308 B
```

- **Filling it per lane:** copy the material's prebuilt template (uniform bindings already resolved at commit), apply the varying patch list `(field ← row)`, normalize the vectors.
- **Subsurface mean free path:** OpenPBR maps `radius_scale` (color) to `kSubsurfaceRadius` and `subsurface_radius` to `kSubsurfaceScale`, so the product is the mean free path in both models.

**Closures.** Built per lane at the hit and consumed right away. They are never stored in SoA queues; on the GPU they live in local memory.

```cpp
enum class LobeType : uint8_t { Lambert, Translucent, Transparent /*unlit straight-through delta*/, Qon, Eon,
    Charlie, Zeltner, ConductorNK, ConductorF82, DielectricR, DielectricT, DielectricRT, ThinDielectric };
enum : uint8_t { kLobeDelta = 1, kLobeTransmit = 2, kLobeThinFilm = 4, kLobeSplitAlpha = 8, kLobeInside = 16 };
struct Lobe {                         // POD, 96 B
    Spectrum weight;                  // all layering/mix/tint/darkening factors, evaluated for wo
    float pick;                       // selection probability (cdf in ClosureList)
    LobeType type; uint8_t flags, frame /*0 base, 1 coat*/, mode;
    float ax, ay, axT, ayT;           // GGX alphas (R; T split for SS transmission_extra_roughness)
    float etaF, etaT;                 // Fresnel / refraction IOR ratio (equal in OpenPBR 1.1.1; split ready for 1.2)
    float energy;                     // MS compensation factor or 1/E
    Spectrum c0, c1;                  // F0|n|rho|tint R ; F82 B|k|tint T
    float tfThicknessNm, tfIor, tfWeight;
};
struct ClosureList {
    static constexpr int kMaxLobes = 8;   // worst case 7: fuzz, coat, metal, dielectric, thin-SSS x2, diffuse
    Lobe lobe[kMaxLobes]; float cdf[kMaxLobes];
    Frame frame[2];                       // base (t = rotated anisotropy tangent), coat
    Spectrum emission, albedo, sigmaA;    // albedo = sum w_j E_j(wo) (AOV, denoiser guide)
    uint8_t count, nonDeltaMask, thinWalled, flags;   // flags: hasTransmission, entering, allDelta
};
struct BsdfSample { Vec3f wi; Spectrum fcos; float pdf; uint8_t flags; uint8_t lobe; }; // pdf < 0: delta
NOVA_HOST_DEVICE Spectrum evalClosures(const ClosureList&, const BsdfTables&, Vec3f wo, Vec3f wi, float &pdf);
NOVA_HOST_DEVICE bool     sampleClosures(const ClosureList&, const BsdfTables&, Vec3f wo, Vec3f u, BsdfSample&);
```

**Materials in the Scene.**

```cpp
// core/scene/material.h
using MaterialId = uint32_t;
struct MaterialDesc {                                   // host side
    std::shared_ptr<const shading::Program> program;    // null = default material (Lambert(displayColor | 0.18))
    std::vector<float> params;                          // in program->params layout
    std::vector<TextureId> textures;                    // per program texture slot
    std::vector<AttributeId> attributes;                // per program attribute slot
};
struct MaterialRecord {                                 // kernel view, 48 B, in MaterialTable::records
    uint32_t codeOffset, codeCount, opacityOffset, opacityCount;   // into MaterialTable::code (programs deduped)
    uint32_t uniformOffset;                             // per material: zeros|consts|params|hoisted
    uint32_t templateOffset;                            // prebuilt SurfaceInputs (uniform bindings applied)
    uint32_t patchOffset; uint16_t patchCount, rowCount;   // varying bindings
    uint32_t samplerOffset, textureOffset;              // TexSampler table; slot -> TextureId
    uint32_t globalMask; uint8_t model, flags, pad[2];
};
struct MaterialTable {                                  // flat, device-uploadable; rebuilt incrementally at commit
    std::vector<MaterialRecord> records; std::vector<shading::Instr> code; std::vector<float> uniforms;
    std::vector<SurfaceInputs> templates; std::vector<shading::OutputBinding> patches;
    std::vector<TexSampler> samplers; std::vector<TextureId> textureIds;
};
// Scene: MaterialId createMaterial(); void updateMaterial(MaterialId, MaterialDesc);
//        void updateMaterialParams(MaterialId, std::vector<float>, std::vector<TextureId>);   // IPR fast path
//        void removeMaterial(MaterialId);
//        void setMeshMaterials(MeshId, std::vector<MaterialId> slots, std::vector<uint16_t> faceSlot); // no BLAS rebuild
//        void updateMeshAttributes(MeshId, std::vector<MeshAttribute>);                             // no BLAS rebuild
//        void setMeshInstanceAttributes(MeshId, std::vector<InstanceAttribute>);
//        void setTextureSampler(const ITextureSampler*); void setShadingGlobals(frame, fps, filterWidth, blur, semantics);
//        MaterialId materialOf(MeshId, uint32_t prim) const;      // slot table + triangleFace
```

`commit()` does the following:
- reruns the prologue of materials whose params changed and rewrites their uniform block in place (sizes are stable when the structure is unchanged);
- rebuilds the per-(mesh, slot) attribute remap for meshes whose slots or slot programs changed;
- updates the opacity filter registration per mesh (any slot with `hasOpacity`).

**Textures in the core** (no OIIO). `ITextureSampler::sample2D(const TexSampler&, const TexQuery&, float* out, uint8_t* status)` is a virtual call once per batch, like `IAccel`. The structs are as in textures §6.1, except that `TexSampler` has no fallback field: the op selects its `default` operand where `status = 1`, so `default` can be connected. `TexSampler.texture` is a material texture slot, resolved through `MaterialRecord::textureOffset`. That makes a texture path change a value edit.

**BSDF tables.** `struct BsdfTables { const float *ggxE, *ggxEavg, *ggxABC, *dielRT, *dielRTinv, *dielR, *fAvgDiel, *charlieE; float xyzToRender[9]; }`. On the CPU they are static arrays; on the GPU one uploaded blob passed in the launch parameters.

### 2.3 Execution model in the wavefront integrator

**2.3.1 Path state.** Added to `WaveQueues`, indexed by path slot:
- `uint8_t flags`: `kLastDelta`, `kLastTransmit`, `kLastDiffuse`, `kLastGlossy`, `kInMedium`. These are the ray types for M4 rayswitch and M6 LPE.
- `float etaScale`.
- `Spectrum sigmaA`.
- `float coneWidth, coneSpread`.
- `bsdfPdf` stays; `-1` means the last bounce was delta.

**2.3.2 Shade stage.** `shadeSurfaces` is split into passes; `integrator/shade.cpp` holds the material part.

1. **Pass 1 (existing logic):** camera-visible lights, misses, area lights along BSDF rays, alpha. The cone width at the hit is `W = coneWidth + coneSpread·t`. If `kInMedium`: `beta *= exp(−sigmaA·t)` (a miss while inside a medium gives 0 unless σa = 0). Survivors become `(rayIndex, materialOf(mesh, prim))`.
2. **Sort:** counting sort by material id (0.6 ns per hit) when `materialCount ≤ 4·hits`, otherwise `std::sort` on `(id << 32 | ray)`. Determinism is unaffected: samples depend only on (pixel, index, stream), and each path's radiance additions keep their bounce order.
3. **Per material group, in chunks of `kLanes = 32`:**
   - `resolveShadingPoint` per lane, computing only the parts `globalMask` needs (P, Pobject, N, Ng, Nobject, T, B, I, St, primvar references). Write the global rows, including dual rows (`dPdx`/`dPdy` seeds) and shifted rows for bump cones. Pad lanes `[n, 32)` with copies of lane n−1.
   - Run the VM unless `varyingFree`: `runBatch<32>` if n > 16, `<16>` if n > 8, `<8>` if n > 2, else `runLane` per lane.
   - Per lane: build `SurfaceInputs` (template plus patch), then `buildClosures(model)` gives a `ClosureList`, then:
     - **Emission:** `radiance += beta·cl.emission` with MIS weight 1, also at depth 0 and in the last iteration.
     - **Last iteration** (`depth == maxDepth`): stop after emission. Lobes are not built.
     - **NEE:** one light (existing power-based selector).
       - `f·cos, pdf = evalClosures(cl, wo, ls.wi)` sums all non-delta lobes, each with its own frame and |cos|.
       - Without transmission, `ls.wi` is accepted only above the face-forwarded Ng. With transmission or translucency (`cl.flags & hasTransmission`), both sides are accepted, subject to `sign(dot(Ng,wi)) == sign(dot(Ns_lobe,wi))`.
       - The shadow-ray origin is offset to the side of `wi`.
       - Lists with only delta lobes skip NEE.
       - Weight: `lightSampleWeight(strategy, lightPdf, pdf, ls.delta)`.
     - **Continuation:** `sampleClosures(cl, wo, get3D(bounce(d, kBsdf)))`.
       - `u.z` picks the lobe from the cdf; the remainder `(u.z − cdf[j−1])/p_j` drives the lobe's inner discrete choice (R vs T, EON uniform vs CLTC), so the Sobol xy pair stays intact.
       - Non-delta: the class is decided by Ng (reflect iff `dot(wi,Ng)·dot(wo,Ng) > 0`) and must match the lobe's class; then the full mixture `fcos`/`pdf` is evaluated at `wi` (one-sample MIS, balance heuristic over lobes).
       - Delta: `fcos = w_j·value/p_j`, `bsdfPdf = −1`.
       - `beta *= fcos/pdf`, or the delta weight. On a non-thin transmission: `etaScale *= η_rel²`, and enter or leave the medium (`sigmaA` from the lobe, `kInMedium`). Entering vs exiting comes from `dot(wo, authored Ng)`. Thin-walled never changes the medium.
       - Origin offset `offsetRayOrigin(P, transmit ? −ng : ng)`.
     - **Russian roulette** from `rrStartDepth`: `survive = clamp(maxComponent(beta)·etaScale, 0.05, 0.95)`.
     - **Ray cone update:** `spread' = spread − 2·k_c·W/(n·d)`, then for a non-delta lobe `max(spread', α_lobe)` (diffuse counts as 1), and `width' = W`.
4. **MIS at escape and area-light hits:** `bsdfSampleWeight(strategy, bsdfPdf, lightPdf)` returns 1 when `bsdfPdf < 0`, for every strategy. Today `powerHeuristic(0, ·) = 0` would make light reached through mirrors and glass vanish.

**2.3.3 First-hit AOVs.** `renderFirstHitAovs` runs the same material evaluation, sorted, on its pixel-center rays (`rnd = −1`, deterministic texture filtering).
- `albedo = cl.albedo`.
- `normal` = the closure base normal after normal mapping, face-forwarded.

**2.3.4 Opacity (cut-outs).** Accel filter callbacks (K10).
- Embree intersect and occluded filters are installed only on geometries whose slot materials have `hasOpacity`.
- The filter resolves the hit and runs `opacityCode` with the lane executor (texture lookups use the ray's cone).
- It accepts the hit iff `u < o`, with `u = fract(opacityRnd[ray] + hash01(mesh, prim, instance))`. `opacityRnd` is `get1D(bounce(d, kOpacity))`; shadow rays add a constant salt. The hash keeps duplicate filter calls consistent.
- Camera rays that pass every surface are misses, so alpha follows `1 − ∏(1 − o_i)`. Karma measured 0.75 for o = 0.5 on a sphere.
- Transmission never reduces alpha.

**2.3.5 Ray types and limits.** M3 keeps the single `nova:maxDepth` (lit events) and records the class of the last bounce in `flags`. Karma's per-type limits are set to 64 in all material reference scenes.

**2.3.6 Texture footprints.**
- **Camera ray:** `spread0 = atan(2·tan(fovY/2)/H)`; `width0` = pixel size (orthographic) or `near·spread0` (perspective).
- **Hit:** footprint vectors `a1 = h1·W/|f·d|` and `a2 = (f×h1)·W` (JCGT 2021, eqs. 8–9). `dPdx = k·a1`, `dPdy = k·a2` with `k = nova:textureFilterWidth (0.25)`, times the per-node `karma_width`.
- **Primvar derivatives:** `d attr/dx = ∇attr·dPdx`, with the gradient taken over the triangle plane (barycentric gradients).
- **Tex2D** receives `(s, t, ds/dx, dt/dx, ds/dy, dt/dy)` as full diameters, plus `rnd = fract(get1D(bounce(d, kTexture)) + φ·hash(slot))`.

**2.3.7 Derivatives in the VM.**
- **Dual rows** (`kDual`) cover the backward cone of every `Tex2D.texcoord`. Rules: add, sub, neg, mul, div, mad, mix, swizzle, extract, combine, matrix·vector / place2d, rotate2d, fract/mod/floor (passed through). Any other op propagates zero; Tex2D then falls back to the geometric `st` derivatives.
- **Bump** (`heighttonormal`, and `bump` = heighttonormal → normalmap) uses **shifted re-evaluation**, as Cycles does.
  - The compiler clones the height cone twice. `Global`/`Primvar` reads in the clones take shift index 1 or 2: P + δ·â1, P + δ·â2, with attributes moved by their gradients.
  - δ = max(0.1·|a|, 1e-4·(1 + |P|)).
  - `HeightToNormal(h, h1, h2, scale)` then applies Karma's world-space bump: P′ = P + Ng·h·scale; n′ = norm(cross(dP′1, dP′2)) oriented to Ng, expressed in the frame (tu⊥ng, tv⊥ng, ng), encoded ×0.5 + 0.5.
  - This works through noise and textures, so no dual-number noise variants are needed.

### 2.4 Running the same code on CUDA (M10)

- Every piece is POD with `NOVA_HOST_DEVICE`: `Program` code, `MaterialTable`, `BsdfTables`, `ClosureList`, the lobe functions, `ops.h`, `vm_lane.h`, `ray_cone.h`, `mx_noise.h` (scalar).
- **Per bounce:** a prefix-sum sort of hit indices by material, then one generic shade kernel in which each thread runs `runLane(code, float R[rowCount] local, U)`. A warp sees one material, so the `switch` does not diverge, row accesses coalesce and uniform reads broadcast.
  - Uniform conditions use `JumpIfUniform`; zero-weight skips use `JumpIfAllZero` (`__all_sync` on the GPU, a chunk test on the CPU).
  - Varying conditions become `Select`.
- **Textures:** `TexSampler`/`TexQuery` map one to one onto OptiX Toolkit DemandLoading `tex2DGrad(id, s, t, ddx, ddy, &resident, jitter)`. The GPU reads the converted EXRs, so the data is identical; only the filter kernels differ (an XPU-parity option can use bilinear + trilinear on the CPU).
- **Opacity:** `opacityCode` runs in OptiX any-hit with the same hash rule.
- **Determinism:** own polynomial transcendentals in `ops.h`; nvcc `--fmad=false` in parity tests.

## 3. BSDF model decisions

### 3.1 Lobes, formulas, sources

| Lobe | Formula / source | Sampling | Energy |
|---|---|---|---|
| GGX core | anisotropic D; stable height-correlated V = 1/(2(t_o\|μ_i\| + t_i\|μ_o\|)) (Heitz 2014; pbrt-v4 `scattering.h`) | spherical-cap VNDF (Dupuy & Benyoub 2023); **bounded** cap (Eto & Tokuyoshi 2023) for reflection-only lobes (62 % → 97 % valid samples at α 0.8, μ 0.95); full cap for transmission | n/a |
| Roughness mapping | SS: `mx_roughness_anisotropy` (r2 = clamp(r², ε, 1); aspect = √(1 − min(a, .98))). OpenPBR: α_t = r²√(2/(1 + (1−a)²)), α_b = (1−a)α_t. Clamp α ≥ 1e-4; delta if max(αx, αy) < 1e-3 | | |
| Dielectric R / T / RT | Walter 2007 with exact Fresnel per microfacet; BTDF × 1/η² (radiance transport); flipped frame for μ_o < 0 | VNDF; R/T split by F(ω_o·m)·tint | RT ×1/E_RT(μ_o, r, η) (η > 1 table or 1/η table when exiting). R-only: same 1/E_RT, layering albedo E_R^comp = E_R/E_RT. T from outside: BTDF × 1/E_T^ss, so after (1 − E_R^comp) its energy is E_T/E_RT, the compensated physical split |
| Thin-walled dielectric | sheet Fresnel (Cycles `bsdf_thin_glass_fresnel`): c = T_color^{1/\|cos θ_t\|}, T′ = c·t1·t2/(1 − (r2·c)²), R′ = r1 + T′·r2·c. T lobe = mirrored R lobe with α_t = saturate(α·√(3.4(η−1)(η−0.5)²/η³)) | as R; mirrored for T | R′ + T′ = 1 for c = 1 |
| Conductor (n, k) | exact conductor Fresnel; SS: `artisticIor(base·base_color, specular·specular_color)` (Gulbrandsen) | bounded VNDF | f·(1 + F_ms(1−E)/E), F_ms = F_avg·E_avg/(1 − F_avg(1 − E_avg)) (Turquin × Kulla–Conty, Cycles); F_avg from an F82 fit through F_c(1) and F_c(1/7) |
| Conductor F82 (OpenPBR metal) | Hoffman / MaterialX `mx_fresnel_hoffman_schlick`, F0 = base_weight·base_color, tint = specular_color, × specular_weight; **clamped ≥ 0**, average formula too | bounded VNDF | as above, closed-form F82 average |
| Thin film | Belcour & Barla Airy per microfacet (ω·m), 2 iterations (MaterialX `hwAiryFresnelIterations`); MaterialX Gaussian sensitivity; XYZ → rendering-space matrix **normalized so the DC term stays grey** (no Karma R/G 1.27 cast); T uses 1 − R_airy; F = lerp(F, F_airy, weight) | as host lobe | layering albedo `lerp(F_airy(μ_o), F0_airy·A + B, √α)` (ABC LUT) |
| Lambert / translucent | ρ/π; translucent in the opposite hemisphere | cosine | exact |
| QON (Standard Surface diffuse) | MaterialX `mx_oren_nayar_diffuse`: A = 1 − 0.5σ²/(σ² + 0.33), B = 0.45σ²/(σ² + 0.09), σ = diffuse_roughness, **uncompensated** (definition) | cosine | MaterialX albedo fit used **only** for lobe selection and the albedo AOV |
| EON (OpenPBR diffuse) | Portsmouth, Kutz & Hill 2024 (the paper's code, not the spec typo in ρ_ms); clamp ρ ≤ 1 inside ρ_ms | CLTC + uniform, defensive one-sample MIS with the fitted P_u (26× lower variance at grazing angles) | exact (albedo 1.0000) |
| Charlie (SS sheen) | MaterialX form D/(4(μ_i + μ_o − μ_iμ_o)), no G, D clamped at r ≥ 0.005 | uniform hemisphere | `charlieE` LUT for layering (exact furnace) |
| Zeltner LTC (OpenPBR fuzz) | MaterialX `mx_microfacet_sheen.glsl:97-188` fits, r ∈ [0.01, 1], R clamped ≥ 0; skip the lobe if \|a⁻¹\| < 1e-5 or R < 1e-5 | exact LTC (pdf = D_LTC, weight = R·color) | R(μ_o, r) is the definition |
| Transparent (unlit transmission) | delta pass-through with weight transmission·transmission_color | delta | n/a |

### 3.2 Energy LUTs

Generated offline by `tools/nova_bsdf_precompute <table>`:
- it calls the **runtime** sample/eval code with Nova's Owen-scrambled Sobol and TBB, accumulates in double, and writes `core/bsdf/tables/bsdf_tables.inc` (hex-float arrays, parameters and the SHA-1 of the generator source in the header);
- the output is committed; the custom target `nova_bsdf_tables` is not part of the default build (about 10 min on 16 threads).

Axes: r = (αxαy)^¼, y = √μ (μ ≥ 1e-4), z = √((η−1)/(η+1)) clamped to [1e-4, 0.99]; bilinear/trilinear lookups.

| Table | Size | Content | Samples per cell |
|---|---|---|---|
| `ggxE` | 32×32 | E_ss, F = 1 | 2²² |
| `ggxEavg` | 32 | 2∫Eμ dμ | 2²⁴ |
| `ggxABC` | 32×32×3 | ∫f₁cos·(1−s), ∫f₁cos·s, ∫f₁cos·μ_h(1−μ_h)⁶ with s = (1−μ_h)⁵, giving the exact single-scatter albedo of any Schlick/F82 Fresnel | 2²² |
| `dielRT`, `dielRTinv` | 32³ each | E_R + E_T for η and 1/η (energy units) | 2²⁰ |
| `dielR` | 32³ | E_R single scatter | 2²⁰ |
| `fAvgDiel` | 64 | exact 2∫Fμ dμ (the Kulla–Conty fit is −23 % at η 1.1) | quadrature |
| `charlieE` | 32×32 | albedo of the MaterialX Charlie form | 2²⁰ |

Total ≈ 405 KB. Measured 32² bilinear error 0.0020 max abs, vs 0.039 for the MaterialX fit.

### 3.3 Model construction (layering)

- **Albedo scaling** at ω_o: `layer(top, base)` multiplies the base weights by `1 − Σ w_top·E_top(ω_o)` with **untinted** E. This is MaterialX semantics and Karma was measured to do the same (G = 0.9585 at μ = 1 for specular_color (1,0,0)). `mix` lerps weights. Every factor is a per-hit constant, so the result is a flat weighted lobe list.
- **Standard Surface:** the closed form in surface-models §2.3:
  - coat factor `(1 − C·E_coat)·lerp(1, coat_color, C)`;
  - `coat_affect_color` exponent γ = 1 + C·coat_affect_color on base and subsurface color;
  - roughness r′ = lerp(r, 1, coat_affect_roughness·C·coat_roughness);
  - metal through artistic IOR;
  - dielectric tR = s·c_s, tT = X·c_t·(1 − s·E_R)/(1 − E_R), split α for `transmission_extra_roughness`;
  - sheen = Charlie; diffuse = QON with ρ = base_color^γ, weight `base`;
  - emission `emission·emission_color·[(1−C) + C·coat_color·(1−F0c)(1 − (1−μ_o)⁵)]`;
  - opacity = luminance(opacity) with ACEScg coefficients (graph default);
  - tangent rotation `T′ = T cos θ + (T×N) sin θ`, θ = 360°·rotation; coat tangent rotated about coat_normal.
- **OpenPBR:** the closed form in surface-models §3.4:
  - fuzz with Zeltner over N (graph uses N);
  - coat roughening of the base r′ = lerp(r, min(1, r⁴ + 2r_c⁴)^¼, C);
  - modulated η′ = openPbrEta(n_b, n_c, C, ξ_s), which also drives refraction (1.1.1);
  - **coat darkening with MaterialX K = 1 − (1 − F0c)/η_c²** (graph and Karma; the spec's E_F form differs: 0.3089 vs 0.2980) and E_b = lerp(lerp(base_color, subsurface_color, S), specular_weight·base_color, M);
  - coat absorption constant lerp(1, coat_color, C);
  - F82 metal; EON diffuse;
  - thin-walled subsurface per spec `½(1−g)C·f_R + ½(1+g)C·f_T`, not the shipped graph's C² bug;
  - thin film thickness µm × 1000; emission luminance used as radiance 1:1.
- **UsdPreviewSurface:** from `bxdf/usd_preview_surface.mtlx` graph semantics on the same lobes (Lambert, dielectric R with `ior` or specular-workflow Schlick, metallic as conductor, clearcoat R); `opacityThreshold > 0` gives a hard cut-out. Calibrated against Karma: 0.5139/0.2733/0.1530 for diffuse C, 0.2062 for defaults.
- **Unlit:** emission, Transparent lobe, opacity.
- **Lobe selection:** `p_j ∝ max(lum(w_j·Ê_j(ω_o)), 1e-4·Σ)` for lobes with weight > 1e-5. Ê comes from the LUTs and depends only on ω_o.
- **Back faces:**
  - opaque materials are double-sided (frame flipped);
  - non-thin materials with transmission > 0 and ω_o on the back side: lobes above the dielectric (coat, fuzz, sheen, R-only) are dropped. The material emits the full RT lobe with 1/η, the `dielRTinv` compensation and weight X·tint, plus the opaque base at (1−X) shading the back side. Karma does the same.
- **`transmission_color` with depth = 0** is applied at every refraction event, as in the graph. With depth > 0 the lobe tint is 1 and σa = −ln(clamp(color, 1e-5, 1))/depth (Karma's mapping; scatter ignored).

### 3.4 Robustness rules

1. Selection probabilities depend only on ω_o.
2. Lobes return f·|cos| against their own frame normal.
3. Each closure normal is bent with Cycles' `ensure_valid_specular_reflection` (threshold min(0.9·(ω_o·N_g), 0.01)); Karma's `_mx_normal_clamp` is equivalent. Sampling additionally rejects N_g/N_s disagreement.
4. Delta lobes are excluded from NEE and from the mixture pdf; `bsdfPdf = −1` makes the MIS weight 1.
5. NaN/Inf guards: reject when `!(pdf > 0) || !isFinite(fcos)`; clamp μ ≥ 1e-6 in lookups.
6. Inputs are sanitized: weights to [0, 1]; colors ≥ 0; multiply tints to [0, 1]; η ≥ 1e-4 (η = 0 gives F = 1, as the Velvet preset expects); α ≥ 1e-4; anisotropy 1 means α_b clamped.
7. The shading normal for NormalMap follows Karma: Gram-Schmidt on T and B, and `r −= Ng·(r·Ng)·1.001` if r crosses Ng. The flat map (0.5, 0.5, 1) must be bit-identical to no normal map.

## 4. Hydra side

### 4.1 Delegate

- **Sprim types:** add `HdPrimTypeTokens->material` to `SUPPORTED_SPRIM_TYPES`; `CreateSprim` returns `HdNovaMaterial`.
- **`GetMaterialRenderContexts()`** returns `{nova, kma, mtlx}`. Never `__all` or `hview`; `vex` only in M5. **`GetMaterialBindingPurpose()`** stays `full` (husk passes `[full, ""]`).
- **`IsParallelSyncEnabled(material)`** is enabled at the end of M3.7, after the stress test (4.7).
- **New settings:** `nova:mtlxSemantics` (token karma|spec), `nova:textureFilterWidth` (0.25), `nova:textureBlur` (0), `nova:textureAutoTx` (1), `nova:textureCacheDir` (`$HOUDINI_TEMP_DIR/nova_tx`, else `%TEMP%/nova_tx`), `nova:abortOnMissingTexture` (0), `nova:missingTextureColor` (unset, debug), `nova:textureStats` (0).
- **Semantics change:** changing `nova:mtlxSemantics` makes the registry re-translate every material from its cached network inside the render pass. No Hydra resync is needed.

### 4.2 Network reading

`HdNovaReadNetwork(terminalSceneIndex, id, ctx, &HdNovaNetworkSource)` is the verified reader from hydra-materials §4.2:
- it keeps `colorSpace` and `typeName` per parameter, terminal output names and `nodeTypeInfo`;
- an empty terminal path disables that terminal.

**Context chain per material:** `nova → kma → mtlx → ""`. The first context whose `surface` terminal translates to a known terminal type wins; with no such context the material gets the default material and one warning.

**Fallback without a terminal scene index** (test rigs): `GetMaterialResource` → `HdConvertToHdMaterialNetwork2`; move the `colorSpace:*`/`typeName:*` pseudo-params into the metadata; fill empty terminal output names from the nodedef's single output.

### 4.3 MaterialX expansion and translation

**`HdNovaNodeDefDatabase`** (built once with `std::call_once`):
- `mx::loadLibraries({"stdlib","pbrlib","bxdf","cmlib","nprlib","lights","houdini"})`, 41 ms, 898 nodedefs.
- Search paths in this order: `NOVA_MTLX_SEARCH_PATHS`, `PXR_MTLX_PLUGIN_SEARCH_PATHS` (user libraries), `PXR_MTLX_STDLIB_SEARCH_PATHS` (set by husk/Houdini), then `<dir of MaterialXCore.dll>/../houdini/materialx/libraries` via `GetModuleHandleW`/`GetModuleFileNameW` (nova_cli; `ArchGetAddressInfo` fails on Windows).
- Duplicate nodedef names: the first one wins, with one warning.
- POD records per nodedef: inputs from `getActiveInputs()` (type, default, defaultgeomprop, uniform, enum), outputs, implementation kind, intrinsic flag.
- Never call `doc->validate()`; Houdini's libraries fail it.

**Per material:**
1. **Canonicalize:**
   - drop pseudo-params;
   - resolve ids: USD aliases (`UsdPreviewSurface` → `ND_UsdPreviewSurface_surfaceshader`, `UsdUVTexture` → `ND_UsdUVTexture_23`, `UsdPrimvarReader_float…` → `ND_UsdPrimvarReader_float/vector2/vector3/vector4/integer/string`, `UsdTransform2d` → `ND_UsdTransform2d`) and legacy renames (`ND_normalmap` → `ND_normalmap_float`);
   - read the version from `config["mtlx:version"]`; if missing, assume "1.38" and run `upgradeVersion()`, as hdMtlx does;
   - DFS from the terminals in sorted input-name order gives canonical node indices, so node names never reach the hash.
2. **Expand**, under a global `std::mutex`, since the thread safety of MaterialX document caches is not established (≈0.2 ms per material):
   - build an `mx::Document` with `setDataLibrary(lib)`; never `HdMtlxCreateMtlxDocumentFromHdNetwork`, which deep-copies the library (18 ms);
   - `flattenSubgraphs("", keepIf(!intrinsic))` with intrinsic = Standard Surface (both versions), OpenPBR, UsdPreviewSurface, `surface_unlit`;
   - `topologicalSort()`;
   - copy the result into a POD `FlatNetwork`.
3. **Lower to IR** (outside the lock), through the (category, type) table in `lowering.cpp`:
   - width-generic ops; FA variants splat; `dot`/`constant`/`convert(bool/int)` vanish; `invert → Sub`; `inside → Mul`; `ifgreater` family → compare + `Select`; ramps and splits; blend modes.
   - **Unconnected inputs:** authored value, else nodedef default. Unconnected `defaultgeomprop` inputs become globals: UV0 → attribute `st` (`USDMTLX_PRIMARY_UV_NAME` override) with dual seeds; Pobject; Nworld/Nobject; Tworld/Bworld (Karma TB).
   - `texcoord.index` is ignored (Karma). `geomcolor` → `displayColor`, default (1,1,1). `geompropvalue.geomprop` → attribute name. Numeric `geompropvalueuniform` → constant attribute read; string/filename → warn and use the default (M4).
   - Intrinsic closures: each input becomes a `SurfaceField` binding; the nodedef version selects `StandardSurface` vs `StandardSurface100` defaults.
4. **Structure vs value:**
   - **Structure** (hashed): node ops, connections, terminals, uniform string/enum/int/bool immediates (`filtertype`, address modes, `space`, `geomprop`), attribute names, texture slot count and signatures, semantics mode.
   - **Value:** float, vector and color inputs (param slots, never folded), filenames (texture slots), texture color spaces (part of the texture request).
   - Color3/color4 constant params with a `colorSpace` other than scene_linear are converted on the host through OCIO.
5. **Unknown nodes** (`kma_*`, unknown ids): warn once per (material, nodeTypeId); outputs = nodedef output default, or zero of the consumer's type. Unknown terminal → next context.
6. **Ignored quietly:** extra inputs on known nodes (`aov:*`, Karma material properties, `karma_mikkt`, `houdini_visualize_signal`).
7. **Compile:** `core::compile(IrGraph)` → `Program`. **ProgramCache**: `structureHash → shared_future<shared_ptr<const Program>>`. On a hash match the canonical IR serialization is compared as well, against collisions.

### 4.4 Material sprim and IPR

```cpp
void HdNovaMaterial::Sync(HdSceneDelegate *sd, HdRenderParam *rp, HdDirtyBits *bits) {
    // Under Hydra 2 emulation every edit arrives as AllDirty (0x7c): diff it ourselves.
    auto &param = *static_cast<HdNovaRenderParam *>(rp); auto &reg = param.GetMaterials();
    if (_id == nova::kInvalidId) _id = reg.Acquire(GetId());            // stable; meshes may have acquired it
    HdNovaNetworkSource src;  const bool found = HdNovaReadMaterial(sd, GetId(), reg.Contexts(), &src);
    if (found && src.context == _context && src.net == _lastNet && src.meta == _lastMeta) { *bits = Clean; return; }
    HdNovaTranslated tr = HdNovaTranslate(src, reg.NodeDefs(), reg.Semantics(), GetId());   // pure, parallel-safe
    reg.Warn(GetId(), tr.diagnostics);
    std::vector<nova::TextureId> tex = param.GetTextureSystem().Acquire(tr.textures);  // converts/opens outside the lock
    if (_program && tr.structureHash == _structureHash) {                // value-only edit: no recompile
        if (tr.params != _params || tex != _textures)
            param.AcquireSceneForEdit().updateMaterialParams(_id, tr.params, tex);
    } else {
        auto prog = reg.Programs().GetOrCompile(tr.structureHash, tr.canonicalKey, tr.graph);
        param.AcquireSceneForEdit().updateMaterial(_id, {prog, tr.params, tex, reg.Attributes().Ids(tr.attributeNames)});
        _program = prog; _structureHash = tr.structureHash;
    }
    _params = std::move(tr.params); _textures = std::move(tex); _lastNet = std::move(src.net);
    _lastMeta = std::move(src.meta); _context = src.context; *bits = Clean;
}
void HdNovaMaterial::Finalize(HdRenderParam *rp) { /* registry.Orphan(path): the id stays, the Scene shows the default */ }
```

- Material edits never re-sync meshes; `Scene::commit` rebuilds the attribute remaps of affected (mesh, slot) pairs.
- **Mesh sync** handles `DirtyMaterialId` with `setMeshMaterials` and no BLAS rebuild.
  - GeomSubsets: slot 0 is the prim binding, then one slot per subset with a `materialId`; `faceSlot` per authored face.
  - A subset rebind arrives as `Points|Primvar|Topology|Normals`: compare the topology (`operator==` includes subsets) and the VtArray identity of points and normals to avoid a BLAS rebuild.
  - Primvar-only edits call `updateMeshAttributes` (no BLAS).
- **Subdivision:** `tessellateSubdiv` outputs `triangleFace` and refines every primvar (Bfr vertex/varying/faceVarying, seams intact).
- **Targets:** value edit < 5 ms engine-side (no recompile); structural edit < 2 ms compile for a `standard_surface` with 3 textures; plan target < 100 ms end to end.

### 4.5 Default material

**Default material:** Lambert(`displayColor` attribute, else 0.18), as today. This keeps every husk/cli test of unbound geometry valid. Karma's unbound look (0.401·C + 0.033) differs (see Q1).

### 4.6 Textures

- **Registry** (`hydra/textureSystem`): a `TextureRequest {resolvedPath (may contain <UDIM>/<UVTILE>), signature (color or non-color), colorSpace hint, layer}` is deduplicated by (path, chosen color space, mtime).
- **Color-space decision** (`hydra/colorSpace.cpp`, shared with light textures):
  1. float/vector signatures → raw (always);
  2. color3/color4 with an explicit `colorSpace:file` (USD 26.05 delivers it, Karma honors it; this includes `UsdUVTexture.sourceColorSpace`, which UsdImaging folds into it);
  3. OCIO file rules, skipping the default rule;
  4. `oiio:ColorSpace` metadata;
  5. 8-bit → sRGB, else linear (this replaces today's extension rule; needed for 8-bit `.tx` files from `imaketx`).
- **Policy per texture:**
  - `Direct`: linear, tiled, mipped.
  - `Matrix`: as Direct, but a linear gamut different from scene_linear; a 3×3 OCIO matrix applied after the lookup (linearity verified).
  - `Convert`: untiled, unmipped or non-linear → `ImageBufAlgo::make_texture` to a half-float, linear, 64² tiled, mipped zip EXR with `maketx:fixnan=box3`, `maketx:constant_color_detect=1`, `maketx:opaque_detect=1` and an explicitly set `oiio:ColorSpace`. The key is the SHA-1 of (canonical path, size, mtime, source color space, format version). Written atomically (temporary name + rename); each UDIM tile under one prefix.
  - `ConvertRat`: `imaketx src.rat cache.exr` as a subprocess.
  - `Missing`.
  - Conversions run in a TBB `task_group` outside the registry mutex.
  - Packaged `usdz[...]` assets are extracted through `ArResolver::OpenAsset` into the cache.
- **Lookup:** handle plus `get_perthread_info()` once per batch; one `TextureOpt` per batch; scalar `texture()` loop (OIIO's batched API is itself a loop). `anisotropic=8`, `conservative_filter`.
  - `filtertype`: closest/linear/cubic → InterpClosest/Bilinear/Bicubic.
  - Address modes are passed explicitly, because maketx files carry `wrapmodes black,black`.
  - UDIM: clamp inside a tile; u < 0, v < 0 or u ≥ 10 → `default` (OIIO's `int()` would read tile 1001).
  - `constant` mode: black under karma semantics (measured), `default` under spec; both need an explicit range test.
  - `framerange`/`frameoffset`/`frameendaction`: ignored with a warning (Karma ignores them too).
- **Errors:**
  - missing file → `default` plus one stderr line (`HD_NOVA_WARN`), counted per texture;
  - missing UDIM tile → `default`, silent (Karma);
  - empty `resolvedPath` for UDIM (USD resolves only when tile 1001 exists) → warning;
  - `nova:abortOnMissingTexture` mirrors `karma:global:abortmissingtexture`.
- **IPR:** on re-sync compare mtimes, `invalidate(path, false)` and reconvert if the source changed.
- **Stats:** with `nova:textureStats ≥ 1`, print OIIO stats, converted files, and files that never used a MIP level above 0.

### 4.7 Thread safety

- The NodeDef database is immutable after `call_once`.
- The MaterialX document work runs under one mutex.
- Program cache, texture registry and attribute registry are locked.
- Scene edits already take a mutex. All heavy work happens before `AcquireSceneForEdit()`.
- **Stress test** before enabling parallel material sync: 200 materials synced in parallel, 50 times; the resulting programs and param blocks must be byte-identical every time.

## 5. Ordered sub-steps

Each step builds, passes the full existing suite, and adds its own tests. Effort is in engineer-days.

### M3.1 Generic primvars, material slots, shading point (4 d, risk: medium)

- **Deliverables:**
  - `scene/attributes.h`; MeshDesc `attributes` and `triangleFace`; instance attributes (generalized `instanceColors`); `Scene::updateMeshAttributes`, `setMeshInstanceAttributes`; `resolveShadingPoint` (st, dPdu/dPdv, Karma TB, Pobject/Nobject).
  - Hydra mesh uploads every numeric primvar (float/int/bool to color4, all interpolations); instancer generic instance-rate primvars.
  - `subdiv.h` `SubdivPrimvar` (Bfr) plus base-face ids.
  - `hydra/material/registry` attribute-name part.
  - displayColor flows through the attribute path.
- **Tests:**
  - unit: interpolation per interp on known meshes; `triangleFace` from quads and n-gons; subdiv faceVarying `st` with seams (bilinear mesh gives exact affine st, Catmull-Clark continuous across faces); Karma TB on a plane with rotated `st` (exact); instance-attribute precedence.
  - All existing tests green, including `husk_instancing` colors and the 14 references.
- **Risk:** Bfr faceVarying tessellation of every primvar per motion step (cost: primvars are not motion-sampled).

### M3.2 Closure integrator, constant materials (4 d, risk: medium)

- **Deliverables:**
  - `scene/material.h` with `MaterialDesc.program == null` plus a constant-`SurfaceInputs` path (test hook `MaterialDesc::constantInputs`, removed in M3.6); `setMeshMaterials`; `materialOf`.
  - `bsdf/closure.h` with Lambert, Translucent and Transparent lobes and generic prepare/eval/sample.
  - Wavefront: path-state SoA, transmission-aware NEE and origin offsets, delta sentinel and `bsdfSampleWeight` fix, `etaScale` in Russian roulette, medium absorption hook, emission, alpha rules.
  - Default material through the closure path.
- **Tests:**
  - existing furnace, determinism, crop and reference tests stay green (Lambert unchanged in distribution);
  - emission-only sphere = (0.5, 0.25, 0.125) ± 1e-5;
  - translucent sheet: front + back irradiance split exact;
  - Transparent sphere over a Lambert plane in a uniform dome: plane = ρ ± 1 % (delta MIS chain);
  - `lightStrategy=light/bsdf` debug modes still agree with MIS in the furnace.

### M3.3 Lobe library, LUTs, statistical harness (8 d, risk: medium)

- **Deliverables:** the `bsdf/*` headers from 2.1, `tools/nova_bsdf_precompute`, committed tables, `tests/bsdf_stats.h` (validated chi² harness: open 8×8 Gauss–Legendre on 2×2 sub-cells, Wilson–Hilferty p-value).
- **Tests** (`test_bsdf_lobes.cpp`, `test_bsdf_tables.cpp`; one `TEST_CASE` per lobe):
  - **chi²:** 48×96 bins, 2¹⁹–2²⁰ samples, 3–5 ω_o, Šidák α = 0.01. Covers GGX conductor α 0.05/0.2/0.6/aniso (full and bounded), dielectric η 1.5 and 1/1.5 × α 0.05/0.3/0.6 including μ_o = 0.1, thin-walled T, EON r 0.5/1, QON, Zeltner, Charlie.
  - **Consistency:** pdf mass vs accepted fraction < 2e-3; `sample` == `eval`/`pdf` to 1e-4 relative; delta lobes have eval = pdf = 0.
  - **Reciprocity:** Lambert, QON, EON, GGX R (uncompensated), Charlie to 1e-5. BTDF: f(o→i)/f(i→o) = (n_o/n_i)², i.e. 0.4444 at η 1.5.
  - **Per-lobe white furnace** within 0.5 % for μ ∈ {0.05, 0.2, 0.5, 1} and r ∈ {0, 0.25, 0.5, 1}: compensated metal F = 1; dielectric RT for η ∈ {1.33, 1.5, 2, 1/1.5}; R + (1 − E_R^comp)·Lambert; EON ρ = 1; thin glass R′ + T′ = 1.
  - **Fresnel identities:** F(1) = ((η−1)/(η+1))²; TIR; conductor with k = 0 equals dielectric; F82(1/7) = tint·Schlick(1/7); F82 average vs quadrature; F ≥ 0; artistic IOR round trip.
  - **LUT spot values:** E(μ 1, α 1) = 0.307; E(0.5, 0.5) = 0.698; E(0.05, 0.5) = 0.913; E_RT(1.5, α 0.3, μ 1) = 0.989; E_RT(1/1.5, 1, 1) = 0.418; E_R(1.5, α 0.05, μ 1) = 0.0403; the `ggxE` rows r = 0.2/0.3/0.5/0.7/1.0 of surface-models §6.4 to ±0.002. 16 random cells recomputed at 2¹⁶ samples agree within 4σ. Monotonic in r for μ ≥ 0.2.
  - **Analytic albedos:** QON σ = 1 row (0.6418 … 0.8126); Zeltner r 0.5 row (0.0259 … 0.5249).
  - **NaN hunt:** 10⁶ random (params, ω_o, ω_i, u) including μ = ±1e-7, ω_o = n, N_s behind N_g, tangent ∥ normal, η 0 and 1, anisotropy 1. All outputs finite, pdf ≥ 0, |ω_i| = 1 ± 1e-4.
- **Risk:** volume of code; mitigated by the validated sketches in `m3research/microfacet/verify/`.

### M3.4 Surface models (7 d, risk: medium-high)

- **Deliverables:** `bsdf/surface.h` (`SurfaceInputs`, fields, `buildStandardSurface`/`StandardSurface100`/`OpenPBR`/`PreviewSurface`/`Unlit`, sanitizing, back-face rules, σa, selection, closure albedo); thin-walled variants; anisotropy frames; integrator uses `build*` for constant-input materials.
- **Tests** (`test_surface_models.cpp`, `test_material_furnace.cpp`):
  - **Layered white furnaces** to 1 ± 0.5 % for all μ_o and roughness values: every OpenPBR spec "White furnace testing" configuration and its Standard Surface equivalent (metal, diffuse, subsurface fallback, translucent with transmission_color 1, mixtures; each with a white coat and with fuzz/sheen; with thin film, anisotropy and normals).
  - **Exact anchors:**
    - glossy-diffuse ρ 0.5, r 0.3, η 1.5 = E_R + (1 − E_R)·0.5;
    - OpenPBR coat η 1.6, smooth, over ρ 0.5 with specular_weight 0 = 0.3089 / 0.3099 / 0.3175 / 0.3468 / 0.4340 / 0.6434 at μ 0.98 … 0.125;
    - SS specular_color (1,0,0) over white: G = 1 − E_R(μ);
    - SS diffuse 0.5 + specular r 0.3 = 0.5199 / 0.5207 / 0.5257 / 0.5434 / 0.5882 / 0.6552.
  - **Head-on μ = 1:**
    - SS specular η 1.5, r 0.3: 0.03971 (physical; Karma 0.041588, documented);
    - SS metal white r 0.8 and OpenPBR metal white r 0.8: 1.000 (Karma 0.98553 / 0.99527);
    - SS QON σ 1: 0.624060;
    - OpenPBR EON σ 1: 1.000;
    - OpenPBR fuzz r 0.5: 0.0249.
  - **Energy fuzz:** 512 random parameter sets × 4 ω_o, E ≤ 1 + 4σ.
  - **Mixture chi²:** 5 random SS and 5 random OpenPBR sets.
  - **Independent oracle:** `tests/reference/tools/gen_albedo_ref.py` (numpy: MaterialX G2, Fresnel, QON, conductor, the chosen compensation) writes `tests/data/albedo_ref.json`; Nova's render-level radial albedo probe agrees to 0.3 % abs.
  - **Render level** (orthographic sphere, uniform dome, maxDepth 64, disk r < 0.97):
    - IOR-1 glass over a Lambert plane: plane = ρ ± 1 %;
    - F = 1 mirror reflecting a rect light of L = 3: 3.0 ± 0.5 %;
    - rough glass r 0.3 furnace: 1 ± 0.3 % (Karma 0.87–0.997, documented);
    - emission parity SS vs OpenPBR;
    - SS and OpenPBR with coinciding semantics (no coat or sheen, specular_weight 1, white specular) render identically.
- **Risk:** subtle layering mistakes. Mitigated by two independent implementations and by the Karma E(μ) tables (surface-models §5.3) used as secondary anchors with explained deviations.

### M3.5 IR, compiler, VM, ops, noise (10 d, risk: medium)

- **Deliverables:**
  - `shading/{ir,ops,program,compiler,vm_lane,vm_batch}`, `mx_noise.h` + `mx_noise_simd.h` + `math/simd8.h`, `IrBuilder`.
  - Every Tier-1 op with karma and spec variants. Own polynomial `sin/cos/exp/log/pow`; ternary `min/max`; NaN-free ops.
  - Passes: fold (by running the real op on a 1-lane program), exact identities only (GLSL `mix = bg·(1−t) + fg·t`; never `mix(x,x,t) → x`), CSE, constant-`Select` collapse, DCE, uniform hoisting into the prologue, dual marking, bump-cone cloning, iterative Sethi–Ullman scheduling, first-fit rows (dst never aliases a src), structure hash (code, layout, constants, immediates; not param values).
- **Tests:**
  - **Compiler** (`test_shading_compiler.cpp`): `2·3 + P.x` → 2 instructions with the uniform 6; CSE merges two `sin(b)`; DCE count; row bounds (400-op chain ≤ globals + 3 rows; balanced depth-7 tree ≤ 9 rows); a parameter-only subgraph → 0 varying instructions; a value edit keeps the hash and an op edit changes it; compile < 1 ms for the expanded `standard_surface` + 3 images + place2d (benchmark assertion at 5 ms).
  - **Differential** (`test_shading_vm.cpp`): 3000 seeded random typed DAGs (5–400 nodes, 45 lanes), bit-exact (NaN == NaN) across a tree-walking evaluator of the unoptimized IR, `runLane`, `runBatch<8/16/32>` with partial chunks, and every optimization toggled off. Dual variants vs finite differences. The seed is logged and failures are shrunk.
  - **Semantics goldens:**
    - `tests/data/mtlx_golden/vex_ops.json` from `vexexec -A 32` including `<mxstd.h>` on inputs k/128 + 1/512 (generator `tests/reference/tools/gen_vex_golden.py`, offline, no license);
    - `mtlx_golden/*.exr` from MaterialX `GlslRenderer.renderTextureSpace` at 64² (`tools/gen_mtlx_golden.py`, offline).
    - Tolerances: exact for cell noise and Worley; ≤ 2 ulp for Perlin/fBm and transcendentals; 2e-5 abs + 1e-4 rel vs the GLSL EXRs.
    - Explicit cases: x/0 = 0, sqrt(−x) = 0, log(≤0) = 0, `pow(−2, 0.5) = −2`, `rint(−0.5) = 0`, `clamp(.5, 1, 0) = 1`, `normalize(0) = 0`, smoothstep low == high → 0, ramptb/splittb orientation, ramplr without clamp, dodge `1 − bg/(1−fg)` (karma) vs spec, rotate2d (1,0)@90° = (0,−1), rotate3d.
    - Noise probe points: Pobject (−0.96875, 0.96875, 0) → 0.061339.
- **Risk:** MSVC vectorization traps (check `/Qvec-report:2` for `vm_batch.cpp` in the build log); the long tail of op variants.

### M3.6 VM in the Shade stage, texture interface, ray cones (6 d, risk: medium)

- **Deliverables:**
  - `integrator/shade.{h,cpp}` (sort, chunks, globals incl. dual and shifted rows, VM dispatch, template/patch gather, varyingFree fast path).
  - Runtime ops `Global`, `Primvar` (with gradient), `TransformSpace` (per-lane instance transform; object = prototype space), `Tex2D` (through `ITextureSampler`), `NormalMap`, `HeightToNormal`.
  - `core/texture/{texture.h, ray_cone.h, memory_sampler}`; cone state; `kTexture` stream; `MaterialDesc` gets program, params, textures and attributes; the constant-input hook is removed; first-hit AOV shading refactor; frame and fps globals.
- **Tests:**
  - a constant-param SS/OpenPBR material through the VM is bit-identical to the M3.4 direct build;
  - `displayColorLambert` built with `IrBuilder` (standard_surface specular 0 + `geompropvalue displayColor`) equals the default material;
  - 16-material scene: tiles vs full frame and crops are bit-identical;
  - **ray cones:** pinhole spread = atan(2 tan(fovY/2)/H) vs finite-difference rays; orthographic width = pixel size and spread 0; major axis W/cos θ and minor axis W at 0/60/85°; `uvGradients` invariant to vertex order, ∇u·e = Δu; sphere curvature ≈ 1/R;
  - **MemorySampler:** the five wrap modes on a 4×1 ramp at s = −0.1/1.1/1.6 (clamp 1,4,4; periodic 4,1,3; mirror 1,4,2; black 0; constant default); MIP mean conservation;
  - flat normal map bit-identical to none; constant tilted normal map gives cos(L, N′)·E/π under a distant light;
  - `heighttonormal` of h = a·u on a plane gives the analytic tilt; bump of noise is finite and continuous.

### M3.7 Hydra materials: sprim, MaterialX translator, registry, IPR (10 d, risk: high)

- **Deliverables:** `hydra/material/*` (4.2–4.4), delegate changes (4.1), mesh slots, subsets and `DirtyMaterialId`, `Nova::MaterialX` targets, `TF_DEBUG HDNOVA_MATERIAL` (dumps the network in the probe's format, the chosen context, and optionally the hdMtlx document for debugging), parallel material sync after the stress test.
- **Tests:**
  - **Translator unit tests** (`test_hydra_materials.cpp`, fixtures copied from the probe logs `out_husk_kma.txt` and `out_si_read.txt`): pseudo-params stripped; relative paths; empty terminal output name fixed; SS 1.0.1 defaults base 1.0 / base_color 0.8 and 1.0.0 defaults 0.8 / 1.0; `ND_normalmap` rename; USD aliases; attribute extraction (`st` from texcoord and from an unconnected image texcoord, `mask` from geompropvalue, `uv` from a primvar reader's `varname`); unknown `kma_rayimport` → warning + default; unknown terminal → next context; every stdlib nodegraph nodedef expands and lowers with no unknown op; hash identical under node renames and shared between two structurally equal materials; a new connection, a `geomprop` change or an enum change gives a new hash.
  - **Hydra IPR tests** (port of `m3research/hydra-materials/probe/driver.cpp`):
    - a value edit hits the cache with no compile and no mesh sync;
    - a `geomprop` rename rebinds the attribute without a mesh sync, and the image changes;
    - an edit on a disconnected node leaves the edit version unchanged;
    - a rebind gives `MaterialId` only, and a subset rebind does not rebuild the BLAS (debug counter);
    - removing a material shows the default material;
    - value-edit latency < 5 ms.
  - **husk** (`tests/husk/check_materials.py`; white dome 1, direct light only, top-down camera, 256 spp):
    - context priority `kma` > `mtlx` > universal = exactly (1,0,0);
    - Lambert calibration exactly C;
    - all-defaults SS 0.8089;
    - UsdPreviewSurface (0.5139, 0.2733, 0.1530) and 0.2062;
    - a GeomSubset two-material mesh;
    - Material interface promotion;
    - preview vs full binding purpose.
  - **Regression gate:** the 14 M2 reference scenes, which bind `/Looks/displayColorLambert` through `mtlx`, pass unchanged now that they go through the full translator.

### M3.8 Textures: OIIO TS, conversion, color, UDIM (6 d, risk: medium)

- **Deliverables:** `hydra/{textureSystem,textureConvert,colorSpace}`, the 4.6 policy, settings, stats, `.rat`, usdz; light textures switched to `colorSpace.cpp`.
- **Tests** (`hydra` label, OIIO linked):
  - MIP texture with constant L per level, orthographic plane at 16/64/4 texels per pixel → level 2.00 / 4.0 / ≈0 with `textureFilterWidth 0.25` (Karma's measured values);
  - wrap table as in M3.6, through OIIO;
  - UDIM: 1001 red, 1002 green, 1011 white at v 1.75 / blue at v 1.25, 1012 missing → default, u < 0 → default; `<UVTILE>` too;
  - constant sRGB PNG, untiled and as a uint8 tx: code 10 → 0.003035 ± 1e-5, code 128 → 0.215861 ± 1e-4;
  - 50 % checker at a coarse footprint → 0.5 (not 0.214);
  - ACEScg EXR → M·c with the measured matrix;
  - gray → RGB replicated; PNG RGBA (1,1,1,0.5) → 0.502;
  - missing file → default plus exactly one stderr line, and `abortOnMissingTexture` aborts;
  - `.rat` → 0.2158;
  - conversion cache reused on the second run (no `make_texture` call).
  - **`husk_textures`** extended to `mtlximage` on surfaces (png, jpg, uint8 tx, half EXR, `*ACEScg*` names, raw roughness map, explicit `colorSpace` metadata with a **saturated** color).
  - **fxhoudini session:** confirm how Houdini's `mtlximage` `filecolorspace` parameter is encoded in USD (unverified so far).

### M3.9 Opacity cut-outs and completion of geometry/medium features (4 d, risk: medium-high)

- **Deliverables:** `IAccel` additions (`RayBatch::opacityRnd`, `IOpacityEvaluator`, `setMeshOpacity`), Embree intersect and occluded filters through an extended `RTCRayQueryContext` (check that Embree's filter-function feature is on in the vcpkg build), opacity programs, UsdPreviewSurface `opacityThreshold`.
- **Tests:**
  - opacity 0.5 white sphere on black → color 0.75, alpha 0.75 (render level, and Karma in `mtl_diffuse`);
  - plane with opacity 0.5 between a distant light and a receiver → irradiance 0.5;
  - alpha-mapped leaf quad vs analytic coverage;
  - repeated filter calls give identical decisions (unit test calling the filter twice);
  - opaque scenes have no filter installed (rays/s unchanged within noise on `cornell_rect`).

### M3.10 Karma node probes, end to end (4 d, risk: low-medium)

- **Deliverables:** `tests/reference/tools/gen_node_probes.py` writes `scenes/mtl_nodes_patterns.usda` (about 40 unlit quads) and `scenes/mtl_nodes_images.usda` (about 20) plus assets. Settings: `ND_surface_unlit.emission_color` = node under test, orthographic camera, `nova:pixelFilter = point` / `karma:global:pixelfilter = point`, float32 output. Karma references; manifest entries with `abs_tol` 2e-5 + rel 1e-4 (the images are deterministic).
- **Coverage:**
  - texcoord; position/normal/tangent in object and world space (incl. a PointInstancer prototype and a native instance);
  - noise2d/3d (all output widths), fractal, cellnoise, worley (distance, solid);
  - ramps and splits; checkerboard, tiledimage, place2d (graphs);
  - math edge cases (divide by 0, sqrt(−), pow(−)); smoothstep low ≥ high; compositing blends incl. dodge; HSV;
  - image orientation and all address modes; UDIM 1001/1002/1011 and a missing tile; color-space metadata with a saturated color; float/vector raw signature;
  - geompropvalue with an absent primvar → default; geomcolor.
- **Spec semantics:** a Nova-only `mtl_nodes_spec` variant (`nova:mtlxSemantics = spec`) is compared against the MaterialX GLSL goldens.

### M3.11 Karma-referenced material scenes (5 d, risk: medium)

- **Deliverables:**
  - New files only, so the M2 reference hashes stay valid: `scenes/common_mtl.usda` (`_class_MtlRefSettings` exactly as in test-strategy §2: limits 64, `causticsenable 1`, `constrainmaxrough 0`, `thinwallatrefractlimit 0`, `internalreflectenable 1`, `causticsroughnessclamp 0`, color limits 1e9, uniform pixel oracle) and `scenes/assets/sphere_st.usda` (160×80, vertex normals, `st`).
  - A `material()` helper in `gen_scenes.py`.
  - `nova_imgcmp --region` with the region-mean test and per-pixel tolerance maps; manifest `compare.regions: [{name, circle:[cx,cy,r], rel_tol, reason}]`.
  - `tests/reference/tools/karma_albedo_probe.py` (re-measures the Karma deviation table when the Karma version changes).
  - The scenes of section 6.3.
  - The Karma references can be generated as soon as M3.4's parameter semantics are frozen, because they need only Karma.

### M3.12 Performance, IPR latency, documentation, Houdini session (3 d, risk: low)

- **Benchmarks** (label `bench`, not gating): ns per (instruction · lane) per op class at N = 8/16/32/64; noise kernels; shade ns per hit for (a) a uniform-only SS, (b) SS with 3 textures, place2d and a normal map, (c) fractal3d with 4 octaves + 30 math ops; sort cost; compile time; IPR edit latency.
- **Targets:** (a) < 60 ns per hit of VM + closure overhead over M2 Lambert; (b) < 1.5 µs per hit; rays/s on `cornell_rect` ≥ 85 % of M2.
- **README:** "Current scope (M3)", settings table, color-space and texture rules, known Karma differences.
- **fxhoudini session:** a LOP scene with a Karma Material Builder (`kma`) and a MaterialX Builder (`mtlx`) material, Nova viewport vs Karma screenshots; a value edit in IPR shows no recompile in the `HDNOVA_MATERIAL` log.

**Total: about 71 engineer-days.** Critical path: M3.1 → M3.2 → M3.3 → M3.4 → M3.5 → M3.6 → M3.7 → M3.8 → M3.11. M3.10/M3.11 scene and reference authoring can run in parallel from M3.4 on.

## 6. Test strategy

### 6.1 Principle

Exactness comes from analytic tests, furnaces and statistics. **Karma comparisons verify translation and integration**, never BSDF exactness. Every Karma difference is either avoided by scene design, designed around, or carries a per-region tolerance with a written reason.

### 6.2 Layers and budgets

| Layer | Files | Content | Wall time (-j4) |
|---|---|---|---|
| Lobe statistics | `tests/bsdf_stats.h`, `test_bsdf_lobes.cpp`, `test_bsdf_tables.cpp` | chi², pdf mass, consistency, reciprocity, furnaces, LUT spot checks, NaN hunt | ~3 s (8 s CPU) |
| Surface models | `test_surface_models.cpp`, `test_material_furnace.cpp`, `tests/data/albedo_ref.json` | layered furnaces, anchors, energy fuzz, render-level analytic scenes | ~2 s |
| VM | `test_shading_compiler.cpp`, `test_shading_vm.cpp`, `test_mtlx_ops.cpp`, `tests/data/mtlx_golden/` | differential tests, goldens, noise | ~1 s |
| Core textures | `test_ray_cone.cpp`, `test_texture_memory.cpp`, `test_primvars.cpp` | cones, wrap, MIP, interpolation | ~0.5 s |
| Hydra | `test_hydra_materials.cpp`, `test_textures_oiio.cpp` | translator, IPR driver, OIIO behaviour | ~3 s |
| husk / cli checks | `check_materials.py`, extended `check_textures.py` | calibrations, contexts, missing-texture messages | ~8 s |
| Reference scenes | 8 new scenes × (husk + cli) | 6.3 | ~7 s (22 s serial) |

The suite grows by about 25 s wall at -j4.

Offline cost:
- Karma references: 6 lit scenes × 2 halves × ~35 s plus the node probes ≈ 8 min;
- goldens < 1 min;
- LUT generation ≈ 10 min.

### 6.3 New reference scenes

192×96, 18 floating spheres from `sphere_st.usda`, `outputs:mtlx:surface` only. Test 64 spp; reference `spp_half` 4096. No diffuse receivers of specular light, constant domes plus rect lights, no textured domes.

| Scene | Content | Compare |
|---|---|---|
| `furnace_materials` | Lambert white, OpenPBR EON r1, plastic r0.2/0.6 (bright base), SS coat r0/0.3, OpenPBR coat r0, sheen over white, fuzz over white, smooth glass, IOR-1 glass, thin smooth glass, translucent, OpenPBR metal F = 1 r0, white plastic r0.4; dome L = 1 visible | constant 1, `karma_check`, block 0, global 5e-4 |
| `mtl_diffuse` | Lambert colors, EON r0.5/1, opacity 0.4 cut-out, emission sphere, UDIM base_color, all-defaults SS sphere | strict 0.01 |
| `mtl_metal` | SS metal r 0/0.2/0.4/0.6 (silver, copper, gold), OpenPBR F82 r0.3, anisotropy 0.8 with `st` and with swapped `st`, smooth thin-film metal | 0.02 (Karma vs exact ≤ 2.1 % for r ≤ 0.4); thin-film region 0.30, reason: Karma's sRGB cast |
| `mtl_glass` | smooth glass η 1.5/1.33, tinted transmission, transmission_depth absorption, smooth thin-walled, IOR 1, r0.15 glass, in front of an emissive checker backdrop | 0.02 |
| `mtl_layered` | plastic r0.2/0.5, SS coat, SS sheen, OpenPBR fuzz, smooth thin-film dielectric, SS QON r1 | 0.03; regions: sheen 0.10 (Karma uses a Lambert shape), QON 0.06 (Karma CPU uses VEX `diffuse`), plastic highlights 0.10 (Karma uses Schlick-fit Fresnel) |
| `mtl_textured` | tilted plane + spheres: UDIM base_color, raw roughness map, `ND_normalmap` from `st`, checker `.tx` minified at grazing angles, tiledimage | strict 0.01 (block means do not depend on filter width) |
| `mtl_nodes_patterns`, `mtl_nodes_images` | M3.10 unlit probes | abs 2e-5 + rel 1e-4 |
| `mtl_kma_context` | one sphere with `outputs:kma:surface` only (Karma Material Builder form) | strict 0.01 (or as part of `check_materials.py`) |

### 6.4 Tolerance tiers and tooling

- **Tiers:**
  - analytic: block 0, global ≤ 5e-4;
  - agrees with Karma: ≤ 0.02, justified by the measured Karma-vs-exact tables;
  - tolerant: per region, ≤ 1.5× the measured deviation, capped at 0.10, with a mandatory `reason`. Each tolerant scene still keeps strict tests on its unflagged regions.
- **`nova_imgcmp`** gains region tests: per-pixel tolerance map, region means with confidence intervals and Šidák correction over regions × channels, JSON diagnostics such as "sheen +7.3 %".
- **Generators and probes** (offline, outputs committed): `gen_albedo_ref.py`, `gen_vex_golden.py`, `gen_mtlx_golden.py`, `gen_node_probes.py`, `karma_albedo_probe.py`.
- **Version drift:** on a Houdini or Karma version change (recorded in `images/<scene>.json`), re-run `karma_albedo_probe.py` before regenerating references.
- **Licensing:** `mxpbr.h`, `ggx_utils.h` and `mxstd.h` are SideFX proprietary. They serve as behaviour documentation only; their code is never copied. Karma-compatible kernels are written from black-box measurements and MaterialX (Apache-2.0) sources.

## 7. Risks and open questions

### 7.1 Risks

| Risk | Mitigation |
|---|---|
| Thread safety of the MaterialX document cache under parallel material sync | One global mutex around MaterialX document work (≈0.2 ms per material); stress test before enabling `IsParallelSyncEnabled`. |
| Users perceive the BSDF deviations from Karma (Schlick dielectric up to −21 % of the specular part at μ 0.5, Lambert-shaped sheen, rough glass energy, thin-film cast) as "wrong" | Measured and documented, tolerance regions; Q2 decides whether a compat mode is built. |
| Embree filter-callback cost and duplicate calls | Filters only on geometry with opacity materials; deterministic hash decisions; a unit test calls the filter twice. |
| Small material groups at deep bounces, where the VM gains less from batching | Adaptive batch<8/16> and per-lane execution for n ≤ 2; 64×64 tiles possible later. |
| First IPR load blocked by texture conversion (≈0.45 s per 4k PNG) | Parallel conversion, persistent disk cache, existing `.tx`/`.exr` next to the source reused when newer. |
| Memory from uploading every primvar | Accepted for M3; filtering in M7 via `HdNovaResyncSceneIndex` with per-path primvar locators. |
| LUTs generated from the same code they compensate (circular) | Independent numpy oracle, Monte Carlo spot values from the research reports, monotonicity checks. |
| Structure-hash collisions | The canonical IR serialization is compared on every cache hit. |
| `<UDIM>` resolved by USD only when tile 1001 exists | Warning; documented. |
| Bfr face-varying refinement cost for subdivided `st` | Measured in M3.1; same budget logic as `nova:subdivMaxTriangles`. |
| Source of `frame`/`fps` for the MaterialX `frame`/`time` nodes is unclear under husk (Karma reads `renderer:frame`) | Probed in M3.7; until confirmed, the stage's current time code and `timeCodesPerSecond`. |

### 7.2 Open questions: user decides (USER)

- **Q1 (USER):** Default material for unbound geometry.
  - Recommended: keep Lambert(displayColor, else 0.18). It keeps all husk/cli tests of unbound scenes valid.
  - Alternative: imitate Karma's undocumented fallback, 0.401·C + 0.033.
- **Q2 (USER):** Should M3 add an optional `nova:shadingCompat = "karma"` closure mode: sheen as albedo-weighted Lambert, dielectric reflection with Schlick and the MaterialX (Apache) albedo fit, thin film with an sRGB matrix?
  - Recommended: no. Decide after the first tolerant-tier results in M3.11. An exact copy of Karma's fits is not possible without proprietary code.
- **Q3 (USER):** Karma renders "fake caustics" by default (transparent tinted shadows; `causticsenable = 0`). Nova renders physically (black shadow plus a real, noisy caustic).
  - Recommended: keep M3 physical and add an optional transparent-shadow mode in M7.
- **Q4 (USER):** Displacement is not scheduled in PLAN.md.
  - Recommended: M4, run on the host over the tessellated mesh with the same VM.
  - M3 ignores displacement terminals with a warning.
- **Q5 (USER):** Default of `nova:mtlxSemantics`.
  - Recommended: `karma`, because the plan names Karma as the material reference and users see Karma's ramps and safe math.
  - Alternative: `spec`, MaterialX 1.39.5 as shipped.
- **Q6 (USER):** Texture auto-conversion on by default, writing half-float EXRs to `$HOUDINI_TEMP_DIR/nova_tx`.
  - Recommended: on. Karma converts too, writing `.rat` files next to the source.
- **Q7 (USER):** Accept the scope addition D5 (UsdPreviewSurface and the universal context in M3)?
  - Recommended: yes.
- **Q8 (USER):** Dispersion and spectral rendering have no milestone in PLAN.md.
  - Recommended: schedule them together with M8 (volumes) or after M9.

### 7.3 Open technical questions (resolved inside M3)

- USD encoding of `mtlximage` `filecolorspace` (fxhoudini check in M3.8).
- Whether Karma converts color-space metadata on non-file color inputs (one unlit probe tile in M3.10).
- Whether Embree's filter-function feature is enabled in the vcpkg build (checked at the start of M3.9).
- Frame/fps source under husk (M3.7 probe).

The research reports are in `C:/Users/Synti/AppData/Local/Temp/claude/C--Users-Synti-Desktop-renderEngine/ac6add8e-dc52-40d3-838a-501e34988043/scratchpad/m3reports/`, the verified prototypes and harnesses in `.../scratchpad/m3research/{shader-vm/proto, microfacet/verify, test-strategy/chi2, mtlx-nodes/cpp, hydra-materials/probe}`. Main files changed in the repo: `core/integrator/wavefront.cpp`, `core/scene/{mesh.h,scene.h,scene.cpp}`, `core/math/sobol.h`, `hydra/{renderDelegate.cpp,mesh.cpp,textureCache.cpp}`, `tests/CMakeLists.txt`, `tests/reference/manifest.json`.