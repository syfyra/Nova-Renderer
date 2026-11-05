#pragma once

// Anisotropic GGX (Trowbridge-Reitz) with height-correlated Smith masking in
// the stable form of Heitz 2014 (no tan, no 1 - cos^2 cancellation), and
// visible-normal sampling with a spherical cap (Dupuy & Benyoub 2023) or the
// bounded cap for reflection-only lobes (Eto & Tokuyoshi 2023: at alpha 0.8,
// mu 0.95 the valid samples rise from 62 % to 97 %). The cap must not be
// bounded for lobes that transmit: it excludes microfacets that refract.

#include "core/bsdf/common.h"

namespace nova {

// Lobes whose larger alpha lies below this are perfectly smooth (a delta
// event), as pbrt's EffectivelySmooth.
constexpr float kSmoothAlpha = 1e-3f;
// Rough lobes clamp their alphas to this, which keeps D and its sampling
// finite.
constexpr float kMinAlpha = 1e-4f;

struct GgxDistribution {
    float ax, ay; // along the tangent (x) and the bitangent (y)

    NOVA_INLINE static GgxDistribution make(float ax, float ay) {
        return {nova::fmin(nova::fmax(ax, kMinAlpha), 1.0f), nova::fmin(nova::fmax(ay, kMinAlpha), 1.0f)};
    }
    NOVA_INLINE bool smooth() const { return nova::fmax(ax, ay) < kSmoothAlpha; }

    // D(m) = 1 / (pi ax ay (mx^2/ax^2 + my^2/ay^2 + mz^2)^2)
    NOVA_INLINE float D(Vec3f m) const {
        if (m.z <= 0.0f)
            return 0.0f;
        const float x = m.x / ax, y = m.y / ay, d = x * x + y * y + m.z * m.z;
        return 1.0f / (kPi * ax * ay * d * d);
    }
    // t(w) = |(ax wx, ay wy, wz)|; Lambda(w) = (t - |wz|) / (2 |wz|).
    NOVA_INLINE float t(Vec3f w) const { return std::sqrt(sqr(ax * w.x) + sqr(ay * w.y) + sqr(w.z)); }
    NOVA_INLINE float G1(Vec3f w) const {
        const float z = std::fabs(w.z);
        return 2.0f * z / (t(w) + z);
    }
    // Height-correlated G2 / (4 |wo.z| |wi.z|) = 1 / (2 (t_o |wi.z| + t_i |wo.z|)).
    NOVA_INLINE float vis(Vec3f wo, Vec3f wi) const {
        return 0.5f / (t(wo) * std::fabs(wi.z) + t(wi) * std::fabs(wo.z));
    }
    NOVA_INLINE float G2(Vec3f wo, Vec3f wi) const { return 4.0f * std::fabs(wo.z) * std::fabs(wi.z) * vis(wo, wi); }

    // Eto & Tokuyoshi 2023: the cap's lower bound for reflection-only lobes.
    NOVA_INLINE float capK(Vec3f wo, bool bounded) const {
        if (!bounded || wo.z <= 0.0f)
            return 1.0f;
        const float a = saturate(nova::fmin(ax, ay)), s = 1.0f + std::sqrt(sqr(wo.x) + sqr(wo.y));
        const float a2 = a * a, s2 = s * s;
        return (1.0f - a2) * s2 / (s2 + a2 * wo.z * wo.z);
    }
    // A visible normal for wo (wo.z > 0).
    NOVA_INLINE Vec3f sampleVisible(Vec3f wo, Vec2f u, bool bounded) const {
        const Vec3f wh = normalize(Vec3f{ax * wo.x, ay * wo.y, wo.z});
        const float b = capK(wo, bounded) * wh.z, phi = kTwoPi * u.x;
        const float z = (1.0f - u.y) * (1.0f + b) - b, s = std::sqrt(saturate(1.0f - z * z));
        const Vec3f h = Vec3f{s * std::cos(phi), s * std::sin(phi), z} + wh;
        return normalize(Vec3f{ax * h.x, ay * h.y, nova::fmax(h.z, 0.0f)});
    }
    // Density of visible normals: D_wo(m) = G1(wo) max(0, wo.m) D(m) / wo.z
    // (full cap).
    NOVA_INLINE float pdfVisible(Vec3f wo, Vec3f m) const {
        const float om = dot(wo, m);
        return om > 0.0f ? G1(wo) * om * D(m) / wo.z : 0.0f;
    }
    // Solid-angle density of wi = reflect(wo, m) for m from sampleVisible():
    // D(m) / (2 (k wo.z + t(wo))), k = 1 for the full cap.
    NOVA_INLINE float pdfReflect(Vec3f wo, Vec3f m, bool bounded) const {
        return D(m) / (2.0f * (capK(wo, bounded) * wo.z + t(wo)));
    }
};

// Standard Surface (MaterialX mx_roughness_anisotropy): alpha = roughness^2,
// stretched by aspect = sqrt(1 - min(anisotropy, 0.98)).
NOVA_INLINE Vec2f alphaStandardSurface(float roughness, float anisotropy) {
    const float r2 = nova::fmin(nova::fmax(roughness * roughness, 1.1920929e-7f), 1.0f);
    if (!(anisotropy > 0.0f))
        return {r2, r2};
    const float aspect = std::sqrt(1.0f - nova::fmin(anisotropy, 0.98f));
    return {nova::fmin(r2 / aspect, 1.0f), r2 * aspect};
}

// OpenPBR 1.1: alpha_t = r^2 sqrt(2 / (1 + (1 - a)^2)), alpha_b = (1 - a) alpha_t.
NOVA_INLINE Vec2f alphaOpenPbr(float roughness, float anisotropy) {
    const float a = saturate(anisotropy), r2 = roughness * roughness;
    const float at = r2 * std::sqrt(2.0f / (1.0f + sqr(1.0f - a)));
    return {nova::fmin(at, 1.0f), nova::fmin((1.0f - a) * at, 1.0f)};
}

} // namespace nova
