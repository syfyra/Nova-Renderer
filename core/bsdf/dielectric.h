#pragma once

// Dielectric interfaces (Walter et al. 2007 with visible-normal sampling;
// docs/research/m3/microfacet.md section 2): rough or perfectly smooth,
// reflection only (a specular or coat layer over a base), transmission only
// (the transmitted share of a split interface) or both (glass). The
// transmission is in radiance units (divided by the squared relative index),
// as camera paths need it. Single scattering only: the closure scales the
// lobe by its energy compensation (core/bsdf/energy.h).
//
// The lobe works in a frame where wo lies above the surface: from below the
// interface is seen with the inverse index. Thin-walled sheets (no bending,
// no medium) are ThinDielectricLobe below.

#include "core/bsdf/fresnel.h"
#include "core/bsdf/microfacet.h"
#include "core/bsdf/thin_film.h"

namespace nova {

// Interfaces closer to index 1 are index-matched: nothing reflects and light
// passes straight through (a delta event at any roughness). The generalized
// half vector of the rough transmission degenerates there.
constexpr float kIndexMatched = 1e-4f;

struct DielectricLobe {
    enum Mode : uint8_t { kReflect = 1, kTransmit = 2, kBoth = 3 };

    GgxDistribution ggx;
    float eta;         // n_inside / n_outside; the inside is below (-z)
    Spectrum tintR;    // multiplies the reflection (e.g. specular_color)
    Spectrum tintT;    // multiplies the transmission (e.g. transmission_color)
    uint8_t mode;
    // Karma's internalreflectenable off (dropInternalReflection sets it on
    // the inside's lobes): no Fresnel reflection, the interface transmits
    // without the Fresnel loss; total internal reflection stays (rough glass
    // keeps its energy, as in Karma's references). Aggregate initializers
    // leave it 0.
    uint8_t noInternal;
    ThinFilm film;          // optional film on the outside (film.active())
    const float *xyzToRgb;  // the film's color conversion
    // Nested dielectrics (core/integrator/nested.h, docs/design/M11-P9b.md):
    // the ratio of the indices of the media on both sides (eta's convention),
    // which bends the transmission and alone decides total internal
    // reflection; 0: eta itself. The reflectance keeps the surface's own
    // index, as in Karma (measured): its outside value at the microfacet
    // angle. Last and 0-neutral, so aggregate initializers leave it unset.
    float etaMedia;

    // The index ratio that bends the transmission.
    NOVA_INLINE float mediaEta() const { return etaMedia > 0.0f ? etaMedia : eta; }

    // Reflection-only lobes sample with the bounded cap.
    NOVA_INLINE bool bounded() const { return mode == kReflect; }

    // The scalar reflectance at the microfacet cosine om and the cosine of
    // the transmission: exact Fresnel for the relative index e (seen from
    // wo); between nested media (index ratio g) the outside reflectance of
    // the surface's own index, with total reflection only where g has it.
    NOVA_INLINE float reflectance(float om, float e, float g, float &cosT) const {
        if (!(etaMedia > 0.0f))
            return fresnelDielectric(om, e, cosT);
        const float s2t = (1.0f - om * om) / (g * g);
        if (s2t >= 1.0f) {
            cosT = 0.0f;
            return 1.0f;
        }
        cosT = std::sqrt(1.0f - s2t);
        return fresnelDielectric(om, e >= 1.0f ? e : 1.0f / e);
    }

    // The reflectance at the microfacet cosine om for the relative index e
    // (seen from wo): exact Fresnel, blended with the film's when wo is on
    // the outside. F is the scalar for refraction, the result per channel.
    NOVA_INLINE Spectrum fresnel(float om, float e, float F) const {
        if (!film.active() || e < 1.0f)
            return spectrum(F);
        AiryBase b{};
        b.n = spectrum(e);
        b.k = spectrum(0.0f);
        return lerp(spectrum(F), fresnelAiry(om, film, b, xyzToRgb), film.weight);
    }

    // Probability of choosing reflection given the reflectance Fs:
    // proportional to the tinted reflected and transmitted energy.
    NOVA_INLINE float reflectProbability(Spectrum Fs) const {
        if (mode == kReflect)
            return 1.0f;
        if (mode == kTransmit)
            return 0.0f;
        const float r = maxComponent(Fs * tintR), t = maxComponent((spectrum(1.0f) - Fs) * tintT);
        return r + t > 0.0f ? r / (r + t) : 0.0f;
    }

    NOVA_INLINE Spectrum eval(Vec3f wo, Vec3f wi, float &pdf) const {
        pdf = 0.0f;
        const Spectrum black{0.0f, 0.0f, 0.0f};
        float e = eta, g = mediaEta();
        if (wo.z < 0.0f) {
            wo.z = -wo.z;
            wi.z = -wi.z;
            e = 1.0f / e;
            g = 1.0f / g;
        }
        if (ggx.smooth() || wo.z == 0.0f || wi.z == 0.0f || std::fabs(g - 1.0f) < kIndexMatched)
            return black;
        const bool reflect = wi.z > 0.0f;
        if (!(mode & (reflect ? kReflect : kTransmit)))
            return black;
        const bool noReflection = noInternal != 0;
        const float etap = reflect ? 1.0f : g;
        Vec3f m = wo + wi * etap;
        if (dot(m, m) == 0.0f)
            return black;
        m = normalize(m);
        if (m.z < 0.0f)
            m = -m;
        const float om = dot(wo, m), im = dot(wi, m);
        if (om <= 0.0f || im * wi.z <= 0.0f)
            return black; // back-facing microfacet
        float cosT;
        const float F = reflectance(om, e, g, cosT);
        const Spectrum Fs = noReflection ? spectrum(F >= 1.0f ? 1.0f : 0.0f) : fresnel(om, e, F);
        const float pR = reflectProbability(Fs);
        if (reflect) {
            pdf = ggx.pdfReflect(wo, m, bounded()) * pR;
            return tintR * Fs * (ggx.D(m) * ggx.vis(wo, wi) * wi.z);
        }
        const float denom = sqr(im + om / etap);
        if (!(denom > 0.0f))
            return black;
        pdf = ggx.pdfVisible(wo, m) * std::fabs(im) / denom * (1.0f - pR);
        const float f = ggx.D(m) * ggx.G2(wo, wi) * std::fabs(im * om / (wi.z * wo.z * denom));
        return tintT * (spectrum(1.0f) - Fs) * (f * std::fabs(wi.z) / (etap * etap));
    }

    // u: the microfacet, uc: reflection vs transmission.
    NOVA_INLINE bool sample(Vec3f wo, Vec2f u, float uc, LobeSample &s) const {
        const bool flip = wo.z < 0.0f;
        const float e = flip ? 1.0f / eta : eta;
        const float g = flip ? 1.0f / mediaEta() : mediaEta();
        const Vec3f w{wo.x, wo.y, std::fabs(wo.z)};
        if (w.z == 0.0f)
            return false;
        if (std::fabs(g - 1.0f) < kIndexMatched) { // straight through, F = 0
            if (!(mode & kTransmit))
                return false;
            s.wi = -wo;
            s.transmit = true;
            s.delta = true;
            s.pdf = 0.0f;
            s.fcos = tintT * (1.0f / (g * g));
            return true;
        }
        const Vec3f m = ggx.smooth() ? Vec3f{0.0f, 0.0f, 1.0f} : ggx.sampleVisible(w, u, bounded());
        const float om = dot(w, m);
        if (om <= 0.0f)
            return false;
        float cosT;
        const float F = reflectance(om, e, g, cosT);
        const bool noReflection = noInternal != 0;
        const Spectrum Fs = noReflection ? spectrum(F >= 1.0f ? 1.0f : 0.0f) : fresnel(om, e, F);
        const float pR = reflectProbability(Fs);
        Vec3f wi;
        if (uc < pR) {
            wi = reflect(w, m);
            if (wi.z <= 0.0f)
                return false;
            s.transmit = false;
        } else {
            if (F >= 1.0f || !(pR < 1.0f))
                return false;
            wi = refract(w, m, g, om, cosT);
            if (wi.z >= 0.0f)
                return false;
            s.transmit = true;
        }
        s.wi = flip ? mirrorZ(wi) : wi;
        if (ggx.smooth()) {
            s.delta = true;
            s.pdf = 0.0f;
            s.fcos = s.transmit ? tintT * (spectrum(1.0f) - Fs) * (1.0f / ((1.0f - pR) * g * g)) : tintR * Fs * (1.0f / pR);
            return true;
        }
        s.delta = false;
        s.fcos = eval(wo, s.wi, s.pdf);
        return s.pdf > 0.0f;
    }
};

// A thin-walled dielectric sheet (OpenPBR thin_walled; Cycles' thin glass):
// both interfaces at once, light passes straight through without bending
// and without entering a medium. Per microfacet with front reflectance R and
// T = 1 - R, and the transmission color c_t = tint^(1 / cos_t) along the
// refracted path inside the sheet:
//   T' = c_t T^2 / (1 - (R c_t)^2),  R' = R + T' R c_t,
// so R' + T' = 1 for a clear sheet. Reflection is a GGX lobe with R'; the
// transmission is the mirrored reflection lobe with T' and the roughness
// widened for the two interfaces (Kulla & Conty 2017, alpha_t below).
struct ThinDielectricLobe {
    GgxDistribution ggxR, ggxT;
    float eta;      // index of the sheet relative to the outside
    Spectrum tintR; // multiplies R'
    Spectrum tintT; // transmission color at normal incidence
    // T' is scaled by 1 + tBoost after the absorption: the energy a reflection
    // below full strength gives up (Standard Surface specular < 1). Not part
    // of tintT, which the absorption clamps to [0, 1]. tBoostSmooth is the
    // boost of the same sheet without roughness at the closure's wo (fake
    // caustics and the albedo AOV ignore the roughness). Last and 0-neutral,
    // so aggregate initializers without them leave T' as is (no default
    // member initializers: the lobe lives in Lobe's union).
    float tBoost, tBoostSmooth;

    // Roughness of the transmission through both interfaces.
    NOVA_INLINE static float transmissionAlpha(float alpha, float eta) {
        return saturate(alpha * std::sqrt(nova::fmax(3.4f * (eta - 1.0f) * sqr(eta - 0.5f) / (eta * eta * eta), 0.0f)));
    }

    // The boost for a reflection weight s when the white sheet reflects rw
    // and transmits tw (energies): s rw + (1 + boost) tw = rw + tw, the
    // transmission takes what the weakened reflection gives up (never
    // below 0 for s > 1).
    NOVA_INLINE static float boostFor(float rw, float tw, float s) {
        return nova::fmax(rw + tw - s * rw, 0.0f) / nova::fmax(tw, 1e-4f) - 1.0f;
    }

    // R' and T' at the microfacet cosine c.
    NOVA_INLINE void sheet(float c, Spectrum &R, Spectrum &T) const {
        float cosT;
        const float r = fresnelDielectric(c, eta, cosT), t = 1.0f - r;
        const float invCos = 1.0f / nova::fmax(cosT, 1e-4f);
        const Spectrum ct{std::pow(saturate(tintT.x), invCos), std::pow(saturate(tintT.y), invCos),
                          std::pow(saturate(tintT.z), invCos)};
        auto through = [&](float c) {
            const float d = 1.0f - sqr(r * c); // 0 at grazing total reflection
            return d > 0.0f ? c * t * t / d : 0.0f;
        };
        T = {through(ct.x), through(ct.y), through(ct.z)};
        R = (spectrum(r) + T * ct * r) * tintR;
        if (tBoost != 0.0f) // after R: the sheet's internal reflections are unchanged
            T *= 1.0f + tBoost;
    }

    // Probability of reflection: from the sheet at the macro cosine, so it
    // depends on wo only.
    NOVA_INLINE float reflectProbability(float muO) const {
        Spectrum R, T;
        sheet(muO, R, T);
        const float r = maxComponent(R), t = maxComponent(T);
        return r + t > 0.0f ? r / (r + t) : 0.0f;
    }

    NOVA_INLINE Spectrum eval(Vec3f wo, Vec3f wi, float &pdf) const {
        pdf = 0.0f;
        if (wo.z < 0.0f) { // the sheet is symmetric
            wo.z = -wo.z;
            wi.z = -wi.z;
        }
        const bool transmit = wi.z < 0.0f;
        const GgxDistribution &g = transmit ? ggxT : ggxR;
        if (g.smooth() || wo.z == 0.0f || wi.z == 0.0f)
            return {0.0f, 0.0f, 0.0f};
        const Vec3f wr = transmit ? mirrorZ(wi) : wi;
        const Vec3f m = normalize(wo + wr);
        const float om = dot(wo, m);
        if (om <= 0.0f || dot(wr, m) <= 0.0f)
            return {0.0f, 0.0f, 0.0f};
        Spectrum R, T;
        sheet(om, R, T);
        const float pR = reflectProbability(wo.z);
        pdf = g.pdfReflect(wo, m, true) * (transmit ? 1.0f - pR : pR);
        return (transmit ? T : R) * (g.D(m) * g.vis(wo, wr) * wr.z);
    }

    NOVA_INLINE bool sample(Vec3f wo, Vec2f u, float uc, LobeSample &s) const {
        const bool flip = wo.z < 0.0f;
        const Vec3f w{wo.x, wo.y, std::fabs(wo.z)};
        if (w.z == 0.0f)
            return false;
        const float pR = reflectProbability(w.z);
        const bool transmit = !(uc < pR);
        const GgxDistribution &g = transmit ? ggxT : ggxR;
        const Vec3f m = g.smooth() ? Vec3f{0.0f, 0.0f, 1.0f} : g.sampleVisible(w, u, true);
        const Vec3f wr = reflect(w, m);
        if (wr.z <= 0.0f || dot(w, m) <= 0.0f)
            return false;
        const Vec3f wi = transmit ? mirrorZ(wr) : wr;
        s.wi = flip ? mirrorZ(wi) : wi;
        s.transmit = transmit;
        if (g.smooth()) {
            Spectrum R, T;
            sheet(w.z, R, T);
            s.delta = true;
            s.pdf = 0.0f;
            s.fcos = transmit ? T * (1.0f / (1.0f - pR)) : R * (1.0f / pR);
            return true;
        }
        s.delta = false;
        s.fcos = eval(wo, s.wi, s.pdf);
        return s.pdf > 0.0f;
    }
};

} // namespace nova
