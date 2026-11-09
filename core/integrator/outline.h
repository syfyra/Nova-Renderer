#pragma once

// Karma's outlines (toon lines, docs/design/M11-P9a.md), measured with Karma
// CPU 22.0.466: at every camera hit of an object with outlines a stencil of
// rays around the camera ray looks for edges; the share of its rays that
// find one, remapped and scaled, is the line's weight w; the beauty becomes
// (1 - w) beauty + w colour per sample, the outline AOV (w colour, w).
//
// The stencil (Karma's "Arctangent of the radius is the cone angle"): rays
// from the camera ray's origin at a tan offset R around its direction,
// R = object radius x render radius x lerp(1, 1 / (1 + t), depth blend)
// (t the hit distance; measured 1 / (1 + t) exactly). The default pattern
// is `stencils` rings at radii R k / stencils, ring k with samples x 2^(k-1)
// points equally spaced, each ring at a random rotation per sample (the
// line's profile is acos(d / R) / pi, measured); the random pattern draws
// `samples` points with a radius uniform in [0, pi/2 R) (fitted, max error
// 0.004) and a uniform angle.
//
// A stencil ray flags (each type on when the hit's object has it on):
//   contour: it misses, or hits another object (Karma: another object id);
//   depth:   its hit lies off the camera hit's tangent plane by more than
//            rho (1 + depth threshold x depth threshold scale), rho = R t
//            the stencil's size at the hit (measured on steps: rho, 1.25,
//            1.5 and 2 rho at thresholds 0, 0.25, 0.5 and 1);
//   crease:  1 - N.Ns > crease threshold (the normals turn), or its hit rises
//            in front of the camera hit's plane, dot(N, Ps - P) > threshold
//            |Ps - P| (measured on concave folds and steps with equal
//            normals). Karma flags fewer points of a convex fold near its
//            edge than this (docs/design/M11-P9a.md 4.4).
// Lines are drawn on camera rays only: Karma 22.0.466 drew none in
// reflections or refractions (mirror, rough mirror, glass; measured).

#include "core/math/rng.h"
#include "core/math/sobol.h"
#include "core/math/vec.h"
#include "core/scene/outline.h"

#include <cstdint>

namespace nova {

// The render's outline settings (Karma's karma:global:outline_*).
struct OutlineSettings {
    bool enable = false;  // outline_enable
    bool primary = true;  // outline_primary: lines on camera rays
    // Karma computes lines only when an outline AOV is among the render's
    // outputs (measured: the beauty has none without one): the host sets
    // this from the film's planes (outlineActive).
    bool requested = false;
    bool randomPattern = false; // outline_mode 1
    float radius = 1.0f;        // outline_radius (times the objects')
    uint32_t samples = 4;       // outline_primary_samples
    uint32_t stencils = 1;      // outline_primary_stencils (the default pattern)
    float intensity = 1.0f;     // outline_intensity
    float fitMin = 0.0f;        // outline_fitmin, outline_fitmax: the share remapped
    float fitMax = 1.0f;
    bool apply = true;          // outline_apply: composite into the beauty
    float depthBlend = 0.0f;    // outline_depthblend
    float color[3] = {1.0f, 0.0f, 1.0f}; // outline_color
    float depthScale = 1.0f;    // outline_depththreshold (scales the objects')

    bool operator==(const OutlineSettings &) const = default;
};

// Line types (the per-type AOVs), as bits.
constexpr uint8_t kOutlineContour = 1u;
constexpr uint8_t kOutlineCrease = 2u;
constexpr uint8_t kOutlineDepth = 4u;
constexpr uint8_t kOutlineValid = 0x80u; // the sample's camera hit has outlines

NOVA_INLINE bool outlineOn(const OutlineSettings &s) { return s.enable && s.primary && s.requested; }

NOVA_INLINE uint32_t outlineSamples(const OutlineSettings &s) {
    return s.samples < 1u ? 1u : (s.samples > 64u ? 64u : s.samples);
}
NOVA_INLINE uint32_t outlineStencils(const OutlineSettings &s) {
    return s.stencils < 1u ? 1u : (s.stencils > 6u ? 6u : s.stencils);
}
// The stencil's rays per camera hit.
NOVA_INLINE uint32_t outlinePointCount(const OutlineSettings &s) {
    const uint32_t n = outlineSamples(s);
    return s.randomPattern ? n : n * ((1u << outlineStencils(s)) - 1u);
}

// The stencil's mean point radius in units of R: Karma's debug thickness is
// R times it (measured: 0.8095 R with three rings, R with the random pattern).
NOVA_INLINE float outlineMeanRadius(const OutlineSettings &s) {
    if (s.randomPattern)
        return 1.0f;
    const uint32_t rings = outlineStencils(s);
    float sum = 0.0f, count = 0.0f;
    for (uint32_t k = 1; k <= rings; ++k) {
        const float n = float(1u << (k - 1));
        sum += n * float(k) / float(rings);
        count += n;
    }
    return sum / count;
}

// The tan radius of the stencil at a hit t away from the camera ray's origin.
NOVA_INLINE float outlineRadius(const OutlineSettings &s, const ObjectOutline &o, float t) {
    const float blend = s.depthBlend < 0.0f ? 0.0f : (s.depthBlend > 1.0f ? 1.0f : s.depthBlend);
    const float scale = 1.0f + blend * (1.0f / (1.0f + (t > 0.0f ? t : 0.0f)) - 1.0f);
    return o.radius * s.radius * scale;
}

// Point k of the stencil as an offset in units of the radius (x, y in the
// plane across the ray). The rotations and random points come from the
// pixel's seed and the sample's number.
NOVA_INLINE Vec2f outlineOffset(const OutlineSettings &s, uint32_t k, uint32_t pixelSeed, uint32_t sampleIndex) {
    float r, phi;
    if (s.randomPattern) {
        Rng rng = Rng::make(pixelSeed, sampleIndex, stream::outline(k));
        r = 0.5f * kPi * rng.next();
        phi = kTwoPi * rng.next();
    } else {
        const uint32_t n = outlineSamples(s), rings = outlineStencils(s);
        uint32_t ring = 0, first = 0, count = n;
        while (k >= first + count && ring + 1 < rings) {
            first += count;
            count <<= 1;
            ++ring;
        }
        Rng rng = Rng::make(pixelSeed, sampleIndex, stream::outline(ring));
        r = float(ring + 1) / float(rings);
        phi = kTwoPi * (float(k - first) + rng.next()) / float(count);
    }
    return {r * std::cos(phi), r * std::sin(phi)};
}

// The direction of a stencil ray: `offset` (outlineOffset) times the tan
// radius R across the unit direction `dir`.
NOVA_INLINE Vec3f outlineDirection(Vec3f dir, Vec2f offset, float R) {
    // A frame across dir (Frame::fromNormal's construction).
    const float sign = std::copysign(1.0f, dir.z);
    const float a = -1.0f / (sign + dir.z);
    const float bb = dir.x * dir.y * a;
    const Vec3f t{1.0f + sign * dir.x * dir.x * a, sign * bb, -sign * dir.x};
    const Vec3f b{bb, sign + dir.y * dir.y * a, -dir.y};
    return normalize(dir + (t * offset.x + b * offset.y) * R);
}

// The camera hit a stencil compares with.
struct OutlineMain {
    Vec3f P{};
    Vec3f N{}; // shading normal, facing the camera ray
    float rho = 0.0f; // the stencil's size at the hit (R t)
    uint32_t mesh = 0;
    uint32_t instance = 0;
    ObjectOutline object;
};

// The line types a stencil ray flags (the main's object's types only).
// `hit`: the stencil ray hit something, at P with the shading normal N
// facing the stencil ray, on (mesh, instance).
NOVA_INLINE uint8_t outlinePointFlags(const OutlineMain &m, const OutlineSettings &s, bool hit, uint32_t mesh,
                                      uint32_t instance, Vec3f P, Vec3f N) {
    const ObjectOutline &o = m.object;
    if (!hit)
        return o.contour ? kOutlineContour : uint8_t(0);
    uint8_t flags = 0;
    if (o.contour && (mesh != m.mesh || instance != m.instance))
        flags |= kOutlineContour;
    const Vec3f d = P - m.P;
    const float h = dot(m.N, d); // the height over the camera hit's plane
    if (o.depth && std::fabs(h) > m.rho * (1.0f + o.depthThreshold * s.depthScale))
        flags |= kOutlineDepth;
    if (o.crease) {
        const float len = length(d);
        if (1.0f - dot(m.N, N) > o.creaseThreshold || (len > 0.0f && h > o.creaseThreshold * len))
            flags |= kOutlineCrease;
    }
    return flags;
}

// The line's weight from the flagged share of the stencil: remapped from
// [fitMin, fitMax] to [0, 1], times the intensity, at most 1.
NOVA_INLINE float outlineWeight(const OutlineSettings &s, uint32_t flagged, uint32_t total) {
    if (flagged == 0 || total == 0)
        return 0.0f;
    const float share = float(flagged) / float(total);
    float w = s.fitMax > s.fitMin ? (share - s.fitMin) / (s.fitMax - s.fitMin) : (share >= s.fitMin ? 1.0f : 0.0f);
    w = w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
    w *= s.intensity > 0.0f ? s.intensity : 0.0f;
    return w > 1.0f ? 1.0f : w;
}

// The debug AOV's object id (Karma's export_outline_id: an id per object
// in [0, 1)).
NOVA_INLINE float outlineObjectId(uint32_t mesh, uint32_t instance) {
    return float(pcgHash(mesh * 0x9E3779B9u ^ pcgHash(instance + 1u)) >> 8) * (1.0f / 16777216.0f);
}

// What one sample adds to the outline planes (the film's per-sample
// utility planes, Film::outline*): w, the line types found, and for the
// debug planes the object's id and thickness.
struct OutlineSample {
    float weight = 0.0f;
    uint8_t types = 0; // kOutline* bits, kOutlineValid with a camera hit that has outlines
    float id = 0.0f;
    float thickness = 0.0f;
};

// The outline planes in UtilityKind's order from Outline on.
enum OutlinePlane : int {
    kOutlinePlaneLine,
    kOutlinePlaneContour,
    kOutlinePlaneCrease,
    kOutlinePlaneDepth,
    kOutlinePlaneId,
    kOutlinePlaneThickness,
    kOutlinePlaneColor,
    kOutlinePlaneCount
};

// One sample's value of an outline plane (`width` floats into out).
NOVA_INLINE void outlinePlaneValue(int plane, const OutlineSample &o, const float color[3], float *out) {
    const bool valid = (o.types & kOutlineValid) != 0;
    switch (plane) {
    case kOutlinePlaneLine:
        out[0] = color[0] * o.weight;
        out[1] = color[1] * o.weight;
        out[2] = color[2] * o.weight;
        out[3] = o.weight;
        break;
    case kOutlinePlaneContour:
    case kOutlinePlaneCrease:
    case kOutlinePlaneDepth: {
        const uint8_t bit = plane == kOutlinePlaneContour ? kOutlineContour
                            : plane == kOutlinePlaneCrease ? kOutlineCrease
                                                           : kOutlineDepth;
        const bool on = (o.types & bit) != 0;
        out[0] = on ? color[0] : 0.0f;
        out[1] = on ? color[1] : 0.0f;
        out[2] = on ? color[2] : 0.0f;
        out[3] = on ? o.weight : 0.0f;
        break;
    }
    case kOutlinePlaneId:
        out[0] = valid ? o.id : 0.0f;
        break;
    case kOutlinePlaneThickness:
        out[0] = valid ? o.thickness : 0.0f;
        break;
    default: // colour
        out[0] = valid ? color[0] : 0.0f;
        out[1] = valid ? color[1] : 0.0f;
        out[2] = valid ? color[2] : 0.0f;
        break;
    }
}

// The beauty sample with the line composited.
NOVA_INLINE Vec3f outlineComposite(const OutlineSettings &s, const OutlineSample &o, Vec3f radiance) {
    if (!s.apply || !(o.weight > 0.0f))
        return radiance;
    return radiance * (1.0f - o.weight) + Vec3f{s.color[0], s.color[1], s.color[2]} * o.weight;
}

} // namespace nova
