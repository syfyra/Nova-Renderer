# Nova M3 implementation plan: shading core

ShaderGraph IR, bytecode VM, Standard Surface and OpenPBR, OIIO textures

Inputs: `docs/PLAN.md`, `README.md`, the seven M3 research reports (`scratchpad/m3reports/*.md`), and the current code (`core/integrator/wavefront.*`, `core/scene/*`, `core/math/sobol.h`, `hydra/mesh.cpp`, `hydra/renderDelegate.cpp`, `hydra/textureCache.cpp`, `tests/CMakeLists.txt`).
Milestone goal (PLAN.md): "MaterialX-Standard-Surface aus Solaris rendert korrekt".
Priorities: fast on the CPU wavefront today, and the same bytecode and closure code on CUDA in M10.

---

## 0. Decisions at a glance

| Topic | Decision |
|---|---|
| Shading representation | Typed SSA IR, compiled to a flat 16-byte-instruction bytecode, with a per-material uniform block. Parameter values never fold, so value edits never recompile. |
| Executors | One op semantics (`core/shading/ops.h`, `NOVA_HOST_DEVICE`) and two executors: the **SoA batch executor** (CPU shade stage, 32-lane chunks) and the **per-lane executor** (CUDA in M10, Embree opacity callbacks, host prologue, test oracle). Both are bit-identical. |
| Closures | No runtime closure instructions. Standard Surface, OpenPBR, UsdPreviewSurface and surface_unlit are **intrinsic uber models**. VM outputs bind to fields of a POD `SurfaceParams`, which `buildSurfaceClosure()` turns into a fixed list of at most 8 lobes. |
| Batching | Per bounce, hits are radix-sorted by material id. Each material group runs in 32-lane chunks (`runBatch<32/16/8>`, per-lane executor for ≤2 hits). Materials with no varying instructions skip the VM entirely. |
| BSDF | Anisotropic GGX (spherical-cap VNDF, bounded cap for reflection-only lobes), exact Fresnel, F82-tint, Gulbrandsen n/k, Turquin × Kulla-Conty energy compensation from LUTs (~414 KB), EON and QON diffuse, MaterialX-form Charlie sheen, Zeltner fuzz, Airy thin film with a grey DC term. |
| Pattern semantics | Karma semantics (VEX safe math, Karma ramp, split, dodge, address-mode, normalmap and heighttonormal behaviour), because PLAN.md names Karma CPU as the material reference. |
| Hydra | Material sprim; render contexts `{nova, kma, mtlx}`, then universal. The network is read from the terminal scene index. MaterialX 1.39.5 from `$HFS` is expanded per material with `setDataLibrary` and `flattenSubgraphs`. Programs are cached by structure hash. |
| Textures | A private OIIO TextureSystem on the shared ImageCache (`flip_t=1`, `gray_to_rgb=1`, `stochastic=3`). Untiled or non-linear sources are auto-converted to tiled, MIP-mapped, scene-linear half EXR. Footprints come from **ray cones** (2 floats per path), with derivative scale 0.25 for Karma parity. |
| Derivatives | **Shifted re-evaluation**: the compiler clones the varying cone of every texture uv or bump-height input twice, with globals shifted by the footprint vectors. No dual variant of any opcode is needed. |
| Opacity | Cutout and fractional opacity are evaluated stochastically by an opacity-only program in Embree filter callbacks, for both closest-hit and occlusion queries. In M10 this becomes an OptiX any-hit program. |
| GPU guard | A new compile-only `nvcc` target checks all kernel-side headers (`core/bsdf`, `core/shading/{program,ops,vm_lane,noise}.h`, `core/texture`) from M3.1 on. |

---

## 1. Scope

### 1.1 What M3 delivers

1. **Shading kernel in core.** IR, optimizer (fold, CSE, DCE, uniform hoisting), Sethi-Ullman scheduling, row allocation, bytecode, batch and lane executors, about 65 opcodes covering the MaterialX 1.39.5 stdlib primitives that remain after nodegraph expansion. Noise is bit-compatible with MaterialX and Karma.
2. **Surface models**, using the shared lobe library:
   - Standard Surface 1.0.1: all parameters, except dispersion and `transmission_scatter`; subsurface uses a fallback lobe (§1.3).
   - OpenPBR 1.1.1: all parameters, with the same exceptions.
   - UsdPreviewSurface and surface_unlit.
   - Energy-compensation LUTs.
3. **Integrator.**
   - Closure-based NEE and BSDF sampling, with transmission, delta lobes and MIS sentinels.
   - Interior absorption (`transmission_depth`), emission, stochastic opacity, and the light `specular` multiplier.
   - Per-path ray cones and path flags (last event type, inside-medium).
   - The first-hit albedo and normal AOVs now come from the closure.
4. **Hydra.**
   - `HdNovaMaterial`, contexts `{nova, kma, mtlx}` plus universal, and MaterialX expansion from `$HFS`.
   - IPR diffing: value edits are no-recompile.
   - Per-face materials (GeomSubsets) and generic primvar upload (incl. subdivision surfaces and instance-rate primvars).
   - The default material for unbound geometry.
   - nova_cli parity.
5. **Textures.** OIIO TextureSystem, UDIM (Nova-defined semantics = Karma's), tx and MIP handling, auto-conversion (incl. `.rat` via `imaketx`), color spaces (explicit metadata, OCIO file rules, 8-bit rule, matrices), wrap modes, missing-texture policy and statistics.
6. **Normal maps, bump, tangents and anisotropy**, with Karma conventions.
7. **Tests.** Lobe statistics (chi², pdf mass, consistency, reciprocity, energy and NaN fuzzing), material furnaces, VM differential tests and golden values, texture tests, a Hydra IPR driver test, a husk material check, and 7 new reference scenes (1 analytic, 6 Karma-referenced).
8. **Tooling.** `tools/nova_bsdf_precompute` (LUT generator), `nova_imgcmp --region`, generators for the reference data, and the CUDA compile check.

### 1.2 Deferred work (consistent with PLAN.md)

| Item | Milestone | Note |
|---|---|---|
| Generic pbrlib BSDF graphs (user-wired `*_bsdf` with layer/mix/add/multiply) | M4 | Flattened at compile time into a static lobe list (`SurfaceModel::LobeList`), using the same lobe library. The integrator does not change. |
| `kma_*` nodes, Karma/Houdini VOP mapping tables, Nova Material Builder HDAs, `UsdRenderers.json` UI | M4 | In M3, unknown `kma_*` nodes produce a warning and nodedef-default outputs. |
| MikkTSpace tangents (`karma_mikkt`, `tangentu`/`signs` primvars) | M4 | M3 computes Karma's st-derived frame at hit time. |
| VEX networks (`vex` context, CVEX bridge) | M5 | `vex` stays out of the context list until then, otherwise VEX materials would lose their universal preview. |
| LPE AOVs, `aov:*` inputs on shaders | M6 | M3 already stores the lobe event type (D/G/S × R/T) in path flags. |
| Adaptive sampling, OptiX denoiser, primvar filtering, IPR delta-update work | M7 | |
| `transmission_scatter` (+anisotropy), interior scattering medium | M8 | M3 is absorption-only, like Karma CPU and XPU. |
| Random-walk subsurface, light BVH, NEE of emissive surfaces, light linking | M9 | |
| CUDA executors and OptiX accel | M10 | Same bytecode, same `vm_lane.h`, same closure code. |

### 1.3 Explicit deviations from PLAN.md

| # | Deviation | Reason |
|---|---|---|
| D1 | **Dispersion** (SS `transmission_dispersion`, OpenPBR `transmission_dispersion_*`) is not rendered in M3. It produces one warning. | It needs spectral transport. PLAN.md defers spectral ("RGB, später spektral") and assigns it to no milestone; the `kWavelength` stream is reserved. "Komplett" excludes it. |
| D2 | **`transmission_scatter`** is ignored (warning). | It needs the M8 volume integrator. Karma CPU and XPU ignore it too, so Solaris parity holds. |
| D3 | **Subsurface** renders as an albedo-`C` diffuse lobe (thin-walled: the spec's R/T diffuse pair). | Random walk is M9 in PLAN.md. The fallback is the OpenPBR spec's own recommendation and keeps energy exact. |
| D4 | **UsdPreviewSurface is native in M3**, pulled forward from M4. | It is Houdini's auto-generated universal material and the context fallback. Its MaterialX implementation is a pbrlib graph (which is M4); a native mapping is about 150 LOC on existing lobes. |
| D5 | **Displacement** is not in M3. It is proposed for **M4**. | PLAN.md assigns it to no milestone. It needs host-side tessellation plus the batch VM over vertices; the IR already has a `displacement` output slot. |
| D6 | The **`kma` context is active from M3**, although its mapping tables are M4. | Karma Material Builder (the default builder) authors `outputs:kma:*` with pure MaterialX inside. Without `kma`, Nova shows only the preview or nothing. |
| D7 | **Emissive surfaces are BSDF-sampled only** (MIS weight 1). | Light-sampling them needs the light BVH (M9). |
| D8 | **No per-ray-type depth limits** (Karma diffuse/reflect/refract) in M3. Proposed for M4 with the Render Settings LOP. | It needs a UI. `nova:maxDepth` stays the only limit, so material reference scenes use limits of 64 on both sides. |
| D9 | **Light `specular` multiplier** (UsdLux `inputs:specular`) is added to core and Hydra. | M2 only applied `diffuse`, because every lobe was diffuse. |
| D10 | **No Karma-compat lobe variants** (Lambert-shaped SS sheen, Schlick-fit dielectric spec, sRGB-cast thin film, uncompensated rough glass). | Nova follows the MaterialX/OpenPBR definitions. The differences are measured and get explicit, reasoned tolerances in the reference tests (§6). See open question Q2. |
| D11 | A **compile-only CUDA check** target is added in M3. | It is tooling, not a GPU feature. It enforces PLAN.md's GPU-portability rule continuously, and CUDA 12.9 is already found in M0. |

---

## 2. Architecture

### 2.1 Data flow

```
Solaris / husk / nova_cli
  └─ UsdImaging scene index ──► HdNovaMaterial::Sync (hydra/material/)
        read HdMaterialSchema from the terminal scene index; context chain nova → kma → mtlx → ""
        → mx::Document over the shared MaterialX library (setDataLibrary), flattenSubgraphs(filter keeps intrinsics)
        → lowering table → nova::shading::IrGraph + param values + TextureRequests + primvar names
        → structureHash ─► ProgramCache (hit: reuse) ─► core compile() → shading::Program
        → Scene::updateMaterial(id, MaterialDesc{program, params, textures})
  └─ HdNovaMesh::Sync → Scene::updateMesh (geometry + primvar channels), Scene::setMeshMaterials (slots, per-triangle slot)

Scene::commit()  → MaterialTable (flat, device-uploadable): programs, instruction pool, uniform pool,
                   SurfaceParams templates, texture-id pool, mesh bindings, primvar remaps; opacity filter flags → accel

renderWave (per tile = 1024 paths, per bounce):
  Intersect (Embree; opacity filter for flagged meshes)
  Shade  A: misses / escaped light / area lights along ray / camera-visible lights   (unchanged logic)
         B: radix-sort surviving hits by material id
         C: per material group, per 32-lane chunk:
              resolveHit + ray-cone footprint → global rows → VM (skipped if varying-free)
              per lane: gather SurfaceParams → buildSurfaceClosure → emission → NEE (evalClosure) → sampleClosure → RR
  Shadow (Embree occluded; opacity filter)
```

### 2.2 File layout

**New in core** (no HDK, USD, OIIO or MaterialX):

```
core/bsdf/
  common.h          sqr, saturate, lerp (float/Spectrum), pow5, pow6, reflect, refract, safeSqrt
  microfacet.h      GgxDistribution (D, t, G1, vis, VNDF sample (full / bounded cap), pdfs), alphaSS(), alphaOpenPBR(), isDelta()
  fresnel.h         fresnelDielectric, fresnelConductor, polarized variants + phases, fresnelF82Tint(+Avg), artisticIor,
                    openPbrModulatedEta, iorToF0/f0ToIor, conductorFavg
  thin_film.h       fresnelAiry(...) (Belcour-Barla, 2 iterations, working-space XYZ→RGB, DC normalised grey), airyAlbedoApprox
  diffuse.h         Lambert, translucent, QON (+ albedo fit), EON (+ CLTC/uniform sampling, analytic albedo)
  sheen.h           Charlie (MaterialX form: no G, Neubelt denominator), Zeltner LTC (fits, sample, pdf)
  dielectric.h      rough R / T / R+T (Walter, VNDF), smooth delta R/T, thin-walled R'/T' (Cycles sheet Fresnel, mirrored lobe)
  conductor.h       microfacet reflection with FresnelMode {ConductorNK, F82Tint, Schlick}
  shading_frame.h   ensureValidSpecularReflection (Cycles), karmaTangentFrame (st-derived T/B), rotateTangent
  energy.h          BsdfTables view, lut2/lut3, msCompensation, dielectric E_R/E_RT lookups, schlickAlbedo (ABC)
  tables/bsdf_tables.inc   generated (hex floats + generator hash)
  closure.h         Lobe, ClosureList, ClosureEval, ClosureSample, evalClosure(), sampleClosure(), prepare (picks)
  surface.h         SurfaceModel, SurfaceParams (canonical POD), ShadingGeometry
  surface_models.h  buildSurfaceClosure() → buildStandardSurface / buildOpenPBR / buildUsdPreview / buildUnlit
  lambert.h         deleted in M3.3
core/shading/
  types.h           IR value types, widths
  ir.h              IrGraph, IrNode, Ref, IrParam, IrOutput (host-only, STL allowed)
  ops.h             scalar op semantics (Karma-safe math, own polynomial sin/cos/exp/log/pow), NOVA_HOST_DEVICE
  noise.h           MaterialX Perlin / fBm / cell / Worley (bit-exact port), scalar, NOVA_HOST_DEVICE
  program.h         Op, Instr (16 B), Global, OutputBinding, TexSamplerState, Program (host), ProgramView (kernel POD)
  compiler.{h,cpp}  compile(const IrGraph&, const CompileOptions&) → std::shared_ptr<const Program>
  vm_lane.h         runLane(const ProgramView&, float* rows, const float* uniforms, const LaneContext&), NOVA_HOST_DEVICE
  vm_batch.{h,cpp}  runBatch<N>(...), CPU only, SoA rows, uses ops.h
  simd_noise.cpp    8-wide AVX2 noise kernels (CPU only), bit-identical to noise.h (uses core/math/simd8.h)
  shade_context.h   ShadingPoint, globals filling, primvar interpolation (incl. shifted barycentrics)
  builtin.{h,cpp}   default material (Lambert displayColor), makeConstantSurfaceMaterial() for tests
core/math/simd8.h   minimal AVX2 vfloat8/vint8 (no OIIO dependency)
core/texture/
  texture.h         TextureId, TexWrap, TexFilter, TexSamplerState, TextureInfo, TexQuery, ITextureSampler
  ray_cone.h        coneWidthAt, uvGradients, coneFootprint, stDerivatives, triangleCurvature, scatterSpread
  memory_sampler.{h,cpp}   reference bilinear+MIP sampler over nova::Image (tests, analytic checks)
core/scene/
  material.h        MaterialId, MaterialDesc, MaterialRecord, MeshMaterialBinding, MaterialTableView
  primvar.h         PrimvarRate, PrimvarChannel, AttributeRegistry
core/integrator/
  shade.{h,cpp}     material sort, chunking, VM dispatch, gather, closure build (shared by beauty and first-hit AOVs)
```

**Changed in core:**

- `core/scene/mesh.h`:
  - add `std::vector<PrimvarChannel> primvars` and `std::vector<uint32_t> triangleFace` (authored face per triangle);
  - remove `displayColor`, `hasDisplayColor` and `cornerColors` in M3.5, when `displayColor` becomes an ordinary channel.
- `core/scene/scene.{h,cpp}`:
  - add the material API, mesh bindings, the instance-rate primvar channels (which generalise `instanceColors`), the texture sampler, texture info and color matrices, and the attribute registry;
  - `commit()` builds the material table and the opacity flags.
- `core/scene/light.h` and `core/lights/lights.{h,cpp}`: add `specular`. Escape and area-light functions take a per-path light multiplier mix.
- `core/integrator/wavefront.{h,cpp}`: the shade stage is split (§2.4), and `WaveQueues` gains the new path state.
- `core/accel/accel.h` and `embree_accel.cpp`:
  - `RayBatch` gains `const uint32_t *opacitySeed` and `const OpacityContext *opacity`;
  - add `setMeshOpacityFilter(MeshId, bool)`;
  - set the Embree scene flag `RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS`.
- `core/geometry/subdiv.{h,cpp}`: generic vertex and face-varying primvar refinement, plus base-face ids per output triangle.
- `core/scene/camera.h`: `rayCone(ndc)` (spread, width at origin).
- `core/math/sobol.h`: `kSpare = 7` becomes `kTexture` (1D, OIIO stochastic filtering). `kLayer` stays reserved.

**New in hydra:**

```
hydra/material/
  material.{h,cpp}        HdNovaMaterial (Sprim)
  networkSource.{h,cpp}   HdNovaReadNetwork (terminal scene index) + GetMaterialResource fallback
  mtlxLibrary.{h,cpp}     MaterialX library load (search paths, std::call_once); nodedef queries
  translator.{h,cpp}      network → mx::Document → flatten → lower → IrGraph/params/textures/primvars/hash (pure function)
  lowering.{h,cpp}        Tier-1 node table (category+type → IR ops), defaultgeomprop inputs, Karma semantics
  usdAliases.h            UsdPreviewSurface/UsdUVTexture/UsdPrimvarReader_* ids, ND_normalmap → ND_normalmap_float
  registry.{h,cpp}        SdfPath→MaterialId, ProgramCache (shared_future), attribute names, warn-once
hydra/textureSystem.{h,cpp}   OIIO TextureSystem, ITextureSampler implementation, TextureId registry, stats
hydra/textureConvert.{h,cpp}  auto-conversion cache (ImageBufAlgo::make_texture), .rat via imaketx subprocess
hydra/colorSpace.{h,cpp}      source color-space decision (shared with textureCache.cpp), OCIO matrices
```

**Changed in hydra:**

- `renderDelegate.cpp`: the `material` sprim, the contexts, settings descriptors, and parallel material sync.
- `renderParam.h`: the material registry, texture system, frame and fps.
- `mesh.cpp`: material slots, generic primvars, the GeomSubset fast path, and primvar-only edits without a BLAS rebuild.
- `instancer.cpp`: generic instance-rate primvars.
- `light.cpp`: `specular`.
- `textureCache.cpp`: uses `colorSpace.cpp`.
- `cmake/NovaHoudini.cmake`: imported targets `Nova::MaterialX` (MaterialXCore and MaterialXFormat from `$HFS/custom/houdini/dsolib`), linked only into `hdNova_objects`.

**New tools and tests:**

- `tools/nova_bsdf_precompute/` (CMake target `nova_bsdf_tables`, not in ALL).
- `tests/test_bsdf_lobes.cpp`, `tests/test_closures.cpp`, `tests/bsdf_stats.h` (the validated harness from `m3research/test-strategy/chi2`).
- `tests/test_material_furnace.cpp`, `tests/test_shading_compiler.cpp`, `tests/test_shading_vm.cpp`, `tests/test_textures_core.cpp`.
- `tests/test_hydra_materials.cpp` (translator and IPR driver).
- `tests/husk/check_materials.py`.
- `tests/cuda/kernel_headers.cu`.
- `tests/data/{albedo_ref.json, mtlx_golden/*.json}`.
- `tests/reference/scenes/{common_mtl.usda, assets/sphere_st.usda, furnace_materials.usda, mtl_*.usda}`.
- `tests/reference/tools/{gen_albedo_ref.py, gen_vex_golden.py, karma_albedo_probe.py}`.
- `docs/research/m3/*.md`: copy the seven reports in as the first commit (the scratchpad is temporary).

### 2.3 Key data structures

#### IR (`core/shading/ir.h`, host only)

```cpp
namespace nova::shading {
enum class Type : uint8_t { Float, Int, Bool, Vec2, Vec3, Color3, Vec4, Color4, Mat33, Mat44 };  // width 1,1,1,2,3,3,4,4,9,16
struct Ref { enum Kind : uint8_t { None, Node, Const, Param, Global } kind = None; uint8_t output = 0; uint32_t index = 0; };
struct IrNode {
    Op op; Type type; uint32_t imm = 0;          // enums, sampler slot, primvar slot, swizzle pattern, shift index
    Ref in[3]; std::vector<Ref> ext;             // >3 operands (noise, fractal, Tex2D default, normalmap, ...)
};
struct IrParam  { std::string key; Type type; float value[16]; };   // "shaderPath.input"; never folded
struct IrOutput { uint16_t field; Ref value; };                     // SurfaceParams float offset
struct IrTexture { TexSamplerState sampler; };                       // per material texture slot (path lives in MaterialDesc)
struct IrGraph {
    std::vector<IrNode> nodes; std::vector<float> consts; std::vector<IrParam> params;
    std::vector<IrOutput> outputs; Ref opacity;                     // opacity also feeds the opacity-only program
    std::vector<std::string> primvars;                               // slot → name ("st", "displayColor", "mask")
    std::vector<IrTexture> textures; SurfaceModel model; uint8_t thinWalled;   // thin_walled is uniform → structural
};
}
```

- Int and Bool are stored as exact floats (exact up to 2^24).
- Strings, filenames and enums are never values; they become immediates or slot tables.
- Closure types exist only as the intrinsic model id.

#### Bytecode and program (`core/shading/program.h`)

```cpp
enum class Op : uint16_t {
  End, JumpIfUniformZero,                                              // control (uniform conditions only)
  Mov, Swizzle,                                                        // imm = 4×2-bit selectors + out width
  Add, Sub, Mul, Div, Mod, Mad, Min, Max, Clamp, Mix, Abs, Sign, Floor, Ceil, Round, Fract,
  Sqrt, Pow, Exp, Ln, Sin, Cos, Tan, Asin, Acos, Atan2, Smoothstep,    // component-wise, width 1-4, Karma-safe
  Gt, Ge, Eq, Select, And, Or, Not,
  Dot, Cross, Normalize, Length, Rotate2D, Rotate3D, TransformSpace, MatVec,
  RgbToHsv, HsvToRgb, Burn, Dodge, Comp4 /*imm: over|in|mask|matte|out|disjointover*/, Premult, Unpremult,
  Noise2, Noise3, Fractal2, Fractal3, Cell2, Cell3, Worley2, Worley3, // imm: out width | style
  Primvar,                                                             // imm: slot | width | shift(0,1,2); default operand
  Tex2D,                                                               // imm: sampler slot; a,b,c = uv, uv_dx, uv_dy; ext: default
  NormalMap, HeightToNormal,                                           // Karma semantics
  Count };
constexpr uint8_t kUA = 1, kUB = 2, kUC = 4, kSplatA = 8, kSplatB = 16, kSplatC = 32, kHasExt = 64;
struct Instr { Op op; uint8_t width, flags; uint16_t dst, a, b, c; uint32_t imm; };  // 16 B; ext words follow as Instr-sized
static_assert(sizeof(Instr) == 16);

enum class Global : uint8_t { P, Pdx, Pdy, Pobject, PobjectDx, PobjectDy, N, Ng, Nobject, T, B, I,
                              Time, Frame, RayDepth, RayType, PrimId, InstanceId, Count };
struct OutputBinding { uint16_t field; uint8_t width; uint8_t uniform; uint16_t index; uint16_t pad; };
struct TexSamplerState { TexWrap wrapS, wrapT; TexFilter filter; uint8_t channels, firstChannel, subimage; float width, blur; };

struct Program {                         // host, immutable, shared by structure hash
    std::vector<Instr> code, prologue, opacityCode;   // varying; host-uniform; opacity-only (DCE to opacity)
    std::vector<UniformInit> uniformInit;              // slot source: zero | const | param | hoisted
    std::vector<float> constants; std::vector<OutputBinding> outputs;
    std::vector<TexSamplerState> samplers; std::vector<std::string> primvars;
    uint16_t rowCount, uniformCount, paramCount; uint32_t globalMask; uint16_t globalRow[size_t(Global::Count)];
    SurfaceModel model; uint32_t flags;    // kVaryingFree, kHasOpacity, kHasEmission, kNeedsTangents, kNeedsDerivs
    uint64_t structureHash;
};
struct ProgramView { const Instr *code, *opacityCode; const OutputBinding *outputs; uint16_t outputCount, rowCount;
                     uint32_t globalMask; uint16_t globalRow[size_t(Global::Count)]; uint32_t flags; SurfaceModel model; };
```

**Register file:**

- Rows of floats. On the CPU a row is `R[row * 32 + lane]`, and a vecW value is W adjacent rows.
- **Hard cap of 256 rows**: 32 KB per CPU thread, which fits Zen 5's 48 KB L1D, and the same cap on the GPU. A program above the cap fails to compile, warns, and falls back to the next context or the default material.
- The destination never aliases a source.

**Uniform block (per material):**

- Slots 0–3 are zeros, then constants, then params, then hoisted results.
- The prologue runs on the host through `runLane` whenever params change.
- **Structure hash** = code, layout, constants, sampler states, primvar names, model and `thinWalled`. It excludes param values and texture paths.

#### Surface parameters (`core/bsdf/surface.h`)

```cpp
enum class SurfaceModel : uint8_t { StandardSurface, OpenPBR, UsdPreview, Unlit, LobeList /*M4*/ };
struct SurfaceParams {                    // ~300 B, linear rendering-space RGB, world-space vectors
    float baseWeight; Spectrum baseColor; float diffuseRoughness, metalness;
    float specularWeight; Spectrum specularColor; float specularRoughness, specularIor, specularAnisotropy, specularRotation;
    float transmissionWeight; Spectrum transmissionColor; float transmissionDepth, transmissionExtraRoughness;
    float subsurfaceWeight; Spectrum subsurfaceColor; Spectrum subsurfaceMfp; float subsurfaceAnisotropy;
    float sheenWeight; Spectrum sheenColor; float sheenRoughness;                 // SS sheen | OpenPBR fuzz
    float coatWeight; Spectrum coatColor; float coatRoughness, coatAnisotropy, coatRotation, coatIor, coatDarkening,
          coatAffectColor, coatAffectRoughness;
    float thinFilmWeight, thinFilmThicknessNm, thinFilmIor;
    Spectrum emission;                    // already weight·color (OpenPBR: max(lum,0)·color)
    float opacity, opacityThreshold;      // opacity only used by the opacity program
    Vec3f normal, coatNormal, tangent, coatTangent;   // (0,0,0) = use the geometric default (Nworld / Tworld)
    uint32_t flags;                       // useSpecularWorkflow (UsdPreview), normalBound, tangentBound, coatNormalBound
};
```

- Each material owns a **SurfaceParams template**: nodedef defaults plus all uniform-bound outputs, computed on the host after the prologue.
- The per-lane gather is a copy of the template plus the varying bindings only.

#### Closure (`core/bsdf/closure.h`)

```cpp
enum class LobeType : uint8_t { Lambert, Translucent, DiffuseQON, DiffuseEON, SheenCharlie, FuzzZeltner,
                                Conductor, Dielectric, ThinDielectric };
enum LobeFlag : uint8_t { kLobeDelta = 1, kLobeHasR = 2, kLobeHasT = 4, kLobeThinFilm = 8, kLobeDiffuseClass = 16, kLobeCoatFrame = 32 };
enum class FresnelMode : uint8_t { Dielectric, ConductorNK, F82Tint, Schlick };
struct Lobe {                             // 80 B POD
    Spectrum weight; float pick;          // weight: all layering/mix/tint factors at wo; pick: selection probability
    LobeType type; uint8_t flags; FresnelMode fresnel; uint8_t event;   // event: D/G/S × R/T (M6 LPE)
    float ax, ay, eta, energy;            // alphas (or sigma/sheen r); oriented IOR; scalar compensation/normalisation
    Spectrum c0, c1;                      // Dielectric: R scale, T scale | Conductor: F0 or n, F82 B or k | diffuse: rho
    float tfThicknessNm, tfIor, tfWeight;
};
struct ClosureList { static constexpr int kMaxLobes = 8; Lobe lobe[kMaxLobes]; Frame frame[2]; Vec3f ng;
                     Spectrum emission, albedo, sigmaA; uint8_t count, nonDeltaMask, flags, pad; };   // ~0.75 KB, stack/local
struct ClosureEval   { Spectrum fcos, fcosDiffuse; float pdf; };
struct ClosureSample { Vec3f wi; Spectrum weight; float pdf; float diffuseShare; float roughness; uint8_t flags; uint8_t event; };

NOVA_HOST_DEVICE void buildSurfaceClosure(const SurfaceParams&, const ShadingGeometry&, Vec3f wo, const BsdfTables&, ClosureList&);
NOVA_HOST_DEVICE ClosureEval evalClosure(const ClosureList&, Vec3f wo, Vec3f wi);              // non-delta lobes, mixture pdf
NOVA_HOST_DEVICE bool sampleClosure(const ClosureList&, Vec3f wo, Vec3f u, ClosureSample&);    // u = get3D(bounce(d,kBsdf))
```

All ωo-dependent factors (layer albedos, compensation, thin-film albedo) are computed once in `buildSurfaceClosure`. `evalClosure` and `sampleClosure` need no tables.

#### Textures (`core/texture/texture.h`)

```cpp
using TextureId = uint32_t;
enum class TexWrap : uint8_t { Periodic, Clamp, Mirror, Black, Constant /*MaterialX default outside [0,1]*/ };
enum class TexFilter : uint8_t { Closest, Linear, Cubic };
struct TextureInfo { uint32_t flags; /*kMissing, kUdim, kConstant*/ float average[4]; uint16_t colorMatrix; };
struct TexQuery { uint32_t count; const float *s, *t, *dsdx, *dtdx, *dsdy, *dtdy, *rnd; };   // SoA over active lanes
class ITextureSampler {   // one virtual call per instruction per chunk (like IAccel); host implementation in hydra
public:
    virtual ~ITextureSampler() = default;
    virtual void sample2D(TextureId, const TexSamplerState&, const float fallback[4], const TexQuery&,
                          float *out /*[c*count+i]*/, uint8_t *status) const = 0;
};
```

- A material's texture slot maps through `MaterialRecord::textureOffset` to a global `TextureId`. A texture path edit is therefore a table update, not a recompile.
- `kConstant` textures (detected at conversion) are answered from `average` with no OIIO call.
- Color matrices (linear gamut, e.g. ACEScg → scene_linear) are applied after the lookup, which is exact under filtering.

#### Primvars (`core/scene/primvar.h`)

```cpp
enum class PrimvarRate : uint8_t { Constant, Uniform /*per triangle*/, Vertex /*per point*/, Corner /*3 per triangle*/ };
struct PrimvarChannel { uint32_t nameId; PrimvarRate rate; uint8_t components; std::vector<float> data; };
```

- `MeshDesc::primvars` holds all authored numeric primvars with 1–4 components. Instance-rate channels live in `MeshRecord::instancePrimvars` (the generalised `instanceColors`).
- At `commit()`, each (mesh slot, material) pair gets a remap: program primvar slot → mesh channel index or instance channel (`int16`), with −1 meaning the op's default.
- Lookup rule as in M2: the mesh channel first, then the innermost instance channel.

#### Material table in the Scene (`core/scene/material.h`)

```cpp
using MaterialId = uint32_t;                       // 0 = default material (Lambert displayColor)
struct MaterialDesc { std::shared_ptr<const shading::Program> program; std::vector<float> params;
                      std::vector<TextureId> textures; };
struct MaterialRecord { uint32_t program, uniformOffset, templateIndex, textureOffset, flags; };   // kernel view
struct MeshMaterialBinding { MaterialId material; uint32_t primvarRemapOffset; };
struct MaterialTableView {                         // all pointers into flat, device-uploadable arrays
    const ProgramView *programs; const shading::Instr *instrPool; const shading::OutputBinding *outputPool;
    const MaterialRecord *materials; const float *uniformPool; const SurfaceParams *templates;
    const TextureId *texturePool; const MeshMaterialBinding *bindings; const int16_t *primvarRemaps; };
// Scene: createMaterial(), updateMaterial(id, MaterialDesc), removeMaterial(id),
//        setMeshMaterials(MeshId, std::vector<MaterialId> slots, std::vector<uint16_t> triangleSlot),
//        attributeId(std::string_view), setTextureSampler(const ITextureSampler*), setTextureInfo(...),
//        setColorInfo(xyzToRgb[9]); materialOf(mesh, prim) → binding index.
```

- `commit()` assigns material indices sorted by program, so materials sharing a program are adjacent. This gives bigger warp-coherent runs on the GPU in M10 at no cost on the CPU.
- Material edits never touch the accel, except for toggling a mesh's opacity filter flag.

#### Path state additions (`WaveQueues`)

```cpp
std::vector<uint8_t>  pathFlags;     // bit0 lastDelta, bit1 insideMedium, bits2-4 last event (D/G/S × R/T), bit5 lastTransmit
std::vector<Spectrum> sigmaA;        // absorption of the medium the path is in (0 outside)
std::vector<float>    diffuseShare;  // diffuse-class share of the last BSDF sample (light diffuse/specular multipliers)
std::vector<float>    coneWidth, coneSpread;
ShadeScratch shade;                  // keys, order, group starts, rows arena (256×32 floats), 32 ShadingPoints
// bsdfPdf: < 0 marks a delta sample (MIS weight 1 at the next light hit)
```

### 2.4 Execution model in the wavefront integrator

Shade stage `shadeSurfaces(...)`, three passes per bounce:

**Pass A, per ray.** The logic is unchanged from M2:
- camera-visible lights and environment at depth 0;
- area lights along BSDF rays and escaped light at depth > 0;
- the light contribution is now scaled by `lightMul = share·light.diffuse + (1−share)·light.specular`, with `share = q.diffuseShare[p]`, and `bsdfSampleWeight` returns 1 if `q.bsdfPdf[p] < 0` (delta);
- if `pathFlags & insideMedium`: `beta *= exp(−sigmaA·t)` (using `tMax` for misses);
- survivors push `(rayIndex, key = binding.material)`;
- `film.alphaSum` is incremented as now.

**Pass B.** LSD radix sort of the survivors by 16-bit material key: 2 passes × 256 buckets (3 passes above 65 535 materials). Target ≤ 1 ns/hit; the counting-sort prototype measured 0.6 ns. Sorting only reorders paths; each path's own additions keep their order, so images stay bit-deterministic.

**Pass C, per material group, chunks of 32 lanes.**

1. **Resolve hits.** `resolveHit` per lane writes a `ShadingPoint`:
   - P, Ng (authored), N (interpolated, **not** face-forwarded), Pobject, Nobject, barycentrics, mesh/prim/instance, wo, time;
   - ray-cone footprint vectors `dPdx = k·a1`, `dPdy = k·a2` (k = `nova:textureFilterWidth` = 0.25) and the barycentric shifts `dbdx`, `dbdy`;
   - T/B (the Karma st frame) only if the program needs tangents.
2. **Fill globals.** Only the global rows in `globalMask`. Pad lanes `[n, N)` with a copy of lane n−1 (FTZ/DAZ is already on).
3. **Run the VM.** Skipped if the program is varying-free (the common look-dev case). Otherwise `runBatch<32>` (n ≥ 17), `<16>` (9–16), `<8>` (3–8), or `runLane` per lane (n ≤ 2).
   - `Tex2D` and `Primvar` loop per lane inside the batch executor.
   - `Tex2D` makes one `ITextureSampler::sample2D` call per instruction and chunk. OIIO's batched API is a scalar loop anyway (texturesys.cpp:1335), so coherence comes from the material sort.
4. **Per lane:**
   - **Gather:** copy the material's SurfaceParams template, then apply the varying bindings.
   - **Build:** `buildSurfaceClosure(sp, geom, wo, tables, cl)`. Lobes with luminance weight < 1e-5 are culled. Pick probabilities are ∝ `lum(w_j·Ê_j(wo))` with a floor of 1e-3 of the total.
   - **Emission:** `radiance += beta·cl.emission`, clamped by `clampIndirect` for depth > 1, as for escaped light. MIS weight 1 (D7).
   - If `lastIteration`: continue.
   - **NEE**, only if `cl.nonDeltaMask != 0`:
     - one light, chosen by power as in M2, sampled from the offset origin;
     - the origin offset is on the `wi` side of `ng`, so transmitted light samples work;
     - `ClosureEval e = evalClosure(cl, wo, ls.wi)`;
     - a lobe returns 0 when its R/T class disagrees with `sign(dot(wi, ng))·sign(dot(wo, ng))`;
     - contribution `= beta·(e.fcosDiffuse·L.diffuse + (e.fcos − e.fcosDiffuse)·L.specular)·Li·w/lightPdf`, with `w = powerHeuristic(lightPdf, e.pdf)`.
   - **Continuation:**
     - `sampleClosure(cl, wo, sampler.get3D(bounce(d, kBsdf)), s)`; reject if `!(pdf > 0) || !isFinite(weight)`;
     - origin offset to the side of `wi`;
     - `beta *= s.weight`; `bsdfPdf = (s.flags & delta) ? −1 : s.pdf`; `diffuseShare = s.diffuseShare`;
     - set the event bits;
     - on refraction into a non-thin dielectric with `cl.sigmaA ≠ 0`: set insideMedium and sigmaA; on refraction out: clear them;
     - ray cone: `spread = scatterSpread(spread, W, curvature, dot(ng, d), s.roughness)` (roughness: diffuse 1, delta 0, microfacet α).
   - **Russian roulette:** unchanged (from `rrStartDepth`, survive = `clamp(maxComponent(beta), 0.05, 0.95)`).

**Rays and ray types.**
- Ray types are derived from path flags: Camera, Diffuse, Glossy, Specular (delta), Transmission, Shadow. The `RayType` and `RayDepth` globals exist for `kma_rayswitch` in M4.
- Shadow rays are occlusion queries with the opacity filter. Glass casts opaque shadows (true caustics only via BSDF paths, see Q5).

**Sobol streams.**
- `kBsdf` 3D: xy direction; z selects the lobe, and its remainder `(z − cdf[j−1])/p_j` feeds the inner choices (dielectric R/T, EON uniform vs CLTC).
- `kOpacity` 1D: the cutout decisions of the rays traced at this bounce, with crossing randomness `u = fract(u0 + φ·hash(mesh, prim, instance))`; shadow rays use a different salt.
- `kTexture` (the old `kSpare`): OIIO `rnd`, decorrelated per texture slot as `fract(u + φ·slot)`.
- `kLayer` stays reserved (albedo-scaling layering needs no randomness).

**Opacity.**
- Meshes whose material has `kHasOpacity` get `rtcSetGeometryEnableFilterFunctionFromArguments`.
- The argument filter resolves the hit, runs `opacityCode` through `runLane` (textures included), and rejects the hit if `u ≥ opacity`.
- The decision is a deterministic hash of (ray seed, prim, instance), so Embree's possible repeated filter calls for one hit agree.
- The filter applies to closest-hit queries (camera and indirect rays continue through the cutout without a scattering event, so `depth` stays global) and to occlusion queries (stochastic shadows).
- The first-hit AOV rays use a fixed seed.

**First-hit AOVs.** `renderFirstHitAovs` runs the same Pass B/C machinery in AOV mode (no NEE or sampling). Albedo = `cl.albedo` (Σ w·Ê); normal = the shading normal after normal mapping, face-forwarded. Textures use `rnd = −1`.

**Bit-exactness of the default path.** A Lambert material (QON with σ = 0, one lobe, pick 1) samples with `get3D(kBsdf).xy`, which equals today's `get2D(kBsdf)`. NEE and MIS values are identical, so all existing exact-value tests stay bit-exact.

### 2.5 Per-hit cost budget (CPU, Ryzen 9800X3D, measured numbers marked m)

| Step | Budget per hit |
|---|---|
| Radix sort by material | ≤ 1 ns (m: counting sort 0.6 ns) |
| resolveHit + footprint + globals | 20–40 ns |
| VM, varying-free material | 0 |
| VM, typical textured graph (~40 varying instructions, N=32) | ~8 ns (m: 0.15–0.16 ns per instruction·lane) |
| Texture lookup, warm cache | 75–250 ns each (m: 240 ns 1-texel aniso, 74 ns coarse; stochastic=3 cuts aniso cost ~40%) |
| Noise3d (AVX2) | 7 ns (m), fBm ≈ octaves × 7 ns |
| Gather + buildSurfaceClosure (2–4 LUT lookups) | 30–60 ns |
| NEE evalClosure (1–3 lobes) | 20–50 ns |
| sampleClosure | 30–60 ns |

**Acceptance target:** a scene of untextured Standard Surface plastics renders at ≤ 1.35× the wall time of the M2 Lambert version at equal spp; varying-free materials skip the VM.

**IPR:**
- value edit: engine side ≤ 1 ms from Sync to restart (no recompile, no texture reopen);
- structure edit: ≤ 1 ms compile for a Standard Surface graph with 3 textures (MaterialX flatten measured 0.18 ms);
- PLAN.md target: < 100 ms end to end.

### 2.6 How the same code runs on CUDA (M10)

| M3 CPU | M10 CUDA |
|---|---|
| Per-wave radix sort by material id | Per-bounce device sort of hit indices by material id (CUB radix); materials sharing a program are adjacent by construction |
| `runBatch<N>` over 32-lane chunks | `runLane` per thread (`vm_lane.h`, same `ops.h`); a warp sees one material, so the switch is non-divergent |
| Rows `R[row*32+lane]` in a 32 KB thread-local arena | `float R[rowCount ≤ 256]` in local memory, coalesced (same offsets across the warp) |
| Uniform block pointer, broadcast operands | Same pointer, `__ldg` broadcast reads |
| `JumpIfUniformZero` | Taken by all threads; varying conditions are already `Select` |
| `ITextureSampler::sample2D` (OIIO) | Device `sampleTexture(TextureId, TexSamplerState, s, t, dsdx..dtdy, rnd)` on the converted tiled EXRs (CUDA texture objects or OptiX Toolkit demand loading, which takes the same `tex2DGrad` inputs) |
| Opacity in the Embree argument filter, via `runLane` | OptiX any-hit program, same `opacityCode`, same `runLane` |
| `ClosureList` on the stack | Local memory; created and consumed in one shade kernel; never stored in queues |
| `BsdfTables` static arrays | Uploaded once to global memory; pointer struct in launch params |
| AVX2 noise kernels | Scalar `noise.h` (bit-identical) |
| Own polynomial transcendentals in `ops.h` | Same functions; compile with `--fmad=false` for bit-equality |

**Rules enforced from M3.1** (by review plus the `nvcc` compile check) for kernel-side headers:
- no STL containers, no virtual functions, no exceptions, no recursion, no host statics, no `double`;
- fixed-size arrays only: 8 lobes, 256 rows, ≤ 4 texture channels, bounded extension words;
- everything `NOVA_INLINE`.

---

## 3. BSDF model decisions

### 3.1 Lobe library

| Lobe | Formula, sampling | Source |
|---|---|---|
| GGX core | `D = 1/(π αx αy (x²/αx²+y²/αy²+z²)²)`; `t(ω) = |(αx ωx, αy ωy, ωz)|`; height-correlated `vis = 1/(2(t_o|μi| + t_i|μo|))`; spherical-cap VNDF (Dupuy & Benyoub 2023); **bounded cap** (Eto & Tokuyoshi 2023) only for reflection-only lobes (valid-sample share at α 0.8, μ 0.95: 62% → 97%); pdf `D/(2(k μo + t_o))` | microfacet §2 |
| Roughness maps | SS: `alphaSS(r,a)` (MaterialX `mx_roughness_anisotropy`, a clamped to 0.98); OpenPBR: `αt = r²√(2/(1+(1−a)²)), αb = (1−a)αt` | surface-models §6.3 |
| Conductor | `f = F·D·vis·energy`; `FresnelMode`: ConductorNK (SS via Gulbrandsen `artisticIor`, r clamped to 0.99), F82Tint (OpenPBR, clamped ≥ 0), Schlick (UsdPreview metal/specular workflow); thin film `F = lerp(F, F_airy, w)` | microfacet §3 |
| Dielectric | Walter 2007 with VNDF; exact Fresnel per microfacet; radiance-mode BTDF ÷ η²; frame flipped so μo > 0, η oriented by the **authored** Ng; R/T chosen per microfacet with `P_R ∝ F·max(c0)`, `P_T ∝ (1−F)·max(c1)`; `etaF` (Fresnel) and `etaT` (refraction) kept separate (equal in OpenPBR 1.1.1; 1.2 decouples them) | microfacet §2, surface-models §6.3 |
| Smooth dielectric | Delta below `max(αx, αy) < 1e-3`; sample weight `c0·F/P_R` or `c1·(1−F)/(P_T η²)`; `|η−1| < 1e-4` → pure pass-through delta (IOR-1 glass is invisible) | pbrt `EffectivelySmooth` |
| ThinDielectric | No bending, no IOR stack. Sheet Fresnel `c_θ = T_color^{1/cosθt}`, `T' = c_θ T²/(1 − (R c_θ)²)`, `R' = R + T' R c_θ`; T = mirrored R lobe with `αt = saturate(α·√(3.4(η−1)(η−0.5)²/η³))`; delta pass-through at αt ≈ 0 | Cycles `bsdf_microfacet.h`, Kulla-Conty 2017 |
| Lambert / Translucent | `ρ/π`, cosine sampling (translucent in the −N hemisphere) | — |
| QON (SS `diffuse_roughness`) | `f = ρ/π (A + B max(0,s)/max(μi,μo))`, `A = 1 − 0.5σ²/(σ²+0.33)`, `B = 0.45σ²/(σ²+0.09)`, σ = roughness; cosine sampling; MaterialX albedo fit for picks only | `mx_microfacet_diffuse.glsl:8-28` |
| EON (OpenPBR `base_diffuse_roughness`) | `f_FON + f_ms` (EON paper Listings 1-4; the spec text has a ρ_ms typo); CLTC + uniform one-sample MIS (26× lower variance at grazing); ρ clamped to ≤ 1 inside ρ_ms | EON 2024, `main.glsl` (MIT) |
| Charlie sheen (SS) | MaterialX form `D/(4(μi+μo−μiμo))`, D clamped at r ≥ 0.005; uniform hemisphere sampling; albedo from `charlieE` LUT (exact furnace); never the original-G form | `mx_microfacet_sheen.glsl:5-26` |
| Zeltner fuzz (OpenPBR) | `μi f = c·R(μo,r)·D_LTC`; exact LTC sampling (weight = c·R); r clamped to [0.01, 1]; R clamped to ≥ 0; skip if `|a⁻¹| < 1e-5` or `R < 1e-5` | `mx_microfacet_sheen.glsl:97-188` |
| Thin film | Belcour & Barla Airy per microfacet (VdotH), 2 iterations, outside IOR 1; XYZ→RGB with the **working-space** matrix (`Scene::setColorInfo`, from OCIO XYZ-D65 → scene_linear), normalised so the DC term is grey; transmission uses `1 − R_airy`; layering albedo `lerp(F_airy(μo), F_airy(1)·S0 + S1, √α)` | microfacet §5 |

### 3.2 Layering and the surface models

Layering is albedo scaling at ωo, which reduces to per-hit weight arithmetic:
- `layer(top, base)`: base weights × `(1 − Σ w_top·E_top(ωo))`, untinted (measured MaterialX and Karma semantics);
- `mix`: lerp;
- `multiply`: weight × `clamp(c, 0, 1)`, throughput unchanged (MaterialX).

**Standard Surface 1.0.1** (the closed form of the MaterialX graph, surface-models §2.3):

```
f = C·f_coat + (1 − C·E_coat)·t_c·[ M·f_metal
      + (1−M)·( s·c_s·f_specR + (1 − s·E_specR)·( X·c_t·f_T(αt)
      + (1−X)·( h·c_h·f_sheen + (1 − h·E_sheen)·( (1−S)·b·ρd·f_QON + S·(thin ? ρs·f_transl : ρs·f_diffuse_fallback) )))) ]
```

- `t_c = lerp(1, coat_color, C)`; `ρd = base_color^γ` with `γ = 1 + C·coat_affect_color`.
- Coat-affected roughness: `lerp(r, 1, coat_affect_roughness·C·r_c)`.
- `specular < 1`: `tR = s·c_s`, `tT = X·c_t·(1 − s·E_R)/(1 − E_R)`.
- Split α for `transmission_extra_roughness` (R uses α, T uses αt, scaled by `(1−E_R(αR))/(1−E_R(αT))`).
- Emission: `[(1−C) + C·coat_color·(1−F0c)(1−(1−μo)^5)]·emission·emission_color`.
- `thin_walled`: Autodesk-spec semantics. Transmission becomes ThinDielectric (Karma does the same; the 1.39.5 graph only switches translucent vs subsurface).

**OpenPBR 1.1.1** (surface-models §3.4):

```
f = F·c_f·f_fuzz + (1 − F·E_fuzz)·[ C·f_coat + (1 − C·E_coat)·t_c·d·( M·f_metal(F82, ξs)
      + (1−M)·( c_s·f_specR(η') + (1 − E_specR(η'))·( T·tint_T·f_T(η') + (1−T)·( S·sss + (1−S)·b·f_EON ) ) ) ) ]
```

- Coat darkening uses the MaterialX/Karma `K = 1 − (1−F0c)/ηc²` (not the spec's E_F form; difference 0.3089 vs 0.2980).
- `E_b = lerp(lerp(base, sss_color, S), ξs·base, M)`.
- Coat roughening `r' = lerp(r, min(1, r⁴ + 2rc⁴)^{1/4}, C)`.
- `η' = openPbrModulatedEta(lerp(nb, ratio_fix, C), ξs)` with the coat TIR fix.
- Thin-walled subsurface uses the **spec** form `½C(1−g)f_diffuse + ½C(1+g)f_transl`, not Houdini's graph, which has a C² bug.
- `transmission_depth > 0`: tint 1, `σa = −ln(clamp(color, 1e-5, 1))/depth` (Karma).

**Dielectric energy split** (exact furnace):
- R is compensated by `1/E_RT`.
- The layering albedo is `E_R^comp = E_R/E_RT`.
- T is normalised by `1/E_T` and then weighted by `(1 − E_R^comp) = E_T/E_RT`, so R + T = 1 for white glass and `E_R^comp + (1−E_R^comp)·ρ` over diffuse.
- On backfaces of non-thin dielectrics, R-only layers emit nothing; the transmission lobe uses `1/η` and `dielRTinv`.

**UsdPreviewSurface** (D4), mapped onto the same lobes:
- clearcoat: dielectric R, ior 1.5, `clearcoatRoughness`;
- metallic workflow: `mix(layer(dielectric R(ior, r²), Lambert(diffuseColor)), Schlick conductor(F0 = diffuseColor, F90 = 1), metallic)`;
- specular workflow: Schlick R (F0 = specularColor, F90 = 1) layered over Lambert;
- emission: `emissiveColor`;
- `opacity` with `opacityThreshold` (> 0: cutout at the threshold; 0: fractional, stochastic);
- `normal`: tangent-space normal through the Karma frame;
- `occlusion` is ignored; displacement is M4.

**surface_unlit**: emission = `emission·emission_color`; transmission = ThinDielectric with η = 1 (a delta pass-through weighted by `transmission·transmission_color`); opacity.

**Default material (id 0)**: Standard Surface with base 1, specular 0, base_color = primvar `displayColor` (default 0.18, instance channel fallback). This equals the existing `displayColorLambert` reference network and today's Lambert (Q1).

### 3.3 Energy-compensation LUTs

**Axes:** `r = (αx αy)^{1/4}`, `y = √μ`, `z = √((η−1)/(η+1))`. All float32, bilinear or trilinear lookup.

| Table | Dims | Content |
|---|---|---|
| `ggxE` | 32×32 | E_ss with F = 1 |
| `ggxEavg` | 32 | 2∫Eμ dμ |
| `ggxABC` | 32×32×3 | ∫f₁cos·(1−s), ∫f₁cos·s, ∫f₁cos·μh(1−μh)⁶ → exact albedo of any Schlick/F82 Fresnel; also the Airy approximation |
| `dielRT`, `dielRTinv` | 32³ each | E_R + E_T for η > 1 and for 1/η (energy units) |
| `dielR` | 32³ | single-scatter E_R (layering E_R/E_RT) |
| `fresnelAvgDiel` | 64 | exact 2∫Fμ dμ (the Kulla-Conty fit is off by 23% at η 1.1) |
| `charlieE` | 32×32 | albedo of the MaterialX Charlie form |

- **Total:** 103 520 floats ≈ 414 KB.
- **Compensation:** conductors and Schlick use `f_ss·(1 + Fms(1−E)/E)` with `Fms = Favg·Eavg/(1 − Favg(1−Eavg))`. No extra lobe and no pdf change; white metal gives exactly 1.
- **Measured LUT error at 32²:** max abs 0.0020, mean 0.0002. The MaterialX analytic fit (0.039) would fail a 1% furnace and is never used.
- **Generation:** `tools/nova_bsdf_precompute <table>`:
  - links only core headers and uses the **runtime** sample functions with Nova's Owen-scrambled Sobol, TBB-parallel, accumulating in double;
  - 2²² samples per cell for 2D tables, 2²⁰ for 3D tables;
  - takes minutes, so it is a CMake target `nova_bsdf_tables` outside ALL;
  - output `core/bsdf/tables/bsdf_tables.inc` (hex floats, parameters and generator source hash) is committed.
- **GPU:** global memory via the `BsdfTables` pointer struct (too big for 64 KB constant memory).

### 3.4 Coverage in M3

| Standard Surface 1.0.1 | M3 | OpenPBR 1.1.1 | M3 |
|---|---|---|---|
| base, base_color, diffuse_roughness (QON) | yes | base_weight/color, base_diffuse_roughness (EON), base_metalness | yes |
| metalness, specular, specular_color/roughness/IOR | yes | specular_weight (IOR modulation), color (F82 tint), roughness, ior | yes |
| specular_anisotropy/rotation | yes | specular_roughness_anisotropy (rotate `geometry_tangent` upstream) | yes |
| transmission, transmission_color | yes | transmission_weight, color | yes |
| transmission_depth (Beer, Karma behaviour) | yes | transmission_depth | yes |
| transmission_scatter(+aniso) | warn (M8) | transmission_scatter(+aniso) | warn (M8) |
| transmission_dispersion | warn (D1) | transmission_dispersion_* | warn (D1) |
| transmission_extra_roughness | yes | — | — |
| subsurface* | diffuse fallback (M9 random walk) | subsurface_* | diffuse fallback; thin-walled spec form |
| sheen* | Charlie (MaterialX form) | fuzz_* | Zeltner |
| coat*, coat_affect_color/roughness, coat_normal, coat_rotation | yes | coat_*, coat_darkening, geometry_coat_normal/tangent | yes |
| thin_film_thickness (nm), IOR | yes | thin_film_weight/thickness (µm)/ior | yes |
| emission, emission_color | yes | emission_luminance (radiance 1:1), color | yes |
| opacity (ACEScg luminance coefficients from the nodedef) | yes, stochastic cutout | geometry_opacity | yes |
| thin_walled | yes, spec semantics | geometry_thin_walled | yes |
| normal, tangent | yes | geometry_normal, geometry_tangent | yes |

### 3.5 Sampling strategy

- **One-sample MIS over lobes.** Pick j with `p_j ∝ lum(w_j·Ê_j(ωo))`, floored, and depending **only on ωo**. The pdf is the mixture `Σ p_k pdf_k` over non-delta lobes; f is the sum.
- **Lobes return f·|cos|** with their own frame normal (base or coat), because coat and base normals differ.
- **Delta lobes** are excluded from NEE and the mixture pdf. The sample weight is `w_j·value/p_j` with the delta flag set. If every lobe is delta, NEE is skipped (no shadow ray).
- **Inner choices** reuse the remapped `u.z` remainder, so the Sobol xy pair stays intact.

### 3.6 Robustness rules

1. Sanitise inputs:
   - weights clamped to [0,1], colors to ≥ 0, multiply tints to [0,1];
   - η clamped to [1e-4, 1e4] (the Velvet preset with IOR 0 gives F0 = 1);
   - α ≥ 1e-4, delta below 1e-3;
   - OpenPBR anisotropy 1 (αb = 0) handled by the delta path on that axis.
2. Bend each closure normal with Cycles' `ensureValidSpecularReflection(Ng, wo, N)` (Karma's equivalent clamp uses bias 0.01).
3. Classify reflect vs transmit by `Ng` during sampling, and reject samples where the N and Ng hemispheres disagree. In eval, test only the lobe's own shading hemisphere.
4. Ray origin offset by the side of `wi`. Entering vs exiting comes from the authored `dot(wo, Ng)`. Thin-walled surfaces always count as entering and never bend.
5. Reject a sample if `!(pdf > 0) || !isFinite(weight)`. Clamp μ ≥ 1e-6 in LUT lookups. The `vis()` form has no tan and stays finite at μ → 0.
6. The MIS sentinel `bsdfPdf < 0` (delta): `bsdfSampleWeight` returns 1. Passing ∞ would give NaN.
7. `MaterialX Nworld` is not face-forwarded in the VM. Face-forwarding happens only in the closure build (otherwise normal maps flip on back faces).
8. The Cycles bump-shadowing term is **not** applied in M3, for Karma parity at terminators; revisit in M7.

### 3.7 Known, deliberate differences from Karma (measured)

| Effect | Size | Handling |
|---|---|---|
| Karma's dielectric spec is Schlick-fit at the macro angle; Nova uses exact Fresnel per microfacet | up to −21% of the spec part at μ 0.5 | Bright bases, soft lights, highlight regions ≤ 0.10 |
| SS sheen is Lambert-shaped in Karma | furnace equal, shape differs 30–60% under back light | Furnace strict, lit-region tolerance 0.10 |
| Thin film: Karma has an sRGB-matrix cast (R/G 1.27) | +27% R | Analytic tests only, no Karma scene |
| Rough glass is not compensated in Karma (r 0.3: 0.965–0.971) | −3 … −34% | Strict scenes use r ≤ 0.2 glass; rough glass only in Nova furnaces |
| SS metal at r ≥ 0.8 (−1.5 … −6%), OpenPBR metal r 0.2 (+0.18%) | | r ≤ 0.6 in Karma scenes |
| SS QON on Karma CPU differs from MaterialX (−3 … −5%); XPU matches | | Region tolerance 0.06 |

Never copy `mxpbr.h`, `ggx_utils.h` or `mxstd.h`: they are marked proprietary by SideFX. They are behaviour documentation only.

---

## 4. Hydra side

### 4.1 Delegate changes (`hydra/renderDelegate.cpp`)

- `SUPPORTED_SPRIM_TYPES` gains `HdPrimTypeTokens->material`. `CreateSprim` returns `HdNovaMaterial`.
- `GetMaterialRenderContexts()` returns `{"nova", "kma", "mtlx"}`.
  - This follows Karma's priority `kma > mtlx > vex > universal` (help `kug/materials.txt:46`, verified with a red/green/blue probe).
  - `vex` comes in M5. `hview` and `__all` are never requested.
- `GetMaterialBindingPurpose()` stays `full`. husk passes `[full, ""]`, and the all-purpose fallback is built in.
- `IsParallelSyncEnabled(material)` returns true from M3.5 on, after the thread-safety test:
  - the MaterialX library is immutable after `call_once`;
  - program and texture registries are mutex- or future-guarded;
  - the heavy work runs before `AcquireSceneForEdit()`.
- New settings descriptors: `nova:textureFilterWidth` (0.25), `nova:textureBlur` (0), `nova:textureAutoTx` (1), `nova:textureCacheDir` ("" = `$HOUDINI_TEMP_DIR/nova_tx`), `nova:abortOnMissingTexture` (0), `nova:textureStats` (0). The UI comes in M4.

### 4.2 Material sprim and network reading

**Reading** (`networkSource.cpp`):
- `HdNovaReadNetwork(terminalSceneIndex, id, ctx, &src)` reads `HdMaterialSchema`, nodes, parameters (value, colorSpace, typeName as separate fields), connections, terminals with output names, `nodeTypeInfo` (kept for M5) and `config` (`mtlx:version`). The code is verified in the probe (`m3research/hydra-materials/probe/probe.cpp`).
- Fallback when no terminal scene index exists: `GetMaterialResource` + `HdConvertToHdMaterialNetwork2`, then move the `colorSpace:*` and `typeName:*` pseudo-params out and fill empty terminal output names from the nodedef.

**Context chain:** `nova → kma → mtlx → ""`, per material. A context is used if it reads and its surface terminal translates. An untranslatable terminal (an unknown node type or a generic pbrlib graph before M4) tries the next context, then falls back to the default material with one warning.

**Sync** (`material.cpp`, same shape as hydra-materials §4.4):
1. Every edit arrives as AllDirty (0x7c) under Hydra 2.
2. **No-op check:** `src.context == _context && src.net == _lastNet`. This catches edits on disconnected nodes.
3. `HdNovaTranslate` (a pure function) produces the IR, params, texture requests, primvar names and `structureHash`.
4. Equal hash → reuse the program: value-only edit.
5. `desc.textures = textureSystem.Acquire(requests)`. Handles open lazily.
6. Commit with `AcquireSceneForEdit().updateMaterial(id, desc)`. The edit version is bumped and the render restarts progressively.
7. `Finalize` orphans the id, so meshes (which are never re-synced) render the default material.

### 4.3 Translation (`translator.cpp`, `lowering.cpp`, `mtlxLibrary.cpp`)

**Library:**
- `mx::loadLibraries({"stdlib","pbrlib","bxdf","cmlib","nprlib","lights","houdini"}, …)` once. Measured: 72 files, 898 nodedefs, 282 nodegraphs in 41 ms.
- Search paths, in order:
  1. `NOVA_MTLX_SEARCH_PATHS`
  2. `PXR_MTLX_PLUGIN_SEARCH_PATHS`
  3. `PXR_MTLX_STDLIB_SEARCH_PATHS` (set by HUSD inside husk/Houdini)
  4. `<dir of MaterialXCore.dll>/../houdini/materialx/libraries` via `GetModuleHandleW` + `GetModuleFileNameW` (`ArchGetAddressInfo` fails on Windows here). This is what makes nova_cli work.

**Per material:**
1. Canonicalise:
   - drop pseudo-params;
   - USD aliases (`UsdPreviewSurface`, `UsdUVTexture` → `ND_UsdUVTexture_23`, `UsdPrimvarReader_*`, `UsdTransform2d`);
   - `ND_normalmap` → `ND_normalmap_float`;
   - `HdMtlxGetNodeDefName`; `upgradeVersion()` when `config:mtlx:version` is missing.
2. Build an `mx::Document` with `setDataLibrary(sharedLib)`. `HdMtlxCreateMtlxDocumentFromHdNetwork` is not used: it costs 18 ms per material (library deep copy) and mistypes unknown nodes. Never call `validate()` with Houdini's libraries attached.
3. `flattenSubgraphs(filter)`. The filter keeps `ND_standard_surface_*`, `ND_open_pbr_surface_*`, `ND_UsdPreviewSurface_*`, `ND_surface_unlit*`, image and the noise family native; the other 198 stdlib graphs, the `hmtlx*` graphs and the UsdUVTexture/UsdPrimvarReader graphs expand. Measured 0.18 ms. Expansion inserts `geomNode_UV0` for interface inputs that have a `defaultgeomprop`.
4. Lower `topologicalSort()` through the Tier-1 table:
   - widths from types; FA variants splat;
   - `dot`, `constant` and `convert(bool/int)` vanish;
   - `invert` → Sub; `remap` → Sub/Mad with a hoisted scale; `ramplr`/`ramptb` → Mix with Karma orientation and no clamp; splits → Ge + Select; ifgreater family → Gt + Select; extract/combine → Swizzle/Mov;
   - unconnected `defaultgeomprop` inputs of primitives become explicit globals: UV0 → primvar `st` (honouring `USDMTLX_PRIMARY_UV_NAME`), Pobject (prototype space for instances), Nworld, Tworld, Bworld;
   - `geomcolor` → primvar `displayColor`;
   - unknown or extra inputs (`karma_*`, `aov:*`, render properties, `houdini_visualize_signal`) are ignored quietly.
5. **Unknown nodes:** warn once per (material, nodeTypeId); outputs = the nodedef output default or a zero of the consumer type.
6. **Defaults** only from the nodedef via `getActiveInputs()` (version inheritance: Standard Surface 1.0.1 has base 1.0 and base_color 0.8). Houdini authors only non-default values.
7. **Structural vs value inputs:**
   - Structural (hashed): node types, connections, terminals; uniform string/enum/int/bool inputs (filtertype, address modes, geomprop, space, thin_walled); primvar names; the model.
   - Values: floats, vectors, colors.
   - Texture paths and color spaces go into texture requests and are not hashed.

**Semantics.** Karma's, as verified by husk probes and `vexexec` (mtlx-nodes §3–4):
- x/0 = 0, sqrt(−x) = 0, log(≤0) = 0, `pow(b<0, rint(e))`, `pow(0, −e)` = 0, asin/acos clamped, `round = floor(x+0.5)`, `clamp = max(lo, min(x, hi))`, `normalize(0) = 0` (not `nova::normalize`, which returns +Z);
- smoothstep `clamp((t−lo)/(hi−lo))` with safe division;
- ramptb and splittb use the Karma (1.39.0–1.39.4) orientation; ramps do not clamp;
- dodge `1 − bg/(1−fg)`;
- address mode `constant` returns black;
- normalmap: Gram-Schmidt plus the sign fix against Ng;
- heighttonormal: world-space bump on finite differences;
- time `= (frame − 1)/fps`.

### 4.4 Program cache, structure hash and IPR

- `ProgramCache`: `unordered_map<uint64_t, shared_future<shared_ptr<const Program>>>`. A hash hit is verified by comparing the serialised structure blob. Two threads never compile the same graph twice.
- Node renames keep the hash, so structurally equal materials share one program, each with its own uniform block (MDL-style class compilation).

| Edit | Engine path |
|---|---|
| Edit on a disconnected node | `HdMaterialNetwork2::operator==` → no-op, no restart |
| Parameter value, interface input | Same hash → new param block → prologue on the host (µs) → template rebuilt → restart |
| Texture path, color space | Same hash → texture table update (`TextureId` re-acquire, conversion if needed) → restart |
| geomprop rename, rewire, enum change | New hash → compile (≤ 1 ms) → `commit()` recomputes primvar remaps for the meshes using the material (no mesh re-sync needed: all primvars are uploaded) |
| `material:binding` rebind | Mesh `DirtyMaterialId` → `setMeshMaterials` (no BLAS rebuild) |
| GeomSubset rebind (arrives as `Points\|Primvar\|Topology\|Normals`) | Topology compare + `_GeometryUnchanged` → slots only, no BLAS rebuild |
| Material removed | Meshes keep the dead path → orphaned id → default material |

### 4.5 Mesh side (`hydra/mesh.cpp`, `hydra/instancer.cpp`, `core/geometry/subdiv.cpp`)

- **Slots.**
  - Slot 0 = `GetMaterialId()`; further slots from `HdGeomSubset{TypeFaceSet}`.
  - `triangleSlot[t] = faceSlot[DecodeFaceIndexFromCoarseFaceParam(primitiveParams[t])]`; for subdivision surfaces via the new base-face ids.
  - `Sync` handles `DirtyMaterialId` without touching geometry (today it returns early, mesh.cpp:503).
- **Primvars.**
  - Every authored numeric primvar with 1–4 components (float, int and half converted to float; indexed primvars flattened) becomes a `PrimvarChannel`: Constant (1 value), Uniform (per triangle via primitiveParams), Vertex (per point; expanded to corners when the mesh is de-indexed for face-varying normals), Corner (`ComputeTriangulatedFaceVaryingPrimvar`).
  - Subdivision surfaces refine vertex and face-varying primvars with Bfr and map uniform ones through base-face ids.
  - `displayColor` becomes an ordinary channel.
  - Instance-rate primvars of point instancers and native instances become instance channels (generalising `instanceColors`, with the same innermost-level rule). This is required to keep the M2 `instancer` reference green, because `displayColorLambert` reads displayColor through geompropvalue.
- **Primvar-only edits** (points, normals and topology unchanged) call `Scene::updateMeshPrimvars` and keep the BLAS. This is an IPR performance win.
- **Asset or string primvars** (geompropvalueuniform for texture paths) are deferred to M4. Warn.

### 4.6 Textures

**System** (`hydra/textureSystem.cpp`):
- `TextureSystem::create(false, ImageCache::create(true))`: a private TS on the shared IC, so husk's `--oiio-*` limits apply.
- TS attributes `flip_t=1`, `gray_to_rgb=1`, `stochastic=3`.
- Per lookup: `anisotropic=8`, `conservative_filter=true`, explicit wrap modes (converted files carry `wrapmodes black,black`).
- `TextureOpt` is built per call, because OIIO writes into it. `Perthread` is fetched once per batch.
- The scalar handle API is called in a loop.
- Filter mapping: closest → Closest, linear → Bilinear (MIP Default), cubic → Bicubic.

**Registration policy per request**, keyed by (path, chosen color space, mtime):

| Case | Policy |
|---|---|
| Linear, tiled, MIP-mapped, scene_linear | Direct |
| Same, but linear in another gamut | Matrix: a 3×3 from the OCIO processor, verified linear, applied after the lookup |
| Untiled, unmipped, or non-linear encoding | Convert: `ImageBufAlgo::make_texture` to half, scene_linear, tiled 64, MIP-mapped EXR with `maketx:fixnan=box3`, `constant_color_detect`, `opaque_detect`, zip, and `oiio:ColorSpace` set explicitly (make_texture copies the input's tag otherwise) |
| `.rat` | ConvertRat: subprocess `imaketx src.rat cache.exr` (OIIO has no rat reader; `iconvert` linearises with gamma 2.2) |
| Missing | Missing |

**Never use `colortransformid`.** In OIIO 2.5.18 it is ignored for untiled files and automip levels and quantises 8-bit tiles.

**Conversion cache:**
- Location `nova:textureCacheDir`, default `$HOUDINI_TEMP_DIR/nova_tx/` (Q6).
- Key SHA-1(canonical path, size, mtime, source color space, format version).
- Write to a temporary name, then rename atomically (several husk processes may run at once).
- UDIM sets are converted per tile under one prefix.
- `usdz[…]` assets are extracted through `ArResolver::OpenAsset`.
- Conversions run outside the registry mutex in a TBB `task_group`.

**Source color space** (`hydra/colorSpace.cpp`, also used by the light texture cache):
1. Explicit: `colorSpace:file` metadata (Karma honours it, verified), `UsdUVTexture.sourceColorSpace` raw/sRGB, or a MaterialX image of non-color type (float/vectorN), which means raw.
2. OCIO file rules, skipping the Default rule (which maps to ACES2065-1 in Houdini's config).
3. File metadata `oiio:ColorSpace`.
4. 8-bit data means sRGB, anything else linear (the Karma rule; it fixes 8-bit `.tx` from `imaketx`, and replaces the extension rule in `textureCache.cpp`).

1- and 2-channel images are linear. Without `$OCIO` (nova_cli), scene_linear is taken to be Linear Rec.709.

**UDIM** (Nova-defined, equal to Karma):
- `tile = 1001 + ⌊u⌋ + 10⌊v⌋` for `0 ≤ u < 10`, `v ≥ 0`; otherwise `default`. This guards OIIO's `int()` truncation, under which u ∈ (−1, 0) would read tile 1001.
- Missing tiles return `default` silently (`missingcolor` is set for UDIM sets only).
- Clamp wrapping inside a tile.
- Tokens `<UDIM>`, `<UVTILE>`, `%(UDIM)d`.
- Hydra's resolved path is empty unless tile 1001 exists; Nova then resolves the authored path itself and warns.

**Footprints** (ray cones, `core/texture/ray_cone.h`):
- Camera: spread `atan(2 tan(fovY/2)/H)` for a pinhole; width = pixel size for orthographic or `near·spread` for perspective.
- Transfer: `W = width + spread·t`.
- Ellipse (JCGT 2021 eqs. 8–9): `a1 = h1·W/|f·d|`, `a2 = (f × h1)·W`.
- uv gradients from barycentric gradients.
- Shifted globals use `k·a1`, `k·a2` with `k = 0.25`. This reproduces Karma's measured MIP choice: level 2.00 / 4.03 / ≈0 at 16 / 64 / 4 texels per pixel, independent of spp.
- Scattering: `spread += −2 k_c W/(n·d)`, then `max(spread, α_lobe)` (Cycles rule).

**Derivatives** (shifted re-evaluation, compiler pass `cloneDerivativeCones`):
- For each `Tex2D` uv input and `HeightToNormal` input, the varying backward cone is cloned twice with `P→Pdx/Pdy`, `Pobject→PobjectDx/Dy` and `Primvar shift 0→1/2` (barycentrics `b + dbdx`).
- Uniform parts are never cloned, and CSE merges shared clones.
- `Tex2D` computes `dsdx = uv_dx.s − uv.s` and so on. For affine cones (place2d, tiledimage) this is exactly `k·∇u·a1`.
- `HeightToNormal` uses `dP'/dx = dPdx + ng·(h_dx − h)·scale` (Karma's world-space bump) with a minimum shift of `1e-5·|P|`.

**Missing textures:** the node's `default`, one stderr warning per file (`HD_NOVA_WARN`; husk prints no Tf diagnostics). `nova:abortOnMissingTexture` aborts.

**Statistics** (`nova:textureStats ≥ 1`): `getstats`, IC `stat:cache_memory_used`, `bytes_read`, `fileio_time`, the list of converted textures, and textures that only ever used level 0.

---

## 5. Ordered sub-steps

Each step builds, keeps all existing tests green, and adds its own tests. LOC estimates include tests.
Effort scale: S ≤ 1 day, M 2–4 days, L 5–8 days.

### M3.0: Housekeeping (S, low risk)
- Copy the seven reports into `docs/research/m3/`.
- Port `tests/bsdf_stats.h` (the validated chi² harness with open Gauss-Legendre per-cell integration and Wilson-Hilferty p-values).
- Add the `tests/cuda/kernel_headers.cu` compile-only target: enabled when CUDA is found, label `cuda`, includes `core/math/*.h` for now.
- **Test:** the target compiles; Catch2 smoke test of the harness (Lambert passes; the wrong-pdf Lambert fails with p ≈ 0).

### M3.1: Lobe library and LUTs (L, ~2 500 LOC, medium risk)
- `core/bsdf/{common,microfacet,fresnel,thin_film,diffuse,sheen,dielectric,conductor,shading_frame,energy}.h`, `tools/nova_bsdf_precompute`, the generated `bsdf_tables.inc`, and nvcc coverage of all of them.
- **Tests** (`test_bsdf_lobes.cpp`):
  - normalisation: ∫D m_z = 1 and ∫D_ωo = 1 (1e-4); pdf mass vs accepted fraction < 2e-3;
  - chi² for GGX (iso, aniso, full and bounded cap), dielectric η ∈ {1.5, 1/1.5} × α ∈ {0.05, 0.5} at grazing μ 0.1, ThinDielectric T, EON r ∈ {0.5, 1}, QON, Zeltner, Charlie;
  - sample/eval/pdf consistency to 1e-4;
  - reciprocity (Lambert, QON, EON, GGX R without compensation, Charlie; BTDF ratio (n_o/n_i)² = 0.4444);
  - Fresnel identities (normal-incidence value, TIR, k = 0 conductor = dielectric, `F82(1/7) = tint·Schlick(1/7)`, average vs quadrature, F ≥ 0, artistic IOR round trip);
  - LUT anchors: E(1, α1) = 0.307, E(0.5, 0.5) = 0.698, E_RT(1.5, α0.3, μ1) = 0.989, E_RT(1/1.5, 1, 1) = 0.418, E_R(1.5, α0.05, μ1) = 0.0403, and the E_ggx table in surface-models §6.4 (±0.002);
  - 16 random cells recomputed at 2¹⁶ samples within 4σ;
  - per-lobe white furnace ±0.5% (compensated metal F = 1; dielectric RT for η ∈ {1.33, 1.5, 2, 1/1.5}; EON ρ = 1; ThinDielectric R' + T' = 1);
  - NaN/Inf hunt over 10⁶ random inputs (μ = ±1e-7, α = 1e-4, IOR 0, anisotropy 1).
- **Risk:** porting EON CLTC; LUT axes near η = 1. Mitigation: the code verified by the researchers (`m3research/microfacet/verify/bsdf_sketch.h`).

### M3.2: Closures and surface models (M–L, ~1 800 LOC, medium risk)
- `closure.h`, `surface.h`, `surface_models.h` (Standard Surface, OpenPBR, UsdPreview, Unlit), with lobe picks and albedo estimates.
- **Tests** (`test_closures.cpp`):
  - full-mixture chi² for 5 random parameter sets per model;
  - layered white furnaces ±0.5% for all μo and roughnesses (metal, diffuse, subsurface fallback, translucent, each with white coat and with sheen/fuzz, thin film, anisotropy);
  - exact anchors: OpenPBR coat η 1.6 smooth over ρ 0.5 with specular_weight 0 = 0.3089 / 0.3099 / 0.3175 / 0.3468 / 0.4340 / 0.6434 at μ 0.98 … 0.125; glossy-diffuse `E_R + (1−E_R)·0.5`; SS `specular_color (1,0,0)` over white gives G = 1 − E_R(μ); QON A(1) = 0.624060; Zeltner R(1, 0.5) = 0.0249;
  - Standard Surface vs OpenPBR on the shared subset (no coat/sheen, specular_weight 1, roughness 0, white specular): identical closures;
  - energy fuzz over 512 sets: E ≤ 1 + 4σ.
- **Risk:** the OpenPBR IOR modulation and coat TIR-fix details; thin-film color matrix.

### M3.3: Integrator rewrite and material table, constant programs only (M, ~1 400 LOC, medium-high risk)
- `program.h` (POD only), `core/scene/material.h`, Scene material API and table, `core/integrator/shade.{h,cpp}`.
- Radix sort, chunking, gather; closure NEE and sampling; transmission; delta sentinel; path flags; absorption; emission; light `specular` (core + `hydra/light.cpp`); closure-based first-hit AOVs; `builtin.cpp` (default material through a temporary `Global::DisplayColor` row filled from `SurfaceHit::color`, removed in M3.5); remove `lambert.h`.
- **Tests:**
  - all existing tests bit-exact (§2.4);
  - `test_material_furnace.cpp` through `Renderer`, orthographic sphere, disk r < 0.97:
    - Lambert ρ (2e-3);
    - white plastic, coat, sheen and fuzz over white, smooth, IOR-1 and thin glass, translucent, F = 1 mirror → 1 ± 3e-3;
    - rough glass r 0.3 → 1 ± 3e-3 (Karma 0.965);
    - emission (0.5, 0.25, 0.125) ± 1e-5;
    - transmissive sphere alpha 1 (exact);
    - IOR-1 sphere over a Lambert plane: plane = ρ (1%);
    - F = 1 mirror reflecting a rect light L = 3 → 3.0 (0.5%);
    - light `specular = 0` removes only the highlight;
  - determinism with multi-material tiles vs full frame;
  - radial albedo probes per lobe vs `tests/data/albedo_ref.json` (0.3% abs; generated by the independent numpy `gen_albedo_ref.py`).
- **Risk:** this is the hot path. MIS at area-light and escape hits after delta bounces; NEE side tests.

### M3.4: IR, compiler, VM, core primvars, noise (L, ~3 500 LOC, medium risk)
- `ir.h`, `ops.h` (own polynomial sin/cos/exp/log/pow, ≤ 2 ulp), `noise.h` and `simd_noise.cpp` (bit-exact MaterialX ports from `m3research/mtlx-nodes/cpp/mx_noise.h`), `compiler.cpp` (passes: fold via `runLane` → identities that are exact for `mix = bg(1−t)+fg·t` → CSE → fold → DCE → uniform hoisting into the prologue → derivative-cone cloning → Sethi-Ullman → first-fit rows ≤ 256 → emit + hash), `vm_lane.h`, `vm_batch.cpp`, `shade_context.h`.
- Mesh primvar channels and the `Primvar` op with remaps; Karma tangent frame globals; nvcc coverage of `program.h`, `ops.h`, `noise.h`, `vm_lane.h`.
- **Tests:**
  - `test_shading_compiler.cpp`:
    - folding (`2·3 + P.x` → Extract + Add with uniform 6);
    - identities, CSE, DCE counts;
    - row bounds (400-op chain ≤ globals + 3; balanced depth-7 tree ≤ 9);
    - hoisting (param-only subgraph → 0 varying instructions; a parameter edit keeps the hash and updates the prologue; an op change changes the hash);
    - compile time < 1 ms for a 3-texture Standard Surface IR.
  - `test_shading_vm.cpp`:
    - 3000 seeded random DAGs × 45 lanes, **bit-exact** across a tree-walking reference of the unoptimised IR, `runLane`, `runBatch<8/16/32>` with partial chunks, and every optimisation toggled off;
    - edge values (±0, denormals, ±inf, NaN, 1e±30);
    - goldens from `tests/data/mtlx_golden/*.json`, generated offline by `gen_vex_golden.py` through `vexexec -A 32` with `#include <mxstd.h>` (black-box; tolerance: Perlin/fBm ≤ 2 ulp, cell/Worley exact).
  - Core render test: a procedural checker and noise3d (Pobject) material vs analytic pixel values.
  - Microbench (label `bench`, non-gating): ns per instruction·lane at N = 8/16/32 (targets ≤ 0.25 at N = 32), noise ≤ 8 ns, sort ≤ 1 ns/hit.
- **Risk:** MSVC vectorisation (use ternary min/max only, own transcendentals, check `/Qvec-report:2` on `vm_batch.cpp`).

### M3.5: Hydra materials (L, ~3 200 LOC, high risk)
- `hydra/material/*`, `Nova::MaterialX` CMake targets, contexts, reading, translation, registry, cache, IPR diffing.
- Mesh slots, GeomSubsets, generic primvars including subdivision refinement and instance channels; primvar-only edit path; drop `SurfaceHit::color`, `cornerColors` and `displayColor` from core; parallel material sync; nova_cli library-path parity; frame and fps from the host (verify the husk token in this step).
- **Tests:**
  - `test_hydra_materials.cpp` (translator fed with `HdMaterialNetwork2` copies of the probe logs):
    - pseudo-params stripped, relative paths, empty terminal output fixed;
    - Standard Surface 1.0.1 defaults 1.0 / 0.8; `ND_normalmap` rename; USD aliases;
    - primvar extraction (`st` from texcoord and from an unconnected image texcoord, `mask` from geompropvalue, varname from UsdPrimvarReader);
    - unknown `kma_*` → warning + defaults; unknown terminal → next context.
  - Structure hash: a value change keeps the hash; renaming nodes shares the program; a connection, geomprop or enum change gives a new hash.
  - IPR driver (port of `probe/driver.cpp`):
    - a value edit hits the cache with no mesh sync;
    - a geomprop rename changes the image without a mesh sync;
    - a disconnected-node edit leaves the edit version unchanged;
    - a rebind yields only `DirtyMaterialId`;
    - a subset rebind does not rebuild the BLAS (debug counter);
    - material removal → default.
  - `husk_materials` (`check_materials.py`), white dome, direct only, top-down:
    - context priority exactly (1, 0, 0);
    - Lambert calibration exactly C;
    - all-defaults Standard Surface ≈ Karma 0.8089 (±0.01);
    - UsdPreviewSurface (0.5139, 0.2733, 0.1530) / 0.2062 (±0.02);
    - `kma`-only material picked up;
    - GeomSubset two-material mesh; interface-promoted parameter; `full` vs `preview` binding;
    - geompropvalue of an instancer primvar;
    - unbound mesh = Lambert(displayColor) (Q1).
  - All 14 M2 reference scenes stay green: they now go through `displayColorLambert`.
- **Risk:** Hydra 2 quirks, MaterialX DLL and library resolution in nova_cli, subdivision face-varying refinement.

### M3.6: Textures (L, ~2 400 LOC, medium-high risk)
- `core/texture/*`, ray-cone state and camera cones, the `Tex2D` op with shifted cones, `kTexture` stream, `hydra/textureSystem`, `textureConvert` and `colorSpace`; light textures switch to the shared color-space rule; settings; stats.
- **Tests:**
  - `test_textures_core.cpp` (MemorySampler, no OIIO):
    - camera spread vs finite differences; orthographic width = pixel and spread 0;
    - footprint major axis W/cosθ and minor W at 0/60/85°;
    - uvGradients invariance; sphere curvature ≈ 1/R;
    - wrap modes on a 4×1 ramp.
  - Hydra level (OIIO):
    - MIP level 2.00 / 4.0 / ≈0 at 16 / 64 / 4 texels per pixel with width 0.25;
    - wrap modes at s = −0.1 / 1.1 / 1.6 with closest filtering: Clamp 1, 4, 4; Periodic 4, 1, 3; Mirror 1, 4, 2; Black 0; Constant → default;
    - UDIM 1001 red / 1002 green / 1011 white top and blue bottom / 1012 missing → default, u < 0 → default;
    - sRGB code 10 → 0.003035 ± 1e-5 and 128 → 0.215861 ± 1e-4 (untiled PNG and uint8 tx);
    - 50% sRGB checker at a coarse footprint → 0.5 (not 0.214);
    - ACEScg EXR → M·c; grey → RGB replicated; RGBA PNG (1, 1, 1, 0.5) → 0.502;
    - missing file → default plus exactly one stderr line; `.rat` → 0.2158.
  - `husk_textures` extended to `mtlximage` on surfaces (png, jpg, uint8 tx, half EXR, `*ACEScg*`).
  - `stat:bytes_read` with cones ≪ without (8k texture in a diffuse room).
- **Risk:** OIIO pitfalls (§4.6), concurrent conversion-cache writes, perf of anisotropic lookups.

### M3.7: Normal maps, bump, tangents, anisotropy (M, ~800 LOC, medium risk)
- `NormalMap` and `HeightToNormal` ops (Karma semantics); the T/B globals from st (degenerate → `makebasis`, no st → first-edge parameterisation); coat normal and tangent; rotation; `ensureValidSpecularReflection` wiring.
- **Tests:**
  - a flat normal map (0.5, 0.5, 1) is bit-identical to none;
  - a tilted constant normal map gives `cos∠(L, N')·E/π` under a distant light;
  - bump of `h = a·u` on a plane gives an analytic tilt;
  - anisotropic highlight direction flips with swapped st (vs Karma in `mtl_metal`);
  - NaN hunt with normals on the far side of Ng.

### M3.8: Opacity (M, ~700 LOC, medium risk)
- Opacity-only programs, `RayBatch` seeds, Embree argument filters (`RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS`, per-geometry enable at commit), first-hit AOV seeds, UsdPreviewSurface `opacityThreshold`.
- **Tests:**
  - opacity 0.5 white sphere, invisible dome: color 0.75 and alpha 0.75 (3e-3; Karma 0.7501 / 0.750);
  - a cutout texture leaf vs Karma (`mtl_diffuse`);
  - stochastic shadow transmittance = 1 − o (1%);
  - meshes without opacity: rays/s unchanged (perf assert ±3%).

### M3.9: Reference scenes, performance, Solaris check, docs (M, ~1 200 LOC scenes and scripts, medium risk)
- `nova_imgcmp --region name,cx,cy,r,relTol` (repeatable; region-mean test with Šidák correction over regions × channels; JSON diagnosis).
- `gen_scenes.py` `material()` helper; new `common_mtl.usda` and `assets/sphere_st.usda`.
- 7 reference scenes (§6.3); Karma references made with `make_references.py`; `karma_albedo_probe.py`.
- Performance pass: tile size 32 vs 64 for batch coherence; ≤ 1.35× wall-time target (§2.5); IPR latency measurement.
- Solaris check via the fxhoudini MCP: a Karma Material Builder (`mtlxstandard_surface` + `mtlximage` + normal map) in the Nova viewport; parameter tweak restart time; screenshot.
- README and docs update.

---

## 6. Test strategy

### 6.1 Layers

| Layer | What it proves | Runtime (wall, -j4) |
|---|---|---|
| Analytic and statistical unit tests (U1 lobes and closures, U2 material furnaces, U3 VM/compiler, U4 textures) | Exactness of every lobe, mixture, layering, op and texture rule | ~5 s |
| Hydra unit and IPR tests | Translation, hash, edit paths, bindings | ~2 s |
| husk / cli checks (`husk_materials`, extended `husk_textures`) | End-to-end through husk and nova_cli, Karma calibration values | ~6 s |
| Reference scenes (husk + cli each) | Translation plus integration against Karma CPU | ~6 s (7 scenes × 2 × ~2.6 s, parallel) |
| Benchmarks (label `bench`, non-gating) | Performance targets of §2.5 | manual |

The suite grows by about **+19 s wall** (from ≈ 30 s). Offline Karma reference generation: 6 scenes × 2 halves × ~35 s ≈ **7 min** (needs a Karma license; regeneration only).

### 6.2 Tolerance policy
- **Analytic tier:** `block_rel_tol 0`, `global_rel_tol ≤ 5e-4`.
- **Karma-agreeing tier:** ≤ 0.02, justified by the measured Karma-vs-exact table.
- **Tolerant tier:** per region, ≤ 1.5 × the measured deviation, capped at 0.10.
- Every tolerance above 0.01 carries a `reason` in `manifest.json`, and each scene keeps a strict test on its unflagged regions. Regions are circles computed analytically from the camera, eroded by 1.5 px.

### 6.3 New reference scenes

All 192×96, floating spheres (no diffuse receivers of specular light), `common_mtl.usda` settings class (Karma limits 64, caustics on, `constrainmaxrough` 0, `thinwallatrefractlimit` 0, `internalreflectenable` 1, color limits 1e9, uniform oracle). Test at 64 spp × 2 seeds; reference `spp_half` 4096.

| Scene | Content | Lighting | Compare |
|---|---|---|---|
| `furnace_materials` | 16 spheres that are energy-conserving in Karma: Lambert white, EON r1, plastic r0.2/0.6, SS coat r0/0.3, OpenPBR coat r0, sheen and fuzz over white, smooth, IOR-1 and thin glass, translucent, F = 1 metal r0, white plastic r0.4 | Constant dome L = 1, light geometry visible | Constant 1, `karma_check`, global 5e-4 |
| `mtl_diffuse` | Lambert colors, EON r0.5/r1, opacity 0.4 cutout, cutout leaf texture, emission sphere, GeomSubset two-material sphere, UsdPreviewSurface | Dome 0.35 + key rect + rim rect | Strict 0.01; preview sphere region 0.02 |
| `mtl_metal` | SS metal r0/0.2/0.4/0.6 (silver, copper, gold), OpenPBR F82 r0.3, anisotropy 0.8 with st and swapped st | Same | 0.02 |
| `mtl_glass` | Smooth glass 1.5 / 1.33, tinted (depth 0), absorption (depth 2), thin-walled, IOR 1, r0.15, in front of an emissive checker backdrop | Dome + key rect | 0.02 |
| `mtl_layered` | Plastic r0.2/0.5 on bright bases, SS coat incl. affect_color, SS sheen, OpenPBR fuzz, OpenPBR coat η 1.6 + darkening, SS QON r1, all-defaults SS and OpenPBR | Dome + soft rect off the mirror direction | 0.03; regions: sheen 0.10, QON 0.06, highlights 0.10 |
| `mtl_textured` | Tilted plane + spheres: UDIM base_color, raw roughness map, normal map, tiledimage/place2d, minified checker `.tx` at grazing, 8-bit sRGB PNG and saturated ACEScg EXR | Dome + rect | Strict 0.01 (block means do not depend on the filter) |
| `mtl_nodes` | Unlit quad grid of Tier-1 nodes: noise2d/3d, fractal, cell, Worley (incl. solid), ramps and splits (orientation, no clamp), smoothstep edge cases, dodge/burn, hsv, rotate, safe math, geompropvalue fallback, Pobject on PointInstancer and native instances | Unlit, pixelfilter point | Strict 0.005 |

Thin film, rough glass r ≥ 0.3, metals r ≥ 0.8 and subsurface are tested only analytically (§3.7). `common.usda` and `assets/sphere.usda` are never edited, because that would invalidate all 14 M2 reference hashes.

### 6.4 Tooling extensions under `tests/reference`
- `tools/gen_scenes.py`: `material(name, shader_id, inputs)`, `sphere_st` asset, region circles written into the manifest.
- `tools/check_references.py`: forwards `compare.regions`.
- `tools/gen_albedo_ref.py`: independent numpy implementation of the lobe formulas and compensation → `tests/data/albedo_ref.json`.
- `tools/gen_vex_golden.py`: `vexexec` tables on exactly representable inputs (`k/128 + 1/512`) → `tests/data/mtlx_golden/*.json`.
- `tools/karma_albedo_probe.py`: re-measures the Karma deviation table when the Karma version changes (recorded in `images/<scene>.json`).

---

## 7. Risks and open questions

### 7.1 Risks

| Risk | Mitigation |
|---|---|
| Batch fragmentation at deep bounces (1024-path waves, many materials) | Adaptive N (8/16/32, per-lane for ≤2); varying-free materials skip the VM; measure tile 64×64 in M3.9 |
| MSVC does not vectorise libm and `std::fmin` under `/fp:precise` | Ternary min/max, own polynomials in `ops.h`, AVX2 noise kernels, `/Qvec-report:2` check |
| OIIO lookups dominate shading cost | Stochastic filtering, `anisotropic=8`, constant-texture short-circuit, material-sorted coherence, ray-cone widening after bounces |
| Hydra 2 quirks (AllDirty, no mesh re-sync, empty primvar lists, terminal output names) | Scene-index reader, own diffing, upload of all primvars, IPR driver test |
| MaterialX library differences between husk and nova_cli | Own search-path resolution from `MaterialXCore.dll`; test in both `husk_materials` and cli references |
| Memory from uploading all primvars | Accepted for M3; filtering to the union of material needs in M7 |
| Karma differences make reference tests fragile | Exactness from analytic tests; tiers and reasoned regions; re-measure tool |
| Embree filter callbacks (opacity) cost and instancing | Enabled only on flagged geometry; deterministic per-hit hash; perf assert in M3.8 |
| ACES configs (scene_linear = ACEScg): MaterialX constant colors are taken as scene_linear | Documented; one saturated-color test under an ACES config in M3.6; revisit if Karma converts document colorspaces |
| Program row cap 256 for very wide graphs | Compile error → next context or default material + warning; counted in tests over all 198 stdlib graphs expanded |
| SideFX proprietary headers | Behaviour only; implement from MaterialX (Apache-2.0), OpenPBR, pbrt and Cycles sources |

### 7.2 Technical open items (resolved during implementation)
- The exact render-setting token husk uses for the frame and fps (M3.5).
- The USD encoding of Houdini `mtlximage` `filecolorspace` (verify with an fxhoudini LOP test in M3.6).
- Whether the tangent fallback without st (first triangle edge) matches Karma's polygon parameterisation on quads. Test with st only until M4 (MikkT).

### 7.3 Questions only the user can decide
- **Q1. Default material for unbound geometry.** Keep Lambert(displayColor, 0.18 default), which today's tests and the M2 references assume, or match Karma's measured internal fallback (0.401·C + 0.033)? **Recommendation: keep Lambert.**
- **Q2. Physically correct vs Karma parity where they differ** (SS sheen shape, thin-film color cast, Schlick-fit dielectric spec, uncompensated rough glass). **Recommendation:** follow MaterialX/OpenPBR as planned, and add opt-in compat switches only if look-matching against Karma becomes a production need.
- **Q3. Displacement milestone.** It is not in PLAN.md. **Recommendation: M4**, with the material workflow.
- **Q4. Dispersion.** It needs spectral rendering, which has no milestone in PLAN.md. When should it be scheduled?
- **Q5. Transparent ("fake caustic") shadows.** Karma enables them by default, so thin glass and windows let light through in shadow rays. M3 renders only physically path-traced transmission, which is noisy for windows. Should Nova add a Karma-like transparent-shadow option (proposed for M7, IPR look-dev), and should it default on like Karma?
- **Q6. Texture auto-conversion cache.** Write converted textures to `$HOUDINI_TEMP_DIR/nova_tx` (recommended; never touches asset directories), or next to the source like Karma's `.rat` auto-conversion?
- **Q7. UsdPreviewSurface native in M3** (D4, pulled forward from M4): approve?
- **Q8. Per-ray-type depth limits** (Karma diffuse/reflect/refract/SSS limits): approve moving them into M4 with the Render Settings LOP (D8)?