#pragma once

// Thin-film interference (Belcour & Barla 2017) as MaterialX's
// mx_fresnel_airy: the Airy summation over the film with two orders
// (AIRY_FRESNEL_ITERATIONS) and the Gaussian fit of the XYZ sensitivity,
// evaluated per microfacet by the lobes. The result is converted with a
// working-space matrix whose rows are normalized so that the DC term (the
// incoherent reflectance) stays grey: MaterialX uses CIE RGB (E), which does
// the same, while Karma's linear sRGB matrix casts it red (R/G 1.27,
// measured). The film sits on air (outside index 1).

#include "core/bsdf/fresnel.h"

namespace nova {

// Plain aggregate (lobes live in unions): ThinFilm{} is no film.
struct ThinFilm {
    float weight;      // blend towards the film's Fresnel; 0 = no film
    float thicknessNm; // Standard Surface in nm, OpenPBR micrometers * 1000
    float ior;

    NOVA_INLINE bool active() const { return weight > 0.0f && thicknessNm > 0.0f; }
};

// XYZ to linear Rec.709 (D65), each row divided by its sum so that
// (1, 1, 1) maps to (1, 1, 1). Replaced by the scene's working space.
constexpr float kXyzToRec709Grey[9] = {
    3.2404542f / 1.2047843f,  -1.5371385f / 1.2047843f, -0.4985314f / 1.2047843f,
    -0.9692660f / 0.9483008f, 1.8760108f / 0.9483008f,  0.0415560f / 0.9483008f,
    0.0556434f / 0.9088427f,  -0.2040259f / 0.9088427f, 1.0572252f / 0.9088427f,
};

// The base under the film: a conductor or dielectric (n, k; k = 0 for a
// dielectric) or the generalized Schlick / F82-tint model.
struct AiryBase {
    bool schlick;
    Spectrum n, k;           // !schlick
    Spectrum F0, tint;       // schlick (F90 1)
    float exponent;          // schlick
};

NOVA_INLINE void fresnelDielectricPolarized(float cosTheta, float eta, float &Rp, float &Rs) {
    const float c2 = sqr(saturate(cosTheta)), s2 = 1.0f - c2;
    const float t0 = nova::fmax(eta * eta - s2, 0.0f), t1 = t0 + c2, t2 = 2.0f * std::sqrt(t0) * cosTheta;
    Rs = (t1 - t2) / (t1 + t2);
    const float t3 = c2 * t0 + s2 * s2, t4 = t2 * s2;
    Rp = Rs * (t3 - t4) / (t3 + t4);
}

NOVA_INLINE void fresnelConductorPolarized(float cosTheta, float n, float k, float &Rp, float &Rs) {
    const float c2 = sqr(saturate(cosTheta)), s2 = 1.0f - c2, n2 = n * n, k2 = k * k;
    const float t0 = n2 - k2 - s2, a2b2 = std::sqrt(t0 * t0 + 4.0f * n2 * k2), t1 = a2b2 + c2;
    const float a = std::sqrt(nova::fmax(0.5f * (a2b2 + t0), 0.0f)), t2 = 2.0f * a * cosTheta;
    Rs = (t1 - t2) / (t1 + t2);
    const float t3 = c2 * a2b2 + s2 * s2, t4 = t2 * s2;
    Rp = Rs * (t3 - t4) / (t3 + t4);
}

// Phase shifts of the reflection at the base (eta1 above, eta2 + i kappa2 below).
NOVA_INLINE void fresnelConductorPhasePolarized(float cosTheta, float eta1, float eta2, float kappa2, float &phiP,
                                                float &phiS) {
    const float k2 = kappa2 / eta2, sin2 = 1.0f - cosTheta * cosTheta;
    const float A = eta2 * eta2 * (1.0f - k2 * k2) - eta1 * eta1 * sin2;
    const float B = std::sqrt(A * A + sqr(2.0f * eta2 * eta2 * k2));
    const float U = std::sqrt((A + B) / 2.0f), V = nova::fmax(0.0f, std::sqrt(nova::fmax((B - A) / 2.0f, 0.0f)));
    phiS = std::atan2(2.0f * eta1 * V * cosTheta, U * U + V * V - sqr(eta1 * cosTheta));
    phiP = std::atan2(2.0f * eta1 * eta2 * eta2 * cosTheta * (2.0f * k2 * U - (1.0f - k2 * k2) * V),
                      sqr(eta2 * eta2 * (1.0f + k2 * k2) * cosTheta) - eta1 * eta1 * (U * U + V * V));
}

// Fourier transform of the XYZ sensitivity at optical path difference opd
// (meters), phase-shifted per channel; S(0) = (1, 1, 1).
NOVA_INLINE Spectrum airySensitivity(float opd, Spectrum shift) {
    const float phase = kTwoPi * opd;
    const Spectrum val{5.4856e-13f, 4.4201e-13f, 5.2481e-13f};
    const Spectrum pos{1.6810e+06f, 1.7953e+06f, 2.2084e+06f};
    const Spectrum var{4.3278e+09f, 9.3046e+09f, 6.6121e+09f};
    Spectrum xyz;
    for (int c = 0; c < 3; ++c)
        xyz[c] = val[c] * std::sqrt(kTwoPi * var[c]) * std::cos(pos[c] * phase + shift[c]) *
                 std::exp(-var[c] * phase * phase);
    xyz.x += 9.7470e-14f * std::sqrt(kTwoPi * 4.5282e+09f) * std::cos(2.2399e+06f * phase + shift.x) *
             std::exp(-4.5282e+09f * phase * phase);
    return xyz * (1.0f / 1.0685e-7f);
}

// Reflectance of the film-coated base at cosine cosTheta (on the air side),
// in the working space of the row-normalized matrix xyzToRgb.
NOVA_INLINE Spectrum fresnelAiry(float cosTheta, const ThinFilm &film, const AiryBase &base, const float *xyzToRgb) {
    constexpr int kIterations = 2;
    const float eta1 = 1.0f, eta2 = nova::fmax(film.ior, eta1);
    const float cosThetaT = safeSqrt(1.0f - (1.0f - cosTheta * cosTheta) * sqr(eta1 / eta2));

    // First interface (air -> film).
    float R12p, R12s;
    fresnelDielectricPolarized(cosTheta, eta2 / eta1, R12p, R12s);
    if (cosThetaT <= 0.0f)
        R12p = R12s = 1.0f;
    const float T121p = 1.0f - R12p, T121s = 1.0f - R12s;

    // Second interface (film -> base) and its phase.
    Spectrum R23p, R23s, phi23p, phi23s;
    for (int c = 0; c < 3; ++c) {
        if (base.schlick) {
            const float f = fresnelF82Tint(cosThetaT, base.F0[c], base.tint[c], 1.0f, base.exponent);
            R23p[c] = R23s[c] = 0.5f * f;
            phi23p[c] = phi23s[c] = f0ToIor(base.F0[c]) < eta2 ? kPi : 0.0f;
        } else {
            fresnelConductorPolarized(cosThetaT, base.n[c] / eta2, base.k[c] / eta2, R23p[c], R23s[c]);
            fresnelConductorPhasePolarized(cosThetaT, eta2, base.n[c], base.k[c], phi23p[c], phi23s[c]);
        }
    }
    const float cosB = std::cos(std::atan(eta2 / eta1));
    const float phi21p = cosTheta < cosB ? 0.0f : kPi, phi21s = kPi;
    const float opd = 2.0f * eta2 * cosThetaT * film.thicknessNm * 1.0e-9f;

    Spectrum I{0.0f, 0.0f, 0.0f};
    for (int pol = 0; pol < 2; ++pol) {
        const float R12 = pol == 0 ? R12p : R12s, T121 = pol == 0 ? T121p : T121s, phi21 = pol == 0 ? phi21p : phi21s;
        const Spectrum R23 = pol == 0 ? R23p : R23s, phi23 = pol == 0 ? phi23p : phi23s;
        const Spectrum r123{std::sqrt(nova::fmax(R12 * R23.x, 0.0f)), std::sqrt(nova::fmax(R12 * R23.y, 0.0f)),
                            std::sqrt(nova::fmax(R12 * R23.z, 0.0f))};
        // DC term, then pairs of Diracs for m >= 1.
        const Spectrum Rs = R23 * (T121 * T121) / spectrumMax(spectrum(1.0f) - R23 * R12, 1e-6f);
        I += spectrum(R12) + Rs;
        Spectrum Cm = Rs - spectrum(T121);
        for (int m = 1; m <= kIterations; ++m) {
            Cm *= r123;
            const Spectrum shift = (phi23 + spectrum(phi21)) * float(m);
            I += Cm * airySensitivity(float(m) * opd, shift) * 2.0f;
        }
    }
    I *= 0.5f;
    const Spectrum rgb{xyzToRgb[0] * I.x + xyzToRgb[1] * I.y + xyzToRgb[2] * I.z,
                       xyzToRgb[3] * I.x + xyzToRgb[4] * I.y + xyzToRgb[5] * I.z,
                       xyzToRgb[6] * I.x + xyzToRgb[7] * I.y + xyzToRgb[8] * I.z};
    return {saturate(rgb.x), saturate(rgb.y), saturate(rgb.z)};
}

} // namespace nova
