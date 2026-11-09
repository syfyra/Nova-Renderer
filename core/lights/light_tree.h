#pragma once

// Light selection for next-event estimation (docs/design/M9.md D11-D13,
// research docs/research/m9/lights.md 2, 6.1-6.4): a pbrt-v4-style light
// tree over every bounded light (area, point, spot and volume lights), one
// light per leaf, and the lights at infinity outside it.
//
// - Each node bounds its lights: a box, their power, the cone of their
//   emission normals (w, theta_o) and the spread around it (theta_e). The
//   importance of a node for a receiver at p is power x the cosine of the
//   smallest angle any emitter direction makes towards p / d^2 (d^2 clamped
//   at half the diagonal, as pbrt-v4), times the receiver's cosine bounded the
//   same way: one-sided for opaque closures (only the upper hemisphere),
//   two-sided for transmitting ones, none for volume events.
// - Selection walks down from the root with one uniform number, rescaled at
//   every level; the probability of the light it returns (and of any light,
//   for MIS of a BSDF sample that reaches it) is the product of the child
//   probabilities along the light's bit trail, evaluated at the same point.
// - Lights at infinity (domes, distant lights) are chosen with q = I_inf /
//   (I_inf + sum over a cut of 16 tree nodes), I_inf the infinite lights'
//   irradiance there; among themselves by power.
// Flat POD arrays: the same view works on the GPU.

#include "core/lights/light_sampler.h"
#include "core/lights/shapes.h"
#include "core/math/vec.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace nova {

// What the receiver of a light sample is.
enum class Receiver : uint8_t {
    None,     // a volume event: no cosine
    OneSided, // an opaque closure: light from below its normal does nothing
    TwoSided, // a closure that transmits
};

// The bounds of one light (or of a subtree).
struct LightBounds {
    Vec3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    float phi = 0.0f;         // power (selection weight)
    Vec3f w{0.0f, 0.0f, 1.0f}; // emission normal cone axis
    float cosO = 1.0f;        // cone half-angle theta_o
    float cosE = 1.0f;        // emission spread theta_e beyond the cone
    bool twoSided = false;    // emits along -w as well
    bool crossable = false;   // an area light that BSDF rays collect by crossing it (not geometry)
};

// Light linking (docs/design/M9.md D15). A light links to one category of
// objects (the bit of the category, 64 at most) or lights every receiver
// (kLinksEveryone); a receiver set is a combination of categories (an object
// instance's, set 0: none). A light lights a set if it lights every receiver
// or the set holds its category: exact for any number of receiver sets.
constexpr uint64_t kLinksEveryone = ~uint64_t(0);
NOVA_INLINE bool lightLinks(uint64_t light, uint64_t receiver) {
    return light == kLinksEveryone || (light & receiver) != 0;
}

// Per light its link (the category bit, or kLinksEveryone) and per receiver
// set its categories; null `light`: no linking, every light lights everything.
struct LightLinks {
    const uint64_t *light = nullptr;
    const uint64_t *receiver = nullptr;
    NOVA_INLINE uint64_t categories(uint32_t set) const { return light ? receiver[set] : 0u; }
    NOVA_INLINE bool links(uint32_t li, uint32_t set) const { return !light || lightLinks(light[li], receiver[set]); }
};

struct LightTreeNode {
    Vec3f lo;
    float phi;
    Vec3f hi;
    float cosO;
    Vec3f w;
    float cosE;
    uint32_t child; // interior: the second child (the first follows the node); leaf: the light
    uint32_t flags; // kLeaf, kTwoSided, kCrossable (some light below is crossable), kLinksAll
    // Light linking (M9 D15): the categories the linked lights below link
    // to; kLinksAll: some light below lights every receiver.
    uint64_t mask;
    static constexpr uint32_t kLeaf = 1u, kTwoSided = 2u, kCrossable = 4u, kLinksAll = 8u;
    // Whether some light below lights a receiver of these categories.
    NOVA_INLINE bool linksTo(uint64_t categories) const { return (flags & kLinksAll) || (mask & categories) != 0; }
};

namespace light_tree_detail {

// cos(a - b) for a >= b, else 1 (the angles as sine and cosine).
NOVA_INLINE float cosSub(float sa, float ca, float sb, float cb) { return ca > cb ? 1.0f : ca * cb + sa * sb; }
NOVA_INLINE float sinSub(float sa, float ca, float sb, float cb) { return ca > cb ? 0.0f : sa * cb - ca * sb; }

} // namespace light_tree_detail

// The importance of a node's lights for a receiver at p with normal n.
NOVA_INLINE float lightImportance(const LightTreeNode &b, Vec3f p, Vec3f n, Receiver r) {
    using namespace light_tree_detail;
    if (!(b.phi > 0.0f))
        return 0.0f;
    const Vec3f pc = (b.lo + b.hi) * 0.5f, diag = b.hi - b.lo;
    const Vec3f d = p - pc;
    // Exact plane tests the angular bounds miss when p is close (a light
    // right behind a wall): the whole box below an opaque receiver's tangent
    // plane, or p behind every emitter of a one-sided planar node.
    const Vec3f half = diag * 0.5f;
    if (r == Receiver::OneSided &&
        -dot(d, n) + std::fabs(n.x) * half.x + std::fabs(n.y) * half.y + std::fabs(n.z) * half.z <= 0.0f)
        return 0.0f;
    if (b.cosO >= 1.0f && !(b.flags & LightTreeNode::kTwoSided) &&
        dot(d, b.w) + std::fabs(b.w.x) * half.x + std::fabs(b.w.y) * half.y + std::fabs(b.w.z) * half.z <= 0.0f)
        return 0.0f;
    const float dd = dot(d, d), halfDiag2 = 0.25f * dot(diag, diag);
    // pbrt-v4's clamp of d^2 at half the box diagonal, unsquared: what the
    // research measured (E1). Squaring it (research 6.1's suggestion) blurs
    // proximity inside large nodes: 6x instead of 50x+ on the street (M9 notes).
    const float halfDiag = std::sqrt(halfDiag2);
    const float d2 = nova::fmax(dd > halfDiag ? dd : halfDiag, 1e-20f);
    // Directions from the box to p: everything if p is inside (or close).
    float cosB = -1.0f;
    const bool inside = p.x >= b.lo.x && p.y >= b.lo.y && p.z >= b.lo.z && p.x <= b.hi.x && p.y <= b.hi.y &&
                        p.z <= b.hi.z;
    if (!inside && dd > halfDiag2)
        cosB = safeSqrt(1.0f - halfDiag2 / dd);
    const float sinB = safeSqrt(1.0f - cosB * cosB);
    const Vec3f wi = dd > 0.0f ? d * (1.0f / std::sqrt(dd)) : Vec3f{0.0f, 0.0f, 1.0f}; // light -> p
    float cosW = dot(b.w, wi);
    if (b.flags & LightTreeNode::kTwoSided)
        cosW = std::fabs(cosW);
    const float sinW = safeSqrt(1.0f - cosW * cosW), sinO = safeSqrt(1.0f - b.cosO * b.cosO);
    const float cosX = cosSub(sinW, cosW, sinO, b.cosO), sinX = sinSub(sinW, cosW, sinO, b.cosO);
    const float cosP = cosSub(sinX, cosX, sinB, cosB);
    if (cosP <= b.cosE)
        return 0.0f;
    float importance = b.phi * cosP / d2;
    if (r != Receiver::None) {
        // The incident direction is -wi; bound its angle to n by the box's.
        const float c = -dot(wi, n);
        const float cosI = r == Receiver::TwoSided ? std::fabs(c) : c;
        const float sinI = safeSqrt(1.0f - cosI * cosI);
        importance *= cosSub(sinI, cosI, sinB, cosB);
    }
    return importance > 0.0f ? importance : 0.0f;
}

struct LightTreeView {
    const LightTreeNode *nodes = nullptr;
    const uint64_t *trail = nullptr; // per light: its path from the root (bit k: level k went to the second child)
    uint32_t lightCount = 0;         // the lights trail, infiniteSlot and links.light cover
    uint32_t nodeCount = 0;          // 0: no bounded light
    // Lights at infinity: their indices, the irradiance each gives at normal
    // incidence, whether it is a distant light (with its direction), and the
    // alias table among them (pmf indexed like `infinite`).
    const uint32_t *infinite = nullptr;
    const float *infiniteE = nullptr;
    const Vec3f *infiniteDir = nullptr; // distant: towards the light; dome: zero
    uint32_t infiniteCount = 0;
    LightSamplerView infiniteTable;
    const int32_t *infiniteSlot = nullptr; // per light: its index in `infinite`, or -1
    const uint32_t *cut = nullptr;         // tree nodes whose importances stand for the tree
    uint32_t cutCount = 0;
    // Light linking (M9 D15; `links.light` covers lightCount lights, null:
    // no linking): a subtree with no light that lights the receiver's set
    // has importance 0, so selection and probabilities see only the lights
    // that light it, for any number of receiver sets.
    LightLinks links;
    uint32_t receiverSetCount = 0; // the sets `links.receiver` covers
    // Domes with light portals (docs/design/M11-P9b.md), per infinite slot:
    // their triangles [portalBegin, + portalCount) of portalTris (null: no
    // portals anywhere). Their infiniteE is the mean radiance, which the
    // portals' solid angle at the receiver turns into an irradiance.
    const PortalTriangle *portalTris = nullptr;
    const uint32_t *portalBegin = nullptr;
    const uint32_t *portalCount = nullptr;

    // The importance of a node for a receiver whose set has these link
    // categories (LightLinks::categories).
    NOVA_INLINE float importance(uint32_t node, Vec3f p, Vec3f n, Receiver r, uint64_t categories) const {
        if (links.light && !nodes[node].linksTo(categories))
            return 0.0f;
        return lightImportance(nodes[node], p, n, r);
    }
    NOVA_INLINE bool linksTo(uint32_t light, uint64_t categories) const {
        return !links.light || lightLinks(links.light[light], categories);
    }

    // The share of the lights at infinity at a receiver of receiver set `set`.
    NOVA_INLINE float infiniteShare(Vec3f p, Vec3f n, Receiver r, uint32_t set) const {
        return infiniteShareOf(p, n, r, links.categories(set));
    }
    NOVA_INLINE float infiniteShareOf(Vec3f p, Vec3f n, Receiver r, uint64_t categories) const {
        if (infiniteCount == 0)
            return 0.0f;
        float inf = 0.0f;
        for (uint32_t i = 0; i < infiniteCount; ++i) {
            float c = 1.0f;
            const Vec3f dir = infiniteDir[i];
            if (r != Receiver::None && (dir.x != 0.0f || dir.y != 0.0f || dir.z != 0.0f)) {
                c = dot(n, dir);
                c = r == Receiver::TwoSided ? std::fabs(c) : (c > 0.0f ? c : 0.0f);
            }
            float e = infiniteE[i];
            if (portalCount && portalCount[i] > 0)
                e *= portalSolidAngle(portalTris + portalBegin[i], portalCount[i], p);
            inf += linksTo(infinite[i], categories) ? e * c : 0.0f;
        }
        if (nodeCount == 0)
            return inf > 0.0f ? 1.0f : 0.0f;
        float tree = 0.0f;
        for (uint32_t i = 0; i < cutCount; ++i)
            tree += importance(cut[i], p, n, r, categories);
        const float total = inf + tree;
        return total > 0.0f ? inf / total : 0.0f;
    }

    // Picks a light for u in [0, 1); kInvalidId if none can light the receiver.
    NOVA_INLINE uint32_t sample(float u, Vec3f p, Vec3f n, Receiver r, uint32_t set, float &pmf) const {
        pmf = 0.0f;
        const uint64_t cats = links.categories(set);
        const float q = infiniteShareOf(p, n, r, cats);
        if (u < q) {
            float pi = 0.0f;
            const uint32_t slot = infiniteTable.sample(nova::fmin(u / q, 0.99999994f), pi);
            if (slot == kInvalidId || !linksTo(infinite[slot], cats)) // unlinked: rejected (unbiased)
                return kInvalidId;
            pmf = q * pi;
            return infinite[slot];
        }
        if (nodeCount == 0 || !(q < 1.0f))
            return kInvalidId;
        u = nova::fmin((u - q) / (1.0f - q), 0.99999994f);
        float p0 = 1.0f - q;
        uint32_t k = 0;
        while (!(nodes[k].flags & LightTreeNode::kLeaf)) {
            const float c0 = importance(k + 1, p, n, r, cats);
            const float c1 = importance(nodes[k].child, p, n, r, cats);
            if (!(c0 + c1 > 0.0f))
                return kInvalidId;
            // The same arithmetic as pmf(), so both give the same numbers.
            const float s = c0 + c1, pl = c0 / s, pr = c1 / s;
            if (u < pl) {
                u = nova::fmin(u / pl, 0.99999994f);
                p0 *= pl;
                k = k + 1;
            } else {
                u = nova::fmin((u - pl) / pr, 0.99999994f);
                p0 *= pr;
                k = nodes[k].child;
            }
        }
        pmf = p0;
        return nodes[k].child;
    }

    // The probability with which sample() returns `light` at this receiver.
    NOVA_INLINE float pmf(uint32_t light, Vec3f p, Vec3f n, Receiver r, uint32_t set) const {
        const uint64_t cats = links.categories(set);
        if (!linksTo(light, cats))
            return 0.0f;
        const float q = infiniteShareOf(p, n, r, cats);
        if (infiniteSlot && infiniteSlot[light] >= 0)
            return q * infiniteTable.pmf[infiniteSlot[light]];
        if (nodeCount == 0 || trail[light] == ~uint64_t(0))
            return 0.0f;
        float pm = 1.0f - q;
        uint64_t bits = trail[light];
        uint32_t k = 0;
        while (!(nodes[k].flags & LightTreeNode::kLeaf)) {
            const float c0 = importance(k + 1, p, n, r, cats);
            const float c1 = importance(nodes[k].child, p, n, r, cats);
            const float s = c0 + c1;
            if (!(s > 0.0f))
                return 0.0f;
            if (bits & 1u) {
                pm *= c1 / s;
                k = nodes[k].child;
            } else {
                pm *= c0 / s;
                k = k + 1;
            }
            bits >>= 1;
        }
        return pm;
    }
};

// Host side: builds and owns the arrays of a LightTreeView.
class LightTree {
public:
    // bounds: per light, with phi 0 for lights the tree leaves out (lights at
    // infinity, lights that cannot be light-sampled); infinitePower and
    // infiniteE / infiniteDir: per light, for the lights at infinity (others
    // 0). The tree is rebuilt from scratch.
    // masks: per light its link (LightLinks::light; empty: no linking);
    // receivers: per receiver set its categories (LightLinks::receiver).
    void build(const std::vector<LightBounds> &bounds, const std::vector<bool> &atInfinity,
               const std::vector<float> &infinitePower, const std::vector<float> &infiniteE,
               const std::vector<Vec3f> &infiniteDir, const std::vector<uint64_t> &masks = {},
               const std::vector<uint64_t> &receivers = {});
    // As build(), but when only the lights' powers changed (the same lights
    // with the same boxes, cones and links) it only sums the powers up the
    // tree; the receiver sets never need a build.
    void update(const std::vector<LightBounds> &bounds, const std::vector<bool> &atInfinity,
                const std::vector<float> &infinitePower, const std::vector<float> &infiniteE,
                const std::vector<Vec3f> &infiniteDir, const std::vector<uint64_t> &masks = {},
                const std::vector<uint64_t> &receivers = {});
    // New powers and boxes with the same lights (value edits; moves keep
    // working but select worse until the next build).
    void refit(const std::vector<LightBounds> &bounds);
    // Light portals of the lights at infinity (after build/update): per
    // light its triangles (empty: none); none at all clears them.
    void setPortals(const std::vector<std::vector<PortalTriangle>> &perLight);

    LightTreeView view() const;
    uint32_t depth() const { return m_depth; }

private:
    void buildNode(std::vector<std::pair<uint32_t, LightBounds>> &items, int begin, int end, uint64_t bits,
                   uint32_t depth, uint32_t index, std::atomic<uint32_t> &maxDepth);
    void setInfinite(const std::vector<bool> &atInfinity, const std::vector<float> &infinitePower,
                     const std::vector<float> &infiniteE, const std::vector<Vec3f> &infiniteDir);
    void linkLeaf(LightTreeNode &node, uint32_t light) const;
    static void linkInterior(LightTreeNode &node, const LightTreeNode &a, const LightTreeNode &b);

    std::vector<LightTreeNode> m_nodes;
    std::vector<uint64_t> m_trail;
    std::vector<uint32_t> m_leaf; // per light: its leaf node (~0: not in the tree)
    std::vector<uint32_t> m_infinite;
    std::vector<float> m_infiniteE;
    std::vector<Vec3f> m_infiniteDir;
    std::vector<int32_t> m_infiniteSlot;
    std::vector<uint32_t> m_cut;
    std::vector<uint64_t> m_mask;     // per light (empty: no linking)
    std::vector<uint64_t> m_receiver; // per receiver set
    LightSampler m_infiniteTable;
    uint32_t m_depth = 0;
    std::vector<PortalTriangle> m_portalTris; // per infinite slot [m_portalBegin, + m_portalCount)
    std::vector<uint32_t> m_portalBegin, m_portalCount;
};

} // namespace nova
