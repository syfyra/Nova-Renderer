#pragma once

// The integrator's ray queries for shading programs (core/shading/query.h,
// docs/design/M4.md 3.5): every call traces the rays of all its lanes as one
// batch and reduces them per lane. Karma's measured behaviour
// (docs/research/m4/karma-nodes.md 4.3-4.6):
// - Occlusion: cosine-weighted directions in the cone (half angle) around the
//   normal, flipped towards the viewer with "front face"; the visibility is
//   the fraction of rays unoccluded within the maximum distance. Rays start
//   on the side of the surface they leave, so rays below a tilted normal's
//   surface see past it.
// - Distance: uniform directions in the cone; mode 0 the nearest hit, mode 1
//   the mean over the rays that hit (misses: the maximum distance);
//   optionally normalized by the maximum distance. The direction output is
//   the nearest hit's direction (mode 0) or the mean direction of the hits
//   (mode 1), zero without hits (not measured in Karma).
// - Curvature: uniform hemispheres around the surface's own normal, inwards
//   (convex) and outwards (concave), `samples` rays each; the mean of
//   max(0, 1 - L / maxdistance) over the rays.
// - Rounded edge: the normal averaged over the same object's surface within
//   the radius, weighted 1 - (d / r)^2 (fits Karma's measured profile to
//   about 1 degree); every sample traces rays along the normal and the two
//   tangents through a disk of the radius (multi-sample importance sampling
//   of the three axes, 3 x samples rays). Surfaces within the cusp angle of the normal, and edges
//   of the kind the mode excludes (1 concave only, 2 convex only), count with
//   the normal itself. The mask is 1 where a sample found a surface blended.
// Query rays are shadow rays for the hit filter, which runs opacity programs
// at the render's frame; fake-caustic objects block them as any other
// surface (only light samples pass them, M4 3.3).

#include "core/scene/scene.h"
#include "core/shading/query.h"

namespace nova {

class SceneShadingQueries final : public IShadingQueries {
public:
    // `globals`: the render's frame and fps (null: frame 1 at 24 fps); not
    // owned, must outlive the queries.
    SceneShadingQueries(const Scene &scene, uint32_t depth, const ShadeGlobals *globals = nullptr)
        : m_scene(scene), m_depth(depth), m_globals(globals) {}
    void run(QueryKind kind, int count, const QueryLane *lanes, const float *direction, const float *params,
             float *out) const override;

private:
    const Scene &m_scene;
    uint32_t m_depth;
    const ShadeGlobals *m_globals;
};

} // namespace nova
