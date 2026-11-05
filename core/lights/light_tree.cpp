#include "core/lights/light_tree.h"

#include <tbb/parallel_invoke.h>

#include <algorithm>
#include <cmath>

namespace nova {

namespace {

float safeAcos(float x) { return std::acos(std::clamp(x, -1.0f, 1.0f)); }

Vec3f rotateAbout(Vec3f v, Vec3f axis, float angle) { // Rodrigues
    axis = normalize(axis);
    const float c = std::cos(angle), s = std::sin(angle);
    return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.0f - c));
}

// The smallest cone containing two direction cones (pbrt-v4).
void coneUnion(Vec3f wa, float cosa, Vec3f wb, float cosb, Vec3f &w, float &cosOut) {
    const float ta = safeAcos(cosa), tb = safeAcos(cosb), td = safeAcos(dot(wa, wb));
    if (std::min(td + tb, kPi) <= ta) {
        w = wa;
        cosOut = cosa;
        return;
    }
    if (std::min(td + ta, kPi) <= tb) {
        w = wb;
        cosOut = cosb;
        return;
    }
    const float to = 0.5f * (ta + td + tb);
    const Vec3f wr = cross(wa, wb);
    if (to >= kPi || !(dot(wr, wr) > 1e-12f)) {
        w = wa;
        cosOut = -1.0f;
        return;
    }
    w = normalize(rotateAbout(wa, wr, to - ta));
    cosOut = std::cos(to);
}

LightBounds unite(const LightBounds &a, const LightBounds &b) {
    if (!(a.phi > 0.0f))
        return b;
    if (!(b.phi > 0.0f))
        return a;
    LightBounds u;
    u.lo = {std::min(a.lo.x, b.lo.x), std::min(a.lo.y, b.lo.y), std::min(a.lo.z, b.lo.z)};
    u.hi = {std::max(a.hi.x, b.hi.x), std::max(a.hi.y, b.hi.y), std::max(a.hi.z, b.hi.z)};
    u.phi = a.phi + b.phi;
    // Two-sided emitters cover their cone and its mirror: a cone of normals
    // that includes both directions.
    Vec3f wa = a.w, wb = b.w;
    float ca = a.cosO, cb = b.cosO;
    if (a.twoSided != b.twoSided) {
        if (a.twoSided)
            ca = -1.0f;
        else
            cb = -1.0f;
    }
    coneUnion(wa, ca, wb, cb, u.w, u.cosO);
    u.cosE = std::min(a.cosE, b.cosE);
    u.twoSided = a.twoSided && b.twoSided;
    u.crossable = a.crossable || b.crossable;
    return u;
}

float component(Vec3f v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; }
Vec3f centre(const LightBounds &b) { return (b.lo + b.hi) * 0.5f; }

// pbrt-v4's surface area orientation heuristic (SAOH).
float cost(const LightBounds &b, const LightBounds &all, int dim) {
    const float tO = safeAcos(b.cosO), tE = safeAcos(b.cosE);
    const float tW = std::min(tO + tE, kPi), sinO = std::sin(tO);
    const float mO = kTwoPi * (1.0f - b.cosO) +
                     kHalfPi * (2.0f * tW * sinO - std::cos(tO - 2.0f * tW) - 2.0f * tO * sinO + b.cosO);
    const Vec3f d = all.hi - all.lo;
    const float kr = std::max(d.x, std::max(d.y, d.z)) / std::max(component(d, dim), 1e-20f);
    const Vec3f e = b.hi - b.lo;
    const float area = 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
    return b.phi * mO * kr * std::max(area, 1e-20f);
}

LightTreeNode nodeOf(const LightBounds &b) {
    LightTreeNode n{};
    n.lo = b.lo;
    n.hi = b.hi;
    n.phi = b.phi;
    n.w = b.w;
    n.cosO = b.cosO;
    n.cosE = b.cosE;
    n.flags = (b.twoSided ? LightTreeNode::kTwoSided : 0u) | (b.crossable ? LightTreeNode::kCrossable : 0u);
    return n;
}

} // namespace

// A subtree over k lights takes 2k - 1 consecutive nodes in depth-first
// order, so both children's slots are known before either is built: large
// subtrees build in parallel into disjoint ranges.
void LightTree::buildNode(std::vector<std::pair<uint32_t, LightBounds>> &items, int begin, int end, uint64_t bits,
                          uint32_t depth, uint32_t index, std::atomic<uint32_t> &maxDepth) {
    for (uint32_t d = maxDepth.load(std::memory_order_relaxed); d < depth;)
        if (maxDepth.compare_exchange_weak(d, depth, std::memory_order_relaxed))
            break;
    if (end - begin == 1) {
        const uint32_t light = items[size_t(begin)].first;
        m_nodes[index] = nodeOf(items[size_t(begin)].second);
        m_nodes[index].flags |= LightTreeNode::kLeaf;
        m_nodes[index].child = light;
        m_nodes[index].mask = m_mask.empty() ? ~uint64_t(0) : m_mask[light];
        m_trail[light] = bits;
        m_leaf[light] = index;
        return;
    }
    LightBounds all;
    Vec3f clo{1e30f, 1e30f, 1e30f}, chi{-1e30f, -1e30f, -1e30f};
    for (int i = begin; i < end; ++i) {
        all = unite(all, items[size_t(i)].second);
        const Vec3f c = centre(items[size_t(i)].second);
        clo = {std::min(clo.x, c.x), std::min(clo.y, c.y), std::min(clo.z, c.z)};
        chi = {std::max(chi.x, c.x), std::max(chi.y, c.y), std::max(chi.z, c.z)};
    }
    // Deep trees fall back to median splits: the bit trail holds 64 levels.
    int mid = (begin + end) / 2;
    if (depth < 48) {
        constexpr int kBuckets = 12;
        float best = 3.4e38f;
        int bestDim = -1, bestBucket = -1;
        for (int dim = 0; dim < 3; ++dim) {
            const float lo = component(clo, dim), hi = component(chi, dim);
            if (!(hi > lo))
                continue;
            LightBounds bucket[kBuckets];
            for (int i = begin; i < end; ++i) {
                const float c = component(centre(items[size_t(i)].second), dim);
                const int b = std::min(kBuckets - 1, int(float(kBuckets) * (c - lo) / (hi - lo)));
                bucket[b] = unite(bucket[b], items[size_t(i)].second);
            }
            for (int s = 0; s < kBuckets - 1; ++s) {
                LightBounds a, c;
                for (int i = 0; i <= s; ++i)
                    a = unite(a, bucket[i]);
                for (int i = s + 1; i < kBuckets; ++i)
                    c = unite(c, bucket[i]);
                if (!(a.phi > 0.0f) || !(c.phi > 0.0f))
                    continue;
                const float k = cost(a, all, dim) + cost(c, all, dim);
                if (k > 0.0f && k < best) {
                    best = k;
                    bestDim = dim;
                    bestBucket = s;
                }
            }
        }
        if (bestDim >= 0) {
            const float lo = component(clo, bestDim), hi = component(chi, bestDim);
            const auto split = std::partition(items.begin() + begin, items.begin() + end, [&](const auto &x) {
                const float c = component(centre(x.second), bestDim);
                return std::min(kBuckets - 1, int(float(kBuckets) * (c - lo) / (hi - lo))) <= bestBucket;
            });
            mid = int(split - items.begin());
            if (mid == begin || mid == end)
                mid = (begin + end) / 2;
        }
    }
    const uint32_t second = index + 2u * uint32_t(mid - begin);
    const auto left = [&] { buildNode(items, begin, mid, bits, depth + 1, index + 1, maxDepth); };
    const auto right = [&] { buildNode(items, mid, end, bits | (uint64_t(1) << depth), depth + 1, second, maxDepth); };
    if (end - begin > 4096)
        tbb::parallel_invoke(left, right);
    else {
        left();
        right();
    }
    m_nodes[index] = nodeOf(all);
    m_nodes[index].child = second;
    m_nodes[index].mask = m_nodes[index + 1].mask | m_nodes[second].mask;
}

void LightTree::build(const std::vector<LightBounds> &bounds, const std::vector<bool> &atInfinity,
                      const std::vector<float> &infinitePower, const std::vector<float> &infiniteE,
                      const std::vector<Vec3f> &infiniteDir, const std::vector<uint64_t> &masks) {
    const size_t n = bounds.size();
    m_mask = masks.size() == n ? masks : std::vector<uint64_t>{};
    m_trail.assign(n, ~uint64_t(0));
    m_leaf.assign(n, ~uint32_t(0));
    std::vector<std::pair<uint32_t, LightBounds>> items;
    for (size_t i = 0; i < n; ++i)
        if (!atInfinity[i] && bounds[i].phi > 0.0f)
            items.emplace_back(uint32_t(i), bounds[i]);
    m_nodes.assign(items.empty() ? 0 : 2 * items.size() - 1, LightTreeNode{});
    std::atomic<uint32_t> depth{0};
    if (!items.empty())
        buildNode(items, 0, int(items.size()), 0, 0, 0, depth);
    m_depth = depth.load();
    setInfinite(atInfinity, infinitePower, infiniteE, infiniteDir);

    // The cut: the shallowest 16 nodes (breadth first) that partition the tree.
    m_cut.clear();
    if (!m_nodes.empty()) {
        m_cut.push_back(0);
        for (bool changed = true; changed && m_cut.size() < 16;) {
            changed = false;
            std::vector<uint32_t> next;
            for (size_t i = 0; i < m_cut.size(); ++i) {
                const uint32_t k = m_cut[i];
                // Split while the level, with what is left of it, stays within 16.
                if (!(m_nodes[k].flags & LightTreeNode::kLeaf) && next.size() + (m_cut.size() - i) + 1 <= 16) {
                    next.push_back(k + 1);
                    next.push_back(m_nodes[k].child);
                    changed = true;
                } else {
                    next.push_back(k);
                }
            }
            m_cut = std::move(next);
        }
    }
}

// Lights at infinity: their table and irradiances.
void LightTree::setInfinite(const std::vector<bool> &atInfinity, const std::vector<float> &infinitePower,
                            const std::vector<float> &infiniteE, const std::vector<Vec3f> &infiniteDir) {
    const size_t n = atInfinity.size();
    m_infinite.clear();
    m_infiniteE.clear();
    m_infiniteDir.clear();
    m_infiniteSlot.assign(n, -1);
    std::vector<float> weights;
    for (size_t i = 0; i < n; ++i)
        if (atInfinity[i]) {
            m_infiniteSlot[i] = int32_t(m_infinite.size());
            m_infinite.push_back(uint32_t(i));
            m_infiniteE.push_back(infiniteE[i]);
            m_infiniteDir.push_back(infiniteDir[i]);
            weights.push_back(infinitePower[i]);
        }
    m_infiniteTable.build(weights);
    if (m_infiniteTable.view().count == 0) // nothing at infinity can be light-sampled
        m_infiniteE.assign(m_infiniteE.size(), 0.0f);
}

void LightTree::update(const std::vector<LightBounds> &bounds, const std::vector<bool> &atInfinity,
                       const std::vector<float> &infinitePower, const std::vector<float> &infiniteE,
                       const std::vector<Vec3f> &infiniteDir, const std::vector<uint64_t> &masks) {
    // The same lights in the same places (only their powers changed, as by
    // an intensity or colour edit): the powers are summed up the tree again,
    // everything else stays (IPR, docs/design/M9.md D11). Otherwise a build.
    const size_t n = bounds.size();
    bool same = n == m_trail.size() && n == m_leaf.size() && (masks.size() == n ? masks == m_mask : m_mask.empty());
    for (size_t i = 0; same && i < n; ++i) {
        const bool inTree = !atInfinity[i] && bounds[i].phi > 0.0f;
        if (inTree != (m_leaf[i] != ~uint32_t(0)) || atInfinity[i] != (m_infiniteSlot[i] >= 0)) {
            same = false;
            break;
        }
        if (!inTree)
            continue;
        const LightTreeNode &leaf = m_nodes[m_leaf[i]];
        const LightTreeNode fresh = nodeOf(bounds[i]);
        const auto eq = [](Vec3f a, Vec3f b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
        same = eq(leaf.lo, fresh.lo) && eq(leaf.hi, fresh.hi) && eq(leaf.w, fresh.w) && leaf.cosO == fresh.cosO &&
               leaf.cosE == fresh.cosE && (leaf.flags & ~LightTreeNode::kLeaf) == fresh.flags;
    }
    if (!same) {
        build(bounds, atInfinity, infinitePower, infiniteE, infiniteDir, masks);
        return;
    }
    for (size_t k = m_nodes.size(); k-- > 0;) { // children come after their parent
        LightTreeNode &node = m_nodes[k];
        node.phi = (node.flags & LightTreeNode::kLeaf) ? bounds[node.child].phi
                                                       : m_nodes[k + 1].phi + m_nodes[node.child].phi;
    }
    setInfinite(atInfinity, infinitePower, infiniteE, infiniteDir);
}

void LightTree::refit(const std::vector<LightBounds> &bounds) {
    // Depth-first order: children come after their parent.
    for (size_t k = m_nodes.size(); k-- > 0;) {
        LightTreeNode &node = m_nodes[k];
        const uint32_t flags = node.flags;
        if (flags & LightTreeNode::kLeaf) {
            const uint32_t light = node.child;
            const uint64_t mask = node.mask;
            node = nodeOf(bounds[light]);
            node.flags |= LightTreeNode::kLeaf;
            node.child = light;
            node.mask = mask;
        } else {
            LightBounds a, b;
            const auto boundsOf = [](const LightTreeNode &x) {
                LightBounds lb;
                lb.lo = x.lo;
                lb.hi = x.hi;
                lb.phi = x.phi;
                lb.w = x.w;
                lb.cosO = x.cosO;
                lb.cosE = x.cosE;
                lb.twoSided = (x.flags & LightTreeNode::kTwoSided) != 0;
                lb.crossable = (x.flags & LightTreeNode::kCrossable) != 0;
                return lb;
            };
            const uint32_t second = node.child;
            const uint64_t mask = node.mask;
            node = nodeOf(unite(boundsOf(m_nodes[k + 1]), boundsOf(m_nodes[second])));
            node.child = second;
            node.mask = mask;
        }
    }
}

LightTreeView LightTree::view() const {
    LightTreeView v;
    v.nodes = m_nodes.data();
    v.trail = m_trail.data();
    v.lightCount = uint32_t(m_trail.size());
    v.nodeCount = uint32_t(m_nodes.size());
    v.infinite = m_infinite.data();
    v.infiniteE = m_infiniteE.data();
    v.infiniteDir = m_infiniteDir.data();
    v.infiniteCount = uint32_t(m_infinite.size());
    v.infiniteTable = m_infiniteTable.view();
    v.infiniteSlot = m_infiniteSlot.data();
    v.cut = m_cut.data();
    v.cutCount = uint32_t(m_cut.size());
    v.lightMask = m_mask.empty() ? nullptr : m_mask.data();
    return v;
}

} // namespace nova
