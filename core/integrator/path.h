#pragma once

// Per-path steps of the wavefront passes that the CPU (wavefront.cpp) and the
// GPU kernels (core/gpu/kernels.cu) share (docs/design/M10.md D3).

#include "core/bsdf/closure.h"
#include "core/bsdf/surface.h"
#include "core/lights/light_tree.h"
#include "core/lights/lights.h"
#include "core/scene/hit.h"
#include "core/scene/ray.h"

#include <cmath>

namespace nova {

// The factor that scales c down to a largest magnitude of maxValue (0 = no
// clamp). By magnitude, so contributions of negative lights are clamped like
// positive ones; the uniform scale keeps sign and hue. Light path expression
// parts take the factor of their site's total, so they still sum to it.
NOVA_INLINE float clampScale(Spectrum c, float maxValue) {
    if (maxValue > 0.0f) {
        const float m = maxComponent(absolute(c));
        if (m > maxValue)
            return maxValue / m;
    }
    return 1.0f;
}

NOVA_INLINE Spectrum clampContribution(Spectrum c, float maxValue) { return c * clampScale(c, maxValue); }

// The geometry closures see at a hit. Curves shade with their tangent
// (anisotropy, hair) and a random number per strand.
NOVA_INLINE ShadingGeometry shadingGeometry(const SurfaceHit &hit) {
    return {hit.Ng, hit.N, hit.curve ? hit.tangent : Vec3f{1.0f, 0.0f, 0.0f}, hit.curve, hit.strandRandom};
}

// The closure of a hit with the given surface inputs, seen from wo.
NOVA_INLINE void buildSurfaceClosure(const SurfaceInputs &in, const SurfaceHit &hit, Vec3f wo,
                                     const ClosureContext &ctx, ClosureList &cl) {
    if (in.model == SurfaceModel::Lambert) // the default material's fast path
        buildLambertClosure(in.c(surface_field::baseColor), shadingGeometry(hit), wo, cl);
    else
        buildClosure(in, shadingGeometry(hit), wo, ctx, cl);
}

// How a closure receives light (the light tree's importance): transmitting
// closures from both sides, external (VEX) ones too (they may transmit).
NOVA_INLINE Receiver closureReceiver(const ClosureList &cl) {
    if (cl.external.active)
        return Receiver::TwoSided;
    for (int i = 0; i < cl.count; ++i)
        if (cl.lobe[i].transmits())
            return Receiver::TwoSided;
    return Receiver::OneSided;
}

// Where rays leave a surface hit towards d (ng: the geometric normal facing
// the ray that found it): round curves from the fibre's far side when d
// enters it (M9 D4), else off the surface.
NOVA_FORCEINLINE Vec3f leaveHit(const SurfaceHit &hit, Vec3f ng, bool tube, Vec3f d) {
    return tube ? tubeRayOrigin(hit.P, hit.N, hit.tangent, hit.curveRadius, d, hit.curveExitEps)
                : offsetRayOrigin(hit.P, dot(d, ng) >= 0.0f ? ng : -ng);
}

// A path's light-linking receiver set (prevSet, docs/design/M9.md D15): its
// MIS vertex's. Before the path has one (camera rays, then delta events
// only) it is the set of the last delta surface, flagged with kPrevSetDelta
// (the camera is not a receiver); every path starts with a defined value, so
// no link decision reads a slot's earlier path. Sets are below 2^31.
constexpr uint32_t kPrevSetDelta = 0x80000000u;
NOVA_INLINE uint32_t prevLinkSet(uint32_t prevSet) { return prevSet & ~kPrevSetDelta; }

// Shadow-linked dome lights (docs/design/M9.md D15): only the objects in a
// dome's shadow set block its light, so the BSDF strategy of such a dome
// cannot ride on the continuation ray, which every object stops. It is a
// shadow ray of its own, traced from the scattering vertex along the
// continuation's direction like the dome's light samples (the same
// occluders, fake-caustic transmittance and volumes), and the continuation
// never collects the dome when it escapes (escapedRadiance, non-delta). Both
// strategies then see the same visibility and keep their MIS weights:
// unbiased wherever either has density. A delta continuation (a mirror)
// keeps collecting the dome as before.
//
// The sample: for a non-delta continuation `dir` of BSDF density `bsdfPdf`
// with the light multipliers' `diffuseShare`, from a vertex whose light
// selection probabilities and links `pmf` gives, one of the shadow-linked
// domes `domes` chosen in proportion to the magnitude of its weighted
// radiance along dir (u: a uniform number, read only with several domes).
// Returns the dome's index (kInvalidId: none) with its MIS-weighted radiance
// divided by the choice probability (Le, times the light multiplier and the
// dome's filters at `site`, the vertex) and the MIS weight alone.
template <typename Pmf>
NOVA_INLINE uint32_t sampleShadowLinkedDome(const LightData *lights, const uint32_t *domes, uint32_t domeCount,
                                            const Pmf &pmf, Vec3f dir, const LightFilterSite &site, float bsdfPdf,
                                            LightStrategy strategy, float diffuseShare, float u, Spectrum &Le,
                                            float &weight) {
    const auto radiance = [&](uint32_t i, float &w) -> Spectrum {
        const LightData &light = lights[i];
        w = 0.0f;
        if (!pmf.links(i))
            return Spectrum{0.0f, 0.0f, 0.0f};
        // With portals, only through them (docs/design/M11-P9b.md).
        bool seen = true;
        const float pdf = domeSamplePdf(light, site.p, dir, seen);
        if (!seen)
            return Spectrum{0.0f, 0.0f, 0.0f};
        w = bsdfSampleWeight(strategy, bsdfPdf, misNeedsPmf(strategy, bsdfPdf) ? pmf(i) * pdf : 0.0f);
        if (!(w > 0.0f))
            return Spectrum{0.0f, 0.0f, 0.0f};
        const Spectrum L = domeRadiance(light, dir) * (lightMultiplier(light, diffuseShare) * w);
        return light.filterCount > 0 ? L * lightFilters(light, site) : L;
    };
    if (domeCount == 1) {
        Le = radiance(domes[0], weight);
        return isBlack(Le) ? kInvalidId : domes[0];
    }
    float total = 0.0f;
    for (uint32_t d = 0; d < domeCount; ++d) {
        float w;
        total += maxComponent(absolute(radiance(domes[d], w)));
    }
    if (!(total > 0.0f))
        return kInvalidId;
    const float target = u * total;
    float sum = 0.0f;
    uint32_t last = kInvalidId;
    for (uint32_t d = 0; d < domeCount; ++d) {
        const Spectrum L = radiance(domes[d], weight);
        const float m = maxComponent(absolute(L));
        if (!(m > 0.0f))
            continue;
        sum += m;
        last = d;
        if (target < sum || d + 1 == domeCount) {
            Le = L * (total / m);
            return domes[d];
        }
    }
    // Rounding past the last dome with radiance: that one.
    Le = radiance(domes[last], weight);
    Le = Le * (total / maxComponent(absolute(Le)));
    return domes[last];
}

// The opacity seed of a hit's shadow rays: they decide independently of the
// ray that found the surface (the same surface may be crossed again).
NOVA_INLINE float shadowRaySeed(float seed) { return seed + 0.5f - std::floor(seed + 0.5f); }
// The seed of a shadow-linked dome's BSDF-sample ray (sampleShadowLinkedDome),
// another decision than the light sample's.
NOVA_INLINE float domeRaySeed(float seed) { return seed + 0.125f - std::floor(seed + 0.125f); }

// A light sample transmitted through the surface: the shadow ray starts
// below it (`below`), towards the same point on the light as from `origin`
// (lights at infinity: the same direction). Returns the direction; `dist`
// becomes the ray's length to the light.
NOVA_INLINE Vec3f transmittedShadowRay(Vec3f origin, Vec3f below, Vec3f wi, float &dist) {
    if (!(dist < kInf))
        return wi;
    const Vec3f target = origin + wi * (dist / kShadowRayShortening);
    const Vec3f to = target - below;
    const float len = length(to);
    dist = len * kShadowRayShortening;
    return to * (1.0f / len);
}

// Russian roulette: the survival probability of a path with throughput beta
// after a scattering event.
NOVA_INLINE float rouletteSurvival(Spectrum beta, float etaScale) {
    const float s = maxComponent(beta) * etaScale;
    return s < 0.05f ? 0.05f : (0.95f < s ? 0.95f : s); // as std::clamp (NaN stays NaN)
}

} // namespace nova
