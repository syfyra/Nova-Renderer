#pragma once

// buildClosure(): the surface models as weighted lobe lists
// (docs/design/M3.md section 5; closed forms in
// docs/research/m3/surface-models.md 2.3 and 3.4). Layers scale everything
// below them by one minus their albedo at wo (MaterialX's albedo scaling,
// untinted, as Karma measured); mixes split the weight. Energy compensation:
// conductors by Turquin/Kulla-Conty, dielectric interfaces by 1 / E_RT with
// E_R / E_RT as the albedo of a reflection layer, thin sheets by their own
// tables (core/bsdf/energy.h). Included by core/bsdf/surface.h.
//
// Orientation: every frame faces wo (opaque materials are two-sided);
// refraction sees the relative index from wo's side of the authored
// geometric normal. Seen from inside a refractive material, only the
// interface (with 1/eta) and its opaque remainder scatter: layers on top of
// the dielectric (coat, sheen, fuzz) are on the outside.

#include "core/bsdf/shading_frame.h"
#include "core/bsdf/surface.h"
#include "core/bsdf/thin_film.h"

namespace nova {

namespace closure_detail {

constexpr float kCut = 1e-5f;

NOVA_INLINE Spectrum spectrumPow(Spectrum c, float e) {
    return {std::pow(nova::fmax(c.x, 0.0f), e), std::pow(nova::fmax(c.y, 0.0f), e), std::pow(nova::fmax(c.z, 0.0f), e)};
}
NOVA_INLINE Spectrum clamp01(Spectrum c) { return {saturate(c.x), saturate(c.y), saturate(c.z)}; }
NOVA_INLINE Spectrum nonNegative(Spectrum c) { return spectrumMax(c, 0.0f); }
// Emission is not clamped: negative values subtract, as in Karma (utility
// and unlit passes rely on it); only non-finite values become 0.
NOVA_INLINE float finiteOr0(float x) { return isFinite(x) ? x : 0.0f; }
NOVA_INLINE Spectrum emissionOf(const SurfaceInputs &in) {
    const Spectrum c = in.c(surface_field::emissionColor);
    return Spectrum{finiteOr0(c.x), finiteOr0(c.y), finiteOr0(c.z)} * finiteOr0(in.f(surface_field::emissionWeight));
}
NOVA_INLINE float average(Spectrum c) { return (c.x + c.y + c.z) * (1.0f / 3.0f); }

// A unit vector from an input, or the fallback when the input is unset (0).
NOVA_INLINE Vec3f inputDirection(Vec3f v, Vec3f fallback) {
    const float l2 = dot(v, v);
    return l2 > 1e-12f ? v * (1.0f / std::sqrt(l2)) : fallback;
}

// The frame of a set of lobes: normal n turned to wo's side of the geometric
// normal and bent so mirror reflections of wo stay above the surface; x
// along the tangent rotated by `turns` (Standard Surface: rotation * 360
// degrees, MaterialX rotate3d T cos + (T x N) sin).
NOVA_INLINE Frame lobeFrame(Vec3f n, Vec3f tangent, bool hasTangent, float turns, Vec3f ngf, Vec3f wo) {
    if (dot(n, ngf) < 0.0f)
        n = -n;
    n = ensureValidSpecularReflection(ngf, wo, n);
    Vec3f t = tangent - n * dot(n, tangent);
    if (!hasTangent || !(dot(t, t) > 1e-12f))
        return Frame::fromNormal(n);
    t = normalize(t);
    if (turns != 0.0f) {
        const float a = kTwoPi * turns;
        t = normalize(t * std::cos(a) + cross(t, n) * std::sin(a));
    }
    return Frame{t, cross(n, t), n};
}

// Albedo and compensation of a reflection-only dielectric interface with
// relative index eta (the lobe weight gets `compensation`, the layers below
// 1 - weight * albedo, albedo already compensated).
struct ReflectionEnergy {
    float singleScatter;  // E_R
    float compensation;   // 1 / E_RT, or the Turquin factor for eta < 1
    float albedo;         // compensated: E_R / E_RT
};
NOVA_INLINE ReflectionEnergy dielectricReflectionEnergy(const BsdfTables &t, const GgxDistribution &g, float eta,
                                                        float mu) {
    ReflectionEnergy e;
    if (g.smooth()) {
        e.singleScatter = e.albedo = fresnelDielectric(mu, eta);
        e.compensation = 1.0f;
        return e;
    }
    const float r = lutRough(g.ax, g.ay);
    if (eta >= 1.0f) {
        const float ert = dielectricAlbedoRT(t, eta, r, mu);
        e.singleScatter = dielectricAlbedoR(t, eta, r, mu);
        e.compensation = 1.0f / nova::fmax(ert, 1e-4f);
    } else { // e.g. specular_IOR 0 (F = 1): Schlick-shaped albedo with conductor-style compensation
        const float F0 = iorToF0(eta);
        e.singleScatter = ggxSchlickAlbedo(t, r, mu, spectrum(F0), spectrum(1.0f), spectrum(0.0f)).x;
        e.compensation =
            msCompensation(ggxAlbedo(t, r, mu), ggxAlbedoAvg(t, r), spectrum(lerp(F0, 1.0f, 1.0f / 21.0f))).x;
    }
    e.albedo = nova::fmin(e.singleScatter * e.compensation, 1.0f);
    return e;
}

NOVA_INLINE Lobe *pushLambert(ClosureList &cl, uint8_t frame, Spectrum weight, Spectrum albedo, bool transmit) {
    Lobe *l = cl.push(transmit ? LobeType::Translucent : LobeType::Lambert, frame, weight,
                      luminance(weight * albedo), true);
    if (l) {
        l->lambert = LambertLobe{albedo, transmit};
        if (transmit)
            cl.addAlbedo(*l, spectrum(0.0f), weight * albedo);
        else
            cl.addAlbedo(*l, weight * albedo);
    }
    return l;
}

// A random-walk subsurface lobe (docs/design/M9.md): `color` the target
// albedo (clamped to [0, 1]), `mfp` the mean free path per channel. Its pick
// estimate and albedo are weight x color, as the Lambert lobe it replaces.
NOVA_INLINE Lobe *pushSubsurface(ClosureList &cl, uint8_t frame, Spectrum weight, Spectrum color, Spectrum mfp,
                                 float anisotropy) {
    const Spectrum albedo = clamp01(color);
    Lobe *l = cl.push(LobeType::Subsurface, frame, weight, luminance(weight * albedo), true);
    if (l) {
        l->subsurface = SubsurfaceLobe::make(albedo, mfp, anisotropy);
        l->sss = true;
        cl.addAlbedo(*l, weight * albedo);
    }
    return l;
}

// The fibre frame of a hit (core/bsdf/hair.h): x along the tangent t, z =
// wo projected into the normal plane; h = n . (z x t), the offset across
// (n: the tube's outward normal). Returns false if wo runs along the fibre.
NOVA_INLINE Frame hairFrame(Vec3f n, Vec3f t, Vec3f wo, float &h) {
    Vec3f z = wo - t * dot(wo, t);
    if (!(dot(z, z) > 1e-12f))
        z = n - t * dot(n, t);
    z = normalize(z);
    const Vec3f y = cross(z, t);
    h = nova::fmin(nova::fmax(dot(n, y), -1.0f), 1.0f);
    return Frame{t, y, z};
}

// Pushes the four lobes of a fibre (weights tint_p x A_p x `scale`) in frame
// `frame`; the R lobe's albedo counts as reflection, the rest as
// transmission (the side most of it goes to).
NOVA_INLINE void pushHair(ClosureList &cl, uint8_t frame, const HairFibre &fibre, const Spectrum tint[4], Spectrum scale) {
    for (int p = 0; p < 4; ++p) {
        const Spectrum weight = tint[p] * fibre.A[p] * scale;
        if (Lobe *l = cl.push(LobeType::Hair, frame, weight, luminance(weight), false)) {
            l->hair = fibre.lobe[p];
            if (p == 0)
                cl.addAlbedo(*l, weight);
            else
                cl.addAlbedo(*l, spectrum(0.0f), weight);
        }
    }
}

// Karma Hair (docs/design/M9.md D6, measured research 3.4-3.5): the
// absorption adds the explicit one, Chiang's inversion of base x baseColor
// and the melanin term thicknessScale (-ln(1 - m)) [(1 - r) 1.2085 eu + r
// 1.833 pheo]; tint scales the fibre; diffuse mixes in a Lambert on the tube
// normal. The randomize controls use the strand's random number (Karma's
// hash is unknown: statistically alike).
NOVA_INLINE void buildHair(const SurfaceInputs &in, const ShadingGeometry &g, Vec3f wo, ClosureList &cl) {
    namespace sf = surface_field;
    const Vec3f ngf = dot(wo, g.ng) < 0.0f ? -g.ng : g.ng;
    const Vec3f n = dot(g.n, g.n) > 0.0f ? normalize(g.n) : ngf;
    const Vec3f t = g.hasTangent ? normalize(g.t - n * dot(n, g.t)) : Frame::fromNormal(n).t;
    float h = 0.0f;
    cl.frame[0] = hairFrame(n, t, wo, h);
    cl.frame[1] = Frame::fromNormal(dot(n, ngf) < 0.0f ? -n : n);
    const float sinO = nova::fmin(nova::fmax(dot(wo, t), -1.0f), 1.0f);
    const float cosO = std::sqrt(nova::fmax(0.0f, 1.0f - sinO * sinO));

    const float r = g.random;
    const float melanin = saturate(in.f(sf::hairMelanin)) * (1.0f - saturate(in.f(sf::hairMelaninRandomize)) * r);
    const float rough = saturate(in.f(sf::hairRoughness));
    const float roughRandom = 1.0f + saturate(in.f(sf::hairRoughnessRandomize)) * (r - 0.5f);
    const float betaM = saturate(rough * roughRandom);
    const float betaN = saturate((in.f(sf::hairRoughnessAnisotropic) > 0.5f ? in.f(sf::hairRoughnessAzimuthal) : rough) *
                                 roughRandom);
    const float redness = saturate(in.f(sf::hairMelaninRedness));
    const float L = -std::log(nova::fmax(1.0f - melanin, 1e-4f)) * nova::fmax(in.f(sf::hairThicknessScale), 0.0f);
    const Spectrum eumelanin{0.419f, 0.697f, 1.37f}, pheomelanin{0.187f, 0.4f, 1.05f};
    const Spectrum sigmaA = nonNegative(in.c(sf::hairAbsorption)) +
                            hairAbsorptionFromColor(clamp01(in.c(sf::hairBaseColor) * in.f(sf::hairBase)), betaN) +
                            (eumelanin * ((1.0f - redness) * 1.2085f) + pheomelanin * (redness * 1.833f)) * L;
    const bool advanced = in.f(sf::hairRoughnessAdvanced) > 0.5f;
    const float vR = hair_detail::longitudinalVariance(betaM);
    const float v[3] = {vR, vR * (advanced ? nova::fmax(in.f(sf::hairTtScale), 0.0f) : 0.25f),
                        vR * (advanced ? nova::fmax(in.f(sf::hairTrtScale), 0.0f) : 4.0f)};
    const float sN = hair_detail::azimuthalScale(betaN);
    const float s[3] = {sN, sN, sN};
    // Positive shift tilts the R highlight toward the root (Karma, measured).
    const float alpha = -in.f(sf::hairShift) * kHalfPi;
    const HairFibre fibre = hairFibre(in.f(sf::hairIor), sigmaA, v, s, alpha, sinO, cosO, kHalfPi, h);
    const float d = saturate(in.f(sf::hairDiffuse));
    const Spectrum tint = nonNegative(in.c(sf::hairTint));
    const Spectrum tints[4] = {tint, tint, tint, tint};
    if (d < 1.0f)
        pushHair(cl, 0, fibre, tints, spectrum(1.0f - d));
    if (d > kCut)
        pushLambert(cl, 1, spectrum(d), nonNegative(in.c(sf::hairDiffuseColor)), false);
    cl.finalize();
}

// Labels the lobes pushed since index `from` (coats, for light path
// expressions; a replaced lobe of a full list may keep another label).
NOVA_INLINE void labelLobes(ClosureList &cl, int from, LobeLabel label) {
    for (int i = from; i < cl.count; ++i)
        cl.lobe[i].label = label;
}

// A reflection-only dielectric layer (coat, specular): pushes the lobe with
// `weight` and returns the albedo the layers below lose (compensated,
// spectral with a thin film).
NOVA_INLINE Spectrum pushDielectricLayer(ClosureList &cl, const ClosureContext &ctx, uint8_t frame, Spectrum weight,
                                         const GgxDistribution &g, float eta, float mu, const ThinFilm &film) {
    const ReflectionEnergy e = dielectricReflectionEnergy(*ctx.tables, g, eta, mu);
    Spectrum albedo = spectrum(e.albedo);
    if (film.weight > 0.0f && film.thicknessNm > 0.0f) {
        // MaterialX's albedo of a film-coated lobe: between the mirror at wo
        // and a rough lobe with the film's normal-incidence color.
        AiryBase base{};
        base.n = spectrum(eta);
        base.k = spectrum(0.0f);
        const Spectrum airyMu = fresnelAiry(mu, film, base, ctx.xyzToRgb);
        const Spectrum airy1 = fresnelAiry(1.0f, film, base, ctx.xyzToRgb);
        const float r = lutRough(g.ax, g.ay);
        const Spectrum rough = g.smooth() ? airy1
                                          : ggxSchlickAlbedo(*ctx.tables, r, mu, airy1, spectrum(1.0f), spectrum(0.0f)) *
                                                e.compensation;
        albedo = lerp(albedo, clamp01(lerp(airyMu, rough, std::sqrt(nova::fmax(g.ax, g.ay)))), film.weight);
    }
    Lobe *l = cl.push(LobeType::Dielectric, frame, weight * e.compensation, luminance(weight * albedo), false);
    if (l) {
        l->dielectric = DielectricLobe{g, eta, spectrum(1.0f), spectrum(1.0f), DielectricLobe::kReflect};
        l->dielectric.film = film;
        l->dielectric.xyzToRgb = ctx.xyzToRgb;
        cl.addAlbedo(*l, weight * albedo);
    }
    return albedo;
}

// The transmitted share of a dielectric interface (weight already includes
// it): a refracting interface (with its reflection: one R+T lobe; with a
// separate transmission roughness: R and T lobes) or a thin-walled sheet.
// tintR multiplies the reflection, tintT the transmission; the
// transmission is normalized so a white interface conserves energy.
// reflectWeight (thin sheets only): the reflection is scaled by this weight
// (Standard Surface `specular`, part of tintR) and the sheet's transmission
// takes the energy the reflection gives up, so the sheet keeps the energy of
// reflectWeight 1: from the white sheet's E_R' and E_T' at mu (tables of
// this lobe; for a smooth sheet its R'/T' split, exact).
NOVA_INLINE void pushTransmission(ClosureList &cl, const ClosureContext &ctx, uint8_t frame, Spectrum weight,
                                  const GgxDistribution &gR, const GgxDistribution &gT, float eta, bool thin,
                                  Spectrum tintR, Spectrum tintT, float mu, float reflectWeight = 1.0f) {
    const BsdfTables &t = *ctx.tables;
    if (thin) {
        const float e1 = nova::fmax(eta, 1.0f), rIdx = lutRough(gR.ax, gR.ay);
        const float e = gR.smooth() ? 1.0f : thinDielectricAlbedo(t, e1, rIdx, mu);
        Lobe *l = cl.push(LobeType::ThinDielectric, frame, weight * (1.0f / nova::fmax(e, 1e-4f)), luminance(weight), false);
        if (l) {
            l->thin = ThinDielectricLobe{gR,
                                         GgxDistribution::make(ThinDielectricLobe::transmissionAlpha(gR.ax, e1),
                                                               ThinDielectricLobe::transmissionAlpha(gR.ay, e1)),
                                         e1, tintR, tintT};
            l->thin.tBoost = l->thin.tBoostSmooth = 0.0f;
            Spectrum R, T, Rw, Tw;
            ThinDielectricLobe white = l->thin;
            white.tintR = white.tintT = spectrum(1.0f);
            white.sheet(mu, Rw, Tw);
            if (reflectWeight != 1.0f) {
                // T' (1 + tBoost) = E - reflectWeight E_R' for the white
                // sheet. Smooth: one event per sample, its R'/T' split at mu
                // (exact). Rough: the boost scales every microfacet's T', so
                // it comes from the sheet's directional albedos E_R' and
                // E_T' = E - E_R' (both integrated from this lobe), exact up
                // to the tables' interpolation.
                l->thin.tBoostSmooth = ThinDielectricLobe::boostFor(average(Rw), average(Tw), reflectWeight);
                if (gR.smooth()) {
                    l->thin.tBoost = l->thin.tBoostSmooth;
                } else {
                    const float rw = thinDielectricAlbedoR(t, e1, rIdx, mu);
                    l->thin.tBoost = ThinDielectricLobe::boostFor(rw, e - rw, reflectWeight);
                }
            }
            // The albedo AOV with the tints: the tinted sheet at mu (roughness
            // ignored, so with the smooth sheet's boost) relative to the white
            // one without the boost (the compensation's reference).
            ThinDielectricLobe aov = l->thin;
            aov.tBoost = l->thin.tBoostSmooth;
            aov.sheet(mu, R, T);
            const float norm = 1.0f / nova::fmax(average(Rw + Tw), 1e-6f);
            cl.addAlbedo(*l, weight * R * norm, weight * T * norm);
        }
        return;
    }
    const float rR = lutRough(gR.ax, gR.ay);
    const float ertR = gR.smooth() ? 1.0f : dielectricAlbedoRT(t, eta, rR, mu);
    if (gR.ax == gT.ax && gR.ay == gT.ay) {
        Lobe *l = cl.push(LobeType::Dielectric, frame, weight * (1.0f / nova::fmax(ertR, 1e-4f)), luminance(weight), false);
        if (l) {
            l->dielectric = DielectricLobe{gR, eta, tintR, tintT, DielectricLobe::kBoth};
            l->dielectric.film = ThinFilm{};
            l->dielectric.xyzToRgb = ctx.xyzToRgb;
            // The albedo AOV with the tints: the interface's reflected share
            // (compensated) and the rest transmitted.
            const float shareR = gR.smooth() ? fresnelDielectric(mu, eta)
                                             : (eta >= 1.0f ? dielectricAlbedoR(t, eta, rR, mu) : 0.0f) / nova::fmax(ertR, 1e-4f);
            cl.addAlbedo(*l, weight * tintR * shareR, weight * tintT * (1.0f - shareR));
        }
        return;
    }
    // Standard Surface transmission_extra_roughness: R with alpha, T with
    // alphaT; each normalized by its own share of the interface's energy.
    const float erR = gR.smooth() ? fresnelDielectric(mu, eta) : (eta >= 1.0f ? dielectricAlbedoR(t, eta, rR, mu) : 0.0f);
    const float rT = lutRough(gT.ax, gT.ay);
    const float ertT = gT.smooth() ? 1.0f : dielectricAlbedoRT(t, eta, rT, mu);
    const float erT = gT.smooth() ? fresnelDielectric(mu, eta) : (eta >= 1.0f ? dielectricAlbedoR(t, eta, rT, mu) : 0.0f);
    const float shareR = erR / nova::fmax(ertR, 1e-4f); // compensated reflection of the interface
    if (Lobe *l = cl.push(LobeType::Dielectric, frame, weight * tintR * (1.0f / nova::fmax(ertR, 1e-4f)),
                          luminance(weight * tintR) * shareR, false)) {
        l->dielectric = DielectricLobe{gR, eta, spectrum(1.0f), spectrum(1.0f), DielectricLobe::kReflect};
        l->dielectric.film = ThinFilm{};
        l->dielectric.xyzToRgb = ctx.xyzToRgb;
        cl.addAlbedo(*l, weight * tintR * shareR);
    }
    const float eT = nova::fmax(ertT - erT, 1e-4f); // single-scatter transmission at alphaT
    if (Lobe *l = cl.push(LobeType::Dielectric, frame, weight * tintT * ((1.0f - shareR) / eT),
                          luminance(weight * tintT) * (1.0f - shareR), false)) {
        l->dielectric = DielectricLobe{gT, eta, spectrum(1.0f), spectrum(1.0f), DielectricLobe::kTransmit};
        l->dielectric.film = ThinFilm{};
        l->dielectric.xyzToRgb = ctx.xyzToRgb;
        cl.addAlbedo(*l, spectrum(0.0f), weight * tintT * (1.0f - shareR));
    }
}

// Interior absorption of transmission_depth (Beer; Karma's mapping: the
// transmission color is reached at the depth).
NOVA_INLINE Spectrum absorptionColorOf(Spectrum color) {
    return {nova::fmin(nova::fmax(color.x, 1e-5f), 1.0f), nova::fmin(nova::fmax(color.y, 1e-5f), 1.0f),
            nova::fmin(nova::fmax(color.z, 1e-5f), 1.0f)};
}
NOVA_INLINE Spectrum absorptionFromDepth(Spectrum color, float depth) {
    const Spectrum c = absorptionColorOf(color);
    return Spectrum{-std::log(c.x), -std::log(c.y), -std::log(c.z)} * (1.0f / depth);
}
// The interior absorption and the colour it stands for (ClosureList).
NOVA_INLINE void setAbsorption(ClosureList &cl, Spectrum color, float depth) {
    cl.sigmaA = absorptionFromDepth(color, depth);
    cl.absorptionColor = absorptionColorOf(color);
}

NOVA_INLINE Spectrum pushSchlickLayer(ClosureList &cl, const ClosureContext &ctx, uint8_t frame, Spectrum weight,
                                      const GgxDistribution &g, Spectrum F0, Spectrum F90, float mu);

// ---------------------------------------------------------------------------
// Standard Surface 1.0.1
// ---------------------------------------------------------------------------

NOVA_INLINE void buildStandardSurface(const SurfaceInputs &in, const ShadingGeometry &g, Vec3f wo,
                                      const ClosureContext &ctx, ClosureList &cl) {
    namespace sf = surface_field;
    const BsdfTables &t = *ctx.tables;
    const Vec3f ngf = dot(wo, g.ng) < 0.0f ? -g.ng : g.ng;
    const bool entering = dot(wo, g.ng) >= 0.0f;
    const float C = saturate(in.f(sf::coatWeight)), M = saturate(in.f(sf::metalness));
    const float X = saturate(in.f(sf::transmissionWeight)), S = saturate(in.f(sf::subsurfaceWeight));
    const float s = nova::fmax(in.f(sf::specularWeight), 0.0f), h = saturate(in.f(sf::sheenWeight));
    const float b = nova::fmax(in.f(sf::baseWeight), 0.0f);
    const bool thin = in.f(sf::thinWalled) > 0.5f;
    const bool principled = in.f(sf::houdiniPrincipled) > 0.5f; // Houdini's Principled Shader (M5.1)

    // Diffuse only (no specular, coat, sheen, metal, transmission or
    // subsurface, e.g. Karma's default look): one lobe without the layer
    // work, framed like the default material (a Lambert lobe needs no valid
    // mirror direction). QON is no Lambert lobe for the closure loops (it is
    // black for wo below its frame), so it gets the general path's bent frame.
    if (C <= kCut && M <= kCut && X <= kCut && S <= kCut && s <= kCut && h <= kCut) {
        const Vec3f n = inputDirection(in.c(sf::normal), g.n);
        const float sigma = saturate(in.f(sf::diffuseRoughness));
        cl.frame[0] = cl.frame[1] = b > kCut && sigma > 0.0f ? lobeFrame(n, g.t, false, 0.0f, ngf, wo)
                                                             : Frame::fromNormal(dot(n, ngf) < 0.0f ? -n : n);
        if (b > kCut) {
            const Spectrum weight = spectrum(b), rhoD = nonNegative(in.c(sf::baseColor));
            if (sigma > 0.0f) {
                const float E = QonLobe::albedoFit(nova::fmax(dot(wo, cl.frame[0].n), 1e-4f), sigma);
                if (Lobe *l = cl.push(LobeType::Qon, 0, weight, luminance(weight * rhoD) * E, true)) {
                    l->qon = QonLobe{rhoD, sigma};
                    cl.addAlbedo(*l, weight * rhoD * E);
                }
            } else {
                pushLambert(cl, 0, weight, rhoD, false);
            }
        }
        cl.emission = emissionOf(in);
        cl.finalize();
        return;
    }

    const bool inside = X > kCut && !thin && !entering; // seen from within a refractive material
    const float ior = nova::fmax(in.f(sf::specularIor), 1e-4f), coatIor = nova::fmax(in.f(sf::coatIor), 1e-4f);

    const Vec3f n = inputDirection(in.c(sf::normal), g.n), nc = inputDirection(in.c(sf::coatNormal), g.n);
    const Vec3f tIn = in.c(sf::tangent);
    const bool hasT = dot(tIn, tIn) > 1e-12f || g.hasTangent;
    const Vec3f tangent = inputDirection(tIn, g.t);
    cl.frame[0] = lobeFrame(n, tangent, hasT, in.f(sf::specularAnisotropy) > 0.0f ? in.f(sf::specularRotation) : 0.0f, ngf, wo);
    // The coat's frame only where there is a coat.
    cl.frame[1] = C > kCut ? lobeFrame(nc, tangent, hasT, in.f(sf::coatAnisotropy) > 0.0f ? in.f(sf::coatRotation) : 0.0f,
                                       ngf, wo)
                           : cl.frame[0];
    const float mu = nova::fmax(dot(wo, cl.frame[0].n), 1e-4f), muC = nova::fmax(dot(wo, cl.frame[1].n), 1e-4f);

    ThinFilm film{};
    if (in.f(sf::thinFilmThickness) > 0.0f) // Standard Surface: nm, weight implied
        film = ThinFilm{1.0f, in.f(sf::thinFilmThickness), in.f(sf::thinFilmIor)};

    // Coat-affected roughness (the coat itself is on the outside, but it
    // roughens the interface from both sides).
    const float k = in.f(sf::coatAffectRoughness) * C * in.f(sf::coatRoughness);
    const float r = lerp(saturate(in.f(sf::specularRoughness)), 1.0f, k);
    const float rT = lerp(saturate(in.f(sf::specularRoughness) + in.f(sf::transmissionExtraRoughness)), 1.0f, k);

    Spectrum w = spectrum(1.0f);
    if (inside) {
        // The interface from inside (index 1/ior) and the opaque remainder,
        // with the transmission roughness: MaterialX's transmission_bsdf
        // serves both directions (the expanded graph's T leaf, bsdf-graphs.md 5.2).
        const Vec2f a = alphaStandardSurface(rT, in.f(sf::specularAnisotropy));
        const GgxDistribution gg = GgxDistribution::make(a.x, a.y);
        const bool absorbing = in.f(sf::transmissionDepth) > 0.0f;
        // Light that leaves crosses the coat as light that entered did: its
        // colour tints the transmission both ways (Karma, measured; audit M-0
        // of 0.2.0: Nova tinted coated glass once). What is reflected back
        // inside never reaches the coat.
        const Spectrum coatThrough = C > kCut ? lerp(spectrum(1.0f), nonNegative(in.c(sf::coatColor)), C) : spectrum(1.0f);
        pushTransmission(cl, ctx, 0, spectrum(X), gg, gg, 1.0f / ior, false, spectrum(1.0f),
                         (absorbing ? spectrum(1.0f) : nonNegative(in.c(sf::transmissionColor))) * coatThrough, mu);
        if (absorbing)
            setAbsorption(cl, in.c(sf::transmissionColor), in.f(sf::transmissionDepth));
        w *= 1.0f - X;
    } else if (C > kCut) {
        const Vec2f ac = alphaStandardSurface(in.f(sf::coatRoughness), in.f(sf::coatAnisotropy));
        const int coatFrom = cl.count;
        const Spectrum Ec = pushDielectricLayer(cl, ctx, 1, w * C, GgxDistribution::make(ac.x, ac.y), coatIor, muC, ThinFilm{});
        labelLobes(cl, coatFrom, LobeLabel::Coat);
        w *= (spectrum(1.0f) - Ec * C) * lerp(spectrum(1.0f), nonNegative(in.c(sf::coatColor)), C);
    }

    // Coat-affected colors, clamped at 0 first (MaterialX's *_nonnegative).
    const float gamma = 1.0f + C * in.f(sf::coatAffectColor);
    // gamma is 1 without a coat: skip the six powf.
    const Spectrum rhoD = gamma == 1.0f ? nonNegative(in.c(sf::baseColor)) : spectrumPow(in.c(sf::baseColor), gamma);
    const Spectrum rhoS =
        gamma == 1.0f ? nonNegative(in.c(sf::subsurfaceColor)) : spectrumPow(in.c(sf::subsurfaceColor), gamma);
    const Vec2f a = alphaStandardSurface(r, in.f(sf::specularAnisotropy));
    const GgxDistribution gs = GgxDistribution::make(a.x, a.y);
    GgxDistribution gt = gs;
    if (X > kCut && in.f(sf::transmissionExtraRoughness) != 0.0f) {
        const Vec2f aT = alphaStandardSurface(rT, in.f(sf::specularAnisotropy));
        gt = GgxDistribution::make(aT.x, aT.y);
    }

    // Metal: exact conductor through the artistic IOR; the Principled
    // Shader's is Schlick from the base colour to white.
    if (M > kCut && principled) {
        pushSchlickLayer(cl, ctx, 0, w * M, gs, clamp01(in.c(sf::baseColor) * b), spectrum(1.0f), mu);
        w *= 1.0f - M;
    } else if (M > kCut) {
        Spectrum nk, kk;
        artisticIor(clamp01(in.c(sf::baseColor) * b), clamp01(in.c(sf::specularColor) * s), nk, kk);
        const float rIdx = lutRough(gs.ax, gs.ay);
        const Spectrum comp = gs.smooth() ? spectrum(1.0f)
                                          : msCompensation(ggxAlbedo(t, rIdx, mu), ggxAlbedoAvg(t, rIdx),
                                                           conductorFresnelAvg(nk, kk));
        const Spectrum albedo = fresnelConductor(mu, nk, kk) * (gs.smooth() ? 1.0f : ggxAlbedo(t, rIdx, mu));
        if (Lobe *l = cl.push(LobeType::Conductor, 0, w * M * comp, luminance(w * M * comp * albedo), false)) {
            l->conductor = ConductorLobe{gs, FresnelModel::Conductor, nk, kk, 5.0f};
            l->conductor.film = film;
            l->conductor.xyzToRgb = ctx.xyzToRgb;
            cl.addAlbedo(*l, w * M * comp * albedo);
        }
        w *= 1.0f - M;
    }

    // Dielectric: transmission and the specular layer over the opaque base.
    if (!inside && X > kCut) {
        // The reflected share of the transmitting interface follows
        // `specular`; the transmission keeps the interface's energy. A thin
        // sheet takes the fix-up from its own R'/T' split (pushTransmission):
        // folded into tintT, its absorption step would clamp it away.
        float tScale = 1.0f;
        if (!thin) { // entering here (not inside)
            const ReflectionEnergy e = dielectricReflectionEnergy(t, gs, ior, mu);
            tScale = (1.0f - s * e.albedo) / nova::fmax(1.0f - e.albedo, 1e-4f);
        }
        const bool absorbing = !thin && in.f(sf::transmissionDepth) > 0.0f;
        pushTransmission(cl, ctx, 0, w * X, gs, gt, ior, thin, nonNegative(in.c(sf::specularColor)) * s,
                         (absorbing ? spectrum(1.0f) : nonNegative(in.c(sf::transmissionColor))) * tScale, mu,
                         thin ? s : 1.0f);
        if (absorbing)
            setAbsorption(cl, in.c(sf::transmissionColor), in.f(sf::transmissionDepth));
        w *= 1.0f - X;
    }
    if (s > kCut) {
        const Spectrum Es =
            pushDielectricLayer(cl, ctx, 0, w * s * nonNegative(in.c(sf::specularColor)), gs, ior, mu, film);
        w *= spectrum(1.0f) - Es * s; // untinted (MaterialX, measured in Karma)
    }

    // Sheen (Charlie), capped where MaterialX's form would gain energy; or
    // the Principled Shader's Disney sheen (albedo at most 0.09).
    if (h > kCut && principled) {
        const float E = DisneySheenLobe::albedo(mu);
        const Spectrum weight = w * h * nonNegative(in.c(sf::sheenColor));
        if (Lobe *l = cl.push(LobeType::DisneySheen, 0, weight, luminance(weight) * E, true)) {
            l->disneySheen = DisneySheenLobe{spectrum(1.0f)};
            cl.addAlbedo(*l, weight * E);
        }
        w *= 1.0f - h * E;
    } else if (h > kCut) {
        const float E = sheenAlbedo(t, saturate(in.f(sf::sheenRoughness)), mu);
        const float scale = E > 1.0f ? 1.0f / E : 1.0f, Ec = nova::fmin(E, 1.0f);
        const Spectrum weight = w * h * nonNegative(in.c(sf::sheenColor)) * scale;
        if (Lobe *l = cl.push(LobeType::Charlie, 0, weight, luminance(weight) * E, false)) {
            l->charlie = CharlieLobe{spectrum(1.0f), saturate(in.f(sf::sheenRoughness))};
            cl.addAlbedo(*l, weight * E);
        }
        w *= 1.0f - h * Ec;
    }

    // Subsurface (a random walk; thin-walled: translucent) and diffuse.
    if (S > kCut) {
        if (thin) {
            if (Lobe *l = pushLambert(cl, 0, w * S, rhoS, true))
                l->sss = true;
        } else {
            pushSubsurface(cl, 0, w * S, rhoS, in.c(sf::subsurfaceRadius) * in.f(sf::subsurfaceScale),
                           in.f(sf::subsurfaceAnisotropy));
        }
    }
    if (b > kCut && S < 1.0f) {
        const float sigma = saturate(in.f(sf::diffuseRoughness));
        const Spectrum weight = w * ((1.0f - S) * b);
        if (sigma > 0.0f) {
            const float E = QonLobe::albedoFit(mu, sigma);
            if (Lobe *l = cl.push(LobeType::Qon, 0, weight, luminance(weight * rhoD) * E, true)) {
                l->qon = QonLobe{rhoD, sigma};
                cl.addAlbedo(*l, weight * rhoD * E);
            }
        } else {
            pushLambert(cl, 0, weight, rhoD, false);
        }
    }

    // Emission, dimmed by the coat's Fresnel and tinted by its color.
    cl.emission = emissionOf(in);
    if (C > kCut) {
        const float F0c = iorToF0(coatIor);
        const Spectrum coatTerm = nonNegative(in.c(sf::coatColor)) * ((1.0f - F0c) * (1.0f - pow5(1.0f - muC)));
        cl.emission *= lerp(spectrum(1.0f), coatTerm, C);
    }
    cl.finalize();
}

// ---------------------------------------------------------------------------
// UsdPreviewSurface 2.6, as MaterialX's graph builds it (Karma renders the
// graph): a Lambert base (refracting smoothly where the opacity mode makes it
// transparent), under a generalized-Schlick specular layer (specular
// workflow: F0 = specularColor; metallic workflow: F0 = mix(0.04 tint,
// diffuse, metallic) and an artistic-IOR conductor mixed in by metallic),
// under a Schlick clearcoat.
// ---------------------------------------------------------------------------

// A reflection-only generalized-Schlick layer: pushes a GGX lobe with
// Schlick Fresnel (F0, F90, exponent 5) and returns its directional albedo,
// which the layers below lose (MaterialX layering, compensated).
NOVA_INLINE Spectrum pushSchlickLayer(ClosureList &cl, const ClosureContext &ctx, uint8_t frame, Spectrum weight,
                                      const GgxDistribution &g, Spectrum F0, Spectrum F90, float mu) {
    const BsdfTables &t = *ctx.tables;
    Spectrum albedo, comp = spectrum(1.0f);
    if (g.smooth()) {
        albedo = lerp(F0, F90, pow5(1.0f - mu));
    } else {
        const float r = lutRough(g.ax, g.ay);
        comp = msCompensation(ggxAlbedo(t, r, mu), ggxAlbedoAvg(t, r), F0 + (F90 - F0) * (1.0f / 21.0f));
        albedo = spectrumMax(ggxSchlickAlbedo(t, r, mu, F0, F90, spectrum(0.0f)), 0.0f) * comp;
    }
    if (Lobe *l = cl.push(LobeType::Conductor, frame, weight * comp, luminance(weight * albedo), false)) {
        l->conductor = ConductorLobe{g, FresnelModel::Schlick, F0, F90, 5.0f};
        l->conductor.film = ThinFilm{};
        l->conductor.xyzToRgb = ctx.xyzToRgb;
        cl.addAlbedo(*l, weight * albedo);
    }
    return clamp01(albedo);
}

NOVA_INLINE void buildUsdPreview(const SurfaceInputs &in, const ShadingGeometry &g, Vec3f wo, const ClosureContext &ctx,
                                 ClosureList &cl) {
    namespace sf = surface_field;
    const BsdfTables &t = *ctx.tables;
    const Vec3f ngf = dot(wo, g.ng) < 0.0f ? -g.ng : g.ng;
    const bool entering = dot(wo, g.ng) >= 0.0f;
    const Vec3f n = inputDirection(in.c(sf::normal), g.n);
    cl.frame[0] = cl.frame[1] = lobeFrame(n, g.t, g.hasTangent, 0.0f, ngf, wo);
    const float mu = nova::fmax(dot(wo, cl.frame[0].n), 1e-4f);
    const Spectrum diffuse = nonNegative(in.c(sf::baseColor));
    const float M = saturate(in.f(sf::metalness)), C = saturate(in.f(sf::coatWeight));
    const float X = saturate(in.f(sf::transmissionWeight));
    const bool specularWorkflow = in.f(sf::useSpecularWorkflow) > 0.5f;
    const float ior = nova::fmax(in.f(sf::specularIor), 1e-4f);
    const float R = (1.0f - ior) / (1.0f + ior), R2 = R * R;
    auto ggxOf = [](float roughness) { // roughness_anisotropy: alpha = r^2 (MX_FLOAT_EPS .. 1)
        const float a = nova::fmin(nova::fmax(roughness * roughness, 1e-6f), 1.0f);
        return GgxDistribution::make(a, a);
    };
    const GgxDistribution gs = ggxOf(saturate(in.f(sf::specularRoughness)));

    Spectrum w = spectrum(1.0f);
    if (C > kCut) {
        const int coatFrom = cl.count;
        w *= spectrum(1.0f) - pushSchlickLayer(cl, ctx, 1, spectrum(C), ggxOf(saturate(in.f(sf::coatRoughness))),
                                               spectrum(R2), spectrum(1.0f), mu) * C;
        labelLobes(cl, coatFrom, LobeLabel::Coat);
    }

    // The transparent share X is a refracting interface of its own: its
    // reflection at the surface's roughness, its transmission smooth, and
    // what cannot leave reflected back in. The opaque share 1 - X is the
    // specular layer over Lambert. Up to 0.1.6 the transmission sat under the
    // specular layer, as MaterialX's graph has it, and took the interface's
    // Fresnel a second time with the reflected part dropped: a fully
    // transparent surface returned 0.90 to 0.94 of a white furnace, where
    // Karma returns 1 (audit M-3 of 0.2.0).
    auto pushClear = [&](Spectrum weight) {
        if (X > kCut) {
            // From inside one smooth interface: the tables hold no rough
            // reflection from the denser side, and a reflection lobe without
            // its share there would add energy on every bounce inside.
            const GgxDistribution smooth = GgxDistribution::make(0.0f, 0.0f);
            pushTransmission(cl, ctx, 0, weight * X, entering ? gs : smooth, smooth, entering ? ior : 1.0f / ior, false,
                             spectrum(1.0f), spectrum(1.0f), mu);
        }
    };
    auto pushBase = [&](Spectrum weight, float diffuseWeight) {
        if (X < 1.0f && diffuseWeight > kCut)
            pushLambert(cl, 0, weight * diffuseWeight, diffuse, false);
    };
    if (specularWorkflow) {
        pushClear(w);
        if (X < 1.0f) {
            const Spectrum wo1 = w * (1.0f - X);
            const Spectrum Es = pushSchlickLayer(cl, ctx, 0, wo1, gs, clamp01(nonNegative(in.c(sf::specularColor))),
                                                 spectrum(1.0f), mu);
            pushBase(wo1 * (spectrum(1.0f) - Es), 1.0f);
        }
    } else {
        if (M > kCut) { // artistic-IOR conductor from the diffuse color
            Spectrum nk, kk;
            artisticIor(clamp01(diffuse), clamp01(diffuse), nk, kk);
            const float rIdx = lutRough(gs.ax, gs.ay);
            const Spectrum comp = gs.smooth() ? spectrum(1.0f)
                                              : msCompensation(ggxAlbedo(t, rIdx, mu), ggxAlbedoAvg(t, rIdx),
                                                               conductorFresnelAvg(nk, kk));
            const Spectrum albedo = fresnelConductor(mu, nk, kk) * (gs.smooth() ? 1.0f : ggxAlbedo(t, rIdx, mu));
            if (Lobe *l = cl.push(LobeType::Conductor, 0, w * M * comp, luminance(w * M * comp * albedo), false)) {
                l->conductor = ConductorLobe{gs, FresnelModel::Conductor, nk, kk, 5.0f};
                l->conductor.film = ThinFilm{};
                l->conductor.xyzToRgb = ctx.xyzToRgb;
                cl.addAlbedo(*l, w * M * comp * albedo);
            }
        }
        if (M < 1.0f) {
            const Spectrum tint = lerp(spectrum(1.0f), diffuse, M);
            const Spectrum F0 = lerp(tint * R2, tint, M);
            pushClear(w * (1.0f - M));
            if (X < 1.0f) {
                const Spectrum ws = w * ((1.0f - M) * (1.0f - X));
                const Spectrum Es = pushSchlickLayer(cl, ctx, 0, ws, gs, clamp01(F0), clamp01(tint), mu);
                pushBase(ws * (spectrum(1.0f) - Es), 1.0f - M); // the graph weighs the diffuse by 1 - metallic again
            }
        }
    }
    cl.emission = emissionOf(in);
    cl.finalize();
}

// ---------------------------------------------------------------------------
// OpenPBR 1.1.1
// ---------------------------------------------------------------------------

// Specular IOR modulated by specular_weight, with the coat's TIR fix.
NOVA_INLINE float openPbrSpecularEta(float nb, float nc, float C, float weight) {
    const float ratio = nb / nc, fix = ratio > 1.0f ? ratio : 1.0f / ratio;
    return openPbrModulatedEta(lerp(nb, fix, C), weight);
}

NOVA_INLINE void buildOpenPbr(const SurfaceInputs &in, const ShadingGeometry &g, Vec3f wo, const ClosureContext &ctx,
                              ClosureList &cl) {
    namespace sf = surface_field;
    const BsdfTables &t = *ctx.tables;
    const Vec3f ngf = dot(wo, g.ng) < 0.0f ? -g.ng : g.ng;
    const bool entering = dot(wo, g.ng) >= 0.0f;
    const float F = saturate(in.f(sf::sheenWeight)), C = saturate(in.f(sf::coatWeight)), M = saturate(in.f(sf::metalness));
    const float X = saturate(in.f(sf::transmissionWeight)), S = saturate(in.f(sf::subsurfaceWeight));
    const float b = nova::fmax(in.f(sf::baseWeight), 0.0f), xi = nova::fmax(in.f(sf::specularWeight), 0.0f);
    const bool thin = in.f(sf::thinWalled) > 0.5f;
    const bool inside = X > kCut && !thin && !entering;
    const float nb = nova::fmax(in.f(sf::specularIor), 1e-4f), nc = nova::fmax(in.f(sf::coatIor), 1e-4f);

    const Vec3f n = inputDirection(in.c(sf::normal), g.n), ncN = inputDirection(in.c(sf::coatNormal), g.n);
    const Vec3f tIn = in.c(sf::tangent), tcIn = in.c(sf::coatTangent);
    const bool hasT = dot(tIn, tIn) > 1e-12f || g.hasTangent, hasTc = dot(tcIn, tcIn) > 1e-12f || g.hasTangent;
    cl.frame[0] = lobeFrame(n, inputDirection(tIn, g.t), hasT, 0.0f, ngf, wo);
    cl.frame[1] = lobeFrame(ncN, inputDirection(tcIn, g.t), hasTc, 0.0f, ngf, wo);
    const float mu = nova::fmax(dot(wo, cl.frame[0].n), 1e-4f), muC = nova::fmax(dot(wo, cl.frame[1].n), 1e-4f);

    ThinFilm film{};
    if (in.f(sf::thinFilmWeight) > 0.0f)
        film = ThinFilm{saturate(in.f(sf::thinFilmWeight)), 1000.0f * in.f(sf::thinFilmThickness), in.f(sf::thinFilmIor)};

    const Spectrum baseColor = nonNegative(in.c(sf::baseColor));
    Spectrum w = spectrum(1.0f);
    // Fuzz (Zeltner): on top of everything, untinted layering.
    if (F > kCut && !inside) {
        const float rf = nova::fmin(nova::fmax(in.f(sf::sheenRoughness), 0.01f), 1.0f);
        const ZeltnerLobe z{spectrum(1.0f), rf};
        if (z.active(mu)) {
            const float R = zeltnerAlbedo(mu, rf);
            const Spectrum weight = w * F * nonNegative(in.c(sf::sheenColor));
            if (Lobe *l = cl.push(LobeType::Zeltner, 0, weight, luminance(weight) * R, false)) {
                l->zeltner = z;
                cl.addAlbedo(*l, weight * R);
            }
            w *= 1.0f - F * R;
        }
    }
    // Coat: reflection, absorption (MaterialX: constant), darkening (MaterialX's K).
    // What crosses the coat is tinted and darkened by it, on the way in and,
    // seen from inside a refracting object, on the way out (Karma, measured;
    // audit M-0 of 0.2.0).
    Spectrum coatThrough = spectrum(1.0f);
    if (C > kCut) {
        coatThrough = lerp(spectrum(1.0f), nonNegative(in.c(sf::coatColor)), C);
        const float K = saturate(1.0f - (1.0f - iorToF0(nc)) / (nc * nc));
        const Spectrum Eb = lerp(lerp(baseColor, nonNegative(in.c(sf::subsurfaceColor)), S), baseColor * xi, M);
        const Spectrum delta = (spectrum(1.0f) - spectrum(K)) / spectrumMax(spectrum(1.0f) - Eb * K, 1e-4f);
        coatThrough *= lerp(spectrum(1.0f), delta, C * saturate(in.f(sf::coatDarkening)));
    }
    if (C > kCut && !inside) {
        const Vec2f ac = alphaOpenPbr(in.f(sf::coatRoughness), in.f(sf::coatAnisotropy));
        const int coatFrom = cl.count;
        const Spectrum Ec = pushDielectricLayer(cl, ctx, 1, w * C, GgxDistribution::make(ac.x, ac.y), nc, muC, ThinFilm{});
        labelLobes(cl, coatFrom, LobeLabel::Coat);
        w *= spectrum(1.0f) - Ec * C;
        w *= coatThrough;
    }
    // Base roughness, roughened by the coat.
    const float rs = saturate(in.f(sf::specularRoughness)), rc = saturate(in.f(sf::coatRoughness));
    const float r = lerp(rs, std::pow(nova::fmin(1.0f, sqr(sqr(rs)) + 2.0f * sqr(sqr(rc))), 0.25f), C);
    const Vec2f a = alphaOpenPbr(r, in.f(sf::specularAnisotropy));
    const GgxDistribution gs = GgxDistribution::make(a.x, a.y);

    // Metal: F82 tint, scaled by specular_weight.
    if (M > kCut) {
        const Spectrum F0 = clamp01(baseColor * b), tint = clamp01(nonNegative(in.c(sf::specularColor)));
        const float rIdx = lutRough(gs.ax, gs.ay);
        Spectrum favg;
        for (int c = 0; c < 3; ++c)
            favg[c] = fresnelF82TintAvg(F0[c], tint[c], 1.0f, 5.0f);
        const Spectrum comp = gs.smooth() ? spectrum(1.0f) : msCompensation(ggxAlbedo(t, rIdx, mu), ggxAlbedoAvg(t, rIdx), favg);
        Spectrum bco;
        for (int c = 0; c < 3; ++c)
            bco[c] = f82Coefficient(F0[c], tint[c], 1.0f, 5.0f);
        const Spectrum albedo = gs.smooth() ? fresnelF82Tint(mu, F0, tint, 1.0f, 5.0f)
                                            : spectrumMax(ggxSchlickAlbedo(t, rIdx, mu, F0, spectrum(1.0f), bco), 0.0f);
        const Spectrum weight = w * (M * xi) * comp;
        if (Lobe *l = cl.push(LobeType::Conductor, 0, weight, luminance(weight * albedo), false)) {
            l->conductor = ConductorLobe{gs, FresnelModel::F82Tint, F0, tint, 5.0f};
            l->conductor.film = film;
            l->conductor.xyzToRgb = ctx.xyzToRgb;
            cl.addAlbedo(*l, weight * albedo);
        }
        w *= 1.0f - M;
    }

    // Dielectric: modulated index (also drives refraction in 1.1.1).
    const float eta = openPbrSpecularEta(nb, nc, C, xi);
    const Spectrum specColor = nonNegative(in.c(sf::specularColor));
    if (X > kCut) {
        const bool absorbing = !thin && in.f(sf::transmissionDepth) > 0.0f;
        const Spectrum tintT = (absorbing ? spectrum(1.0f) : nonNegative(in.c(sf::transmissionColor))) *
                               (inside ? coatThrough : spectrum(1.0f));
        pushTransmission(cl, ctx, 0, w * X, gs, gs, inside ? 1.0f / eta : eta, thin, inside ? spectrum(1.0f) : specColor,
                         tintT, mu);
        if (absorbing)
            setAbsorption(cl, in.c(sf::transmissionColor), in.f(sf::transmissionDepth));
        w *= 1.0f - X;
    }
    {
        const Spectrum Es = pushDielectricLayer(cl, ctx, 0, w * specColor, gs, eta, mu, film);
        w *= spectrum(1.0f) - Es; // untinted
    }
    // Opaque base: subsurface (a random walk; thin-walled: OpenPBR's diffuse
    // pair) and the EON diffuse.
    if (S > kCut) {
        const Spectrum sc = nonNegative(in.c(sf::subsurfaceColor));
        if (thin) {
            const float gAniso = nova::fmin(nova::fmax(in.f(sf::subsurfaceAnisotropy), -1.0f), 1.0f);
            Lobe *l[2] = {pushLambert(cl, 0, w * S * (0.5f * (1.0f - gAniso)), sc, false),
                          pushLambert(cl, 0, w * S * (0.5f * (1.0f + gAniso)), sc, true)};
            for (Lobe *x : l)
                if (x)
                    x->sss = true;
        } else {
            pushSubsurface(cl, 0, w * S, sc, in.c(sf::subsurfaceRadius) * in.f(sf::subsurfaceScale),
                           in.f(sf::subsurfaceAnisotropy));
        }
    }
    if (b > kCut && S < 1.0f) {
        const EonLobe eon{baseColor, saturate(in.f(sf::diffuseRoughness))};
        const Spectrum weight = w * ((1.0f - S) * b);
        const Spectrum albedo = eon.directionalAlbedo(mu);
        if (Lobe *l = cl.push(LobeType::Eon, 0, weight, luminance(weight * albedo), true)) {
            l->eon = eon;
            cl.addAlbedo(*l, weight * albedo);
        }
    }
    const float F0c = iorToF0(nc);
    const Spectrum coatTerm = nonNegative(in.c(sf::coatColor)) * ((1.0f - F0c) * (1.0f - pow5(1.0f - muC)));
    cl.emission = emissionOf(in) * lerp(spectrum(1.0f), coatTerm, C);
    cl.finalize();
}

} // namespace closure_detail

} // namespace nova

// Generic graphs and surface mixes (shares the helpers above).
#include "core/bsdf/graph_closure.h"

namespace nova {

// The Lambert model on its own, without a SurfaceInputs block: the default
// material's path (one lobe, so sampling needs no mixture evaluation).
NOVA_INLINE void buildLambertClosure(Spectrum albedo, const ShadingGeometry &g, Vec3f wo, ClosureList &cl) {
    cl.clear();
    cl.ng = g.ng;
    // A diffuse lobe needs no valid specular reflection (Cycles bends only
    // microfacet normals): the shading normal on the viewer's side.
    const Vec3f ngf = dot(wo, g.ng) < 0.0f ? -g.ng : g.ng;
    cl.frame[0] = Frame::fromNormal(dot(g.n, ngf) < 0.0f ? -g.n : g.n); // no coat frame
    closure_detail::pushLambert(cl, 0, spectrum(1.0f), closure_detail::nonNegative(albedo), false);
    cl.finalize();
}

namespace closure_detail {

// Houdini's plastic (the VEX shader of that name): a Lambert lobe and a Phong
// lobe, added. Neither needs a valid specular reflection of its own, so the
// frame is the shading normal on the viewer's side, as for the Lambert model.
NOVA_INLINE void buildPlastic(const SurfaceInputs &in, const ShadingGeometry &g, Vec3f wo, ClosureList &cl) {
    namespace sf = surface_field;
    const Vec3f ngf = dot(wo, g.ng) < 0.0f ? -g.ng : g.ng;
    cl.frame[0] = cl.frame[1] = Frame::fromNormal(dot(g.n, ngf) < 0.0f ? -g.n : g.n);
    pushLambert(cl, 0, spectrum(nova::fmax(in.f(sf::baseWeight), 0.0f)), nonNegative(in.c(sf::baseColor)), false);
    const Spectrum specular = nonNegative(in.c(sf::specularColor)) * nova::fmax(in.f(sf::specularWeight), 0.0f);
    if (Lobe *l = cl.push(LobeType::Phong, 0, specular, luminance(specular), false)) {
        l->phong = PhongLobe{1.0f / nova::fmax(in.f(sf::specularRoughness), 1e-3f)};
        cl.addAlbedo(*l, specular);
    }
    cl.emission = emissionOf(in);
    cl.finalize();
}

// The native models into a cleared list (ng set).
NOVA_INLINE void buildNativeClosure(const SurfaceInputs &in, const ShadingGeometry &g, Vec3f wo, const ClosureContext &ctx,
                                    ClosureList &cl) {
    namespace sf = surface_field;
    switch (in.model) {
    case SurfaceModel::StandardSurface:
        buildStandardSurface(in, g, wo, ctx, cl);
        return;
    case SurfaceModel::OpenPBR:
        buildOpenPbr(in, g, wo, ctx, cl);
        return;
    case SurfaceModel::Unlit: {
        // MaterialX surface_unlit: emission plus a straight pass-through.
        const Vec3f ngf = dot(wo, g.ng) < 0.0f ? -g.ng : g.ng;
        cl.frame[0] = cl.frame[1] = Frame::fromNormal(ngf);
        cl.emission = emissionOf(in);
        const Spectrum through = nonNegative(in.c(sf::transmissionColor)) * saturate(in.f(sf::transmissionWeight));
        if (Lobe *l = cl.push(LobeType::Dielectric, 0, through, luminance(through), false)) {
            l->dielectric = DielectricLobe{GgxDistribution::make(0.0f, 0.0f), 1.0f, spectrum(1.0f), spectrum(1.0f),
                                           DielectricLobe::kTransmit};
            l->dielectric.film = ThinFilm{};
            l->dielectric.xyzToRgb = ctx.xyzToRgb;
        }
        cl.finalize();
        return;
    }
    case SurfaceModel::UsdPreview:
        buildUsdPreview(in, g, wo, ctx, cl);
        return;
    case SurfaceModel::Hair:
        buildHair(in, g, wo, cl);
        return;
    case SurfaceModel::Plastic:
        buildPlastic(in, g, wo, cl);
        return;
    case SurfaceModel::External: // the integrator replays the bridge's results (closure.h)
        return;
    default: // Lambert (the default material)
        buildLambertClosure(in.c(sf::baseColor), g, wo, cl);
        return;
    }
}

} // namespace closure_detail

// Out of line: the models are large and must not crowd the integrator's hot
// loop (the default material takes buildLambertClosure).
NOVA_OUTLINE void buildClosure(const SurfaceInputs &in, const ShadingGeometry &g, Vec3f wo, const ClosureContext &ctx,
                               ClosureList &cl) {
    cl.clear();
    cl.ng = g.ng;
    if (in.model == SurfaceModel::Graph && in.graph)
        closure_detail::buildGraphSurface(in, g, wo, ctx, cl);
    else
        closure_detail::buildNativeClosure(in, g, wo, ctx, cl);
}

} // namespace nova
