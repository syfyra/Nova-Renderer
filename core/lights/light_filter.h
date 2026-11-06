#pragma once

// Light filters (docs/design/M11-P4.md): per light an ordered list of filter
// records, each a colour factor for the point being lit. The factors of a
// light's filters multiply. LightFilterKind leaves room for more kinds
// (Karma's gobo, barn doors, gel, attenuation); only the blocker exists so far.
//
// Karma's blocker (kma_lfilter_blocker), as measured with Karma CPU 22.0.466:
// - It tests only the point being lit: a light sample's shading point, the
//   origin of a BSDF ray that reaches the light (the mirror point for a
//   mirror) and, for camera-visible lights, the camera ray's origin. Neither
//   the point on the light nor the segment between them.
// - The shape lives in the filter's space: q = spaceFromWorld(P). Box: width,
//   height and depth along local X, Y, Z; capsule: a segment of length height
//   along local Y with the radius around it; sphere: the radius.
// - The signed distance to the shape is measured in units of the shape's size
//   (sphere and capsule: the radius; box: its smallest half extent), and the
//   box's rounded corner is min(roundedcorner, 1) of that size.
// - The light passes with u = smoothstep(-f, f, s) for that distance s,
//   f = min(falloff, 1) (0: a hard edge); invert takes 1 - u. The factor is
//   k + (1 - k) u, k = intensity 2^exposure tint: k inside, 1 outside.

#include "core/math/spectrum.h"
#include "core/math/vec.h"

#include <cstdint>

namespace nova {

enum class LightFilterKind : uint32_t { Blocker };
enum class BlockerShape : uint32_t { Sphere, Capsule, Box };

// Where a light's filters are evaluated: the point being lit and its
// light-linking categories (receiver set, docs/design/M9.md D15), which
// filterLink collections select by. The camera has none.
struct LightFilterSite {
    Vec3f p{0.0f, 0.0f, 0.0f};
    uint64_t categories = 0;
};

// One filter of one light, as the integrator evaluates it.
struct LightFilterData {
    // World -> the filter's space: q = x * P.x + y * P.y + z * P.z + t.
    Vec3f x{1.0f, 0.0f, 0.0f}, y{0.0f, 1.0f, 0.0f}, z{0.0f, 0.0f, 1.0f}, t{0.0f, 0.0f, 0.0f};
    LightFilterKind kind = LightFilterKind::Blocker;
    BlockerShape shape = BlockerShape::Sphere;
    // Blocker: the shape in its space (sphere: radius in half.x; capsule:
    // radius, half the segment length; box: the half extents), the box's
    // corner radius, 1 / the shape's size (0: an empty shape) and the falloff
    // in units of that size.
    Vec3f half{0.5f, 0.0f, 0.0f};
    float rounded = 0.0f;
    float invSize = 2.0f;
    float falloff = 0.0f;
    uint32_t invert = 0;
    Spectrum blocked{0.0f, 0.0f, 0.0f}; // the factor inside: intensity 2^exposure tint
    // A link category (docs/design/M9.md D15), the mechanism UsdLux's filterLink
    // would use: the objects the filter acts on, as a light-link bit
    // (kLinksEveryone: every object). Hydra leaves it at every object, as Karma
    // ignores filterLink (docs/design/M11-P4.md F6).
    uint64_t link = ~uint64_t(0);

    NOVA_INLINE Vec3f toSpace(Vec3f p) const { return x * p.x + y * p.y + z * p.z + t; }
};

// The blocker's signed distance at q (in its space), in units of its size.
NOVA_INLINE float blockerDistance(const LightFilterData &f, Vec3f q) {
    switch (f.shape) {
    case BlockerShape::Sphere:
        return length(q) * f.invSize - 1.0f;
    case BlockerShape::Capsule: {
        const float c = nova::fmin(nova::fmax(q.y, -f.half.y), f.half.y);
        return length(Vec3f{q.x, q.y - c, q.z}) * f.invSize - 1.0f;
    }
    default: { // a box with rounded edges and corners
        const Vec3f a = absolute(q) - f.half + spectrum(f.rounded);
        const float outside = length(componentMax(a, spectrum(0.0f)));
        const float inside = nova::fmin(maxComponent(a), 0.0f);
        return (outside + inside - f.rounded) * f.invSize;
    }
    }
}

// The factor of one filter for the point p.
NOVA_INLINE Spectrum lightFilterFactor(const LightFilterData &f, Vec3f p) {
    // Blocker: the share of light that passes, then the blend towards k.
    const float s = f.invSize > 0.0f ? blockerDistance(f, f.toSpace(p)) : 1.0f;
    float u;
    if (f.falloff > 0.0f) {
        const float v = saturate((s + f.falloff) / (2.0f * f.falloff));
        u = v * v * (3.0f - 2.0f * v);
    } else {
        u = s > 0.0f ? 1.0f : 0.0f;
    }
    if (f.invert)
        u = 1.0f - u;
    return f.blocked + (spectrum(1.0f) - f.blocked) * u;
}

// The product of the factors of `count` filters at a site; filters limited
// to other objects (filterLink) leave the light as it is.
NOVA_INLINE Spectrum lightFilterProduct(const LightFilterData *filters, uint32_t count, const LightFilterSite &site) {
    Spectrum k{1.0f, 1.0f, 1.0f};
    for (uint32_t i = 0; i < count; ++i) {
        const LightFilterData &f = filters[i];
        if (f.link == ~uint64_t(0) || (f.link & site.categories) != 0)
            k *= lightFilterFactor(f, site.p);
    }
    return k;
}

// The largest factor a filter can give (its colour at full effect or 1):
// lights scale their selection weight by the product over their filters, so
// a filter that brightens never leaves the light under-sampled.
NOVA_INLINE float lightFilterBound(const LightFilterData &f) {
    return nova::fmax(1.0f, maxComponent(absolute(f.blocked)));
}

} // namespace nova
