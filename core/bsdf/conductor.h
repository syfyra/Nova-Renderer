#pragma once

// Microfacet reflection with a configurable Fresnel term: an exact conductor
// (Standard Surface metal through the artistic IOR), the F82-tint model
// (OpenPBR metal), generalized Schlick (UsdPreviewSurface's specular
// workflow) or 1 (energy tables, tests). Rough lobes sample visible normals
// with the bounded cap; smooth ones are mirrors (delta).

#include "core/bsdf/fresnel.h"
#include "core/bsdf/microfacet.h"
#include "core/bsdf/thin_film.h"

namespace nova {

enum class FresnelModel : uint8_t {
    One,       // F = 1
    Conductor, // c0 = n, c1 = k
    F82Tint,   // c0 = F0, c1 = tint; F90 = 1, exponent 5
    Schlick,   // c0 = F0, c1 = F90, `exponent`
    Hoffman,   // c0 = F0, c1 = F82 tint, c2 = F90, `exponent` (MaterialX generalized_schlick_bsdf)
};

struct ConductorLobe {
    GgxDistribution ggx;
    FresnelModel fresnel;
    Spectrum c0, c1;
    float exponent;
    ThinFilm film;          // optional thin film on top (film.active())
    const float *xyzToRgb;  // the film's color conversion (kXyzToRec709Grey)
    Spectrum c2;            // Hoffman: F90 (last: the aggregate initializers of the other models leave it 0)

    NOVA_INLINE Spectrum F(float cosI) const {
        const Spectrum base = baseF(cosI);
        if (!film.active())
            return base;
        AiryBase b{};
        b.schlick = fresnel == FresnelModel::F82Tint || fresnel == FresnelModel::Schlick ||
                    fresnel == FresnelModel::Hoffman;
        b.n = c0;
        b.k = fresnel == FresnelModel::Conductor ? c1 : spectrum(0.0f);
        b.F0 = c0;
        b.tint = fresnel == FresnelModel::F82Tint || fresnel == FresnelModel::Hoffman ? c1 : spectrum(1.0f);
        b.exponent = fresnel == FresnelModel::Schlick || fresnel == FresnelModel::Hoffman ? exponent : 5.0f;
        return lerp(base, fresnelAiry(cosI, film, b, xyzToRgb), film.weight);
    }

    NOVA_INLINE Spectrum baseF(float cosI) const {
        switch (fresnel) {
        case FresnelModel::Conductor:
            return fresnelConductor(cosI, c0, c1);
        case FresnelModel::F82Tint:
            return fresnelF82Tint(cosI, c0, c1, 1.0f, 5.0f);
        case FresnelModel::Schlick: {
            const float w = std::pow(1.0f - saturate(cosI), exponent);
            return lerp(c0, c1, w);
        }
        case FresnelModel::Hoffman:
            return {fresnelF82Tint(cosI, c0.x, c1.x, c2.x, exponent), fresnelF82Tint(cosI, c0.y, c1.y, c2.y, exponent),
                    fresnelF82Tint(cosI, c0.z, c1.z, c2.z, exponent)};
        default:
            return spectrum(1.0f);
        }
    }

    // Reflection only: wo and wi above the surface.
    NOVA_INLINE Spectrum eval(Vec3f wo, Vec3f wi, float &pdf) const {
        pdf = 0.0f;
        if (ggx.smooth() || wo.z <= 0.0f || wi.z <= 0.0f)
            return {0.0f, 0.0f, 0.0f};
        const Vec3f m = normalize(wo + wi);
        const float om = dot(wo, m);
        if (om <= 0.0f)
            return {0.0f, 0.0f, 0.0f};
        pdf = ggx.pdfReflect(wo, m, true);
        return F(om) * (ggx.D(m) * ggx.vis(wo, wi) * wi.z);
    }

    NOVA_INLINE bool sample(Vec3f wo, Vec2f u, LobeSample &s) const {
        if (wo.z <= 0.0f)
            return false;
        s.transmit = false;
        if (ggx.smooth()) {
            s.wi = {-wo.x, -wo.y, wo.z};
            s.fcos = F(wo.z);
            s.pdf = 0.0f;
            s.delta = true;
            return true;
        }
        const Vec3f m = ggx.sampleVisible(wo, u, true);
        s.wi = reflect(wo, m);
        if (s.wi.z <= 0.0f)
            return false;
        s.delta = false;
        s.fcos = eval(wo, s.wi, s.pdf);
        return s.pdf > 0.0f;
    }
};

} // namespace nova
