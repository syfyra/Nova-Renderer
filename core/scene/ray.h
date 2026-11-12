#pragma once

#include "core/math/vec.h"

#include <cstdint>

namespace nova {

struct Ray {
    Vec3f org;
    Vec3f dir;
    float tMin = 0.0f;
    float tMax = kInf;
};

constexpr float kFloatUlp = 1.0f / 8388608.0f; // 2^-23

// Moves a surface point p off the surface along the geometric normal n (on
// the side the new ray leaves from) by the rounding of p, so that secondary
// rays can start at tMin = 0 without finding the surface again.
//
// p is a sum of float products (corners by barycentrics, a transform's rows),
// and the ray that starts there is taken into the shape's own space by such
// sums again. An instance's transform mixes the axes, so there the origin is
// off by ulps of p's LARGEST coordinate in every direction, whatever the
// normal: a level floor far away along x, whose mesh is rotated in its own
// space, has an origin that is wrong along its normal by ulps of x. So p
// moves kPointUlps float ulps of its largest coordinate along n. Measured
// (docs/bug-audit-2026-11.md, section 15): Embree and OptiX both find the
// surface again with 1 ulp and not with 2; the constant is four times that.
//
// Up to 0.1.4 this was Wächter and Binder's offset ("A Fast and Robust
// Method for Avoiding Self-Intersection", Ray Tracing Gems, ch. 6) with its
// constants: 256 ulps of every coordinate scaled by the normal's component
// along that axis, and 1/65 536 where a coordinate is below 1/32. Per axis,
// that is nothing along an axis the normal is perpendicular to (the floor
// above lost 27 % of its light 1 000 units from the origin); 256 ulps are
// 3 x 10^-5 of the distance from the origin (2 units at 100 000: next to a
// bump of a displaced surface the ray started inside its neighbour); and the
// absolute step is 15 % of the radius of a sphere of 0.0001 units. The
// intersectors' own error, which that offset also had to cover, is
// SurfaceHit::leave's (leaveSurface() below).
//
// A point at the origin itself has no rounding, and an axis-aligned polygon
// through it no intersector's error either: the ray would start exactly on
// the polygon and meet it at distance 0. Any step off it will do there, so
// the offset is never less than kLeastStep, which is below every scene's
// scale (and changes no other offset).
constexpr float kPointUlps = 8.0f;
constexpr float kLeastStep = 1e-30f;
NOVA_INLINE Vec3f offsetRayOrigin(Vec3f p, Vec3f n) {
    const float reach = nova::fmax(std::fabs(p.x), nova::fmax(std::fabs(p.y), std::fabs(p.z)));
    return p + n * nova::fmax(kPointUlps * kFloatUlp * reach, kLeastStep);
}

// The float error of a hit distance grows with the coordinates of the
// triangle hit (the intersectors work relative to its corners), not with the
// distance: a light lying on a large rotated plane can be a few ulps of the
// plane's coordinates behind it. Tolerances for that error are this many
// float ulps of the coordinates involved.
constexpr float kIntersectionUlps = 32.0f;

// The origin of a ray leaving a surface hit at p on the side of the
// geometric normal n. offsetRayOrigin() moves p by ulps of p itself, which
// covers the error of p. The intersectors, though, test a shape in its own
// space with its corners relative to the ray's origin, and err by ulps of
// those: a polygon far larger than the hit point's coordinates (a slope, a
// rotated wall or ground), or a mesh far from its own origin under a
// transform that brings it back, has an error far above ulps of p. The ray
// then starts inside that error and finds the polygon again: shadow acne
// and lost light (on the CPU 0.4 % of a rotated polygon's light at 60 units
// across, 3 % at 200, over 20 % at 2000). So p first moves kLeaveUlps float
// ulps of `leave` along n: the shape's reach as the intersector's error
// sees it (SurfaceHit::leave). Measured (docs/bug-audit-2026-11.md, section
// 12): Embree needs 1 ulp of it, OptiX 2; the constants are four times that.
#if defined(__CUDA_ARCH__)
constexpr float kLeaveUlps = 8.0f;
#else
constexpr float kLeaveUlps = 4.0f;
#endif
NOVA_INLINE Vec3f leaveSurface(Vec3f p, Vec3f n, float leave) {
    return offsetRayOrigin(p + n * (kLeaveUlps * kFloatUlp * leave), n);
}

// The least distance of a subsurface walk's first segment. The walk starts
// on the surface itself (an offset origin would sit a sizeable part of a
// small mean free path deep) and skips the entry surface by a minimum
// distance instead: of the order of the hit point's rounding (ulps of its
// largest coordinate, with room for a segment that leaves at a grazing
// angle), and of the intersector's error for the shape entered
// (leaveSurface()'s, which is the larger one on a mesh far from its own
// origin). Both scale with the scene: there is no least distance in units.
NOVA_INLINE float walkEntryDistance(Vec3f p, float leave) {
    const float reach = nova::fmax(std::fabs(p.x), nova::fmax(std::fabs(p.y), std::fabs(p.z)));
    return nova::fmax(64.0f * kFloatUlp * reach, kLeaveUlps * kFloatUlp * leave);
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
