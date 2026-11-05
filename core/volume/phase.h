#pragma once

// Volume phase functions (docs/design/M8.md D10): Henyey-Greenstein, and
// Karma's two-lobe mixture of HG(g) and HG(g2) with weight `mix` on the
// second lobe. Directions follow the light's travel: cosTheta is the cosine
// between the incoming ray's direction and the scattered direction, so g > 0
// scatters forward (Karma, measured). A phase function is its own pdf, so
// sampling it has weight 1.

#include "core/math/common.h"
#include "core/math/frame.h"
#include "core/math/vec.h"

#include <cmath>

namespace nova {

// HG's anisotropy is kept inside (-0.99, 0.99) (VolumeEvaluator).
NOVA_INLINE float hgPhase(float g, float cosTheta) {
    const float d = 1.0f + g * g - 2.0f * g * cosTheta;
    return (0.25f * kInvPi) * (1.0f - g * g) / (d * std::sqrt(d));
}

// cosTheta of an HG sample, u in [0, 1).
NOVA_INLINE float sampleHgCos(float g, float u) {
    if (std::fabs(g) < 1e-3f)
        return 1.0f - 2.0f * u;
    const float s = (1.0f - g * g) / (1.0f - g + 2.0f * g * u);
    return std::fmax(-1.0f, std::fmin(1.0f, (1.0f + g * g - s * s) / (2.0f * g)));
}

struct PhaseFunction {
    float g = 0.0f, g2 = 0.0f, mix = 0.0f; // mix: the second lobe's weight

    // d: the incoming ray's direction; wi: the scattered direction.
    NOVA_INLINE float eval(Vec3f d, Vec3f wi) const {
        const float c = dot(d, wi);
        const float p = hgPhase(g, c);
        return mix > 0.0f ? p + mix * (hgPhase(g2, c) - p) : p;
    }

    // u.z picks the lobe; returns the direction, pdf = eval(d, wi).
    NOVA_INLINE Vec3f sample(Vec3f d, Vec3f u, float &pdf) const {
        const float gl = mix > 0.0f && u.z < mix ? g2 : g;
        const float cosTheta = sampleHgCos(gl, u.x);
        const float sinTheta = std::sqrt(std::fmax(0.0f, 1.0f - cosTheta * cosTheta));
        const float phi = kTwoPi * u.y;
        const Vec3f wi = Frame::fromNormal(d).toWorld({sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta});
        pdf = eval(d, wi);
        return wi;
    }
};

} // namespace nova
