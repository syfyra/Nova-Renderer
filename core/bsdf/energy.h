#pragma once

// Energy of single-scattering microfacet lobes, from tables integrated
// offline with the lobes' own sampling code (tools/nova_bsdf_precompute,
// docs/design/M3.md K16). Rough lobes lose the light that scatters more than
// once between microfacets; the closure scales them back:
//   conductors   f (1 + F_ms (1 - E) / E), F_ms = F_avg E_avg / (1 - F_avg (1 - E_avg))
//                (Turquin 2019 with Kulla & Conty's average Fresnel, as Cycles);
//   dielectrics  f / E_RT (reflection + transmission is lossless), with E_R / E_RT
//                as the albedo of a reflection-only layer over a base.
// The analytic fits of MaterialX are off by up to 4 %, which fails furnace
// tests at 1 %; 32-point tables are within 0.2 %.
//
// Table axes (all in [0, 1]): roughness r = sqrt(alpha) with the root mean
// square alpha of anisotropic lobes, y = sqrt(mu) and
// z = sqrt((eta - 1) / (eta + 1)) for eta >= 1. Tables are integrated for
// isotropic lobes; the rms alpha predicts an anisotropic lobe's albedo
// exactly at normal incidence and within a few percent elsewhere (up to
// about 10 % at grazing angles for alpha ratios beyond 2 at high
// roughness; the geometric mean is off by up to 17 % at normal incidence).

#include "core/bsdf/common.h"

namespace nova {

constexpr int kLutRough = 32, kLutMu = 32, kLutEta = 32, kLutFresnelAvg = 64;
constexpr float kLutMinRough = 0.01f;   // alpha 1e-4 (kMinAlpha)
constexpr float kLutMinMu = 1e-4f;
constexpr float kLutMinEtaIndex = 1e-4f, kLutMaxEtaIndex = 0.99f;

// Pointers to the tables (flat, uploadable as one blob for the GPU). Layouts:
//   ggxE, dielR-like 2D tables [r][y], 3D tables [z][r][y], ggxABC [r][y][3].
struct BsdfTables {
    const float *ggxE;       // E(mu) of GGX reflection with F = 1
    const float *ggxEavg;    // 2 int E mu dmu, per roughness
    const float *ggxABC;     // A, B, C of the generalized-Schlick albedo F0 A + F90 B - b C
    const float *dielRT;     // E_R + E_T (energy units) for eta > 1
    const float *dielRTinv;  // the same for 1 / eta (seen from inside)
    const float *dielR;      // E_R, eta > 1
    const float *fresnelAvg; // 2 int F mu dmu of the exact dielectric Fresnel, eta > 1
    const float *charlieE;   // albedo of the Charlie sheen lobe (core/bsdf/sheen.h), [sheen roughness][y]
    const float *thinRT;     // E_R' + E_T' of a clear thin-walled sheet (ThinDielectricLobe), eta > 1
    const float *thinR;      // E_R' of the same sheet (its reflection lobe; E_T' = thinRT - thinR)
};

// The number of tables and their lengths in floats, in the order of
// BsdfTables' members.
constexpr int kBsdfTableCount = 10;
inline constexpr size_t kBsdfTableFloats[kBsdfTableCount] = {
    size_t(kLutRough) * kLutMu,           size_t(kLutRough),           size_t(kLutRough) * kLutMu * 3,
    size_t(kLutEta) * kLutRough * kLutMu, size_t(kLutEta) * kLutRough * kLutMu,
    size_t(kLutEta) * kLutRough * kLutMu, size_t(kLutFresnelAvg),      size_t(kLutRough) * kLutMu,
    size_t(kLutEta) * kLutRough * kLutMu, size_t(kLutEta) * kLutRough * kLutMu};

// The generated tables (host memory, core/bsdf/tables/bsdf_tables.cpp).
const BsdfTables &bsdfTables();

// ---------------------------------------------------------------------------
// Axes: lookup coordinates and the parameters at the grid nodes
// ---------------------------------------------------------------------------

NOVA_INLINE float lutRough(float ax, float ay) { return std::sqrt(std::sqrt(0.5f * (ax * ax + ay * ay))); }
NOVA_INLINE float lutMu(float mu) { return std::sqrt(saturate(mu)); }
NOVA_INLINE float lutEta(float eta) { // eta >= 1
    return std::sqrt(nova::fmax((eta - 1.0f) / (eta + 1.0f), 0.0f));
}
NOVA_INLINE float lutRoughAt(int i) { return nova::fmax(float(i) / float(kLutRough - 1), kLutMinRough); }
NOVA_INLINE float lutMuAt(int j) { return nova::fmax(sqr(float(j) / float(kLutMu - 1)), kLutMinMu); }
NOVA_INLINE float lutEtaAt(int k, int count) {
    const float z = nova::fmin(nova::fmax(float(k) / float(count - 1), kLutMinEtaIndex), kLutMaxEtaIndex);
    return (1.0f + z * z) / (1.0f - z * z);
}

// ---------------------------------------------------------------------------
// Interpolation (clamped, linear per axis)
// ---------------------------------------------------------------------------

NOVA_INLINE void lutCoordinate(float x, int n, int &i, float &f) {
    const float s = saturate(x) * float(n - 1);
    i = s < float(n - 1) ? int(s) : n - 2;
    f = s - float(i);
}
NOVA_INLINE float lut1(const float *t, int n, float x) {
    int i;
    float f;
    lutCoordinate(x, n, i, f);
    return lerp(t[i], t[i + 1], f);
}
// t[ix * ny + iy]
NOVA_INLINE float lut2(const float *t, int nx, int ny, float x, float y) {
    int ix, iy;
    float fx, fy;
    lutCoordinate(x, nx, ix, fx);
    lutCoordinate(y, ny, iy, fy);
    const float *r0 = t + ix * ny, *r1 = r0 + ny;
    return lerp(lerp(r0[iy], r0[iy + 1], fy), lerp(r1[iy], r1[iy + 1], fy), fx);
}
// t[(ix * ny + iy) * nz + iz]
NOVA_INLINE float lut3(const float *t, int nx, int ny, int nz, float x, float y, float z) {
    int ix;
    float fx;
    lutCoordinate(x, nx, ix, fx);
    return lerp(lut2(t + ix * ny * nz, ny, nz, y, z), lut2(t + (ix + 1) * ny * nz, ny, nz, y, z), fx);
}

// ---------------------------------------------------------------------------
// Albedos and compensation
// ---------------------------------------------------------------------------

// Albedo of GGX reflection with F = 1 at roughness index r, and its average.
NOVA_INLINE float ggxAlbedo(const BsdfTables &t, float r, float mu) {
    return lut2(t.ggxE, kLutRough, kLutMu, r, lutMu(mu));
}
NOVA_INLINE float ggxAlbedoAvg(const BsdfTables &t, float r) { return lut1(t.ggxEavg, kLutRough, r); }

// Single-scatter albedo of GGX reflection with a generalized-Schlick
// Fresnel of exponent 5 (F82-tint: b = f82Coefficient(), else 0): exact up
// to the clamp of F82 at 0.
NOVA_INLINE Spectrum ggxSchlickAlbedo(const BsdfTables &t, float r, float mu, Spectrum F0, Spectrum F90, Spectrum b) {
    int ix, iy;
    float fx, fy;
    lutCoordinate(r, kLutRough, ix, fx);
    lutCoordinate(lutMu(mu), kLutMu, iy, fy);
    const float *g = t.ggxABC;
    float abc[3];
    for (int c = 0; c < 3; ++c) {
        const float v00 = g[(ix * kLutMu + iy) * 3 + c], v01 = g[(ix * kLutMu + iy + 1) * 3 + c];
        const float v10 = g[((ix + 1) * kLutMu + iy) * 3 + c], v11 = g[((ix + 1) * kLutMu + iy + 1) * 3 + c];
        abc[c] = lerp(lerp(v00, v01, fy), lerp(v10, v11, fy), fx);
    }
    return F0 * abc[0] + F90 * abc[1] - b * abc[2];
}

// Factor restoring the multiply scattered energy of a reflection lobe with
// albedo E (F = 1) and average Eavg, for a Fresnel with average Favg.
NOVA_INLINE Spectrum msCompensation(float E, float Eavg, Spectrum Favg) {
    const Spectrum one = spectrum(1.0f);
    const Spectrum Fms = Favg * Eavg / spectrumMax(one - Favg * (1.0f - Eavg), 1e-4f);
    return one + Fms * ((1.0f - E) / nova::fmax(E, 1e-4f));
}

// Albedo of the white Charlie sheen lobe at sheen roughness r in [0, 1]
// (linear axis).
NOVA_INLINE float sheenAlbedo(const BsdfTables &t, float r, float mu) {
    return lut2(t.charlieE, kLutRough, kLutMu, r, lutMu(mu));
}

// E_R + E_T of a rough dielectric (energy units) for the relative index eta
// seen from wo (below 1 when wo is inside).
NOVA_INLINE float dielectricAlbedoRT(const BsdfTables &t, float eta, float r, float mu) {
    const bool inside = eta < 1.0f;
    const float z = lutEta(inside ? 1.0f / eta : eta);
    return lut3(inside ? t.dielRTinv : t.dielRT, kLutEta, kLutRough, kLutMu, z, r, lutMu(mu));
}
// E_R' + E_T' of a clear thin-walled sheet of index eta >= 1 (roughness
// index of its reflection alpha); 1 / thinDielectricAlbedo compensates the
// sheet.
NOVA_INLINE float thinDielectricAlbedo(const BsdfTables &t, float eta, float r, float mu) {
    return lut3(t.thinRT, kLutEta, kLutRough, kLutMu, lutEta(eta), r, lutMu(mu));
}
// E_R' alone: the clear sheet's reflection lobe (R' per microfacet, inter-
// reflections inside the sheet included). Integrated from the same samples
// as E_R' + E_T', so thinDielectricAlbedo - thinDielectricAlbedoR is the
// sheet's E_T' (both interpolate linearly on the same grid).
NOVA_INLINE float thinDielectricAlbedoR(const BsdfTables &t, float eta, float r, float mu) {
    return lut3(t.thinR, kLutEta, kLutRough, kLutMu, lutEta(eta), r, lutMu(mu));
}

// E_R of a rough dielectric seen from outside (eta >= 1).
NOVA_INLINE float dielectricAlbedoR(const BsdfTables &t, float eta, float r, float mu) {
    return lut3(t.dielR, kLutEta, kLutRough, kLutMu, lutEta(eta), r, lutMu(mu));
}

// Average 2 int F mu dmu of the exact dielectric Fresnel for any relative
// index; from inside by reciprocity: 1 - F_avg(1/eta) = (1 - F_avg(eta)) / eta^2.
NOVA_INLINE float dielectricFresnelAvg(const BsdfTables &t, float eta) {
    if (eta >= 1.0f)
        return lut1(t.fresnelAvg, kLutFresnelAvg, lutEta(eta));
    const float outside = lut1(t.fresnelAvg, kLutFresnelAvg, lutEta(1.0f / eta));
    return 1.0f - (1.0f - outside) * eta * eta;
}

} // namespace nova
