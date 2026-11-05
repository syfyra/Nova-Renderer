# Nova M3: ShaderGraph IR and bytecode VM (research report)

**Scope:** the IR, optimizer, register allocation, bytecode format and VM (CPU batch now, CUDA later in M10), and how they plug into `shadeSurfaces`.

**Sources:**
- Cycles at blender `main` 05b4ed5d (2026-10-12)
- pbrt-v4 b4ce9687
- OSL `main` (latest release v1.15.7.0)
- MDL-SDK `master`
- MaterialX 1.39.5, OIIO 2.5.18 and USD 26.05 from `$HFS/toolkit/include`

All local copies, scripts, benchmarks and the prototype are under `C:/Users/Synti/AppData/Local/Temp/claude/C--Users-Synti-Desktop-renderEngine/ac6add8e-dc52-40d3-838a-501e34988043/scratchpad/m3research/shader-vm/`, written `<S>` below.

---

## 0. Recommendations and key numbers

1. **One bytecode, two executors, one set of op semantics.**
   - `core/shading/ops.h` holds scalar `NOVA_HOST_DEVICE` op functions.
   - A **per-lane executor** (`vm_lane.h`, host+device) runs on CUDA, in Embree any-hit/opacity callbacks, for displacement points and as a test oracle.
   - A **SoA batch executor** (`vm_batch.h`, CPU only) runs in the shade stage.
   - A CPU batch of 32 lanes is the same thing as one CUDA warp running the same material.
2. **Measured on this machine** (Ryzen 7 9800X3D, MSVC 14.44, `/O2 /arch:AVX2 /fp:precise`, `<S>/bench/bench_vm.cpp`):

   | Executor | Cost |
   |---|---|
   | Per-lane interpreter (Cycles-SVM style) | **3.0–3.8 ns per instruction per lane**, dispatch-bound |
   | Batch interpreter, N=32 | **0.15–0.16 ns per instruction per lane** for arithmetic (~20× faster) |
   | Batch, N=8 / 16 / 64 | 0.46–0.50 / 0.28–0.30 / 0.13–0.14 |
   | Hand-written per-lane C++ of the same graph | 0.16–0.21 ns (the batch interpreter beats it because its SoA loops vectorize and the AoS C++ does not) |

3. **Vectorization traps (MSVC):**
   - `std::fmin/fmax`, `sin`, `pow` do not vectorize under `/fp:precise` (C5002 reason 1200, errno side effects). With those ops the batch cost rises to 0.94–1.04 ns.
   - Ternary min/max fixes the first. `/fp:fast` (vectorized math calls) gives 0.30–0.35 ns. Own polynomial transcendentals are better because they are portable and deterministic on CPU and GPU.
   - Complex ops are never auto-vectorized. MaterialX `noise3d` is 60 ns/lane scalar (C5002 reason 1305) and **7.1 ns/lane hand-vectorized** with 8-wide AVX2, bit-identical to scalar (`<S>/bench/noise_simd.cpp`).
4. **Execution model:**
   - Sort the hits of each bounce by material with a counting sort (**0.60 ns/hit measured**, 1024 hits, 16 materials).
   - Run each material group in fixed chunks of 32 lanes; groups of 8 or fewer use `runBatch<8>`, 16 or fewer `runBatch<16>`, 2 or fewer the per-lane executor.
   - Today's wave is 1024 paths per thread (`renderer.cpp:17` `kTileSize = 32`), so deep-bounce groups can be small. That is acceptable: N=8 is still about 6× faster than per-lane.
5. **Values never fold, structure folds.**
   - Authored parameter values live in a per-material **uniform block**.
   - Subgraphs that depend only on parameters are hoisted into a host **prologue**, run once per parameter edit.
   - Literal constants from nodegraph implementations do fold.
   - An edit that keeps the **structure hash** only rewrites the uniform block and reruns the prologue (no recompile). This is the same idea as MDL class compilation and OSL `ReParameter`/`ParamHints::interactive`.
6. **The closure layout is compile-time; only numeric closure parameters are VM outputs.**
   - `standard_surface` and `open_pbr_surface` are kept intrinsic: they are filtered out of `flattenSubgraphs`, and their roughly 68 float inputs bind to uber-parameter ids.
   - Generic pbrlib closure graphs (mix/add/multiply/layer of lobes) are flattened at compile time into a weighted lobe list, like Cycles `transform_multi_closure`.
   - No runtime closure instructions are needed.
7. **Prototype validated.** `<S>/proto/{vm.h,compiler.h,compiler.cpp,test_proto.cpp}` (~1000 lines, MSVC `/W4 /permissive-` clean except two warnings):
   - IR, folding, identities, CSE, DCE, uniform hoisting, Sethi-Ullman scheduling, first-fit register allocation, 16-byte instructions, lane and batch executors.
   - Differential test: **3000 random graphs × 45 lanes, bit-exact** across a tree-walking reference, the per-lane executor, `batch<8>`, `batch<32>` with a partial chunk, and the unoptimized compile.
8. **Karma node-probe method validated.**
   - `ND_surface_unlit` with the node under test feeding `emission_color`, orthographic camera, `pixelfilter=point`. Karma CPU reproduced texcoords to 6e-8.
   - Its `noise3d` matches a C++ port of MaterialX `mx_perlin_noise_float` to ≤1e-6 (e.g. Pobj(−0.96875, 0.96875, 0) → 0.061339).
   - This is the oracle for every VM op.

---

## 1. Current Nova code relevant to M3

| Fact | Location |
|---|---|
| Shade stage is one loop over the ray queue; per hit `resolveHit`, then a hard-coded `LambertBsdf{hit.color}`, NEE, BSDF sampling | `core/integrator/wavefront.cpp:169` (`shadeSurfaces`), `:231` resolveHit, `:235` Lambert, `:264` eval, `:272` sample |
| Wave = one 32×32 tile per thread (1024 path slots), queues in `WaveQueues` | `core/integrator/wavefront.h:63`, `core/integrator/renderer.cpp:17` |
| FTZ/DAZ already set per worker (matters for padded lanes) | `core/integrator/renderer.cpp:94` |
| `SurfaceHit` carries P, Ng, N, barycentrics, `color`. resolveHit already interpolates object-space corners and `nObject` (cheap Pobject/Nobject) | `core/scene/scene.h:22`, `core/scene/scene.cpp:274`, `:326`, `:343` |
| Mesh attributes: only displayColor (`cornerColors` per triangle corner); no generic primvars, no material id | `core/scene/mesh.h:15`, `:32` |
| Sobol streams already reserved for M3 | `core/math/sobol.h:146` kBsdf (3D, lobe in z), `:148` kOpacity, `:149` kLayer |
| Delegate already advertises `nova` and `mtlx` material render contexts | `hydra/renderDelegate.cpp:377-378` |
| `nova_core` is built with `/arch:AVX2` | `core/CMakeLists.txt:31` |
| Texture loading (OIIO, OCIO, NaN sanitizing) exists for light textures | `hydra/textureCache.cpp` |

---

## 2. Prior art

### 2.1 Cycles SVM (main, 05b4ed5d)

**Interpreter** (`kernel/svm/svm.h`):
- `svm_eval_nodes<node_feature_mask, type>` at `svm.h:102-121`.
- Per-thread `float stack[SVM_STACK_SIZE]` with `SVM_STACK_SIZE 255` (`kernel/svm/types.h:41`).
- `offset = (sd->shader & SHADER_MASK) * (1 + sizeof(SVMNodeShaderJump)/sizeof(uint))`, then `while (true) { node_type = kernel_data_fetch(svm_nodes, offset++); switch ... }`.
- The header comment (`svm.h:7-26`) says nodes are encoded as uint4 words in a 1D texture, and the stack is "stored in local memory on the GPU … If the same shader is being executed, memory access will be coalesced and cached".

**Node encoding:**
- Current main uses typed POD node structs read in place: `svm_node_get<T>` (`kernel/svm/util.h:204-213`, `static_assert(sizeof(T) % sizeof(uint) == 0)`).
- Inputs are `SVMInputFloat`: either an inline float or a stack offset NaN-boxed with `SVM_INPUT_STACK_OFFSET_MASK 0x7FC00000u` (`types.h:24-37`; decode at `util.h:83-100`). Constants therefore need no load instruction or stack slot.
- `SVM_STACK_INVALID = 255` marks "not on stack" (`types.h:43`).

**Derivatives** are new in main:
- `*_DERIVATIVE` node variants store val/dx/dy in 3× stack slots (`util.h:52-78`, `stack_load<dual3>`).
- `mark_nodes_requiring_derivatives` marks the backward cone of texture nodes that need derivatives (`scene/svm.cpp:1002`, called at `:1096`).

**Closures:**
- Written into `ShaderData::closure[MAX_CLOSURE]` with `MAX_CLOSURE 64` (`kernel/types.h:869-872`) via `closure_alloc` (`kernel/closure/alloc.h:11-31`).
- Larger closures take extra slots from the array end (`alloc.h:33-50`).
- Weight cutoff happens in `closure_sample_weight` (`alloc.h:52-70`).

**Compiler** (`scene/svm.cpp`, `scene/shader_graph.cpp`):
- `ShaderGraph::clean`: `constant_fold` → `simplify_settings` → `deduplicate_nodes` → `optimize_volume_output` → `break_cycles` → remove unvisited nodes (`shader_graph.cpp:801-851`).
- Constant folding walks inputs-done order and calls `node->constant_fold(folder)` (`:533-590`). Nodes fold with the **kernel function itself**, e.g. `MathNode::constant_fold` → `svm_math(...)` (`scene/shader_nodes.cpp:7000-7008`). Identities live in `ConstantFolder::fold_math` (`scene/constant_fold.cpp:445-500`): x+0, x·1, x·0→0, x/1, 0/x, pow.
- Deduplication merges bottom-up nodes with equal type, settings and input links (`shader_graph.cpp:600-677`).
- `transform_multi_closure` turns the mix/add closure tree into `MixClosureWeightNode`/Math nodes feeding each closure's `SurfaceMixWeight`, so closures are written straight into an array (`:1101`).
- Stack allocation is first-fit over `active_stack.users[]` for `size` contiguous floats (`svm.cpp:206-238`). Freeing happens when the node is the sole remaining user (`stack_clear_users`, `:402-422`).
- Comment `:404-409`: reusing input slots for outputs "was tried … disabled".
- Scheduling is **Sethi-Ullman**: producers in descending (SU number − output size), sinks sorted the same way (`svm.cpp:606-756`).
- Mix closures emit `NODE_JUMP_IF_ONE`/`NODE_JUMP_IF_ZERO` around exclusive dependencies so zero-weight branches are skipped per ray (`:848-1001`, jump emission at `:953`). Shared dependencies are generated before the jumps.
- AOV-only nodes go after `NODE_AOV_START` (`:1149-1152`; kernel early-out at `svm.h:637-640`).
- `compile()` emits one `NODE_SHADER_JUMP` header per shader with offsets for surface/volume/displacement (`svm.cpp:1177-1251`, kernel `svm.h:129-145`).

**Integration:**
- **CPU** Cycles runs a megakernel per path state (`integrator/path_trace_work_cpu.cpp:119-182` → `integrator_megakernel`), so CPU SVM is always per-ray scalar.
- **GPU** uses a wavefront with shader sorting: `INTEGRATOR_SORT_KEY(key, state) = key + max_shaders * (state / sort_partition_divisor)` (`kernel/integrator/state_flow.h:162-199`), and the intersect kernel routes by shader (`intersect_closest.h:157-161`).
- Shaders using AO/bevel/raycast nodes go to a separate `SHADE_SURFACE_RAYTRACE` kernel. The default kernel is compiled with `KERNEL_FEATURE_NODE_MASK_SURFACE & ~KERNEL_FEATURE_NODE_RAYTRACE` (`shade_surface.h:919-975`).
- Under `__KERNEL_USE_DATA_CONSTANTS__` (scene-specialized kernels), `SVM_CASE` skips node types the scene never uses via `kernel_data_svm_usage_<node>` (`svm.h:92-99`, `data_template.h:257`).

**Why SVM works on GPU:**
- Flat, read-only node stream, uniform across a warp when sorted.
- Fixed-size stack in local memory, coalesced because the same offsets are used across the warp.
- No recursion and no function pointers; a single switch.
- Closures in a fixed array.
- Feature masks and specialization remove unused node cases, which lowers register pressure.
- Expensive features (raytrace nodes) go to separate kernels.
- Sorting by shader minimizes divergence.

### 2.2 Cycles OSL (contrast)

- CPU OSL runs JIT-compiled shader groups. On OptiX, each shader is a direct callable invoked with `optixDirectCall(2 + 1 + shader + type*max_shaders, ...)` (`kernel/osl/osl.h:190-285`).
- The resulting `ClosureColor` tree (MUL/ADD/LAYER nodes with a 16-entry explicit stack) is flattened into the SVM closure array by `flatten_closure_tree` (`osl.h:73`ff). For layers, top-layer albedos scale the base.
- Takeaway: even with a JIT, the renderer-side closure representation is a flat array, which is exactly what Nova's compile-time flattening produces.

### 2.3 pbrt-v4 (no VM)

- `Material` is a `TaggedPointer<...>` (`base/material.h:35`): type tag in bits 57+ (`util/taggedptr.h:736`, `tagShift = 57` at `:865`), `Dispatch` is a switch over the tag index (`:834`).
- The wavefront runs `ForEachType(EvaluateMaterialCallback, Material::Types())`: one queue and one kernel per **C++ material type** (`wavefront/surfscatter.cpp:39-43`, queues are `MultiWorkQueue` at `wavefront/workitems.h:499`).
- Each material type has two variants: `BasicTextureEvaluator` (constant/image textures only, the fast GPU path) and `UniversalTextureEvaluator` (`surfscatter.cpp:45-55`, `textures.h:1158-1194`).
- Texture filtering without ray differentials: `Camera::Approximate_dp_dxy` (`cameras.h:155-180`) uses minimum camera differentials (found once with 512 probe rays, `cameras.cpp:154-194`) scaled by `max(1/8, 1/sqrt(spp))`. du/dx etc. come from least squares with `dpdu, dpdv` (`surfscatter.cpp:75-103`).
- Takeaway: specialization by type is fast but needs compile-time material types. User graphs need a VM or JIT. The stateless derivative approximation is a good M3 seed.

### 2.4 OSL runtime

- LLVM JIT of whole shader groups, with runtime optimization that uses actual instance parameter values. Parameters marked `ParamHints::interactive` (`oslexec.h:108-124`) are not folded and can be changed with `ReParameter` (`oslexec.h:749-771`); interactive values live in a separate block (`getattribute` "interactive_params", `:558-563`).
- Batched SIMD execution, compiled only with `OSL_USE_BATCHED`: `configure_batch_execution_at(width)` and `BatchedExecutor<WidthT>::execute(ctx, group, batch_size, Wide<const int,WidthT> shadeindex, BatchedShaderGlobals<WidthT>&, ...)` (`oslexec.h:972-1033`).
- Shader globals are SoA blocks: `VaryingShaderGlobals<WidthT>` with `Block<Vec3> P, dPdx, dPdy, ...` (`batched_shaderglobals.h:77-108`).
- Uniform vs varying analysis decides which ops run once per batch.
- Automatic derivatives use `Dual<T,2>` (`dual.h:228`, `Dual2` at `:397`).
- Nova's batch VM is a JIT-less version of OSL batched execution: the same SoA globals, uniform/varying split and masked width, with the dispatch cost amortized over N.

### 2.5 MDL SDK

- Backends: `MB_CUDA_PTX, MB_LLVM_IR, MB_GLSL, MB_NATIVE, MB_HLSL` (`imdl_backend_api.h:85-90`).
- Generated distribution functions follow `init/sample/evaluate/pdf`, each taking `(state, res_data, arg_block_data)` (`target_code_types.h:1308-1414`).
- **Class compilation** folds calls but keeps arguments symbolic, so structurally equal materials share code and differ only in the *argument block* (`icompiled_material.h:96-103`; `ITarget_argument_block`, `imdl_backend.h:708-727`).
- The `init` function can precompute texture-dependent expressions into `text_results` (`num_texture_results`, `imdl_backend.h:124`) so sample/eval/pdf do not re-evaluate the graph.
- Nova analogues: the structure hash is class compilation, the uniform block is the argument block, and the closure-parameter output rows play the role of `text_results`.

### 2.6 Arnold, RenderMan, Karma

- **Arnold:** C++ shader nodes return closure lists (BSDF objects built through API constructors). OSL is used for user shaders. There is no VM; per-sample shading calls compiled code (API documentation, no line references).
- **RenderMan RIS:** pattern plugins compute outputs for all points of a `RixShadingContext` in one `ComputeOutputParams` call, looping over `sctx->numPts`, with varying buffers sized `numPts`. Bxdfs evaluate in batches too ([Writing Patterns](https://renderman.atlassian.net/wiki/spaces/REN24/pages/21759862/Writing+Patterns), [RixPattern reference](https://renderman.pixar.com/resources/rman26/classRixPattern.html)). This is the closest production analogue to Nova's batch-over-hits design.
- **Karma CPU** shades with VEX, an interpreted SIMD VM:
  - `isvarying()`: "different value for each processor in the VEX SIMD array" (`$HFS/houdini/help/vex.zip: functions/isvarying.txt:12-16`).
  - `upush` pushes "for all SIMD processors (regardless of the processor enabled state)" (`functions/upush.txt`).
  - `bsdf` is an opaque type that can be summed and scaled (`vex/pbr.txt:10,43`).
- **Karma XPU** "will never support VEX-based shaders"; it compiles MaterialX to device code and caches it (`solaris.zip: karma.txt:19`, `karma_xpu.txt:33-35`). Its "fixed shader model reduces shader re-compilation" (`quick_materials.txt:9`). This supports avoiding recompiles on parameter edits.

---

## 3. Measurements on this machine

All runs: Ryzen 7 9800X3D (Zen 5, 48 KB L1D), MSVC 14.44, `/O2 /arch:AVX2`, same bytecode in every executor, results checked for equality.

**`<S>/bench/bench_vm.cpp`** (36 instructions / 54 component ops, and a 4× version with 111 instructions):

| Variant | per-lane | batch N=8 | N=16 | N=32 | N=64 | N=128 | native C++ |
|---|---|---|---|---|---|---|---|
| `/fp:precise`, ternary min/max, no sin/pow | 3.10 | 0.50 | 0.30 | **0.151** | 0.133 | 0.117 | 0.206 |
| same, 111 instructions | 3.02 | 0.46 | 0.28 | **0.164** | 0.140 | 0.129 | 0.163 |
| `/fp:precise`, std::fmin/fmax, scalar sin/pow | 3.59 | 1.15 | 1.04 | 0.935 | 0.906 | 0.894 | 0.952 |
| `/fp:fast`, ternary, vectorized sin/pow | 3.20 | 0.57 | 0.41 | **0.295** | 0.272 | 0.261 | 0.620 |

Units: ns per (instruction × lane).

**Other measurements:**
- **Noise:** MaterialX `mx_perlin_noise_float(vec3)` ported verbatim. Scalar 60 ns/lane, and MSVC does not vectorize it even in a SoA loop (C5002 reason 1305). AVX2 `vfloat8/vint8` (OIIO `simd.h`, `<S>/bench/noise_simd.cpp`) gives **7.07 ns/lane**, with output bit-identical to scalar (same checksum).
- **Counting sort** of 1024 hits by 16 material ids: **0.60 ns/hit**.
- **Prototype** (`<S>/proto/test_proto.cpp`, random 400-node graph → 132 varying instructions, 49 rows): batch32 0.40–0.63 ns/(instr·lane) including harness input load and output gather; per-lane 2.4–2.7.
- **Karma probe** (`<S>/karma_probe/probe.usda`, husk `BRAY_HdKarma`, isolated prefs, `HOUDINI_PACKAGE_SKIPLIST=nova`):
  - `ND_surface_unlit(emission_color = convert(texcoord))` gives max |uv error| 5.96e-8.
  - `noise3d(position(object))` at three probes matches the C++ port within 1e-6. Karma CPU uses the MaterialX reference noise.

**Conclusions:**
- Dispatch cost (~3 ns) disappears at N ≥ 16.
- The batch VM reaches native-code speed for arithmetic.
- Heavy ops need explicit SIMD.
- Transcendentals need vectorizable implementations.

---

## 4. Design for Nova

### 4.1 Pipeline

```
HdMaterialNetwork2 ─(hydra/material: MaterialX nodedef lookup, flattenSubgraphs(filter keeps intrinsics),
                     type/convert insertion, uniform strings → immediates, filenames → texture handles,
                     geomprops/texcoord → primvar slots)
  → core/shading IrGraph (typed SSA DAG; no MaterialX/USD types)
  → passes: fold(literals) → CSE → fold → closure flatten → DCE → uniform analysis
            → derivative marking → Sethi-Ullman schedule → liveness + first-fit rows → emit
  → Program (POD: prologue, code, uniformInit, constValues, globals, output bindings, texture/primvar tables, structureHash)
  → Material = Program* (shared by structureHash) + uniform block (per material) + resolved texture ids
  → Scene::updateMaterial / commit → MaterialTable (flat arrays, device-uploadable)
  → shadeSurfaces: sort hits by material → per 32-lane chunk: fill global rows → runBatch → per lane gather
    closure params → BSDF build → NEE + continuation
```

MaterialX entry points in `$HFS/toolkit/include`:
- `GraphElement::flattenSubgraphs(const string& target, NodePredicate filter)` (`MaterialXCore/Node.h:308`), with the filter returning false for intrinsic nodedefs (`ND_standard_surface_*`, `ND_open_pbr_surface_*`, `ND_image_*`, noise family, normalmap).
- `NodeDef::getImplementation` (`Definition.h:150`), `topologicalSort` (`Node.h:312`).
- `HdMtlxCreateMtlxDocumentFromHdNetwork(...)` (`pxr/imaging/hdMtlx/hdMtlx.h:101`), `HdMtlxStdLibraries()` (`:45`).
- MaterialX data libraries: `$HFS/houdini/materialx/libraries/{stdlib,pbrlib,bxdf}` (1.39.5).

**What remains after expanding nodegraphs** (`<S>/scripts/mtlx_inventory.py`, `mtlx_types.py`):
- **stdlib:** 495 nodedefs with source implementations, 198 nodegraph-implemented.
- **pbrlib:** 41 source-implemented (lobes, EDFs, VDFs, add/mix/multiply/layer, surface, displacement, artistic_ior, roughness helpers).
- **bxdf:** all nodegraph-implemented. The `standard_surface` nodegraph has 60 nodes and 22 closure-typed nodes; `open_pbr_surface` has 108 and 32.
- **Intrinsic nodes have about 68 float inputs:** `ND_open_pbr_surface_surfaceshader` 41 inputs / 67 components; `ND_standard_surface_surfaceshader_100` 42 / 68.
- **Value types used:** float, integer, boolean, vector2/3/4, color3/4, matrix33/44, string, filename, plus BSDF/EDF/VDF/surfaceshader/displacementshader/volumeshader/material.
- **Geometric defaults** (`defaultgeomprop`): UV0 ×85, Pobject ×34, Nworld ×19, Tworld ×10, Nobject ×6, Bworld ×6.

### 4.2 IR (`core/shading/ir.h`)

- **Types:** `Float, Int, Bool, Vec2, Vec3, Color3, Vec4, Color4, Mat33, Mat44` are register types of width 1/1/1/2/3/3/4/4/9/16 float rows.
  - Int and Bool are stored as exact floats (ints exact to 2^24; MaterialX stdlib has no bitwise integer ops).
  - `String` and `Filename` are never values. MaterialX marks every such input `uniform="true"`: `image.file/filtertype/uaddressmode/...`, `geompropvalue.geomprop`, `texcoord.index`, `*.space`. They become instruction immediates: enums, a texture handle index, a primvar slot.
  - Closure types (`BSDF/EDF/VDF/surfaceshader`) exist only in the IR. They are eliminated by closure flattening (§4.9).
- **Node:** `{ op, type, inputs[k] (Ref), imm (enums/handles/slots), outputs }`. `Ref = {Node(output), Const, Param, Global, None}`.
  - Multi-output MaterialX nodes (artistic_ior: 2, chiang_hair_roughness: 3, flake2d: 4) write one contiguous row block, with output k at offset `o_k`.
- **Params:** every authored input value of a translated graph, plus nodedef defaults of the material interface, with a stable key (`shaderPath.inputName`). Nodegraph-internal literals are `Const`.
- **Globals** (rows filled by the shade stage only when `globalMask` asks):
  - P, Pobject, N (interpolated shading normal, not face-forwarded, per MaterialX semantics), Ng, Nobject, T, B (world; from authored tangents or dP/du of UV0), I (view direction), time, frame, ray depth, ray type.
  - primId, instanceId, objectId, needed for Nova nodes later.
  - dPdx/dPdy when derivatives are needed.
- **Outputs:** `{closureParamId, Ref}` for the intrinsic uber model or the flattened lobe list, plus `displacement`, `opacity`, `emission`, and AOV outputs later.

### 4.3 Optimizer (order and rules)

1. **Fold literals.** A node with all-`Const` inputs is evaluated by running the real op through the lane executor on a 1-lane program. This is Cycles' `svm_math` reuse; prototype `foldNode`, `<S>/proto/compiler.cpp:21`. The result is bit-identical to runtime.
2. **Identities, exact ones only:** x·1, x+0, x−0, `mix(bg,fg,0)→bg`, `mix(bg,fg,1)→fg`, `Mov` of the same type.
   - With `mix = bg·(1−t)+fg·t` (GLSL/OSL `mix`, which MaterialX genglsl uses), the endpoint rules are exact, but `mix(x,x,t)→x` is **not**.
   - With `bg+(fg−bg)·t`, `t=1→fg` is inexact and catastrophically wrong for |bg| ≫ |fg|.
   - Both failures were found by the prototype's differential test (23 and 9 mismatches respectively), and fixed by choosing the GLSL form and dropping `mix(x,x,t)`.
   - `x·0→0` (Cycles `constant_fold.cpp`, the NODE_MATH_MULTIPLY case) is only valid if ops never produce NaN/Inf. Keep ops NaN-free (safe div/sqrt/log/pow) and keep the existing texture NaN sanitizing.
3. **CSE:** hash of `(op, type, imm, resolved inputs)`, with commutative inputs sorted. Then fold again, because merging exposes identities.
4. **Closure flattening and pruning** (§4.9).
5. **DCE** from outputs (Cycles `clean`).
6. **Uniform analysis:** a node is uniform if no input is varying (Global, texture, primvar, or a varying node). Uniform nodes go into the **prologue**, evaluated on the host into uniform slots whenever parameters change. Varying ops read them as uniform operands.
7. **Derivative marking:** the backward cone of every `texcoord` input of texture and heighttonormal ops is emitted with dual variants (§4.8).
8. **Scheduling and allocation** (§4.4).

Parameters never fold, so IPR never recompiles for value edits. The per-hit cost of parameter-only math is still zero because of hoisting.

### 4.4 Codegen

**Register file:**
- Per lane, rows of floats. CPU layout is `R[row*N + lane]`, and a vecW value occupies W adjacent rows, so every component-wise op is W contiguous lane loops.
- Rows are capped at 1024 (16-bit operands). The allocator minimizes the count so the working set fits L1: 32 lanes × 64 rows × 4 B = 8 KB.
- CPU: a thread-local arena of `1024 × 32` floats (128 KB, only used rows are touched).
- GPU: `float R[maxRows]` in local memory per thread. Cap at about 128–256 rows (Cycles uses 255). Programs above the cap fail compile with a diagnostic and an error material (Cycles: `compile_failed` gives an empty shader, `svm.cpp:1166-1170`).
- No spilling is needed: rows are already memory, L1/L2-resident.

**Allocation:**
- Sethi-Ullman order (Cycles `generate_svm_nodes`), last-use liveness, first-fit search for W contiguous free rows (Cycles `stack_find_offset`).
- Global rows are allocated first and freed at last use. Varying closure-output rows stay live to the end.
- The **destination never aliases an input**; ops like dot/normalize/cross/swizzle read several components after writing (Cycles `svm.cpp:404-409` makes the same choice).

**Instruction (16 B, one `uint4`):**
```cpp
struct Instr { Op op /*u16*/; uint8_t width; uint8_t flags; uint16_t dst, a, b, c; uint32_t imm; };
// flags bit0..2: operand a/b/c is a uniform slot (else a varying row); bit3..5: scalar broadcast over components
```
- Ops with more than 3 inputs (noise, fractal, image, normalmap, remap) are followed by k extension words of `{uint16 operand, flags}`, as Cycles nodes do.
- Uniform operands are read once per op and broadcast inside the lane loop (`batchCwT<..., UA, UB, UC>`, `<S>/proto/vm.h:157`). The 8 uniform-mask variants are template instantiations. On GPU the same operand reads `U[idx]` (one address per warp, broadcast). This replaces Cycles' NaN-boxed inline constants, because values must stay editable without recompiling.

**Uniform block layout:** slots 0–3 are zeros (dummy operands), then constants, then params, then hoisted uniform results. `uniformInit` describes where each slot comes from. The structure hash covers code, layout and constant values, not parameter values.

### 4.5 Execution in the wavefront

**CPU** (`shadeSurfaces` refactor, skeleton in §6.4):
1. **Pass 1 (unchanged logic):** camera-visible lights, misses and escapes, area lights along the ray, alpha. Collect `(rayIndex, materialId)` of surviving hits. The material comes from the mesh, or a per-triangle subset id from Hydra GeomSubsets.
2. **Pass 2:** counting sort by material id (0.6 ns/hit).
3. **Pass 3:** per material group, chunks of `kLanes = 32`.
   - `resolveHit` for each lane, write the requested global rows, pad lanes `[n, 32)` with a copy of lane n−1 (valid data, no NaN or denormal traps).
   - `runMaterial(n)` chooses `runBatch<8|16|32>` by n, or `runLane` for n ≤ 2.
   - Per lane: gather closure parameters (uniform slot or row), build the BSDF (`core/bsdf`), then NEE and continuation exactly as now.
   - Sorting changes queue order but not results: samples depend only on (pixel, sample index, stream), and each path's radiance additions keep their stage order.
4. **Uniform-only materials** (no varying instructions, the common look-dev case) skip the VM entirely; closure parameters come straight from the uniform block.

**Why not per-ray interpretation on CPU:**
- 3 ns/instruction per lane versus 0.15.
- Cycles CPU only does it because its megakernel shades one path at a time.

**Per-lane executor uses on CPU:**
- Opacity evaluation in Embree occlusion filter callbacks (single candidate hits).
- Displacement or host evaluation of single points.
- The differential-test oracle.

**GPU (M10):**
- Per bounce: counting sort (prefix sum) of hit indices by material, Cycles-style with partitioning for locality.
- One generic shade kernel over the sorted index array.
- Each thread runs `runLane(code, R_local, U)`. A warp sees one material, so the `switch` is non-divergent, local-memory rows coalesce, and uniform reads broadcast.
- Branches:
  - Uniform conditions use `JumpIfUniform` (taken by all lanes).
  - "Skip if all lanes have weight 0" uses `JumpIfAllZero` (CPU: test over the chunk; GPU: `__all_sync`).
  - Varying conditions become `Select` (branch-free; MaterialX nodes are pure).
- Later optimization without changing the IR: kernel variants by op-feature mask (Cycles `node_feature_mask`, data-constant specialization), or NVRTC codegen of hot programs (MDL/OSL style).

### 4.6 IPR parameter updates

- `HdNovaMaterial::Sync` rebuilds the IR, which is cheap.
- If the `structureHash` equals the current program's: copy new parameter values into the material's uniform block, rerun the prologue (microseconds), bump `scene.version()`. No code change, texture re-resolution or GPU re-upload of code.
- Otherwise recompile; under 1 ms is expected for a `standard_surface` graph (to be measured).
- Programs are shared by hash across materials (MDL class compilation); every material keeps its own uniform block.
- Texture path changes are uniform immediates (handles), so they count as a structure change. That is acceptable, or handles could be put in the uniform block as integers.
- Uniform blocks may only be written while the render is stopped (existing `Scene` edit rule).

### 4.7 Textures and primvars

**Texture table:**
- The program lists `TextureRef {path, colorspace, uaddress, vaddress, filter, udim, layer, frameoffset/endaction}`.
- The hydra texture manager resolves these to global texture ids.
- The `Tex2D` immediate indexes the material's resolved-id table.
- CPU: a `ITextureSampler` batch interface in core (one virtual call per op per chunk, like `IAccel`), implemented with OIIO `TextureSystem`. OIIO 2.5.18 has the batched overload `texture(TextureHandle*, Perthread*, TextureOptBatch&, RunMask, const float* s, t, dsdx, dtdx, dsdy, dtdy, int nchannels, float* result, ...)` at `OpenImageIO/texture.h:1190`, with `Tex::BatchWidth = 16` (`:130-137`). Call it twice per 32-lane chunk.
- GPU: a handle → `cudaTextureObject_t` or software-filtered tile table (textures topic).

**Primvar slots:**
- The program lists the primvar names it reads (`["st", "displayColor", "myattr"]`); `texcoord(index 0)` maps to `st` (USD convention), `geomcolor` to `displayColor`.
- A mesh stores generic primvars as `{name, interpolation, components, data}`. This generalizes today's `cornerColors`: constant / uniform (needs triangle→face map) / vertex / faceVarying (per triangle corner).
- At `commit()` each (mesh, material) pair builds `primvarRemap[slot] → meshPrimvarIndex | -1` (−1 uses the op's uniform default).
- The `Primvar(slot, w)` op reads per-lane hit references (mesh, prim, barycentrics) from the shade context.
- Hydra should upload all authored float/float2/float3/color primvars, because materials can sync after meshes. displayColor becomes an ordinary primvar.
- The default material when nothing is bound reproduces today's displayColor Lambert, which is the reference scenes' `displayColorLambert` network (`tests/reference/scenes/common.usda`), so existing tests stay valid.

### 4.8 Derivatives (texture filtering, bump)

- **Dual rows,** as Cycles main (`*_DERIVATIVE` nodes, `svm.cpp:1002`) and OSL `Dual2`: values marked "needs derivative" use 3W rows (value, d/dx, d/dy).
- **Rules:** add/sub/neg; mul/div (product/quotient rule); mad; mix (with varying t, include t's derivative); swizzle/extract/combine; matrix·vector (place2d); fract/mod/floor (pass-through, ignoring discontinuities); `Primvar` (d/dx = ∇attr · dPdx, using the attribute gradient on the triangle plane); `P`. Every other op gives a zero derivative and a one-time warning (Cycles `stack_zero_incomplete_derivatives`).
- **Seed in M3** (stateless, no per-path state): pbrt-v4 `Approximate_dp_dxy` (`cameras.h:155-180`) plus least-squares du/dx (`surfscatter.cpp:75-103`). Ray cones are an M7 upgrade (Akenine-Möller et al. 2021).
- **Consumers:** `Tex2D` (OIIO dsdx…dtdy) and `heighttonormal` (its `texcoord` input exists exactly for this), which gives bump without triple evaluation.

### 4.9 Closures

**Intrinsic uber model:**
- `ND_standard_surface_*` and `ND_open_pbr_surface_*` are not flattened. Their inputs become `IrOutput{paramId, value}` with ~68 components per model. For example `geometry_normal` defaults to Nworld, so a normal map simply binds a row.
- After compile each binding is `{id, uniform?, index, width}`. The BSDF stage gathers it per lane into a POD `SurfaceParams`.
- `geometry_thin_walled` is uniform (an immediate or uniform slot).

**Generic pbrlib graphs** (M4 if not needed in M3):
- Flatten mix/add/multiply of closures into per-lobe weight expressions at compile time (Cycles `transform_multi_closure`, `shader_graph.cpp:1101`).
- `layer(top, base)` gives a "layered under" link evaluated by the BSDF (base scaled by 1 − albedo_top(wo), as Cycles' OSL flattener does with `layer_albedo`, `osl.h:105-125`).
- The lobe list is static per program and each lobe's parameters are VM outputs. Lobes are oren_nayar, burley, translucent, dielectric, conductor, generalized_schlick, subsurface, sheen, chiang_hair; EDFs are uniform, conical, measured, generalized_schlick. These should be the same lobe implementations the uber model uses.
- **Equivalence test:** the intrinsic `standard_surface` and its MaterialX nodegraph expanded through the generic path must render statistically equal.

### 4.10 Later: opacity, displacement, emission

- **Opacity (cutout / geometry_opacity):**
  - Compile an opacity-only program (DCE with respect to the opacity output).
  - Shadow rays: run it per lane in Embree filter callbacks.
  - Camera/indirect rays: stochastic pass-through using `stream::kOpacity`.
  - **Integrator dependency:** path depth must become per-path state (`WaveQueues`), because the global `depth` loop counter (`wavefront.cpp:323`) cannot express transparent bounces.
- **Displacement:** a program DCE'd to the displacement output, run by the batch executor on the host over tessellated vertex arrays (positions, normals, UV as input rows), before the BVH build.
- **Emission:** add `Le` at hits (camera and BSDF rays; MIS weight 1 until emissive meshes are light-sampled in M9).

---

## 5. M3 instruction set

Covers the MaterialX source-implemented set after `flattenSubgraphs`. Simple closed-form source nodes are lowered to primitives by the translator. `w` = width 1–4; "u|v" = operand may be a uniform slot or a varying row; splat = scalar broadcast.

Costs: arithmetic rows are **measured** (batch32, AVX2). Other rows are estimates scaled from the measured noise and arithmetic numbers.

| Group | Ops (signature) | MaterialX coverage | Cost / lane |
|---|---|---|---|
| Control | `End`; `JumpIfUniform(slot, off)`; `JumpIfAllZero(row, off)` | conditionals with uniform condition; zero-weight lobe skipping | ~0 |
| Component-wise | `Mov a`; `Add/Sub/Mul a,b`; `Div a,b` (safe); `Mod a,b` (= a − b·floor(a/b)); `Mad a,b,c`; `Min/Max a,b` (ternary); `Clamp x,lo,hi`; `Mix bg,fg,t` (= bg(1−t)+fg·t); `Abs Sign Floor Ceil Round Fract`; `Sqrt` (safe) | add subtract multiply divide modulo min max clamp mix absval sign floor ceil round fract invert (→Sub) remap (→Sub,Mad with hoisted scale) plus minus difference screen burn dodge inside outside (lowered) | **0.10–0.16 ns per component** |
| Compare/select | `Gt Ge Eq a,b` → 0/1; `Select c,a,b`; `And Or Not` | ifgreater ifgreatereq ifequal switch (NG→ifequal chains) and or not xor (NG) | 0.1–0.15 |
| Transcendental | `Pow` (MaterialX pow; safepower is NG), `Exp`, `Ln` (safe), `Sin Cos Tan Asin Acos`, `Atan2` | power exp ln sin cos tan asin acos atan2 | 1–2 ns vectorized polynomial; 10–20 ns scalar libm |
| Smooth | `Smoothstep lo,hi,x` | smoothstep (float source; vector variants NG) | ~0.3 |
| Shape | `Dot a,b` (w→1); `Cross a,b`; `Normalize a`; `Extract a imm`; `Combine2/3/4`; `Swizzle a imm(pattern,outW)` | dotproduct crossproduct normalize magnitude (Dot+Sqrt) extract combine* convert separate* (NG) distance (NG) luminance (Dot with uniform coefficients) | 0.2–0.6 |
| Matrix | `MatVec(m, v, imm point/vector/normal)`; `MatMul`; `Transpose`; `Determinant`; `Inverse`; `CreateMatrix` | transformmatrix creatematrix transpose determinant invertmatrix (rare; low priority) | 1–5 |
| Color | `RgbToHsv`, `HsvToRgb` | rgbtohsv hsvtorgb hsvadjust (NG) | ~2 |
| Rotate | `Rotate2D v, deg`; `Rotate3D v, deg, axis` | rotate2d rotate3d place2d (NG) | ~3 |
| Space | `TransformSpace v, imm(kind, from, to)` (per-lane object↔world from context) | transformpoint/vector/normal, `space` inputs of position/normal/tangent/bitangent | ~2 |
| Geometry | Globals are pre-filled rows (no op); `Primvar slot, w, default(u)` | position normal tangent bitangent texcoord geomcolor geompropvalue(+uniform) time frame UsdPrimvarReader | 5–15 (per-lane interpolation) |
| Procedural | `Noise(dim 2/3, outW 1/2/3/4) amp,pivot,pos`; `CellNoise(dim) pos`; `Fractal(dim,outW) amp,octaves,lacunarity,diminish,pos`; `Worley(dim,outW, imm style/metric) pos,jitter`; `RampLR/TB/SplitLR/TB` (lower to mix/step on uv) | noise2d/3d cellnoise2d/3d fractal2d/3d worleynoise2d/3d unifiednoise* / checkerboard / grid / ramp* (NG) | noise3d **7.1 ns measured (AVX2)**, 60 ns scalar; fractal ≈ octaves × noise; worley 3D ≈ 30–80 (estimate) |
| Texture | `Tex2D(handle imm, uv(dual), default(u), outW, imm flags)` | image, tiledimage/latlongimage/triplanarprojection (NG), UsdUVTexture (NG), hextiledimage (later) | 100–500 ns OIIO (dominant; not measured here) |
| Normal | `NormalMap(in, scale, N, T, B)`; `HeightToNormal(h(dual), scale)` | normalmap (1.39: `in, scale, normal, tangent, bitangent`), bump (NG→heighttonormal) | ~3 |
| Closures | none at runtime: the binding table maps outputs to rows/slots | standard_surface open_pbr_surface surface surface_unlit, pbrlib lobes (flattened) | gather ~20 ns per lane |

That is about 60 opcodes. Heavy ops (noise family, Tex2D, Primvar, HSV, rotate, matrix) get hand-written 8-wide CPU kernels, or per-lane calls for texture and primvar. The arithmetic families are generated by the `Fn<Op>` template (prototype `vm.h:57-83`).

**Typical costs** (per hit, estimates from the measurements):
- Textured `standard_surface` (3 images, place2d, normal map; about 40 varying instructions): VM ≈ 40 × 0.2 ns ≈ 8 ns + 3 textures ≈ 0.3–1.5 µs. The VM overhead is negligible.
- Procedural-heavy graph (fractal3d with 4 octaves plus 30 math ops): about 35 ns batch versus about 330 ns per-lane scalar.

---

## 6. Code skeletons

Compile-checked versions are in `<S>/proto/` (MSVC C++20, all tests pass). Line references: `vm.h:39` `Instr`, `:113` `runLane`, `:157` `batchCwT`, `:194` `runBatch`; `compiler.cpp:21` foldNode, `:56` fold, `:102` cse, `:133` DCE, `:143` uniform analysis, `:159` slots, `:199` Sethi-Ullman, `:242` liveness and allocation, `:299` structure hash, `:314` evalUniforms; `test_proto.cpp:18` RefEval, `:66` randomGraph, `:193-327` tests.

### 6.1 `core/shading/program.h` (POD, shared with CUDA)

```cpp
namespace nova::shading {
enum class Op : uint16_t { End, Mov, Add, Sub, Mul, Div, Mod, Mad, Mix, Clamp, Min, Max, Abs, Sign, Floor, Ceil,
    Round, Fract, Sqrt, Pow, Exp, Ln, Sin, Cos, Tan, Asin, Acos, Atan2, Smoothstep, Gt, Ge, Eq, Select, And, Or,
    Not, Dot, Cross, Normalize, Extract, Combine2, Combine3, Combine4, Swizzle, MatVec, MatMul, Transpose,
    Determinant, Inverse, RgbToHsv, HsvToRgb, Rotate2D, Rotate3D, TransformSpace, Primvar, Noise, CellNoise,
    Fractal, Worley, Tex2D, NormalMap, HeightToNormal, JumpIfUniform, JumpIfAllZero, Count };
constexpr uint8_t kUniformA = 1, kUniformB = 2, kUniformC = 4, kSplatA = 8, kSplatB = 16, kSplatC = 32;
constexpr uint8_t kDual = 64;                       // operands/result carry dx, dy rows (3*w rows)
struct Instr { Op op; uint8_t width, flags; uint16_t dst, a, b, c; uint32_t imm; };   // 16 B
static_assert(sizeof(Instr) == 16);

enum class Global : uint8_t { P, Pobject, N, Ng, Nobject, T, B, I, Time, Frame, RayDepth, RayType,
                              PrimId, InstanceId, dPdx, dPdy, Count };
struct OutputBinding { uint16_t param; uint8_t width; uint8_t uniform; uint16_t index; };

// Flat, device-uploadable view of one material (all pointers into MaterialTable arrays).
struct MaterialView {
    const Instr *code;            // varying program, ends with End
    const float *uniforms;        // uniform block of this material
    const OutputBinding *outputs; uint16_t numOutputs;
    uint16_t numRows;             // varying rows needed
    uint32_t globalMask;          // bit g: shade stage writes rows globalRow[g]
    uint16_t globalRow[uint32_t(Global::Count)];
    const uint32_t *textures;     // material texture slot -> global texture id
    uint16_t primvarSlots;        // remap table per (mesh, material) lives in the mesh record
    uint8_t closureModel;         // OpenPBR / StandardSurface / LobeList
    bool varyingFree;             // code is just End: closure params all uniform
};
}
```

### 6.2 Batch executor core (CPU, `vm_batch.h`)

```cpp
template <Op O, int N, bool UA, bool UB, bool UC>
inline void batchCwT(const Instr &in, float *__restrict R, const float *__restrict U) {
    for (int k = 0; k < in.width; ++k) {
        const int ka = (in.flags & kSplatA) ? 0 : k, kb = (in.flags & kSplatB) ? 0 : k, kc = (in.flags & kSplatC) ? 0 : k;
        const float *__restrict a = UA ? U + in.a + ka : R + (in.a + ka) * N;
        const float *__restrict b = UB ? U + in.b + kb : R + (in.b + kb) * N;
        const float *__restrict c = UC ? U + in.c + kc : R + (in.c + kc) * N;
        float *__restrict d = R + (in.dst + k) * N;
        const float ua = a[0], ub = b[0], uc = c[0];           // loop-invariant broadcasts
        for (int l = 0; l < N; ++l)                             // vectorizes (MSVC C5001)
            d[l] = Fn<O>::eval(UA ? ua : a[l], UB ? ub : b[l], UC ? uc : c[l]);
    }
}
// runBatch<N>: switch (in.op) { case Op::Add: dispatch on (in.flags & 7) -> batchCwT<Op::Add,N,...>; ...
//   case Op::Noise: noiseKernel<N>(in, ext, R, U);   // 8-wide AVX2 (core/math/simd8.h), N/8 iterations
//   case Op::Tex2D: ctx.textures->lookup(handle, N, s, t, dsdx.., out) // OIIO batched, 2 x 16 lanes
//   case Op::Primvar: for (l < n) interpolatePrimvar(ctx.hit[l], remap[slot], out + l, N); }
```

Scalar op semantics live in `ops.h`, as in prototype `vm.h:52-83`: `opMin(x,y){return x<y?x:y;}`, `opMix(bg,fg,t){return bg*(1-t)+fg*t;}`, safe `opDiv/opSqrt`. The per-lane executor (`vm_lane.h`, `NOVA_HOST_DEVICE`) calls the same `Fn<O>::eval` with `ld(idx, uniform, k, splat) = uniform ? U[idx+c] : R[idx+c]` (prototype `vm.h:98-140`).

### 6.3 Compiler pipeline (`core/shading/compiler.cpp`)

```cpp
Program compile(const IrGraph &in, const CompileOptions &opt) {
    Compiler c(in);
    c.lowerSimpleNodes();      // remap/invert/screen/... -> primitives; insert Convert/Splat/Swizzle
    c.fold(); c.cse(); c.fold();               // literals only; exact identities only
    c.flattenClosures();       // mix/add/multiply/layer -> lobe weights; prune const-zero lobes
    c.deadCode();              // from outputs (closure params, displacement, opacity, AOVs)
    c.uniformAnalysis();       // -> prologue
    c.markDerivatives();       // backward cone of Tex2D.uv / HeightToNormal.texcoord
    c.scheduleSethiUllman();   // producers by (need - outWidth) desc
    c.allocateRows();          // first-fit contiguous, free at last use, dst != src, outputs pinned
    return c.emit();           // prologue, code, uniformInit, constValues, bindings, structureHash
}
```

Allocation core (prototype `compiler.cpp:242-297`): `alloc(w)` scans `used[]` for w free rows. After emitting instruction `pos`, rows of inputs with `lastUse == pos` are freed (deduplicated). Global rows are allocated before the first instruction.

### 6.4 `shadeSurfaces` integration (`core/integrator/wavefront.cpp:169`)

```cpp
struct ShadeScratch {                      // in WaveQueues (per thread)
    std::vector<uint32_t> hitRay, hitKey, order, groupStart;
    std::vector<float> rows;               // kMaxRows * kLanes
    SurfaceHit hit[kLanes];
};
void shadeSurfaces(const Scene &scene, ..., WaveQueues &q) {
    // pass 1: existing light/miss/alpha code; survivors -> s.hitRay[n], s.hitKey[n] = scene.materialOf(mesh, prim)
    countingSort(s.hitKey, s.hitRay, n, scene.materialCount(), s.order, s.groupStart);   // 0.6 ns/hit
    for (uint32_t m = 0; m < scene.materialCount(); ++m)
        for (uint32_t c = s.groupStart[m]; c < s.groupStart[m + 1]; c += kLanes) {
            const MaterialView mat = scene.material(m);
            const uint32_t n = std::min<uint32_t>(kLanes, s.groupStart[m + 1] - c);
            for (uint32_t l = 0; l < n; ++l) {
                const uint32_t k = s.order[c + l];
                s.hit[l] = scene.resolveHit(q.hitMesh[k], q.hitPrim[k], q.hitUv[k], q.hitT[k], q.hitInstance[k], q.rays.time[k]);
                writeGlobals(mat, s.rows.data(), kLanes, l, s.hit[l], q.rays.dir[k]);  // only mat.globalMask
            }
            padLanes(mat, s.rows.data(), n);                 // copy lane n-1 into [n, kLanes)
            ShadeContext ctx{&scene, s.hit, n, threadTextureState()};
            if (!mat.varyingFree) runMaterial(mat, s.rows.data(), ctx, n);   // <8>/<16>/<32> or runLane
            for (uint32_t l = 0; l < n; ++l) {
                SurfaceParams sp; gatherParams(mat, s.rows.data(), kLanes, l, sp);   // uniform slot or row
                // emission, opacity (later), then: Bsdf bsdf = makeBsdf(sp, frame, wo);
                // ... existing NEE (wavefront.cpp:246-268) and continuation (:270-291) with LambertBsdf -> bsdf
            }
        }
}
```

---

## 7. Pitfalls

1. **MSVC vectorization:**
   - Use ternary `min/max/select`, never `std::fmin/fmax` in lane loops.
   - libm calls do not vectorize under `/fp:precise` (reason 1200). Either build `vm_batch.cpp` alone with `/fp:fast` (measured 0.30 ns versus ~1 ns) or, preferably, use own polynomial `sin/cos/exp/log/pow` in `ops.h`, which are identical on CPU and GPU.
   - Check `/Qvec-report:2` in CI for the VM translation unit.
2. **Complex ops never auto-vectorize** (noise: reason 1305). Write explicit 8-wide kernels with a small own `core/math/simd8.h` (AVX2 intrinsics), which keeps `nova_core` free of OIIO. The OIIO `simd.h` version measured 7.1 ns with bit-identical results.
3. **FMA contraction:** nvcc contracts by default (`--fmad=true`). Use `--fmad=false` or explicit `fmaf` on both sides if CPU/GPU bit-equality is wanted. Prototype lane and batch paths are bit-exact under `/fp:precise`.
4. **Identity folding must be exact for the chosen formula** (`mix` example in §4.3). The constant folder must call the runtime op code, never a separate host implementation.
5. **`x·0 → 0` and NaN:** make every op NaN-free (safe div/sqrt/log/pow, clamp acos/asin domains). Textures are already sanitized (`hydra/textureCache.cpp:122-134`).
6. **Uniform operands in "special" ops:** every op must accept uniform operands, because `hoistUniform=false` or a partial hoist can produce them. The prototype's batch `Normalize` read a uniform slot as a row and crashed in release; the differential test caught it.
7. **No dst/src aliasing,** or only for ops whose output component k depends on input component k alone.
8. **Partial chunks:** pad with a valid lane. FTZ/DAZ are already on (`renderer.cpp:94`).
9. **Small groups:** the 1024-path waves fragment at deep bounces. Use adaptive N and per-lane execution for n ≤ 2. Larger tiles (64×64) are an option later.
10. **Ints in float rows:** exact only to 2^24. `round/floor/ceil` integer variants must return exact integers. MaterialX `integer` inputs such as `octaves` can be connected, so ops must take them as values.
11. **MaterialX uniform inputs** (strings, filenames, enums) must be immediates. A connected uniform input is invalid: report it and use the default.
12. **Normals:** MaterialX `Nworld` is not face-forwarded. Face-forwarding belongs in the BSDF, as in today's code (`wavefront.cpp` uses `faceForward(hit.N, ng)` after resolveHit). Do not bake it into the N global, or normal maps flip on back faces.
13. **Transparency or cutouts** need per-path depth state before M3 can support `geometry_opacity` (§4.10).
14. **Structure hash** must include constant values and all immediates (texture handles, enums, primvar slots), but not parameter values.
15. **Deep graphs:** the prototype recurses (mark, Sethi-Ullman). Production code should use iterative passes for graphs with thousands of nodes.

---

## 8. Test plan

1. **Compiler unit tests** (`tests/test_shading_compiler.cpp`, from the prototype; Catch2):
   - Folding: `2·3 + P.x` → 3 instructions (Extract, Add with uniform 6, ...).
   - Identities: x·1 bypass, `mix(·,·,0/1)`.
   - CSE: two `sin(b)` → one.
   - DCE: removed count.
   - Register reuse:

     | Case | Bound | Measured |
     |---|---|---|
     | 400-op chain | ≤ globals + 3 rows | 6 rows, 5 of them globals |
     | Balanced tree, depth 7 | ≤ P(3) + depth + 1 rows | 9 rows |

   - Hoisting: param-only subgraph → 0 varying instructions; prologue output updates after a parameter edit with the same structure hash; an op change changes the hash.
   - Compile time per graph < 1 ms (`standard_surface` with 3 textures).
2. **Differential testing** (`tests/test_shading_vm.cpp`):
   - Seeded random typed DAGs: 3000 graphs, 5–400 nodes, 45 lanes.
   - Reference is a tree-walking evaluator of the **unoptimized IR**.
   - Must match **bit-exactly** (NaN == NaN): `runLane`, `runBatch<8/16/32>` with partial chunks, and every optimization toggle off.
   - Log the seed and shrink failing graphs by deleting nodes while the failure persists.
   - Extend the generator to every opcode as it lands, including dual variants (compare derivatives against finite differences with tolerance).
3. **Op semantics against MaterialX:**
   - Port the genglsl reference functions verbatim into the test as oracles (`$HFS/houdini/materialx/libraries/stdlib/genglsl/lib/mx_noise.glsl:265` `mx_perlin_noise_float`, etc.) and compare on random inputs. Bit-exact for integer-hash ops; ≤2 ulp for polynomial transcendentals.
   - **Karma node probes** (`tests/reference/scenes/node_*.usda`): `ND_surface_unlit` with `emission_color` driven by the node under test, orthographic camera, `pixelfilter point`. Compare Nova and Karma per pixel (validated here: texcoord 6e-8, noise3d ≤1e-6). One scene can probe many nodes in tiles.
4. **Shading equivalence:**
   - `standard_surface` with constant params through the VM must be bit-identical to the BSDF built directly from the same values.
   - White furnace (base_color 1, specular 0) through the VM equals the existing Lambert furnace.
   - Intrinsic versus expanded-nodegraph `standard_surface` via `nova_imgcmp` once the generic path exists.
   - The displayColor default material keeps all current reference tests green.
5. **Determinism:** material sorting must not change images. Run existing determinism and crop tests, plus one multi-material scene with tiles versus full frame.
6. **Microbenchmarks** (`tools/` or `tests/bench`, not gating or with loose thresholds):
   - ns/(instr·lane) per op class at N = 8/16/32/64.
   - noise kernels; shade-stage ns/hit for representative materials.
   - Sort cost; compile time; IPR parameter-edit latency (< 1 ms engine side; the plan's target is < 100 ms end to end).
   - `<S>/bench/bench_vm.cpp` and `noise_simd.cpp` are ready-made starting points.

---

## 9. Files produced

All under `<S>`; none in the project.
- `src/{cycles,pbrt,osl,mdl}/…`: downloaded source excerpts cited above.
- `houdini_help/{vex,solaris}/…`: extracted help pages.
- `scripts/mtlx_inventory.py`, `mtlx_types.py`, `ng_stats.py`, `count_inputs.py`: MaterialX library inventory (run with `$HFS/python313/python.exe -I`).
- `bench/bench_vm.cpp` (+ `build*.bat`, logs with `/Qvec-report:2`), `bench/noise_check.cpp`, `bench/noise_simd.cpp`.
- `proto/vm.h`, `proto/compiler.h`, `proto/compiler.cpp`, `proto/test_proto.cpp`, `proto/build.bat`: working prototype; last run `ALL PASSED (0 failures)`.
- `karma_probe/probe.usda`, `karma_probe/out/probe.exr`, `scripts/read_probe.py`: Karma CPU node-probe check.