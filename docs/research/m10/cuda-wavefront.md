# M10 research: porting Nova's wavefront integrator to CUDA kernels (CUDA 12.9, RTX 5080)

Scope: the kernel part of docs/PLAN.md M10 ("GPU-Backend: CUDA-Wavefront-Kernels + OptiX-Accel, gleicher Bytecode-Interpreter auf der GPU | XPU-artiger Modus"). OptiX acceleration structures and ray queries are covered by another M10 report. Here intersection is a black box: a function that takes a device SoA ray queue and writes (t, mesh, prim, instance, uv) per ray, plus the occlusion variant.

This report feeds the binding M10 design document. It implements nothing in the renderer and changes nothing outside `docs/research/m10/`.

It answers eight questions:
1. Kernel decomposition: which passes become which kernels, the state between them, queue compaction, and how material-sorted shading maps to the GPU.
2. The bytecode VM on the GPU: one lane per thread, register file, uniforms, measured throughput against a hand-written kernel and the CPU.
3. Textures on the GPU: texture objects, demand loading or resident MIP chains, UDIMs.
4. Volumes: NanoVDB on the device, majorant grids, tracker divergence.
5. Memory: path state, paths in flight, queue double buffering, film accumulation, AOVs and LPE outputs.
6. Determinism and CPU/GPU agreement, and how to test it.
7. What cannot run on the GPU, and an "XPU-like" mode.
8. Code sharing: the macros, std containers, virtual interfaces.

It ends with a recommended M10 architecture and porting order (section 10) and open questions (section 11).

Sources:
- Nova's code, read for this report: `core/integrator/wavefront.{h,cpp}`, `material_eval.{h,cpp}`, `volume_media.{h,cpp}`, `core/shading/{vm,ops,program}.h`, `core/lights/{lights,light_tree}.h`, `core/film/{lpe,film,crypto}.h`, `core/volume/{grid,majorant}.h`, `core/accel/accel.h`, `core/texture/texture.h`, and docs/design/M3.md (K5, K14, K15, the "CUDA mapping" paragraph of section 4), docs/design/M9.md, docs/research/m3/plans/plan-gpu-perf.md section 2.6 and docs/research/m3/textures.md section 5;
- probes built and run on this machine: an NVIDIA GeForce RTX 5080 (84 SMs, sm_120, 16 GB, driver 617.42, Windows 11, WDDM) with CUDA 12.9 (nvcc V12.9.86) and the VS 2022 Build Tools (MSVC 14.44) as host compiler. The CPU numbers are from the AMD Ryzen 7 9800X3D (8 cores; `std::thread::hardware_concurrency()` reports 8), MSVC `/O2 /arch:AVX2`;
- Houdini 22.0.466's help (`$HFS/houdini/help/solaris.zip`: `karma_xpu.txt`), paraphrased;
- the literature and open-source code: Laine, Karras and Aila 2013 (the PDF was read for this report), pbrt-v4's `src/pbrt/wavefront/{integrator.cpp,workqueue.h,surfscatter.cpp}` and Cycles' `src/integrator/path_trace_work_gpu.cpp` (read for this report). Other items are marked "from memory".

Every claim carries its source:
- **(measured)**: probe output in `probes/cw_out_*.txt`;
- **(from code file:line)**: Nova's code;
- **(help)**: Houdini's help, paraphrased;
- **(lit)**: literature or source code read for this report; **(lit, from memory)**: not re-checked;
- **(design)**: a proposal.

GPU timings vary by 10-20 % between runs (the card also drives the display under WDDM). Where two runs differed, both values are given as a range. Probe files are in `probes/` with the prefix `cw_` (section 12).

---

## 0. Findings in short

1. **Nova's bytecode VM runs unchanged on the GPU (measured).** `shading::execute<1>` from `core/shading/vm.h` compiles for sm_120 and runs one lane per thread.
   - A representative 60-instruction program (3 texture lookups, Perlin noise, HSV, pow/sin/exp/atan2, normal mapping; 112 component operations, 72 rows) costs 0.47-0.61 ns per lane on 1M lanes, which is 100-127 G bytecode instructions/s.
   - A hand-written kernel of the same math costs 0.137 ns per lane: the interpreter is 3.6-4.5x slower. With hardware texture lookups the gap is 0.57-0.68 against 0.155 ns, again about 3.7-4.4x.
   - The CPU batch executor (`execute<32>`) runs the same program at 2.2-2.3 ns per instruction and lane on one thread and 0.50-0.56 on 8 threads. The GPU VM is 62-68x faster than all 8 cores. On an arithmetic-only variant it is 31-45x faster.
2. **Material sorting is required, a material key radix sort costs almost nothing, and one shade kernel per bounce is enough (measured).**
   - Warps that mix programs serialize them: with lanes picking randomly among 2 / 4 / 8 / 32 different 60-instruction programs, the VM takes 1.7x / 3.1x / 5.6x / 12.1x longer than with lanes sorted by program.
   - CUB's stable radix sort of 2,073,600 (material, ray) pairs, restricted to the key bits in use, takes 0.037-0.079 ms. A full 32-bit sort takes 0.09-0.15 ms.
   - A kernel launch costs 5.4-7.3 µs on a WDDM stream and 0.40-0.80 µs inside a CUDA graph. One launch per material (pbrt-v4's and Laine's pattern) would cost 1.4 ms per bounce at 256 materials. A single interpreter kernel over the sorted queue (Cycles' pattern) avoids that.
3. **Queue compaction is not a bottleneck (measured).**
   - Compacting 2M indices costs 16-24 µs with CUB `DeviceSelect` (stable) or a block-aggregated atomic, and 38-56 µs with per-thread atomics. nvcc already aggregates a plain `atomicAdd(&counter, 1)` per warp: the explicit ballot version measured the same.
   - Pushing 32-byte rays at a warp-aggregated slot from the producing kernel (pbrt-v4, Laine) costs 0.05-0.08 ms. Fixed slots followed by a select and a gather cost 0.12-0.40 ms. Recommended: fused appends.
4. **Uniforms belong in global memory, read-only; code in global memory; the register file in local memory (measured).**
   - Uniforms in constant memory are 16-19 % faster when the warp reads one material's block (2 % on the random programs of the divergence test). When lanes of a warp read different uniform blocks (K = 32), they are 2.7x slower than global memory, because constant-cache reads to different addresses serialize.
   - Code in constant memory was slower than code in global memory (0.66-0.67 against 0.47-0.50 ns per lane).
   - A register file in shared memory, with an odd stride and no bank conflicts, was 2x slower than local memory, because it limits occupancy.
5. **Device-side structures are large: ClosureList is 2368 bytes, SurfaceInputs 1552 bytes (measured, the same size on host and device).** With a 72-row register file the VM kernels already use 72-128 registers and 192-560 bytes of stack.
   - A full shade kernel holds about 4-5 KB of local memory per thread.
   - CUDA reserves that much for every resident thread: about 0.5-0.65 GB on this card (design estimate).
6. **Today's WaveQueues use 648 bytes per path slot (measured from the element types).** That is 1.34 GB for a 1080p wave at 1 spp (2,073,600 paths). Laine et al. kept 1M paths of 212 bytes and found 8M paths only about 5 % faster (lit). pbrt-v4 renders 1M samples per pass (lit). A wave of 1-2M paths fits a 16 GB card comfortably.
7. **Film and LPE accumulation must be owned per pixel, not done with atomics in shading order (measured).**
   - Adding 2M paths' radiance with atomics in pixel order costs the same as owned writes (0.03 ms).
   - In shuffled order, which is what material-sorted shading produces, the same atomics cost 7x more.
   - With 9 LPE outputs they cost 29x more (24 ms against 0.83 ms), and at 4 samples per pixel per wave 65x more (104 ms against 1.6 ms).
   - Recommended: per-path accumulators (radiance, alpha, LPE outputs) and one owned flush per wave, as `renderWave` already does for the beauty (from code `wavefront.cpp:2415-2421`).
8. **Textures: hardware lookups are fast and bindless handles cost nothing when they are warp-uniform (measured).**
   - tex2DGrad on RGBA16F reaches 348-354 G lookups/s with coherent coordinates and 49 G/s with random coordinates on cached MIP levels. Random lookups at level 0 of a 4K texture (128 MB working set) reach 8.6 G/s, and anisotropic 8:1 random lookups 3.9 G/s.
   - Texture handles read from a table per warp cost nothing (53-65 G/s). Handles that differ per lane cost 7-8x.
   - Hardware bilinear weights have 8 fractional bits: the maximum error is 1/512 of the step between two texels, with 257 distinct values (measured). This is the artifact Karma XPU's help warns about for normal maps.
   - A software bilinear filter over float4 data in global memory is faster than the hardware one for float4 textures (139 against 96 G lookups/s).
   - A 4K RGBA16F MIP chain takes 180 MB and 61-69 ms to allocate and upload from pageable memory.
9. **NanoVDB 32.6 (Houdini's copy) works on the device (measured).**
   - Trilinear sampling with `ReadAccessor` + `SampleFromVoxels<1>` compiles for sm_120. It reaches 56 G lookups/s along rays and 10 G/s at random points.
   - Trackers on a 3.9M-voxel cloud take 0.37-0.54 ms per 1M coherent rays and 0.62-0.85 ms per 1M shuffled rays, at a SIMD efficiency of 28-59 %.
   - A majorant grid cuts tentative collisions from 9.7 to 2.1 per ray but saves only 13 % of time here, because the DDA dominates.
   - Nova's own `MajorantDda` does not compile as device code: `MajorantLayout::index` lacks `NOVA_INLINE` (measured; from code `majorant.h:32`).
10. **CPU/GPU agreement is limited by FMA contraction, then by the math library (measured).**
    - With nvcc's default `--fmad=true`, 33 % of the VM outputs differ from the CPU (max 2.2e-6). With `--fmad=false`, 2.2 % differ (max 1.5e-8), only in outputs that went through `sinf`/`powf`.
    - MSVC does not contract: CPU `execute<1>` and `execute<32>` are bit-identical.
    - `--fmad=false` cost nothing measurable in the VM probe.
    - Images can only agree statistically: tiny differences flip discrete decisions (Russian roulette, lobe choice, edge hits). nova_imgcmp's block tests (CPU renders as the reference) are the right tool.
11. **Already device-ready (measured, compiled with `-Werror=cross-execution-space-call`):** besides what `tests/cuda/kernel_headers.cu` covers, `lights.h`, `light_tree.h` (LightTreeView), `light_sampler.h`, `distribution.h`, `lpe.h`, `phase.h`, `subsurface.h` (`randomWalk` with a device lambda tracer) and `hair.h`.
    - Not device-ready: everything in `wavefront.cpp` and `volume_media.cpp` (std::vector push_backs, `thread_local` vectors, `std::sort`), `Scene::resolveHit` (host data structures), `Program` (std::vector), and the virtual interfaces `ITextureSampler`, `IShadingQueries`, `IHitFilter`.
12. **Toolchain traps (measured).**
    - CUDA 12.9's CUB does not compile for `-arch=sm_120` on Windows. The cause is an inline-asm `"l"` constraint on a 32-bit `long` in `cuda/__ptx/instructions/generated/clusterlaunchcontrol.h`. Workaround: `-gencode arch=compute_90,code=sm_120` for translation units that use CUB.
    - NanoVDB 32.6's host-side builder (`util/GridBuilder.h`) needs C++17 (`std::result_of`) and `--extended-lambda`. The device-side sampling headers compile in C++20.
13. **XPU (help, paraphrased).** Karma XPU renders with OptiX GPU devices and an Embree CPU device together and shows each device's share of the image. It aims for the same result whatever the device mix, picks up the work of a failed device (for example a GPU out of memory), and compiles shaders per device. For Nova the measured 31-68x GPU/CPU ratio means a CPU device would add only a few percent of throughput. Its value is fallback and failover.

---

## 1. Nova today (from code)

`renderWave` (from code `wavefront.cpp:2374-2427`) renders one tile per call on one TBB thread.
- **Setup.** It runs the first-hit AOV pass for pixels without samples (`renderFirstHitAovs`), then the camera rays (`generateCameraRays`, one slot per pixel sample: slot = pixel × spp + j).
- **Bounce loop.** Then, per depth:

| Pass | Function (wavefront.cpp) | Reads | Writes | Host-only constructs |
|---|---|---|---|---|
| Intersect | `intersectRays` :257 | `rays` | `hit*` (24 B per ray) | `IAccel` (virtual, batched) |
| Volumes along rays | `traceMedia` :916 | rays, hits, path state | `mediumT`, crossings (push_back), volume events, emission into `radiance` and the film (LPE); equiangular shadow rays | `MediaWalker` `thread_local std::vector` (volume_media.cpp:88-92), `VolumeEvalCache` (unique_ptr per volume) |
| A: lights and escapes | `gatherLights` :1080 | rays, hits, media results | `radiance`, `beta`, film (alpha, crypto, LPE), `shadeRay/shadeMaterial`, `volumeShade` | appends by `++count` |
| B: sort | `sortByMaterial` :1235 | `shadeMaterial` | `shadeOrder` (stable counting sort; skipped for a single material) | `std::vector::assign` |
| C: surfaces | `shadeHits` :1876 → `shadeHit` :1557 | sorted hits, 32-lane chunks per material | shadow queue (up to 3 per path), `nextRays`, `walks`, AO rays, path state, film (LPE) | `evaluateSurfaceInputs` (Scene, MaterialRecord vectors), `ExternalBinding` (CVEX), LPE parts (push_back) |
| C'': SSS walks | `walkSubsurface` :2059 | `walks` | shadow queue, `nextRays` | `Scene::intersectLocal` (IAccel) |
| C': volume events | `shadeVolumes` :1956 | `volumeShade` | shadow queue, `nextRays` | as above |
| Shadow | `traceShadowRays` :2291 (+ `emitterShadowRadiance` :2235, `fakeCausticTransmittance` :2195) | shadow queue | `radiance`, film (LPE) | `thread_local` vector + `std::sort` (emitters), a closest-hit loop with material evaluation (fake caustics) |
| AO | `traceAoRays` :2361 | `aoRays` | utility plane | |

After the loop the path radiance goes to `film.colorSum[pixel]` in slot order (:2415-2421).

What the GPU port inherits for free:
- Everything is already SoA and pass-structured, and every path has at most one ray per bounce, so pass order never changes a path's sum (comment at :615-617).
- Sampling is stateless: Owen-scrambled Sobol by (pixel seed, sample index, stream) and PCG streams per decision (from code `sobol.h:1-20`), so no RNG state moves between kernels.
- The light tree, the power table, distributions and the LPE automaton are already exposed as POD views (`LightTreeView`, `LightSamplerView`, `Distribution1DView`, `lpe::Program`).

What it does not inherit:
- On the CPU, a tile belongs to one thread. Film writes (alpha, cryptomatte, LPE deposits, AO) therefore go straight into the film without synchronization (comment at :139-141).
- On the GPU the same pixel's samples and LPE deposits come from many threads in shuffled order (section 6).

---

## 2. Kernel decomposition (question 1)

### 2.1 Published wavefront designs

**Laine, Karras, Aila 2013, "Megakernels Considered Harmful" (lit).**
- A pool of 1M (2^20) paths stays alive at all times: every iteration advances each path by one segment, and terminated paths are regenerated in the same iteration.
- Path state is 212 bytes per path in global memory, 212 MB in all. Growing the pool from 1M to 8M paths (1.7 GB) gained only about 5 %.
- There are three stages:
  - a logic kernel over all paths (MIS weights, throughput, accumulating light-sample contributions, Russian roulette);
  - material kernels, one per material, fed by queues;
  - ray-cast kernels for new paths, extension rays and shadow rays.
- Queues are preallocated index buffers with a global counter (4 MB each).
- Two measured effects:
  - Warp-coalescing the atomic increments with ballots gave +40 % total, +32 % on ray casts from better coherence.
  - SoA instead of AoS path state gave +80 % total.
- Motivation: divergence and register pressure in a megakernel with expensive materials, and instruction-cache thrashing.

**pbrt-v4's GPU wavefront integrator (lit).**
- Queue size: `maxSamples = 1024 * 1024` samples per pass (scanlines per pass = maxSamples / width). Every queue has `maxQueueSize = width × scanlinesPerPass` entries.
- Per depth: `GenerateRaySamples`, `IntersectClosest` (OptiX), `SampleMediumInteraction`, `HandleEscapedRays`, `HandleEmissiveIntersection`, `EvaluateMaterialsAndBSDFs`, `TraceShadowRays`, `SampleSubsurface`. `UpdateFilm` runs once per pass after the depth loop.
- Queues append with a relaxed `cuda::atomic` fetch_add per item (`WorkQueue::AllocateEntry`).
- `ForAllQueued` launches the maximum queue size and lets threads past the current size exit, so the host never reads queue sizes back.
- Materials are not sorted. A `MultiWorkQueue` holds one queue per material *type*, and `EvaluateMaterialsAndBSDFs` launches one kernel per (material type, texture evaluator) pair that has work. Each launch is a compile-time specialized material: there is no interpreter.

**Cycles (lit, read for this report; details from `path_trace_work_gpu.cpp`).**
- Kernel and state:
  - Path states live in SoA ("integrator state"). How many run concurrently is decided per device from the single-state size (`estimate_single_state_size`, `num_concurrent_states`).
  - Kernels are separate (`INTEGRATOR_INTERSECT_CLOSEST`, `_SHADOW`, `_SUBSURFACE`, `SHADE_SURFACE`, `SHADE_SURFACE_RAYTRACE`, `SHADE_VOLUME`, `SHADE_SHADOW`, `SHADE_LIGHT`, ...). The host always runs the kernel with the most queued paths (`get_most_queued_kernel`).
  - New camera paths are added when intersect-closest is the most queued kernel. That is path regeneration.
  - States are compacted when the live paths are scattered (`COMPACT_STATES`).
- Sorting: paths are sorted by shader before `SHADE_SURFACE` and `SHADE_SURFACE_RAYTRACE`, with a bucket pass and a write pass per partition, or a prefix sum over `max_shaders` buckets.
- Shading: one shade kernel interprets the SVM (shader virtual machine) bytecode for all shaders (lit, from memory). Materials with ray-tracing nodes (AO, bevel) are shaded by the `_RAYTRACE` variant, which is an OptiX program that can trace (`KERNEL_FEATURE_NODE_RAYTRACE`).

**Karma XPU.** Its kernel structure is not public. The help says it compiles shaders into device-specific formats, caches the results, and takes 1-2 minutes on first use or a new driver (help).

**What this means for Nova.**
- Nova already has the Cycles shape: a bytecode interpreter, a sort by material, one shade pass.
- pbrt-v4 and Laine specialize code per material and launch per material. Nova's materials are data for one interpreter, so the per-material launch only buys launch overhead (2.5).

### 2.2 Proposed kernels per bounce (design)

Kernel kinds:
- "CUDA" = a plain kernel.
- "OptiX" = a launch of an OptiX raygen program. It is needed wherever the code traces rays itself.
- Intersection launches belong to the OptiX report.

| # | Kernel | Kind | Input | Output | CPU counterpart |
|---|---|---|---|---|---|
| 0 | `generateCamera` | CUDA | wave description | path state init, `rays` | `generateCameraRays` |
| 0a | `firstHitAovs` | CUDA + intersect | pixel-centre rays | utility planes, albedo/normal (owned pixels) | `renderFirstHitAovs` |
| 1 | `intersectClosest` | OptiX | `rays` | `hit*` | `intersectRays` |
| 2 | `traceMedia` (only with active volumes) | CUDA | rays, hits | `mediumT`, crossings, volume events, emission into path accumulators, equiangular shadow rays | `traceMedia` |
| 3 | `gatherLights` (pass A) | CUDA | rays, hits | path accumulators, shade queue (key, ray), volume-event queue | `gatherLights` |
| 4 | `sortShade` | CUB radix sort | shade queue | sorted (key, ray) | `sortByMaterial` |
| 5 | `shadeSurface` (pass C) | CUDA, persistent | sorted queue | shadow queue, `nextRays`, walk queue, AO rays, path state | `shadeHits`/`shadeHit` |
| 5q | `shadeSurfaceQuery` | OptiX | the sorted key range of materials with `Program::needsQueries` | as 5, with inline traces for the VM's Query op | the same, with `SceneShadingQueries` |
| 6 | `walkSubsurface` (pass C'') | OptiX | walk queue | shadow queue, `nextRays` | `walkSubsurface` (instance-local traces in a loop) |
| 7 | `shadeVolumes` (pass C') | CUDA | volume-event queue | shadow queue, `nextRays` | `shadeVolumes` |
| 8 | `traceShadow` | OptiX (occlusion, any-hit opacity, shadow-link mask) | compacted shadow list | `occluded` per slot | `occluded()` |
| 9 | `emitterShadow` | sort + CUDA | shadow slots aimed at emissive triangles | contribution × Le | `emitterShadowRadiance` |
| 10 | `shadowMedia` + fake caustics | CUDA (media), OptiX loop (fake caustics) | unoccluded shadow slots | transmittance | `traceShadowRays` inner loop |
| 11 | `resolveShadow` | CUDA, per path | up to 3 fixed shadow slots of the path | path radiance and LPE accumulators | `traceShadowRays` accumulation |
| 12 | `traceAo` (depth 0) | OptiX occlusion + CUDA | AO rays | per-path AO count | `traceAoRays` |
| end | `accumulateFilm` | CUDA, per pixel | the pixel's spp slots | colorSum, count, alpha, LPE sums, AO | :2415-2421 |

Notes on the less obvious rows:
- **Pass A and pass C separated** (kernels 3 and 5) as on the CPU. Pass A runs in ray order (coherent), pass C in material order.
- **The shade kernel stays one kernel.** It interprets the VM, builds the closure (a switch over `SurfaceModel`), does NEE (light tree) and samples the continuation, as `shadeHit` does now. Splitting off "evaluate material inputs" (Laine's material kernel) is an optimization to measure, not a starting point (section 11, question 1).
- **Ray tracing inside shading** has three sources:
  - shader queries (`Op::Query`, from code `query.h:1-8`);
  - SSS walks (`intersectLocal` in a loop);
  - fake caustics (a closest-hit loop that evaluates materials).

  All three run as OptiX raygen programs (kernels 5q, 6, 10) built from the same kernel-body functions. This is Cycles' `SHADE_SURFACE_RAYTRACE` split (lit, from memory). Kernel 5q receives only the key range of the materials that need it, because the sort key puts them in a range of their own.
- **Shadow results by path** (kernel 11): a path has fixed shadow slots (path × 3 + k, the three cases of `wavefront.h:204-207`). The OptiX launch traces a compacted list that stores the slot. The per-path kernel then adds the results without atomics, in path order (section 6.3).

### 2.3 State between kernels (design)

- **Path state** (SoA by path slot, fixed for the wave): today's 21 arrays (111 B, section 6.1) plus per-path accumulators for radiance, alpha and LPE outputs (6.3).
- **Ray queues**: `rays` and `nextRays` (41 B per ray) with an atomic counter each. They are double-buffered by pointer swap. In a CUDA graph the swap becomes the parity of the unrolled depth (2.5).
- **Hits**: 24 B per ray, indexed by ray.
- **Shade queue**: (key, ray) pairs. The key is the MaterialId of a material order that groups materials by program (plan-gpu-perf: "commit() assigns material indices sorted by program"), with one key range per kernel variant (5 / 5q). It is sorted into a second buffer (CUB double buffer).
- **Volume-event queue, walk queue**: append lists of ray indices with their per-event data (`volumeT`, `volumeId`, `PhaseFunction`; `Walk`, 68 B).
- **Shadow slots**: 3 per path (ray, contribution, category, emitter data; 77 B each today). Plus the compacted list of live slots for the OptiX launch.
- **LPE parts**: an append arena with an atomic bump allocator. It replaces `ShadowQueue::partState/partValue/partUnit` (push_back today). Its capacity bound is 3 × paths × (largest part count of a site). The CPU builds parts per distinct final state, at most one per lobe and side.
- **Never stored**: `ClosureList` and `SurfaceInputs` are created and consumed inside one kernel (as plan-gpu-perf 2.6 planned).

### 2.4 Queues and compaction (measured, `cw_compact_probe.cu`)

The probe compacts n = 1920 × 1080 = 2,073,600 elements with survival rates of 10, 50 and 90 %:

| Method (indices only) | 10 % | 50 % | 90 % | Order |
|---|---|---|---|---|
| `atomicAdd` per surviving element | 0.038-0.056 ms | 0.040 | 0.042 | not stable |
| warp-aggregated (ballot, popc, one atomic per warp) | 0.038 | 0.040 | 0.043 | not stable |
| block-aggregated (BlockScan, one atomic per block) | 0.019 | 0.019-0.020 | 0.020 | stable within a block |
| CUB `DeviceSelect::Flagged` (byte flags) | 0.016 | 0.017-0.018 | 0.018 | stable |
| CUB `DeviceSelect::If` (predicate) | 0.017 | 0.018 | 0.018-0.024 | stable |
| CUB `ExclusiveSum` + scatter | 0.028 | 0.028-0.030 | 0.028-0.071 | stable |
| `thrust::copy_if` | 0.043-0.044 | 0.043 | 0.060-0.106 | stable |

With a 32-byte ray payload (8 floats SoA):

| Method | 10 % | 50 % | 90 % |
|---|---|---|---|
| fused push at a warp-aggregated slot (in the producing kernel) | 0.046-0.068 ms | 0.065 | 0.077 |
| fixed slots + `Select::Flagged` + gather | 0.123-0.143 | 0.313-0.315 | 0.397-0.399 |
| fixed slots, no compaction (the write alone) | 0.098 | 0.136-0.137 | 0.140-0.142 |

Reading:
- nvcc aggregates `atomicAdd(&counter, 1u)` per warp by itself: the explicit ballot version has the same time.
- All index compactions are in the 16-106 µs range per 2M elements (thrust at 90 % survival is the slowest), a few percent of a bounce at most (2.6).
- The fused push writes each ray once. Compacting after the fact writes and reads it twice.

**Recommended (design):**
- Producing kernels append their outputs (next rays, shadow-list entries, walk and volume events, shade-queue pairs) with warp-aggregated atomics (nvcc's automatic aggregation is enough).
- The order in a queue then varies between runs. No result depends on it: each path has its own state, and film accumulation is owned (section 7).
- CUB `DeviceSelect` is used where a stable order matters (none in the proposed design).

### 2.5 Material sort, launches and divergence (measured, `cw_sort_probe.cu`, `cw_vm_probe.cu` B)

CUB `DeviceRadixSort` on 2,073,600 (material, ray index) pairs, in ms:

| Keys | all 32 bits | `end_bit` = ceil(log2 K) | packed key (material · 2^21 + index), SortKeys | counting sort (shared histogram + atomic scatter, unstable) | RunLengthEncode after the sort | gather of a 24 B hit record |
|---|---|---|---|---|---|---|
| K = 1 | 0.090 | 0.040-0.045 | 0.080 | 0.92 | 0.021-0.028 | 0.127 |
| K = 16 random | 0.098-0.099 | 0.037 | 0.098 | 0.30 | 0.020 | 0.223 |
| K = 256 random | 0.104-0.151 | 0.044-0.079 | 0.098-0.157 | 0.36 | 0.021 | 0.299-0.302 |
| K = 256 in screen regions | 0.098-0.106 | 0.037 | 0.100 | 0.52-0.56 | 0.021-0.028 | 0.128 |
| K = 4096 random | 0.110-0.113 | 0.070 | 0.099 | 0.11 | 0.021 | 0.303-0.305 |
| K = 65536 random | 0.112-0.114 | 0.074-0.075 | 0.099-0.102 | - | 0.021 | 0.306-0.308 |

Reading:
- The bit-limited stable radix sort costs 0.04-0.08 ms per bounce at 1080p and is stable.
- A counting sort with atomics is slower: contention on few bins, 0.92 ms for K = 1.
- Gathering the hit records into sorted order costs 3-8x more than the sort itself. The shade kernel should read hits through the sorted ray index instead. It reads them once in either case.
- CPU's shortcut for a single material (`sortByMaterial` :1240) should stay.

Launch overhead (empty kernel, measured):

| | per launch |
|---|---|
| on a stream (WDDM) | 5.4-7.3 µs |
| in a CUDA graph | 0.40-0.80 µs |

Per-material launches would cost 256 × 5.5 µs = 1.4 ms per bounce at 256 materials. They also need the run boundaries on the host, which means a synchronization per bounce. One shade launch per bounce over the sorted queue avoids both.

Divergence of the interpreter (1M lanes, 60-instruction random programs, from a pool of 24 operations):

| Programs in the lane mix | lanes sorted by program | lanes random |
|---|---|---|
| K = 1 | 0.82 ms | 0.82 ms |
| K = 2 | 0.83 | 1.41 (1.7x) |
| K = 4 | 0.83 | 2.60 (3.1x) |
| K = 8 | 0.85 | 4.71 (5.6x) |
| K = 32 | 0.86 | 10.4 (12.1x) |

One program with K different uniform blocks (materials sharing a program), lanes random:

| K | U in global memory | U in constant memory |
|---|---|---|
| 1 | 0.82 ms | 0.80 ms |
| 8 | 0.86 | 0.88 |
| 32 | 0.94 | 2.50 |

- After sorting, a warp straddles a material boundary at most at the start and end of each run, so divergence costs only in the few mixed warps. Sorting removes almost all of the 12x.
- Materials that share a program diverge only in their uniform reads, which global memory absorbs (+15 % at K = 32). Constant memory serializes them (+3x).

**Recommended (design):**
- Pass B is a CUB `SortPairs` over the whole queue capacity with `end_bit = bits(materialKeyCount + 1)`. Empty entries get the all-ones sentinel key and sort last, so the sort needs no host-side count and can be captured in a graph.
- The key is the material id from the program-grouped material order. A high key bit selects the OptiX variant (5q) for materials with queries.

### 2.6 Budget at 1080p, 1 spp (measured components, design estimate)

Per bounce at 2M paths:

| Step | Cost |
|---|---|
| sort | 0.04-0.08 ms |
| appends of next rays and shadow slots | 2 × 0.05-0.08 ms |
| VM for a 60-instruction textured program (0.57-0.68 ns per lane) | 1.2-1.4 ms |
| closure build, light-tree NEE, continuation | not measured (several hundred bytes of local state; probably similar to the VM) |
| volumes (when present) | 0.4-0.85 ms per 1M rays crossing a cloud |
| launch overhead, about 12 kernels | 70 µs on a stream, 6 µs in a graph |

Per wave: the film flush, 0.03 ms (beauty) to 0.8 ms (9 LPE outputs).

Shading dominates. Sorting and compaction are below 10 %. The OptiX report gives intersection costs.

---

## 3. The bytecode VM on the GPU (question 2)

### 3.1 The probe (`cw_vm_probe.cu`)

The probe includes Nova's `core/shading/vm.h` and calls `execute<1>` per lane, the per-lane executor M3 K5 designated for CUDA (from code `vm.h:252`).

The program is built with the real `Instr` format (16 bytes). It has:
- 60 instructions, 112 component operations, 72 rows, 46 uniform floats;
- uv transform, 3 × `Tex2D` (base color, roughness, normal map, with footprint derivatives), Perlin noise;
- pow (gamma), mix, clamp, RGB→HSV→RGB, luminance dot, normal mapping (cross, normalize, frame);
- smoothstep roughness remap, a Schlick-like `pow(1 − cos, 5)` with mix, sin/abs emission, length/fract/greater/select;
- exp, atan2 and mod.

Inputs (P, N, uv, I, 4 derivatives) come from global SoA. The 15 outputs go to global SoA.

On the device `vm.h` returns the Tex2D fallback (`#if !defined(__CUDA_ARCH__)` at `vm.h:201`). The "hw tex" variants split the program at its Tex2D instructions and do `tex2DGrad<float4>` on CUDA texture objects (2048² mip-mapped RGBA16F / R16F, trilinear, anisotropic 8) between the segments. That is what a device branch in `vm.h` would do.

The hand-written kernel is the same math in straight-line CUDA, reading the same uniforms (so nothing folds) and calling the same `vexPow`, `vexSmoothstep`, `noise::perlin`, `rgbToHsv`.

### 3.2 Results (measured, 1M = 2^20 lanes, 128 threads per block)

| Variant | ns per lane, coherent | ns per lane, shuffled | G instr/s |
|---|---|---|---|
| VM, fallback textures, rows local, U + code global | 0.583-0.585 | 0.587-0.590 | 102-103 |
| VM, fallback, rows local, **U constant**, code global | **0.472-0.493** | 0.478-0.499 | 121-127 |
| VM, fallback, rows local, U + code constant | 0.663-0.672 | 0.664-0.672 | 89-91 |
| VM, fallback, rows shared (odd stride), U + code global | 1.081-1.087 | 1.092-1.098 | 55 |
| VM, fallback, rows shared, U + code constant | 0.980-0.987 | 0.987-0.998 | 61 |
| VM + hardware textures, rows local, U + code global | 0.657-0.659 | 0.678-0.681 | 88-91 |
| VM + hardware textures, rows local, U + code constant | 0.570-0.603 | 0.593-0.624 | 96-105 |
| VM + hardware textures, rows shared, U + code constant | 0.995-0.997 | 1.012-1.019 | 59-60 |
| hand-written, fallback textures | 0.137-0.138 | 0.137-0.138 | 436-438 |
| hand-written + hardware textures | 0.154-0.155 | 0.154-0.155 | 386-389 |
| VM, arithmetic-only variant (pow/sin/exp/atan2 → mul/abs, noise/HSV → copy), U constant | 0.422-0.444 | | 135-142 |

CPU, same program, fallback textures, `vm.h` compiled by MSVC (`/O2 /arch:AVX2`):

| Variant | ms per 1M lanes | ns per (instr × lane) |
|---|---|---|
| `execute<32>`, 1 thread | 136-147 | 2.16-2.34 |
| `execute<1>`, 1 thread | 383-398 | 6.08-6.33 |
| `execute<32>`, 8 threads | 31.6-35.2 | 0.50-0.56 |
| arithmetic-only, `execute<32>`, 1 thread | 63-70 | 1.00-1.11 |
| arithmetic-only, `execute<32>`, 8 threads | 14.1-20.8 | 0.22-0.33 |

M3 measured 0.15-0.16 ns per (instr × lane) for pure arithmetic at N = 32. Its prototype measured 0.40-0.63 ns for a random graph including load and gather (from docs/research/m3/shader-vm.md:27-28, 187). This probe's program has transcendentals and procedurals, which run in scalar loops by design (`vm.h:1-11`), so 2.2 ns is consistent with those numbers.

ptxas reports for the VM kernels: 72-128 registers and 192-560 B of stack, some with small spills. The hand-written kernel uses 48 registers and no stack (measured, build log).

### 3.3 Reading

- **The interpreter costs 3.6-4.5x a compiled kernel**, about 8 ps per bytecode instruction and lane. Where the time goes:
  - dispatch: the switch over `opClass` and `op`;
  - operand fetch through local memory: `R[index]` is a dynamic index, so the register file cannot live in registers;
  - an instruction stream read per instruction.

  Textures shrink the relative gap only slightly, because texture latency overlaps the dispatch.
- **Against the CPU** the GPU VM is 62-68x faster than all 8 cores on the representative program, and 31-45x on pure arithmetic.
- **Register file in local memory.** Local memory interleaves per thread (the same row offset across a warp is one coalesced access) and is cached in L1/L2. Shared memory with an odd stride avoids bank conflicts, but its size limits occupancy: 72 × 4 B × 128 threads = 37 KB per block. Shared memory lost 2x.
- **Uniforms in constant memory** win 16-19 % when warp-uniform and lose 2.7x when not (2.5).
- **Code in global memory.** Instructions read from constant memory with a dynamic index were slower than from global memory.

### 3.4 Register file and per-thread memory (measured sizes, design)

- `Program::rowCount` is 72 here. The compiler's GPU cap is 256 rows (M3 K15, a 1 KB local array).
- The device kernel can declare `float R[kMaxRows]` at the cap. Local memory is reserved per resident thread whatever the array size, so a single cap of 256 is simplest. A per-program row count only matters if the kernel is specialized per program (section 11, question 2).
- Measured sizes:
  - `ClosureList` is 2368 B (16 lobes × 128 B + CDF);
  - `SurfaceInputs` is 1552 B (384 floats; `copyInputs` copies only `size` of them, from code `surface.h:108`).
- A full shade thread therefore holds R (≤ 1 KB) + SurfaceInputs (1.5 KB) + ClosureList (2.4 KB) + light and LPE temporaries, about 5 KB of local memory.
- CUDA reserves local memory for every thread that can be resident (lit, from memory). On 84 SMs × 1536 threads that is about 0.65 GB. Affordable, but L1 traffic.
- Later options:
  - build the closure from (uniform template + varying rows) through an accessor, without copying the template into a 1.5 KB `SurfaceInputs`;
  - a device `ClosureList` with 8 lobes where the closure model allows it.

### 3.5 Where uniforms live (measured, design)

**Recommended:**
- **Uniforms**: the material's uniform block in global memory as a `const float *__restrict__`, which the compiler reads through the read-only path. Constant memory only helps when a warp is guaranteed to read one block, and it costs 2.7x when that fails.
- **Code**: the `Instr` arrays of all programs in one global buffer, indexed by program offset.
- **Constant memory** is kept for launch parameters (the scene and queue views).

### 3.6 Tex2D and Query on the device (design)

- **Tex2D**: `vm.h:201` gets a `__CUDA_ARCH__` branch that calls a device `sampleTexture(table, TexSampler, s, t, dsdx..dtdy, rnd)` (section 4) with the same per-lane status. The probe's segment split shows what that costs: 0.57-0.68 against 0.47-0.59 ns per lane for three lookups.
- **Query** (`vm.h:177`): queries trace rays.
  - The CPU hands one call per 32-lane chunk to the integrator.
  - On the GPU the shade kernel variant 5q runs inside an OptiX launch and calls `optixTrace` in place (2.2).
  - Splitting the program at Query instructions and suspending rows to memory is the alternative. It is more complex and not needed when the OptiX variant exists.
- **Opacity programs** run in OptiX any-hit programs through the same `execute<1>` (M3 plan 2.6; the OptiX report).

---

## 4. Textures (question 3)

### 4.1 Today (from code)

- Kernels see a texture as a `TextureId` and a POD `TexSampler`.
- Lookups go through `ITextureSampler::sample2D` (virtual, from code `texture.h:54`) over SoA `TexQuery` lanes with (s, t), full-diameter footprint derivatives and a stochastic filtering sample.
- In Hydra, `HdNovaTextureSystem` implements it with OIIO's TextureSystem on converted tiled, MIP-mapped files, with `stochastic = 3` (from code `hydra/textureSystem.cpp:47`).
- UDIM follows Karma: tile = 1001 + ⌊u⌋ + 10⌊v⌋ for 0 ≤ u < 10, v ≥ 0, else the fallback (from code `textureSystem.h:9-12`).
- docs/research/m3/textures.md section 5 already fixed the GPU-facing rules:
  - ids plus POD sampler state;
  - blur as derivative padding;
  - color baked into the converted files;
  - per-lane status;
  - at most 4 channels;
  - converted files that a GPU loader can read directly.

### 4.2 Options

| Option | For | Against |
|---|---|---|
| (a) **resident MIP chains as CUDA mipmapped arrays**, one bindless texture object per texture (UDIM tile), hardware trilinear/anisotropic filtering | simplest; deterministic; full-rate lookups (350-400 G/s coherent, measured); no residency logic in kernels; IPR edits are a re-upload of one texture | all levels resident: 180 MB per 4K RGBA16F (measured); scenes beyond VRAM fail; upload time 61-69 ms per 4K RGBA16F from pageable memory (measured) |
| (b) **tile cache with demand loading**: sparse textures, a page table, misses recorded in the kernel and filled by the host between launches (NVIDIA OptiX Toolkit DemandLoading, BSD; Karma XPU's approach) | memory proportional to what is visible; supports scenes far larger than VRAM | residency logic in the hot path; a lookup that misses must return a fallback or a coarser level, so the image depends on timing unless the pass is re-rendered after loading; host-side tile I/O pipeline; more code |
| (c) **resident chains with a budget**: (a), but when the scene exceeds a budget the finest MIP levels of the largest textures are dropped (an automatic `KARMA_XPU_MAX_SHADER_TEXTURE_RES`), with a warning | (a)'s simplicity and determinism; degrades gracefully | blurrier close-ups of oversized textures; no streaming |

Karma XPU (help):
- loads shader textures on demand in tiles (for example 128 × 128) and evicts tiles when the GPU is full, with a strong slowdown in that state (sparse textures can be disabled);
- keeps lighting textures untiled, up to 8192 by default;
- limits UDIMs to 10 × 10 by default;
- auto-generates MIP maps;
- uses hardware linear filtering, which can show artifacts with normal maps. The remedies are cubic filtering on the image node or an HQ linear filtering mode at some performance cost.

### 4.3 Measurements (`cw_tex_probe.cu`)

**T1, creation.** A 4096² RGBA16F MIP chain (13 levels) allocates 180.4 MB (179.0 MB in theory), and allocation plus upload take 61-69 ms. RGBA8 takes 90.2 MB and 28-49 ms. The upload came from pageable host memory: pinned staging buffers would be faster (M7 research measured 2x for frames).

**T2, tex2DGrad throughput** (4M threads × 4 lookups):

| Case | G lookups/s |
|---|---|
| RGBA16F 4K, coherent (screen-mapped) | 348-354 |
| RGBA8 4K, coherent | 407 |
| RGBA16F, random coordinates, level 2-3 (fits L2) | 48.6-49.2 |
| RGBA8, random, level 2 | 54 |
| RGBA16F, random, level 0 (128 MB working set) | 8.6 |
| RGBA16F, random, anisotropic 8:1 footprint | 3.9 |
| RGBA16F, coherent, anisotropic 8:1 | 58 |
| bindless: 64 textures, handle from a table, warp-uniform | 53-65 |
| bindless: handle random per lane | 7.7-7.9 |

**T3, precision.** Hardware linear filtering of a 0/1 step gives 257 distinct values between two texels, a maximum error of 1/512 of the step: 8-bit fixed-point weights. For a float4 texture, software bilinear from global memory (four loads) ran at 139 G/s against 96 G/s for the hardware path. Float4 filtering is not full rate in hardware.

Reading:
- Texture handles must be warp-uniform. This is another reason to sort by material (per-lane random handles cost 7-8x).
- Incoherent lookups at fine levels are memory-bound. Ray cones keep secondary bounces on coarse levels (Nova's cones, M3).
- Anisotropic filtering is expensive only when incoherent.

### 4.4 UDIMs (design)

Each UDIM tile is its own texture object. A UDIM texture is a small table (default 10 × 10, Karma XPU's limit) of tile handles plus a "missing" marker. The device lookup computes the tile with Nova's rule, reads the handle (warp-uniform in practice: neighbouring hits share tiles), clamps the address mode inside the tile (as the CPU does, `textureSystem.cpp:125`), and returns status 1 for a missing tile.

### 4.5 Recommendation for M10 (design)

**(c) resident MIP chains with a memory budget**, with these properties:
- **Source.** The same converted files the CPU path reads, opened by OIIO on the host (the texture system's file conversion stays as it is) and uploaded level by level. The CPU and GPU see the same texels.
- **Formats.** RGBA16F for half/float data (what the color-converted files contain, textures.md), RGBA8/R8 for 8-bit raw data, R16F/RG16F/RGBA16F by channel count. 3-channel data is padded to 4 (CUDA has no 3-channel arrays).
- **Filtering.** Hardware trilinear (or anisotropic, by `TexFilter` and footprint) for color. For data textures (normal maps, displacement-like data), a software bilinear path for `TexFilter::Linear`, which avoids the 1/512 weight quantization. Measured as no slower for 4-channel float data. Cubic in software.
- **Stochastic filtering.** OIIO's `stochastic = 3` picks the MIP level and the anisotropic probe with the lane's random number. The GPU uses hardware trilinear instead (deterministic). Both are unbiased estimates of slightly different filters, which the image tests tolerate (section 7).
- **Budget.** A setting (default about 50 % of free VRAM after geometry). Over budget: drop the finest level of the texture with the largest finest level, repeat, warn once with the list.
- **Lighting textures** (dome, rect light textures, importance maps) stay as they are today: Nova's own `Spectrum` arrays with distributions, uploaded as plain buffers.

Limits:
- No out-of-core: a scene whose coarsest acceptable levels exceed VRAM fails with a message.
- Upload time at scene load and on texture edits: about 60 ms per 4K RGBA16F from pageable memory, less with pinned staging and parallel decoding.
- The filter differs from OIIO's, so CPU/GPU images differ by filter, not only by noise.

**(b) demand loading** is the follow-up when scenes outgrow VRAM. The interface (`TextureId` plus a per-lane status) already allows a "not resident → fallback" answer.

---

## 5. Volumes (question 4)

### 5.1 Today (from code)

- Fields are NanoVDB float grids in one pointer-free buffer, "the same bytes upload unchanged to the GPU" (from code `grid.h:1-9`).
- Majorant grids are dense arrays of cells over a reference grid, built on the host (`majorant.h`).
- Per thread:
  - `VolumeEvaluator` holds a `VolumeSampler` (a NanoVDB accessor) per field and the volume shader's rows. It runs `execute<1>` per point (from code `volume_eval.cpp:101`).
  - `MediaWalker` merges overlapping volumes with `thread_local` vectors (from code `volume_media.cpp:85-93`).
- Interval bytecode (`core/shading/interval.h`) builds majorants on the host and never runs per ray, so the device does not need it.

### 5.2 Measurements (`cw_vol_probe.cu`)

The scene:
- A heterogeneous cloud: a sphere of radius 96 voxels modulated by sines, density in [0, 1].
- 3,928,309 active voxels, a 28.1 MB NanoVDB buffer, built on the host with `nanovdb::build::Grid` + `createNanoGrid` in 139-209 ms.
- A majorant grid of 28³ cells of 8³ voxels, dilated by one voxel for trilinear lookups, 41 % of the cells empty. σ_t = 0.05 per voxel at density 1.
- 1024 × 1024 orthographic rays.

The probe uses NanoVDB 32.6.0 from `$HFS/toolkit/include` unchanged on the device: `ReadAccessor` + `SampleFromVoxels<..., 1, true>`, one accessor per thread.

| Case | Time | Steps per ray | SIMD efficiency |
|---|---|---|---|
| V1 trilinear lookups, random points in the box | 10.15 G lookups/s | | |
| V1 trilinear lookups along the rays | 56.5 G lookups/s | | |
| V2 ratio tracking, one global majorant, rays in order | 0.542 ms | 9.7 | 58.6 % |
| V2 shuffled rays | 0.834-0.850 ms | 9.7 | 54.4 % |
| V3 ratio tracking on the majorant grid, in order | 0.474 ms | 2.1 | 42.3 % |
| V3 shuffled | 0.788-0.789 ms | 2.1 | 31.4 % |
| V4 delta-tracking free flight on the grid, in order | 0.374-0.376 ms | 1.3 | 35.3 % |
| V4 shuffled | 0.618-0.620 ms | 1.3 | 28.5 % |

(SIMD efficiency = sum of lane steps / (32 × sum of warp maxima).)

Compile checks (measured):
- NanoVDB's device headers compile in C++20 for sm_120. Its host-side builder needs C++17 and `--extended-lambda` (Nova does not build grids in CUDA code).
- Nova's `MajorantDda::next` calls `MajorantLayout::index`, a plain member function, so device compilation fails (error at `majorant.h:118`; the fix is `NOVA_INLINE` on `index` and `cellOf`, `majorant.h:32-36`).
- `VolumeSampler` (`grid.h:87`) is a host class around the accessor. The device needs a small `__device__` equivalent, as in the probe.

Reading:
- Tracking 1M rays through a dense cloud costs well under a millisecond. Incoherent rays (bounces) cost 1.6x.
- The trackers' divergence wastes half or more of the lanes. Pulling rays from a shared queue in chunks (persistent threads) or re-queuing long walkers could recover some of it, at most about 2x on the tracker kernels alone.
- The majorant grid cuts lookups 4.6x but here saves only 13 %, because the DDA crosses up to 28 cells per ray. On real Pyro grids with expensive volume shaders (a VM run per tentative collision), saving lookups matters more.

### 5.3 Design for the device (design)

- **Upload**: each `VolumeGrid` buffer with one `cudaMemcpy`, majorant and extinction-majorant arrays, light-cell distributions, the per-volume transforms over the shutter, and the volume program (a `ProgramView`, section 9).
- **Device sampler**: per field a NanoVDB `ReadAccessor` created per ray segment (cheap, measured 52-70 registers for the tracking kernels) and `SampleFromVoxels<1>`.
- **Trackers**: one thread per ray in `traceMedia` (kernel 2), the shadow-ray transmittance in kernel 10, the same `flyMedia`/`trackMedia` logic.
  - `MediaWalker`'s spans and active lists become fixed-size arrays (for example 8 overlapping volumes; insertion sort as today).
  - `EmissionGather` already has fixed slots (`kSlots = 4`).
  - The area-light crossings have a fixed array (16).
  - Equiangular sampling is inline math.
- **Volume shader**: `execute<1>` per tentative collision, rows in local memory, uniforms global (section 3). Field reads as program inputs, as today.
- **First**: one thread per ray, no chunking. Measure on Pyro scenes before adding work redistribution.

---

## 6. Memory (question 5)

### 6.1 Per-path state today (measured, `cw_sizes.cu`)

Bytes per path slot of `WaveQueues` (summed from the element types, `wavefront.h:148-256`):

| Part | Bytes per slot × slots per path |
|---|---|
| path state (21 arrays: pixel, beta, radiance, seeds, pdfs, medium, cone, limits, previous vertex, LPE state) | 111 |
| `rays`, `nextRays`, `aoRays` (41 B each) | 123 |
| hits | 24 |
| shadow queue (ray + contribution + parts index + category + emitter data, 77 B) × 3 | 231 |
| shade order | 16 |
| volumes (mediumT .. eqB) | 74 |
| walks | 68 |
| other | 1 |
| **total** | **648 B** (without the dynamic LPE part vectors) |

At 2,073,600 paths (1080p, 1 spp) that is 1.34 GB.

### 6.2 Paths in flight (lit, measured, design)

- Laine et al. ran 1M paths (212 B each), and 8M gained only 5 % (lit). pbrt-v4 renders 1M samples per pass (lit). Cycles sizes the pool per device from the state size (lit).
- The probes reach full throughput at 1-2M lanes.

**Recommended (design):**
- A wave is min(pixels × spp, 2M) path slots, the setting `nova:gpu:pathsInFlight`. At 1080p and 1 spp a wave is the whole frame. At 4K (8.3M pixels) a frame is four waves.
- With today's layout plus per-path accumulators (6.3), 2M paths cost about 1.4 GB, leaving more than 10 GB on a 16 GB card for geometry, BVHs (OptiX report), textures (4.5) and volumes.
- The volume and walk arrays should be allocated only when the scene has volumes or SSS. The AO queue is needed only at depth 0.

**Waves against regeneration.** Nova's CPU wave runs all bounces of a fixed set of paths. The GPU kernels then run with few live paths in the last bounces. With persistent grid-stride kernels and graph launches (section 10), an empty or nearly empty bounce costs a few microseconds per kernel. Regenerating paths as they finish (Laine, Cycles) keeps the GPU full but needs a sample allocator per pixel. Wave-synchronous comes first; measure the tail before deciding (open question).

**Double buffering.** `rays` and `nextRays` swap per bounce, as today (`wavefront.cpp:2412`). In a captured graph the depth loop is unrolled, so each depth's kernels get the buffer of their parity as parameters. The pointer swap needs no copy.

### 6.3 Film accumulation (measured, `cw_film_probe.cu`)

Adding each path's RGB (+ count), and optionally 8 LPE outputs, into a 1080p film, in ms:

| Planes | spp per wave | owned (thread per pixel, plain +=) | atomicAdd, paths in pixel order | atomicAdd, paths shuffled |
|---|---|---|---|---|
| 1 | 1 | 0.027-0.044 | 0.027-0.031 | 0.211-0.215 |
| 1 | 4 | 0.164-0.180 | 0.214-0.221 | 4.98-5.00 |
| 9 | 1 | 0.831-0.834 | 0.828-0.831 | 23.9-24.0 |
| 9 | 4 | 1.586-1.593 | 1.662-1.666 | 103.8-104.0 |

Atomics into the film from kernels that run in material order are 7-65x slower than owned writes. They also make the floating-point sums depend on the order.

**Recommended (design):**
- Every contribution goes to per-path accumulators:
  - radiance (12 B);
  - alpha (4 B);
  - per LPE output 12 B (and the `lpeLit` sums for shadow; outputs);
  - AO count.
- Writers:
  - kernels that handle a path once per pass write its accumulators directly;
  - shadow results go through the per-path `resolveShadow` kernel (2.2), because a path has up to 3 shadow rays in one pass;
  - camera-visible light and environment contributions in pass A (one thread per ray, at most one ray per path).
- After the wave, `accumulateFilm` runs one thread per pixel over its spp consecutive slots (the CPU's slot layout) and adds to colorSum, count, alphaSum, LPE sums and AO.
- The memory: 2M paths × 9 outputs × 12 B = 224 MB.

### 6.4 AOVs and LPE outputs (design)

- **First-hit utility planes** (P, N, depth, primvars, base color, shader AOVs): a pixel-centre pass with one ray per pixel (as `renderFirstHitAovs`), owned writes.
- **Cryptomatte** (`CryptoPlane::add`, per-pixel fixed slots with a find-or-insert loop, from code `crypto.h:51-65`): the depth-0 hits in a per-pixel kernel over the pixel's spp slots, after pass A stored each path's first-hit ids. Never from a ray-parallel kernel when spp per wave > 1.
- **LPE**:
  - `lpe::Program` is POD pointers into tables (`lpe.h:50-67`), uploaded once. A DFA step is two table reads.
  - `LpeTables` (std::vectors, `wavefront.h:102`) becomes a device view of the same arrays.
  - Deposits go to the per-path accumulators by output index. Shadow-ray parts live in the append arena (2.3) until `resolveShadow`.
- **Alpha, AO**: per-path accumulators.

---

## 7. Determinism and CPU/GPU agreement (question 6)

### 7.1 What is exactly reproducible (from code, design)

- Sample values are integer computations: Owen-scrambled Sobol with hashed shuffles (`sobol.h`) and PCG (`rng.h`). The conversion to float is exact, so the CPU and GPU draw bit-identical numbers for the same (pixel seed, sample index, stream).
- With per-path state, owned film accumulation, a stable sort and no floating-point atomics, a GPU render is bit-reproducible run to run on the same GPU and build, even though queue order varies. This also needs a deterministic OptiX traversal for a given BVH (OptiX report).

### 7.2 Floating-point differences (measured, `cw_vm_probe.cu` D)

The 15 outputs × 1M lanes of the representative program (90 % of the instructions are float math):

| Comparison | `--fmad=true` (nvcc default) | `--fmad=false` |
|---|---|---|
| GPU VM vs GPU hand-written (same math) | 1,493,460 floats differ (9.5 %), max 6.0e-7 | 0 differ |
| GPU VM vs CPU `execute<32>` (fallback textures) | 5,179,636 differ (33 %), max 2.2e-6 | 353,550 differ (2.2 %), max 1.5e-8 |
| CPU `execute<1>` vs CPU `execute<32>` | 0 | 0 |

With `--fmad=false` the remaining GPU/CPU differences are only in outputs computed through `sinf` (emission, 117,814 lanes) and `powf` (specular, 34-37 lanes). That is CUDA's math library against MSVC's CRT.

- MSVC (`/fp:precise`, VS 2022) does not contract a·b + c into FMA: the CPU executors are bit-identical to each other.
- `--fmad=false` did not slow the VM or the hand-written kernel down here (VM 0.472 against 0.491 ns per lane, hand-written 0.137 against 0.138). BSDF-heavy code, where FMAs are a larger share of the work, may lose a few percent (not measured).

Bit-identical paths would also need:
- the same transcendentals: own polynomial `sin/cos/exp/log/pow` in `ops.h`, as M3 planned (docs/research/m3/plans/plan-gpu-perf.md:894), used by both backends;
- the same intersection results: Embree and OptiX differ in watertightness, barycentrics and t, so hit points and offsets differ in the last bits (OptiX report);
- the same texture filters (4.5).

Even then a 1-ulp difference flips discrete decisions:
- a Russian-roulette or lobe-selection threshold;
- the stochastic opacity test;
- a ray that grazes an edge.

From that vertex on the two paths are different random samples. **CPU and GPU images agree statistically, not per pixel.**

### 7.3 Testing GPU against CPU (design)

- **Kernel-level, bit-exact or ulp-bounded.** A test target that runs the device versions of the per-item functions (BSDF eval/sample/pdf, closure builds, light sampling and tree pmf, LPE steps, the VM over the M3 VM goldens) on fixed inputs and compares with the CPU:
  - built with `--fmad=false`;
  - exact except where transcendentals are involved (bounded ulps).

  This catches porting errors where they happen.
- **Image-level, statistical.** `nova_imgcmp` already implements the right test (from code `tools/nova_imgcmp/nova_imgcmp.cpp:1-50`):
  - a reference made from two independent renders (`--make-ref A B`);
  - per-block mean tests with Sidak-corrected thresholds at a family-wise error rate;
  - a global test, relative tolerances, documented-difference regions.

  For M10 the CPU renders are the reference and two GPU renders (different seeds) the test. Every reference scene in `tests/scenes` gets a GPU run, with regions only for documented differences (texture filters).
- **Paired same-seed comparison.** A quick high-sensitivity check: GPU and CPU with the same seed share most paths, so the per-block mean of (GPU − CPU) has a much smaller variance than independent renders. A systematic bias shows at low spp. Its statistics are not exact (the samples are correlated, not independent), so it is a diagnostic, not a pass/fail gate.
- **Run-to-run determinism.** Two GPU renders with the same seed must be bit-identical (7.1).

---

## 8. What cannot run on the GPU, and an XPU-like mode (question 7)

### 8.1 Host-only parts (from code, design)

| Part | GPU treatment |
|---|---|
| CVEX bridge (`ExternalBinding`, VEX materials, M5) | not on the GPU; the material's preview renders, with one warning (PLAN.md; M5 D12; Karma XPU never runs VEX either, docs/research/m5/vex-materials.md:30) |
| OIIO TextureSystem, OCIO, auto-tx conversion | host side at load; the device reads uploaded MIP chains (section 4) |
| Houdini/USD asset resolver callbacks, file I/O | host side at load (Hydra sync); nothing per ray needs them |
| Light tree build/refit, power tables, distributions | host build, upload the arrays (views exist) |
| Majorant grids, interval programs, NanoVDB conversion | host build, upload |
| OpenSubdiv refinement, displacement | host (as today), upload the refined meshes |
| `IShadingQueries` (virtual) | OptiX variant of the shade kernel (2.2) |
| `IHitFilter` (virtual) | OptiX any-hit with the opacity program (OptiX report) |
| `ITextureSampler` (virtual) | device texture function over a handle table |
| `std::sort`, `thread_local` vectors in shadow and emitter passes, media walker | CUB sort, fixed arrays, append arenas |

### 8.2 Karma XPU's model (help, paraphrased)

- **Devices.** XPU uses CPU and GPU together: OptiX GPU devices and an Embree CPU device, each GPU addressed separately (memory not pooled). The viewport lists each device with its status (idle, init, disabled, fail) and its share of the image so far. The GPU devices are expected to contribute most, since XPU is optimized for GPUs.
- **Failover.** If a device fails, for example a GPU running out of memory, the others finish the frame.
- **Consistency.** The result should be the same for any mix of devices.
- **Scope.** XPU is not a replacement for Karma CPU: a high-performance alternative with fewer features and possibly different output.
- **Shaders** are compiled per device, with a cache.

### 8.3 What an XPU mode means for Nova (measured ratio, design)

- **Throughput.** On shading work the GPU is 31-68x faster than all 8 CPU cores (3.2). RT cores widen the gap for intersection (OptiX report). A CPU device would add about 1.5-3 % throughput on this machine. Its value is:
  - (i) machines without a usable GPU;
  - (ii) failover when the GPU runs out of memory or fails;
  - (iii) a shared code path that keeps CPU and GPU results consistent.
- **Consistency.** "The same result" for a mixed render requires every device to render the same feature subset. In XPU mode the CPU device must run the GPU's subset:
  - no CVEX (preview fallback);
  - the GPU's texture filter (bilinear/trilinear, deterministic level selection) instead of OIIO's stochastic filtering;
  - the same light and volume code (already shared).

  A mixed image with VEX-shaded CPU tiles and preview-shaded GPU tiles would show seams.
- **Work distribution.** A scheduler hands out (pixel range, sample range) items. The GPU takes waves of up to 2M path slots, and the CPU takes 32 × 32 tiles as today. Sample indices stay a function of the pixel's count, as now (`generateCameraRays` uses `film.count[pix] + j`, `wavefront.cpp:577`), so devices never render the same sample. Per-device contribution is the share of samples.
- **Film.** Each device accumulates into its own film (the GPU in device memory). The host merges per pass, or the GPU film is primary and CPU tiles are uploaded.

**Recommended:**
- M10 delivers the GPU device and keeps the CPU wavefront as the second device behind one `RenderDevice` interface.
- A mixed mode ("XPU") comes last in the porting order (10.4). It is enabled only when it helps, which on this machine is mostly as failover.

---

## 9. Code sharing (question 8)

### 9.1 What exists (from code, measured)

- **Macros** (`core/math/common.h:9-35`):
  - `NOVA_HOST_DEVICE` (`__host__ __device__` under nvcc);
  - `NOVA_INLINE`;
  - `NOVA_FORCEINLINE` (`__forceinline__` on CUDA);
  - `NOVA_OUTLINE` (inline on CUDA, noinline on the host).
- **K14 compile check.** `tests/cuda/kernel_headers.cu` compiles math, BSDF lobes and closures (`buildClosure`, `sampleClosure`, `evalClosure`), the pixel filter, attribute fetches, Sobol/PCG and `shading::execute<1>` with `-Werror=cross-execution-space-call` for sm_120 (from code `tests/CMakeLists.txt:56-75`).
- **This report's check** (`cw_header_check.cu`) adds headers that compile as device code: `lights.h` (`sampleLight`, `lightSampleWeight`, `areaLightsAlongRay`, `escapedRadiance`, `cameraEnvironment`), `light_tree.h` (`LightTreeView::sample/pmf`), `light_sampler.h`, `distribution.h`, `lpe.h` (`Program::step`), `phase.h`, `subsurface.h` (`randomWalk` with a device lambda as tracer) and `hair.h`.
- **Failed the check**: `majorant.h` (5.2).

### 9.2 What blocks sharing (from code)

| Item | Problem | Device form (design) |
|---|---|---|
| `shading::Program` (`program.h:61`) | std::vector, std::string, shared_ptr | `ProgramView` {code offset, count, rowCount, globals, primvar bindings, outputs, texture slots} in flat device arrays |
| `Scene`, `MeshRecord`, `MaterialRecord` | std::vector members; `resolveHit`, `materialIdOf`, `attributeRefs`, `emitterLight`, `receiverSet`, `curvature` are host member functions | `SceneView` of raw pointers + counts; the math of `resolveHit` (scene.cpp:1289-) moved into `NOVA_INLINE` functions over a `MeshView` (pattern of `LightTreeView`) |
| `WaveQueues` / `ShadowQueue` / `LpeTables` | std::vector and `push_back`, `thread_local` scratch | `QueuesView` of SoA pointers + device counters; append through an `append(counter)` helper (atomic on the device, `++` on the CPU) |
| `evaluateSurfaceInputs` / `fillAndRun<N>` | `Scene` access, chunked N-lane rows | a per-lane `fillRows` + `execute<1>` path for the device (the CPU keeps `execute<32>`) |
| `VolumeEvaluator`, `VolumeEvalCache`, `MediaWalker` | unique_ptr, std::vector, `thread_local` | per-ray fixed arrays, a device NanoVDB sampler |
| `MajorantLayout::index/cellOf` | missing `NOVA_INLINE` | add it (one line each) |
| `ITextureSampler`, `IShadingQueries`, `IHitFilter`, `IAccel` | virtual interfaces | free device functions over tables; OptiX programs; the host-side `IAccel` launches OptiX on device queues |
| `ExternalShader` (CVEX) | host only | not ported (8.1) |

### 9.3 Recommendation: shared per-item kernel bodies (design)

The PLAN's rule ("gleiche Struktur wie später in CUDA") holds for the passes, but each pass today is one host function that loops over items and pushes into vectors.

M10's first step should therefore be a **CPU-only refactor**: every pass body becomes a `NOVA_HOST_DEVICE` function of (context view, queues view, item index) in a kernel-side header, for example `core/integrator/kernels/*.h`. The CPU loops call these functions over their items (TBB threads per tile as now). The CUDA kernels call the same functions one thread per item.

Done this way:
- The CPU images must stay bit-identical through the refactor (the existing reference tests).
- The GPU gets the integrator logic, not a copy of it.
- Every new kernel-side header joins `kernel_headers.cu` (lights, light tree, LPE, phase, subsurface, hair, distribution and the fixed majorant header first).

---

## 10. Recommended M10 architecture (design)

### 10.1 Modules

- **`core/gpu/`** (CUDA language in CMake, nvcc host compiler MSVC):
  - `DeviceScene`: views uploaded from `Scene` (mesh views, material programs and uniforms, lights, the light tree, distributions, volume grids and majorants, the texture table);
  - `DeviceQueues`: SoA allocations per wave capacity;
  - the kernels (`.cu`) that call the shared kernel bodies (9.3);
  - a `GpuRenderer` with the same interface as `Renderer` (`renderWave`-like waves of pixel samples).
- **OptiX module** (OptiX report): intersect, occlusion, any-hit opacity, and the raygen variants of kernels 5q, 6 and 10.
- **Hydra**: device selection, IPR delta uploads, the OptiX denoiser reading device buffers directly (M7).

### 10.2 Kernels and launches

- **Kernels** as in 2.2.
- **Persistent grid-stride kernels**: grid = SMs × resident blocks; each thread loops `i < *count` over a device-side queue counter. No host reads queue sizes back.
- **The bounce loop** (maxDepth iterations unrolled, about 12 nodes each) is captured once per (scene, settings) as a CUDA graph. Measured launch cost falls from 5.4-7.3 µs to 0.4-0.8 µs. The graph is re-captured when settings that change the kernel set change.
- **Sorts** run over the full capacity with sentinel keys (2.5), so CUB's host-side item count is fixed and capturable.
- **Shade kernel**: one `shadeSurface` launch per bounce over the sorted queue, `execute<1>` per lane with:
  - rows in local memory (`float R[256]`);
  - uniforms read-only from global memory;
  - code in a global program buffer;
  - device Tex2D.
- **Build flags**:
  - `-arch=sm_120` for kernels without CUB;
  - `-gencode arch=compute_90,code=sm_120` for translation units with CUB under CUDA 12.9 (section 0, item 12), or a newer CCCL;
  - `-Werror=cross-execution-space-call`;
  - `--fmad=false` in the parity/test configuration (production default: open question, 11).

### 10.3 Data layout

- **Path slots**: pixel × spp + j within a wave (the CPU layout). SoA arrays of the path state, plus accumulators (radiance, alpha, LPE outputs, AO).
- **Ray queues**: `rays` and `nextRays` (SoA, 41 B per ray), append counters.
- **Hits**: indexed by ray.
- **Shade queue**: (key, ray) pairs in two buffers for CUB.
- **Shadow slots**: 3 per path, plus a compacted trace list; LPE part arena.
- **Volume-event and walk queues**: only allocated when the scene uses them.

About 0.65-0.7 KB per path plus 12 B per LPE output; 2M paths per wave.

### 10.4 Porting order, each step rendering and tested against the CPU

| Step | Content | Test |
|---|---|---|
| G0 (CPU only) | Shared kernel bodies (9.3); device views of Scene/Program/LPE tables; `majorant.h` fix; `kernel_headers.cu` extended | CPU reference images bit-identical before/after; nvcc compile check |
| G1 | `GpuRenderer`: camera rays, OptiX intersect/occlusion, pass A (environment, camera-visible lights, area lights along rays), Lambert/default material without the VM, NEE with all analytic lights (power table), MIS, Russian roulette, per-path accumulators, owned film flush, CUDA graph | Cornell box, furnace tests: nova_imgcmp, CPU as reference; run-to-run bit-identity |
| G2 | VM on the device (`execute<1>`, varying and varying-free materials), Standard Surface / OpenPBR / generic graph closures, material sort, resident textures + UDIM, ray cones | M3 material spheres and textured scenes vs CPU (filter regions documented); kernel-level VM and BSDF differential tests |
| G3 | Light tree, emissive triangles (`emitterShadow`), light and shadow linking, dome importance sampling | M9 light scenes vs CPU |
| G4 | LPE (device automaton, part arena, `resolveShadow`), first-hit AOVs, alpha, cryptomatte, AO | M6 LPE scenes: the sum of the parts equals the beauty; AOVs vs CPU |
| G5 | Volumes: NanoVDB upload, majorants, `traceMedia`, `shadeVolumes`, shadow transmittance, volume shader VM, emission, volume lights, equiangular | M8 Pyro scenes vs CPU |
| G6 | OptiX-launched kernels: SSS walks, shader ray queries (5q), fake caustics loop; hair and curves (OptiX curves); opacity any-hit | M9 SSS/hair scenes, M4 query scenes vs CPU |
| G7 | Hydra/IPR: delta uploads, adaptive sampling, OptiX denoiser on device buffers, texture budget warnings, device memory statistics | IPR latency target (< 100 ms), husk runs |
| G8 | XPU mode: `RenderDevice` interface, scheduler, CPU device in GPU-parity mode, failover | mixed-device renders vs GPU-only (statistical) |

### 10.5 Tests to add (design)

- A `cuda` test label:
  - kernel-level differential tests (7.3) with `--fmad=false`;
  - VM goldens on the device;
  - a device LPE automaton test;
  - a NanoVDB sampler test against `VolumeSampler`.
- GPU variants of every reference-scene test, with CPU renders as the reference (nova_imgcmp block tests).
- A determinism test: two GPU renders with the same seed are bit-identical.

---

## 11. Open questions

1. **One shade kernel or two.** The full shade kernel (VM + closure + NEE + LPE) will need more registers than the 72-128 measured for the VM alone, plus about 5 KB of local memory. A split into "evaluate material inputs" and "scatter" (Laine's material and logic kernels) trades a global round trip of the varying inputs for occupancy. Measure on real materials in G2.
2. **Specializing hot programs.** The interpreter is 3.6-4.5x slower than compiled code (3.2). Karma XPU compiles shaders per device (help). NVRTC-compiling a program's bytecode into a CUDA function and dispatching on it could close the gap for the most expensive materials, at the price of compile time in IPR.
3. **Wave tail and regeneration** (6.2): measure the last bounces' utilization before adding a per-pixel sample allocator for regeneration.
4. **Texture residency.** When does a production scene exceed the budget? Measure scene texture footprints before committing to demand loading (4.5).
5. **Data-texture filtering.** Use software bilinear by default for normal maps (1/512 weight error, measured), or only on request as Karma does?
6. **Fake caustics on the GPU.** Iterate closest hits in an OptiX raygen loop (the CPU's order and 8-crossing cap), or take the product in any-hit? The product is order-independent, but the cap semantics differ.
7. **`--fmad` in production.** It cost nothing in the VM probe. Measure on BSDF-heavy kernels; if the cost stays small, `--fmad=false` everywhere simplifies the parity story.
8. **Volume tracker divergence** (28-59 % SIMD efficiency): is chunked work redistribution worth it on Pyro scenes with volume shaders?
9. **CCCL version.** Stay on CUDA 12.9 with the compute_90 PTX workaround, take a standalone newer CCCL (header-only), or move to CUDA 13 for sm_120 CUB.
10. **Early termination inside a graph.** CUDA 12.4+ conditional graph nodes (lit, from memory) could stop the unrolled depth loop when the queues are empty. Alternatively accept a few µs per empty bounce.
11. **LPE part arena bound.** The largest number of distinct final states per light sample is bounded by lobes × sides. Confirm the worst case on generic graph closures (16 lobes).
12. **OptiX determinism.** Bit-identical closest hits across runs and builds of the same BVH (OptiX report).

---

## 12. Files (`docs/research/m10/probes/`)

| File | Purpose |
|---|---|
| `cw_build.bat` | Builds all `cw_` probes: `cw_build.bat [<outdir> (default %TEMP%\nova_cw_bin)] [vm\|vm_nofma\|compact\|sort\|tex\|vol\|sizes\|film\|headers]`. CUDA 12.9, VS 2022 Build Tools (vcvars64), sm_120; CUB probes as compute_90 PTX → sm_120 SASS; the volume probe as C++17 with `--extended-lambda` and `$HFS/toolkit/include` |
| `cw_vm_probe.cu`, `cw_out_vm.txt`, `cw_out_vm_nofma.txt` | Section 3: Nova's `execute<1>` on the device (variants A), divergence (B), CPU `execute<32>/<1>` (C), arithmetic-only calibration (A2/C2), agreement GPU/CPU and VM/hand-written (D); default `--fmad` and `--fmad=false` builds. Under a minute per run |
| `cw_compact_probe.cu`, `cw_out_compact.txt` | Section 2.4: compaction of 2M elements (atomics, CUB, thrust) and ray payload pushes |
| `cw_sort_probe.cu`, `cw_out_sort.txt` | Section 2.5: CUB radix sort by material key, counting sort, RLE, gather; launch overhead on a stream and in a graph |
| `cw_tex_probe.cu`, `cw_out_tex.txt` | Section 4.3: MIP-chain creation, tex2DGrad throughput, bindless handles, hardware filter precision, software bilinear |
| `cw_vol_probe.cu`, `cw_out_vol.txt` | Section 5.2: NanoVDB 32.6 on the device, trilinear lookups, ratio and delta tracking with and without a majorant grid, SIMD efficiency |
| `cw_film_probe.cu`, `cw_out_film.txt` | Section 6.3: owned film accumulation vs atomics, in order and shuffled |
| `cw_sizes.cu`, `cw_out_sizes.txt` | Sections 3.4, 6.1: device/host sizes of ClosureList, SurfaceInputs, LightData; WaveQueues bytes per path |
| `cw_header_check.cu`, `cw_out_headers.txt` | Section 9.1: device compile check of integrator-side headers (`-DCW_CHECK_MAJORANT` shows the `majorant.h` failure) |

Binaries go to the output directory, not into the repository. The probes include Nova headers read-only, CUDA's CUB/thrust and Houdini's NanoVDB headers. No SideFX code is copied. The Karma XPU statements are paraphrased from the help.

## 13. References

- S. Laine, T. Karras, T. Aila. Megakernels Considered Harmful: Wavefront Path Tracing on GPUs. HPG 2013 (sections 4.2-4.4 read for this report).
- M. Pharr, W. Jakob, G. Humphreys. Physically Based Rendering, 4th ed.; pbrt-v4 `src/pbrt/wavefront/integrator.cpp`, `workqueue.h`, `surfscatter.cpp`.
- Blender Cycles: `src/integrator/path_trace_work_gpu.cpp` (kernel scheduling, shader sorting, state compaction); SVM shader interpreter and `SHADE_SURFACE_RAYTRACE` (from memory).
- NVIDIA: CUDA C++ Programming Guide (texture objects, mipmapped arrays, local memory), CUB (`DeviceRadixSort`, `DeviceSelect`, `DeviceScan`, `DeviceRunLengthEncode`), OptiX Toolkit DemandLoading (`tex2DGrad` with residency, from docs/research/m3/textures.md section 5).
- K. Museth et al. NanoVDB (OpenVDB 11 / NanoVDB 32.6 headers in `$HFS/toolkit/include/nanovdb`).
- SideFX: Houdini 22.0 help, `solaris/karma_xpu` (devices, textures, materials), paraphrased.
- Nova: docs/design/M3.md (K5, K14, K15, section 4 "CUDA mapping"), docs/research/m3/plans/plan-gpu-perf.md section 2.6, docs/research/m3/textures.md section 5, docs/research/m3/shader-vm.md, docs/design/M5.md D12, docs/design/M8.md, docs/design/M9.md, docs/research/m7/denoising.md (pinned staging).
