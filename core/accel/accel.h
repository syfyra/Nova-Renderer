#pragma once

// Acceleration-structure interface. Queries are batched (one call per wave of
// rays), so the virtual dispatch never sits in the per-ray hot path; backends
// loop over the batch internally. Embree implements it on the CPU, an OptiX
// backend will implement the same interface on the GPU.

#include "core/math/vec.h"
#include "core/math/xform.h"
#include "core/scene/mesh.h"
#include "core/scene/motion.h"

#include <cstdint>
#include <memory>

namespace nova {

struct ShadeGlobals; // core/integrator/material_lane.h

// Non-owning SoA view of rays. Ray origins are already offset from surfaces,
// so every ray starts at t = 0.
struct RayBatch {
    const Vec3f *org = nullptr;
    const Vec3f *dir = nullptr;
    const float *tMax = nullptr;
    uint32_t count = 0;
    const float *time = nullptr; // normalized shutter time per ray in [0, 1]; nullptr = 0
    const float *filterSeed = nullptr; // per ray, for the hit filter (stochastic opacity); nullptr = none
    // Ray state for the hit filter (opacity programs read it): the class of
    // each ray (RayClass, core/bsdf/closure.h; nullptr = camera; occlusion
    // queries are shadow rays) and the scattering events before the batch.
    const uint8_t *rayType = nullptr;
    uint32_t depth = 0;
    // Shadow linking (M9 D15): per occlusion ray, the category of the objects
    // that may block it (0xFFFF: all; nullptr: none restricted).
    const uint16_t *shadowCategory = nullptr;
    // The render's frame and fps for opacity programs (their Frame and Time
    // globals, as the shade stage's); nullptr: frame 1 at 24 fps.
    const ShadeGlobals *globals = nullptr;
    // Occlusion queries: whether fake-caustic objects let the rays pass.
    // Light samples' shadow rays do (their transmittance follows, M4 3.3);
    // ambient occlusion clears it, every surface blocks it.
    bool skipFakeCaustics = true;
};

// Closest-hit results, one entry per ray. meshId == kInvalidId marks a miss.
// Barycentrics follow Embree: P = (1 - u - v) * p0 + u * p1 + v * p2.
struct HitBatch {
    float *t = nullptr;
    uint32_t *meshId = nullptr;
    uint32_t *primId = nullptr;
    Vec2f *uv = nullptr;
    uint32_t *instance = nullptr; // optional: index into the mesh's instance list
};

// A candidate hit offered to the hit filter. Barycentrics as in HitBatch.
struct HitCandidate {
    MeshId mesh;
    uint32_t prim;
    uint32_t instance;
    Vec2f uv;
    float t;
    uint32_t ray;  // index into the RayBatch
    bool occlusion; // from occluded() (shadow rays), else from intersect()
};

// Decides whether a candidate hit counts (cut-out and stochastic opacity,
// docs/design/M3.md K10). The accelerator calls it during traversal, only for
// meshes with filtering enabled, from any thread and possibly several times
// for the same hit: the answer must depend only on the candidate and the
// ray's data. Rejected hits are ignored as if the surface were not there.
// The same structure as an OptiX any-hit program.
class IHitFilter {
public:
    virtual ~IHitFilter() = default;
    virtual bool accept(const HitCandidate &hit, const RayBatch &rays) const = 0;
};

class IAccel {
public:
    virtual ~IAccel() = default;

    // Geometry edits; they take effect at the next commit().
    // (Re)builds the object-space triangles of a mesh, with one vertex set
    // per deformation step; its instances are kept.
    virtual void setMesh(MeshId id, const MeshDesc &desc) = 0;
    // Object-to-world transforms of the mesh's instances. The array stays
    // owned by the caller and must stay valid and unchanged until the next
    // setInstances*() or removeMesh() of this id (the backend shares it).
    // count == 0 renders nothing.
    virtual void setInstances(MeshId id, const Xform34 *xforms, uint32_t count) = 0;
    // Moving instances: `steps` uniform keys per instance over the shutter,
    // step-major (keys[s * count + i]); same ownership rules.
    virtual void setInstanceMotion(MeshId id, const SrtTransform *keys, uint32_t count, uint32_t steps) = 0;
    virtual void setEnabled(MeshId id, bool enabled) = 0;
    virtual void removeMesh(MeshId id) = 0;
    // Hits on this mesh go through the hit filter (kept across setMesh()).
    virtual void setMeshFiltered(MeshId id, bool filtered) = 0;
    // The filter for meshes marked filtered; not owned, must outlive the
    // queries. nullptr: every hit counts.
    virtual void setHitFilter(const IHitFilter *filter) = 0;
    virtual void commit() = 0;
    // Interactive sessions refit the BVH of a mesh whose new points keep its
    // topology instead of rebuilding it (docs/design/M7.md D7).
    virtual void setRefit(bool refit) { (void)refit; }

    // Thread-safe after commit(); may be called concurrently.
    virtual void intersect(const RayBatch &rays, HitBatch &hits) const = 0;
    virtual void occluded(const RayBatch &rays, uint8_t *occluded) const = 0;
    // The closest hit on one mesh alone, in its object space (subsurface
    // walks, docs/design/M9.md): the ray's parameter is kept, so with a
    // direction mapped from a unit world direction t is the world distance.
    // Hits closer than tMin are skipped. Hit filters do not apply. False on a
    // miss or for a mesh without geometry.
    virtual bool intersectLocal(MeshId id, Vec3f org, Vec3f dir, float tMin, float tMax, float time, float &t,
                                uint32_t &prim, Vec2f &uv) const = 0;

    // World-space bounds of the enabled meshes after commit(); lower > upper
    // (or non-finite) if there are none.
    virtual void bounds(Vec3f &lower, Vec3f &upper) const = 0;
};

// `threads` limits the BVH build threads (0 = all hardware threads).
std::unique_ptr<IAccel> createEmbreeAccel(int threads = 0);

} // namespace nova
