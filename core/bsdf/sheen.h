#pragma once

// Sheen and fuzz, reflection only (wo above the surface):
//   CharlieLobe  Standard Surface sheen: MaterialX's sheen_bsdf "conty_kulla"
//                form, the Imageworks NDF (Estevez & Kulla 2017) without a
//                masking term and with the smoother denominator
//                4 (mu_i + mu_o - mu_i mu_o); uniform hemisphere sampling.
//                Karma renders it as a Lambert lobe scaled by MaterialX's
//                albedo fit (measured); Nova follows the MaterialX definition.
//   ZeltnerLobe  OpenPBR fuzz: Zeltner, Burley & Chiang 2022 LTC sheen with
//                MaterialX's fits. Sampled exactly, so a sample's weight is
//                the albedo R(mu_o, r) times the color.

#include "core/bsdf/common.h"
#include "core/bsdf/diffuse.h" // ltcBasis
#include "core/math/sampling.h"

namespace nova {

NOVA_INLINE float charlieD(float cosH, float r) {
    const float inv = 1.0f / nova::fmax(r, 0.005f), sin2 = nova::fmax(0.0f, 1.0f - cosH * cosH);
    return (2.0f + inv) * std::pow(sin2, 0.5f * inv) * (0.5f * kInvPi);
}

struct CharlieLobe {
    Spectrum color;
    float r;

    NOVA_INLINE Spectrum eval(Vec3f wo, Vec3f wi, float &pdf) const {
        if (wo.z <= 0.0f || wi.z <= 0.0f) {
            pdf = 0.0f;
            return {0.0f, 0.0f, 0.0f};
        }
        const Vec3f h = normalize(wo + wi);
        pdf = kInvTwoPi;
        return color * (charlieD(h.z, r) / (4.0f * (wi.z + wo.z - wi.z * wo.z)) * wi.z);
    }
    NOVA_INLINE bool sample(Vec3f wo, Vec2f u, LobeSample &s) const {
        const float z = u.x, st = safeSqrt(1.0f - z * z), phi = kTwoPi * u.y;
        s.wi = {st * std::cos(phi), st * std::sin(phi), z};
        s.delta = false;
        s.transmit = false;
        s.fcos = eval(wo, s.wi, s.pdf);
        return s.pdf > 0.0f;
    }
    // MaterialX's rational fit of the albedo (white color), for parity
    // checks; Nova layers with the exact table (energy.h).
    NOVA_INLINE static float albedoFit(float mu, float r) {
        const float x = 13.67300f - 68.78018f * mu + 799.08825f * r - 905.00061f * mu * r + 60.28956f * mu * mu +
                        1086.96473f * r * r;
        const float y = 1.0f + 61.57746f * mu + 442.78211f * r + 2597.49308f * mu * r + 121.81241f * mu * mu +
                        3045.55075f * r * r;
        return saturate(x / y);
    }
};

// Zeltner LTC: inverse matrix coefficients and the albedo (MaterialX fits).
NOVA_INLINE float zeltnerAInv(float mu, float r) {
    return (2.58126f * mu + 0.813703f * r) * r / (1.0f + 0.310327f * mu * mu + 2.60994f * mu * r);
}
// mu may round to 1 + 1 ulp (dot of two normalized float vectors): clamp so
// the sqrt stays real, as MaterialX clamps NdotV to [eps, 1].
NOVA_INLINE float zeltnerBInv(float mu, float r) {
    return std::sqrt(nova::fmax(1.0f - mu, 0.0f)) * (r - 1.0f) * r * r * r / (0.0000254053f + 1.71228f * mu - 1.71506f * mu * r + 1.34174f * r * r);
}
// Clamped at 0: the fit dips slightly below at r 0.1, mu >= 0.5.
NOVA_INLINE float zeltnerAlbedo(float mu, float r) {
    const float s = r * (0.0206607f + 1.58491f * r) / (0.0379424f + r * (1.32227f + r));
    const float m = r * (-0.193854f + r * (-1.14885f + r * (1.7932f - 0.95943f * r * r))) / (0.046391f + r);
    const float o = r * (0.000654023f + (-0.0207818f + 0.119681f * r) * r) / (1.26264f + r * (-1.92021f + r));
    return nova::fmax(std::exp(-0.5f * sqr((mu - m) / s)) / (s * std::sqrt(kTwoPi)) + o, 0.0f);
}

struct ZeltnerLobe {
    Spectrum color;
    float r; // clamped to [0.01, 1] (the range of the fits)

    NOVA_INLINE float roughness() const { return nova::fmin(nova::fmax(r, 0.01f), 1.0f); }
    // Degenerate fits: the lobe is skipped (Cycles).
    NOVA_INLINE bool active(float muO) const {
        const float rr = roughness();
        return muO > 0.0f && std::fabs(zeltnerAInv(muO, rr)) >= 1e-5f && zeltnerAlbedo(muO, rr) >= 1e-5f;
    }

    // Normalized LTC distribution of wi (the sampling density).
    NOVA_INLINE float density(Vec3f wo, Vec3f wi) const {
        if (wi.z <= 0.0f)
            return 0.0f;
        Vec3f X, Y;
        ltcBasis(wo, X, Y);
        const Vec3f w{dot(wi, X), dot(wi, Y), wi.z};
        const float rr = roughness(), aI = zeltnerAInv(wo.z, rr), bI = zeltnerBInv(wo.z, rr);
        const Vec3f o{aI * w.x + bI * w.z, aI * w.y, w.z};
        const float l2 = dot(o, o);
        return nova::fmax(o.z, 0.0f) * kInvPi * sqr(aI / l2);
    }

    NOVA_INLINE Spectrum eval(Vec3f wo, Vec3f wi, float &pdf) const {
        pdf = 0.0f;
        if (wi.z <= 0.0f || !active(wo.z))
            return {0.0f, 0.0f, 0.0f};
        pdf = density(wo, wi);
        return color * (zeltnerAlbedo(wo.z, roughness()) * pdf);
    }

    NOVA_INLINE bool sample(Vec3f wo, Vec2f u, LobeSample &s) const {
        if (!active(wo.z))
            return false;
        const Vec3f c = sampleCosineHemisphere(u);
        const float rr = roughness(), aI = zeltnerAInv(wo.z, rr), bI = zeltnerBInv(wo.z, rr);
        const Vec3f w = normalize(Vec3f{(c.x - c.z * bI) / aI, c.y / aI, c.z});
        Vec3f X, Y;
        ltcBasis(wo, X, Y);
        s.wi = X * w.x + Y * w.y + Vec3f{0.0f, 0.0f, w.z};
        s.delta = false;
        s.transmit = false;
        s.fcos = eval(wo, s.wi, s.pdf);
        return s.pdf > 0.0f;
    }
};

// ---------------------------------------------------------------------------
// Disney sheen (Houdini's Principled Shader, docs/design/M5.md 3.3): f =
// color (1 - |wi.h|)^5, Burley 2012 without the 1/pi; the albedo is at most
// 0.088 (grazing). Cosine sampled.
// ---------------------------------------------------------------------------

struct DisneySheenLobe {
    Spectrum color;

    NOVA_INLINE Spectrum eval(Vec3f wo, Vec3f wi, float &pdf) const {
        if (wo.z <= 0.0f || wi.z <= 0.0f) {
            pdf = 0.0f;
            return {0.0f, 0.0f, 0.0f};
        }
        const Vec3f h = normalize(wo + wi);
        pdf = wi.z * kInvPi;
        return color * (pow5(1.0f - nova::fmin(std::fabs(dot(wi, h)), 1.0f)) * wi.z);
    }
    NOVA_INLINE bool sample(Vec3f wo, Vec2f u, LobeSample &s) const {
        s.wi = sampleCosineHemisphere(u);
        s.delta = false;
        s.transmit = false;
        s.fcos = eval(wo, s.wi, s.pdf);
        return s.pdf > 0.0f;
    }
    // The directional albedo for a white color (quadrature, 17 steps in mu).
    NOVA_INLINE static float albedo(float mu) {
        constexpr float table[17] = {0.088453f, 0.073844f, 0.061113f, 0.050119f, 0.040698f, 0.032685f,
                                     0.025923f, 0.020264f, 0.015572f, 0.011722f, 0.008603f, 0.006114f,
                                     0.004163f, 0.002671f, 0.001564f, 0.000779f, 0.000257f};
        const float x = saturate(mu) * 16.0f;
        const int i = x < 15.0f ? int(x) : 15;
        return lerp(table[i], table[i + 1], x - float(i));
    }
};

} // namespace nova
