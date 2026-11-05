# Nova M3 implementation plan: shading core (ShaderGraph IR, bytecode VM, OpenPBR / Standard Surface, textures)

Inputs used: `docs/PLAN.md`, `README.md`, all 7 M3 research reports (`docs/research/m3/*.md`, identical to the scratch copies), and the current code: `core/integrator/wavefront.*`, `renderer.cpp`, `core/scene/*`, `core/math/sobol.h`, `core/lights/lights.h`, `core/accel/*`, `core/geometry/subdiv.h`, `hydra/mesh.cpp`, `renderDelegate.cpp`, `renderParam.h`, `textureCache.cpp`, the CMake files, `tests/CMakeLists.txt` and `tests/reference/*`.

**Milestone goal (PLAN.md M3):** "MaterialX-Standard-Surface aus Solaris rendert korrekt" (a MaterialX Standard Surface from Solaris renders correctly). In this plan, "correct" means three things:
- The network renders through Nova's own IR and VM.
- It matches Karma CPU wherever Karma follows the MaterialX definition.
- It is analytically exact where Karma deviates, and every such deviation is documented and tolerated per region in the tests.

---

## 0. Decisions at a glance

| # | Decision | Why (short) |
|---|---|---|
| D1 | **Native uber closure** for Standard Surface 1.0.1, OpenPBR 1.1.1, `surface_unlit` and (M3.11) UsdPreviewSurface. The terminal inputs bind to a canonical `SurfaceInputs` block. Generic pbrlib BSDF graphs come in M4. | Lobe-exact sampling, a fixed ≤8-lobe POD closure, the same code on the GPU (Cycles SVM pattern). |
| D2 | BSDF physics: **MaterialX graph semantics + exact per-microfacet Fresnel + LUT energy compensation.** Known Karma deviations are documented, not copied. | Karma uses fits, Schlick, sheen as Lambert and an sRGB thin-film matrix. Exactness comes from analytic tests; Karma comparisons check translation and integration. |
| D3 | stdlib node semantics: **Karma's** (VEX safe math, ramptb/splittb orientation, dodge/burn, `constant` address mode = black, heighttonormal as world-space bump). No "spec" switch in M3; one flag bit per instruction stays reserved. | PLAN.md names Karma CPU as the material reference. Karma expands the same stdlib nodegraphs, so expanded graphs match bit for bit. |
| D4 | The network is **read from the terminal scene index** (`HdMaterialSchema`), with a `GetMaterialResource` fallback. Context chain per material: `nova > kma > mtlx > ""` (universal). `vex` is added in M5. | Keeps terminal output names, `colorSpace`/`typeName` and `nodeTypeInfo` (needed in M5). Verified in the probe. |
| D5 | Translation with Houdini's **MaterialX 1.39.5 C++ API**: Nova's own `HdMaterialNetwork2 → mx::Document` with `setDataLibrary` (not hdMtlx), then `flattenSubgraphs(filter)`, then lowering to Nova IR. MaterialX is linked only by `hydra/`. | 0.18 ms per material instead of 18 ms. Handles nodegraph interface defaults and inserts `geomNode_UV0`. hdMtlx mis-types unknown nodes. |
| D6 | VM: **16-byte instructions; registers are rows of floats.** Per-material uniform block. Literals fold, parameters never do. Uniform-only subgraphs are hoisted into a host prologue. The **structure hash** decides program sharing and lets IPR value edits skip recompiling. Two executors share one op set: CPU SoA batch (N = 8/16/32) and a `NOVA_HOST_DEVICE` per-lane executor. | Measured 0.15 ns/(instr·lane) at N=32 vs 3 ns per lane. The prototype is bit-exact across executors. The per-lane executor is the M10 GPU path. |
| D7 | Shade stage: **per-bounce counting sort of hits by MaterialId**, 32-lane chunks, uniform-only materials skip the VM. | 0.6 ns/hit. A 32-lane chunk is one CUDA warp. |
| D8 | Texture footprints: **ray cones** (2 floats per path), plus compile-time **shifted re-evaluation** (forward differences) of the texcoord and height cones. No dual numbers. | Works for every op with no per-op derivative code in two executors. The same mechanism gives bump. Cycles precedent. |
| D9 | Textures: a **private OIIO TextureSystem over the shared ImageCache**. Anything that is not linear, tiled and mipped is converted once to a half-float, linear, tiled, mipped EXR in a disk cache. Linear gamut changes are a 3×3 matrix after the lookup. UDIM follows Karma's semantics. `stochastic=3` with a Sobol stream. Footprint = 0.25 × pixel (Karma, measured). | `colortransformid` is broken for untiled and automip inputs and quantizes 8-bit data (measured). Filtering encoded sRGB is biased by up to −57%. |
| D10 | Opacity: **Embree argument filter functions** (intersect and occluded) run an opacity-only program per candidate hit, with hash-based stochastic acceptance. | No per-path depth rework in the integrator. Shadow rays are correct. Maps 1:1 to OptiX any-hit in M10. |
| D11 | Primvars: **all numeric primvars are stored on meshes as generic attributes**, remapped per (mesh slot, material) at commit. GeomSubsets give per-triangle material slots without a BLAS rebuild. | Material edits never re-sync meshes (verified), and `HdMaterialNetwork::primvars` is always empty. |
| D12 | **8 LUTs, about 400 KB**, generated offline by `tools/nova_bsdf_precompute` from the runtime sampling code and committed. | MaterialX analytic fits are off by up to 3.9%. A 32² LUT has 0.002 maximum error (measured). |

### Conflicts between the reports and how they are resolved

| Topic | Report positions | Decision |
|---|---|---|
| Derivatives | shader-vm and textures: dual rows; mtlx-nodes: shifted re-evaluation | Shifted re-evaluation (D8) |
| Batch width | shader-vm: 32 (measured); mtlx-nodes: 256 | 32 |
| Network source | hydra-materials: scene index; mtlx-nodes: `GetMaterialResource` | Scene index + fallback (D4) |
| Nodegraph expansion | hydra-materials: own recursive expansion; mtlx-nodes: `flattenSubgraphs` | `flattenSubgraphs` with `setDataLibrary`. `nodeDefDatabase` only for defaults and metadata. |
| Layering albedo of compensated dielectric R | surface-models: `E_R·(1+F_ms(1−E)/E)`; microfacet: `E_R/E_RT` | `E_R/E_RT`: exact furnace and consistent with glass compensation |
| Dielectric table size | 16³ vs 32³ | 32³ |
| Delta threshold | α < 1e-4 vs max(αx, αy) < 1e-3 | 1e-3 (pbrt `EffectivelySmooth`) |
| Opacity | shade-stage pass-through + occlusion filter vs filter only | Argument filters for both ray kinds (D10) |
| Texture reference in bytecode | immediate (path edit = recompile) vs per-material slot table | Slot table: a path edit is a value edit |
| UsdPreviewSurface | native vs nodegraph expansion | Native surface; UsdUVTexture / UsdPrimvarReader / UsdTransform2d through their nodegraphs |
| Golden values for ops | MaterialX GLSL renderer vs `vexexec` | `vexexec -A 32` with `mxstd.h` (Karma semantics, no license, no OpenGL) + the C++ noise port as bit-exact oracle |
| Airy iterations | MaterialX/Karma 2, Cycles 3 | 2 |

---

## 1. Scope

### 1.1 What M3 delivers

**Shading core (`core/shading`)**
- ShaderGraph IR, optimizer (fold, CSE, DCE, uniform hoisting, Sethi-Ullman scheduling, row allocation), bytecode, CPU batch and per-lane VMs, about 75 opcodes.

**BSDF library (`core/bsdf`)**
- Lambert, QON, EON, translucent, GGX conductor (n,k and F82-tint), dielectric R / T / R+T, thin-walled dielectric, Charlie sheen (MaterialX form), Zeltner fuzz, Airy thin film.
- Energy-compensation LUTs.
- Closure list with one-sample MIS.
- Standard Surface 1.0.1 and OpenPBR 1.1.1: every parameter except those listed in 1.2.
- Emission; stochastic opacity (cutout and fractional) in camera, indirect and shadow rays; Beer absorption (`transmission_depth`); the UsdLux `specular` light multiplier.

**Integrator**
- Class-aware NEE (reflection and transmission) and MIS with delta lobes.
- Ray offsets per side, `etaScale` in Russian roulette, emission at hits, closure albedo AOV, ray cones.

**Hydra**
- `HdNovaMaterial` sprim, context chain including `kma`, scene-index network reading, MaterialX expansion, Karma-semantics lowering.
- Generic primvars (meshes, subdivision surfaces, instancers), GeomSubset material slots.
- IPR with structure-hash program sharing, and value edits without recompiling.

**Textures**
- OIIO TextureSystem, auto-conversion to tx/EXR, `.rat` through `imaketx`, UDIM (`<UDIM>`, `<UVTILE>` and OIIO's patterns), address modes, filters.
- Color spaces (OCIO rules + Karma's 8-bit rule + `colorSpace:file` metadata).
- Ray-cone mip selection with Karma parity, stochastic filtering.
- Normal maps (Karma variant), bump/heighttonormal, Karma tangent frame from `st`, anisotropy rotation.

**Nodes**
- All 495 stdlib primitives except the matrix family listed in 1.2 are native (noise family bit-exact to MaterialX and Karma).
- All 198 stdlib nodegraphs plus Houdini's `hmtlx*` graphs work through expansion.

**Tests and tooling**
- Chi² / furnace / albedo harness, VM differential tests, a Hydra IPR driver test, 8 new reference scenes (7 against Karma, 1 analytic), and region tolerances in `nova_imgcmp`.

### 1.2 Deferred

| Item | Milestone | Note |
|---|---|---|
| Generic pbrlib BSDF graphs (`layer`/`mix`/`add`/`multiply` of lobes, `ND_mix_surfaceshader`, kMaxLobes 16) | **M4** | Same lobe library; only a compile-time flattening pass and the `LobeList` model are new. M3 warns and falls back to the next context, then to the default material. |
| Karma/Houdini VOP mapping tables (`kma_*` natives such as `kma_rayswitch`, `kma_ao`, `kma_curvature`), Nova Material Builder HDAs, `UsdRenderers.json` UI for the new settings | **M4** | M3 warns once per node type and uses the node's default output. |
| Per-ray-type depth limits (diffuse / reflect / refract), fake caustics or transparent shadows (see Q7) | **M4** (render settings) | M3 keeps `nova:maxDepth`. Glass blocks shadow rays (Karma reference settings use `causticsenable=1`). |
| `vex` render context, CVEX fallback node | **M5** | Opcode `External` is reserved. |
| `aov:*` terminal inputs, LPE labels, bump-aware N AOV | **M6** | Lobe class flags exist from M3.2. |
| Parallel material sync, primvar filtering to the union of used names, file watching for textures, NVRTC specialization | **M7** | |
| `transmission_scatter` and its anisotropy (interior scattering medium), nested dielectrics | **M8** | M3 uses absorption only (σa = μt, as Karma does). |
| SSS random walk; light sampling of emissive meshes (MIS) | **M9** | M3 uses the SSS fallback `Lambert(subsurface_color)` (OpenPBR spec "Flexibility"). Emission is BSDF-sampled only, with MIS weight 1. |
| Matrix ops (`transformmatrix`, `creatematrix`, `transpose`, `determinant`, `invertmatrix`), `hextiledimage`/`hextilednormalmap`, `flake2d/3d` | **M4** | Rare in Houdini materials. |
| Dispersion (`transmission_dispersion`, OpenPBR `dispersion_*`) | **open (Q3)** | Needs spectral sampling (`stream::kWavelength` is reserved). M3 warns once. |
| Displacement terminal | **open (Q3)** | The terminal is parsed and ignored with one warning. |
| MikkTSpace tangents (`karma_mikkt`, `tangentu`/`signs`) | **M4** | M3 uses Karma's default st-derived frame. |

### 1.3 Explicit deviations from PLAN.md

1. **"OpenPBR/Standard Surface komplett" is not fully reached in M3.**
   - Dispersion and displacement are not in M3; PLAN.md has no milestone for either (Q3).
   - `transmission_scatter` moves to M8 (volumes).
   - SSS random walk stays in M9, as PLAN.md already says.
   - Reason: each needs infrastructure (spectral sampling, media, tessellation-time shading) that PLAN.md places later or not at all.
2. **UsdPreviewSurface is supported natively in M3.11**, although PLAN.md lists node mappings under M4. It is small, and the universal network is the only safety net for VEX-builder materials before M5 and for USD assets from other DCCs.
3. **Karma semantics for stdlib ops, without a MaterialX-spec switch.** PLAN.md says Karma CPU is the material reference. MaterialX 1.39.5 itself flipped `ramptb`/`splittb`, so the two can't both be followed.
4. **BSDF physics deliberately differs from Karma in documented places** (D2, §3.7). PLAN.md asks to "compare with Karma"; Nova compares and tolerates instead of copying, partly because `mxpbr.h`/`ggx_utils.h` are SideFX-proprietary.
5. **Opacity uses Embree argument filters instead of shade-stage pass-through** (D10). Not a PLAN.md item, but a deliberate deviation from the research recommendation.
6. **Light `specular` multiplier.** Nova ignored it so far; it is added in M3.2 because lobes now carry a diffuse/specular class.
7. **Texture auto-conversion writes files to disk** (default `$HOUDINI_TEMP_DIR/nova_tx`). PLAN.md says nothing about this (Q4).

---

## 2. Architecture

### 2.1 Data flow

```
Solaris / husk / nova_cli
  UsdImaging scene index ── material sprim ──> HdNovaMaterial::Sync
     networkSource:  HdMaterialSchema (terminal SI), ctx chain nova>kma>mtlx>""  [fallback GetMaterialResource]
     mtlxDocument:   HdMaterialNetwork2 -> mx::Document(setDataLibrary) -> flattenSubgraphs(native filter)
     lower:          mx graph -> nova::shading::IrGraph (+ texture requests, primvar names, params)
     registry:       structureHash -> ProgramCache (shared Program), TextureSystem handles, AttributeIds
     Scene::updateMaterial(MaterialId, MaterialDesc)          (render stopped only here)
  HdNovaMesh::Sync ── attributes (all numeric primvars), material slots + per-triangle slot (GeomSubsets)
  Scene::commit ── packs Program/Material tables, runs prologues, builds SurfaceInputs templates,
                   attribute remaps per (mesh, slot), Embree filter state
Renderer / renderWave (per tile, per thread)
  Generate -> Intersect (Embree, opacity filter) -> Shade:
     pass 1  lights/misses/escapes (unchanged), Beer absorption, collect hits
     pass 2  counting sort by MaterialId
     pass 3  per material, 32-lane chunks: ShadingPoint -> global rows -> VM -> gather SurfaceInputs
             -> buildClosures -> emission, NEE (shadow queue), BSDF sample, RR, cone update
  -> Shadow (Embree occluded, opacity filter)
```

### 2.2 File layout (new = N, changed = C)

```
core/
  bsdf/      N bsdf_common.h  microfacet.h  fresnel.h  thin_film.h  energy.h  dielectric.h  conductor.h
             N diffuse.h  sheen.h  shading_frame.h  closure.h  surface.h  surface.cpp
             N tables/bsdf_tables.h  tables/bsdf_tables.cpp (generated, committed)
             C lambert.h (deleted at the end of M3.4; LambertBsdf survives as LobeType::Lambert)
  shading/   N ir.h ir.cpp  compiler.h compiler.cpp  program.h  ops.h  mx_noise.h  simd8.h
             N vm_lane.h  vm_batch.h vm_batch.cpp  shade_context.h  default_material.cpp
  texture/   N texture.h  ray_cone.h  memory_sampler.h memory_sampler.cpp
  scene/     N material.h  attributes.h      C scene.h scene.cpp mesh.h light.h camera.h (rayCone)
  geometry/  C subdiv.h subdiv.cpp (generic primvars, base-face id per triangle)
  integrator/N material_eval.h material_eval.cpp (sort, VM dispatch, gather; shared by shade + first-hit AOVs)
             C wavefront.h wavefront.cpp renderer.cpp settings.h
  accel/     C accel.h (HitFilter) embree_accel.cpp (argument filters)
  lights/    C lights.h lights.cpp (delta-aware bsdfSampleWeight, diffuse/specular multipliers)
hydra/
  material/  N material.{h,cpp} networkSource.{h,cpp} nodeDefDatabase.{h,cpp} mtlxDocument.{h,cpp}
             N lower.{h,cpp} surfaceMap.h usdAliases.h registry.{h,cpp}
  N textureSystem.{h,cpp} textureConvert.{h,cpp} colorSpace.{h,cpp}
  C textureCache.cpp (uses colorSpace) mesh.{h,cpp} meshSubdiv.cpp instancer.{h,cpp} light.cpp
  C renderDelegate.{h,cpp} renderParam.h renderPass.cpp CMakeLists.txt
cmake/       C NovaHoudini.cmake (Nova::MaterialX imported targets: MaterialXCore, MaterialXFormat)
tools/       N nova_bsdf_precompute/   C nova_imgcmp (--region)
tests/       N bsdf_stats.h test_bsdf.cpp test_closures.cpp test_material_furnace.cpp test_shading_compiler.cpp
             N test_shading_vm.cpp test_mtlx_ops.cpp test_textures.cpp test_hydra_materials.cpp
             N data/albedo_ref.json data/mtlx_golden/*.json husk/check_materials.py
             N reference/scenes/{common_mtl.usda, assets/sphere_st.usda, furnace_materials, mtl_*}.usda
             N reference/tools/{gen_albedo_ref.py, karma_albedo_probe.py, gen_mtlx_golden.py}
             C reference/manifest.json, tools/{gen_scenes.py, check_references.py}, husk/check_textures.py
docs/design/ N M3.md (this plan, condensed; per docs/research/m3/README.md the design document wins)
```

`nova_core` stays free of USD, HDK, OIIO and MaterialX.

### 2.3 Key data structures

**IR** (`core/shading/ir.h`; no MaterialX types):

```cpp
namespace nova::shading {
enum class Type : uint8_t { Float, Int, Bool, Vec2, Vec3, Color3, Vec4, Color4, Mat33, Mat44 }; // widths 1,1,1,2,3,3,4,4,9,16
struct Ref { enum Kind : uint8_t { None, Node, Const, Param, Global } kind = None; uint8_t output = 0; uint32_t index = 0; };
struct IrNode  { Op op; Type type; uint32_t imm; Ref in[3]; std::vector<Ref> ext; };   // ext: >3 operands
struct IrParam { std::string key; Type type; float value[16]; };   // "shaderPath.input"; authored value or nodedef default
struct IrOutput{ uint16_t field; Type type; Ref value; };          // SurfaceField id, or kOpacityOutput / kEmissionOutput
struct IrGraph {
    std::vector<IrNode> nodes; std::vector<float> consts; std::vector<IrParam> params;
    std::vector<IrOutput> outputs; std::vector<std::string> primvars;  // primvar slot -> name (structural)
    uint16_t textureSlots = 0;                                         // Tex2D imm indexes the material's slot table
    SurfaceModel model = SurfaceModel::StandardSurface;
};
uint64_t structureHash(const IrGraph &);  // ops, types, imms, wiring, consts, primvar names, slot count; NOT param values
}
```

**Program and bytecode** (`core/shading/program.h`, POD, uploadable as is):

```cpp
struct Instr { Op op /*u16*/; uint8_t width; uint8_t flags; uint16_t dst, a, b, c; uint32_t imm; };  // 16 B
static_assert(sizeof(Instr) == 16);
// flags: kUniformA/B/C (operand is a uniform slot), kSplatA/B/C (scalar broadcast), bit6 reserved (spec semantics)
// ops with >3 operands are followed by extension words {uint16 operand; uint16 flags}
enum class Global : uint8_t { P, Pobject, N, Ng, Nobject, T, B, I, UV0, Time, Frame, RayDepth, PrimId, InstanceId,
                              Count };   // N is NOT face-forwarded (MaterialX semantics)
struct OutputBinding { uint16_t field; uint8_t width; uint8_t uniform; uint16_t index; uint16_t pad; };
struct UniformInit { uint8_t kind /*Zero, Const, Param, Hoisted*/; uint8_t pad; uint16_t width; uint32_t src; };
struct Program {                       // immutable, shared by structureHash (shared_ptr<const Program>)
    uint64_t structureHash;
    std::vector<Instr> code;           // varying, ends with End
    std::vector<Instr> prologue;       // uniform-only; run on the host (lane executor) after every param edit
    std::vector<Instr> opacityCode;    // DCE'd to the opacity output; empty = opaque
    std::vector<OutputBinding> bindings;
    std::vector<UniformInit> uniformInit;  // layout: 4 zero slots | consts | params | hoisted
    std::vector<float> constValues;
    uint16_t uniformCount, paramCount, numRows, textureSlots;
    uint32_t globalMask; std::array<uint16_t, size_t(Global::Count)> globalRow;
    std::vector<std::string> primvarNames;
    SurfaceModel model; uint32_t flags;    // kVaryingFree, kEmissive, kHasOpacity, kTransmissive, kNeedsTangents, kNeedsDerivatives
};
Program compile(const IrGraph &, const CompileOptions &);   // CompileOptions{ maxRows = 1024 (CPU) | 256 (GPU), ... }
```

**Op set (M3, about 75):**
- Control: `End`, `JumpIfUniform`, `JumpIfAllZero`.
- Component-wise (width 1–4, Karma safe semantics): `Mov Add Sub Mul Div Mod Mad Mix(bg(1−t)+fg·t) Clamp Min Max Abs Sign Floor Ceil Round(floor(x+.5)) Fract Sqrt`.
- Transcendental, as own vectorizable polynomials: `Pow Exp Ln Sin Cos Tan Asin Acos Atan2`.
- `Smoothstep` (Karma form).
- Compare and logic: `Gt Ge Eq Select And Or Not`.
- Shape: `Dot Cross Normalize(0→0) Extract Combine2/3/4 Swizzle`.
- `Rotate2D Rotate3D`, `RgbToHsv HsvToRgb`, `MatVec`, `TransformSpace(object↔world)`.
- `Primvar(slot,w,default)`.
- Noise: `Noise(dim,outW) Fractal CellNoise Worley(style)`.
- `RampLR RampTB SplitLR SplitTB` (Karma orientation, no clamp).
- `Blend(plus|minus|difference|burn|dodge|screen)`, `Premult Unpremult`, `Comp4(over|in|mask|matte|out|disjointover)`.
- `Tex2D(slot, uv, default, outW)`.
- `NormalMap`, `HeightToNormal`.
- Reserved: `External` (M5 CVEX).

There are no closure ops at run time. Closure parameters are outputs routed by `OutputBinding`.

**Material and program records** (`core/scene/material.h`):

```cpp
using MaterialId = uint32_t;                 // 0 = built-in default material (always present)
struct MaterialDesc {                        // host -> Scene
    std::shared_ptr<const shading::Program> program;
    std::vector<float> params;               // program param order
    std::vector<TexSampler> textures;        // per texture slot
    std::vector<AttributeId> attributes;     // per primvar slot
};
struct ProgramRecord {  // kernel view, one per distinct Program
    uint32_t codeOffset, codeCount, opacityOffset, opacityCount, bindingOffset;
    uint16_t bindingCount, numRows; uint32_t globalMask, globalRowOffset;
    uint8_t model, flags; uint16_t textureSlots;
};
struct MaterialRecord { // kernel view, one per material
    uint32_t program, uniformOffset, textureOffset, attributeOffset, templateIndex;
    uint16_t attributeCount, flags;   // templateIndex -> SurfaceInputs with every uniform binding pre-applied
};
// Scene API
MaterialId createMaterial(); void updateMaterial(MaterialId, MaterialDesc); void removeMaterial(MaterialId);
void setMeshMaterials(MeshId, std::vector<MaterialId> slots, std::vector<uint16_t> triangleSlot); // no BLAS change
MaterialId materialOf(MeshId, uint32_t prim) const;
void setTextureSampler(const ITextureSampler *);   // host-owned (HdNovaTextureSystem); null = defaults
void setColorSpace(const ColorSpaceInfo &);        // XYZ->scene_linear matrix (thin film), from OCIO; Rec.709 fallback
```

**`SurfaceInputs`** (`core/bsdf/surface.h`):
- It is the union of both models' raw inputs (the canonical table in surface-models §6.2/§6.6). Fields: `model`, `thinWalled`, base, metalness, specular (+anisotropy, rotation), transmission (color, depth, scatter, extra roughness), subsurface, sheen/fuzz, coat (+darkening, affect_color, affect_roughness), thin film, `emissionWeight` + `emissionColor`, `opacity` (color3), and the vectors `normal`, `coatNormal`, `tangent`, `coatTangent`.
- Model-specific semantics live in `buildStandardSurface` / `buildOpenPBR`, never in IR rewrites. Examples: SS emission = weight·color, OpenPBR emission = max(lum,0)·color; film thickness in nm vs µm; ACEScg luminance for SS opacity.
- `enum class SurfaceField : uint16_t` comes from an X-macro `NOVA_SURFACE_FIELDS(X)` (name, offset, width).
- `hydra/material/surfaceMap.h` maps `(model, mtlx input name) → SurfaceField`.
- Unbound vector fields (`normal`, `tangent`, …) default to the shading frame, so uniform-only materials need no rows at all.
- `SurfaceModel { StandardSurface, OpenPBR, Unlit, UsdPreview, LobeList /*M4, reserved*/ }`.

**Closure** (`core/bsdf/closure.h`):

```cpp
enum class LobeType : uint8_t { Lambert, Translucent, Qon, Eon, ConductorNK, ConductorF82, DielectricR, DielectricT,
                                DielectricRT, ThinDielectric, Charlie, Zeltner };
enum LobeFlags : uint8_t { kDelta = 1, kTransmit = 2, kThinFilm = 4, kClassDiffuse = 8, kClassGlossy = 16, kClassCoat = 32 };
struct Lobe {                      // ~96 B
    Spectrum weight;               // all layering/mix/tint/darkening factors, evaluated at wo
    LobeType type; uint8_t flags, frame /*0 base, 1 coat*/, mode;
    float ax, ay;                  // GGX alphas
    float etaF, etaT;              // Fresnel / refraction eta (equal for OpenPBR 1.1.1; split for 1.2)
    float energy;                  // MS compensation factor or 1/E_RT
    Spectrum c0, c1;               // F0|n|rho ; F82 tint|k|T tint
    float tfThicknessNm, tfIor, tfWeight;
};
struct ClosureList {
    static constexpr int kMaxLobes = 8;            // worst case 7 (fuzz, coat, metal, dielR, dielT, 2x thin SSS / diffuse)
    Lobe lobe[kMaxLobes]; float cdf[kMaxLobes]; Frame frame[2];
    Spectrum emission, albedo, sigmaA;
    uint8_t count, nonDeltaMask, hasTransmission, thinWalled;
};
struct ClosureEval   { Spectrum fcosDiffuse, fcosSpecular; float pdf; };   // f*|cos| per light multiplier class
struct ClosureSample { Vec3f wi; Spectrum fcos; float pdf /* kDeltaPdf = -1 */; uint8_t flags /*class, transmit, delta*/; float alpha; };
NOVA_HOST_DEVICE void buildClosures(const SurfaceInputs &, const ShadingGeom &, Vec3f wo, const BsdfTables &, ClosureList &);
NOVA_HOST_DEVICE ClosureEval evalClosures(const ClosureList &, Vec3f wo, Vec3f wi, Vec3f ngGeom);
NOVA_HOST_DEVICE bool sampleClosures(const ClosureList &, Vec3f wo, Vec3f u /*kBsdf 3D*/, Vec3f ngGeom, ClosureSample &);
```

**Textures** (`core/texture/texture.h`):
- `TextureId`, `TexWrap {Periodic, Clamp, Mirror, Black, Constant}`, `TexFilter {Closest, Linear, Cubic}`.
- `TexSampler {TextureId; wrapS, wrapT; filter; channels; firstChannel; subimage; colorMatrix; width; blur; fallback[4]}`.
- `TexQuery {count; s, t, dsdx, dtdx, dsdy, dtdy, rnd}`.
- `class ITextureSampler { virtual void sample2D(const TexSampler&, const TexQuery&, float *out, uint8_t *status) const = 0; }`: one virtual call per chunk, like `IAccel`.
- `MemorySampler`: a reference implementation for unit tests.

**Attributes and material slots** (`core/scene/attributes.h`):

```cpp
using AttributeId = uint32_t;                          // host registry (TfToken -> id)
enum class AttrRate : uint8_t { Constant, Uniform /*per triangle, expanded on host*/, Vertex, Corner, Instance };
struct MeshAttribute { AttributeId id; AttrRate rate; uint8_t width /*1..4*/; std::vector<float> data; };
// MeshDesc::attributes replaces displayColor / hasDisplayColor / cornerColors.
// MeshRecord::instanceAttributes replaces instanceColors.
// MeshRecord: materialSlots, triangleSlot (empty = slot 0),
//   attrRemap per slot: per primvar slot, >= 0 mesh attribute, <= -2 instance attribute (-2-i), -1 = op default
```

**Path state additions** (`WaveQueues`, per path slot):
- `uint8_t flags` (lastDelta, insideMedium, lastClass: 2 bits).
- `float etaScale`.
- `Spectrum sigmaA`.
- `float coneWidth, coneSpread`.
- `ShadeScratch` (per thread): `hitRay`, `hitKey`, `order`, `groupStart`, `rows` (1024×32 floats, only touched rows used), `ShadingPoint[32]`.

**`ShadingPoint`** (`core/shading/shade_context.h`): `P, Ng, N, Pobj, Nobj, T, B, wo, uv0, bary, dPdx, dPdy, duv0dx, duv0dy, time, t, mesh, prim, instance, frontFacing`.
- Pobject is prototype space for instances; `resolveHit` already interpolates object-space corners.

### 2.4 Execution model in the wavefront integrator

1. **Pass 1** (existing logic, unchanged): camera-visible lights, misses and escapes, area lights along BSDF rays. New pieces:
   - `beta *= exp(-sigmaA·hitT)` for paths inside an absorbing dielectric.
   - The light multiplier now follows `flags.lastClass` (`light.diffuse` or `light.specular`).
   - Surviving hits push `(k, materialOf(mesh, prim))`.
2. **Pass 2:** counting sort by MaterialId (stable, so lanes stay in queue order).
3. **Pass 3:** per material group, chunks of 32 lanes:
   - `resolveHit` → `ShadingPoint`. The Karma tangent frame comes from the hit triangle's `st` corners (`dPdu`, `dPdv`); the fallback is the first triangle edge.
   - Write only the global rows in `globalMask`. Pad lanes [n, 32) with lane n−1.
   - `runMaterial`: `runLane` for n ≤ 2, otherwise `runBatch<8|16|32>`. Skipped entirely for `kVaryingFree`.
   - Per lane: copy the material's `SurfaceInputs` template, patch the varying bindings from rows, normalize the vectors, then `buildClosures`.
   - Emission: `radiance += beta·cl.emission` for camera and BSDF rays (clamped for indirect). The MIS weight is 1 until M9.
   - **NEE:**
     - Pick one light by power (`kLight` 3D, unchanged). The light is sampled from the front-offset origin.
     - Accept `wi` if it is on the reflection side, or on the transmission side when `cl.hasTransmission`. The geometric class is `dot(wi,Ng)·dot(wo,Ng)`.
     - Transmission-side shadow rays start at the back-offset origin, with the distance recomputed to the sampled point.
     - Contribution = `Li·(fcosDiffuse·light.diffuse + fcosSpecular·light.specular)·w/lightPdf`, with the power heuristic against `ClosureEval::pdf` (mixture over non-delta lobes).
     - Lists that contain only delta lobes skip NEE.
   - **BSDF sample:**
     - `u = get3D(bounce(depth, kBsdf))`; z selects the lobe and its remainder drives inner choices (R/T, EON CLTC vs uniform).
     - `beta *= fcos/pdf` (delta: `w·value/p_j`).
     - `q.bsdfPdf = pdf`, or `kDeltaPdf = −1` after a delta lobe. `bsdfSampleWeight` returns 1 for `bsdfPdf < 0` under every strategy.
     - The origin is offset by side.
     - Entering or exiting comes from the authored Ng: `etaScale *= η²` or `/η²`, `insideMedium` and `sigmaA` updated. Thin-walled surfaces never enter.
   - **Russian roulette:** `survive = clamp(maxComponent(beta)·etaScale, 0.05, 0.95)` from `rrStartDepth`, same stream as today.
   - **Cone:** `coneSpread = scatterSpread(spread, W, curvature, n·d, sample.alpha)` (diffuse lobes count as alpha 1, delta lobes as 0).
4. **First-hit AOVs** use the same `material_eval` path (center rays, sample-0 seeds). The albedo AOV becomes `Σ w_j·Ê_j(ωo)`; normal, depth and ids are unchanged.
5. **Opacity** never reaches the shade stage. The Embree filters discard candidates with `u = hash(raySeed, geomID, primID, instID) ≥ opacity`, where `raySeed` comes from `get1D(bounce(depth, kOpacity))`. Camera, indirect, first-hit and shadow rays are all covered, and alpha follows automatically. Filters are installed only when some bound material has `kHasOpacity`.
6. **Determinism:**
   - Sorting changes the processing order but not results: samples depend only on (pixel, index, stream), and each path is written once per stage.
   - Streams: `kBsdf` (3D, already reserved), `kOpacity` (filter seed), `kSpare` (7) is renamed `kTexture` (stochastic texture filtering; per texture `rnd' = fract(rnd + slot·φ)`), `kLayer` stays reserved.
   - A single-lobe Lambert closure draws exactly the M2 sample, because `get3D(..).xy == get2D(..)`, so the default material reproduces M2's noise.
7. **Ray and event classes:** camera, diffuse reflection, glossy reflection, delta reflection, diffuse transmission, glossy transmission, delta transmission, shadow. M3 uses them for the light multipliers and cone spread; M6 LPE and the M4 per-type limits use the same labels.

### 2.5 The same code on CUDA (M10)

- `Program`, `ProgramRecord`, `MaterialRecord`, uniform blocks, `TexSampler` and `BsdfTables` are flat POD arrays, uploaded unchanged.
- Shade kernel per bounce:
  - Key = `materialOf(hit)`, then a CUB radix or counting sort, then one thread per hit runs `runLane` (switch dispatch, uniform within a sorted warp).
  - Rows are local memory, capped at `maxRows = 256`; a failed compile gives a warning and the default material.
  - Uniform operands are broadcast reads.
  - `gatherInputs`, `buildClosures`, NEE and sampling run in the same kernel. The `ClosureList` (≈0.8 KB) never enters the SoA queues.
- `JumpIfAllZero` maps to `__all_sync`. Varying conditions are already `Select`.
- Textures: the same converted EXR files are served by the OptiX Toolkit DemandLoading `tex2DGrad(id, s, t, ddx, ddy, &resident, jitter)`, with the same (s, t, footprint, rnd) inputs. `ITextureSampler` is the CPU host interface only.
- Opacity: an OptiX any-hit program runs `runLane(opacityCode)` with the same hash acceptance.
- Noise: scalar `mx_noise.h` is `NOVA_HOST_DEVICE`. The AVX2 kernels are CPU specializations tested bit-exact against it.
- Tables: a pointer struct in the launch parameters (global memory, `__ldg`). They don't fit the 64 KB of constant memory.
- FMA: `ops.h` keeps contraction-insensitive forms. CPU/GPU bit equality is decided in M10 (`--fmad=false` for the shading translation unit if required).

---

## 3. BSDF model decisions

### 3.1 Lobes (all `NOVA_HOST_DEVICE`, float, switch dispatch)

| Lobe | Used for | Formula / source | Sampling | Albedo / selection estimate |
|---|---|---|---|---|
| Lambert | default material; SS diffuse at σ = 0 | ρ/π | cosine | ρ |
| QON | SS `diffuse_roughness` > 0 | MaterialX `mx_oren_nayar_diffuse`, σ = r (no compensation, graph semantics) | cosine | MaterialX rational fit |
| EON | OpenPBR `base_diffuse_roughness` | Portsmouth/Kutz/Hill 2024, listings 1–4 (MIT); ρ ≤ 1 in ρ_ms | CLTC + uniform, defensive P_u | analytic Ê |
| Translucent | thin-walled SSS | ρ/π in the lower hemisphere | cosine | ρ |
| SSS fallback | `subsurface` (non-thin) | Lambert(subsurface_color^γ) | cosine | ρ |
| ConductorNK | SS metal | exact conductor Fresnel (Lagarde), Gulbrandsen artistic IOR, GGX with height-correlated Smith in the stable form | bounded VNDF (Eto & Tokuyoshi 2023) | ggxE / Eavg + F_avg from the F82 fit through F(1) and F(1/7) |
| ConductorF82 | OpenPBR metal | Hoffman F82-tint, **clamped ≥ 0**, × specular_weight | bounded VNDF | ABC LUT |
| DielectricR | specular and coat over a base | exact Fresnel per microfacet, tint, energy 1/E_RT | bounded VNDF | E_R/E_RT (LUT) |
| DielectricT | partial transmission; SS split α | Walter 2007 BTDF (1−F), /η² (radiance), normalized 1/E_T | full spherical-cap VNDF | E_T/E_RT |
| DielectricRT | glass (X = 1, no split α) | Walter R+T with tints tR/tT, choice ∝ F·max(tR) vs (1−F)·max(tT), energy 1/E_RT (η > 1) or 1/E_RTinv | full VNDF + Fresnel choice | 1 (energy units) |
| ThinDielectric | thin-walled transmission | sheet R′ = R + T′Rc, T′ = cT²/(1−(Rc)²), c = T_color^{1/cosθt} (Cycles thin glass); T is the mirrored lobe with α_t = sat(α·√(3.4(η−1)(η−0.5)²/η³)) | bounded VNDF | R′, T′ |
| Charlie | SS sheen | **MaterialX form** D/(4(μi+μo−μiμo)), D clamped at r ≥ 0.005; mode 1 = Karma's albedo-weighted Lambert, kept for Q2 | uniform hemisphere | charlieE LUT |
| Zeltner | OpenPBR fuzz | LTC fits (MaterialX `mx_microfacet_sheen.glsl:97-188`); r ∈ [0.01, 1]; R ≥ 0; skip when \|a⁻¹\| < 1e-5 | exact LTC (weight = R·color) | Zeltner R fit |
| Thin film (modifier) | DielectricR, ConductorNK/F82 | Belcour & Barla Airy per microfacet (VdotH), 2 iterations, `F = lerp(F, F_airy, w)`, XYZ → working space normalized so the DC term stays grey; T uses 1 − R_airy | – | MaterialX approximation `lerp(F_airy(μo), E(F_airy(1)), √α)` |

### 3.2 Model assembly

- **Weights.** All layering is albedo scaling at ωo, so every layer factor is a per-hit scalar or color. A closure is a flat weighted lobe list.
  - Standard Surface follows the closed form of surface-models §2.3: untinted layering albedos (measured in Karma), `(1 − C·E_coat)·lerp(1, coat_color, C)`, coat_affect_color/roughness per the graph.
  - OpenPBR follows §3.4: MaterialX coat darkening `K = 1 − (1−F0c)/η_c²` (Karma), constant coat absorption `lerp(1, coat_color, C)`, coat roughening `(r⁴ + 2r_c⁴)^¼`, modulated η with the TIR fix driving both R and T (stored as etaF/etaT).
- **Glass split.** Pure glass is one DielectricRT lobe. Partial transmission or SS `transmission_extra_roughness` ≠ 0 becomes DielectricR + DielectricT with the exact split (microfacet §8), so white configurations stay at 1.
- **Sides.**
  - Opaque closures (no transmission lobe) are two-sided: frames are face-forwarded to ωo.
  - Transmissive closures are oriented by the **authored** Ng. Front: full list. Back: R-only lobes emit nothing, and the RT/T lobe uses 1/η and E_RTinv (TIR inside).
  - Thin-walled closures always act as front.
- **Beer absorption.** `sigmaA = −ln(clamp(transmission_color, 1e-5, 1))/depth` for non-thin closures with depth > 0; the T tint is then 1 (Karma rule).
- **Opacity.** SS uses `luminance(opacity)` with ACEScg coefficients (0.2722287, 0.6740818, 0.0536895), as the graph does. OpenPBR uses `geometry_opacity`.
- **Emission.** Both models use the formulas of surface-models §2.3/§3.3. Nits are treated as radiance 1:1.

### 3.3 Energy compensation and LUTs

| Table | Size | Content |
|---|---|---|
| `ggxE` | 32 r × 32 √μ | E_ss for F = 1 |
| `ggxEavg` | 32 | 2∫Eμ dμ |
| `ggxABC` | 32 × 32 × 3 | exact albedo of any Schlick/F82 Fresnel: F0·A + F90·B − b·C |
| `dielRT`, `dielRTinv`, `dielR` | 32 z × 32 √μ × 32 r each | E_R+E_T for η > 1, the same for 1/η, and single-scatter E_R |
| `fresnelAvgDiel` | 64 z | exact 2∫Fμ dμ (the Kulla-Conty fit is 23% off at η = 1.1) |
| `charlieE` | 32 × 32 | albedo of the MaterialX Charlie form |

- **Axes:** r = (αxαy)^¼, y = √μ, z = ((η−1)/(η+1))^½. Lookups are bilinear or trilinear. Total ≈ 400 KB float32.
- **Rules:**
  - Conductor and F82: `f = f_ss·(1 + F_ms(1−E)/E)` with `F_ms = F_avg·E_avg/(1 − F_avg(1−E_avg))`.
  - Glass: `f/E_RT`.
  - Layered DielectricR: scaled by 1/E_RT, layering albedo `E_R/E_RT`.
  - QON: uncompensated (MaterialX). EON: built in.
  - Rough glass reaches 1, where Karma gives 0.87–0.997.
- **Generator** `tools/nova_bsdf_precompute <table>`:
  - Links only `core/`, calls the runtime `sample` functions, accumulates in double, uses Nova's Owen-scrambled Sobol and TBB.
  - 2²² samples per cell for 2D tables, 2²⁰ for 3D.
  - Writes `core/bsdf/tables/bsdf_tables.cpp` with the parameters and a generator source hash.
  - CMake custom target `nova_bsdf_tables`, outside the default build (minutes of CPU time).

### 3.4 Coverage

| Parameter group | Standard Surface 1.0.1 | OpenPBR 1.1.1 |
|---|---|---|
| base, metalness, specular (+ anisotropy, rotation), coat (all variants), sheen/fuzz, thin film, emission, opacity, normals and tangents | M3 | M3 |
| transmission color/depth, extra roughness (SS), thin-walled glass (spec form; SS beyond the graph, as Karma does) | M3 | M3 |
| subsurface weight/color | M3 (Lambert fallback) | M3 (fallback; thin-walled in spec form, not the graph's C² bug) |
| subsurface radius/scale/anisotropy | M9 | M9 |
| transmission_scatter, scatter_anisotropy | M8 | M8 |
| dispersion | Q3 | Q3 |

Defaults always come from the nodedef via `getActiveInputs()`: SS 1.0.1 has base 1 and base_color 0.8; 1.0.0 has 0.8 and 1.0.

### 3.5 Sampling strategy

- **One-sample MIS over lobes.** `pick_j ∝ luminance(w_j·Ê_j(ωo))`, floored at 1e-3·max so no lobe with weight > 0 gets probability 0. Selection depends only on ωo.
- Lobes return `f·|cos|` in their own frame; coat and base normals differ.
- Mixture pdf = `Σ_nondelta p_k·pdf_k`. A delta lobe's sample is flagged and excluded from the mixture and from NEE.
- Bounded VNDF for reflection-only lobes (62% → 97% valid samples at α 0.8). The full cap for anything that transmits (the bounded cap is biased for refraction).
- Reflect/transmit classification uses Ng; samples whose class disagrees with the lobe's class are rejected.

### 3.6 Robustness rules

- α clamped to [1e-4, 1]; `max(αx, αy) < 1e-3` makes the lobe delta.
- η clamped to [0.01, 100]; |η−1| < 1e-4 makes the surface index-matched (F = 0, straight transmission).
- Colors clamped ≥ 0, weights to [0, 1], multiply tints to [0, 1].
- Clamp F82 and its average, the Zeltner fit, and ρ ≤ 1 in EON ρ_ms.
- Cycles' `ensure_valid_specular_reflection` on every closure frame. Bump shadowing term on diffuse lobes under bent normals.
- Reject a sample if `!(pdf > 0) || !isFinite(fcos)`. μ clamped ≥ 1e-6 in LUT lookups.
- Thin-walled surfaces never refract and never change the medium state.
- `assert(count ≤ kMaxLobes)` in debug builds; release builds drop the lowest-weight lobe.

### 3.7 Deliberate differences from Karma (tolerated in references, each with a `reason`)

| Effect | Size |
|---|---|
| Dielectric R: Karma uses a curve-fit at the macro angle | +4.7% at μ = 1 and α = 0.09; −21% of the specular part at μ = 0.5 (smooth) |
| SS sheen: Karma uses a Lambert shape | furnace equal, lobe shape differs by 30–60% under back light |
| Thin film: Karma's linear-sRGB matrix | R/G cast of 1.27 in Karma |
| Rough glass: uncompensated in Karma | −3% at r 0.3, −34% at r 0.7 |
| Metals at r ≥ 0.7 | Karma −1.5…−8% |
| SS QON: Karma CPU differs from MaterialX (XPU matches) | 3–5% |
| OpenPBR thin-walled SSS: graph C² bug, CPU ≠ XPU | never used as a reference |

A test-only Karma-compatibility closure variant is not built in M3 (Q2).

---

## 4. Hydra side

### 4.1 Delegate

```cpp
SUPPORTED_SPRIM_TYPES += HdPrimTypeTokens->material;          // CreateSprim -> new HdNovaMaterial(id)
TfTokenVector GetMaterialRenderContexts() const { return {TfToken("nova"), TfToken("kma"), TfToken("mtlx")}; }
GetMaterialBindingPurpose() -> HdTokens->full                  // unchanged; husk passes [full, ""]
IsParallelSyncEnabled(material) -> false in M3 (M7)
```

- Karma's priority is `kma > mtlx > vex > universal` (verified: a red/green/blue test material renders red).
- `nova` is reserved for M4 Nova-native networks. `hview` and `__all` are never requested.

### 4.2 Network reading (`networkSource`)

- `HdNovaReadNetwork(terminalSceneIndex, id, ctx, HdNovaNetworkSource*)`: the verified probe reader (hydra-materials §4.2). It keeps per-parameter `colorSpace`/`typeName`, terminal output names, `nodeTypeInfo` and `config`.
- The terminal scene index comes from `sd->GetRenderIndex().GetTerminalSceneIndex()`, the pattern `mesh.cpp` already uses.
- Fallback `HdNovaReadNetworkLegacy(sd, id)`: `GetMaterialResource` → `HdConvertToHdMaterialNetwork2`. It moves the `colorSpace:`/`typeName:` pseudo-parameters into the metadata and fills missing terminal output names from the nodedef.
- Chain per material: for ctx in {nova, kma, mtlx, ""}, read, translate, and accept the first network with a recognized surface terminal. Otherwise use the default material with one warning.
- `TF_DEBUG HDNOVA_MATERIAL` dumps the received network and the chosen context in the probe's format.

### 4.3 Translation pipeline (pure, unit-testable; MaterialX only here)

1. **`nodeDefDatabase`** (`std::call_once`):
   - Search paths, in order: `NOVA_MTLX_SEARCH_PATHS`, `PXR_MTLX_PLUGIN_SEARCH_PATHS`, `PXR_MTLX_STDLIB_SEARCH_PATHS`, then `dir(GetModuleFileNameW(GetModuleHandleW(L"MaterialXCore.dll")))/../houdini/materialx/libraries`. `ArchGetAddressInfo` fails on Windows here.
   - `mx::loadLibraries({"stdlib","pbrlib","bxdf","cmlib","nprlib","lights","houdini"}, …)`: ≈41 ms, 898 nodedefs, 282 nodegraphs.
   - Produces immutable POD records: inputs, `VtValue` defaults, uniform flag, enum strings, `defaultgeomprop`, implementation kind.
2. **`mtlxDocument`:**
   - Canonicalize: drop pseudo-parameters; resolve USD id aliases (`UsdPreviewSurface → ND_UsdPreviewSurface_surfaceshader`, `UsdUVTexture → ND_UsdUVTexture_23`, `UsdPrimvarReader_* → ND_UsdPrimvarReader_*`, `UsdTransform2d`) and the legacy rename `ND_normalmap → ND_normalmap_float`.
   - Version: `config mtlx:version`, default 1.38, then `upgradeVersion()` as hdMtlx does.
   - Build the document: `doc->setDataLibrary(lib)`, a node graph of the nodes reachable from the terminal, authored values only.
   - Input `colorSpace` metadata on non-file color inputs: convert the constant to scene_linear with OCIO at translation time.
   - Then `flattenSubgraphs(EMPTY, filter)`. The filter keeps `standard_surface`, `open_pbr_surface`, `UsdPreviewSurface` (M3.11) and anything with a native lowering. Never call `validate()`.
3. **`lower`:**
   - Iterate `topologicalSort()`. A table keyed by node category + output type selects the lowering; width comes from the type; FA variants splat.
   - Unconnected `defaultgeomprop` inputs become globals: UV0 → primary UV (`st`, honoring `USDMTLX_PRIMARY_UV_NAME`), Pobject, Nworld, Tworld, Bworld.
   - Uniform strings and enums become immediates. Filenames become texture slots plus a `TextureRequest`. `geompropvalue`/`texcoord`/`geomcolor` (→ `displayColor`) and primvar readers become primvar slots.
   - Surface terminal inputs become `IrOutput{SurfaceField}` through `surfaceMap.h`.
   - Silently ignored: unknown extra inputs on known nodes (`aov:*`, Karma render properties, `karma_mikkt`, `houdini_visualize_signal`).
   - Unknown nodes (`kma_*` without a nodegraph): the nodedef output default, or zero of the consumer type; one warning per (material, node type).
   - Unknown or closure-graph terminals: next context.
4. **`core` compile** via the `ProgramCache`, keyed by `structureHash`, as `shared_future` so two threads never compile the same graph. Equal structures share one `Program`.

### 4.4 Nodes

- **Native:** see the op set in §2.3.
- **Through expansion:** all 198 stdlib nodegraphs (tiledimage, place2d, triplanarprojection, ramp4/ramp, checkerboard and the shape patterns, unifiednoise, random*, contrast, range, hsvadjust, saturate, colorcorrect, overlay, switch, safepower, distance, separate*, convert*, vector smoothstep, blur), the `hmtlx*` graphs, and UsdUVTexture / UsdPrimvarReader / UsdTransform2d.
- Expansion is bit-identical to Karma, which expands the same graphs (`karmaNodeGraphs.json` has the same node counts).

### 4.5 Primvars and GeomSubsets

- **Mesh attributes:**
  - `HdNovaMesh` stores every numeric primvar: float/int/double and their vec2/3/4, color3f/4f, texCoord2f, normal3f, vector3f, point3f.
  - Excluded: points, normals, velocities, accelerations, widths, `nova:*`.
  - Face-varying values are expanded per triangle corner with `ComputeTriangulatedFaceVaryingPrimvar`; uniform values per triangle via `DecodeFaceIndexFromCoarseFaceParam`.
  - `_GeometryUnchanged` compares all primvar `VtValue`s (identity short-circuit).
- **Subdivision:** `SubdivPrimvar{AttributeId, width, interp, values, indices}` replaces `SubdivColorPrimvar`. All primvars are refined, and the tessellator outputs a base-face id per triangle for subset slots.
- **Instancers:** `HdNovaInstancer::ComputeInstanceAttributes` generalizes `ComputeInstanceColors` (nested levels, innermost wins). Lookup chain: mesh, then instance, then op default (today's displayColor rule).
- **GeomSubsets:**
  - slot 0 = the prim binding, slot i = subset i; `triangleSlot` per triangle → `Scene::setMeshMaterials`.
  - `DirtyMaterialId` updates slots only.
  - A subset rebind arrives as `Points|Primvar|Topology|Normals`. Compare the `PxOsdMeshTopology`, the subsets and the primvar identity, and keep the BLAS when they are equal.

### 4.6 IPR edit handling (every Hydra 2 material edit arrives as AllDirty 0x7c)

| Edit | Action | Restart |
|---|---|---|
| identical network (e.g. an edit on a disconnected node) | `src.net == _lastNet` and same context → return | no |
| float/vector/color value | same hash → new params; commit reruns the prologue and rebuilds the template | yes, no compile |
| texture path or color space | new `TextureId` (opened lazily), slot table updated | yes, no compile |
| connection, enum, `geomprop`, node type | recompile or `ProgramCache` hit, new attribute ids | yes |
| `material:binding` | `setMeshMaterials` only | yes |
| subset rebind | slots only, BLAS kept | yes |
| material removed | registry orphans the id → default program; meshes keep the id (they are never re-synced); a re-created path reuses the id | yes |
| texture file changed on disk | mtime check on re-sync, `ts->invalidate(path, false)`, reconvert | yes |

All heavy work (read, translate, compile, open and convert textures) happens before `AcquireSceneForEdit()`.

Target: engine-side value-edit latency < 10 ms; the plan's < 100 ms is end to end.

### 4.7 Textures

- **System:** `HdNovaTextureSystem : nova::ITextureSampler` with `TextureSystem::create(false, ImageCache::create(true))`, so husk's `--oiio-*` limits apply.
  - TS options: `flip_t=1`, `gray_to_rgb=1`, `stochastic=3`.
  - Per batch: one `TextureOpt` and `get_perthread_info()`; `anisotropic=8`, explicit wrap modes (maketx files carry `black,black`).
  - The scalar handle call runs in a loop; the batched API is a scalar loop in 2.5.18.
- **Color space** (`colorSpace.{h,cpp}`, shared with the light `textureCache.cpp`):
  1. explicit `colorSpace:file`, `UsdUVTexture.sourceColorSpace`, or a non-color signature → raw;
  2. OCIO file rules, skipping the default rule;
  3. `oiio:ColorSpace`;
  4. 8-bit → sRGB, otherwise linear.
  - Linear gamut differences become a 3×3 matrix (extracted from OCIO and verified linear) applied after the lookup.
- **Conversion** (`textureConvert`):
  - Policies: Direct (linear, tiled, mipped), Matrix, Convert, ConvertRat, Missing.
  - Convert: `make_texture` to half, tile 64, mipped, zip, `fixnan box3`, `oiio:ColorSpace` set explicitly.
  - Cache key: SHA-1 of (canonical path, size, mtime, source color space, format version). Written to a temporary name, then renamed atomically.
  - UDIM: each tile converted under one prefix. `.rat` goes through an `imaketx` subprocess (never `iconvert`, which applies gamma 2.2).
  - Packaged usdz assets are extracted with `ArResolver::OpenAsset`.
- **UDIM and address modes (Karma):**
  - tile = 1001 + ⌊u⌋ + 10⌊v⌋ for 0 ≤ u < 10 and v ≥ 0; otherwise `default`. This guards OIIO's `int()` truncation.
  - Missing tile → `default`, silently (`missingcolor` only for UDIM sets).
  - Clamp inside a tile. `constant` → black. `default` for empty files.
  - A missing file returns `default` and writes one stderr warning (husk prints no Tf warnings).
  - Frame tokens and `framerange` are ignored with a warning, as in Karma.
- **Footprint:** ray cones. JCGT 2021 ellipse with the diameter, through the triangle's uv gradients, scaled by `nova:textureFilterWidth` (0.25, Karma-measured: level = log2(texels/px) − 2, independent of spp) × `karma_width` on the node.
  - The derivative cones are compiler-generated: the texcoord input's upstream subgraph is duplicated twice with globals shifted by ε·a₁ and ε·a₂ (ε = 1/16), and the derivative = Δ/ε (exact for affine maps such as tiledimage).
  - Inside shifted copies, `Tex2D` uses geometric derivatives.

### 4.8 New render settings (RenderSettings attributes; UI in M4)

`nova:textureFilterWidth` (0.25), `nova:textureBlur` (0), `nova:textureAutoTx` (1), `nova:textureCacheDir` ("" → `$HOUDINI_TEMP_DIR/nova_tx`, else `%TEMP%/nova_tx`), `nova:abortOnMissingTexture` (0), `nova:textureStats` (0).

### 4.9 Default material

`MaterialId 0` is built in core (`default_material.cpp`): Standard Surface with base 1, specular 0, `base_color = Primvar("displayColor", default 0.18)`.
- This is exactly today's Lambert(displayColor) including the instance-color fallback, so all unbound husk/cli test scenes keep their results.
- The 14 M2 reference scenes are all bound to `/Looks/displayColorLambert`, the same network with magenta default. From M3.5 on they run through the full Hydra pipeline and are its end-to-end regression.
- Karma's own unbound default (0.401·C + 0.033) is not matched (Q1).

---

## 5. Sub-steps

Each step builds, passes the full `ctest`, and leaves the renderer working. Effort is in developer-days. Steps M3.1 and M3.3 are independent and can run in parallel.

Dependencies: `M3.0 → M3.1a → M3.1b → M3.2`; `M3.0 → M3.3`; `{M3.2, M3.3} → M3.4 → M3.5 → {M3.6, M3.7 → M3.8 → M3.9, M3.10} → M3.11`.

### M3.0 Spikes and design document (2–3 d, retires the integration unknowns)

**S1. MaterialX inside hdNova.**
- `Nova::MaterialX` imported targets (`$HFS/custom/houdini/dsolib/MaterialX{Core,Format}.lib`, DLLs in `$HFS/bin`); minimal `nodeDefDatabase` (search paths + load).
- Test `[hydra][mtlx]`:
  - ≥ 890 nodedefs; `ND_standard_surface_surfaceshader` active inputs base = 1.0 and base_color = 0.8;
  - flattening `ND_tiledimage_color3` inserts `geomNode_UV0`;
  - load < 100 ms;
  - husk and nova_cli resolve the same library file count.

**S2. Embree argument filters.**
- `RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS` on BLAS and TLAS, `rtcSetGeometryEnableFilterFunctionFromArguments` on triangle geometries, `args.filter` in `rtcIntersect1`/`rtcOccluded1`.
- Test in `test_accel`: a filter rejecting even primIDs gives the same hits and occlusion as a mesh with only the odd triangles, for instance arrays and moving instances. A null filter costs nothing (rays/s ±2%).
- Fallback if argument filters don't reach instance-array geometry: geometry filters set at commit. Decision recorded.

**S3. OIIO TextureSystem in hdNova** under husk and nova_cli.
- Private TS on the shared IC (`imagecache()` == shared, the TS-private `flip_t` doesn't leak), Perthread per TBB worker, one lookup on a generated tiled EXR. Test `[hydra][oiio]`.

**Design document:** `docs/design/M3.md`.

**Exit:** three spike tests green; the fallback decisions recorded.

### M3.1a BSDF core I (4–5 d, renderer untouched)

**Deliverables:**
- `bsdf_common.h`, `microfacet.h` (GGX, stable G2, bounded and full VNDF, pdfs), `fresnel.h` (dielectric, conductor, F82-tint clamped + average, generalized Schlick, artistic IOR, OpenPBR modulated η), `dielectric.h` (R, T, RT, delta variants, thin sheet), `conductor.h`, `diffuse.h` (Lambert, translucent), `energy.h`.
- `tools/nova_bsdf_precompute` and the tables ggxE, Eavg, ABC, dielRT, RTinv, dielR, fAvg.
- `tests/bsdf_stats.h`: the validated harness — 48×96 bins, 2²⁰ samples, open Gauss-Legendre per sub-cell, Wilson–Hilferty, Šidák 0.01.
- `tests/test_bsdf.cpp`.

**Tests:**
- D and VNDF normalization to 1e-4.
- Chi² for GGX iso/aniso (bounded and full, α ∈ {0.05, 0.2, 0.6}) and dielectric RT for η ∈ {1.5, 1/1.5} × α ∈ {0.05, 0.3, 0.6} at μo ∈ {0.1, 0.5, 0.9}.
- `sample` ↔ `eval`/`pdf` agreement to 1e-4 relative.
- Reciprocity: GGX R; the BTDF ratio is 0.4444.
- Fresnel identities.
- Per-lobe furnace 1 ± 0.5%: compensated conductor with F = 1 at any α; RT for η ∈ {1.33, 1.5, 2, 1/1.5}; thin sheet R′ + T′ = 1.
- LUT anchors: E(1, α=1) = 0.307, E(0.5, 0.5) = 0.698, E_RT(1.5, 0.3, 1) = 0.989, E_RT(1/1.5, 1, 1) = 0.418, E_R(1.5, 0.05, 1) = 0.0403, the `E_ggx` r = 0.3 row (0.9904 … 0.8876).
- 16 random cells recomputed at 2¹⁶ samples within 4σ.
- NaN/Inf fuzz with 10⁶ inputs.

**Exit:** all green, < 5 s CPU. **Risk:** medium (a lot of math), contained by the harness.

### M3.1b BSDF core II (3–4 d)

**Deliverables:**
- QON (+ MaterialX fit), EON (listings 1–4, CLTC + uniform), Charlie (+ `charlieE` LUT), Zeltner.
- `thin_film.h`: Airy with a working-space matrix, DC grey, airy albedo approximation.
- `shading_frame.h`: `ensureValidSpecularReflection`, `bumpShadowingTerm`.

**Tests:**
- Chi² for QON, EON (r ∈ {0.5, 1}), Charlie, Zeltner; pdf integral = 1.0000.
- EON albedo = 1.0000 at ρ = 1; Zeltner R anchors at r 0.5 (0.0259 … 0.5249); QON albedo at σ = 1 (0.6418 … 0.8126).
- Airy: thickness 0 equals the base Fresnel; normalized DC grey within 1e-3.
- Energy-bound fuzz: 512 parameter sets, E ≤ 1 + 4σ.

**Risk:** low-medium.

### M3.2 Closures, surface models, integrator (6–7 d)

**Deliverables:**
- `closure.h`; `surface.{h,cpp}` with `buildStandardSurface`, `buildOpenPBR` and `Unlit`.
- Minimal `material.h`: MaterialDesc v0 = model + constant `SurfaceInputs`.
- Scene material table, `setMeshMaterials`, `materialOf`.
- The default material temporarily takes base_color from `hit.color` (flag `kBaseColorFromDisplayColor`).
- `LightDesc`/`LightData::specular` (+ `hydra/light.cpp`); delta-aware `bsdfSampleWeight`.
- `WaveQueues` additions (flags, etaScale, sigmaA) and all integrator changes of §2.4 except the sort, the VM and cones. Albedo AOV from the closure.

**Tests:**
- `test_closures.cpp`:
  - mixture chi² for 5 random SS + 5 random OpenPBR parameter sets;
  - OpenPBR spec white-furnace configurations (metal, diffuse, SSS fallback, translucent, glossy-diffuse, + coat, + fuzz/sheen, + thin film, + anisotropy) at 1 ± 0.5% for all μo and r;
  - anchors: glossy-diffuse ρ 0.5 r 0.3 η 1.5 = E_R + (1−E_R)·0.5; OpenPBR coat η 1.6 over ρ 0.5 with specular_weight 0 = 0.3089, 0.3099, 0.3175, 0.3468, 0.4340, 0.6434; SS specular_color (1,0,0) over white gives G = 1 − E_R(μ).
- `test_material_furnace.cpp` (orthographic sphere 128×64, dome L = 1, maxDepth 64, clamp 0, disk r < 0.97):
  - Lambert ρ (2e-3);
  - white plastic r 0.2/0.6, coat, sheen, fuzz, smooth glass, IOR-1, thin glass, translucent, F = 1 mirror, rough glass r 0.3 → 1.0 (3e-3);
  - emission exactly (0.5, 0.25, 0.125) (1e-5);
  - **IOR-1 sphere over a Lambert plane leaves no shadow** (1%);
  - **an F = 1 mirror reflects a rect light of L = 3 as 3.0** (0.5%);
  - a transmissive sphere has alpha 1.
- The whole M2 suite unchanged; the default material reproduces M2 sampling.

**Exit:** all green; `cornell_rect` rays/s within 10% of M2. **Risk:** medium-high (integrator surgery); every changed code path has a targeted test.

### M3.3 Shading IR, compiler, VM (5–6 d, renderer untouched)

**Deliverables:**
- `core/shading/*` (production port of the validated prototype): iterative passes, 16-byte `Instr` + extension words, uniform and splat flags, `maxRows` option.
- The tier-1 op set (everything in §2.3 except noise, ramps, compositing, `Tex2D`, `NormalMap`, `HeightToNormal`) with Karma safe semantics and own polynomial transcendentals.
- `vm_batch.cpp` lane loops checked with `/Qvec-report:2`.

**Tests:**
- `test_shading_compiler`:
  - folding (`2·3 + P.x` → Extract + Add with uniform 6), exact identities only, CSE, DCE counts;
  - row bounds: a 400-op chain ≤ globals + 3 rows; a depth-7 balanced tree ≤ 9 rows;
  - a param-only subgraph gives 0 varying instructions;
  - the hash is invariant to param values and node names and changes with op, imm and primvar names;
  - compile < 1 ms for 300 nodes.
- `test_shading_vm`: 3000 seeded graphs (5–400 nodes) × 45 lanes, bit-exact between a tree-walking reference of the unoptimized IR, `runLane`, `runBatch<8/16/32>` with partial chunks, and every optimization switched off.
- `test_mtlx_ops`: golden JSON from `vexexec -A 32` with `#include <mxstd.h>` (safe div/mod/sqrt/log, pow with negative base, rint, smoothstep lo ≥ hi, hsv, rotate3d, clamp lo > hi).
- Benchmark (non-gating): about 0.15 ns/(instr·lane) at N = 32.

**Risk:** low-medium.

### M3.4 VM in the shade stage and generic attributes (5–6 d)

**Deliverables:**
- `attributes.h`; `MeshDesc::attributes`; `instanceAttributes`.
- `hydra/mesh.cpp` `buildAttributes`; generic subdivision primvars + base-face ids; `ComputeInstanceAttributes`.
- `material.h` final (`MaterialDesc`, `ProgramRecord`, `MaterialRecord`); commit packing; attribute remap.
- `material_eval.{h,cpp}`; `ShadingPoint`; the global writer (incl. Karma T/B from `st`).
- Default material as a compiled program; `hit.color`, `cornerColors` and `lambert.h` removed.

**Tests:**
- Full M2 suite green; it now exercises the `Primvar` op, instance colors and face-varying subdivision colors.
- A multi-material grid (4 C++ materials): tile render == full render, bit-exact; permuting material ids gives the same image.
- A constant SS through the VM gives a bit-identical `ClosureList` to the direct build.
- Attribute interpolation for every rate × width 1–4.
- Subdivision `st` seam.
- A checker program from `st` (floor/mod) against analytic coverage.

**Exit:** green; `cornell_rect` rays/s within 15% of M3.2. **Risk:** medium (mesh, subdiv and scene all touched).

### M3.5 Hydra materials, untextured (6–8 d)

**Deliverables:**
- §4.1–4.6 in full: `hydra/material/*`, mesh slots and subsets, IPR, `HDNOVA_MATERIAL`.
- `Tex2D` still returns `default` with one warning.
- `nova_imgcmp --region name,cx,cy,r,relTol` and manifest `compare.regions` with a `reason` each.

**Tests:**
- `test_hydra_materials.cpp`, translator unit tests on probe-network copies:
  - pseudo-parameters, relative paths, empty terminal outputs;
  - SS defaults 1.0/0.8; the `ND_normalmap` rename; USD aliases;
  - attribute extraction (`st` from texcoord and from an image's default texcoord, `mask` from geompropvalue, `varname`);
  - an unknown `kma_*` node warns and defaults; an unknown terminal falls back to the next context;
  - structure hash: a value edit keeps it, renaming nodes keeps it, a connection, `geomprop` or enum change gives a new one; two materials of one structure share one `Program*`.
- Hydra-level IPR driver (port of the probe `driver.cpp`):
  - a value edit compiles nothing and syncs no mesh;
  - a `geomprop` rename changes the image without a mesh sync;
  - a disconnected-node edit leaves the edit version unchanged;
  - a rebind gives `MaterialId` only;
  - a subset rebind does not rebuild the BLAS (accel build counter);
  - removal gives the default.
- `husk/check_materials.py` (white dome 1, direct only, top-down):
  - context priority exactly (1,0,0);
  - Lambert calibration exactly C;
  - SS all-defaults 0.8089 ± 1% (Karma);
  - OpenPBR defaults;
  - a `kma`-only material is picked up;
  - an interface-promoted parameter;
  - a GeomSubset two-material mesh.
- All 14 M2 references green, now through Hydra materials.
- New references (§6.3): `furnace_materials`, `mtl_metal`, `mtl_glass`, `mtl_layered`, and `mtl_diffuse` without its texture and opacity spheres.

**Exit:** green, plus a Solaris check through fxhoudini: a Karma Material Builder default material and mtlx SS/OpenPBR preset balls render in the Nova viewport, and a slider edit restarts in < 100 ms.

**Risk:** medium-high; the probe code is already verified.

### M3.6 Node coverage (4–5 d)

**Deliverables:**
- `mx_noise.h` (MaterialX GLSL port, Apache-2.0) and `simd8.h` 8-wide AVX2 kernels: Noise, Fractal, Cell, Worley.
- Ramps and splits, Blend, Premult/Unpremult, Comp4.
- Frame and time: `houdini:frame`/`houdini:fps` if husk/Solaris pass them, else `nova:frame`; default 1/24; time = (frame−1)/fps as in Karma.

**Tests:**
- SIMD == scalar bit-exact; Perlin ≤ 6e-8 against `vexexec`, cell and Worley exact.
- Every stdlib nodegraph nodedef + `hmtlx*` flattens and lowers with no unknown op (hydra test).
- Reference `mtl_nodes` (unlit quads, orthographic, point filter) against Karma: ramp/split orientation, smoothstep lo ≥ hi, dodge, divide by zero, noise2d/3d (Pobject on a PointInstancer), fractal, cellnoise, Worley solid, checkerboard, hsvadjust, place2d/rotate2d.

**Risk:** low.

### M3.7 Textures (5–6 d)

**Deliverables:**
- `core/texture/texture.h` + `MemorySampler`; the `Tex2D` op (zero derivatives for now, i.e. finest level: the mean is correct).
- `colorSpace`, `textureConvert` and `textureSystem` (§4.7); render settings; stats.

**Tests:**
- `test_textures`: wrap modes on a 4×1 ramp at s = −0.1 / 1.1 / 1.6 — clamp 1,4,4; periodic 4,1,3; mirror 1,4,2; black 0; constant = default. MIP mean conservation.
- Hydra OIIO tests:
  - UDIM 1001 / 1002 / 1011 colors; missing tile and negative u give the default;
  - v orientation;
  - sRGB code 10 → 0.003035 ± 1e-5 and 128 → 0.215861 ± 1e-4, for an untiled PNG and a uint8 tx;
  - a 50% checker at a coarse footprint → 0.5;
  - an ACEScg EXR → M·c;
  - grey → RGB;
  - PNG RGBA(1,1,1,0.5) → 0.502;
  - a missing file → default + exactly one stderr line;
  - `.rat` → 0.2158.
- `husk/check_textures.py` extended to `mtlximage` (png, jpg, uint8 tx, half EXR, `*ACEScg*`).
- Reference `mtl_textured` part 1: UDIM base_color, raw roughness map, closest-filter checker at magnification.

**Risk:** medium.

### M3.8 Footprints (3–4 d)

**Deliverables:**
- Cones in `WaveQueues`, `Camera::rayCone` (cached per tile), `scatterSpread`, curvature.
- Compiler pass "derivative cones" (§4.7), `kTexture` stream, stochastic `rnd`.

**Tests:**
- Pinhole spread = atan(2·tan(fovY/2)/H); orthographic width = pixel size.
- Footprint major axis W/cosθ and minor axis W at 0°, 60°, 85°.
- uv-gradient invariants; sphere curvature ≈ 1/R.
- Derivative cones are duplicated only for `Tex2D` texcoord inputs; tiledimage with uvtiling 8 gives exactly ×8 derivatives.
- MIP parity with a `maketx:mipimages` texture: 16 / 64 / 4 texels per pixel → level 2.00 / 4.0 / ≈ 0 (Karma).
- A minified 1-texel checker has mean 0.5 ± 0.005 and per-pixel σ < 0.02.
- `mtl_textured` part 2: grazing tx checker against Karma (block means).

**Risk:** medium-low.

### M3.9 Normals, tangents, anisotropy, bump (3 d)

**Deliverables:**
- `NormalMap` (Karma: Gram-Schmidt, Ng flip guard ×1.001).
- `HeightToNormal`: a surface gradient from shifted re-evaluation, with Karma's world-space meaning P′ = P + Ng·h·scale. The `bump` graph follows from it.
- Bindings for normal, coat_normal and tangent; `specular_rotation`/`coat_rotation`; bent-normal robustness.

**Tests:**
- A flat normal map (0.5, 0.5, 1) is bit-identical to none.
- A constant tilted map matches the analytic irradiance cos∠(L, N′)·E/π.
- A linear height h = a·u gives the analytic tilt.
- Anisotropy with `st` vs swapped `st` matches Karma's orientation.
- A furnace with a random normal map stays ≤ 1 + 3σ.
- The `mtl_metal` anisotropic sphere and the `mtl_textured` normal-mapped sphere pass against Karma.

**Risk:** medium-low.

### M3.10 Opacity (3–4 d)

**Deliverables:**
- `Program::opacityCode`; `IAccel::setHitFilter(const HitFilter *)` (core callback `float opacity(MeshId, prim, instance, Vec2f bary, float time)` + per-ray seed); argument filters from S2.
- Per-mesh opacity flags at commit; first-hit AOV rays included.

**Tests:**
- Opacity 0.5 white sphere on black → color 0.75, alpha 0.75 (3e-3; Karma 0.7501).
- A 0/1 checker cutout gives exact coverage.
- The shadow of a 0.5-opacity plane is 0.5 of the irradiance.
- Opaque scenes: rays/s unchanged (no filter installed).
- The opacity sphere in `mtl_diffuse` against Karma.

**Risk:** medium; retired by S2.

### M3.11 UsdPreviewSurface, reference completion, performance, docs (3–4 d)

**Deliverables:**
- `SurfaceModel::UsdPreview`:
  - metallic workflow → base/metal/specular_roughness/coat (clearcoat)/IOR/opacity/emissive;
  - specular workflow → a generalized-Schlick R lobe (F0 = specularColor, F90 = 1) layered by the ABC albedo.
  - UsdUVTexture `scale`/`bias`/`fallback` through the expanded graph.
- `karma_albedo_probe.py`, `gen_albedo_ref.py` + `tests/data/albedo_ref.json` (an independent numpy implementation).
- README "Current scope"; `docs/design/M3.md` updated.
- Performance report: rays/s, shade ns/hit, compile ms per material, IPR latency.

**Tests:**
- `mat_preview` in `check_materials.py`: Karma (0.5139, 0.2733, 0.1530) and 0.2062, ± 2%.
- A VEX-builder material with an auto-generated preview renders its universal network.
- Per-lobe albedo probes (radial, 10 μ bins) against `albedo_ref.json` at 0.3% absolute.

**M3 exit (milestone):**
- Every test above green.
- 7 Karma scenes + 1 analytic pass with the documented tolerances.
- Solaris check: SS and OpenPBR preset grid (surface-models §8.2 minus subsurface and dispersion) in the Nova viewport next to Karma.
- Performance gates (§6.6) met.

**Total:** about 52–65 developer-days.

---

## 6. Test strategy

### 6.1 Tiers

1. **Analytic unit tests:** BSDF, closures, VM, texture math. Exactness lives here.
2. **Render-level analytic tests** in `nova_tests`: furnaces, emission, delta-MIS probes.
3. **Hydra-level tests:** translator, IPR driver, OIIO.
4. **husk and cli checks:** calibration values, color spaces, warnings.
5. **Karma-referenced scenes:** these check translation and integration, with tolerance tiers and regions.
6. **Offline tools** (not in ctest): LUT generator, `albedo_ref`, `vexexec` golden values, Karma albedo probe, Karma references.

### 6.2 New tests and runtime budget

| Test | Contents | Wall at -j4 |
|---|---|---|
| `test_bsdf`, `test_closures` | chi², mass, consistency, reciprocity, furnaces, anchors, fuzz, LUT regression | ~3 s (8 s CPU; one `TEST_CASE` per lobe) |
| `test_material_furnace` | §M3.2 table + opacity | ~1.5 s |
| `test_shading_compiler`, `test_shading_vm`, `test_mtlx_ops` | §M3.3 | ~0.7 s |
| `test_textures` | memory sampler, cones | ~0.3 s |
| `test_hydra_materials` | translator, hash, IPR driver, OIIO (generated textures in a temp dir), MaterialX loading | ~3 s |
| `husk_materials` (`check_materials.py`) | ~8 husk runs | ~7 s (serial inside the test) |
| `husk_textures` (extended) | surface textures | +3 s |
| reference: 8 new scenes × (husk + cli) | §6.3 | ~21 s serial, ~6 s wall |

ctest wall time grows by about 12 s at -j4 (the suite goes from ≈ 30 s to ≈ 45 s). Offline Karma references: 7 scenes × 2 halves × ~35 s ≈ 8 min.

### 6.3 Karma-referenced scenes

- 192×96, 6×3 floating spheres from `assets/sphere_st.usda` (160×80, `st` with u along φ).
- Test at 64 spp; references at 2 × 4096 spp.
- No textured domes, no diffuse receivers of specular light.

| Scene | Content | Lighting | Tolerance |
|---|---|---|---|
| `furnace_materials` | 16 energy-conserving materials (Lambert white, EON r1, plastic r 0.2/0.6, coat, sheen and fuzz over white, smooth / IOR-1 / thin glass, translucent, F = 1 metal, …) | constant dome L = 1, visible | analytic constant 1, `global_rel_tol 5e-4`, `karma_check` |
| `mtl_diffuse` | Lambert colors, EON r 0.5/1, all-defaults SS, emission sphere, opacity 0.4 (from M3.10), UDIM base_color (from M3.7) | dome 0.35 + key rect + rim rect | strict, block 0.01 |
| `mtl_metal` | SS metal r 0/0.2/0.4/0.6 (silver, copper, gold), OpenPBR F82 r 0.3, anisotropy 0.8 with `st` and with swapped `st` (M3.9), smooth thin-film metal | same | 0.02; thin-film region 0.30 (reason: Karma sRGB cast) |
| `mtl_glass` | smooth glass IOR 1.5/1.33, tinted, thin-walled, IOR 1, r 0.15, in front of an emissive checker backdrop | dome + key | 0.02 |
| `mtl_layered` | plastic r 0.2/0.5 on bright bases, SS coat, SS sheen, OpenPBR fuzz, thin-film dielectric, SS QON r1 | dome + soft rect | 0.03; regions: sheen 0.10, QON 0.06, plastic highlights 0.10 |
| `mtl_textured` | tilted plane + spheres: UDIM, raw roughness map, normal map, tx checker at grazing angles | dome + rect | strict 0.01 (block means) |
| `mtl_nodes` | unlit probe quads (§M3.6) | none (unlit), point filter | 0.002 absolute per block |
| `mtl_kma_context` | the same SS through `outputs:kma:surface` only | dome | strict |

**Never edit** `scenes/common.usda` or `assets/sphere.usda`: that invalidates the 14 M2 reference hashes.

### 6.4 Extending `tests/reference`

- **New `common_mtl.usda`** with the verified Karma settings class: `convergence_mode` Path Traced, uniform oracle, color limits 1e9, `constrainmaxrough 0`, object limits 64, `causticsenable 1`, `causticsroughnessclamp 0`, `internalreflectenable 1`, `thinwallatrefractlimit 0`. Nova side: `nova:maxDepth 64` and `nova:clampIndirect 0`.
- **`gen_scenes.py`:** a `material(name, shader_id, inputs)` helper; it writes the sphere asset and computes region circles from the camera projection, minus 1.5 px.
- **`manifest.json`:** `compare.regions: [{name, circle: [cx,cy,r], rel_tol, reason}]`. Every tolerance > 0.01 needs a `reason`. Excluded cases (subsurface, dispersion) use `pending`.
- **`check_references.py`** forwards `--region`.
- **`nova_imgcmp`:**
  - a per-pixel tolerance map;
  - a region-mean z-test with Šidák over regions × channels, sharing α_FWER with the blocks;
  - JSON output per region (`rel_diff`, `z`, `pass`).
- **`make_references.py`** is unchanged. Object-level Karma settings are protected only by the scene hash; they don't appear in `husk:render_stats`.
- **On a Karma version change:** rerun `karma_albedo_probe.py` and recheck the deviation table before regenerating references.

### 6.5 Tolerance policy

| Tier | Limits |
|---|---|
| Analytic | block 0, global ≤ 5e-4 |
| Karma-agreeing | ≤ 0.02, justified by the measured Karma-vs-exact table |
| Tolerant | per region ≤ 1.5 × the measured deviation, at most 0.10, with the remaining regions of the scene strict |

### 6.6 Performance gates (measured on this machine)

- Untextured Standard Surface material grid: ≤ 1.5× Lambert time per sample.
- Shade stage: VM ≤ 10 ns/hit for a 40-instruction graph; closure build ≤ 150 ns/hit.
- Translation + compile ≤ 2 ms per material (SS + 3 textures).
- IPR value edit ≤ 10 ms engine side, < 100 ms end to end in Solaris.
- Opaque scenes: rays/s unchanged by the opacity support.

---

## 7. Risks and open questions

### 7.1 Risks

| Risk | Mitigation |
|---|---|
| Integrator surgery (transmission, delta MIS, offsets) breaks M2 behavior | The default material reproduces M2 sampling; the full M2 suite; targeted IOR-1, mirror and glass furnaces in M3.2 |
| Subtle BSDF math bugs | Validated chi² harness + anchors from two independent numeric sources before any integration |
| MaterialX library differences between husk and nova_cli | S1 spike; explicit search-path order; both hosts tested |
| Hydra 2: every edit is AllDirty, meshes never re-sync | Material-independent mesh data; network diff; structure hash; IPR driver test |
| Embree argument filters with instance arrays | S2 spike; geometry-filter fallback |
| Texture conversion races (several husk processes), disk use | Content-hashed names, temp + atomic rename, `nova:textureAutoTx 0`, stats listing |
| Shade-stage performance (small groups at deep bounces, closure build) | Adaptive N, lane executor for n ≤ 2, uniform-only fast path; gates in §6.6; 64×64 tiles later if needed |
| Memory from storing all primvars | Measure on a production SOP asset in M3.4; union filter in M7 |
| Register pressure in huge expanded graphs (ND_ramp has 270 primitives) | Folding, CSE, DCE; caps of 1024 rows (CPU) and 256 (GPU option); failure → default material + warning |
| Karma deviations make reference tests flaky | Tiers, regions with reasons; exactness asserted only analytically |
| Proprietary code | Never copy `mxpbr.h`, `ggx_utils.h`, `mxstd.h` (SideFX). Implement from MaterialX (Apache-2.0), pbrt and Cycles (Apache-2.0), EON (MIT) and black-box measurements. `vexexec` is used only to produce golden numbers. |

### 7.2 Technical open questions (answered inside the steps)

- Whether Embree argument filters reach instance-array geometry (S2).
- The USD encoding of Houdini's `mtlximage` `filecolorspace` parameter (presumably `colorSpace` metadata on `inputs:file`): check with a LOP via fxhoudini in M3.7.
- Whether husk/Solaris pass the frame and fps to the delegate (`houdini:frame`/`houdini:fps`), M3.6.
- Whether `flattenSubgraphs` is thread-safe on a shared data library: not needed in M3 (serial material sync); test before M7.
- Real compile and translation times on production networks (gate in §6.6).

### 7.3 Questions only the user can decide

- **[USER] Q1. Default material for unbound geometry.** Keep Lambert(displayColor, 0.18), which keeps every existing test and the current look, or match Karma's unbound default (≈ 0.401·C + 0.033)? *Recommendation: keep.*
- **[USER] Q2. Karma look compatibility for BSDFs.** Add a `nova:karmaCompat` switch (Schlick dielectric reflection, sheen as albedo-weighted Lambert, sRGB thin-film matrix, uncompensated rough glass) so look-dev matches Karma 1:1, or stay physically and MaterialX exact? *Recommendation: exact by default; decide on a compat switch after the first `mtl_layered` results.*
- **[USER] Q3. Milestones for dispersion and displacement**, neither of which PLAN.md assigns. *Recommendation: displacement as an M4 extension (it reuses the batch VM on tessellated vertices); dispersion together with spectral rendering, after M9.*
- **[USER] Q4. Texture auto-conversion.** On by default, written to `$HOUDINI_TEMP_DIR/nova_tx` (Nova's choice), or next to the source files (Karma writes `.rat` next to originals, Arnold `.tx`)? *Recommendation: temp cache, on by default.*
- **[USER] Q5. MaterialX op semantics.** Karma's only (this plan), or also a MaterialX-spec mode? *Recommendation: Karma only.*
- **[USER] Q6.** Confirm UsdPreviewSurface support in M3 (deviation 2).
- **[USER] Q7. Glass shadows.** Karma's default is "fake caustics" (transparent, tinted shadows). Nova M3 renders physically: glass blocks shadow rays and caustics only arrive through BSDF paths. Should a fake-caustics or transparent-shadow option come with the M4 render settings, or earlier? *Recommendation: M4.*