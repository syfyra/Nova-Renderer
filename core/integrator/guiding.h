#pragma once

// Path guiding (docs/design/M11-P9c.md): Practical Path Guiding's SD-tree
// (Mueller, Gross, Novak 2017) as plain node arrays that the CPU wavefront
// and the CUDA kernels read alike. A binary spatial tree over the scene's
// bounds (made a cube; children split at the middle of x, y, z in turn by
// depth) leads to a leaf with two directional quadtrees: the sampling one,
// learned in the last training iteration, and the building one the current
// iteration's records go into. A quadtree covers the sphere mapped to the
// unit square by (cos theta, phi), which keeps areas, so a quadrant's density
// is its share of its parent's energy times 4, over 4 pi at the end.
//
// Guided sampling is one-sample MIS of the closure's own strategy and the
// guide (sampleClosureGuided): a guided lobe takes its direction from the
// guide with probability alpha, and every non-delta sample is weighted by
// the mixture's density, so the estimate stays unbiased wherever the BSDF's
// own density covers f. Training records go into the building quadtrees as
// 64-bit fixed-point integers, so the sums do not depend on the order threads
// or warps add them: the field, and the image, are deterministic.

#include "core/bsdf/closure.h"
#include "core/math/common.h"
#include "core/math/rng.h"
#include "core/math/sobol.h"
#include "core/math/spectrum.h"
#include "core/math/vec.h"
#include "core/scene/ray_limits.h"

#include <cmath>
#include <cstdint>

#if !defined(__CUDACC__)
#include <atomic>
#endif

namespace nova {

namespace guide {
// The components Karma's guiding_components lists (GuidingSettings::components).
constexpr uint8_t kDiffuse = 1, kReflect = 2, kCoat = 4, kRefract = 8, kVolume = 16, kSss = 32;
constexpr uint8_t kDefaultComponents = kDiffuse | kVolume | kSss;
constexpr uint32_t kVertices = 4;          // recorded vertices per path
constexpr int kMaxDepth = 20;              // of a directional quadtree
constexpr float kProbability = 0.5f;       // the guide's share at a guided lobe (PPG's BSDF fraction)
constexpr float kRecordScale = 1048576.0f; // fixed point of the records (2^20)
constexpr float kRecordMax = 1e6f;         // a record's largest value (overflow guard)
} // namespace guide

namespace stream {
// The guide-or-lobe choice at a guided vertex (1D per depth), the jitter of a
// record's position (an Rng seeded per depth) and its rounding (per vertex).
constexpr uint32_t kGuideBase = 0xE0000u;
NOVA_INLINE constexpr uint32_t guide(uint32_t depth) { return kGuideBase + depth; }
constexpr uint32_t kGuideRecordBase = 0xE8000u;
NOVA_INLINE constexpr uint32_t guideRecord(uint32_t depth) { return kGuideRecordBase + depth; }
constexpr uint32_t kGuideRoundBase = 0xF0000u;
NOVA_INLINE constexpr uint32_t guideRound(uint32_t vertex) { return kGuideRoundBase + vertex; }
} // namespace stream

// A spatial node: its first child (the second follows; 0: a leaf) or its leaf.
struct GuideSpatialNode {
    uint32_t child;
    uint32_t leaf;
};

// A spatial leaf's quadtrees: the sampling root (kInvalidId: no distribution
// yet) and the building root.
struct GuideLeaf {
    uint32_t sampleRoot;
    uint32_t buildRoot;
};

// A sampling quadtree node: per quadrant (x = q & 1, y = q >> 1 of the
// square) its energy and child node (0: the quadrant is a leaf).
struct GuideDirNode {
    float sum[4];
    uint32_t child[4];
};

// What a pass sees of the field. sampling: leaves may have distributions;
// training: guided vertices are recorded into the building quadtrees.
struct GuideView {
    const GuideSpatialNode *spatial = nullptr;
    const GuideLeaf *leaves = nullptr;
    const GuideDirNode *nodes = nullptr; // sampling quadtrees
    const uint32_t *buildChild = nullptr; // building quadtrees: 4 children per node (0: leaf)
    const uint32_t *buildLeaf = nullptr;  // per building node: its spatial leaf
    uint64_t *buildSum = nullptr;         // 4 fixed-point sums per building node
    uint64_t *leafCount = nullptr;        // records per spatial leaf
    Vec3f origin{0.0f, 0.0f, 0.0f};
    float size = 0.0f;        // the cube's edge
    float recordScale = 0.0f; // fixed-point scale of this iteration's records
    float probability = guide::kProbability; // the guide's share at a guided lobe
    float filter = 1.0f; // the record jitter, in cells (PPG's stochastic box filter)
    uint8_t components = 0;
    uint8_t sampling = 0;
    uint8_t training = 0;

    NOVA_INLINE bool active() const { return sampling || training; }
};

// The per-path records of a wave (WaveQueues, GpuWave): per path the count and
// whether the newest one waits for its snapshot, per vertex [path * kVertices
// + j] its building slot (node * 4 + quadrant), 1 / (throughput after the
// vertex x its density) and the path's radiance once the vertex's own light
// samples are in.
struct GuidePaths {
    uint8_t *count = nullptr;
    uint8_t *pending = nullptr;
    uint32_t *slot = nullptr;
    Spectrum *weight = nullptr;
    Spectrum *snap = nullptr;
};

// --- Directions and quadtrees ---------------------------------------------

NOVA_INLINE Vec2f guideToSquare(Vec3f d) {
    const float cosTheta = d.z < -1.0f ? -1.0f : (d.z > 1.0f ? 1.0f : d.z);
    float phi = std::atan2(d.y, d.x);
    if (phi < 0.0f)
        phi += kTwoPi;
    const float u = (cosTheta + 1.0f) * 0.5f, v = phi * kInvTwoPi;
    return {u < 0.99999994f ? u : 0.99999994f, v < 0.99999994f ? (v > 0.0f ? v : 0.0f) : 0.99999994f};
}

NOVA_INLINE Vec3f guideFromSquare(Vec2f p) {
    const float cosTheta = 2.0f * p.x - 1.0f;
    const float sinTheta = std::sqrt(nova::fmax(0.0f, 1.0f - cosTheta * cosTheta));
    const float phi = kTwoPi * p.y;
    return {sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta};
}

// The spatial leaf at p, with its cell (lower corner and extent).
NOVA_INLINE uint32_t guideLeafAt(const GuideView &g, Vec3f p, Vec3f &lo, Vec3f &ext) {
    lo = g.origin;
    ext = Vec3f{g.size, g.size, g.size};
    uint32_t n = 0;
    int axis = 0;
    while (g.spatial[n].child != 0) {
        const float half = ext[axis] * 0.5f;
        const bool upper = p[axis] >= lo[axis] + half;
        if (upper)
            lo[axis] += half;
        ext[axis] = half;
        n = g.spatial[n].child + (upper ? 1u : 0u);
        axis = axis == 2 ? 0 : axis + 1;
    }
    return g.spatial[n].leaf;
}

NOVA_INLINE uint32_t guideLeafAt(const GuideView &g, Vec3f p) {
    Vec3f lo, ext;
    return guideLeafAt(g, p, lo, ext);
}

// The sampling quadtree's density of direction d (solid angle).
NOVA_INLINE float guidePdf(const GuideView &g, uint32_t root, Vec3f d) {
    Vec2f p = guideToSquare(d);
    float pdf = 1.0f;
    uint32_t n = root;
    for (int depth = 0; depth < guide::kMaxDepth; ++depth) {
        const GuideDirNode &node = g.nodes[n];
        const float total = node.sum[0] + node.sum[1] + node.sum[2] + node.sum[3];
        const int qx = p.x >= 0.5f ? 1 : 0, qy = p.y >= 0.5f ? 1 : 0, q = qx + 2 * qy;
        if (!(total > 0.0f) || !(node.sum[q] > 0.0f))
            return 0.0f;
        pdf *= 4.0f * node.sum[q] / total;
        p.x = 2.0f * p.x - float(qx);
        p.y = 2.0f * p.y - float(qy);
        if (node.child[q] == 0)
            break;
        n = node.child[q];
    }
    return pdf * (0.25f * kInvPi);
}

// A direction from the sampling quadtree, u stratified (warped down the tree,
// so it stays stratified); its density in pdf.
NOVA_INLINE Vec3f guideSample(const GuideView &g, uint32_t root, Vec2f u, float &pdf) {
    Vec2f origin{0.0f, 0.0f};
    float scale = 1.0f;
    pdf = 1.0f;
    uint32_t n = root;
    for (int depth = 0; depth < guide::kMaxDepth; ++depth) {
        const GuideDirNode &node = g.nodes[n];
        const float *s = node.sum;
        const float total = s[0] + s[1] + s[2] + s[3];
        if (!(total > 0.0f)) {
            pdf = 0.0f;
            return Vec3f{0.0f, 0.0f, 1.0f};
        }
        // The column (x) by the halves' energies, then the row within it.
        const float left = s[0] + s[2];
        int qx;
        float x = u.x * total;
        if (x < left) {
            qx = 0;
            u.x = x / left;
        } else {
            qx = 1;
            u.x = (x - left) / (total - left);
        }
        const float column = qx ? total - left : left, bottom = s[qx];
        int qy;
        const float y = u.y * column;
        if (y < bottom) {
            qy = 0;
            u.y = y / bottom;
        } else {
            qy = 1;
            u.y = (y - bottom) / (column - bottom);
        }
        u.x = nova::fmin(nova::fmax(u.x, 0.0f), 0.99999994f);
        u.y = nova::fmin(nova::fmax(u.y, 0.0f), 0.99999994f);
        const int q = qx + 2 * qy;
        pdf *= 4.0f * s[q] / total;
        scale *= 0.5f;
        origin.x += scale * float(qx);
        origin.y += scale * float(qy);
        if (node.child[q] == 0)
            break;
        n = node.child[q];
    }
    pdf *= 0.25f * kInvPi;
    return guideFromSquare({origin.x + scale * u.x, origin.y + scale * u.y});
}

// The building quadtree's slot (node * 4 + quadrant) of direction d.
NOVA_INLINE uint32_t guideBuildSlot(const GuideView &g, uint32_t root, Vec3f d) {
    Vec2f p = guideToSquare(d);
    uint32_t n = root;
    for (int depth = 0; depth < guide::kMaxDepth; ++depth) {
        const int qx = p.x >= 0.5f ? 1 : 0, qy = p.y >= 0.5f ? 1 : 0, q = qx + 2 * qy;
        const uint32_t c = g.buildChild[4 * n + uint32_t(q)];
        if (c == 0)
            return 4 * n + uint32_t(q);
        p.x = 2.0f * p.x - float(qx);
        p.y = 2.0f * p.y - float(qy);
        n = c;
    }
    return 4 * n; // not reached: building trees are at most kMaxDepth deep
}

// --- Guided vertices --------------------------------------------------------

// A vertex's guide: the sampling distribution (root; kInvalidId: none), the
// guide's probability at a guided lobe (alpha; 0: the closure's own
// sampling alone) and the guided lobes' pick sum (share). `record`: the
// vertex has guided lobes and the field is training.
struct GuideSite {
    uint32_t root = kInvalidId;
    float alpha = 0.0f;
    float share = 0.0f;
    bool record = false;
};

// The components of a lobe on the side it reflects to and transmits to (a
// coat counts as coat, translucency and thin-walled SSS as sss, diffuse lobes
// and sheen as diffuse, dielectrics and fibres reflect and refract).
NOVA_INLINE void guideLobeComponents(const Lobe &l, uint8_t &reflect, uint8_t &transmit) {
    reflect = transmit = 0;
    if (l.label == LobeLabel::Coat) {
        reflect = guide::kCoat;
        return;
    }
    if (l.sss || l.type == LobeType::Translucent) {
        transmit = guide::kSss;
        if (l.reflects())
            reflect = guide::kSss;
        return;
    }
    switch (l.type) {
    case LobeType::Conductor:
        reflect = guide::kReflect;
        return;
    case LobeType::Dielectric:
    case LobeType::ThinDielectric:
    case LobeType::Hair:
        reflect = l.reflects() ? guide::kReflect : uint8_t(0);
        transmit = l.transmits() ? guide::kRefract : uint8_t(0);
        return;
    case LobeType::Subsurface:
        return;
    default:
        reflect = guide::kDiffuse;
        return;
    }
}

// Whether a path may still take a full continuation of each surface class
// (bits by LimitClass: diffuse, reflect, refract, sss), from its event counts
// and the limits.
NOVA_INLINE uint8_t guideOpenClasses(uint64_t eventCounts, const RayLimitOverrides &o, const RayLimits &limits,
                                    float dither) {
    uint8_t open = 0;
    for (int cls = 0; cls < 4; ++cls) {
        const float limit = o.limit[cls] >= 0.0f ? o.limit[cls] : limits.limit[cls];
        if ((uint32_t(eventCounts >> (8 * cls)) & 0xFFu) < pathLimit(limit, dither))
            open |= uint8_t(1u << cls);
    }
    return open;
}

// The guide at a surface hit: marks the closure's guided lobes (listed
// components whose event class is open; never delta or external ones) and
// looks the distribution up at P.
NOVA_INLINE GuideSite guideSurfaceSite(const GuideView &g, ClosureList &cl, Vec3f P, uint8_t openClasses) {
    GuideSite site;
    cl.guideMask = 0;
    if (!g.active() || cl.external.active)
        return site;
    constexpr uint8_t kDiffuseClass = 1, kReflectClass = 2, kRefractClass = 4, kSssClass = 8;
    uint16_t mask = 0;
    float share = 0.0f;
    for (int i = 0; i < cl.count; ++i) {
        if (!(cl.nonDeltaMask & (1u << i)))
            continue;
        const Lobe &l = cl.lobe[i];
        uint8_t reflect, transmit;
        guideLobeComponents(l, reflect, transmit);
        // The event class of each side (Lobe::eventClass).
        const uint8_t reflectClass = (l.sss || l.type == LobeType::Translucent) ? kSssClass
                                     : (reflect & (guide::kDiffuse)) ? kDiffuseClass
                                                                      : kReflectClass;
        const uint8_t transmitClass = (l.sss || l.type == LobeType::Translucent) ? kSssClass : kRefractClass;
        const bool guided = ((reflect & g.components) && (openClasses & reflectClass)) ||
                            ((transmit & g.components) && (openClasses & transmitClass));
        if (guided) {
            mask |= uint16_t(1u << i);
            share += l.pick;
        }
    }
    if (!mask)
        return site;
    site.record = g.training != 0;
    site.share = share;
    if (g.sampling) {
        const uint32_t root = g.leaves[guideLeafAt(g, P)].sampleRoot;
        if (root != kInvalidId) {
            site.root = root;
            site.alpha = g.probability;
            cl.guideMask = mask;
        }
    }
    return site;
}

// The guide at a volume event or a subsurface walk's exit (one lobe of
// component `component`, its class open).
NOVA_INLINE GuideSite guidePointSite(const GuideView &g, Vec3f P, uint8_t component, bool open) {
    GuideSite site;
    if (!g.active() || !(g.components & component) || !open)
        return site;
    // Outside the field's cube (the sky atmosphere's far air): not guided,
    // so the scene's cells keep the scene's light.
    for (int a = 0; a < 3; ++a)
        if (!(P[a] >= g.origin[a] && P[a] <= g.origin[a] + g.size))
            return site;
    site.record = g.training != 0;
    site.share = 1.0f;
    if (g.sampling) {
        const uint32_t root = g.leaves[guideLeafAt(g, P)].sampleRoot;
        if (root != kInvalidId) {
            site.root = root;
            site.alpha = g.probability;
        }
    }
    return site;
}

// The density of direction wi at a guided surface vertex, from the closure's
// evaluation there (light samples' MIS).
NOVA_INLINE float guidedPdf(const GuideView &g, const GuideSite &site, const ClosureEval &e, Vec3f wi) {
    if (!(site.alpha > 0.0f))
        return e.pdf;
    return e.pdf + site.alpha * (site.share * guidePdf(g, site.root, wi) - e.pdfGuided);
}

// The density of wi at a single-lobe vertex (volume, walk exit) whose own
// density there is `own`.
NOVA_INLINE float guidedPointPdf(const GuideView &g, const GuideSite &site, float own, Vec3f wi) {
    if (!(site.alpha > 0.0f))
        return own;
    return (1.0f - site.alpha) * own + site.alpha * guidePdf(g, site.root, wi);
}

// The continuation at a surface vertex: sampleClosure, with the guided lobes
// taking the guide's direction with probability alpha (uGuide decides). The
// weight is f |cos| over the mixture's density for every non-delta sample;
// delta and unguided lobes keep their probabilities. Without a guide (alpha
// 0) exactly sampleClosure, drawing nothing more.
template <typename PerLobe = NoLobeSink>
NOVA_INLINE bool sampleClosureGuided(const ClosureList &cl, Vec3f wo, Vec3f u, float uGuide, const GuideView &g,
                                     const GuideSite &site, ClosureSample &s, const PerLobe &perLobe = PerLobe{}) {
    if (!(site.alpha > 0.0f))
        return sampleClosure(cl, wo, u, s, perLobe);
    int j = 0; // the lobe sampleClosure picks
    if (cl.count > 1)
        while (j + 1 < cl.count && u.z >= cl.cdf[j])
            ++j;
    if (((cl.guideMask >> j) & 1u) && uGuide < site.alpha) {
        float pg = 0.0f;
        const Vec3f wi = guideSample(g, site.root, {u.x, u.y}, pg);
        if (!(pg > 0.0f))
            return false;
        const ClosureEval e = evalClosure(cl, wo, wi, perLobe);
        const float pdf = e.pdf + site.alpha * (site.share * pg - e.pdfGuided);
        if (!(pdf > 0.0f) || isBlack(e.fcos))
            return false;
        const Lobe &l = cl.lobe[j];
        const bool transmit = dot(wo, cl.ng) * dot(wi, cl.ng) < 0.0f;
        s.wi = wi;
        s.weight = e.fcos * (1.0f / pdf);
        s.pdf = pdf;
        s.pdfGuided = e.pdfGuided;
        s.delta = false;
        s.walk = false;
        s.lobe = int8_t(j);
        s.transmit = transmit;
        s.rayClass = l.eventClass(transmit);
        s.roughness = l.roughness();
        s.refract = transmit && l.type == LobeType::Dielectric && std::fabs(l.dielectric.eta - 1.0f) >= kIndexMatched;
        s.eta = s.refract ? l.dielectric.eta : 1.0f;
        const float total = luminance(e.fcos);
        s.diffuseShare = total > 0.0f ? saturate(luminance(e.fcosDiffuse) / total) : 0.0f;
        return isFinite(s.weight);
    }
    if (!sampleClosure(cl, wo, u, s, perLobe))
        return false;
    if (s.delta || s.walk)
        return true;
    const float pdf = s.pdf + site.alpha * (site.share * guidePdf(g, site.root, s.wi) - s.pdfGuided);
    if (!(pdf > 0.0f))
        return false;
    s.weight *= s.pdf / pdf;
    s.pdf = pdf;
    return isFinite(s.weight);
}

// --- Training records -------------------------------------------------------

// Records a guided vertex at P whose continuation wi (density pdf) left the
// path with throughput beta (after the vertex's weight and Russian
// roulette): the building slot at P jittered within its spatial cell (PPG's
// stochastic box filter; jitter: three uniforms) and 1 / (beta pdf). Its
// radiance snapshot follows once its light samples are in (guideSnapshot).
NOVA_INLINE void guideRecordVertex(const GuideView &g, const GuidePaths &r, uint32_t p, Vec3f P, Vec3f wi, float pdf,
                                   Spectrum beta, Vec3f jitter) {
    const uint32_t j = r.count[p];
    if (!g.training || j >= guide::kVertices || !(pdf > 0.0f))
        return;
    Vec3f lo, ext;
    guideLeafAt(g, P, lo, ext);
    Vec3f q;
    const float top = g.size * 0.99999994f;
    for (int a = 0; a < 3; ++a) {
        const float x = P[a] + (jitter[a] - 0.5f) * ext[a] * g.filter - g.origin[a];
        q[a] = g.origin[a] + (x < 0.0f ? 0.0f : (x > top ? top : x));
    }
    const uint32_t leaf = guideLeafAt(g, q, lo, ext);
    const uint32_t i = p * guide::kVertices + j;
    r.slot[i] = guideBuildSlot(g, g.leaves[leaf].buildRoot, wi);
    const float inv = 1.0f / pdf;
    r.weight[i] = Spectrum{beta.x > 0.0f ? inv / beta.x : 0.0f, beta.y > 0.0f ? inv / beta.y : 0.0f,
                           beta.z > 0.0f ? inv / beta.z : 0.0f};
    r.count[p] = uint8_t(j + 1);
    r.pending[p] = 1;
}

// At the start of a bounce: the newest vertex of path p takes the path's
// radiance as its snapshot (its own light samples were the last bounce's).
NOVA_INLINE void guideSnapshot(const GuidePaths &r, uint32_t p, const Spectrum &radiance) {
    if (r.pending[p]) {
        r.snap[p * guide::kVertices + r.count[p] - 1] = radiance;
        r.pending[p] = 0;
    }
}

NOVA_INLINE void guideAtomicAdd(uint64_t *x, uint64_t v) {
#if defined(__CUDA_ARCH__)
    atomicAdd(reinterpret_cast<unsigned long long *>(x), static_cast<unsigned long long>(v));
#elif !defined(__CUDACC__)
    std::atomic_ref<uint64_t>(*x).fetch_add(v, std::memory_order_relaxed);
#else
    (void)x;
    (void)v;
#endif
}

// When path p ends with radiance L: each recorded vertex's incident radiance
// over its density, the luminance of (L - snapshot) / (throughput x
// density), goes to its slot (fixed point, rounded stochastically with a
// hash of the path's sample), and counts for its spatial leaf. A vertex still
// waiting for its snapshot saw no light after it. `seed`, `index`: the
// path's pixel seed and sample number.
NOVA_INLINE void guideFlushPath(const GuideView &g, const GuidePaths &r, uint32_t p, const Spectrum &L,
                                uint32_t seed, uint32_t index) {
    const uint32_t n = r.count[p] - (r.pending[p] ? 1u : 0u);
    for (uint32_t j = 0; j < n; ++j) {
        const uint32_t i = p * guide::kVertices + j;
        const Spectrum d = L - r.snap[i];
        const Spectrum w = r.weight[i];
        float v = luminance(Spectrum{nova::fmax(d.x, 0.0f) * w.x, nova::fmax(d.y, 0.0f) * w.y,
                                     nova::fmax(d.z, 0.0f) * w.z});
        if (!(v > 0.0f))
            v = 0.0f; // NaN too
        const float x = nova::fmin(v, guide::kRecordMax) * g.recordScale;
        Rng rng = Rng::make(seed, index, stream::guideRound(j));
        const uint64_t fixed = uint64_t(std::floor(x + rng.next()));
        const uint32_t slot = r.slot[i];
        if (fixed)
            guideAtomicAdd(g.buildSum + slot, fixed);
        guideAtomicAdd(g.leafCount + g.buildLeaf[slot / 4], 1u);
    }
    r.count[p] = 0;
    r.pending[p] = 0;
}

} // namespace nova
