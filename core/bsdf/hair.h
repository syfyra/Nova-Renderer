#pragma once

// Hair: Chiang, Bitterli, Tappan and Burley 2016 in pbrt's formulation, with
// Karma Hair's conventions (docs/design/M9.md D5; research
// docs/research/m9/curves-hair.md 3.4, 4.3). A fibre scatters through four
// lobes p: R (reflection), TT, TRT and the residual TRRT+. Each is one Nova
// lobe: weight tint_p A_p (the attenuation through the cortex, from the
// Fresnel term and the absorption at wo), a normalised longitudinal
// distribution M_p (d'Eon et al. 2011) and an azimuthal N_p (a logistic
// trimmed to [-pi, pi] around Phi(p); TRRT+ uniform), so f |cos| =
// sum_p weight_p M_p N_p. Everything that depends on wo alone is computed
// when the closure is built.
//
// The lobe frame: x = the fibre's tangent (root to tip), y and z span the
// normal plane; sin(theta) = w.x, phi = atan2(w.z, w.y), and the lobe keeps
// wo's phi. h in [-1, 1] is the offset across the fibre: n . (z_o x x) with
// z_o = wo projected into the normal plane. Karma's measured conventions: pbrt's 10-term
// Bessel series (its lobes match it, not the exact I0), and the cuticle
// tilt rotates theta_i (R: theta_i - 2a, TT: + a, TRT: + 4a), not theta_o.

#include "core/bsdf/common.h"
#include "core/bsdf/fresnel.h"
#include "core/math/common.h"

#include <cmath>

namespace nova {

namespace hair_detail {

// pbrt's I0: 10 terms of the power series, and its asymptotic logarithm.
NOVA_INLINE float besselI0(float x) {
    float val = 0.0f, term = 1.0f;
    const float q = x * x * 0.25f;
    for (int i = 0; i < 10; ++i) {
        if (i > 0)
            term *= q / float(i * i);
        val += term;
    }
    return val;
}
NOVA_INLINE float logI0(float x) {
    if (x > 12.0f)
        return x + 0.5f * (-std::log(kTwoPi) + std::log(1.0f / x) + 1.0f / (8.0f * x));
    return std::log(besselI0(x));
}

// d'Eon's longitudinal function (normalised over the sphere in the cos(theta)
// d(theta) measure). Small variances take the log form, written so the large
// terms cancel before the exponential: a - 1/v - b = (cos(theta_i + theta_o) - 1) / v.
NOVA_INLINE float longitudinal(float cosI, float cosO, float sinI, float sinO, float v) {
    const float a = cosI * cosO / v;
    if (v <= 0.1f)
        return std::exp(logI0(a) - a + (cosI * cosO - sinI * sinO - 1.0f) / v + std::log(1.0f / v));
    return std::exp(-sinI * sinO / v) * besselI0(a) / (std::sinh(1.0f / v) * 2.0f * v);
}

NOVA_INLINE float logistic(float x, float s) {
    const float e = std::exp(-std::fabs(x) / s);
    return e / (s * (1.0f + e) * (1.0f + e));
}
NOVA_INLINE float logisticCdf(float x, float s) { return 1.0f / (1.0f + std::exp(-x / s)); }
NOVA_INLINE float trimmedLogistic(float x, float s) {
    return logistic(x, s) / (logisticCdf(kPi, s) - logisticCdf(-kPi, s));
}
NOVA_INLINE float sampleTrimmedLogistic(float u, float s) {
    const float lo = logisticCdf(-kPi, s), k = logisticCdf(kPi, s) - lo;
    const float x = -s * std::log(1.0f / (u * k + lo) - 1.0f);
    return std::fmin(std::fmax(x, -kPi), kPi);
}

NOVA_INLINE float wrapPi(float x) {
    while (x > kPi)
        x -= kTwoPi;
    while (x < -kPi)
        x += kTwoPi;
    return x;
}

// The roughness maps of Chiang 2016 (eqs. 7, 8): beta_m -> v_R, beta_n -> s.
NOVA_INLINE float longitudinalVariance(float betaM) {
    const float b = nova::fmin(nova::fmax(betaM, 1e-3f), 1.0f);
    const float b2 = b * b, b4 = b2 * b2, b8 = b4 * b4, b16 = b8 * b8;
    const float x = 0.726f * b + 0.812f * b2 + 3.7f * b16 * b4;
    return nova::fmax(x * x, 1e-5f);
}
NOVA_INLINE float azimuthalScale(float betaN) {
    const float b = nova::fmin(nova::fmax(betaN, 1e-3f), 1.0f);
    const float b2 = b * b, b4 = b2 * b2, b8 = b4 * b4, b16 = b8 * b8;
    return 0.626657069f * (0.265f * b + 1.194f * b2 + 5.372f * b16 * b4 * b2); // sqrt(pi/8)
}

} // namespace hair_detail

// Chiang 2016 eq. 9: the absorption whose fibre has the colour c (multiple
// scattering in a hair volume) at azimuthal roughness beta_n.
NOVA_INLINE Spectrum hairAbsorptionFromColor(Spectrum c, float betaN) {
    const float b = nova::fmin(nova::fmax(betaN, 0.0f), 1.0f), b2 = b * b, b3 = b2 * b;
    const float d = 5.969f - 0.215f * b + 2.532f * b2 - 10.73f * b3 + 5.574f * b2 * b2 + 0.245f * b3 * b2;
    Spectrum s;
    for (int i = 0; i < 3; ++i) {
        const float l = std::log(nova::fmax(c[i], 1e-4f)) / d;
        s[i] = l * l;
    }
    return s;
}

// One lobe of the fibre at a fixed wo (in the lobe frame of this header).
struct HairLobe {
    uint8_t p;               // 0 R, 1 TT, 2 TRT, 3 TRRT+
    float v;                 // longitudinal variance
    float s;                 // azimuthal logistic scale
    float sinO, cosO;        // theta_o
    float sinTilt, cosTilt;  // the cuticle tilt of theta_i, k_p alpha
    float phiO;              // wo's azimuth in the lobe frame
    float phiP;              // Phi(p) = 2 p gamma_t - 2 gamma_o + p pi

    // f |cos| / weight (= M_p N_p) and the density of sample().
    NOVA_INLINE Spectrum eval(Vec3f wo, Vec3f wi, float &pdf) const {
        (void)wo;
        const float sinI = nova::fmin(nova::fmax(wi.x, -1.0f), 1.0f);
        const float cosI = std::sqrt(nova::fmax(0.0f, 1.0f - sinI * sinI));
        const float dphi = hair_detail::wrapPi(std::atan2(wi.z, wi.y) - phiO - phiP);
        // theta_i' = theta_i + k alpha is what M_p sees.
        const float sinIp = sinI * cosTilt + cosI * sinTilt;
        const float cosIp = cosI * cosTilt - sinI * sinTilt;
        const float m = hair_detail::longitudinal(std::fabs(cosIp), cosO, sinIp, sinO, v);
        const float n = p < 3 ? hair_detail::trimmedLogistic(dphi, s) : 0.5f * kInvPi;
        // sample() draws theta_i' from M_p cos(theta_i') d(theta_i'): in
        // solid angle that is M_p cos(theta_i') / cos(theta_i).
        pdf = cosIp > 0.0f && cosI > 1e-6f ? m * n * (cosIp / cosI) : 0.0f;
        return spectrum(m * n);
    }

    // u.x, uc: the longitudinal angle (d'Eon's sampler about the mirror
    // direction); u.y: the azimuth.
    NOVA_INLINE bool sample(Vec3f wo, Vec2f u, float uc, LobeSample &ls) const {
        const float u1 = nova::fmax(u.x, 1e-5f);
        const float cosTheta = 1.0f + v * std::log(u1 + (1.0f - u1) * std::exp(-2.0f / v));
        const float sinTheta = std::sqrt(nova::fmax(0.0f, 1.0f - cosTheta * cosTheta));
        const float sinIp = nova::fmin(nova::fmax(-cosTheta * sinO + sinTheta * std::cos(kTwoPi * uc) * cosO, -1.0f), 1.0f);
        const float cosIp = std::sqrt(nova::fmax(0.0f, 1.0f - sinIp * sinIp));
        // theta_i = theta_i' - k alpha; beyond the pole there is no direction.
        const float sinI = sinIp * cosTilt - cosIp * sinTilt;
        const float cosI = cosIp * cosTilt + sinIp * sinTilt;
        if (!(cosI > 1e-6f))
            return false;
        const float dphi = p < 3 ? phiP + hair_detail::sampleTrimmedLogistic(u.y, s) : kTwoPi * u.y;
        const float phiI = phiO + dphi;
        ls.wi = {sinI, cosI * std::cos(phiI), cosI * std::sin(phiI)};
        ls.fcos = eval(wo, ls.wi, ls.pdf);
        ls.delta = false;
        ls.transmit = false; // sampleClosure classifies by the geometric side
        return ls.pdf > 0.0f;
    }
};

// What the four lobes of a fibre need at wo: per lobe its HairLobe and the
// attenuation A_p (per channel).
struct HairFibre {
    HairLobe lobe[4];
    Spectrum A[4];
};

// eta: the cortex's index; sigmaA: its absorption per unit of the fibre's
// diameter (pbrt's sigma_a); v, s: per lobe R, TT, TRT (TRRT+ takes TRT's);
// alpha: the cuticle tilt in radians (positive tilts the R highlight toward
// the tip); sinO/cosO/phiO: wo in the lobe frame; h: the offset across.
NOVA_INLINE HairFibre hairFibre(float eta, Spectrum sigmaA, const float v[3], const float s[3], float alpha, float sinO,
                                float cosO, float phiO, float h) {
    HairFibre f;
    eta = nova::fmax(eta, 1.0001f);
    cosO = nova::fmax(cosO, 1e-4f);
    h = nova::fmin(nova::fmax(h, -1.0f), 1.0f);
    const float sinT = sinO / eta, cosT = std::sqrt(nova::fmax(0.0f, 1.0f - sinT * sinT));
    const float etap = std::sqrt(nova::fmax(eta * eta - sinO * sinO, 0.0f)) / cosO;
    const float sinGammaT = nova::fmin(nova::fmax(h / etap, -1.0f), 1.0f);
    const float cosGammaT = std::sqrt(nova::fmax(0.0f, 1.0f - sinGammaT * sinGammaT));
    const float gammaO = std::asin(h), gammaT = std::asin(sinGammaT);
    const float F = fresnelDielectric(cosO * std::sqrt(nova::fmax(0.0f, 1.0f - h * h)), eta);
    Spectrum T;
    for (int c = 0; c < 3; ++c)
        T[c] = std::exp(-nova::fmax(sigmaA[c], 0.0f) * (2.0f * cosGammaT / nova::fmax(cosT, 1e-4f)));
    f.A[0] = spectrum(F);
    f.A[1] = T * ((1.0f - F) * (1.0f - F));
    f.A[2] = f.A[1] * T * F;
    for (int c = 0; c < 3; ++c)
        f.A[3][c] = f.A[2][c] * F * T[c] / nova::fmax(1.0f - T[c] * F, 1e-6f);
    static constexpr float k[4] = {-2.0f, 1.0f, 4.0f, 0.0f};
    for (int p = 0; p < 4; ++p) {
        HairLobe &l = f.lobe[p];
        l.p = uint8_t(p);
        l.v = nova::fmax(v[p < 3 ? p : 2], 1e-5f);
        l.s = nova::fmax(s[p < 3 ? p : 2], 1e-4f);
        l.sinO = sinO;
        l.cosO = cosO;
        l.sinTilt = std::sin(k[p] * alpha);
        l.cosTilt = std::cos(k[p] * alpha);
        l.phiO = phiO;
        l.phiP = 2.0f * float(p) * gammaT - 2.0f * gammaO + float(p) * kPi;
    }
    return f;
}

} // namespace nova
