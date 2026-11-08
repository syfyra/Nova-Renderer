#pragma once

// Volume phase functions (docs/design/M8.md D10): Henyey-Greenstein, and
// Karma's two-lobe mixture of HG(g) and HG(g2) with weight `mix` on the
// second lobe. Directions follow the light's travel: cosTheta is the cosine
// between the incoming ray's direction and the scattered direction, so g > 0
// scatters forward (Karma, measured). A phase function is its own pdf, so
// sampling it has weight 1.

#include "core/math/common.h"
#include "core/math/frame.h"
#include "core/math/spectrum.h"
#include "core/math/vec.h"

#include <cmath>
#include <cstdint>

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

// Rayleigh's phase function, 3 / (16 pi) (1 + cos^2) (the sky atmosphere's
// air, docs/design/M11-P7.md), and the cosine of a sample by inverting its
// distribution (3c + c^3 + 4) / 8 = u (Cardano: c = A - 1 / A, A^3 = z +
// sqrt(z^2 + 1), z = 4u - 2).
NOVA_INLINE float rayleighPhase(float cosTheta) { return (3.0f / 16.0f) * kInvPi * (1.0f + cosTheta * cosTheta); }
NOVA_INLINE float sampleRayleighCos(float u) {
    const float z = 4.0f * u - 2.0f;
    const float A = std::cbrt(z + std::sqrt(z * z + 1.0f));
    return std::fmax(-1.0f, std::fmin(1.0f, A - 1.0f / A));
}

struct PhaseFunction {
    float g = 0.0f, g2 = 0.0f, mix = 0.0f; // mix: the second lobe's weight
    // The kind: kLobes (HG and Karma's second lobe), kRayleigh, or kSky, the
    // sky atmosphere's air (docs/design/M11-P7.md): Rayleigh scattering and
    // Mie scattering (HG with g) whose share of the scattering is `mie` per
    // channel; directions are sampled from the mixture of weight `mix` on
    // Mie (one scalar), and weight() corrects each channel to its own mix.
    static constexpr uint8_t kLobes = 0, kRayleigh = 1, kSky = 2;
    uint8_t kind = kLobes;
    Spectrum mie{0.0f, 0.0f, 0.0f};

    // d: the incoming ray's direction; wi: the scattered direction. The
    // density of sample() (the phase function itself but for kSky, where the
    // scattered light is weight() times it).
    NOVA_INLINE float eval(Vec3f d, Vec3f wi) const {
        const float c = dot(d, wi);
        if (kind == kRayleigh)
            return rayleighPhase(c);
        if (kind == kSky)
            return rayleighPhase(c) + mix * (hgPhase(g, c) - rayleighPhase(c));
        const float p = hgPhase(g, c);
        return mix > 0.0f ? p + mix * (hgPhase(g2, c) - p) : p;
    }

    // kSky: each channel's phase function over eval(); 1 otherwise.
    NOVA_INLINE Spectrum weight(Vec3f d, Vec3f wi) const {
        if (kind != kSky)
            return Spectrum{1.0f, 1.0f, 1.0f};
        const float c = dot(d, wi);
        const float pr = rayleighPhase(c), pm = hgPhase(g, c);
        const float p = pr + mix * (pm - pr);
        if (!(p > 0.0f))
            return Spectrum{0.0f, 0.0f, 0.0f};
        const float inv = 1.0f / p;
        return Spectrum{(pr + mie.x * (pm - pr)) * inv, (pr + mie.y * (pm - pr)) * inv, (pr + mie.z * (pm - pr)) * inv};
    }

    // u.z picks the lobe; returns the direction, pdf = eval(d, wi).
    NOVA_INLINE Vec3f sample(Vec3f d, Vec3f u, float &pdf) const {
        float cosTheta;
        if (kind == kRayleigh) {
            cosTheta = sampleRayleighCos(u.x);
        } else if (kind == kSky) {
            cosTheta = u.z < mix ? sampleHgCos(g, u.x) : sampleRayleighCos(u.x);
        } else {
            const float gl = mix > 0.0f && u.z < mix ? g2 : g;
            cosTheta = sampleHgCos(gl, u.x);
        }
        const float sinTheta = std::sqrt(std::fmax(0.0f, 1.0f - cosTheta * cosTheta));
        const float phi = kTwoPi * u.y;
        const Vec3f wi = Frame::fromNormal(d).toWorld({sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta});
        pdf = eval(d, wi);
        return wi;
    }
};

} // namespace nova
