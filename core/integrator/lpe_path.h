#pragma once

// Light path expressions along a path (docs/design/M6.md D3-D8): the events'
// state steps, the deposits into the outputs and the parts of light samples,
// shared by the CPU wavefront (wavefront.cpp) and the GPU kernels
// (docs/design/M10.md G4). A path's state holds its events so far (state 0:
// no output can match any more, nothing to do). Contributions go to the
// outputs of the state after their light or emission event, straight into
// the film.
//
// Film: anything with lpe(output) and lpeLitSum(slot), pointers to the
// per-pixel sums (Film; the GPU's device film). Parts: a site's parts summed
// by final state, with size(), stateAt(i), add(i, value, unit) and
// push(state, value, unit) (WaveQueues' on the CPU, LpeParts on the GPU).

#include "core/bsdf/closure.h"
#include "core/film/lpe.h"
#include "core/lights/lights.h"
#include "core/math/sobol.h"
#include "core/scene/scene_view.h"

#include <cstdint>

namespace nova {

// LpeTables (wavefront.h) as plain arrays.
struct LpeView {
    static constexpr uint16_t kMaterialTag = 0xFFFF; // the object has no tag: its material's

    lpe::Program program;
    Span<uint16_t> meshTag;     // per MeshId
    Span<uint16_t> materialTag; // per MaterialId
    Span<uint16_t> volumeTag;   // per VolumeId
    Span<uint16_t> lightTag;    // per Scene::lights() index
    uint16_t label[size_t(LobeLabel::kCount)] = {};
    Span<uint8_t> albedoAfter;
    bool albedo = false;
    uint32_t albedoSymbol = 0, backgroundSymbol = 0;
    bool unit = false;
    Span<uint32_t> plainBegin, unitBegin, litBegin;
    Span<uint16_t> plainOuts, unitOuts, litSlots;
    Span<uint8_t> unitShadow;           // per unitOuts entry: a shadow; output
    uint16_t holdoutTotalSlot = 0xFFFF; // background points' whole light (lit slot), 0xFFFF: none

    NOVA_INLINE uint32_t objectTag(uint32_t mesh, uint32_t material) const {
        const uint16_t t = mesh < meshTag.size() ? meshTag[mesh] : 0;
        if (t != kMaterialTag)
            return t;
        return material < materialTag.size() ? materialTag[material] : 0u;
    }
};

// The per-event helpers are tiny and hot: always inlined.
NOVA_FORCEINLINE lpe::State lpeStep(const LpeView &t, lpe::State s, lpe::Type type, lpe::Scatter scatter,
                                    uint32_t tag, uint32_t label) {
    return s == 0 ? lpe::State(0) : t.program.step(s, t.program.symbol(type, scatter, tag, label));
}

// The scattering event of a native lobe towards a direction on the
// `transmit` side of the geometric normal.
NOVA_FORCEINLINE lpe::State lpeLobeStep(const LpeView &t, lpe::State s, const Lobe &l, bool transmit, uint32_t tag) {
    return lpeStep(t, s, l.lpeTransmit(transmit) ? lpe::TypeTransmit : lpe::TypeReflect,
                   l.lpeDiffuse() ? lpe::ScatterDiffuse : lpe::ScatterGlossy, tag, t.label[size_t(l.lpeLabel(transmit))]);
}

NOVA_FORCEINLINE lpe::State lpeLightStep(const LpeView &t, lpe::State s, uint32_t light) {
    return lpeStep(t, s, lpe::TypeLight, lpe::ScatterNone, t.lightTag[light], 0);
}

// Adds to a pixel's sum. A GPU wave may hold several samples of one pixel
// (adaptive passes, docs/design/M10.md G7), whose threads would race on its
// sums: on the device the adds are atomic. With one sample per pixel and
// wave the only writer adds in program order, so the result is the plain
// sum's. The CPU film has one writer per pixel.
NOVA_FORCEINLINE void lpeAddTo(Spectrum &sum, const Spectrum &v) {
#ifdef __CUDA_ARCH__
    atomicAdd(&sum.x, v.x);
    atomicAdd(&sum.y, v.y);
    atomicAdd(&sum.z, v.z);
#else
    sum += v;
#endif
}

// Adds a contribution that ends in state s to the outputs that take it
// (NaN/Inf as black, as in addRadiance). unoccluded; and shadow; outputs
// are not fed by these sums.
template <class Film>
NOVA_FORCEINLINE void lpeDeposit(const LpeView &t, Film &film, uint32_t pixel, lpe::State s, const Spectrum &v) {
    const uint32_t begin = t.plainBegin[s], end = t.plainBegin[s + 1];
    if (begin == end || !isFinite(v))
        return;
    for (uint32_t i = begin; i < end; ++i)
        lpeAddTo(film.lpe(t.plainOuts[i])[pixel], v);
}

// Light with MIS weight 1 for unoccluded; and shadow; outputs (D7): to the
// unoccluded sums if `unoccluded`, to shadow;'s lit sums if `lit`.
template <class Film>
NOVA_FORCEINLINE void lpeDepositUnit(const LpeView &t, Film &film, uint32_t pixel, lpe::State s, const Spectrum &v,
                                     bool unoccluded, bool lit) {
    const uint32_t ub = unoccluded ? t.unitBegin[s] : 0, ue = unoccluded ? t.unitBegin[s + 1] : 0;
    const uint32_t lb = lit ? t.litBegin[s] : 0, le = lit ? t.litBegin[s + 1] : 0;
    if ((ub == ue && lb == le) || !isFinite(v))
        return;
    for (uint32_t i = ub; i < ue; ++i)
        lpeAddTo(film.lpe(t.unitOuts[i])[pixel], v);
    for (uint32_t i = lb; i < le; ++i)
        lpeAddTo(film.lpeLitSum(t.litSlots[i])[pixel], v);
}

// Weight-1 light at a background point (a camera hit on a Background
// holdout, docs/design/M11-P2.md D14). Karma divides the occluded light of
// a shadow; output there by the point's whole unoccluded light (all lobes,
// the lights that do not illuminate the background) instead of the
// output's own: unoccluded; outputs take `unoccluded` as anywhere, shadow;
// outputs (with `shadow`; not for illumbackground lights) their occluded
// part as a negative lit sum, and lpeDepositHoldoutTotal() the whole light,
// which the resolve adds to both sums of holdout shadow; outputs.
template <class Film>
NOVA_FORCEINLINE void lpeDepositBackgroundUnit(const LpeView &t, Film &film, uint32_t pixel, lpe::State s,
                                               const Spectrum &unoccluded, const Spectrum &lit, bool shadow) {
    if (!isFinite(unoccluded) || !isFinite(lit))
        return;
    for (uint32_t i = t.unitBegin[s], end = t.unitBegin[s + 1]; i < end; ++i)
        if (!t.unitShadow[i])
            lpeAddTo(film.lpe(t.unitOuts[i])[pixel], unoccluded);
    if (shadow)
        for (uint32_t i = t.litBegin[s], end = t.litBegin[s + 1]; i < end; ++i)
            lpeAddTo(film.lpeLitSum(t.litSlots[i])[pixel], lit - unoccluded);
}

// A background point's whole unoccluded light of weight 1 (D14).
template <class Film>
NOVA_FORCEINLINE void lpeDepositHoldoutTotal(const LpeView &t, Film &film, uint32_t pixel, const Spectrum &v) {
    if (t.holdoutTotalSlot != 0xFFFF && isFinite(v))
        lpeAddTo(film.lpeLitSum(t.holdoutTotalSlot)[pixel], v);
}

// Light that no shadow ray decides (camera-visible lights, emission seen by
// a path): every kind of output, unoccluded and lit.
template <class Film>
NOVA_INLINE void lpeDepositAll(const LpeView &t, Film &film, uint32_t pixel, lpe::State s, const Spectrum &v) {
    lpeDeposit(t, film, pixel, s, v);
    if (t.unit)
        lpeDepositUnit(t, film, pixel, s, v, true, true);
}

// The parts of one site, summed by final state; states without outputs are
// dropped. `unit`: the part with MIS weight 1 (light samples).
template <class Parts>
NOVA_FORCEINLINE void lpePartsAdd(const LpeView &t, Parts &parts, lpe::State s, const Spectrum &v,
                                  const Spectrum &unit = {0.0f, 0.0f, 0.0f}) {
    if (t.program.outputCount(s) == 0)
        return;
    for (uint32_t i = 0; i < uint32_t(parts.size()); ++i) {
        if (parts.stateAt(i) == s) {
            parts.add(i, v, unit);
            return;
        }
    }
    parts.push(s, v, unit);
}

template <class Film, class Parts>
NOVA_INLINE void lpeDepositParts(const LpeView &t, Film &film, const Parts &parts, uint32_t pixel,
                                 const Spectrum &factor) {
    for (uint32_t i = 0; i < uint32_t(parts.size()); ++i)
        lpeDeposit(t, film, pixel, parts.stateAt(i), parts.valueAt(i) * factor);
}

// A site's parts on the GPU: at most N states (more are dropped; a site's
// states differ only by its lights' and lobes' tags and labels).
template <int N>
struct LpeParts {
    lpe::State state[N];
    Spectrum value[N], unitValue[N];
    uint32_t count = 0;

    NOVA_INLINE uint32_t size() const { return count; }
    NOVA_INLINE lpe::State stateAt(uint32_t i) const { return state[i]; }
    NOVA_INLINE const Spectrum &valueAt(uint32_t i) const { return value[i]; }
    NOVA_INLINE const Spectrum &unitAt(uint32_t i) const { return unitValue[i]; }
    NOVA_INLINE void add(uint32_t i, const Spectrum &v, const Spectrum &u) {
        value[i] += v;
        unitValue[i] += u;
    }
    NOVA_INLINE void push(lpe::State s, const Spectrum &v, const Spectrum &u) {
        if (count == uint32_t(N))
            return;
        state[count] = s;
        value[count] = v;
        unitValue[count] = u;
        ++count;
    }
    NOVA_INLINE void clear() { count = 0; }
};

// A events (D8): per lobe and side its albedo at wo, after the lobe's
// event (VEX: the whole albedo as a diffuse reflection).
template <class Film>
NOVA_INLINE void lpeAlbedoEvents(const LpeView &t, Film &film, uint32_t pixel, lpe::State s, uint32_t tag,
                                 const ClosureList &cl, const Spectrum &beta) {
    const auto emit = [&](lpe::State e, const Spectrum &albedo) {
        if (e != 0 && t.albedoAfter[e] && !isBlack(albedo))
            lpeDeposit(t, film, pixel, t.program.step(e, t.albedoSymbol), beta * albedo);
    };
    if (cl.external.active) {
        emit(lpeStep(t, s, lpe::TypeReflect, lpe::ScatterDiffuse, tag, 0), cl.albedo);
        return;
    }
    for (int i = 0; i < cl.count; ++i) {
        const Lobe &l = cl.lobe[i];
        emit(lpeLobeStep(t, s, l, false, tag), l.albedoR);
        emit(lpeLobeStep(t, s, l, true, tag), l.albedoT);
    }
}

// The unoccluded; estimate of the lights at infinity that light sampling
// never picks (pmf 0), along a continuation (direction, weight already in
// beta). After a non-delta event (`nonDelta`) shadow-linked lights are left
// out: the continuation never collects them, a dome's own shadow ray carries
// its weight-1 parts (path.h). Lights whose illumination is off (sampling
// quality 0, docs/design/M9.md D18) give nothing. The lights' filters act at
// `site`, where the continuation gathers them (gatherSite()).
// At a background point (`wholeBeta`: the continuation's weight over all
// lobes) the light goes in as lpeDepositBackgroundUnit() and the whole
// light to the holdout total (D14).
template <class Film>
NOVA_INLINE void lpeUnoccludedEnvironment(const LpeView &t, const LightData *lights, uint32_t lightCount,
                                          const float *pmf, Film &film, uint32_t pixel, lpe::State state, Vec3f dir,
                                          const LightFilterSite &site, float diffuseShare, const Spectrum &beta,
                                          bool nonDelta, const Spectrum *wholeBeta = nullptr) {
    for (uint32_t i = 0; i < lightCount; ++i) {
        const LightData &light = lights[i];
        if (pmf[i] > 0.0f || !light.illuminates() || (nonDelta && light.shadowLinked()))
            continue;
        Spectrum Le;
        if (light.type == LightType::Dome) {
            if (!domeSeen(light, site.p, dir)) // portals: through them only
                continue;
            Le = domeRadiance(light, dir);
        } else if (light.type == LightType::Distant && !light.delta && inDistantCone(light, dir))
            Le = light.emission;
        else
            continue;
        if (light.filterCount > 0)
            Le *= lightFilters(light, site);
        const float m = lightMultiplier(light, diffuseShare);
        if (wholeBeta) {
            lpeDepositBackgroundUnit(t, film, pixel, lpeLightStep(t, state, i), beta * Le * m, spectrum(0.0f),
                                     !light.illumBackground);
            if (!light.illumBackground)
                lpeDepositHoldoutTotal(t, film, pixel, *wholeBeta * Le * m);
        } else {
            lpeDepositUnit(t, film, pixel, lpeLightStep(t, state, i), beta * Le * m, true, false);
        }
    }
}

// evalClosure and sampleClosure with the lobes' values (lobeF, bits in mask).
NOVA_INLINE ClosureEval evalClosureLobes(const ClosureList &cl, Vec3f wo, Vec3f wi, Spectrum *lobeF, uint32_t &mask) {
    return evalClosure(cl, wo, wi, [&](int i, const Spectrum &f) {
        lobeF[i] = f;
        mask |= 1u << i;
    });
}

NOVA_INLINE bool sampleClosureLobes(const ClosureList &cl, Vec3f wo, Vec3f u, ClosureSample &s, Spectrum *lobeF,
                                    uint32_t &mask) {
    return sampleClosure(cl, wo, u, s, [&](int i, const Spectrum &f) {
        lobeF[i] = f;
        mask |= 1u << i;
    });
}

// The scattering event of a continuation (docs/design/M6.md D5). A sample
// weighted by the whole mixture (several lobes, `lobeF` their values at the
// sampled direction) whose lobes fall into several LPE classes takes one
// class, chosen in proportion to its luminance, with its share of the
// weight divided by the choice probability: unbiased per class, and the
// beauty keeps the sample's luminance (only its hue varies). Otherwise the
// sampled lobe's event. Returns the state after the event.
NOVA_INLINE lpe::State lpeScatter(const LpeView &t, uint32_t depth, lpe::State s, const ClosureList &cl,
                                  ClosureSample &cs, const Spectrum *lobeF, uint32_t lobeMask, uint32_t tag,
                                  const Sampler &sampler) {
    if (cl.external.active) {
        // VEX: the class of the sampled bounce.
        const bool sss = cs.rayClass == RayClass::Sss;
        const bool diffuse = sss || cs.rayClass == RayClass::Diffuse || cs.rayClass == RayClass::Volume;
        return lpeStep(t, s, cs.transmit || sss ? lpe::TypeTransmit : lpe::TypeReflect,
                       diffuse ? lpe::ScatterDiffuse : lpe::ScatterGlossy, tag,
                       sss ? t.label[size_t(LobeLabel::Sss)] : 0u);
    }
    if (cs.delta || lobeMask == 0)
        return lpeLobeStep(t, s, cl.lobe[cs.lobe], cs.transmit, tag);
    lpe::State state[ClosureList::kMaxLobes];
    Spectrum f[ClosureList::kMaxLobes], fDiffuse[ClosureList::kMaxLobes];
    int groups = 0;
    for (int i = 0; i < cl.count; ++i) {
        if (!(lobeMask & (1u << i)))
            continue;
        const lpe::State n = lpeLobeStep(t, s, cl.lobe[i], cs.transmit, tag);
        int g = 0;
        while (g < groups && state[g] != n)
            ++g;
        if (g == groups) {
            state[g] = n;
            f[g] = fDiffuse[g] = spectrum(0.0f);
            ++groups;
        }
        f[g] += lobeF[i];
        if (cl.lobe[i].diffuse)
            fDiffuse[g] += lobeF[i];
    }
    if (groups == 1)
        return state[0];
    float lum[ClosureList::kMaxLobes], total = 0.0f;
    Spectrum fTotal = spectrum(0.0f);
    for (int g = 0; g < groups; ++g) {
        lum[g] = luminance(f[g]);
        total += lum[g];
        fTotal += f[g];
    }
    if (!(total > 0.0f))
        return lpeLobeStep(t, s, cl.lobe[cs.lobe], cs.transmit, tag);
    const float u = sampler.get1D(stream::lpeSplit(depth)) * total;
    int k = 0;
    float edge = lum[0];
    while (k + 1 < groups && u >= edge)
        edge += lum[++k];
    while (k > 0 && !(lum[k] > 0.0f)) // rounding past the last class with a share
        --k;
    const auto share = [](float a, float b) { return b > 0.0f ? a / b : 0.0f; };
    const Spectrum ratio{share(f[k].x, fTotal.x), share(f[k].y, fTotal.y), share(f[k].z, fTotal.z)};
    cs.weight = cs.weight * ratio * (total / lum[k]);
    cs.diffuseShare = saturate(luminance(fDiffuse[k]) / lum[k]);
    return state[k];
}

// Whether the closure refracts into a solid (a transmitting dielectric lobe,
// not thin-walled): its inside is the side opposite the authored normal.
NOVA_INLINE bool refractsInto(const ClosureList &cl) {
    for (int i = 0; i < cl.count; ++i)
        if (cl.lobe[i].type == LobeType::Dielectric && cl.lobe[i].transmits())
            return true;
    return false;
}

// Whether light samples and continuations need the lobes' own values: the
// lobes that scatter to one side lead to different states, or the weight-1
// parts must leave out directions into a solid. Otherwise the closure is
// one event per side and is evaluated as a whole (lobeMask 0).
NOVA_INLINE bool lpeNeedsLobes(const LpeView &t, lpe::State s, const ClosureList &cl, uint32_t tag) {
    if (cl.external.active)
        return false;
    if (t.unit && refractsInto(cl))
        return true;
    if (cl.count < 2)
        return false;
    for (int side = 0; side < 2; ++side) {
        const bool transmit = side != 0;
        lpe::State first = 0;
        bool any = false;
        for (int i = 0; i < cl.count; ++i) {
            const Lobe &l = cl.lobe[i];
            if (transmit ? !l.transmits() : !l.reflects())
                continue;
            const lpe::State n = lpeLobeStep(t, s, l, transmit, tag);
            if (any && n != first)
                return true;
            first = n;
            any = true;
        }
    }
    return false;
}

// The parts of a light sample (docs/design/M6.md D5): per lobe its event
// and the light's, f_lobe times the light multiplier of the lobe's class
// times `unit` (throughput, radiance / pdf), and with `weight` (MIS). An
// external closure is one diffuse event with its whole value f. `entering`:
// the hit is on the outside of its surface (authored normal).
template <class Parts>
NOVA_INLINE void lpeLightSampleParts(const LpeView &t, Parts &parts, lpe::State s, uint32_t tag, const ClosureList &cl,
                                     const Spectrum *lobeF, uint32_t lobeMask, const LightData &light, uint32_t li,
                                     bool transmit, bool entering, const Spectrum &f, const Spectrum &unit,
                                     float weight) {
    if (cl.external.active) {
        const lpe::State n =
            lpeStep(t, s, transmit ? lpe::TypeTransmit : lpe::TypeReflect, lpe::ScatterDiffuse, tag, 0);
        lpePartsAdd(t, parts, lpeLightStep(t, n, li), f * unit * weight, f * unit);
        return;
    }
    if (lobeMask == 0) {
        // One event for the whole closure (lpeNeedsLobes): any lobe on that side gives it.
        for (int i = 0; i < cl.count; ++i) {
            const Lobe &lobe = cl.lobe[i];
            if (transmit ? !lobe.transmits() : !lobe.reflects())
                continue;
            const lpe::State n = lpeLobeStep(t, s, lobe, transmit, tag);
            lpePartsAdd(t, parts, lpeLightStep(t, n, li), f * unit * weight, f * unit);
            return;
        }
        return;
    }
    const bool solid = refractsInto(cl);
    for (int i = 0; i < cl.count; ++i) {
        if (!(lobeMask & (1u << i)))
            continue;
        const Lobe &lobe = cl.lobe[i];
        const lpe::State n = lpeLobeStep(t, s, lobe, transmit, tag);
        const Spectrum fi = lobeF[i] * (lobe.diffuse ? light.diffuse : light.specular) * unit;
        // Directions into the inside of a solid dielectric (refraction in,
        // reflection inside) are not light-sampled for the weight-1 sums:
        // the object itself blocks them. Karma's unoccluded;C<TG>L is 0,
        // light leaving the object counts (measured).
        const bool intoSolid = solid && lobe.type == LobeType::Dielectric && (entering ? transmit : !transmit);
        lpePartsAdd(t, parts, lpeLightStep(t, n, li), fi * weight, intoSolid ? spectrum(0.0f) : fi);
    }
}

} // namespace nova
