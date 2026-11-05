#pragma once

#include "core/math/vec.h"

#include <bit>
#include <cstdint>

namespace nova {

struct Ray {
    Vec3f org;
    Vec3f dir;
    float tMin = 0.0f;
    float tMax = kInf;
};

NOVA_INLINE int32_t floatToBits(float f) {
#if defined(__CUDA_ARCH__)
    return __float_as_int(f);
#else
    return std::bit_cast<int32_t>(f);
#endif
}

NOVA_INLINE float bitsToFloat(int32_t i) {
#if defined(__CUDA_ARCH__)
    return __int_as_float(i);
#else
    return std::bit_cast<float>(i);
#endif
}

// Moves a surface point off the surface along the geometric normal `n` (on the
// side the new ray leaves from) by a few ULPs, so secondary rays can start at
// tMin = 0 without self-intersection. Wächter & Binder, "A Fast and Robust
// Method for Avoiding Self-Intersection", Ray Tracing Gems (2019), ch. 6.
NOVA_INLINE Vec3f offsetRayOrigin(Vec3f p, Vec3f n) {
    constexpr float kOrigin = 1.0f / 32.0f;
    constexpr float kFloatScale = 1.0f / 65536.0f;
    constexpr float kIntScale = 256.0f;
    const int32_t ox = int32_t(kIntScale * n.x);
    const int32_t oy = int32_t(kIntScale * n.y);
    const int32_t oz = int32_t(kIntScale * n.z);
    const Vec3f pi{bitsToFloat(floatToBits(p.x) + (p.x < 0.0f ? -ox : ox)),
                   bitsToFloat(floatToBits(p.y) + (p.y < 0.0f ? -oy : oy)),
                   bitsToFloat(floatToBits(p.z) + (p.z < 0.0f ? -oz : oz))};
    return {std::fabs(p.x) < kOrigin ? p.x + kFloatScale * n.x : pi.x,
            std::fabs(p.y) < kOrigin ? p.y + kFloatScale * n.y : pi.y,
            std::fabs(p.z) < kOrigin ? p.z + kFloatScale * n.z : pi.z};
}

// The origin of a ray leaving a round curve (a tube of radius r, outward
// normal N at P, unit tangent T) in direction d (docs/design/M9.md D4):
// Embree's round curves report hits from inside the tube, so a ray into the
// fibre starts where it leaves it again, at the chord's end. Both sides
// add 1e-3 r along the normal there (at least minEps: SurfaceHit's
// curveExitEps, the intersection error of long round linear segments), then
// the usual ULP offset.
NOVA_INLINE Vec3f tubeRayOrigin(Vec3f P, Vec3f N, Vec3f T, float r, Vec3f d, float minEps = 0.0f) {
    const float eps = nova::fmax(1e-3f * r, minEps);
    if (dot(d, N) >= 0.0f)
        return offsetRayOrigin(P + N * eps, N);
    if (std::fabs(dot(N, T)) > 1e-3f) { // a linear curve's spherical end cap
        const float chord = -2.0f * r * dot(d, N);
        const Vec3f exitN = normalize(N + d * (chord / r));
        return offsetRayOrigin(P + d * chord + exitN * eps, exitN);
    }
    const Vec3f across = d - T * dot(d, T);
    const float aa = dot(across, across);
    if (!(aa > 0.0f) || !(r > 0.0f))
        return offsetRayOrigin(P, -N); // along the axis: leave as from a sheet
    const float chord = 2.0f * r * -dot(across, N) / aa;
    const Vec3f exitN = normalize(N + across * (chord / r));
    return offsetRayOrigin(P + d * chord + exitN * eps, exitN);
}

} // namespace nova
