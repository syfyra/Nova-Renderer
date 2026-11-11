#pragma once

// The OptiX programs' launch parameters (docs/design/M10.md D5, D8): a SoA
// ray queue in (as RayBatch), a hit record or an occlusion flag per ray out
// (as HitBatch). Shared by the host and core/gpu/programs.cu.

#include "core/math/vec.h"

#include <optix_types.h>

#include <cstdint>

namespace nova::gpu {

// A mesh's custom primitives for the intersection programs (D5): analytic
// spheres and points (centers and radii: one radius for all, one per
// primitive or one per primitive and step) and oriented discs (their
// normals, per primitive or per primitive and step), in object space; with
// steps > 1 the keys of a deforming mesh, step-major, count per step.
// Normal-oriented ribbons are triangles, ribbonQuads per segment (ribbonHit
// maps a hit back to the segment and its parameter).
struct ShapeData {
    const Vec3f *centers = nullptr;
    const float *radii = nullptr;
    const Vec3f *normals = nullptr;
    uint32_t radiusCount = 0;
    uint32_t normalCount = 0;
    uint32_t count = 0;
    uint32_t steps = 1;
    uint32_t ribbonQuads = 0;
};

// A mesh alone (subsurface walks, Scene::intersectLocal): its GAS and its
// hit group without the filter.
struct MeshGas {
    OptixTraversableHandle handle = 0;
    uint32_t sbtOffset = 0;
};

struct TraceParams {
    OptixTraversableHandle handle = 0;
    // Rays: origins, unit directions, tMax (also the hit distance written
    // back), normalized shutter times (null: 0), and the count on the device
    // (null: the launch width).
    const Vec3f *org = nullptr;
    const Vec3f *dir = nullptr;
    float *tMax = nullptr;
    const float *time = nullptr;
    const uint32_t *count = nullptr;
    // Closest hits: the mesh (kInvalidId: a miss), primitive, barycentrics
    // or curve u, and instance number within the mesh.
    uint32_t *mesh = nullptr;
    uint32_t *prim = nullptr;
    Vec2f *uv = nullptr;
    uint32_t *instance = nullptr;
    // Occlusion: 1 if blocked. Shadow linking (M9 D15, M10 D7): per ray its
    // light's category (null or 0xFFFF: every object blocks); categories 0-6
    // are the instances' visibility-mask bits 0-6, bit 7 marks every
    // instance (OptixScene::build).
    uint8_t *occluded = nullptr;
    const uint16_t *category = nullptr;
    // Per mesh: its first instance in the IAS (Nova's instance number is the
    // IAS instance index minus this), and its custom primitives' data.
    const uint32_t *firstInstance = nullptr;
    const ShapeData *shapes = nullptr;
    // The any-hit filter of filtered meshes (D7, core/gpu/device_shading.h's
    // acceptHit): the scene (a GpuFilter in device memory; null: accept
    // every hit), per ray its opacity seed and class, the scattering events
    // before the rays, and whether they are shadow rays.
    const void *filter = nullptr;
    const float *seed = nullptr;
    const uint8_t *rayType = nullptr;
    uint32_t depth = 0;
    uint32_t occlusion = 0;
    // Occlusion rays: fake-caustic objects let them pass (light samples'
    // shadow rays, RayBatch::skipFakeCaustics); 0 for ambient occlusion,
    // which every surface blocks.
    uint32_t skipFakeCaustics = 1;
    // Occlusion rays: Karma's bgoccludeshadow probe (core/integrator/holdout.h):
    // the rays from background points (rayType's kRayFromBackground) are
    // traced again against background holdouts alone; a hit clears
    // `occluded` and sets kRayBackgroundLit in `backgroundLit` (the same
    // rays' rayType, writable). Every mesh is filtered while it may run.
    uint32_t probeBackground = 0;
    uint8_t *backgroundLit = nullptr;
    // Closest hits as shadow rays (the hit filter's class; where scattered
    // rays find their lights, Scene::shadowVisibilityDiffers), the distance
    // into hitT instead of tMax.
    uint32_t shadowClass = 0;
    float *hitT = nullptr;
    // Subsurface walks (Raygen::Walk): per mesh its GAS, and the walks (a
    // GpuWalks in device memory; null: another raygen).
    const MeshGas *gas = nullptr;
    const void *walks = nullptr;
};

// Raygen programs, in the order of their SBT records.
enum class Raygen : uint32_t { Closest, Occluded, Walk, kCount };

} // namespace nova::gpu
