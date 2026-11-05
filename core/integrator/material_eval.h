#pragma once

// Material evaluation for the shade stage (docs/design/M3.md section 4, pass
// C): the surface inputs of a chunk of hits that share a material. The
// shading program runs once for the whole chunk (8, 16 or 32 lanes, or one
// lane for a single hit); padding lanes repeat the last hit.

#include "core/integrator/material_lane.h"
#include "core/scene/scene.h"
#include "core/shading/query.h"

#include <vector>

namespace nova {

constexpr int kShadeChunk = 32;

struct MaterialEvalScratch {
    std::vector<float> rows;
    SurfaceInputs inputs[kShadeChunk];
};

// Per-hit texture data (docs/design/M3.md K7, K8): the two footprint vectors
// of the hit's ray cone, already scaled by nova:textureFilterWidth (null:
// none, the finest level), and the stochastic filtering sample (null:
// deterministic).
struct HitTextureData {
    const Vec3f *footprintX = nullptr;
    const Vec3f *footprintY = nullptr;
    const float *rnd = nullptr;
};

// Per-hit ray state (docs/design/M4.md 3.1): the class of each hit's ray
// (RayClass; null: camera rays) and the scattering events before the hits;
// the ray queries of shading programs (M4.4; null: their defaults) and each
// hit's random numbers for them (null: 0.5).
struct HitRayData {
    const uint8_t *type = nullptr;
    uint32_t depth = 0;
    const IShadingQueries *queries = nullptr;
    const Vec3f *queryRnd = nullptr;
};

// Writes a pointer to the surface inputs of each hit into `out` (into the
// scratch, or the material's own inputs if they do not vary per hit). `dirs`
// are the rays' directions (towards the surfaces), `times` their normalized
// shutter times.
void evaluateSurfaceInputs(const Scene &scene, MaterialId material, const SurfaceHit *hits, const Vec3f *dirs,
                           const float *times, int count, const SurfaceInputs **out, MaterialEvalScratch &scratch,
                           const ShadeGlobals &globals = {}, const HitTextureData &texture = {},
                           const HitRayData &ray = {});

} // namespace nova
