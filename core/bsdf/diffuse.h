#pragma once

// Diffuse lobes: Lambert reflection and its transmitting twin (translucent,
// the thin-walled subsurface share); Standard Surface's rough diffuse
// (qualitative Oren-Nayar, MaterialX mx_oren_nayar_diffuse, not energy
// preserving by definition) and OpenPBR's energy-preserving Oren-Nayar (EON,
// Portsmouth, Kutz & Hill 2024, as MaterialX mx_oren_nayar_compensated_diffuse
// with the exact Fujii albedo). The rough lobes reflect only and expect wo
// above the surface (the closure faces the frame towards wo).

#include "core/bsdf/common.h"
#include "core/math/sampling.h"

namespace nova {

struct LambertLobe {
    Spectrum albedo;
    bool transmit; // translucent: light from the opposite hemisphere

    NOVA_INLINE Spectrum eval(Vec3f wo, Vec3f wi, float &pdf) const {
        // Reflection: wi on wo's side; translucency: on the other side.
        const bool other = wo.z * wi.z < 0.0f;
        if (wo.z == 0.0f || wi.z == 0.0f || other != transmit) {
            pdf = 0.0f;
            return {0.0f, 0.0f, 0.0f};
        }
        const float c = std::fabs(wi.z);
        pdf = c * kInvPi;
        return albedo * (c * kInvPi);
    }

    NOVA_INLINE bool sample(Vec3f wo, Vec2f u, LobeSample &s) const {
        if (wo.z == 0.0f)
            return false;
        Vec3f wi = sampleCosineHemisphere(u);
        if ((wo.z < 0.0f) != transmit) // sample the hemisphere below
            wi.z = -wi.z;
        s.wi = wi;
        s.transmit = transmit;
        s.delta = false;
        s.fcos = eval(wo, wi, s.pdf);
        return s.pdf > 0.0f;
    }
};

// ---------------------------------------------------------------------------
// Burley's diffuse (MaterialX burley_diffuse_bsdf, mx_burley_diffuse: Disney's
// retro-reflective Schlick factors F_L F_V with F90 = 0.5 + 2 r (L.H)^2; not
// energy conserving by definition, albedo up to 1.43 at grazing for r 1)
// ---------------------------------------------------------------------------

struct BurleyLobe {
    Spectrum albedo;
    float roughness;

    NOVA_INLINE Spectrum eval(Vec3f wo, Vec3f wi, float &pdf) const {
        if (wo.z <= 0.0f || wi.z <= 0.0f) {
            pdf = 0.0f;
            return {0.0f, 0.0f, 0.0f};
        }
        const Vec3f h = normalize(wo + wi);
        const float ldh = nova::fmax(dot(wi, h), 1e-6f);
        const float f90 = 0.5f + 2.0f * roughness * ldh * ldh;
        const float fl = 1.0f + (f90 - 1.0f) * pow5(1.0f - nova::fmin(wi.z, 1.0f));
        const float fv = 1.0f + (f90 - 1.0f) * pow5(1.0f - nova::fmin(wo.z, 1.0f));
        pdf = wi.z * kInvPi;
        return albedo * (fl * fv * wi.z * kInvPi);
    }
    NOVA_INLINE bool sample(Vec3f wo, Vec2f u, LobeSample &s) const {
        s.wi = sampleCosineHemisphere(u);
        s.delta = false;
        s.transmit = false;
        s.fcos = eval(wo, s.wi, s.pdf);
        return s.pdf > 0.0f;
    }
    // MaterialX's fit of the directional albedo (Stephen Hill), for albedo 1.
    NOVA_INLINE static float albedoFit(float mu, float roughness) {
        const float x = mu;
        const float fit0 = 0.97619f - 0.488095f * pow5(1.0f - x);
        const float fit1 = 1.55754f + (-2.02221f + (2.56283f - 1.06244f * x) * x) * x;
        return lerp(fit0, fit1, roughness);
    }
};

// ---------------------------------------------------------------------------
// Qualitative Oren-Nayar (Standard Surface: sigma = diffuse_roughness)
// ---------------------------------------------------------------------------

struct QonLobe {
    Spectrum albedo;
    float sigma;

    NOVA_INLINE Spectrum eval(Vec3f wo, Vec3f wi, float &pdf) const {
        if (wo.z <= 0.0f || wi.z <= 0.0f) {
            pdf = 0.0f;
            return {0.0f, 0.0f, 0.0f};
        }
        const float s = dot(wi, wo) - wi.z * wo.z;
        const float stinv = s > 0.0f ? s / nova::fmax(wi.z, wo.z) : 0.0f;
        const float s2 = sigma * sigma;
        const float A = 1.0f - 0.5f * (s2 / (s2 + 0.33f)), B = 0.45f * s2 / (s2 + 0.09f);
        pdf = wi.z * kInvPi;
        return albedo * ((A + B * stinv) * wi.z * kInvPi);
    }
    NOVA_INLINE bool sample(Vec3f wo, Vec2f u, LobeSample &s) const {
        s.wi = sampleCosineHemisphere(u);
        s.delta = false;
        s.transmit = false;
        s.fcos = eval(wo, s.wi, s.pdf);
        return s.pdf > 0.0f;
    }
    // MaterialX's rational fit of the albedo (for albedo 1): lobe selection
    // and the albedo AOV only, the lobe itself stays uncompensated.
    NOVA_INLINE static float albedoFit(float mu, float sigma) {
        const float x = 1.0f - 0.4297f * sigma - 0.7632f * mu * sigma + 1.4385f * sigma * sigma;
        const float y = 1.0f - 0.6076f * sigma - 0.4993f * mu * sigma + 2.0315f * sigma * sigma;
        return saturate(x / y);
    }
};

// ---------------------------------------------------------------------------
// Energy-preserving Oren-Nayar (OpenPBR: r = base_diffuse_roughness)
// ---------------------------------------------------------------------------

constexpr float kFujii1 = 0.5f - 2.0f / (3.0f * kPi);
constexpr float kFujii2 = 2.0f / 3.0f - 28.0f / (15.0f * kPi);
constexpr float kFloatEps = 1.1920929e-7f;

// Directional albedo of the single-scattering Fujii lobe (exact form).
NOVA_INLINE float fujiiAlbedo(float mu, float r) {
    const float A = 1.0f / (1.0f + kFujii1 * r), B = r * A;
    const float c = nova::fmin(nova::fmax(mu, 1e-6f), 1.0f), si = safeSqrt(1.0f - c * c);
    const float G = si * (std::acos(c) - si * c) + 2.0f * ((si / c) * (1.0f - si * si * si) - si) / 3.0f;
    return A + B * G * kInvPi;
}
NOVA_INLINE float fujiiAlbedoAvg(float r) { return (1.0f + kFujii2 * r) / (1.0f + kFujii1 * r); }

// Basis with wo in the xz plane (x along wo's projection).
NOVA_INLINE void ltcBasis(Vec3f wo, Vec3f &X, Vec3f &Y) {
    const float l2 = wo.x * wo.x + wo.y * wo.y;
    X = l2 > 0.0f ? Vec3f{wo.x, wo.y, 0.0f} * (1.0f / std::sqrt(l2)) : Vec3f{1.0f, 0.0f, 0.0f};
    Y = {-X.y, X.x, 0.0f};
}

// EON's importance sampling: a clipped linearly transformed cosine plus a
// uniform hemisphere share (defensive one-sample MIS); paper Listings 2-4.
NOVA_INLINE void eonLtcCoefficients(float mu, float r, float &a, float &b, float &c, float &d) {
    a = 1.0f + r * (0.303392f + (-0.518982f + 0.111709f * mu) * mu + (-0.276266f + 0.335918f * mu) * r);
    b = r * (-1.16407f + 1.15859f * mu + (0.150815f - 0.150105f * mu) * r) / (mu * mu * mu - 1.43545f);
    c = 1.0f + r * (0.20013f + (-0.506373f + 0.261777f * mu) * mu);
    d = r * (0.540852f + (-1.01625f + 0.475392f * mu) * mu) / (-1.0743f + (0.0725628f + mu) * mu);
}
NOVA_INLINE float eonUniformProbability(float mu, float r) {
    return std::pow(r, 0.1f) * (0.162925f + (-0.372058f + (0.538233f - 0.290822f * mu) * mu) * mu);
}

struct EonLobe {
    Spectrum albedo;
    float r;

    NOVA_INLINE Spectrum multiScatterAlbedo() const {
        const float avg = fujiiAlbedoAvg(r);
        return albedo * albedo * avg / spectrumMax(spectrum(1.0f) - albedo * nova::fmax(0.0f, 1.0f - avg), 1e-6f);
    }

    NOVA_INLINE Spectrum eval(Vec3f wo, Vec3f wi, float &pdf) const {
        pdf = 0.0f;
        if (wo.z <= 0.0f || wi.z <= 0.0f)
            return {0.0f, 0.0f, 0.0f};
        const float s = dot(wi, wo) - wi.z * wo.z;
        const float stinv = s > 0.0f ? s / nova::fmax(wi.z, wo.z) : s;
        const float A = 1.0f / (1.0f + kFujii1 * r);
        const Spectrum single = albedo * (A * (1.0f + r * stinv));
        const float k = nova::fmax(kFloatEps, 1.0f - fujiiAlbedo(wo.z, r)) *
                        nova::fmax(kFloatEps, 1.0f - fujiiAlbedo(wi.z, r)) /
                        nova::fmax(kFloatEps, 1.0f - fujiiAlbedoAvg(r));
        pdf = density(wo, wi);
        return (single + multiScatterAlbedo() * k) * (wi.z * kInvPi);
    }

    NOVA_INLINE float density(Vec3f wo, Vec3f wi) const {
        if (wi.z <= 0.0f)
            return 0.0f;
        const float Pu = eonUniformProbability(wo.z, r);
        return Pu * kInvTwoPi + (1.0f - Pu) * ltcPdf(wo, wi);
    }

    // uc chooses between the uniform and the LTC part.
    NOVA_INLINE bool sample(Vec3f wo, Vec2f u, float uc, LobeSample &s) const {
        if (wo.z <= 0.0f)
            return false;
        if (uc < eonUniformProbability(wo.z, r)) {
            const float z = u.x, st = safeSqrt(1.0f - z * z), phi = kTwoPi * u.y;
            s.wi = {st * std::cos(phi), st * std::sin(phi), z};
        } else {
            s.wi = ltcSample(wo, u);
        }
        s.delta = false;
        s.transmit = false;
        s.fcos = eval(wo, s.wi, s.pdf);
        return s.pdf > 0.0f;
    }

    // Single scattering E plus the multiply scattered rest: albedo at mu.
    NOVA_INLINE Spectrum directionalAlbedo(float mu) const {
        return lerp(multiScatterAlbedo(), albedo, fujiiAlbedo(mu, r));
    }

    NOVA_INLINE float ltcPdf(Vec3f wo, Vec3f wiLocal) const {
        Vec3f X, Y;
        ltcBasis(wo, X, Y);
        const Vec3f wi{dot(wiLocal, X), dot(wiLocal, Y), wiLocal.z};
        float a, b, c, d;
        eonLtcCoefficients(wo.z, r, a, b, c, d);
        const float detM = c * (a - b * d);
        const Vec3f wh{c * (wi.x - b * wi.z), (a - b * d) * wi.y, -c * (d * wi.x - a * wi.z)};
        const float lenSqr = dot(wh, wh);
        const float vz = 1.0f / std::sqrt(d * d + 1.0f), sh = 0.5f * (1.0f + vz);
        return detM * detM / (lenSqr * lenSqr) * nova::fmax(wh.z, 0.0f) / (kPi * sh);
    }
    NOVA_INLINE Vec3f ltcSample(Vec3f wo, Vec2f u) const {
        float a, b, c, d;
        eonLtcCoefficients(wo.z, r, a, b, c, d);
        const float R = std::sqrt(u.x), phi = kTwoPi * u.y;
        float x = R * std::cos(phi);
        const float y = R * std::sin(phi);
        const float vz = 1.0f / std::sqrt(d * d + 1.0f), sh = 0.5f * (1.0f + vz);
        x = -lerp(safeSqrt(1.0f - y * y), x, sh);
        const Vec3f wh{x, y, safeSqrt(1.0f - (x * x + y * y))};
        const Vec3f w{a * wh.x + b * wh.z, c * wh.y, d * wh.x + wh.z};
        Vec3f X, Y;
        ltcBasis(wo, X, Y);
        return normalize(X * w.x + Y * w.y + Vec3f{0.0f, 0.0f, w.z});
    }
};

} // namespace nova
