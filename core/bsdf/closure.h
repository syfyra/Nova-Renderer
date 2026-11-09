#pragma once

// A closure: the scattering of one shading point as a flat, weighted list of
// at most kMaxLobes lobes (docs/design/M3.md K1, K13). The surface models
// (core/bsdf/surface.h) build it once per hit for the fixed outgoing
// direction wo, folding every layering, mixing, tint and energy
// compensation factor into the lobe weights, so evaluation and sampling need
// no tables. Fixed-size POD, switch dispatch: the same code runs on CUDA.
//
// Sampling is one-sample MIS over the lobes: lobe j is picked with
// probability p_j (from wo only), sampled, and the result is weighted with
// the full mixture f / sum_k p_k pdf_k. Delta events (smooth interfaces)
// carry their weight directly and are excluded from the mixture and from
// light sampling.
//
// World-space interface: wo and wi point away from the surface. The
// geometric normal decides reflection vs transmission: a lobe contributes
// only on the side its event class allows, so shading normals never leak
// light through the surface.

#include "core/bsdf/conductor.h"
#include "core/bsdf/dielectric.h"
#include "core/bsdf/diffuse.h"
#include "core/bsdf/energy.h"
#include "core/bsdf/hair.h"
#include "core/bsdf/sheen.h"
#include "core/bsdf/subsurface.h"
#include "core/math/frame.h"
#include "core/scene/ray_limits.h"

namespace nova {

// Class of a scattering event, and so of the ray it spawns (Karma's ray
// types, measured: docs/research/m4/karma-nodes.md 4.1). Sheen counts as
// diffuse, coat and specular as glossy reflection, the thin-walled
// subsurface share (translucent) as SSS.
enum class RayClass : uint8_t { Camera, Diffuse, Reflect, Refract, Sss, Volume, Shadow };

enum class LobeType : uint8_t {
    Lambert,     // lambert (reflection)
    Translucent, // lambert (transmission)
    Qon,
    Eon,
    Burley,
    Charlie,
    Zeltner,
    DisneySheen, // Houdini's Principled Shader sheen (M5.1)
    Conductor,
    Dielectric,
    ThinDielectric,
    Subsurface, // a random walk (M9): no value at the entry, sampling it starts the walk
    Hair,       // one Chiang lobe of a fibre (M9): reflects and transmits in any frame
};

// A lobe's BSDF label for light path expressions (docs/design/M6.md D4):
// Karma's component labels. Set explicitly for coats; the others follow
// from the lobe (lpeLabel()).
enum class LobeLabel : uint8_t { None, Diffuse, Reflect, Refract, Coat, Sheen, Sss, kCount };

struct Lobe {
    LobeType type;
    uint8_t frame;   // index into ClosureList::frame (native models: 0 base, 1 coat)
    bool diffuse;    // diffuse class: light diffuse multipliers, ray cones
    bool sss;        // the event class is SSS (subsurface walks, thin-walled translucency)
    LobeLabel label; // explicit label (coat); None: from the lobe
    // The lobe's share of ClosureList::albedo, by the side it scatters to
    // (light path expressions' A events, docs/design/M6.md D8).
    Spectrum albedoR, albedoT;
    Spectrum weight; // every factor of the model at this hit
    float pick;      // selection probability (normalized by finalize())
    union {
        LambertLobe lambert;
        QonLobe qon;
        EonLobe eon;
        BurleyLobe burley;
        CharlieLobe charlie;
        ZeltnerLobe zeltner;
        DisneySheenLobe disneySheen;
        ConductorLobe conductor;
        DielectricLobe dielectric;
        ThinDielectricLobe thin;
        SubsurfaceLobe subsurface;
        HairLobe hair;
    };

    NOVA_INLINE bool lambertian() const { return type == LobeType::Lambert || type == LobeType::Translucent; }

    // f |cos| and pdf in the lobe's frame. Out of line: the closure loops
    // handle the Lambert lobes inline (the default material) and call this
    // for the rest.
    NOVA_OUTLINE Spectrum eval(Vec3f wo, Vec3f wi, float &pdf) const {
        switch (type) {
        case LobeType::Lambert:
        case LobeType::Translucent:
            return lambert.eval(wo, wi, pdf);
        case LobeType::Qon:
            return qon.eval(wo, wi, pdf);
        case LobeType::Eon:
            return eon.eval(wo, wi, pdf);
        case LobeType::Burley:
            return burley.eval(wo, wi, pdf);
        case LobeType::Charlie:
            return charlie.eval(wo, wi, pdf);
        case LobeType::Zeltner:
            return zeltner.eval(wo, wi, pdf);
        case LobeType::DisneySheen:
            return disneySheen.eval(wo, wi, pdf);
        case LobeType::Conductor:
            return conductor.eval(wo, wi, pdf);
        case LobeType::Dielectric:
            return dielectric.eval(wo, wi, pdf);
        case LobeType::Subsurface:
            pdf = 0.0f;
            return spectrum(0.0f);
        case LobeType::Hair:
            return hair.eval(wo, wi, pdf);
        default:
            return thin.eval(wo, wi, pdf);
        }
    }
    NOVA_OUTLINE bool sample(Vec3f wo, Vec2f u, float uc, LobeSample &s) const {
        switch (type) {
        case LobeType::Lambert:
        case LobeType::Translucent:
            return lambert.sample(wo, u, s);
        case LobeType::Qon:
            return qon.sample(wo, u, s);
        case LobeType::Eon:
            return eon.sample(wo, u, uc, s);
        case LobeType::Burley:
            return burley.sample(wo, u, s);
        case LobeType::Charlie:
            return charlie.sample(wo, u, s);
        case LobeType::Zeltner:
            return zeltner.sample(wo, u, s);
        case LobeType::DisneySheen:
            return disneySheen.sample(wo, u, s);
        case LobeType::Conductor:
            return conductor.sample(wo, u, s);
        case LobeType::Dielectric:
            return dielectric.sample(wo, u, uc, s);
        case LobeType::Subsurface:
            return false; // sampleClosure starts the walk
        case LobeType::Hair:
            return hair.sample(wo, u, uc, s);
        default:
            return thin.sample(wo, u, uc, s);
        }
    }
    // Roughness for ray cones: 1 for diffuse and sheen lobes, the larger
    // GGX alpha otherwise (0 when smooth).
    NOVA_INLINE float roughness() const {
        switch (type) {
        case LobeType::Conductor:
            return conductor.ggx.smooth() ? 0.0f : nova::fmax(conductor.ggx.ax, conductor.ggx.ay);
        case LobeType::Dielectric:
            return dielectric.ggx.smooth() ? 0.0f : nova::fmax(dielectric.ggx.ax, dielectric.ggx.ay);
        case LobeType::ThinDielectric:
            return thin.ggxR.smooth() ? 0.0f : nova::fmax(thin.ggxR.ax, thin.ggxR.ay);
        case LobeType::Hair:
            return nova::fmin(std::sqrt(hair.v), 1.0f);
        default:
            return 1.0f;
        }
    }
    // Only delta events (nothing to evaluate for light sampling). A
    // subsurface lobe has no value at its entry either: its light is
    // gathered at the walk's exit.
    NOVA_INLINE bool delta() const {
        switch (type) {
        case LobeType::Subsurface:
            return true;
        case LobeType::Conductor:
            return conductor.ggx.smooth();
        case LobeType::Dielectric:
            return dielectric.ggx.smooth() || std::fabs(dielectric.eta - 1.0f) < kIndexMatched;
        case LobeType::ThinDielectric:
            return thin.ggxR.smooth() && thin.ggxT.smooth();
        default:
            return false;
        }
    }
    // The class of an event of this lobe (transmit: the sampled direction
    // crossed the surface).
    NOVA_INLINE RayClass eventClass(bool transmit) const {
        if (sss)
            return RayClass::Sss;
        switch (type) {
        case LobeType::Translucent:
            return RayClass::Sss;
        case LobeType::Conductor:
            return RayClass::Reflect;
        case LobeType::Dielectric:
        case LobeType::ThinDielectric:
        case LobeType::Hair: // by the side of the tube normal (Karma, measured)
            return transmit ? RayClass::Refract : RayClass::Reflect;
        default:
            return RayClass::Diffuse;
        }
    }
    // Light path expression events (M6): diffuse (D) or glossy (G; delta
    // lobes too, as in Karma), and the label.
    NOVA_INLINE bool lpeDiffuse() const {
        return type != LobeType::Conductor && type != LobeType::Dielectric && type != LobeType::ThinDielectric &&
               type != LobeType::Hair;
    }
    NOVA_INLINE LobeLabel lpeLabel(bool transmit) const {
        if (label != LobeLabel::None)
            return label;
        if (sss || type == LobeType::Translucent)
            return LobeLabel::Sss;
        switch (type) {
        case LobeType::Charlie:
        case LobeType::Zeltner:
        case LobeType::DisneySheen:
            return LobeLabel::Sheen;
        case LobeType::Conductor:
            return LobeLabel::Reflect;
        case LobeType::Dielectric:
        case LobeType::ThinDielectric:
        case LobeType::Hair:
            return transmit ? LobeLabel::Refract : LobeLabel::Reflect;
        default:
            return LobeLabel::Diffuse;
        }
    }
    // SSS events are transmissions on either side (Karma's sss is <TD>).
    NOVA_INLINE bool lpeTransmit(bool transmit) const { return transmit || sss || type == LobeType::Translucent; }

    // Which sides the lobe scatters to (light samples; a subsurface lobe
    // takes none at its entry).
    NOVA_INLINE bool reflects() const { return type != LobeType::Translucent && type != LobeType::Subsurface; }
    NOVA_INLINE bool transmits() const {
        return type == LobeType::Translucent || type == LobeType::ThinDielectric || type == LobeType::Hair ||
               (type == LobeType::Dielectric && (dielectric.mode & DielectricLobe::kTransmit));
    }
};

// What evaluating the closure in a direction gives: f |cos| of all
// non-delta lobes, the diffuse-class part of it and the mixture density.
struct ClosureEval {
    Spectrum fcos;
    Spectrum fcosDiffuse;
    float pdf;
    float pdfGuided; // the guided lobes' part of pdf (ClosureList::guideMask)
};

struct ClosureSample {
    Vec3f wi;
    Spectrum weight;    // f |cos| / pdf (delta: the event weight)
    float pdf;          // mixture density; 0 for delta events
    float pdfGuided;    // the guided lobes' part of pdf (ClosureList::guideMask)
    float diffuseShare; // diffuse-class share of the weight (light multipliers)
    float roughness;    // of the sampled lobe (ray cones)
    float eta;          // relative index of a refraction (n_transmitted / n_incident), else 1
    bool delta;
    bool transmit;
    bool refract;       // the path enters or leaves a medium (not thin-walled, not index-matched)
    bool walk;          // a subsurface lobe: the path continues with a random walk (wi unused)
    RayClass rayClass;  // class of the sampled lobe's event
    int8_t lobe;        // the sampled lobe (-1: an external closure)
};

// An external (VEX) closure (docs/design/M5.md D6, D7): the CVEX bridge
// evaluated the light direction NEE draws and sampled the continuation for
// this hit; evalClosure and sampleClosure replay both.
struct ExternalClosure {
    Vec3f wiLight;           // the evaluated light direction (zero: none)
    Spectrum lightF;         // f cos there
    float lightPdf = 0.0f;   // MIS density there
    Vec3f dir;               // the continuation
    Spectrum weight;         // its throughput weight
    float pdf = 0.0f;        // its MIS density (0: a delta event)
    RayClass rayClass = RayClass::Diffuse;
    bool active = false;     // the closure is external
    bool valid = false;      // a continuation exists
};

struct ClosureList {
    static constexpr int kMaxLobes = 16;
    static constexpr int kMaxFrames = 4;

    Lobe lobe[kMaxLobes];
    float cdf[kMaxLobes];
    Frame frame[kMaxFrames];
    Vec3f ng;          // geometric normal (authored orientation)
    Spectrum emission; // radiance emitted towards wo
    Spectrum albedo;   // sum of weight x albedo at wo (AOV, denoiser guide)
    Spectrum sigmaA;   // absorption inside the medium a transmission enters (0: none)
    uint8_t count;
    bool overflow;         // a lobe was dropped or replaced (more than kMaxLobes)
    uint16_t nonDeltaMask; // lobes that can be evaluated (light sampling)
    // Lobes whose directions path guiding mixes with its own (core/
    // integrator/guiding.h): evaluation reports their share of the density.
    uint16_t guideMask;
    // Sum of the lobes' estimates at the last finalize() (before the
    // normalization): a finalized closure's absolute scale, which appending
    // it to another needs (its albedo misses lobes without one, e.g. unlit).
    float estimateSum;
    ExternalClosure external; // SurfaceModel::External

    NOVA_INLINE void clear() {
        count = 0;
        overflow = false;
        nonDeltaMask = 0;
        guideMask = 0;
        estimateSum = 0.0f;
        emission = albedo = sigmaA = spectrum(0.0f);
        external.active = false;
    }

    // Appends a lobe; `estimate` is its expected reflected energy at wo (the
    // weight times its albedo, as luminance) for lobe selection. Lobes with
    // a negligible weight are dropped. A full list replaces the lobe with
    // the smallest estimate if the new one is larger (its energy is lost,
    // its share of the albedo AOV stays).
    NOVA_INLINE Lobe *push(LobeType type, uint8_t frameIndex, Spectrum weight, float estimate, bool diffuse) {
        if (!(maxComponent(weight) > 1e-6f))
            return nullptr;
        int slot = count;
        if (count >= kMaxLobes) {
            overflow = true;
            slot = 0;
            for (int i = 1; i < count; ++i)
                if (lobe[i].pick < lobe[slot].pick)
                    slot = i;
            if (!(nova::fmax(estimate, 0.0f) > lobe[slot].pick))
                return nullptr;
        } else {
            ++count;
        }
        Lobe &l = lobe[slot];
        l.type = type;
        l.frame = frameIndex;
        l.diffuse = diffuse;
        l.sss = false;
        l.label = LobeLabel::None;
        l.albedoR = l.albedoT = spectrum(0.0f);
        l.weight = weight;
        l.pick = nova::fmax(estimate, 0.0f);
        return &l;
    }

    // Adds a lobe's albedo at wo (reflected and transmitted parts) to the
    // closure's albedo.
    NOVA_INLINE void addAlbedo(Lobe &l, Spectrum reflected, Spectrum transmitted = {0.0f, 0.0f, 0.0f}) {
        albedo += reflected + transmitted;
        l.albedoR += reflected;
        l.albedoT += transmitted;
    }

    // Normalizes the selection probabilities: proportional to the energy
    // estimates, floored at 1e-3 of their sum so every lobe can be picked.
    NOVA_INLINE void finalize() {
        if (count == 1) { // the common single-lobe closure
            estimateSum = lobe[0].pick;
            lobe[0].pick = 1.0f;
            cdf[0] = 1.0f;
            nonDeltaMask = lobe[0].delta() ? 0 : 1;
            return;
        }
        float total = 0.0f;
        for (int i = 0; i < count; ++i)
            total += lobe[i].pick;
        estimateSum = total;
        const float floor = total > 0.0f ? 1e-3f * total : 1.0f;
        float sum = 0.0f;
        for (int i = 0; i < count; ++i) {
            lobe[i].pick = nova::fmax(lobe[i].pick, floor);
            sum += lobe[i].pick;
        }
        float c = 0.0f;
        nonDeltaMask = 0;
        for (int i = 0; i < count; ++i) {
            lobe[i].pick /= sum;
            c += lobe[i].pick;
            cdf[i] = c;
            if (!lobe[i].delta())
                nonDeltaMask |= uint16_t(1u << i);
        }
        if (count > 0)
            cdf[count - 1] = 1.0f;
    }
};

// Fake caustics (docs/design/M4.md 3.3): what a straight line through the
// surface keeps at wo's angle: the smooth interface transmission 1 - F times
// the transmissive lobes' weights (their energy compensation undone: the
// roughness is ignored, as in Karma) and tints. The line is not bent, so both
// sides use the outside angle with the index above 1: by Fresnel's
// reciprocity the exit of a slab reflects what its entry does, and there is
// no total reflection. A closed object crossed twice keeps (1 - F)^2. Thin
// walls keep their sheet's transmission.
NOVA_INLINE Spectrum closureTransmittance(const ClosureList &cl, Vec3f wo, const BsdfTables &tables) {
    Spectrum T{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < cl.count; ++i) {
        const Lobe &l = cl.lobe[i];
        const Vec3f woL = cl.frame[l.frame].toLocal(wo);
        const float mu = nova::fmax(std::fabs(woL.z), 1e-4f);
        if (l.type == LobeType::Dielectric && (l.dielectric.mode & DielectricLobe::kTransmit)) {
            const float e = l.dielectric.eta >= 1.0f ? l.dielectric.eta : 1.0f / l.dielectric.eta;
            const GgxDistribution &g = l.dielectric.ggx;
            float cosT;
            const float t = 1.0f - fresnelDielectric(mu, e, cosT);
            // pushTransmission weighs an R+T lobe by 1 / E_RT (seen from wo).
            const float uncompensate = g.smooth() || l.dielectric.mode != DielectricLobe::kBoth
                                           ? 1.0f
                                           : dielectricAlbedoRT(tables, l.dielectric.eta, lutRough(g.ax, g.ay), mu);
            T += l.weight * l.dielectric.tintT * (t * uncompensate);
        } else if (l.type == LobeType::ThinDielectric) {
            // The smooth sheet at mu: with its own specular-weight boost and
            // the compensation 1 / E undone (roughness ignored).
            ThinDielectricLobe smooth = l.thin;
            smooth.tBoost = l.thin.tBoostSmooth;
            Spectrum R, Ts;
            smooth.sheet(mu, R, Ts);
            const GgxDistribution &g = l.thin.ggxR;
            const float e = g.smooth() ? 1.0f : thinDielectricAlbedo(tables, l.thin.eta, lutRough(g.ax, g.ay), mu);
            T += l.weight * Ts * e;
        }
    }
    return T;
}

// Karma's fake-caustic transmittance (measured, docs/design/M11-P1.md): per
// transmissive lobe 1 - F with Schlick's F at wo's angle, a second 1 - F
// where the surface reflects on wo's side (outside; inside only with
// internal reflection), the lobe's weight and transmission tint; roughness
// and the energy compensation of rough lobes ignored. Thin walls reflect on
// both sides.
NOVA_INLINE Spectrum karmaTransmittance(const ClosureList &cl, Vec3f wo, bool internalReflection,
                                        const BsdfTables &tables) {
    Spectrum T{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < cl.count; ++i) {
        const Lobe &l = cl.lobe[i];
        const Vec3f woL = cl.frame[l.frame].toLocal(wo);
        const float mu = nova::fmin(std::fabs(woL.z), 1.0f);
        const float m5 = sqr(sqr(1.0f - mu)) * (1.0f - mu);
        if (l.type == LobeType::Dielectric && (l.dielectric.mode & DielectricLobe::kTransmit)) {
            // The frames face wo; the inside's lobe has the inverse index.
            const bool outside = l.dielectric.eta >= 1.0f;
            const float e = outside ? l.dielectric.eta : 1.0f / l.dielectric.eta;
            const float F0 = sqr((e - 1.0f) / (e + 1.0f));
            const float t = 1.0f - (F0 + (1.0f - F0) * m5);
            const bool reflects = outside || internalReflection;
            const GgxDistribution &g = l.dielectric.ggx;
            const float uncompensate = g.smooth() || l.dielectric.mode != DielectricLobe::kBoth
                                           ? 1.0f
                                           : dielectricAlbedoRT(tables, l.dielectric.eta, lutRough(g.ax, g.ay), mu);
            T += l.weight * l.dielectric.tintT * (t * (reflects ? t : 1.0f) * uncompensate);
        } else if (l.type == LobeType::ThinDielectric) {
            const float F0 = sqr((l.thin.eta - 1.0f) / (l.thin.eta + 1.0f));
            const float t = 1.0f - (F0 + (1.0f - F0) * m5);
            const GgxDistribution &g = l.thin.ggxR;
            const float e = g.smooth() ? 1.0f : thinDielectricAlbedo(tables, l.thin.eta, lutRough(g.ax, g.ay), mu);
            T += l.weight * l.thin.tintT * (t * t * e);
        }
    }
    return T;
}

// Schlick's bias, Karma's fakecausticsfresneldarken d (measured per
// crossing: mu / ((1 / (1 - d) - 2) (1 - mu) + 1); d = 0.5 gives mu, 1
// blocks the light).
NOVA_INLINE float fakeCausticDarken(float mu, float d) {
    if (!(d > 0.0f))
        return 1.0f;
    if (d >= 1.0f)
        return 0.0f;
    return mu / ((1.0f / (1.0f - d) - 2.0f) * (1.0f - mu) + 1.0f);
}

// What a fake-caustic surface passes per crossing (docs/design/M4.md 3.3,
// docs/design/M11-P1.md): Nova's physical transmittance or Karma's, or with
// Karma's BSDF switch off the colour alone (surfaces that transmit at all);
// tinted by Karma's colour, darkened towards grazing angles, faded towards 1
// by its opacity.
NOVA_INLINE Spectrum fakeCausticFactor(const ClosureList &cl, Vec3f wo, const ObjectShading &s,
                                       const BsdfTables &tables) {
    const FakeCausticsStyle &f = s.fake;
    const Spectrum color{f.color[0], f.color[1], f.color[2]};
    Spectrum t;
    if (!f.bsdf) {
        bool transmits = false;
        for (int i = 0; i < cl.count; ++i)
            transmits |= (cl.lobe[i].type == LobeType::Dielectric &&
                          (cl.lobe[i].dielectric.mode & DielectricLobe::kTransmit)) ||
                         cl.lobe[i].type == LobeType::ThinDielectric;
        if (!transmits)
            return spectrum(0.0f); // opaque whatever the controls
        t = color;
    } else {
        t = (f.karma ? karmaTransmittance(cl, wo, s.internalReflection, tables) : closureTransmittance(cl, wo, tables)) *
            color;
        if (isBlack(t) && f.color[0] > 0.0f && f.color[1] > 0.0f && f.color[2] > 0.0f)
            return t; // opaque (no transmission) whatever the opacity
    }
    if (f.fresnelDarken > 0.0f)
        t *= fakeCausticDarken(nova::fmin(std::fabs(dot(wo, cl.ng)), 1.0f), f.fresnelDarken);
    const Spectrum o{f.opacity[0], f.opacity[1], f.opacity[2]};
    return spectrum(1.0f) - o + t * o;
}

// Karma's internalreflectenable off (docs/design/M11-P1.md): seen from the
// inside of a refracting interface (the surface models face the frames to
// wo and give the inside's lobes the inverse index), its dielectric lobes
// have no Fresnel reflection, only total internal reflection
// (DielectricLobe::noInternal), and reflection-only dielectric lobes of the
// inside (a split reflection) are dropped.
NOVA_INLINE void dropInternalReflection(ClosureList &cl, Vec3f) {
    bool inside = false;
    for (int i = 0; i < cl.count; ++i) {
        Lobe &l = cl.lobe[i];
        if (l.type == LobeType::Dielectric && (l.dielectric.mode & DielectricLobe::kTransmit) &&
            l.dielectric.eta < 1.0f) {
            l.dielectric.noInternal = 1;
            inside = true;
        }
    }
    if (!inside)
        return;
    int n = 0;
    for (int i = 0; i < cl.count; ++i) {
        const Lobe &l = cl.lobe[i];
        if (!(l.type == LobeType::Dielectric && l.dielectric.mode == DielectricLobe::kReflect && l.dielectric.eta < 1.0f))
            cl.lobe[n++] = l;
    }
    if (n != cl.count) {
        cl.count = uint8_t(n);
        cl.finalize();
    }
}

// The GGX alpha of a glossy lobe (0: not glossy).
NOVA_INLINE float glossyAlpha(const Lobe &l) {
    switch (l.type) {
    case LobeType::Conductor:
        return nova::fmax(l.conductor.ggx.ax, l.conductor.ggx.ay);
    case LobeType::Dielectric:
        return nova::fmax(l.dielectric.ggx.ax, l.dielectric.ggx.ay);
    case LobeType::ThinDielectric:
        return nova::fmax(l.thin.ggxR.ax, l.thin.ggxR.ay);
    default:
        return 0.0f;
    }
}

// Raises the GGX alpha of the glossy lobes to at least `alpha` (Karma's
// constrainmaxrough and causticsroughnessclamp, docs/design/M11-P1.md);
// lobes that stop being smooth take part in light sampling. The weights
// keep the compensation of their own roughness.
NOVA_INLINE void clampLobeRoughness(ClosureList &cl, float alpha) {
    if (!(alpha > kMinAlpha))
        return;
    bool changed = false;
    const auto raise = [&](GgxDistribution &g) {
        if (g.ax < alpha || g.ay < alpha) {
            g = GgxDistribution::make(nova::fmax(g.ax, alpha), nova::fmax(g.ay, alpha));
            changed = true;
        }
    };
    for (int i = 0; i < cl.count; ++i) {
        Lobe &l = cl.lobe[i];
        if (l.type == LobeType::Conductor) {
            raise(l.conductor.ggx);
        } else if (l.type == LobeType::Dielectric) {
            raise(l.dielectric.ggx);
        } else if (l.type == LobeType::ThinDielectric) {
            const GgxDistribution before = l.thin.ggxR;
            raise(l.thin.ggxR);
            if (l.thin.ggxR.ax != before.ax || l.thin.ggxR.ay != before.ay)
                l.thin.ggxT = GgxDistribution::make(ThinDielectricLobe::transmissionAlpha(l.thin.ggxR.ax, l.thin.eta),
                                                    ThinDielectricLobe::transmissionAlpha(l.thin.ggxR.ay, l.thin.eta));
        }
    }
    if (!changed)
        return;
    cl.nonDeltaMask = 0;
    for (int i = 0; i < cl.count; ++i)
        if (!cl.lobe[i].delta())
            cl.nonDeltaMask |= uint16_t(1u << i);
}

// The closure without its non-diffuse lobes (fake caustics after a diffuse
// event: glossy reflection is skipped, transmission becomes a pass-through).
NOVA_INLINE void keepDiffuseLobes(ClosureList &cl) {
    int n = 0;
    for (int i = 0; i < cl.count; ++i)
        if (cl.lobe[i].diffuse)
            cl.lobe[n++] = cl.lobe[i];
    cl.count = uint8_t(n);
    cl.finalize();
}

// No per-lobe results.
struct NoLobeSink {
    NOVA_INLINE void operator()(int, const Spectrum &) const {}
};

// `perLobe(i, fcos)` receives each evaluated lobe's f |cos| (light path
// expressions split it by component, docs/design/M6.md D5).
template <typename PerLobe = NoLobeSink>
NOVA_INLINE ClosureEval evalClosure(const ClosureList &cl, Vec3f wo, Vec3f wi, const PerLobe &perLobe = PerLobe{}) {
    ClosureEval e{spectrum(0.0f), spectrum(0.0f), 0.0f, 0.0f};
    if (cl.external.active) {
        // Only the direction the bridge evaluated (VEX's eval has no
        // diffuse/specular split: all of it takes the light's diffuse share).
        if (dot(wi, cl.external.wiLight) > 0.999999f) {
            e.fcos = e.fcosDiffuse = cl.external.lightF;
            e.pdf = cl.external.lightPdf;
        }
        return e;
    }
    const bool geometricTransmit = dot(wo, cl.ng) * dot(wi, cl.ng) < 0.0f;
    for (int i = 0; i < cl.count; ++i) {
        if (!(cl.nonDeltaMask & (1u << i)))
            continue;
        const Lobe &l = cl.lobe[i];
        if (geometricTransmit ? !l.transmits() : !l.reflects())
            continue;
        const Frame &f = cl.frame[l.frame];
        float pdf = 0.0f;
        Spectrum c;
        if (l.lambertian()) {
            // The frame faces wo's geometric side; as in Cycles (and Karma),
            // a diffuse lobe sees max(n.wi, 0) whatever side of a bent
            // shading normal wo is on (a normal-mapped rim stays lit).
            const float cosI = dot(wi, f.n);
            if ((cosI < 0.0f) != geometricTransmit)
                continue;
            c = l.weight * l.lambert.eval({0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, cosI}, pdf);
        } else {
            const Vec3f woL = f.toLocal(wo), wiL = f.toLocal(wi);
            if (l.type != LobeType::Hair && (woL.z * wiL.z < 0.0f) != geometricTransmit)
                continue; // the lobe's frame disagrees with the geometry (a fibre's frame has no side)
            c = l.weight * l.eval(woL, wiL, pdf);
        }
        e.fcos += c;
        if (l.diffuse)
            e.fcosDiffuse += c;
        e.pdf += l.pick * pdf;
        if (cl.guideMask & (1u << i))
            e.pdfGuided += l.pick * pdf;
        perLobe(i, c);
    }
    return e;
}

// u: x, y for the direction, z picks the lobe; its remainder drives the
// lobe's own discrete choice (reflection vs transmission, ...), which keeps
// the stratified xy pair intact. `perLobe` receives the lobes' f |cos| at
// the sampled direction when the weight is the mixture's (several lobes,
// a non-delta sample).
template <typename PerLobe = NoLobeSink>
NOVA_INLINE bool sampleClosure(const ClosureList &cl, Vec3f wo, Vec3f u, ClosureSample &s,
                               const PerLobe &perLobe = PerLobe{}) {
    if (cl.external.active) {
        const ExternalClosure &x = cl.external;
        if (!x.valid)
            return false;
        s.wi = x.dir;
        s.weight = x.weight;
        s.delta = !(x.pdf > 0.0f);
        s.pdf = s.delta ? 0.0f : x.pdf;
        s.pdfGuided = 0.0f;
        s.transmit = dot(wo, cl.ng) * dot(x.dir, cl.ng) < 0.0f;
        s.rayClass = x.rayClass;
        s.roughness = x.rayClass == RayClass::Diffuse || x.rayClass == RayClass::Sss ? 1.0f : 0.5f;
        s.refract = false; // no medium tracking through VEX
        s.walk = false;
        s.eta = 1.0f;
        s.lobe = -1;
        s.diffuseShare = x.rayClass == RayClass::Diffuse ? 1.0f : 0.0f;
        return isFinite(s.weight);
    }
    if (cl.count == 0)
        return false;
    int j = 0;
    float p = 1.0f, uc = u.z;
    if (cl.count > 1) {
        while (j + 1 < cl.count && u.z >= cl.cdf[j])
            ++j;
        const float lo = j > 0 ? cl.cdf[j - 1] : 0.0f;
        p = cl.lobe[j].pick;
        uc = p > 0.0f ? nova::fmin((u.z - lo) / p, 0.99999994f) : 0.0f;
    }
    const Lobe &l = cl.lobe[j];
    const Frame &f = cl.frame[l.frame];
    s.lobe = int8_t(j);
    s.walk = false;
    s.pdfGuided = 0.0f;
    if (l.type == LobeType::Subsurface) {
        // A walk into the surface: its weight, picked with probability p; the
        // walk and its exit do the rest (one SSS event, like a delta event
        // for MIS and light path expressions).
        s.walk = true;
        s.wi = -cl.ng; // unused
        s.weight = l.weight * (1.0f / p);
        s.pdf = 0.0f;
        s.delta = true;
        s.transmit = true;
        s.refract = false;
        s.eta = 1.0f;
        s.rayClass = RayClass::Sss;
        s.roughness = 1.0f;
        s.diffuseShare = 1.0f;
        return isFinite(s.weight);
    }
    LobeSample ls;
    const Vec3f woL = f.toLocal(wo);
    // Diffuse lobes sample the frame's own hemisphere (see evalClosure).
    if (!(l.lambertian() ? l.lambert.sample({0.0f, 0.0f, 1.0f}, {u.x, u.y}, ls) : l.sample(woL, {u.x, u.y}, uc, ls)))
        return false;
    s.wi = normalize(f.toWorld(ls.wi));
    const bool geometricTransmit = dot(wo, cl.ng) * dot(s.wi, cl.ng) < 0.0f;
    if (l.type == LobeType::Hair)
        ls.transmit = geometricTransmit; // a fibre scatters to both sides
    else if (geometricTransmit != ls.transmit)
        return false; // the shading normal sent it to the wrong side
    s.transmit = ls.transmit;
    s.rayClass = l.eventClass(ls.transmit);
    s.roughness = l.roughness();
    s.refract = ls.transmit && l.type == LobeType::Dielectric && std::fabs(l.dielectric.eta - 1.0f) >= kIndexMatched;
    s.eta = s.refract ? l.dielectric.eta : 1.0f;
    if (ls.delta) {
        s.delta = true;
        s.pdf = 0.0f;
        s.weight = l.weight * ls.fcos * (1.0f / p);
        s.diffuseShare = 0.0f;
        return isFinite(s.weight);
    }
    s.delta = false;
    if (cl.count == 1) {
        // The mixture is the lobe itself (picked with probability 1).
        if (!(ls.pdf > 0.0f))
            return false;
        s.pdf = ls.pdf;
        s.pdfGuided = (cl.guideMask & 1u) ? ls.pdf : 0.0f;
        // Lambert: f |cos| / pdf is the albedo itself.
        s.weight = l.lambertian() ? l.weight * l.lambert.albedo : l.weight * ls.fcos * (1.0f / ls.pdf);
        s.diffuseShare = l.diffuse ? 1.0f : 0.0f;
        return isFinite(s.weight);
    }
    const ClosureEval e = evalClosure(cl, wo, s.wi, perLobe);
    if (!(e.pdf > 0.0f))
        return false;
    s.pdf = e.pdf;
    s.pdfGuided = e.pdfGuided;
    s.weight = e.fcos * (1.0f / e.pdf);
    const float total = luminance(e.fcos);
    s.diffuseShare = total > 0.0f ? saturate(luminance(e.fcosDiffuse) / total) : 0.0f;
    return isFinite(s.weight);
}

} // namespace nova
