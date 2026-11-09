#pragma once

// Closures of generic surfaces (docs/design/M4.md 3.4, docs/research/m4/
// bsdf-graphs.md 5.2-5.4): pbrlib BSDF graphs and mixes of surface shaders.
// Included by core/bsdf/surface_models.h (it shares the native models' lobe
// helpers, so both paths compute the same numbers).
//
// A graph is walked twice per hit, in post-order without recursion (shared
// subgraphs work):
// - bottom-up, A: what a node blocks from a base below it at wo (spectral).
//   Interface leaves (dielectric and Schlick reflection, sheen) block their
//   weighted albedo, every other leaf its weight (coverage); mix lerps,
//   multiply scales, add sums (at most 1), layer composes
//   A_top + (1 - A_top) A_base.
// - top-down, W: the weight reaching each node. Mix splits it, multiply
//   scales it, add passes it to both, layer passes it to the top and
//   W (1 - A_top) to the base; leaves push their lobes with W.
// These rules are linear (MaterialX's spec, OSL testrender): where genglsl
// and Karma disagree with the spec (multiply or add inside a layer's top, a
// weighted opaque top), Nova follows the spec (M4 D5).
//
// Leaves: oren_nayar (Lambert, QON, EON), burley (MaterialX's Burley, D6),
// translucent, subsurface (a random walk, M9; thin-walled: translucent), dielectric R / T / RT (T alone
// normalized to unit albedo by 1 / E_T, D7, so layer(R, T) equals the native
// R+T lobe), conductor, generalized Schlick (Hoffman's F82 term; T and RT
// refract with f0_to_ior(average color0), as MaterialX), sheen (Charlie,
// Zeltner). Seen from inside a refracting graph, T and RT leaves become the
// interface with 1/ior and reflections layered over refraction drop out
// (D11). Emission is not attenuated by the BSDF layers (MaterialX).

#include <algorithm>
#include <cstring>

namespace nova {
namespace closure_detail {

NOVA_INLINE Spectrum graphColor(const SurfaceInputs &in, uint16_t o) { return nonNegative(in.c(o)); }

// The interface albedo of a reflection-only generalized-Schlick leaf
// (Hoffman's F82 term; the albedo tables assume exponent 5) and its
// multiple-scattering compensation.
NOVA_INLINE void hoffmanEnergy(const BsdfTables &t, const GgxDistribution &g, Spectrum F0, Spectrum F82, Spectrum F90,
                               float exponent, float mu, Spectrum &albedo, Spectrum &comp) {
    Spectrum b, favg;
    for (int c = 0; c < 3; ++c) {
        b[c] = f82Coefficient(F0[c], F82[c], F90[c], exponent);
        favg[c] = fresnelF82TintAvg(F0[c], F82[c], F90[c], exponent);
    }
    if (g.smooth()) {
        comp = spectrum(1.0f);
        for (int c = 0; c < 3; ++c)
            albedo[c] = fresnelF82Tint(mu, F0[c], F82[c], F90[c], exponent);
        return;
    }
    const float r = lutRough(g.ax, g.ay);
    comp = msCompensation(ggxAlbedo(t, r, mu), ggxAlbedoAvg(t, r), favg);
    albedo = spectrumMax(ggxSchlickAlbedo(t, r, mu, F0, F90, b), 0.0f) * comp;
}

NOVA_INLINE ThinFilm leafFilm(const SurfaceInputs &in, uint16_t thickness) {
    const float nm = in.f(thickness);
    return nm > 0.0f ? ThinFilm{1.0f, nm, in.f(uint16_t(thickness + 1))} : ThinFilm{};
}

NOVA_INLINE GgxDistribution leafGgx(const SurfaceInputs &in, uint16_t alpha) {
    return GgxDistribution::make(nova::fmax(in.f(alpha), 0.0f), nova::fmax(in.f(uint16_t(alpha + 1)), 0.0f));
}

// The energy of an interface leaf at wo, computed once per hit (the layer
// walk needs the albedo, the lobe the same albedo and its compensation).
struct LeafEnergy {
    Spectrum albedo;
    Spectrum compensation;
    bool valid;
};

// The film-blended albedo of a reflection-only dielectric interface, as
// pushDielectricLayer computes it.
NOVA_INLINE Spectrum dielectricLayerAlbedo(const ClosureContext &ctx, const GgxDistribution &g, float eta, float mu,
                                           const ThinFilm &film, float &compensation) {
    const ReflectionEnergy e = dielectricReflectionEnergy(*ctx.tables, g, eta, mu);
    compensation = e.compensation;
    Spectrum albedo = spectrum(e.albedo);
    if (film.weight > 0.0f && film.thicknessNm > 0.0f) {
        AiryBase base{};
        base.n = spectrum(eta);
        base.k = spectrum(0.0f);
        const Spectrum airyMu = fresnelAiry(mu, film, base, ctx.xyzToRgb);
        const Spectrum airy1 = fresnelAiry(1.0f, film, base, ctx.xyzToRgb);
        const Spectrum rough = g.smooth() ? airy1
                                          : ggxSchlickAlbedo(*ctx.tables, lutRough(g.ax, g.ay), mu, airy1, spectrum(1.0f),
                                                             spectrum(0.0f)) *
                                                e.compensation;
        albedo = lerp(albedo, clamp01(lerp(airyMu, rough, std::sqrt(nova::fmax(g.ax, g.ay)))), film.weight);
    }
    return albedo;
}

// What a leaf blocks from a base below it (A), at wo; interface leaves
// leave their energy in `cache` for pushLeaf.
NOVA_INLINE Spectrum leafBlock(const SurfaceInputs &in, const GraphNode &n, const ClosureContext &ctx, float mu,
                               bool inside, LeafEnergy &cache) {
    cache.valid = false;
    const float w = nova::fmax(in.f(n.param), 0.0f);
    if (w <= 0.0f)
        return spectrum(0.0f);
    const bool reflectOnly = (n.flags & (kLeafReflect | kLeafTransmit)) == kLeafReflect;
    switch (n.kind) {
    case LeafKind::Dielectric:
        if (reflectOnly) {
            if (inside && (n.flags & kLeafOverRefraction))
                return spectrum(0.0f);
            const float ior = nova::fmax(in.f(uint16_t(n.param + 4)), 1e-4f);
            float comp;
            cache.albedo = dielectricLayerAlbedo(ctx, leafGgx(in, uint16_t(n.param + 5)), inside ? 1.0f / ior : ior, mu,
                                                 leafFilm(in, uint16_t(n.param + 7)), comp);
            cache.compensation = spectrum(comp);
            cache.valid = true;
            return clamp01(cache.albedo * w);
        }
        break;
    case LeafKind::Schlick:
        if (reflectOnly) {
            if (inside && (n.flags & kLeafOverRefraction))
                return spectrum(0.0f);
            hoffmanEnergy(*ctx.tables, leafGgx(in, uint16_t(n.param + 11)), graphColor(in, uint16_t(n.param + 1)),
                          graphColor(in, uint16_t(n.param + 4)), graphColor(in, uint16_t(n.param + 7)),
                          in.f(uint16_t(n.param + 10)), mu, cache.albedo, cache.compensation);
            cache.valid = true;
            return clamp01(cache.albedo * w);
        }
        break;
    case LeafKind::Sheen: {
        const float r = in.f(uint16_t(n.param + 4));
        if (n.flags & kLeafZeltner) {
            const float rf = nova::fmin(nova::fmax(r, 0.01f), 1.0f);
            if (!ZeltnerLobe{spectrum(1.0f), rf}.active(mu))
                return spectrum(0.0f);
            cache.albedo = spectrum(zeltnerAlbedo(mu, rf));
        } else {
            cache.albedo = spectrum(sheenAlbedo(*ctx.tables, saturate(r), mu));
        }
        cache.compensation = spectrum(1.0f);
        cache.valid = true;
        return spectrum(saturate(nova::fmin(cache.albedo.x, 1.0f) * w));
    }
    default:
        break;
    }
    return spectrum(nova::fmin(w, 1.0f)); // opaque, or transmitting: coverage
}

// Pushes the lobes of leaf n with weight W (the weight input applied here);
// `cache` holds its energy if the layer walk computed it.
NOVA_INLINE void pushLeaf(const SurfaceInputs &in, const GraphNode &n, Spectrum W, const ClosureContext &ctx,
                          ClosureList &cl, Vec3f wo, float mu, bool inside, bool thin, bool entering,
                          const LeafEnergy &cache) {
    const BsdfTables &t = *ctx.tables;
    const float w = nova::fmax(in.f(n.param), 0.0f);
    const Spectrum weight = W * w;
    if (!(maxComponent(weight) > kCut))
        return;
    const uint8_t frame = n.frame;
    const uint16_t p = n.param;
    switch (n.kind) {
    case LeafKind::Diffuse: {
        const Spectrum color = graphColor(in, uint16_t(p + 1));
        const float r = saturate(in.f(uint16_t(p + 4)));
        if (n.flags & kLeafEnergyComp) {
            const EonLobe eon{color, r};
            const Spectrum albedo = eon.directionalAlbedo(mu);
            if (Lobe *l = cl.push(LobeType::Eon, frame, weight, luminance(weight * albedo), true)) {
                l->eon = eon;
                cl.addAlbedo(*l, weight * albedo);
            }
        } else if (r > 0.0f) {
            const float E = QonLobe::albedoFit(mu, r);
            if (Lobe *l = cl.push(LobeType::Qon, frame, weight, luminance(weight * color) * E, true)) {
                l->qon = QonLobe{color, r};
                cl.addAlbedo(*l, weight * color * E);
            }
        } else {
            pushLambert(cl, frame, weight, color, false);
        }
        return;
    }
    case LeafKind::Burley: {
        const Spectrum color = graphColor(in, uint16_t(p + 1));
        const float r = saturate(in.f(uint16_t(p + 4)));
        const float E = BurleyLobe::albedoFit(mu, r);
        if (Lobe *l = cl.push(LobeType::Burley, frame, weight, luminance(weight * color) * E, true)) {
            l->burley = BurleyLobe{color, r};
            cl.addAlbedo(*l, weight * color * E);
        }
        return;
    }
    case LeafKind::Translucent:
        pushLambert(cl, frame, weight, graphColor(in, uint16_t(p + 1)), true);
        return;
    case LeafKind::Subsurface:
        if (thin) {
            if (Lobe *l = pushLambert(cl, frame, weight, graphColor(in, uint16_t(p + 1)), true))
                l->sss = true;
        } else {
            pushSubsurface(cl, frame, weight, graphColor(in, uint16_t(p + 1)), in.c(uint16_t(p + 4)),
                           in.f(uint16_t(p + 7)));
        }
        return;
    case LeafKind::ChiangHair: {
        // MaterialX chiang_hair_bsdf (docs/design/M9.md D6): per lobe (v, s)
        // as given (s without its sqrt(pi/8)), TRRT+ with TRT's roughness and
        // tint, cuticle_angle 0.5 untilted (above: toward the root, as
        // Karma Hair's positive shift). The frame's x is the fibre (the
        // curve's tangent, or curve_direction).
        const Frame &f = cl.frame[frame];
        const Vec3f woL = f.toLocal(wo);
        const float sinO = nova::fmin(nova::fmax(woL.x, -1.0f), 1.0f);
        const float cosO = std::sqrt(nova::fmax(0.0f, 1.0f - sinO * sinO));
        Vec3f z = wo - f.t * dot(wo, f.t);
        z = dot(z, z) > 1e-12f ? normalize(z) : f.n;
        const float h = nova::fmin(nova::fmax(dot(f.n, cross(z, f.t)), -1.0f), 1.0f);
        constexpr float kSqrtPiOver8 = 0.626657069f;
        const float v[3] = {in.f(uint16_t(p + 11)), in.f(uint16_t(p + 13)), in.f(uint16_t(p + 15))};
        const float s[3] = {in.f(uint16_t(p + 12)) * kSqrtPiOver8, in.f(uint16_t(p + 14)) * kSqrtPiOver8,
                            in.f(uint16_t(p + 16)) * kSqrtPiOver8};
        const float alpha = -(in.f(uint16_t(p + 17)) - 0.5f) * kPi;
        const HairFibre fibre = hairFibre(in.f(uint16_t(p + 10)), nonNegative(in.c(uint16_t(p + 18))), v, s, alpha,
                                          sinO, cosO, std::atan2(woL.z, woL.y), h);
        const Spectrum tints[4] = {graphColor(in, uint16_t(p + 1)), graphColor(in, uint16_t(p + 4)),
                                   graphColor(in, uint16_t(p + 7)), graphColor(in, uint16_t(p + 7))};
        pushHair(cl, frame, fibre, tints, weight);
        return;
    }
    case LeafKind::Sheen: {
        const Spectrum color = graphColor(in, uint16_t(p + 1));
        const float r = in.f(uint16_t(p + 4));
        if (n.flags & kLeafZeltner) {
            const float rf = nova::fmin(nova::fmax(r, 0.01f), 1.0f);
            const ZeltnerLobe z{spectrum(1.0f), rf};
            if (!z.active(mu))
                return;
            const float R = cache.valid ? cache.albedo.x : zeltnerAlbedo(mu, rf);
            if (Lobe *l = cl.push(LobeType::Zeltner, frame, weight * color, luminance(weight * color) * R, false)) {
                l->zeltner = z;
                cl.addAlbedo(*l, weight * color * R);
            }
            return;
        }
        // Charlie, capped where MaterialX's form would gain energy (as the
        // native Standard Surface sheen).
        const float rc = saturate(r);
        const float E = cache.valid ? cache.albedo.x : sheenAlbedo(t, rc, mu);
        const Spectrum lw = weight * color * (E > 1.0f ? 1.0f / E : 1.0f);
        if (Lobe *l = cl.push(LobeType::Charlie, frame, lw, luminance(lw) * E, false)) {
            l->charlie = CharlieLobe{spectrum(1.0f), rc};
            cl.addAlbedo(*l, lw * E);
        }
        return;
    }
    case LeafKind::Conductor: {
        const GgxDistribution g = leafGgx(in, uint16_t(p + 7));
        const Spectrum nk = graphColor(in, uint16_t(p + 1)), kk = graphColor(in, uint16_t(p + 4));
        const float r = lutRough(g.ax, g.ay);
        const Spectrum comp =
            g.smooth() ? spectrum(1.0f) : msCompensation(ggxAlbedo(t, r, mu), ggxAlbedoAvg(t, r), conductorFresnelAvg(nk, kk));
        const Spectrum albedo = fresnelConductor(mu, nk, kk) * (g.smooth() ? 1.0f : ggxAlbedo(t, r, mu));
        if (Lobe *l = cl.push(LobeType::Conductor, frame, weight * comp, luminance(weight * comp * albedo), false)) {
            l->conductor = ConductorLobe{g, FresnelModel::Conductor, nk, kk, 5.0f};
            l->conductor.film = leafFilm(in, uint16_t(p + 9));
            l->conductor.xyzToRgb = ctx.xyzToRgb;
            cl.addAlbedo(*l, weight * comp * albedo);
        }
        return;
    }
    default:
        break;
    }

    // Interfaces: dielectric and generalized Schlick.
    const bool dielectric = n.kind == LeafKind::Dielectric;
    const bool reflect = (n.flags & kLeafReflect) != 0, transmit = (n.flags & kLeafTransmit) != 0;
    const GgxDistribution g = leafGgx(in, uint16_t(dielectric ? p + 5 : p + 11));
    const ThinFilm film = leafFilm(in, uint16_t(dielectric ? p + 7 : p + 13));
    float ior;
    Spectrum tint = spectrum(1.0f);
    if (dielectric) {
        ior = nova::fmax(in.f(uint16_t(p + 4)), 1e-4f);
        tint = graphColor(in, uint16_t(p + 1));
    } else {
        const Spectrum F0 = graphColor(in, uint16_t(p + 1));
        ior = f0ToIor(nova::fmin(average(F0), 0.99f));
    }
    if (reflect && !transmit) {
        if (inside && (n.flags & kLeafOverRefraction))
            return;
        if (dielectric) {
            const float eta = inside ? 1.0f / ior : ior;
            if (!cache.valid) {
                pushDielectricLayer(cl, ctx, frame, weight * tint, g, eta, mu, film);
                return;
            }
            const Spectrum lw = weight * tint;
            if (Lobe *l = cl.push(LobeType::Dielectric, frame, lw * cache.compensation.x, luminance(lw * cache.albedo),
                                  false)) {
                l->dielectric = DielectricLobe{g, eta, spectrum(1.0f), spectrum(1.0f), DielectricLobe::kReflect};
                l->dielectric.film = film;
                l->dielectric.xyzToRgb = ctx.xyzToRgb;
                cl.addAlbedo(*l, lw * cache.albedo);
            }
            return;
        }
        const Spectrum F0 = graphColor(in, uint16_t(p + 1)), F82 = graphColor(in, uint16_t(p + 4)),
                       F90 = graphColor(in, uint16_t(p + 7));
        const float exponent = in.f(uint16_t(p + 10));
        Spectrum albedo = cache.albedo, comp = cache.compensation;
        if (!cache.valid)
            hoffmanEnergy(t, g, F0, F82, F90, exponent, mu, albedo, comp);
        if (Lobe *l = cl.push(LobeType::Conductor, frame, weight * comp, luminance(weight * albedo), false)) {
            l->conductor = ConductorLobe{g, FresnelModel::Hoffman, F0, F82, exponent};
            l->conductor.c2 = F90;
            l->conductor.film = film;
            l->conductor.xyzToRgb = ctx.xyzToRgb;
            cl.addAlbedo(*l, weight * albedo);
        }
        return;
    }
    if (thin) {
        pushTransmission(cl, ctx, frame, weight, g, g, ior, true, reflect ? tint : spectrum(0.0f), tint, mu);
        return;
    }
    if (inside || reflect) { // the whole interface (from inside: with 1/ior)
        pushTransmission(cl, ctx, frame, weight, g, g, inside ? 1.0f / ior : ior, false, inside ? spectrum(1.0f) : tint,
                         tint, mu);
        return;
    }
    // Transmission alone, normalized to unit albedo (M4 D7): under a
    // reflection of the same interface it completes the native R+T lobe.
    const float eta = entering ? ior : 1.0f / ior;
    float eT;
    if (g.smooth()) {
        eT = 1.0f - fresnelDielectric(mu, eta);
    } else {
        const float r = lutRough(g.ax, g.ay);
        eT = dielectricAlbedoRT(t, eta, r, mu) - (eta >= 1.0f ? dielectricAlbedoR(t, eta, r, mu) : 0.0f);
    }
    if (Lobe *l = cl.push(LobeType::Dielectric, frame, weight * tint * (1.0f / nova::fmax(eT, 1e-4f)),
                          luminance(weight * tint), false)) {
        l->dielectric = DielectricLobe{g, eta, spectrum(1.0f), spectrum(1.0f), DielectricLobe::kTransmit};
        l->dielectric.film = ThinFilm{};
        l->dielectric.xyzToRgb = ctx.xyzToRgb;
        cl.addAlbedo(*l, spectrum(0.0f), weight * tint);
    }
}

// One graph segment into cl (frames 0 .. frameCount - 1; no clear, no
// finalize).
NOVA_INLINE void buildGraphSegment(const SurfaceInputs &in, const ClosureGraph &G, const GraphSegment &seg,
                                   const ShadingGeometry &g, Vec3f wo, const ClosureContext &ctx, ClosureList &cl) {
    const Vec3f ngf = dot(wo, g.ng) < 0.0f ? -g.ng : g.ng;
    const bool entering = dot(wo, g.ng) >= 0.0f;
    const bool thin = seg.thinWalled != kNoSlot && in.f(seg.thinWalled) > 0.5f;
    const bool inside = G.refractive && !thin && !entering;
    float mu[ClosureGraph::kMaxFrames];
    const int frames = G.frameCount > 0 ? G.frameCount : 1;
    for (int k = 0; k < frames; ++k) {
        const GraphFrame f = k < G.frameCount ? G.frame[k] : GraphFrame{kNoSlot, kNoSlot};
        const Vec3f n = f.normal != kNoSlot ? inputDirection(in.c(f.normal), g.n) : g.n;
        const Vec3f tIn = f.tangent != kNoSlot ? in.c(f.tangent) : Vec3f{0.0f, 0.0f, 0.0f};
        const bool hasT = dot(tIn, tIn) > 1e-12f || g.hasTangent;
        cl.frame[k] = lobeFrame(n, inputDirection(tIn, g.t), hasT, 0.0f, ngf, wo);
        mu[k] = nova::fmax(dot(wo, cl.frame[k].n), 1e-4f);
    }

    if (seg.root != ClosureGraph::kNoNode) {
        Spectrum A[ClosureGraph::kMaxNodes], W[ClosureGraph::kMaxNodes];
        LeafEnergy energy[ClosureGraph::kMaxNodes];
        for (int i = 0; i < G.nodeCount; ++i) {
            const GraphNode &n = G.node[i];
            W[i] = spectrum(0.0f);
            energy[i].valid = false;
            switch (n.op) {
            case GraphOp::Leaf:
                A[i] = (n.flags & kLeafInTop) ? leafBlock(in, n, ctx, mu[n.frame], inside, energy[i]) : spectrum(0.0f);
                break;
            case GraphOp::Mix: {
                const float m = saturate(in.f(n.param));
                A[i] = lerp(A[n.b], A[n.a], m);
                break;
            }
            case GraphOp::Multiply:
                A[i] = clamp01(A[n.a] * graphColor(in, n.param));
                break;
            case GraphOp::Add:
                A[i] = clamp01(A[n.a] + A[n.b]);
                break;
            case GraphOp::Layer:
                A[i] = A[n.a] + (spectrum(1.0f) - A[n.a]) * A[n.b];
                break;
            default: // LayerVdf
                A[i] = A[n.a];
                break;
            }
        }
        W[seg.root] = spectrum(1.0f);
        for (int i = int(seg.root); i >= 0; --i) {
            const GraphNode &n = G.node[i];
            const Spectrum w = W[i];
            if (!(maxComponent(w) > kCut))
                continue;
            switch (n.op) {
            case GraphOp::Leaf:
                pushLeaf(in, n, w, ctx, cl, wo, mu[n.frame], inside, thin, entering, energy[i]);
                break;
            case GraphOp::Mix: {
                const float m = saturate(in.f(n.param));
                W[n.a] += w * m;
                W[n.b] += w * (1.0f - m);
                break;
            }
            case GraphOp::Multiply:
                W[n.a] += w * graphColor(in, n.param);
                break;
            case GraphOp::Add:
                W[n.a] += w;
                W[n.b] += w;
                break;
            case GraphOp::Layer:
                W[n.a] += w;
                W[n.b] += w * spectrumMax(spectrum(1.0f) - A[n.a], 0.0f);
                break;
            default: // LayerVdf: the medium below the top's interfaces
                W[n.a] += w;
                if (!thin) {
                    cl.sigmaA = graphColor(in, n.param);
                    // Per unit length: the colour of a unit's depth.
                    cl.absorptionColor = Spectrum{std::exp(-nova::fmax(cl.sigmaA.x, 0.0f)),
                                                  std::exp(-nova::fmax(cl.sigmaA.y, 0.0f)),
                                                  std::exp(-nova::fmax(cl.sigmaA.z, 0.0f))};
                }
                break;
            }
        }
    }

    // Emission (not attenuated by the layers above it, as in MaterialX).
    if (seg.emissionEnd > seg.emissionBegin) {
        const Vec3f nf = dot(g.n, ngf) < 0.0f ? -g.n : g.n;
        const float muE = nova::fmin(nova::fmax(dot(nf, wo), 1e-4f), 1.0f);
        for (int e = seg.emissionBegin; e < seg.emissionEnd; ++e) {
            const GraphEmission &em = G.emission[e];
            const Spectrum c = in.c(em.color);
            Spectrum term{finiteOr0(c.x), finiteOr0(c.y), finiteOr0(c.z)};
            if (em.schlick != kNoSlot)
                term *= lerp(in.c(em.schlick), in.c(uint16_t(em.schlick + 3)),
                             std::pow(1.0f - muE, nova::fmax(in.f(uint16_t(em.schlick + 6)), 0.0f)));
            cl.emission += term;
        }
    }
}

// Appends the closure `s` (finalized) to cl with weight w, its frames after
// frameBase (the last frame is shared when they run out).
NOVA_INLINE void appendClosure(ClosureList &cl, const ClosureList &s, int sFrames, Spectrum w, int &frameBase) {
    for (int k = 0; k < sFrames && frameBase + k < ClosureList::kMaxFrames; ++k)
        cl.frame[frameBase + k] = s.frame[k];
    // The picks back on the absolute estimate scale: the estimates' own sum,
    // not the albedo (an unlit pass-through has an estimate but no albedo).
    const float total = nova::fmax(s.estimateSum, 1e-6f), scale = nova::fmax(luminance(w), 0.0f);
    for (int i = 0; i < s.count; ++i) {
        const Lobe &src = s.lobe[i];
        const int slot = frameBase + int(src.frame);
        const uint8_t frame = uint8_t(slot < ClosureList::kMaxFrames - 1 ? slot : ClosureList::kMaxFrames - 1);
        if (Lobe *l = cl.push(src.type, frame, src.weight * w, src.pick * total * scale, src.diffuse)) {
            *l = src;
            l->frame = frame;
            l->weight = src.weight * w;
            l->pick = src.pick * total * scale;
            l->albedoR = src.albedoR * w;
            l->albedoT = src.albedoT * w;
        }
    }
    cl.overflow |= s.overflow;
    cl.emission += s.emission * w;
    cl.albedo += s.albedo * w; // the lobes carry their shares (above)
    frameBase += sFrames;
}

NOVA_INLINE void buildNativeClosure(const SurfaceInputs &in, const ShadingGeometry &g, Vec3f wo, const ClosureContext &ctx,
                                    ClosureList &cl);

// A generic surface: one graph built in place, or several segments, each
// built on its own and appended with its weight.
NOVA_INLINE void buildGraphSurface(const SurfaceInputs &in, const ShadingGeometry &g, Vec3f wo, const ClosureContext &ctx,
                                   ClosureList &cl) {
    const ClosureGraph &G = *in.graph;
    if (G.segmentCount == 1 && G.segment[0].model == SurfaceModel::Graph && G.segment[0].weight == kNoSlot) {
        buildGraphSegment(in, G, G.segment[0], g, wo, ctx, cl);
        cl.finalize();
        return;
    }
    int frameBase = 0;
    float bestT = 0.0f;
    for (int s = 0; s < G.segmentCount; ++s) {
        const GraphSegment &seg = G.segment[s];
        const float w = seg.weight != kNoSlot ? nova::fmax(in.f(seg.weight), 0.0f) : 1.0f;
        if (!(w > kCut))
            continue;
        ClosureList part;
        part.clear();
        part.ng = g.ng;
        int frames;
        if (seg.model == SurfaceModel::Graph) {
            buildGraphSegment(in, G, seg, g, wo, ctx, part);
            part.finalize();
            frames = G.frameCount > 0 ? G.frameCount : 1;
        } else {
            SurfaceInputs native;
            native.model = seg.model;
            native.size = surface_field::kCount;
            native.graph = nullptr;
            std::memcpy(native.v, in.v + seg.base, sizeof(float) * surface_field::kCount);
            buildNativeClosure(native, g, wo, ctx, part);
            frames = 2;
        }
        // The interior absorption of the segment that transmits most.
        float transmitted = 0.0f;
        for (int i = 0; i < part.count; ++i)
            if (part.lobe[i].transmits())
                transmitted += luminance(part.lobe[i].weight);
        if (transmitted * w > bestT) {
            bestT = transmitted * w;
            cl.sigmaA = part.sigmaA;
            cl.absorptionColor = part.absorptionColor;
        }
        appendClosure(cl, part, frames, spectrum(w), frameBase);
    }
    cl.finalize();
}

} // namespace closure_detail
} // namespace nova
