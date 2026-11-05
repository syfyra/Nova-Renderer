#pragma once

// Fresnel reflectance: exact dielectric and conductor, generalized Schlick
// with Hoffman's F82 tint (OpenPBR metal), Gulbrandsen's artistic IOR
// (Standard Surface metal) and OpenPBR's specular_weight IOR modulation.
// Sources: MaterialX 1.39 mx_microfacet_specular.glsl, Hoffman 2023,
// Gulbrandsen 2014, OpenPBR 1.1 (docs/research/m3/microfacet.md section 3).

#include "core/bsdf/common.h"

namespace nova {

// Exact unpolarized dielectric Fresnel. cosI >= 0 is the cosine on the
// incident side, eta = n_transmitted / n_incident. Returns 1 on total
// internal reflection; cosT receives the cosine on the transmitted side.
NOVA_INLINE float fresnelDielectric(float cosI, float eta, float &cosT) {
    const float s2t = (1.0f - cosI * cosI) / (eta * eta);
    if (s2t >= 1.0f) {
        cosT = 0.0f;
        return 1.0f;
    }
    cosT = std::sqrt(1.0f - s2t);
    const float rs = (cosI - eta * cosT) / (cosI + eta * cosT);
    const float rp = (eta * cosI - cosT) / (eta * cosI + cosT);
    return 0.5f * (rs * rs + rp * rp);
}
NOVA_INLINE float fresnelDielectric(float cosI, float eta) {
    float cosT;
    return fresnelDielectric(cosI, eta, cosT);
}

// Exact conductor Fresnel for the relative complex index n + ik (Lagarde;
// MaterialX mx_fresnel_conductor).
NOVA_INLINE float fresnelConductor(float cosI, float n, float k) {
    const float c2 = cosI * cosI, s2 = 1.0f - c2, n2 = n * n, k2 = k * k, t0 = n2 - k2 - s2;
    const float a2b2 = std::sqrt(t0 * t0 + 4.0f * n2 * k2), t1 = a2b2 + c2;
    const float a = std::sqrt(nova::fmax(0.5f * (a2b2 + t0), 0.0f)), t2 = 2.0f * a * cosI;
    const float rs = (t1 - t2) / (t1 + t2);
    const float t3 = c2 * a2b2 + s2 * s2, t4 = t2 * s2;
    return 0.5f * (rs + rs * (t3 - t4) / (t3 + t4));
}
NOVA_INLINE Spectrum fresnelConductor(float cosI, Spectrum n, Spectrum k) {
    return {fresnelConductor(cosI, n.x, k.x), fresnelConductor(cosI, n.y, k.y), fresnelConductor(cosI, n.z, k.z)};
}

// Normal-incidence reflectance of a dielectric and its inverse.
NOVA_INLINE float iorToF0(float eta) { return sqr((eta - 1.0f) / (eta + 1.0f)); }
NOVA_INLINE float f0ToIor(float F0) {
    const float s = std::sqrt(nova::fmin(nova::fmax(F0, 0.0f), 0.9999f));
    return (1.0f + s) / (1.0f - s);
}

// Generalized Schlick: F0 + (F90 - F0) (1 - mu)^p.
NOVA_INLINE float fresnelSchlick(float mu, float F0, float F90, float p) {
    return lerp(F0, F90, std::pow(1.0f - saturate(mu), p));
}

// Hoffman's F82-tint model (MaterialX mx_fresnel_hoffman_schlick): the
// generalized Schlick curve with its value at mu = 1/7 multiplied by `tint`.
// OpenPBR metal: F0 = base color, tint = specular color, F90 = 1, p = 5.
// Clamped at 0, because the raw curve goes negative for small tints.
constexpr float kF82Mu = 1.0f / 7.0f;
constexpr float kF82Den = 0.0566528f; // kF82Mu (1 - kF82Mu)^6

NOVA_INLINE float f82Coefficient(float F0, float tint, float F90, float p) {
    return lerp(F0, F90, std::pow(1.0f - kF82Mu, p)) * (1.0f - tint) / kF82Den;
}
NOVA_INLINE float fresnelF82Tint(float mu, float F0, float tint, float F90, float p) {
    const float x = saturate(mu);
    return nova::fmax(lerp(F0, F90, std::pow(1.0f - x, p)) - f82Coefficient(F0, tint, F90, p) * x * pow6(1.0f - x),
                     0.0f);
}
NOVA_INLINE Spectrum fresnelF82Tint(float mu, Spectrum F0, Spectrum tint, float F90, float p) {
    return {fresnelF82Tint(mu, F0.x, tint.x, F90, p), fresnelF82Tint(mu, F0.y, tint.y, F90, p),
            fresnelF82Tint(mu, F0.z, tint.z, F90, p)};
}
// Hemispherical average 2 int F mu dmu of the F82-tint curve (exact for the
// unclamped curve; clamped at 0 like the curve).
NOVA_INLINE float fresnelF82TintAvg(float F0, float tint, float F90, float p) {
    return nova::fmax(lerp(F0, F90, 2.0f / ((p + 1.0f) * (p + 2.0f))) - f82Coefficient(F0, tint, F90, p) / 126.0f,
                     0.0f);
}

// Average Fresnel of a conductor without quadrature (Cycles): the F82-tint
// curve through F0 = F(1) and F82 = F(1/7), averaged.
NOVA_INLINE float conductorFresnelAvg(float n, float k) {
    const float F0 = fresnelConductor(1.0f, n, k), F82 = fresnelConductor(kF82Mu, n, k);
    const float schlick82 = fresnelSchlick(kF82Mu, F0, 1.0f, 5.0f);
    return nova::fmin(nova::fmax(lerp(F0, 1.0f, 1.0f / 21.0f) - (schlick82 - F82) / (kF82Den * 126.0f), 0.0f), 1.0f);
}
NOVA_INLINE Spectrum conductorFresnelAvg(Spectrum n, Spectrum k) {
    return {conductorFresnelAvg(n.x, k.x), conductorFresnelAvg(n.y, k.y), conductorFresnelAvg(n.z, k.z)};
}

// Gulbrandsen 2014 (MaterialX mx_artistic_ior): the complex index whose
// normal-incidence reflectance is `reflectivity` and whose grazing tint is
// `edge`. Reflectivity is clamped to 0.99.
NOVA_INLINE void artisticIor(float reflectivity, float edge, float &n, float &k) {
    const float r = nova::fmin(nova::fmax(reflectivity, 0.0f), 0.99f), sr = std::sqrt(r);
    n = lerp((1.0f + sr) / (1.0f - sr), (1.0f - r) / (1.0f + r), edge);
    k = std::sqrt(nova::fmax((sqr(n + 1.0f) * r - sqr(n - 1.0f)) / (1.0f - r), 0.0f));
}
NOVA_INLINE void artisticIor(Spectrum reflectivity, Spectrum edge, Spectrum &n, Spectrum &k) {
    artisticIor(reflectivity.x, edge.x, n.x, k.x);
    artisticIor(reflectivity.y, edge.y, n.y, k.y);
    artisticIor(reflectivity.z, edge.z, n.z, k.z);
}

// OpenPBR specular_weight: scales the normal-incidence reflectance of the
// relative index etaS (> 0) by `weight`, keeping the side of 1.
NOVA_INLINE float openPbrModulatedEta(float etaS, float weight) {
    const float F0 = iorToF0(etaS);
    const float e = std::copysign(std::sqrt(nova::fmin(nova::fmax(weight * F0, 0.0f), 0.99999f)), etaS - 1.0f);
    return (1.0f + e) / (1.0f - e);
}

} // namespace nova
