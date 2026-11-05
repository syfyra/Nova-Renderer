# M10 research: the OptiX acceleration backend (OptiX 9.1 on an RTX 5080)

Scope: the acceleration side of docs/PLAN.md M10 ("GPU-Backend: CUDA-Wavefront-Kernels + OptiX-Accel ... XPU-artiger Modus"): how Nova's `IAccel` (core/accel/accel.h) maps onto OptiX 9.1, what it costs, and where OptiX behaves differently from the Embree backend (core/accel/embree_accel.cpp). The CUDA wavefront integrator and the bytecode VM on the GPU are covered by another M10 report; this one stops at ray queries and acceleration structures. It implements nothing in the renderer.

It answers eight questions:
1. Mapping every Nova geometry kind to OptiX 9.1 primitives, and how the hit attributes compare with Embree's.
2. Instancing and motion: IAS over GAS, SRT and matrix motion transforms, deformation keys, memory.
3. Hit filtering (stochastic opacity, shadow linking) as any-hit programs, and what any-hit costs.
4. Wavefront usage: raygen-only queries over SoA ray queues, closest-hit against miss-only designs, occlusion, payload registers, launch overhead.
5. Tracing one instance's GAS only (Nova's `intersectLocal` for subsurface walks).
6. Refit and compaction for IPR; build times for large meshes and many instances.
7. Precision: Nova's ray offsets with OptiX's triangle and curve intersectors.
8. Build integration: OptiX-IR or PTX from nvcc and CMake, embedding, module and pipeline creation, driver/ABI checks.

Sources:
- the OptiX 9.1 headers fetched by Nova's CMake (`$O` = `build/vs2022/_deps/nova_optix_headers-src/include`, `OPTIX_VERSION 90100`, `OPTIX_ABI_VERSION 118`, `$O/optix_function_table.h:20`);
- a stand-alone probe, `probes/oa_probe.cpp` with device programs `probes/oa_device.cu` (section 10). It ran on this machine:
  - NVIDIA GeForce RTX 5080 (sm_120, 84 SMs, 16 GB), driver 617.42 (nvidia-smi; the CUDA driver API reports 13.4), Windows 11;
  - CUDA 12.9 nvcc for the device code, MSVC (VS 2022 Build Tools) for the host;
  - the GPU also drives the desktop. All numbers reproduced within about ±3 % over six runs, except one run in which the deformation-motion section alone came out 30-45 % slower (not reproduced, see 3.4);
- Nova's code (the accelerator, scene, ray offsets, the OptiX denoiser's run-time loading);
- the literature (OptiX programming guide and release notes), marked "from memory" where not re-checked.

Every claim carries its source:
- **(measured)**: probe output, `probes/oa_out_all.txt` unless another file is named;
- **(from headers file:line)**: an OptiX 9.1 header under `$O`;
- **(code file:line)**: Nova's code;
- **(lit)** / **(lit, from memory)**: documentation or literature, re-checked or not;
- **(design)**: a proposal.

How the probe measures: every rate is the median GPU time (CUDA events) of 9 launches after 2 warm-up launches; builds are medians of 3-5 builds. "Mrays/s" counts rays per launch. The test scenes:
- **blob**: a bumpy UV sphere of 1000 x 500 quads, 1,000,000 triangles and 501,501 vertices (4M and 16M variants for build scaling);
- **world**: 8 x 8 instances of the blob (random rotation, scale 0.8-1.2, spacing 2.5) and a two-triangle ground; 65 instances, 64M instanced triangles;
- **ray sets**: *primary* 1920 x 1080 = 2,073,600 pinhole rays; *bounce*: from every primary hit a uniformly random direction in the hemisphere facing back along the incoming ray, primary misses replaced by random rays inside the scene box; *shadow*: from the primary hits to random points of a 6 x 6 square light above the scene, tmax = the distance;
- **hair**: 100,000 curves x 8 segments (800,000 segments) on a 10 x 10 patch, length 1, radius 0.005 to 0.002, with lean and wiggle; bounce rays take uniform sphere directions;
- **points**: 1,000,000 points in [0, 10]^3, radius 0.01-0.03.

---

## 0. Findings in short

1. **OptiX has no exact built-in equivalent for five of Nova's nine geometry kinds (measured, 2.3): analytic spheres, flat and oriented curves, discs and oriented discs.**
   - Built-in spheres and round curves report **no hits from inside**: a ray from the centre of a sphere, or from inside a tube, misses. Embree reports them for round curves (docs/research/m9/curves-hair.md:398, lit) and, from memory, for sphere points, and Nova's analytic spheres (`MeshShape::Sphere`, UsdGeomSphere) need them for glass. They need a custom intersection program.
   - `FLAT_QUADRATIC_BSPLINE` ribbons ignore normals (`normalBuffers` is "Reserved for future use", from headers optix_types.h:716-718) and face a fixed direction: their width runs along the curve's binormal (measured). OptiX has no ray-facing ribbon and no normal-oriented ribbon.
   - Discs and oriented discs exist only as custom primitives.
2. **Hit attributes match Embree's where both exist (measured, 2.2).** Triangle barycentrics follow P = (1-u-v) p0 + u p1 + v p2. The curve parameter is the basis parameter of the segment for linear, quadratic, cubic B-spline, Catmull-Rom and Bezier, exact to 1e-7. On linear end caps u is exactly 0 or 1, the condition `resolveCurveHit` already tests (code scene.cpp:1202). The ROCAPS curve variants report u only to about 1e-4.
3. **Ray-facing ribbons and ray-facing discs can use round tubes and built-in spheres** (design, 2.4). The coverage is the same (silhouette width 2r) and Nova computes their normals from the ray anyway. Normal-oriented ribbons need a custom intersector or tessellation; oriented discs need a custom intersector (2.1 G against 6.7 G primary rays/s for built-in spheres, measured).
4. **Curves: FAST_TRACE is essential, and linear curves are 2.7x faster than cubic (measured, 2.5).** Cubic B-spline FAST_TRACE: 6.2 ms build, 155 B per segment compacted, 2.0 G primary / 0.76 G bounce rays/s. FAST_BUILD needs only 45 B per segment but traces 2.8-3.6x slower. ROCAPS variants are 9-26 % faster. Linear: 49 B per segment, 5.4 G / 2.1 G.
5. **Triangles are fast to build (measured, 7.1).** A 1M-triangle GAS builds in 1.5 ms and compacts to 19.8 MB (34 % of the build size, 20 B per triangle); 16M in 25 ms (285 MB). Refit takes 0.22 ms against 1.35 ms for a rebuild, but loses up to 2.4x of the trace speed after large deformations, and an updatable GAS compacts only to 86 % (49 MB). **For IPR, rebuild instead of refitting.**
6. **Raw ray throughput is 7-17 G rays/s for triangles (measured, 5.1).** On the 64-instance world: primary 9.9 G, bounce 7.3 G, shadow 9.4-9.9 G rays/s; 2160p primary 12.0 G.
7. **A raygen-only design with hit objects is as fast as payload + closest-hit (measured, 5.1).** `optixTraverse` and the `optixHitObject*` getters read t, primitive, instance and the attributes in the raygen. No closest-hit or miss program and no payload registers are needed (9.83 against 9.88 G rays/s).
8. **Pipeline flags cost 7-12 % on closest-hit and 20-23 % on occlusion rays in static scenes (measured, 5.2).** `ALLOW_ANY`, `ALLOW_SINGLE_GAS` or `usesMotionBlur` each drop primary rays from 9.9 to 8.8-9.1 G and shadow rays from 9.9 to 7.6-7.9 G. Recommended: separate pipelines for static scenes, motion scenes and subsurface walks.
9. **Launch overhead is about 13 µs per `optixLaunch`, CPU-bound (measured, 5.3).** A plain `cuLaunchKernel` costs 5.3 µs. Uploading the params per launch adds 5 µs. Below about 64k rays a launch is overhead-dominated (4,096 rays: 0.25 G rays/s; 2M rays: 6.7 G rays/s). Launching at the queue's capacity with the count on the device costs 21 µs instead of 14-19 µs and avoids a host read-back. Capturing `optixLaunch` into a CUDA graph failed here.
10. **Any-hit is cheap in itself (measured, 4.1).** A trivial any-hit costs 11-14 % on closest-hit primary rays, 5-8 % on bounces and nothing measurable on occlusion. It runs about once per hitting ray (1.001 calls at opacity 1; 1.4-1.5 at opacity 0.5). The real cost will be the opacity program. Instance flags disable it per instance at the speed of the geometry flag, so Nova's `setMeshFiltered` needs only an IAS rebuild.
11. **Shadow linking through visibility masks is 2.8x faster than an any-hit filter (measured, 4.3).** There are 8 mask bits. With 32 of 65 instances in the shadow set: mask 14.8 G, no linking 9.4 G, any-hit filter 5.3 G rays/s.
12. **OptiX interpolates SRT keys with nlerp, Embree (and Nova's `interpolateSrt`) with slerp (measured, 3.2).** With keys 0° and 90°, OptiX turns the instance by 21.60° at t = 0.25; slerp gives 22.50°. The largest difference is 0.0012° for 10° per key interval and 0.92° for 90°. Matrix motion lerps the matrices (18.43°, and the object shrinks). Nova's `SrtTransform` holds the same fields as `OptixSRTData` in a different order (quaternion real part first), so the keys need repacking, not a memcpy.
13. **Motion is expensive (measured, 3.4-3.5).**
    - Deformation keys: compacted memory grows about 46 MB per key for 1M triangles (2 keys 57.8 MB, 16 keys 733 MB). Rays/s drop by 34-62 %.
    - Instance motion through SRT or matrix transforms: 5.8 G against 8.8 G rays/s for static instances.
    - IAS motion keys did not help.
    - Building an IAS over SRT transforms takes 5.7 ms for 64 instances with 20° per key interval, against 0.08 ms without rotation.
    - The limit is 255 keys.
14. **`intersectLocal` on a GAS handle works and costs nothing extra (measured, 6).** Tracing the mesh's GAS with Nova's object-space ray runs at 4.16 G rays/s, the same as a one-instance IAS, with the same primitive for 99.98 % of the hits. It needs `ALLOW_SINGLE_GAS`, so it belongs in its own pipeline.
15. **Nova's `offsetRayOrigin` gives zero self-intersections on OptiX (measured, 8).** This holds at translations of 0, 1e3 and 1e5 for reflection and transmission. A fixed tmin of 1e-4 fails from 1e3 on (26 % self-hits). Triangles are watertight: 560,601 rays aimed exactly at vertices and edge midpoints from inside a closed mesh all hit, at every translation. Curves need `tubeRayOrigin` unchanged: rays into a fibre do not see its exit on OptiX, so jumping to the exit is required.
16. **Build integration is simple (measured, 9).** `nvcc -optix-ir` (or `-ptx`, equally fast at run time) from CUDA 12.9 with `-arch=compute_75`, or CMake 3.27+'s `CUDA_OPTIX_COMPILATION`. Embed the bytes in the DLL. Module compilation takes 118-130 ms without cache and 0.5 ms from OptiX's disk cache. `optixInit` reports `OPTIX_ERROR_UNSUPPORTED_ABI_VERSION` on old drivers, as the denoiser already handles.
17. **A pitfall: legacy-stream copies are not ordered with a non-blocking stream (measured, 9.4).** A pageable `cuMemcpyHtoD` may still be in flight when work on a `CU_STREAM_NON_BLOCKING` stream starts, which produced `CUDA_ERROR_ILLEGAL_ADDRESS` in IAS builds. All copies must go through the backend's stream.

---

## 1. Nova today

- **Interface** (code accel.h:70-112): batched `intersect` / `occluded` over a SoA `RayBatch` (`org`, `dir`, `tMax`, optional `time`, `filterSeed`, `rayType`, `depth`, `shadowCategory`), writing a SoA `HitBatch` (`t`, `meshId`, `primId`, `uv`, optional `instance`). Rays start at t = 0 (origins are already offset). `intersectLocal` traces one mesh in object space for subsurface walks; hit filters do not apply there (code accel.h:101-107).
- **Hit filter** (code accel.h:58-68): `IHitFilter::accept` runs during traversal for filtered meshes only. It may run several times per hit and must decide from the candidate and the ray alone. "The same structure as an OptiX any-hit program." Nova's filter (code scene.cpp:116-147):
  - fake caustics: shadow rays ignore the mesh;
  - shadow linking: a light's shadow category against the instance's mask;
  - opacity: uniform or evaluated by the opacity program, then a hash of (filter seed, mesh, prim, instance) decides stochastically.
- **Embree backend** (code embree_accel.cpp):
  - one BLAS per mesh, one `INSTANCE_ARRAY` per mesh in the top scene; geometry ID = `MeshId`, `instPrimID` = instance index;
  - SRT keys shared with Embree as `RTCQuaternionDecomposition` (code embree_accel.cpp:21-26);
  - refit of meshes with unchanged topology, a rebuild every 16 edits (code embree_accel.cpp:199-224);
  - filter functions enabled per mesh, because they cost about 5 % even when unused (code embree_accel.cpp:32-41).
- **Geometry** (code mesh.h:24-35, embree_accel.cpp:113-197):
  - triangles;
  - analytic spheres: one `SPHERE_POINT` with one centre and radius per key;
  - curves of four bases (linear, Bezier, B-spline, Catmull-Rom) in three styles: round, flat (ray-facing) and oriented (normals; linear falls back to flat);
  - points as spheres, ray-facing discs or oriented discs.
  - Round linear curves use neighbour flags, so caps appear only at curve ends.
- **Hit resolution** reads `uv.x` as the curve parameter and treats `u <= 0 || u >= 1` on linear round curves as a cap (code scene.cpp:1169-1243). Points ignore `uv` (code scene.cpp:1246-1290).
- **Motion**: uniform keys over the shutter. `motionSegment` mirrors Embree's segment lookup, and `interpolateSrt` reproduces Embree's slerp (code motion.h:20-26, 72-119). At most 16 steps (code motion.h:15).
- **Ray offsets**: `offsetRayOrigin` (Wächter and Binder) and `tubeRayOrigin`. The latter moves rays entering a round fibre to the chord's exit, "Embree's round curves report hits from inside the tube" (code ray.h:37-73).
- **OptiX today**: only the denoiser. It resolves `nvcuda.dll` at run time and calls `optixInit`, which loads `nvoptix.dll`. It works on a `CU_STREAM_NON_BLOCKING` stream (code optix_denoiser.cpp:36-62, 142-172).

---

## 2. Geometry kinds (question 1)

### 2.1 What OptiX 9.1 offers (from headers)

- Built-in primitive types (optix_types.h:600-628):
  - `TRIANGLE`;
  - `SPHERE`;
  - `CUSTOM` (AABBs plus an intersection program);
  - curves: `ROUND_LINEAR`, `ROUND_QUADRATIC_BSPLINE`, `ROUND_CUBIC_BSPLINE`, `ROUND_CATMULLROM`, `ROUND_CUBIC_BEZIER`, the four `*_ROCAPS` variants ("using rocaps intersection"), and `FLAT_QUADRATIC_BSPLINE` (a ribbon, "oriented, flat cross-section").
- End caps (optix_types.h:665-671): by default, round caps for linear curves and none for quadratic or cubic ones. `OPTIX_CURVE_ENDCAP_ON` adds flat caps to quadratic and cubic segments.
- Curve input (optix_types.h:673-741):
  - one index per segment, its first control point, as Nova's `curveSegments`;
  - separate vertex and width buffers per motion key. Width means the radius and is interpolated with the curve basis;
  - normals are "Reserved for future use";
  - one SBT record per curve build input.
- Hit queries in raygen through hit objects (optix_device.h:1588-1767): `optixHitObjectGetPrimitiveIndex`, `...GetInstanceId`, `...GetInstanceIndex`, `...GetHitKind`, `...GetTriangleBarycentrics`, `...GetCurveParameter`, `...GetRibbonParameters`, `...GetAttribute_0..7`.

### 2.2 Hit attributes against Embree's (measured, `attrs` in oa_out_all.txt)

| Probe | OptiX result | Embree / Nova expectation |
|---|---|---|
| Triangle p0 = 0, p1 = x, p2 = y, hit at (0.2, 0.3) | u 0.200000, v 0.300000; hit kind 0xFE front, 0xFF back | the same convention (code accel.h:38) |
| Round linear, x = 0..1..2, r = 0.1: side hit at x = 0.3 | t 0.9, prim 0, u 0.300000 | u along the segment |
| ... at the joint x = 1 | prim 0, u 1.0 | |
| ... end cap from the side (x = 2.05) / obliquely / along the axis | prim 1, u 1.000000; t 0.9134 / 0.9697 / 0.9000, on the cap sphere | caps report u = 0 or 1 (Nova's cap test) |
| ... start cap from the side / obliquely | prim 0, u 0.000000 | |
| ... a ray exactly parallel to the axis, 0.05 off it, onto the start cap | **misses the start cap**, hits the sphere at the joint (t 1.913) | degenerate case, see below |
| Quadratic / cubic B-spline, Catmull-Rom, Bezier: side hits at u = 0.1, 0.25, 0.5, 0.8 | reported u equal to 1e-7; x(u) − x(ray) ≤ 3.6e-7 | u = the basis parameter (Embree's u) |
| The same with ROCAPS | u off by up to 2.6e-4 (x error up to 3.2e-4 on a segment of length 2) | |
| Cubic curves, rays onto the open ends (axial, oblique, beyond the end) | miss (open sweep, no inner wall) | open ends, as Embree and Karma (curves-hair.md:38) |
| Cubic B-spline + `ENDCAP_ON`, oblique rays onto the end faces | hit, u 1.0 at the end, u 0.0 at the start | flat caps |
| Any round curve, ray from inside the tube (axis or 0.0001 below the surface) | **miss** | Embree: hit (curves-hair.md:398, lit) |
| Built-in sphere r = 1: from outside / from the centre / from 0.999 / from the surface outwards | t 4 front / **miss** / **miss** / miss | Embree's sphere points: exit hit (lit, from memory) |
| `FLAT_QUADRATIC_BSPLINE`, straight along x | hit from +z (normal (0, 0, −1)), **miss from ±y** | no ray-facing ribbon |
| ... bent in the xy plane | hit from ±y and obliquely, normal (0, −1, 0); miss from +z | width along the binormal |
| ... bent in the xz plane | hit from +z, normal (0, 0, −1); miss from ±y | |

- The ribbons' v (across) is reported (`optixGetRibbonParameters`), and their normal from `optixHitObjectGetRibbonNormal`. Hits from either side were front-facing.
- The rays exactly parallel to a linear segment's axis behave oddly: they miss the near cap and then hit the cap sphere of the joint from inside the cone. A ray inside a tube sees the end-sphere surfaces of the segment, because the segment is a union of a cone and spheres. Rays from outside are unaffected (they stop at the first hit); exactly axis-parallel rays are rare.

### 2.3 The mapping (measured semantics; design for the choices)

| Nova kind | Embree today (code) | OptiX 9.1 | Exact? |
|---|---|---|---|
| Triangles | `TRIANGLE` | built-in triangles | yes |
| `MeshShape::Sphere` (analytic, one primitive, keys) | `SPHERE_POINT` | **custom sphere IS** (reports the exit when the entry is behind tmin) | yes; the built-in sphere misses from inside (glass) |
| Curves, linear, round | `ROUND_LINEAR_CURVE` + neighbour flags | `ROUND_LINEAR` | caps at every segment end (u = 0/1); the outer surface is the same union |
| Curves, cubic (Bezier, B-spline, Catmull-Rom), round | `ROUND_*_CURVE` | `ROUND_CUBIC_BEZIER` / `_BSPLINE` / `ROUND_CATMULLROM`, or their `_ROCAPS` | yes (ROCAPS: u to 1e-4) |
| Curves, flat (ray-facing), any basis; linear oriented (falls back to flat) | `FLAT_*_CURVE` | the **round** type of the same basis | same coverage, t up to r earlier, see 2.4 |
| Curves, cubic, oriented (normals) | `NORMAL_ORIENTED_*_CURVE` | **none built in**: tessellate to triangles (host) or a custom IS | no built-in |
| Points, spheres | `SPHERE_POINT` | built-in `SPHERE` when the material does not transmit, else the custom sphere | no exit hits on the built-in sphere |
| Points, discs (ray-facing) | `DISC_POINT` | built-in `SPHERE` (or a custom disc IS) | same coverage, t up to r earlier |
| Points, oriented discs | `ORIENTED_DISC_POINT` | custom disc IS | yes |

Nova's Hydra side never produces quadratic curves (UsdGeomBasisCurves has no quadratic basis; code mesh.h:30-32), so `*_QUADRATIC_*` and the ribbon type are unused.

### 2.4 Why round tubes and spheres can stand in for ray-facing ribbons and discs (design)

- A ray-facing ribbon of half-width r and a tube of radius r cover the same pixels: both are 2r wide across the projected tangent. Only where the tangent points along the ray does the tube show a disc where the ribbon shows a line. The same holds for ray-facing discs against spheres.
- `resolveCurveHit` computes a flat curve's normal from the ray (`across(-dir)`) and `resolvePointHit` a disc's as `-dir` (code scene.cpp:1198, 1266). Neither reads the hit point's position on the surface.
- The hair offset h projects P − centre on b = T x dir (code scene.cpp:1223-1225). A tube hit and a ribbon hit on the same ray differ only along the ray, which b is orthogonal to, so h is the same.
- What changes is t: the tube's surface lies up to r in front of the ribbon's plane. For hair radii this is far below anything visible. Transmitted rays from such hits go through the surrogate without seeing its exit, because OptiX reports no inside hits.
- The oriented styles are the real gap. Oriented ribbons (grass blades) seen edge-on must vanish, which a tube does not do. Options:
  - tessellate each segment into a strip of 4-8 quads oriented by the interpolated normals, and map each triangle back to (segment, u) in the raygen;
  - or a custom intersector after Embree's (ray-space subdivision of the ribbon).
  Tessellation keeps hardware traversal and is the smaller project.

### 2.5 Curve cost (measured, `curves`; 800,000 segments; rays/s in G)

| Type | FAST_TRACE build | compacted (B/segment) | FAST_BUILD build, compacted | primary FT / FB | bounce FT / FB | occlusion bounce FT |
|---|---|---|---|---|---|---|
| ROUND_LINEAR | 1.93 ms | 37.6 MB (49) | 1.57 ms, 41.2 MB (54) | **5.37** / 4.55 | **2.07** / 2.08 | 2.71 |
| ROUND_CUBIC_BSPLINE | 6.19 ms | 118.1 MB (155) | 1.02 ms, 34.4 MB (45) | 2.00 / 0.72 | 0.76 / 0.26 | 0.92 |
| ... + ENDCAP_ON | 6.21 ms | 118.1 MB (155) | 1.00 ms, 34.4 MB | 1.96 / 0.72 | 0.75 / 0.26 | 0.91 |
| ROUND_CUBIC_BSPLINE_ROCAPS | 6.47 ms | 118.0 MB (155) | 1.02 ms, 34.4 MB | **2.22** / 0.92 | **0.86** / 0.41 | 1.03 |
| ROUND_CATMULLROM | 6.15 ms | 115.5 MB (151) | 1.02 ms, 34.2 MB | 1.50 / 0.41 | 0.58 / 0.15 | 0.71 |
| ROUND_CATMULLROM_ROCAPS | 6.47 ms | 115.5 MB (151) | 1.05 ms, 34.2 MB | 1.85 / 0.70 | 0.73 / 0.30 | 0.88 |
| ROUND_CUBIC_BEZIER | 6.27 ms | 136.5 MB (179) | 1.10 ms, 55.5 MB (73) | 1.69 / 0.52 | 0.63 / 0.19 | 0.76 |
| ROUND_CUBIC_BEZIER_ROCAPS | 6.48 ms | 136.4 MB (179) | 1.12 ms, 55.6 MB | 1.84 / 0.71 | 0.70 / 0.30 | 0.84 |
| ROUND_QUADRATIC_BSPLINE (_ROCAPS) | 6.08 (6.30) ms | 116.2 MB (152) | 0.98 ms, 32.7 MB | 2.11 (2.40) / 0.65 (1.03) | 0.77 (0.90) / 0.26 (0.44) | 0.95 (1.08) |
| FLAT_QUADRATIC_BSPLINE | 7.42 ms | 135.1 MB (177) | 2.04 ms, 57.3 MB | 1.57 / 0.61 | 0.66 / 0.29 | 0.71 |

- **FAST_TRACE splits cubic segments into several boxes.** It needs 3.4x the memory of FAST_BUILD and 370 MB of temporary memory for 800k segments, and pays it back with 2.8-3.6x the trace speed. For linear curves the two flags are close.
- ROCAPS is 9-26 % faster than the regular intersector of the same basis, with no caps either (2.2) and u to 1e-4.
- For a rough comparison (different scenes): triangles in the same harness reach 9.9 G primary and 7.3 G bounce rays/s on 64M instanced triangles (5.1). Cubic hair is 5-10x more expensive per ray than triangles; linear hair 2-3.5x.
- Nova's Bezier segments share end points (index stride 3), so a Bezier curve needs 3n + 1 control points. The B-spline and Catmull-Rom ones need n + 3 (code mesh.h:59-70; probe `makeHair`).

### 2.6 Point cost (measured, `points`; 1M points; rays/s in G)

| Variant | build | compacted | primary | bounce | occlusion bounce |
|---|---|---|---|---|---|
| built-in `SPHERE` | 2.34 ms | 41.2 MB (43 B/point) | **6.74** | **5.09** | 5.86 |
| custom sphere (AABB + IS) | 1.63 ms | 31.7 MB (33 B/point) | 4.01 | 2.94 | 3.44 |
| custom ray-facing disc | 1.63 ms | 31.7 MB | 4.00 | 2.91 | 3.43 |
| custom oriented disc | 1.65 ms | 31.7 MB | 2.13 | 1.41 | 1.53 |

A custom intersection program costs about 40 % against the built-in sphere. The oriented discs are slower still: edge-on discs let rays travel further (85 % against 92 % primary hits), and their AABB, the sphere's box, is loose.

---

## 3. Instancing and motion (question 2)

### 3.1 IAS over GAS (measured, `info`, `ias`; from headers)

- Limits on this GPU (measured): 2^29 primitives per GAS, 2^28 instances per IAS, instance ids up to 2^28 − 1, **8 visibility-mask bits**, trace depth and traversable graph depth 31. Shader execution reordering and cluster acceleration structures are supported (flags 1). RT core version 40.
- `OptixInstance` is 80 bytes: a 3x4 row-major object-to-world transform, `instanceId`, `sbtOffset`, `visibilityMask`, `flags` and the child handle (from headers optix_types.h:1003-1027). Nova's `Xform34` is column-major (`c[col][row]`, code xform.h:15-17), so instances are written into a separate array; Embree shares Nova's buffer.
- IAS memory is about 196 B per instance (1M instances: 195.9 MB, plus the 76 MB instance array; measured).
- The probe's hit record reads `optixHitObjectGetInstanceId` (set to the mesh) and `...GetInstanceIndex` (the position in the IAS). With the instances of a mesh contiguous in the IAS, Nova's instance number is `instanceIndex − firstInstance[mesh]` (design).

### 3.2 SRT and matrix motion transforms (measured, `attrs`; from headers)

- An `OptixSRTMotionTransform` is a traversable between the IAS instance and the GAS. It holds N keys of `OptixSRTData {sx, a, b, pvx, sy, c, pvy, sz, pvz, qx, qy, qz, qw, tx, ty, tz}`, M = T R S (optix_types.h:1351-1382).
- Nova's `SrtTransform {sx, sy, sz, sxy, sxz, syz, px, py, pz, qw, qx, qy, qz, tx, ty, tz}` (code motion.h:32-38) has the same meaning: a = sxy, b = sxz, c = syz, pv = p, quaternion real part last in OptiX. motion.h's comment says the layout "maps field by field to OptiX's OptixSRTData, so key arrays can be shared". The fields map, but the order differs, so the keys need a 64-byte repack each.
- **Interpolation**: "the elements of the components of S, R, and T will be interpolated linearly" (optix_types.h:1376). The device helper renormalises the lerped quaternion (internal/optix_device_impl_transformations.h:197-201), i.e. nlerp. The traversal does the same (measured): a target rotating about z between keys 0° and θ, found by scanning rays in 0.01° steps:

| keys | t | measured | slerp (Embree, Nova) | nlerp | matrix lerp |
|---|---|---|---|---|---|
| SRT 0°, 90° | 0.25 / 0.5 / 0.75 | 21.60 / 45.00 / 68.40 | 22.50 / 45.00 / 67.50 | 21.60 / 45.00 / 68.40 | 18.43 / 45.00 / 71.57 |
| SRT 0°, 170° | 0.25 / 0.75 | 35.77 / 134.23 | 42.50 / 127.50 | 35.77 / 134.23 | 4.92 / 165.08 |
| matrix 0°, 90° | 0.25 / 0.75 | 18.43 / 71.57 | | | 18.43 / 71.57 |

- The largest slerp − nlerp difference over a key interval of θ is:

| θ | difference |
|---|---|
| 2° | 0.00001° |
| 5° | 0.00015° |
| 10° | 0.0012° |
| 30° | 0.033° (5.8e-4 of the distance from the rotation axis) |
| 60° | 0.27° |
| 90° | 0.92° (1.6e-2 of that distance) |
| 170° | 6.8° |

- A hit rebuilt by `resolveHit` with slerp at θ = 30° per interval lies up to 5.8e-4 of its distance from the rotation axis off the surface the traversal hit. That exceeds the float offsets of `offsetRayOrigin`. The OptiX build must interpolate as OptiX does (recommendation 12.4).
- Matrix motion lerps the 3x4 matrices: rotating objects shrink between keys (radius 0.79 at 45° between 0° and 90°). Not usable for Nova's SRT keys.

### 3.3 Key limits (measured, `motion`)

- A GAS with 255 vertex keys builds; 256 fails with `OPTIX_ERROR_INVALID_VALUE` (`numKeys` is an unsigned short, optix_types.h:1203). Nova's limit of 16 (code motion.h:15) is far below.
- Transforms with N keys take 32 + 64N bytes (SRT) or 32 + 48N bytes (matrix): 160 B for 2 SRT keys, 1056 B for 16.

### 3.4 Deformation motion (measured, `motion`; 1M-triangle blob, one instance; random ray times)

| keys | build | compacted | temp | primary | bounce |
|---|---|---|---|---|---|
| 1 (motion pipeline) | 1.56 ms | 19.8 MB | 13.7 MB | 12.8 G | 7.4 G |
| 2 | 1.83 ms | 57.8 MB | 51.4 MB | 8.4 G | 4.0 G |
| 4 | 2.80 ms | 156.2 MB | 51.7 MB | 8.0 G | 3.8 G |
| 8 | 4.78 ms | 348.8 MB | 52.1 MB | 7.3 G | 3.3 G |
| 16 | 8.15 ms | 732.5 MB | 53.1 MB | 6.3 G | 2.8 G |

- Memory grows by about 46 MB per key per million triangles. Two keys already cost 2.9x the static size.
- The same scene in the static pipeline runs at 16.7 G / 10.5 G rays/s (`tri`). `usesMotionBlur` alone costs 23-30 % here, on the 64-instance world 7-12 % (5.2).
- An IAS with 2 motion keys over the deforming GAS changed nothing (same rates).
- One full run measured this section 30-45 % slower (1 key: 8.7 G). Neither the standalone run nor six later runs, alone or after each other test, reproduced it. The display or another GPU client is the likely cause; the table above is from the final run.

### 3.5 Instance motion (measured, `motion`; the 64-instance world, 20° spin and 0.6 translation over the shutter; random times)

| Variant | IAS build | primary | bounce |
|---|---|---|---|
| static instances, static pipeline | | 9.69 G | 7.52 G |
| static instances, motion pipeline (ALLOW_ANY, depth 3) | | 8.84 G | 6.24 G |
| SRT, 2 keys, static IAS | **5.66 ms** | 5.80 G | 3.51 G |
| SRT, 2 keys, IAS 2 keys | 5.68 ms | 5.76 G | 3.51 G |
| SRT, 16 keys, static IAS / IAS 16 keys | 0.79 / 0.75 ms | 5.72 / 5.74 G | 3.44 / 3.49 G |
| SRT, 2 keys, spin 1° | 0.11 ms | 6.01 G | 3.59 G |
| SRT, 2 keys, translation only | 0.08 ms | 6.00 G | 3.59 G |
| matrix, 2 keys, static IAS / IAS 2 keys | 0.08 / 0.09 ms | 5.81 / 5.78 G | 3.51 / 3.50 G |

- A motion transform per instance costs about 30-45 % against static instances in the same pipeline, even for pure translation. Each instance's box covers its whole motion, and every ray pays the transform's interpolation.
- IAS motion keys brought nothing here (expected to matter for large moves; not measured).
- **The IAS build time grows with the rotation per key interval**: 0.08 ms without rotation, 0.11 ms for 1°, 0.79 ms for 1.3° per interval over 15 intervals, 5.7 ms for 20° in one interval. OptiX presumably refines the motion bounds of rotating transforms. Nova resamples motion to uniform keys anyway and can keep the rotation per interval small.

---

## 4. Hit filtering (question 3)

### 4.1 Any-hit cost (measured, `anyhit`; the 64-instance world; G rays/s)

| Variant | closest primary | closest bounce | occlusion shadow | occlusion bounce |
|---|---|---|---|---|
| `GEOMETRY_FLAG_DISABLE_ANYHIT`, no AH program | **9.82** | **7.30** | 9.35 | 8.39 |
| AH empty (accept all) | 8.70 | 6.95 | 8.99 | 8.37 |
| AH = Nova's opacity hash, opacity 1 | 8.41 | 6.70 | 9.34 | 8.45 |
| ... opacity 0.5 (rays pass through, different result) | 6.02 | 4.54 | 6.01 | 4.96 |
| ... `REQUIRE_SINGLE_ANYHIT_CALL` | 8.39 | 6.70 | 9.35 | 8.44 |
| ... instance flag `DISABLE_ANYHIT` | **9.89** | **7.30** | 9.37 | 8.39 |
| ... ray flag `DISABLE_ANYHIT` | 9.90 | 7.51 | 9.36 | 8.39 |
| ... AH on 1 of 64 instances, the others flagged | 9.36 | 7.02 | 9.37 | 8.45 |

- An any-hit program costs 11-14 % on closest-hit primary rays and 5-8 % on bounces. On occlusion rays it costs nothing measurable: `TERMINATE_ON_FIRST_HIT` makes the first accepted hit the last.
- **Geometry built with any-hit enabled and instances flagged `DISABLE_ANYHIT` run exactly as fast as geometry built without** (9.89 against 9.82 G). The GAS can always be built AH-capable, and Nova's per-mesh `setMeshFiltered` becomes an instance flag (an IAS rebuild of 0.07 ms for 65 instances, 7.3), not a GAS rebuild.
- `REQUIRE_SINGLE_ANYHIT_CALL` cost nothing and changed nothing. Nova's decision is deterministic per candidate (code accel.h:58-62), so the flag is not needed.
- The probe's any-hit runs a hash and a constant. Nova's real cost is the opacity program (the VM, textures), times the invocation count below.

### 4.2 Invocations (measured, per-ray counters)

| opacity | closest primary | closest bounce | occlusion shadow | occlusion bounce |
|---|---|---|---|---|
| 1.0: per ray / per ray with any call / max | 0.815 / 1.001 / 4 | 0.583 / 1.008 / 5 | 0.356 / 1.000 / 1 | 0.578 / 1.000 / 1 |
| 0.5 | 1.153 / 1.416 / 13 | 0.838 / 1.449 / 15 | 0.535 / 1.503 / 12 | 0.835 / 1.443 / 15 |

- The traversal visits candidates nearly front to back: with opaque surfaces the any-hit runs once per hitting ray. The rate equals the hit fraction (81.4 % of primary rays hit).
- With `REQUIRE_SINGLE_ANYHIT_CALL` the counts were the same to 1 %.

### 4.3 Shadow linking (measured; 32 even blobs of 65 instances are in the light's shadow set)

| Method | shadow rays | bounce rays as occlusion |
|---|---|---|
| no linking (everything blocks; reference speed) | 9.36 G, 35.6 % blocked | 8.42 G |
| **visibility masks**: blobs in the set 0x81, the rest 0x80; ray mask 0x01 | **14.84 G**, 17.8 % | **12.98 G**, 22.0 % |
| any-hit filter on the instance id (64-bit set in the params) | 5.27 G, 17.8 % | 4.07 G, 22.0 % |

- Both give the same result. The mask culls whole instances during traversal; the filter visits them and rejects every hit. The masks are 2.8-3.2x faster.
- With 8 mask bits, Nova can map up to 7 shadow categories to bits and keep one bit set on every instance for unrestricted rays (design). Nova allows 64 categories (code scene.cpp:632). Lights beyond the first 7 categories fall back to the any-hit filter.
- Fake caustics (shadow rays ignore the mesh, code scene.cpp:119-120) fit the same scheme: a mask bit that shadow rays leave out.

---

## 5. Wavefront queries (question 4)

### 5.1 Query designs (measured, `trace`; static pipeline, SINGLE_LEVEL_INSTANCING; G rays/s)

| Design | primary 1080p | bounce | shadow |
|---|---|---|---|
| closest: raygen + `optixTraverse` + hit object getters (no CH, no MS, no payload) | 9.83 | 7.30 | |
| closest: raygen + `optixTrace` with 5 payloads (t, prim, instance id, u, v) written by a CH program; no miss program | 9.88 | 7.32 | |
| occlusion: `optixTrace` with `TERMINATE_ON_FIRST_HIT \| DISABLE_CLOSESTHIT`, 1 payload cleared by a miss program | | | **9.90** |
| occlusion: `optixTraverse` with `TERMINATE_ON_FIRST_HIT`, `optixHitObjectIsHit` | | 8.70 (bounce, tmax inf) | 9.31 |
| closest, hit object, primary 3840 x 2160 (8.3M rays) | 12.01 | | |
| one blob alone (IAS of 1 instance): closest, hit object | 16.63 | 10.44 | |

- The hit-object design reads everything the `HitBatch` needs in the raygen. The SBT then needs hit groups only for intersection and any-hit programs.
- For occlusion the classic trace + miss is 6 % faster in this pipeline. In the ALLOW_ANY and motion pipelines the two are equal (7.6-7.9 G).
- The OptiX-IR and PTX modules ran equally fast (9.91 against 9.83 G; `trace`).
- Payload registers: the hit-object design uses none, the payload design 5. Both run at the same speed here, so the register count is no argument either way. `numAttributeValues` = 2 suffices for every built-in type and the custom programs (from headers optix_types.h:2528-2530).

### 5.2 Pipeline flags on a static scene (measured; G rays/s, hit-object designs)

| Pipeline | primary | bounce | shadow (trace + miss) |
|---|---|---|---|
| `ALLOW_SINGLE_LEVEL_INSTANCING`, no motion | **9.83** | **7.30** | **9.90** |
| `ALLOW_SINGLE_GAS \| ALLOW_SINGLE_LEVEL_INSTANCING` | 8.83 | 6.46 | 7.90 |
| `ALLOW_ANY` (graph depth 2) | 8.92 | 6.45 | 7.86 |
| `ALLOW_SINGLE_LEVEL_INSTANCING`, `usesMotionBlur` | 9.10 | 6.47 | 7.86 |
| `ALLOW_ANY` (depth 3), `usesMotionBlur` | 8.85 | 6.41 | 7.64 |

- Any flag beyond plain single-level instancing costs 7-12 % on closest-hit rays and 20-23 % on occlusion.
- Instance motion transforms require `ALLOW_ANY`: an IAS → transform → GAS graph is not "single level instancing" (from headers optix_types.h:2102-2113).
- Deformation motion requires `usesMotionBlur`.
- Tracing a GAS directly requires `ALLOW_SINGLE_GAS`.

### 5.3 Wave size and launch overhead (measured, `launch`; shuffled bounce rays; µs per launch)

| rays | empty raygen | + params upload per launch | CPU submit | `cuLaunchKernel`, same work | trace | trace G rays/s | launch + sync, empty / trace | at capacity (2M threads, count on the device) |
|---|---|---|---|---|---|---|---|---|
| 1 | 13.6 | 18.7 | 13.6 | 5.5 | 13.8 | < 0.001 | 13.2 / 40.0 | 22.4 |
| 1,024 | 12.7 | 19.8 | 12.7 | 5.2 | 15.4 | 0.07 | 13.6 / 25.4 | 22.1 |
| 4,096 | 14.6 | 18.8 | 14.5 | 5.3 | 16.6 | 0.25 | 13.1 / 24.9 | 21.5 |
| 16,384 | 13.4 | 18.5 | 13.4 | 5.4 | 16.5 | 1.00 | 13.2 / 24.7 | 23.8 |
| 65,536 | 12.7 | 18.6 | 12.7 | 5.6 | 24.7 | 2.65 | 13.1 / 34.3 | 31.7 |
| 262,144 | 14.1 | 19.2 | 13.6 | 5.0 | 54.4 | 4.82 | 15.3 / 57.4 | 55.4 |
| 1,048,576 | 17.7 | 22.1 | 12.2 | 6.2 | 164.3 | 6.38 | 21.4 / 168.0 | 164.0 |
| 2,073,600 | 21.5 | 21.6 | 10.7 | 7.7 | 307.8 | 6.74 | 32.3 / 315.4 | 308.2 |

- **`optixLaunch` costs about 13 µs**: back-to-back empty launches take as long on the GPU as the CPU needs to submit them. A plain CUDA kernel with the same work costs 5.3 µs. Uploading new launch params each time (`cuMemcpyHtoDAsync` from pageable memory) adds about 5 µs.
- A launch below about 64k rays is overhead-dominated: 4,096 rays take 16.6 µs, 0.25 G rays/s against 6.7 G for 2M.
- Launching at the queue's capacity, with raygens that exit early above a count read from device memory, costs 21-24 µs for small counts: 5-8 µs more than an exact launch, and no read-back of the count. From about 64k rays on the two are equal.
- For a wavefront with, say, 6 trace launches per bounce and 8 bounces, the launch overhead is about 0.6-1 ms per sample pass. That matters for IPR at one sample per frame; recommendations 12.7.
- `optixLaunch` is not thread-safe per pipeline: "Concurrent launches require separate OptixPipeline objects" (from headers optix_host.h:624).

### 5.4 CUDA graphs (measured; `oa_out_launch_graph.txt`)

Capturing `optixLaunch` into a CUDA graph (`cuStreamBeginCapture`, global and relaxed mode) failed in every variant tried:
- during development (not in the output files): capturing 400 launches of a pipeline that had launched before failed inside the capture with CUDA error 905 ("dependency created on uncaptured work in another stream"), also with a fresh pipeline in the same context. With a fresh `OptixDeviceContext` and 400 launches, OptiX failed with 907 ("operation not permitted on an event last recorded in a capturing stream") once it reused its internal launch buffers;
- the final probe (`OA_GRAPH=1`: the probe's context, a fresh pipeline, 16 launches; `OA_GRAPH=fresh`: a fresh context): the capture succeeds, but the replay faults with `CUDA_ERROR_ILLEGAL_ADDRESS`. In development this happened even for a single captured launch.

The same fresh-context pipeline without capture works, so the traversable handles are valid across contexts. Whether OptiX 9.1 supports stream capture at all was not established (open question 13.3).

---

## 6. Tracing one instance's GAS (question 5; measured, `local`)

- **The setup**: Nova's `Scene::intersectLocal` already maps the world ray to the instance's object space and keeps the ray parameter (code scene.cpp:1444-1452). `IAccel::intersectLocal` traces the mesh's BLAS with that ray (code embree_accel.cpp:465-493).
- **The test**: 2M walk segments starting 0.002 inside the 1M blob in random directions, tmax 0.3; instance transform with rotation, scale 1.3 and translation.

| Query | rays/s | agreement |
|---|---|---|
| world rays through a one-instance IAS | 4.15 G | |
| object-space rays on the GAS handle (`optixTraverse(gasHandle, ...)`) | 4.16 G | 1,261,967 of 1,262,170 hits on the same primitive; max \|Δt\| 2.1e-4 on the same primitive (grazing rays); 3 rays hit in one query only |

- Tracing the GAS directly is the natural OptiX form of `intersectLocal`. A raygen can pick the GAS per ray from a table of handles, so one launch serves walks in different meshes.
- Deformation motion still works (the GAS keys use the ray time). Instance motion is applied by Nova's transform of the ray, which must then interpolate as OptiX does (3.2).
- The cost is the `ALLOW_SINGLE_GAS` flag (5.2): put subsurface walks in their own pipeline and launch, which a wavefront does anyway (docs/design/M9.md D9 already has a separate pass C'' for walks).
- A per-mesh IAS of one instance would also work at the same speed, but costs a build per instance and gains nothing.

---

## 7. Builds, refit and compaction (question 6)

### 7.1 Triangle GAS (measured, `tri`; GPU ms, median of 3)

| triangles (input) | flags | build | output | temp | compacted (share, time) |
|---|---|---|---|---|---|
| 1M (17.2 MB; host upload 1.6 ms) | FAST_TRACE | 1.51 | 59.0 MB | 13.7 MB | 19.8 MB (34 %, 0.05 ms) |
| | FAST_BUILD | 1.11 | 51.2 MB | 7.7 MB | 27.3 MB (53 %) |
| | FAST_TRACE \| ALLOW_UPDATE | 1.32 | 57.4 MB | 3.8 MB | 49.1 MB (86 %) |
| 4M (68.7 MB; 4.3 ms) | FAST_TRACE | 5.85 | 236 MB | 54.7 MB | 73.7 MB (31 %, 0.20 ms) |
| | FAST_BUILD | 4.83 | 205 MB | 31.0 MB | 109.7 MB |
| 16M (275 MB; 18.3 ms) | FAST_TRACE | 25.0 | 944 MB | 219 MB | 284.8 MB (30 %, 0.80 ms) |
| | FAST_BUILD | 21.9 | 820 MB | 124 MB | 436.3 MB |
| | FAST_TRACE \| ALLOW_UPDATE | 22.3 | 918 MB | 61 MB | 785.1 MB (86 %) |

- Builds scale linearly at about 1.5 ms per million triangles. Compaction is necessary: it cuts FAST_TRACE GASes to 30-34 %, about 18-20 B per triangle, and takes well under a millisecond.
- Nova's 12-byte `Vec3f` vertices build as fast as 16-byte ones (1.51 against 1.55 ms), although the header recommends a 16-byte stride (optix_types.h:520).
- Trace speed hardly depends on the build flag for triangles (1M blob: FAST_TRACE 16.7 G / 10.5 G, FAST_BUILD 16.6 G / 9.9 G primary / bounce).

### 7.2 Refit against rebuild (measured; 1M blob, FAST_TRACE | ALLOW_UPDATE)

| Edit | vertex upload | update | after update: primary / bounce | rebuild | after rebuild |
|---|---|---|---|---|---|
| small (bump +0.02) | 0.28 ms | 0.22 ms | 16.8 G / 9.9 G | 1.37 ms | 16.8 G / 9.9 G |
| large (twist 1.5 rad per unit y) | 0.29 ms | 0.22 ms | 15.1 G / 7.6 G | 1.37 ms | 16.8 G / 10.3 G |
| very large (twist 4 rad, bump 0.1) | 0.29 ms | 0.23 ms | 8.4 G / 3.5 G | 1.30 ms | 14.1 G / 8.4 G |

- A refit is 6x faster than a rebuild but saves only about 1.1 ms per million triangles. After the large edit the refitted tree traces 1.1-1.4x slower, after the very large one 1.7-2.4x. An updatable GAS compacts to 86 % instead of 34 % (2.5x the memory).
- Embree needed refit for IPR (docs/design/M7.md D7). On the GPU a rebuild of 1.4 ms per million triangles fits the 100 ms IPR budget up to about 60M edited triangles per update.

### 7.3 IAS builds (measured, `ias`; instances of a 20k-triangle GAS, random rotation and scale)

| instances | instance upload (pageable) | build FAST_TRACE / FAST_BUILD | output | update |
|---|---|---|---|---|
| 1 / 16 / 65 / 256 | 0.01 ms | 0.04 / 0.05 / 0.07 / 0.08 ms | < 0.1 MB | 0.02-0.04 ms |
| 1,000 | 0.03 ms | 0.11 / 0.12 ms | 0.3 MB | 0.03 ms |
| 100,000 | 0.61 ms | 0.31 / 0.29 ms | 28.7 MB | 0.12 ms |
| 1,000,000 | 5.76 ms (76 MB) | 2.24 / 2.09 ms | 195.9 MB, temp 81.5 MB | 0.94 ms |
| 4,000,000 | 19.5 ms (305 MB) | 10.4 / 9.9 ms | 692 MB | 5.4 ms |

- The IAS build time does not depend on the instanced GAS or the rotation (64 instances of 10k or 1M triangles, rotated or not: 0.061-0.065 ms).
- Single cold builds were sometimes slow: the world's first IAS of 65 instances took 5.3 ms in one measurement, against 0.07 ms as a median of repeated builds.
- Rays through 1M instances: 5.2 G primary, 5.0 G bounce. FAST_BUILD traced as fast as FAST_TRACE here. After the update that moved every instance, the IAS traced 5.9 G (a different scene; not a quality comparison).
- With rebuilds this cheap, the IAS is rebuilt at every commit; `ALLOW_UPDATE` is not worth it.

---

## 8. Precision (question 7; measured, `precision`)

### 8.1 Watertightness

The 160k-triangle blob is closed and star-shaped around its centre. From the centre, 560,601 rays aimed exactly at every vertex and every edge midpoint were traced with the instance translated by 0, 1e3 and 1e5 and rotated. **None missed** at any translation.

### 8.2 Triangles: self-intersection of secondary rays

From 576k primary hits on the rotated, translated blob:
- P is computed as Nova does it, by interpolating the world-space corners with the barycentrics (code scene.cpp:1313-1331);
- Ng is the geometric normal facing the incoming ray;
- reflection rays go into the hemisphere of Ng, transmission rays into the other;
- "self-hit" means the new ray hits its own triangle.

| translation | origin | reflection self-hits | transmission self-hits |
|---|---|---|---|
| 0 | P, tmin 0 | 41.6 % | 58.4 % |
| 0 | **`offsetRayOrigin(P, ±Ng)`, tmin 0** | **0** | **0** |
| 0 | P, tmin 1e-4 | 0.012 % | 0.022 % |
| 1e3 | P, tmin 0 | 64.2 % | 32.6 % |
| 1e3 | **`offsetRayOrigin`** | **0** | **0** |
| 1e3 | P, tmin 1e-4 | 26.0 % | 9.3 % |
| 1e5 | **`offsetRayOrigin`** | **0** | **0** |
| 1e5 | P, tmin 1e-4 | 16.8 % | 10.0 % |

Nova's offsets work unchanged on OptiX, including the ray's transformation into instance space by the traversal. A fixed epsilon does not, as expected.

### 8.3 Curves (20k curves x 8 segments, radius 0.005, rotated and translated)

- **Hit accuracy**: distance of P = org + t dir from the axis, relative to r:

| | translation 0: mean / max | 1e3: mean / max |
|---|---|---|
| linear | 1.8e-5 / 2.2e-4 | 5.2e-3 / 2.3e-2 |
| cubic B-spline | 3.1e-5 / 4.5e-4 | 5.4e-3 / 2.3e-2 |

  The error at 1e3 is the float resolution of world coordinates: 6e-5 absolute on a radius of 0.005.
- **Rays leaving the fibre** ("away", into the hemisphere of the radial normal): no same-segment hits within 4r for linear curves with the offsets (2 of 729k from the raw P at 1e3). For B-splines there were 0.05 % at translation 0, with or without offsets; probably genuine re-hits of bent segments at grazing angles (open question 13.6).
- **Rays into the fibre**: the same segment within 4r was hit as follows:

| | origin P | origin `offsetRayOrigin(P, −N)` (just inside) | origin `tubeRayOrigin` (the chord's exit) |
|---|---|---|---|
| linear, translation 0 / 1e3 | 51 % / 40 % | 0 / 0.35 % | 0.001 % / 0.04 % |
| cubic B-spline, 0 / 1e3 | 64 % / 39 % | 0 / 0 | 0.010 % / 0.002 % |

  - From P the ray starts slightly outside the tube and re-enters it at once (a self-hit).
  - From just inside it sees nothing: OptiX reports no inside hits (2.2), so the ray passes the fibre without its exit.
  - **`tubeRayOrigin` is therefore exactly right for OptiX**: it starts the ray at the exit, which OptiX would never report.

### 8.4 What changes against Embree

- Triangles: nothing.
- Round curves and spheres: no inside hits. That helps against self-intersection, but analytic spheres need the custom intersector for refraction (2.3).
- Flat curves lose Embree's built-in 2r self-intersection avoidance (curves-hair.md:398) when they become tubes. `tubeRayOrigin` takes over, as for round curves.
- Interpolated instance transforms must match OptiX's nlerp (3.2), otherwise rebuilt hit points leave the surface by up to 6e-4 of their distance from the rotation axis at 30° per key.

---

## 9. Build integration (question 8)

### 9.1 Device code

- `nvcc -optix-ir -arch=compute_75 -use_fast_math -std=c++17 -I<optix include>` produces OptiX-IR, `-ptx` PTX (probe `oa_build.bat`). compute_75 is enough: OptiX compiles for the device (sm_120 here) when the module is created. nvcc warns (#20044-D) about an `extern "C" __constant__` declaration; `extern "C" { __constant__ OaParams params; }` avoids it.
- CMake 3.31 ships with the VS 2022 Build Tools (measured `cmake --version`). Its `CUDA_OPTIX_COMPILATION` target property (new in 3.27) compiles an OBJECT library's `.cu` files to `.optixir` (`cmake --help-property CUDA_OPTIX_COMPILATION`).
- The files can be embedded with a custom command: CUDA's `bin2c.exe` (in `$CUDA_PATH/bin`, measured) or a `file(READ ... HEX)` script that writes a `.cpp` with a byte array.
- Nova's CMake currently only finds CUDA and the OptiX headers (code cmake/NovaOptiX.cmake:20-101). M10 adds `enable_language(CUDA)` for the device programs.
- OptiX-IR (63 KB) and PTX (55 KB) compiled equally fast (118-130 ms) and traced equally fast (5.1). OptiX-IR is the documented choice for debugging (lit, from memory).

### 9.2 Run time (measured, `info`)

- The probe follows the denoiser:
  - `nvcuda.dll` through `LoadLibrary` / `GetProcAddress`, with versioned exports such as `cuMemAlloc_v2` resolved through the macros (code optix_denoiser.cpp:36-62);
  - `optix_stubs.h` loads `nvoptix.dll` and links `Cfgmgr32.lib` itself (from headers optix_stubs.h:47-48).
- `optixInit` returns `OPTIX_ERROR_UNSUPPORTED_ABI_VERSION` when the driver predates ABI 118. The denoiser already reports that (code optix_denoiser.cpp:162-166).
- Module creation for the probe's 14 programs:
  - 118-130 ms with the cache off or cold;
  - **0.5 ms** from OptiX's disk cache (`optixDeviceContextSetCacheLocation` / `SetCacheEnabled`).
- A pipeline with 6 raygens and 5 hit groups (triangles, cubic B-spline and sphere built-in IS modules, a custom IS, an any-hit):
  - 8.2 ms with the module cached, 1.8 ms the second time;
  - 288 ms in an earlier run whose cache did not hold it yet (measured, not in the output file).
- Built-in IS modules come from `optixBuiltinISModuleGet` with the GAS build flags, the motion flag and the end-cap flags (from headers optix_host.h:466-474, optix_types.h:2329-2338). The probe gave curves built with FAST_BUILD a matching module.

### 9.3 Stack sizes and graph depth (from headers; measured in the probe)

- `optixUtilAccumulateStackSizes` and `optixUtilComputeStackSizes` (optix_stack_size.h:56, 90) with trace depth 1 and no callables. Then `optixPipelineSetStackSize` with a traversable graph depth of 2 (IAS → GAS), 3 with motion transforms, or 1 for a single GAS.

### 9.4 A stream-ordering pitfall (measured)

- The probe first copied host data with plain `cuMemcpyHtoD` and worked on a `CU_STREAM_NON_BLOCKING` stream.
- A pageable `cuMemcpyHtoD` returns once the data is staged, not delivered (lit: CUDA "API synchronization behavior"), and the legacy stream does not order a non-blocking stream.
- IAS builds that followed an instance upload then failed with `CUDA_ERROR_ILLEGAL_ADDRESS`, but only after earlier tests had left stale handles in recycled memory. They passed once every copy went through the stream (`cuMemcpyHtoDAsync` on it, then a stream sync).
- Read-backs through the legacy stream likewise returned partly written any-hit counters until the stream was synchronised first.
- Nova's GPU backend must keep every copy on its own stream, as the denoiser does (code optix_denoiser.cpp:159).

---

## 10. Files (`docs/research/m10/probes/`)

| File | Purpose |
|---|---|
| `oa_probe.cpp` | Host program: CUDA driver API from nvcuda.dll, OptiX context, builds, pipelines, ray sets, the 12 experiments. `oa_probe.exe [info\|tri\|trace\|anyhit\|launch\|curves\|points\|attrs\|motion\|ias\|local\|precision\|all]`, or a comma-separated list. All tests take about 11 s. |
| `oa_device.cu` | Device programs: raygens (hit object, payload, two occlusion forms, empty, inspect), closest hit, miss, any-hit (empty, opacity hash, counting, shadow link), custom IS (disc, oriented disc, sphere) |
| `oa_shared.h` | Launch parameters and SBT data shared by host and device |
| `oa_cuda_kernel.cu` | A plain CUDA kernel for the launch-overhead comparison |
| `oa_build.bat` | `oa_build.bat <outdir>`: nvcc (OptiX-IR, PTX) and MSVC; needs CUDA 12.x, the VS 2022 Build Tools and Nova's fetched OptiX headers (`OPTIX_INC` overrides) |
| `oa_out_all.txt` | Output of `oa_probe.exe all` (the numbers in this report) |
| `oa_out_launch_graph.txt` | The CUDA-graph capture failures (`OA_GRAPH=1` and `OA_GRAPH=fresh`) |

No NVIDIA SDK sample code is used: the probe includes the OptiX headers and is otherwise written for this report.

## 11. References

- NVIDIA OptiX 9.1 headers: `optix_types.h`, `optix_device.h`, `optix_host.h`, `optix_stack_size.h`, `internal/optix_device_impl_transformations.h` (github.com/NVIDIA/optix-dev, tag v9.1.0, fetched by `cmake/NovaOptiX.cmake`).
- NVIDIA OptiX Programming Guide (curves, spheres, motion blur, shader execution reordering) (lit, from memory).
- CUDA Driver API, "API synchronization behavior" and stream capture (lit).
- C. Wächter, N. Binder. A Fast and Robust Method for Avoiding Self-Intersection. Ray Tracing Gems, ch. 6, 2019.
- Embree 4 curve and point geometry types (via docs/research/m9/curves-hair.md 4.1).
- CMake 3.27 release notes and `CUDA_OPTIX_COMPILATION` (via `cmake --help-property`).
- Nova: docs/research/m9/curves-hair.md (Karma and Embree curve conventions), docs/design/M9.md (D1-D4 curves and points, D9 subsurface pass, D15 light linking), docs/design/M7.md (D7 refit), docs/design/M3.md (K10 opacity in filters).

---

## 12. Recommendations for Nova's IAccel OptiX backend

### 12.1 Data layout (design)

- **One GAS per `MeshId`**, as Embree's BLAS, compacted. Triangles take Nova's `Vec3f` points and `uint32` triangle indices directly (12-byte stride, one vertex buffer per `pointSteps` key). Curves take the points (stride 12), `curveRadius` as the width buffer and `curveSegments` as the index buffer; the Embree float4 packing is not needed.
- **One IAS for the scene**, rebuilt at every commit (0.07 ms for 65 instances, 2.2 ms for 1M):
  - `OptixInstance` per instance: `instanceId` = `MeshId`, transform = transposed `Xform34`;
  - `sbtOffset` = the hit group of the mesh's kind;
  - `visibilityMask` = shadow-link bits and enable state (mask 0 for disabled meshes);
  - `flags` = `DISABLE_ANYHIT` unless the mesh is filtered.
  - Instances of a mesh stay contiguous, and a device table `firstInstance[mesh]` turns `optixGetInstanceIndex()` into Nova's instance number.
- **Moving instances**: one `OptixSRTMotionTransform` per instance with Nova's keys repacked into `OptixSRTData` order (3.2), uploaded per commit.
- **Device tables for the filter**: per mesh the flags (filtered, fake caustics) and the material slots; per instance the 64-bit shadow masks; per material the uniform opacity and the opacity program.
- **Hit record** (SoA, as `HitBatch`): t, meshId (instance id), primId, uv (barycentrics, or curve u and 0), instance. 20-24 bytes per ray.

### 12.2 Programs and pipelines (design)

- Raygens:
  - **closest**: `optixTraverse` + hit-object getters; no closest-hit, no miss program (5.1);
  - **occlusion**: `optixTrace` with `TERMINATE_ON_FIRST_HIT | DISABLE_CLOSESTHIT` and a one-word payload cleared by a miss program;
  - **local**: subsurface walks on a GAS handle from a per-mesh table.
- Hit groups by geometry kind:
  - triangles;
  - round linear;
  - each cubic basis (built-in IS modules; ROCAPS where the u precision of 1e-4 is acceptable, which it is for shading);
  - built-in sphere;
  - custom sphere;
  - custom oriented disc;
  - each with the same any-hit program.
- **Any-hit**: Nova's `OpacityFilter::accept` in CUDA:
  - fake caustics;
  - shadow categories beyond the mask bits;
  - uniform opacity from the table;
  - the opacity program through the GPU VM (the other M10 report);
  - the same hash.
- **Pipelines**, chosen per scene and per query:
  1. static: `ALLOW_SINGLE_LEVEL_INSTANCING`, no motion (the fastest);
  2. motion: `ALLOW_ANY`, `usesMotionBlur`, graph depth 3, used only when the scene has motion;
  3. local: `ALLOW_SINGLE_GAS` (+ `usesMotionBlur` when needed), for the subsurface pass.

  Merging them into one would cost 7-23 % (5.2). The modules are shared; the pipelines differ only in their compile options. Separate pipeline objects are also what concurrent launches need (5.3).

### 12.3 Geometry kinds (design; 2.3-2.6)

- **Built in**:
  - triangles;
  - round linear, cubic Bezier, B-spline and Catmull-Rom curves;
  - spheres for points whose materials do not transmit;
  - **ray-facing ribbons as round tubes** of the same basis;
  - **ray-facing discs as built-in spheres**.
- **Custom intersection**:
  - `MeshShape::Sphere` (analytic; exit hits for glass; motion of centre and radius in the program);
  - point spheres with transmitting materials;
  - oriented discs.
- **Fallback for normal-oriented ribbons**: tessellate on the host into 4-8 quads per segment along the interpolated normals. The raygen maps triangle hits back to (segment, u) with a per-mesh table, so `resolveCurveHit` sees the same hit as from Embree. A custom ribbon intersector is the alternative if tessellation proves too coarse or too large.
- **Curves**: build with FAST_TRACE + compaction by default (3x faster tracing, 155 B per cubic segment). Offer FAST_BUILD (45 B per segment) when memory is short, and prefer linear curves where the look allows.

### 12.4 Motion (design; 3.2-3.5)

- Repack `SrtTransform` into `OptixSRTData` (do not share the buffers as with Embree).
- **Make Nova's interpolation backend-dependent**: `srtAtTime` / `interpolateSrt` (code motion.h:72-128) must use nlerp for the OptiX backend. Hit reconstruction, `objectToWorld` and `intersectLocal` then match the traversal. For XPU the CPU path keeps slerp; the difference is 0.0012° at 10° per key interval.
- Keep the rotation per key interval small: resample to more keys when an interval rotates more than about 10-20°. That reduces both the slerp/nlerp mismatch and the IAS build cost (5.7 ms for 64 instances at 20° per interval, 3.5).
- Deformation keys cost about 46 MB per key per million triangles: use as few as the motion needs (2 for linear motion). Keep `kMaxMotionSteps` = 16; OptiX allows 255.
- No IAS motion keys (no measured gain); build the IAS static over the motion transforms.

### 12.5 Filtering (design; 4)

- Build every GAS AH-capable. Toggle filtering per mesh with the instance flag `DISABLE_ANYHIT`: same speed as unfiltered geometry, and an IAS rebuild instead of a GAS rebuild.
- Shadow linking: up to 7 categories as visibility-mask bits (bit 7 on every instance for unrestricted rays), the rest through the any-hit filter. The same for fake caustics.
- Do not set `REQUIRE_SINGLE_ANYHIT_CALL`; Nova's decision is deterministic.

### 12.6 Builds and IPR (design; 7)

- Rebuild deformed meshes instead of refitting (1.4 ms per million triangles). Drop `ALLOW_UPDATE` (2.5x the compacted memory). Revisit only for meshes beyond about 20M triangles in interactive drags.
- Compact every GAS. Read the compacted sizes in one batch after a commit's builds rather than synchronising after each build.
- Build on the backend's stream. Upload with `cuMemcpyHtoDAsync` on that stream (9.4), from pinned staging buffers for large meshes.

### 12.7 Wavefront integration (design; 5.3)

- Launch every queue at its capacity with the count on the device: no read-back, 5-8 µs more than an exact launch.
- Keep the launch params constant during a pass. Counts and queue pointers live in device memory, so no per-launch params upload is needed (saves 5 µs per launch).
- Fewer, larger launches:
  - one trace launch per bounce for closest-hit and shadow rays together, with a per-ray kind (two raygen branches in one program);
  - the subsurface walk in its own launch.
  This halves the 13 µs launches per bounce.
- Do not plan on CUDA graphs for OptiX launches until 13.3 is answered.

### 12.8 Changes to `IAccel` (design)

- **Device batches**: `RayBatch` / `HitBatch` hold device pointers, a device-side count and a stream; `intersect` / `occluded` enqueue without synchronising. The CPU contract (host pointers, synchronous) stays for Embree.
- **Batched `intersectLocal`**: a batch with per-ray mesh (and instance) and object-space rays, or world rays plus per-ray transforms. The OptiX backend traces GAS handles in one launch.
- **The filter as data**: `IHitFilter` is a virtual host interface. The OptiX backend needs the tables of 12.1 plus the opacity programs, so the scene must hand the backend the filter description instead of an object.
- **Capabilities** the integrator queries: inside hits on curves (Embree yes, OptiX no), the SRT interpolation (slerp or nlerp), and the styles the backend emulates (flat → round, disc → sphere, oriented → tessellated).

### 12.9 Tests (design)

- Port Nova's accel unit tests to both backends with identical expectations, plus the semantics of 2.2 as regression tests:
  - barycentrics;
  - curve u per basis;
  - u at linear caps = 0/1;
  - no inside hits on OptiX curves;
  - exit hits on custom spheres;
  - nlerp SRT;
  - self-hit counts with `offsetRayOrigin` = 0 at 1e5.
- Reference renders of hair (round, flat, oriented), points (three styles) and a glass sphere on both backends, compared by FLIP/RMSE.

---

## 13. Open questions

1. **Normal-oriented ribbons**: tessellation density, memory and how close it comes to Embree's oriented curves; whether a custom intersector is needed. Will a future OptiX use `normalBuffers` (now reserved)?
2. **ROCAPS**: what the intersector does differently (no caps here, u to 1e-4, 9-26 % faster). Is it robust for thin, strongly bent hair?
3. **CUDA graphs**: can `optixLaunch` be captured at all in OptiX 9.1 (both variants failed here, 5.4)? Without graphs, launch overhead stays at about 13 µs per launch.
4. **Opacity cost in practice**: the any-hit numbers measure only the framework. The VM-based opacity program (registers, divergence inside traversal) needs measuring with the GPU VM. Could opacity micromaps replace texture cut-outs?
5. **Instance motion**: why IAS motion keys did not help (expected for large moves), and how the IAS build cost scales with many rotating instances (5.7 ms for 64).
6. **Curve re-hits**: the 0.05 % same-segment hits of rays leaving bent B-spline segments at translation 0 (8.3). Are they genuine re-hits or precision?
7. **Cluster acceleration structures** (supported on this GPU): worth it for very large or frequently rebuilt meshes (IPR, displacement)?
8. **Shader execution reordering** (supported): does `optixReorder` before shading help the wavefront? (Belongs to the CUDA integrator report.)
9. **Cold-build outlier**: the single 5.3 ms IAS build (7.3) against 0.07 ms medians. A one-time cost to hide at startup?
10. **Driver floor**: the oldest driver that accepts ABI 118. Should Nova build against an older OptiX ABI to support more installed drivers? The denoiser already depends on 9.1.
11. **XPU consistency**: the emulated styles (flat → round, disc → sphere) change t by up to r. Is that acceptable when CPU and GPU tiles of one image use different backends?
