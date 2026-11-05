# M7 research: IPR latency

PLAN.md sets the IPR goal: "IPR-Latenz < 100 ms beim Parameterwechsel". M7 lists "IPR-Performance (Delta-Updates, schneller Neustart)". This report measures where the time goes today, from a USD edit to the first new image.

## 0. Findings in short

1. **The first full pass is the latency.**
   - Sync and restart take 1-4 ms for every edit except deformation; the first full pass at 1 spp takes 180-200 ms.
   - Every edit lands at 185-215 ms (measured, section 2), twice the goal. A camera move is no faster than a material edit.
2. **Delta updates already work.** The edit itself costs almost nothing for all of these:
   - a light, a material value or a transform (cube or 1M triangles): sync 1.2-2.3 ms;
   - visibility, or adding and removing a prim: sync 1.2-1.3 ms;
   - one instance of 20 000: sync 2.1 ms, restart 11 ms (the top-level BVH).
   - None of them rebuilds geometry or recompiles a material (since M3/M4).
3. **Deformation rebuilds the BVH.** Moving one point of a 1M-triangle mesh costs 23 ms of sync (the points arrive, the mesh record is rebuilt) and 39 ms of restart (Embree rebuilds the mesh's BLAS at `RTC_BUILD_QUALITY_MEDIUM`). An Embree refit (`RTC_BUILD_QUALITY_REFIT`, same topology) is the standard remedy for interactive deformation.
4. **The remedy for the first pass is a progressive start.**
   - Render the first pass(es) after a restart at a reduced pixel rate: one pixel per 4x4 block, then per 2x2, then all. Each sparse pass fills its block in the display buffer.
   - Arnold's progressive refinement (negative AA samples) and Karma's IPR work this way, from their documentation; not measured here.
   - At one pixel in 16, the first image would take about 200 / 16 ≈ 13 ms of rendering on this scene. That leaves the budget for sync, restart and the host's redraw.
   - The pass then continues at full resolution. Samples already taken stay in the film, because the sparse passes are the first samples of their pixels.
5. **Resolution scales the problem.** The first pass is proportional to the pixel count.
   - At 2560x1440 (a viewport on a 4K monitor) it would take about 4 x 190 ms.
   - The progressive start keeps the first image near-constant in time; it does not grow with the number of pixels.
6. **Per-ray speed helps every pass.** The M6.6 profile of the beauty found:
   - `fmaxf`/`fminf` as CRT calls (~4 %);
   - `asinf` in spherical-rectangle sampling (~5 %);
   - helpers that are not inlined.

   These are general wins, worth taking in M7 (docs/benchmarks.md, M6).

## 1. Method

`tests/test_ipr_latency.cpp` (`nova_hydra_tests "[ipr-latency]"`, hidden from the default run) drives Nova as the Solaris viewport does:
- the UsdImaging scene indices of a USD stage and a render index with Nova's delegate;
- a render pass bound to a 1280x720 color buffer;
- `Execute` after every sync;
- a sample budget of 2^20, so the render never converges.

The scene:
- a 700x700-quad ground grid (980 000 triangles, Standard Surface);
- a 4-unit cube;
- a point instancer of 20 000 cubes;
- a rect light and a dome.

Each edit is applied six times, toggling between two values, with 150 ms of rendering before each, so it interrupts a busy render. The median run is reported, in three parts:
- **sync:** the USD edit, `ApplyPendingUpdates` and `HdEngine::Execute` (Hydra's sync of the dirty prims);
- **restart:** the render pass's `Execute`: stop the render thread, `Scene::commit` (BVH updates), reset the film and buffers, start the thread;
- **first pass:** until the delegate reports the first finished sample after the restart. It includes the copy into the color buffer.

The machine was also running two research agents' husk/hython probes, so the numbers carry a few percent of noise.

## 2. Measurements (2026-10-13, Release build, Ryzen 7 9800X3D)

| Edit | Total ms | Sync | Restart | First pass |
|---|---|---|---|---|
| initial (load, BVH, first pass) | 289 | 71.3 | 42.4 | 175.3 |
| camera move | 202 | 0.0 | 3.3 | 198.9 |
| light intensity | 191 | 1.3 | 2.3 | 187.8 |
| light transform | 185 | 1.4 | 1.9 | 181.6 |
| material value (Standard Surface base color) | 183 | 2.3 | 2.1 | 178.7 |
| object transform (cube) | 185 | 1.2 | 2.2 | 181.2 |
| object transform (1M triangles) | 195 | 1.5 | 2.4 | 191.2 |
| instancer: one position of 20 000 | 216 | 2.1 | 11.3 | 202.5 |
| deform points (1M triangles) | 256 | 22.8 | 38.6 | 194.9 |
| visibility (cube) | 197 | 1.3 | 3.0 | 192.3 |
| add / remove a prim | 192 | 1.2 | 2.5 | 188.2 |

## 3. Where the first pass goes

The first pass after a restart is an ordinary 1-spp pass over all 921 600 pixels: camera rays, the per-type limits (Karma's defaults, diffuse 1), NEE to one of two lights, and the color copy at the end.

At the 17-20 Mrays/s of the benchmarks (docs/benchmarks.md) that is about three rays per pixel, so 190 ms is the expected cost, not an anomaly. Nothing in the restart path (stop, commit, reset) is worth optimizing for these edits.

The host sees nothing until the whole pass ends. `HdNovaRenderDelegate::_RenderLoop` copies the film into the bound buffers after each complete pass (M6.6 kept that for the beauty).

## 4. Options for M7

| Option | Gain | Cost / risk |
|---|---|---|
| **Progressive start:** after a restart, pass k renders one pixel per 4x4, then per 2x2 block; each sparse pass writes block-filled pixels into the display buffers; full passes after that | first image in ~1/16 of a pass (~15 ms here), independent of resolution; PLAN.md's < 100 ms holds with room for the host | a pixel-subset mode in the renderer's tiles. The film keeps exact per-pixel sample counts, because sparse pixels get their first sample early; this touches adaptive sampling's per-pixel counts (docs/research/m7/adaptive-sampling.md) and the buffers' normalization. Final renders (husk) do not use it |
| Tile-progressive copies (copy each finished tile, not the whole pass) | the image fills in while the pass runs | copies race with the render. Tiles are thread-exclusive, but a tile copied mid-pass mixes samples, so it needs a per-tile sample count or a copy of finished tiles only |
| Embree refit for deformations in IPR (`RTC_BUILD_QUALITY_REFIT` while the topology is unchanged; a full rebuild for final renders or after many refits) | deform restart 39 ms → a few ms | BVH quality degrades with large deformations; rebuild after N refits or when the bounds grow a lot |
| Faster mesh sync for point edits (reuse the mesh record, update points only) | deform sync 23 ms → less | `MeshDesc` rebuild paths (subdivision, displacement) must stay correct |
| General per-ray speed (`fmaxf`/`fminf`, `asinf`, inlining) | ~5-10 % on every pass | none |

The progressive start is the one that meets the goal. The others shorten deformation edits and every pass.
