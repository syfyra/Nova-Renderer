#pragma once

// Dicing rates of displaced meshes (docs/design/M4.md 3.6,
// docs/research/m4/displacement.md 5.3): every control edge gets
// ceil(L_px * q) segments, L_px its length in pixels as the dicing camera
// sees it and q the dicing quality (Karma's 0.5: 2-pixel micropolygons). The
// pixel length is a spherical measure (the distance to the eye, not the
// depth), so it stays finite behind the camera and off screen.

#include "core/math/mat.h"
#include "core/math/vec.h"

#include <algorithm>
#include <cmath>

namespace nova {

struct DicingView {
    bool valid = false;
    bool perspective = true;
    Vec3f eye{0.0f, 0.0f, 0.0f}; // world-space camera position
    Vec3f forward{0.0f, 0.0f, -1.0f};
    // Perspective: pixels per world unit at distance 1 (res_y / (2 tan(fov_y / 2)));
    // orthographic: pixels per world unit.
    float pixelsPerUnit = 1.0f;
    float nearDistance = 1e-3f;
    // The frustum for the off-screen test: world-to-clip and nothing else.
    Mat4 worldToClip = Mat4::identity();
};

// Pixel length of the world-space edge a-b.
NOVA_INLINE float edgePixels(const DicingView &v, Vec3f a, Vec3f b) {
    const float l = length(b - a);
    if (!v.perspective)
        return l * v.pixelsPerUnit;
    const float d = nova::fmax(length((a + b) * 0.5f - v.eye), v.nearDistance);
    return l * v.pixelsPerUnit / d;
}

// Segments of an edge of `pixels` pixels at dicing quality q (1 .. 1024).
NOVA_INLINE int dicingRate(float pixels, float quality) {
    const float r = std::ceil(pixels * quality);
    return !(r >= 1.0f) ? 1 : r > 1024.0f ? 1024 : int(r);
}

// True if the world-space box [lo, hi] may be inside the view frustum.
NOVA_INLINE bool boxInView(const DicingView &v, Vec3f lo, Vec3f hi) {
    // Clip-space test of the 8 corners against each plane (all outside one: culled).
    int outside[6] = {0, 0, 0, 0, 0, 0};
    for (int c = 0; c < 8; ++c) {
        const Vec3f p{(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z};
        const Mat4 &m = v.worldToClip;
        const double x = p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0];
        const double y = p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1];
        const double z = p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2];
        const double w = p.x * m.m[0][3] + p.y * m.m[1][3] + p.z * m.m[2][3] + m.m[3][3];
        outside[0] += x < -w;
        outside[1] += x > w;
        outside[2] += y < -w;
        outside[3] += y > w;
        outside[4] += z < -w;
        outside[5] += w <= 0.0;
    }
    for (int k = 0; k < 6; ++k)
        if (outside[k] == 8)
            return false;
    return true;
}

} // namespace nova
