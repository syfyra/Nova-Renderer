#pragma once

// Karma's outlines, the per-object part (docs/design/M11-P9a.md): which
// line types an object draws and its thresholds (Karma's
// karma:object:outline_*). The render settings carry the defaults of
// objects without their own (Scene::setDefaultOutline); the integrator part
// is core/integrator/outline.h.

#include "core/math/common.h"

#include <cstdint>

namespace nova {

struct ObjectOutline {
    bool enable = true;  // outline_enable: camera hits on the object get lines
    bool contour = true; // outline_contour: the stencil leaves the object (or the scene)
    bool crease = true;  // outline_crease: the normal turns, or the surface rises in front of the hit's plane
    bool depth = true;   // outline_depth: a step away from the hit's plane
    // outline_radius: the stencil's cone, tan of its half angle (times the
    // render's outline_radius).
    float radius = 0.002f;
    float creaseThreshold = 0.1f; // outline_creasethreshold
    float depthThreshold = 0.5f;  // outline_depththreshold

    bool operator==(const ObjectOutline &) const = default;
};

// An object's own values (prim primvars); negative: the render settings'.
struct OutlineOverrides {
    int8_t enable = -1;
    int8_t contour = -1;
    int8_t crease = -1;
    int8_t depth = -1;
    float radius = -1.0f;
    float creaseThreshold = -1.0f;
    float depthThreshold = -1.0f;

    bool operator==(const OutlineOverrides &) const = default;
};

NOVA_INLINE ObjectOutline resolveObjectOutline(const ObjectOutline &d, const OutlineOverrides &o) {
    ObjectOutline s = d;
    if (o.enable >= 0)
        s.enable = o.enable != 0;
    if (o.contour >= 0)
        s.contour = o.contour != 0;
    if (o.crease >= 0)
        s.crease = o.crease != 0;
    if (o.depth >= 0)
        s.depth = o.depth != 0;
    if (o.radius >= 0.0f)
        s.radius = o.radius;
    if (o.creaseThreshold >= 0.0f)
        s.creaseThreshold = o.creaseThreshold;
    if (o.depthThreshold >= 0.0f)
        s.depthThreshold = o.depthThreshold;
    return s;
}

} // namespace nova
