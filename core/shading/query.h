#pragma once

// Ray queries issued by shading programs (docs/design/M4.md 3.5, M4.4):
// ambient occlusion, distance, curvature and rounded edges, which trace rays
// from the shading point. The VM's Query op hands one call per 32-lane chunk
// to the integrator's implementation (core/integrator/shading_queries.h),
// which traces every lane's rays as one batch. On the GPU (M10) the op splits
// the kernel the same way. Without an implementation (displacement, opacity
// programs, unit tests) the queries return their defaults.

#include "core/math/vec.h"

#include <cstdint>

namespace nova {

enum class QueryKind : uint8_t {
    Occlusion,   // params (cone half angle deg, max distance, samples, front face): visibility (1)
    Distance,    // params (cone half angle deg, max distance, samples, mode | normalize << 1 | front face << 2):
                 // distance, direction (4)
    Curvature,   // params (max distance, samples): convex, concave (2)
    RoundedEdge, // params (radius, samples, mode, cusp angle deg): rounded normal, mask (4)
};

// What the queries need of a lane's hit (world space).
struct QueryLane {
    Vec3f P;  // the hit
    Vec3f N;  // shading normal (authored orientation)
    Vec3f Ng; // geometric normal (authored orientation)
    Vec3f I;  // the ray's direction, towards the surface
    Vec3f rnd;  // this sample's random numbers (shift the query's sample pattern)
    float time; // normalized shutter time
    uint32_t mesh, instance;
    // Round curves (M9 D4): query rays leave from the tube (tubeRayOrigin).
    bool tube = false;
    Vec3f tangent{0.0f, 0.0f, 0.0f};
    float curveRadius = 0.0f, curveExitEps = 0.0f;
};

// The output width of a query.
NOVA_INLINE int queryWidth(QueryKind kind) {
    switch (kind) {
    case QueryKind::Occlusion:
        return 1;
    case QueryKind::Curvature:
        return 2;
    default:
        return 4;
    }
}

// Default results without an implementation.
NOVA_INLINE void queryDefault(QueryKind kind, const float *direction, float *out) {
    switch (kind) {
    case QueryKind::Occlusion:
        out[0] = 1.0f;
        return;
    case QueryKind::Distance:
        out[0] = 1.0f;
        out[1] = out[2] = out[3] = 0.0f;
        return;
    case QueryKind::Curvature:
        out[0] = out[1] = 0.0f;
        return;
    default:
        out[0] = direction[0];
        out[1] = direction[1];
        out[2] = direction[2];
        out[3] = 0.0f;
        return;
    }
}

class IShadingQueries {
public:
    virtual ~IShadingQueries() = default;
    // For `count` lanes: `direction` 3 floats per lane (the query's normal;
    // zero: the lane's shading normal), `params` 4 per lane; writes
    // queryWidth(kind) floats per lane to `out` (4 apart).
    virtual void run(QueryKind kind, int count, const QueryLane *lanes, const float *direction, const float *params,
                     float *out) const = 0;
};

} // namespace nova
