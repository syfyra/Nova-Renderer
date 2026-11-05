#pragma once

// Volumes along rays (docs/design/M8.md D8, D9, D18), shared by the CPU
// (volume_media.cpp) and the GPU kernels (docs/design/M10.md G5): the
// volumes a ray crosses, ratio tracking through them on the majorant cells
// (an unbiased per-channel transmittance) and free flights that may
// scatter; both collect, at every tentative collision, the volumes'
// emission (T ε / σ̄; research A E7, "tentative collisions times
// ratio-tracked T, never absorbed"). Overlapping volumes are one medium:
// their cells' majorants add, and so do their coefficients at a point (D8).
//
// Media: the scene's volumes as the trackers see them, with
//   uint32_t activeCount() const; VolumeId active(uint32_t i) const;
//   MediumVolume volume(VolumeId) const;
//   Affine3f volumeToWorld(VolumeId, float time) const;
//   VolumePoint evaluate(VolumeId, Vec3f pObject, Vec3f pWorld, Vec3f dir,
//                        float time, uint8_t rayType, uint32_t depth);
//   void violation(const Spectrum &extinction, float majorant);
// Lists: std::vector-like (clear, push_back, size, operator[], erase(i))
// of VolumeSpan, MediumActive and MediumHit (the CPU's grow, the GPU's are
// fixed: FixedList).

#include "core/lights/light_tree.h"
#include "core/lights/lights.h"
#include "core/math/distribution.h"
#include "core/math/rng.h"
#include "core/scene/motion.h"
#include "core/volume/majorant.h"
#include "core/volume/phase.h"
#include "core/volume/volume_point.h"

#include <cmath>
#include <cstdint>

namespace nova {

using VolumeId = uint32_t;

// A volume's data the trackers read (VolumeRecord's): its world box over
// the shutter, majorant cells (with the emission floor; without it, for
// rays that collect no emission, null: the same), the light it is (kInvalidId:
// none) with its cells' distribution and cell volume, and the reference
// field's object-to-index map. The layout stays where it is: the DDAs keep
// pointing at it.
struct MediumVolume {
    Vec3f lower{1.0f, 1.0f, 1.0f}, upper{-1.0f, -1.0f, -1.0f};
    const MajorantLayout *layout = nullptr;
    const float *majorant = nullptr;
    const float *extinctionMajorant = nullptr;
    uint32_t lightIndex = 0xFFFFFFFFu;
    Distribution1DView lightCells;
    float cellVolume = 0.0f;
    Affine3f objectToIndex;
};

// A volume a ray crosses: [t0, t1) and the ray in the volume's reference
// index space and object space (t stays the world distance).
struct VolumeSpan {
    VolumeId id = 0;
    float t0 = 0.0f, t1 = 0.0f;
    Vec3f oIndex, dIndex, oObject, dObject;
};

struct MediumRay {
    Vec3f o, d;
    float tEnd = 0.0f;
    float time = 0.0f;
    uint8_t rayType = 0; // what the shader sees as the ray class (shadow for transmittance queries)
    uint32_t depth = 0;
};

// The emission a ray collects at its tentative collisions (radiance per
// unit throughput): that of volumes that are not lights (an O event; Karma:
// only rays that may scatter), and that of volume lights (D13: an L event
// for every ray that gathers lights), weighted by MIS against their light
// samples and by the lights' multipliers.
struct EmissionGather {
    bool objects = true; // the emission of volumes that are not lights
    bool lights = true;  // the emission of volume lights
    // MIS: the density of the ray's direction (negative: none, for camera
    // rays and after delta events), the strategy and the lights' selection
    // probabilities. The lights' multipliers with the ray's diffuse share
    // (not for camera rays).
    float bsdfPdf = -1.0f;
    float diffuseShare = 1.0f;
    bool multipliers = false;
    LightStrategy strategy = LightStrategy::MIS;
    const LightData *lightData = nullptr;
    // The lights' selection probabilities at the vertex the ray left: the
    // light tree's there (M9), or the table's.
    const LightTreeView *tree = nullptr;
    const float *tablePmf = nullptr;
    Vec3f prevP{0.0f, 0.0f, 0.0f}, prevN{0.0f, 0.0f, 0.0f};
    Receiver prevReceiver = Receiver::None;
    uint32_t prevSet = 0; // its light-linking receiver set (M9 D15)
    LightLinks links;     // light linking (null: none)
    NOVA_INLINE float lightPmf(uint32_t li) const {
        return tree ? tree->pmf(li, prevP, prevN, prevReceiver, prevSet) : tablePmf[li];
    }
    NOVA_INLINE bool linksLight(uint32_t li) const { return links.links(li, prevSet); }
    // Out: the sums; per light up to kSlots lights (further ones add to
    // the last slot).
    static constexpr int kSlots = 4;
    Spectrum objectSum{0.0f, 0.0f, 0.0f};
    int lightCount = 0;
    uint32_t lightIndex[kSlots] = {};
    Spectrum lightSum[kSlots];
};

// The outcome of a free flight: a real scattering event at distance t in
// `volume`, or none (the ray reaches its end). `weight` multiplies the path
// throughput up to the event or the end (with the event's albedo weight).
struct MediumFlight {
    bool scattered = false;
    float t = 0.0f;
    VolumeId volume = 0;
    PhaseFunction phase;
    Spectrum weight{1.0f, 1.0f, 1.0f};
    int crossings = 0; // the crossT before the event (all without one)
};

// The medium at distance t along the ray (a point query, D17): each covering
// volume's scattering, phase function and id.
struct MediumPart {
    Spectrum scattering;
    PhaseFunction phase;
    VolumeId volume;
};

// A volume the walker is inside: its span, data, current majorant cell
// [a, b) and the majorants it uses.
struct MediumActive {
    const VolumeSpan *span = nullptr;
    MediumVolume vol;
    const float *majorant = nullptr; // with the emission floor if the ray collects its emission
    MajorantDda dda;
    size_t cell = 0;
    float a = 0.0f, b = 0.0f;
};

// A covering volume at a free flight's tentative collision.
struct MediumHit {
    Spectrum scattering;
    Spectrum emission;
    VolumeId volume;
    const MediumActive *active;
    PhaseFunction phase;
};

// A list of at most N elements in place (the GPU's lists; more are dropped).
template <class T, int N>
struct FixedList {
    T items[N];
    uint32_t count = 0;

    NOVA_INLINE void clear() { count = 0; }
    NOVA_INLINE size_t size() const { return count; }
    NOVA_INLINE bool empty() const { return count == 0; }
    NOVA_INLINE void push_back(const T &v) {
        if (count < uint32_t(N))
            items[count++] = v;
    }
    NOVA_INLINE void erase(size_t i) {
        for (size_t k = i + 1; k < count; ++k)
            items[k - 1] = items[k];
        --count;
    }
    NOVA_INLINE T &operator[](size_t i) { return items[i]; }
    NOVA_INLINE const T &operator[](size_t i) const { return items[i]; }
    NOVA_INLINE T &front() { return items[0]; }
    NOVA_INLINE const T &front() const { return items[0]; }
};

namespace medium_detail {
template <class T>
NOVA_INLINE void swapValues(T &a, T &b) {
    T c = a;
    a = b;
    b = c;
}
} // namespace medium_detail

// The volumes a ray (unit direction) crosses before tEnd at shutter time
// `time`, by entry distance.
template <class Media, class Spans>
NOVA_INLINE void volumeSpans(const Media &media, Vec3f o, Vec3f d, float tEnd, float time, Spans &spans) {
    spans.clear();
    for (uint32_t i = 0; i < media.activeCount(); ++i) {
        const VolumeId id = media.active(i);
        const MediumVolume rec = media.volume(id);
        // The world box over the shutter first (slab test).
        float t0 = 0.0f, t1 = tEnd;
        const float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
        const float lo[3] = {rec.lower.x, rec.lower.y, rec.lower.z}, hi[3] = {rec.upper.x, rec.upper.y, rec.upper.z};
        for (int a = 0; a < 3 && t0 <= t1; ++a) {
            if (dd[a] == 0.0f) {
                if (oo[a] < lo[a] || oo[a] > hi[a])
                    t1 = -1.0f;
                continue;
            }
            const float inv = 1.0f / dd[a];
            float ta = (lo[a] - oo[a]) * inv, tb = (hi[a] - oo[a]) * inv;
            if (ta > tb)
                medium_detail::swapValues(ta, tb);
            t0 = t0 < ta ? ta : t0; // std::max
            t1 = tb < t1 ? tb : t1; // std::min
        }
        if (!(t0 < t1))
            continue;
        VolumeSpan s;
        s.id = id;
        s.t0 = t0;
        s.t1 = t1;
        const Affine3f worldToObject = inverseAffine(media.volumeToWorld(id, time));
        s.oObject = worldToObject.point(o);
        s.dObject = worldToObject.vector(d);
        s.oIndex = rec.objectToIndex.point(s.oObject);
        s.dIndex = rec.objectToIndex.vector(s.dObject);
        // By entry distance (insertion).
        spans.push_back(s);
        for (size_t k = spans.size() - 1; k > 0 && spans[k - 1].t0 > spans[k].t0; --k)
            medium_detail::swapValues(spans[k - 1], spans[k]);
    }
}

// The volumes along a ray merged into one majorant (D8): consecutive
// intervals [a, b) over which the volumes covering them, and so the sum of
// their cells' majorants, stay the same. Overlapping volumes add their
// majorants and, at a point, their coefficients.
template <class Media, class Spans, class Actives>
class MediaWalker {
public:
    // `gather`: what the ray collects (null: transmittance only).
    NOVA_INLINE MediaWalker(Media &media, Spans &spans, Actives &active, const MediumRay &ray,
                            const EmissionGather *gather)
        : m_media(media), m_ray(ray), m_gather(gather), m_spans(spans), m_active(active) {
        volumeSpans(media, ray.o, ray.d, ray.tEnd, ray.time, m_spans);
        m_active.clear();
    }

    // The next interval and its summed majorant; false past the last volume.
    // The volumes covering it stay current until the next call.
    NOVA_INLINE bool next(float &a, float &b, float &sigma) {
        for (;;) {
            retire();
            while (m_pending < m_spans.size() && m_spans[m_pending].t0 <= m_cursor)
                activate(m_spans[m_pending++]);
            if (m_active.empty()) {
                if (m_pending >= m_spans.size())
                    return false;
                m_cursor = m_spans[m_pending].t0;
                continue;
            }
            float end = m_pending < m_spans.size() ? m_spans[m_pending].t0 : kInf;
            sigma = 0.0f;
            for (size_t i = 0; i < m_active.size(); ++i) {
                const MediumActive &v = m_active[i];
                if (v.a > m_cursor) { // its cells start later
                    end = v.a < end ? v.a : end;
                    continue;
                }
                end = v.b < end ? v.b : end;
                sigma += v.majorant[v.cell];
            }
            a = m_start = m_cursor;
            b = m_cursor = end;
            if (b > a)
                return true;
        }
    }

    // The volumes covering distance t of the current interval, in order.
    template <typename F>
    NOVA_INLINE void forEachAt(float t, F &&f) {
        for (size_t i = 0; i < m_active.size(); ++i) {
            const MediumActive &v = m_active[i];
            if (v.a <= t && t < v.b)
                f(v, m_media.evaluate(v.span->id, v.span->oObject + v.span->dObject * t, m_ray.o + m_ray.d * t,
                                      m_ray.d, m_ray.time, m_ray.rayType, m_ray.depth));
        }
    }

private:
    // The cells that ended with the last interval [m_start, m_cursor) move
    // on; volumes past their last cell leave.
    NOVA_INLINE void retire() {
        for (size_t i = m_active.size(); i-- > 0;) {
            MediumActive &v = m_active[i];
            if (v.a <= m_start && v.b <= m_cursor && !v.dda.next(v.cell, v.a, v.b))
                m_active.erase(i);
        }
    }
    NOVA_INLINE void activate(const VolumeSpan &s) {
        MediumActive v;
        v.span = &s;
        v.vol = m_media.volume(s.id);
        if (!v.dda.init(*v.vol.layout, s.oIndex, s.dIndex, s.t0, s.t1) || !v.dda.next(v.cell, v.a, v.b))
            return;
        const bool collects =
            m_gather && (v.vol.lightIndex != 0xFFFFFFFFu ? m_gather->lights : m_gather->objects);
        v.majorant = collects || !v.vol.extinctionMajorant ? v.vol.majorant : v.vol.extinctionMajorant;
        m_active.push_back(v);
    }

    Media &m_media;
    const MediumRay &m_ray;
    const EmissionGather *m_gather;
    Spans &m_spans;
    Actives &m_active;
    size_t m_pending = 0;
    float m_start = 0.0f, m_cursor = 0.0f; // the last interval
};

// Adds the emission of one volume at a tentative collision at x (weight w:
// the tracker's T or F; inv: 1 / the summed majorant there).
NOVA_INLINE void gatherEmission(EmissionGather &g, const MediumVolume &rec, size_t cell, const Spectrum &emission,
                                const Spectrum &w, float inv, Vec3f x) {
    if (isBlack(emission))
        return;
    if (rec.lightIndex == 0xFFFFFFFFu) {
        if (g.objects)
            g.objectSum += w * emission * inv;
        return;
    }
    if (!g.lights)
        return;
    const uint32_t li = rec.lightIndex;
    if (!g.linksLight(li)) // light linking (M9 D15): the light does not light the ray's receiver
        return;
    float weight = 1.0f;
    if (g.bsdfPdf > 0.0f) {
        // The light sample's density here (sampleVolumeLight's measure), seen
        // from the MIS vertex: the ray origin differs from it after a
        // fake-caustic pass-through.
        const Distribution1DView &cells = rec.lightCells;
        const float pCell = cells.integral > 0.0f ? cells.func[cell] / (cells.integral * float(cells.count)) : 0.0f;
        const float sigma = rec.majorant[cell];
        const Vec3f r = x - g.prevP;
        const float lightPdf = sigma > 0.0f ? g.lightPmf(li) * pCell / rec.cellVolume * dot(r, r) / sigma : 0.0f;
        weight = bsdfSampleWeight(g.strategy, g.bsdfPdf, lightPdf);
    }
    if (g.multipliers)
        weight *= lightMultiplier(g.lightData[li], g.diffuseShare);
    if (weight == 0.0f)
        return;
    int s = 0;
    while (s < g.lightCount && g.lightIndex[s] != li)
        ++s;
    if (s == g.lightCount) {
        if (s == EmissionGather::kSlots) {
            s = EmissionGather::kSlots - 1;
        } else {
            g.lightIndex[s] = li;
            g.lightSum[s] = spectrum(0.0f);
            ++g.lightCount;
        }
    }
    g.lightSum[s] += w * emission * (inv * weight);
}

// The medium at distance t along the ray: each covering volume's scattering,
// phase function and id (at most `max`, the rest dropped), and their summed
// extinction. Returns how many scatter.
template <class Media>
NOVA_INLINE int mediumAt(Media &media, const MediumRay &ray, float t, MediumPart *parts, int max,
                         Spectrum &extinction) {
    extinction = spectrum(0.0f);
    int n = 0;
    const Vec3f x = ray.o + ray.d * t;
    for (uint32_t i = 0; i < media.activeCount(); ++i) {
        const VolumeId id = media.active(i);
        const MediumVolume rec = media.volume(id);
        if (x.x < rec.lower.x || x.y < rec.lower.y || x.z < rec.lower.z || x.x > rec.upper.x || x.y > rec.upper.y ||
            x.z > rec.upper.z)
            continue;
        const Affine3f worldToObject = inverseAffine(media.volumeToWorld(id, ray.time));
        const VolumePoint p = media.evaluate(id, worldToObject.point(x), x, ray.d, ray.time, ray.rayType, ray.depth);
        extinction += p.extinction();
        if (!isBlack(p.scattering) && n < max)
            parts[n++] = {p.scattering, {p.g, p.g2, p.mix}, id};
    }
    return n;
}

// Where a ray's volumes start and end before ray.tEnd (their world boxes):
// false if it crosses none.
template <class Media, class Spans>
NOVA_INLINE bool mediumExtent(const Media &media, Spans &spans, const MediumRay &ray, float &a, float &b) {
    volumeSpans(media, ray.o, ray.d, ray.tEnd, ray.time, spans);
    if (spans.empty())
        return false;
    a = spans.front().t0; // by entry distance
    b = 0.0f;
    for (size_t i = 0; i < spans.size(); ++i)
        b = b < spans[i].t1 ? spans[i].t1 : b; // std::max
    return b > a;
}

// Ratio tracking along the ray through every volume it crosses: returns the
// transmittance estimate; with `gather`, adds the emission it collects.
// `crossT` (ascending distances, may be empty) receive in `crossOut` the
// transmittance in front of each. `u0` is the first uniform (a Sobol
// sample), `rng` gives the rest.
template <class Media, class Spans, class Actives>
NOVA_INLINE Spectrum trackMedia(Media &media, Spans &spans, Actives &active, const MediumRay &ray, float u0,
                                Rng &rng, EmissionGather *gather, const float *crossT = nullptr,
                                Spectrum *crossOut = nullptr, int crossCount = 0) {
    Spectrum T{1.0f, 1.0f, 1.0f};
    int nextCross = 0;
    const auto flush = [&](float t) {
        while (nextCross < crossCount && crossT[nextCross] < t)
            crossOut[nextCross++] = T;
    };
    MediaWalker<Media, Spans, Actives> walk(media, spans, active, ray, gather);
    bool first = true;
    float a = 0.0f, b = 0.0f, sigma = 0.0f;
    while (walk.next(a, b, sigma)) {
        if (!(sigma > 0.0f))
            continue;
        const float inv = 1.0f / sigma;
        float t = a;
        for (;;) {
            const float u = first ? u0 : rng.next();
            first = false;
            t -= std::log(1.0f - u) * inv;
            if (!(t < b))
                break;
            flush(t);
            Spectrum e{0.0f, 0.0f, 0.0f};
            walk.forEachAt(t, [&](const MediumActive &w, const VolumePoint &p) {
                e += p.extinction();
                if (gather)
                    gatherEmission(*gather, w.vol, w.cell, p.emission, T, inv, ray.o + ray.d * t);
            });
            media.violation(e, sigma);
            T *= Spectrum{1.0f - e.x * inv, 1.0f - e.y * inv, 1.0f - e.z * inv};
            // Russian roulette once the estimate is small.
            const float m = maxComponent(absolute(T));
            if (m < 0.1f) {
                const float s = m * 10.0f, survive = s < 1e-3f ? 1e-3f : s; // std::max
                if (!(m > 0.0f) || rng.next() >= survive) {
                    T = spectrum(0.0f);
                    flush(kInf);
                    return T;
                }
                T *= 1.0f / survive;
            }
        }
    }
    flush(kInf);
    return T;
}

// A free flight (D8, research A E4 "B3"): spectral tracking on the common
// majorant with survival biasing. At each tentative collision the emission
// is collected (beta weight ε / σ̄), absorption is never sampled, and the
// scatter-or-null choice takes the throughput-weighted averages of σs and
// |σ̄ - σt| as probabilities; roulette below 0.1 as in trackMedia. `beta`
// is the path throughput before the flight (it weighs the choice only).
// With `alphaT`, the ratio-tracked transmittance over the whole segment
// from the same tentative collisions, continued past a scattering event
// (camera rays' alpha, D12). crossT / crossOut as for trackMedia.
template <class Media, class Spans, class Actives, class Hits>
NOVA_INLINE MediumFlight flyMedia(Media &media, Spans &spans, Actives &active, Hits &parts, const MediumRay &ray,
                                  Spectrum beta, float u0, Rng &rng, EmissionGather *gather, Spectrum *alphaT,
                                  const float *crossT = nullptr, Spectrum *crossOut = nullptr, int crossCount = 0) {
    MediumFlight f;
    Spectrum &F = f.weight;
    bool flying = true;             // no event yet, not ended by roulette
    bool alpha = alphaT != nullptr; // the alpha transmittance is still tracked
    if (alphaT)
        *alphaT = spectrum(1.0f);
    int nextCross = 0;
    const auto flush = [&](float t) {
        while (nextCross < crossCount && crossT[nextCross] < t)
            crossOut[nextCross++] = F;
    };
    // Roulette once an estimate is small (as trackMedia); false: it ended.
    const auto roulette = [&](Spectrum &w) {
        const float m = maxComponent(absolute(w));
        if (m >= 0.1f)
            return true;
        const float s = m * 10.0f, survive = s < 1e-3f ? 1e-3f : s; // std::max
        if (!(m > 0.0f) || rng.next() >= survive) {
            w = spectrum(0.0f);
            return false;
        }
        w *= 1.0f / survive;
        return true;
    };
    // At a point covered by several volumes, each one's scattering, id and
    // phase function: an event picks one in proportion to its share.
    MediaWalker<Media, Spans, Actives> walk(media, spans, active, ray, gather);
    bool first = true;
    float a = 0.0f, b = 0.0f, sigma = 0.0f;
    while ((flying || alpha) && walk.next(a, b, sigma)) {
        if (!(sigma > 0.0f))
            continue;
        const float inv = 1.0f / sigma;
        float t = a;
        while (flying || alpha) {
            const float u = first ? u0 : rng.next();
            first = false;
            t -= std::log(1.0f - u) * inv;
            if (!(t < b))
                break;
            if (flying)
                flush(t);
            parts.clear();
            VolumePoint v;
            walk.forEachAt(t, [&](const MediumActive &w, const VolumePoint &p) {
                v.scattering += p.scattering;
                v.absorption += p.absorption;
                parts.push_back({p.scattering, p.emission, w.span->id, &w, {p.g, p.g2, p.mix}});
            });
            const Spectrum e = v.extinction();
            media.violation(e, sigma);
            if (alpha) {
                *alphaT *= Spectrum{1.0f - e.x * inv, 1.0f - e.y * inv, 1.0f - e.z * inv};
                alpha = roulette(*alphaT);
            }
            if (!flying)
                continue;
            if (gather)
                for (size_t i = 0; i < parts.size(); ++i)
                    gatherEmission(*gather, parts[i].active->vol, parts[i].active->cell, parts[i].emission, F, inv, ray.o + ray.d * t);
            // Scatter or null, by the throughput-weighted coefficients. The
            // weights are magnitudes: after a majorant violation (a negative
            // null coefficient, D8) F turns negative, and the probabilities
            // must stay positive for the signed weights below to stay
            // unbiased (weighted delta tracking).
            const Spectrum bF = absolute(beta * F);
            const Spectrum null{sigma - e.x, sigma - e.y, sigma - e.z};
            const float ps = dot(bF, v.scattering);
            const float pn = dot(bF, absolute(null));
            const float total = ps + pn;
            const float r = total > 0.0f ? rng.next() * total : 0.0f;
            if (!(total > 0.0f)) { // nothing scatters or passes: absorbed
                F = spectrum(0.0f);
                flying = false;
            } else if (r < ps) {
                // The event's volume, in proportion to its share of the
                // throughput-weighted scattering (the same draw).
                size_t k = 0;
                float edge = dot(bF, parts[0].scattering);
                while (k + 1 < parts.size() && !(r < edge))
                    edge += dot(bF, parts[++k].scattering);
                while (k > 0 && !(dot(bF, parts[k].scattering) > 0.0f)) // rounding past the last share
                    --k;
                F *= parts[k].scattering * (total / (sigma * dot(bF, parts[k].scattering)));
                f.scattered = true;
                f.t = t;
                f.volume = parts[k].volume;
                f.phase = parts[k].phase;
                f.crossings = nextCross;
                flying = false;
            } else {
                F *= null * (total / (sigma * pn));
                flying = roulette(F);
            }
        }
    }
    if (!f.scattered) {
        flush(kInf);
        f.crossings = crossCount;
    }
    return f;
}

// Equiangular sampling (docs/design/M9.md D17, Kulla and Fajardo 2012): the
// distance along a ray's medium segment [a, b] in proportion to 1 / r^2 to
// a light at c.
struct Equiangular {
    float delta = 0.0f, h = 0.0f;       // c's distance along the ray, and from it
    float thetaA = 0.0f, thetaB = 0.0f; // the angles of a and b seen from c

    // False if the segment is empty.
    NOVA_INLINE bool init(Vec3f o, Vec3f d, float a, float b, Vec3f c) {
        delta = dot(c - o, d);
        const Vec3f perp = c - o - d * delta;
        // A light on the ray: a small offset keeps the density finite.
        h = nova::fmax(length(perp), 1e-4f * nova::fmax(b - a, 1e-3f));
        thetaA = std::atan((a - delta) / h);
        thetaB = std::atan((b - delta) / h);
        return thetaB > thetaA;
    }
    NOVA_INLINE float sample(float u) const { return delta + h * std::tan(thetaA + u * (thetaB - thetaA)); }
    NOVA_INLINE float pdf(float t) const {
        const float x = t - delta;
        return h / ((thetaB - thetaA) * (h * h + x * x));
    }
};

// The free flight's density at t, a deterministic stand-in for both
// strategies' MIS weights (D17): sigma e^(-sigma (t - a)), sigma the mean
// extinction at t and a where the ray enters the medium. Exact for a
// homogeneous medium entered at a; any stand-in keeps the weights summing to
// one, so both estimators stay unbiased.
NOVA_INLINE float freeFlightProxy(Spectrum extinction, float a, float t) {
    const float sigma = (extinction.x + extinction.y + extinction.z) * (1.0f / 3.0f);
    return sigma > 0.0f ? sigma * std::exp(-sigma * nova::fmax(t - a, 0.0f)) : 0.0f;
}

} // namespace nova
