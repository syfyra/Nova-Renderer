#include "core/integrator/guide_field.h"

#include <algorithm>
#include <cmath>

namespace nova {

namespace {

float nodeTotal(const GuideDirNode &n) { return n.sum[0] + n.sum[1] + n.sum[2] + n.sum[3]; }

} // namespace

void PathGuide::reset(Vec3f lower, Vec3f upper, const GuidingSettings &settings, uint64_t sceneVersion) {
    m_settings = settings;
    m_version = sceneVersion;
    m_valid = true;
    if (lower.x <= upper.x && lower.y <= upper.y && lower.z <= upper.z && std::isfinite(lower.x) &&
        std::isfinite(upper.x) && std::isfinite(lower.y) && std::isfinite(upper.y) && std::isfinite(lower.z) &&
        std::isfinite(upper.z)) {
        const float edge = std::max({upper.x - lower.x, upper.y - lower.y, upper.z - lower.z});
        // A margin keeps points on the bounds inside the cube.
        m_size = std::max(edge * 1.002f, 1e-3f);
        const Vec3f center = (lower + upper) * 0.5f;
        m_origin = center - Vec3f{m_size, m_size, m_size} * 0.5f;
    } else {
        m_size = 1.0f;
        m_origin = Vec3f{-0.5f, -0.5f, -0.5f};
    }
    spatial.assign(1, GuideSpatialNode{0, 0});
    leaves.assign(1, GuideLeaf{kInvalidId, 0});
    nodes.clear();
    m_samples = m_iterStart = m_iteration = m_iterations = 0;
    m_training = true;
    m_sampling = false;
    // The first building quadtree: uniform (no energy known yet).
    refine(true);
    m_iterations = 0;
    ++m_generation;
}

GuideView PathGuide::view() const {
    GuideView v;
    if (!m_valid)
        return v;
    v.spatial = spatial.data();
    v.leaves = leaves.data();
    v.nodes = nodes.empty() ? nullptr : nodes.data();
    v.buildChild = buildChild.empty() ? nullptr : buildChild.data();
    v.buildLeaf = buildLeaf.empty() ? nullptr : buildLeaf.data();
    // The sums take the records of passes reading this view (atomic adds).
    v.buildSum = buildSum.empty() ? nullptr : const_cast<uint64_t *>(buildSum.data());
    v.leafCount = leafCount.empty() ? nullptr : const_cast<uint64_t *>(leafCount.data());
    v.origin = m_origin;
    v.size = m_size;
    v.components = m_settings.components;
    v.sampling = m_sampling && !nodes.empty();
    v.training = m_training && !buildChild.empty();
    // Records scaled by the iteration's length: a node's sum stays the size of
    // one pass's (no overflow late in long renders).
    v.recordScale = guide::kRecordScale / float(iterationLength());
    return v;
}

uint32_t PathGuide::iterationLength() const { return 1u << std::min(m_iteration, 30u); }

bool PathGuide::iterationEnds(uint32_t spp) const {
    if (!m_valid || !m_training)
        return false;
    const uint32_t after = m_samples + spp;
    if (m_settings.trainingSamples > 0 && after >= m_settings.trainingSamples)
        return true;
    return after - m_iterStart >= iterationLength();
}

bool PathGuide::endPass(uint32_t spp) {
    if (!m_valid || !m_training)
        return false;
    m_samples += spp;
    const uint32_t done = m_samples - m_iterStart;
    if (m_settings.trainingSamples > 0 && m_samples >= m_settings.trainingSamples) {
        // Training ends here: the last field from everything recorded (the
        // earlier iterations' energy is carried, so a short last iteration
        // only adds to it).
        refine(false);
        m_training = false;
        buildChild.clear();
        buildLeaf.clear();
        buildSum.clear();
        ++m_generation;
        return true;
    }
    if (done >= iterationLength()) {
        refine(true);
        m_iterStart = m_samples;
        ++m_iteration;
    }
    return false;
}

void PathGuide::refine(bool train) {
    const uint32_t iterationSamples = std::max(1u, m_samples - m_iterStart);
    // 1. The building quadtrees (when there are any records) become the
    //    sampling ones: the same structure, the sums propagated up.
    std::vector<uint32_t> leafRoot(leaves.size(), kInvalidId);
    if (!buildChild.empty() && m_samples > 0) {
        std::vector<GuideDirNode> out;
        out.reserve(buildChild.size() / 4);
        // Depth-first; returns the node's total.
        struct Frame {
            uint32_t build, index, quadrant;
        };
        std::vector<Frame> stack;
        for (uint32_t L = 0; L < uint32_t(leaves.size()); ++L) {
            const uint32_t root = uint32_t(out.size());
            out.push_back(GuideDirNode{});
            stack.assign(1, Frame{leaves[L].buildRoot, root, 0});
            while (!stack.empty()) {
                Frame &f = stack.back();
                if (f.quadrant == 4) {
                    const float total = nodeTotal(out[f.index]);
                    const uint32_t index = f.index;
                    stack.pop_back();
                    if (!stack.empty()) {
                        Frame &parent = stack.back();
                        out[parent.index].sum[parent.quadrant] = total;
                        out[parent.index].child[parent.quadrant] = index;
                        ++parent.quadrant;
                    }
                    continue;
                }
                const uint32_t q = f.quadrant;
                const uint32_t c = buildChild[4 * f.build + q];
                if (c == 0) {
                    out[f.index].sum[q] = float(double(buildSum[4 * f.build + q]) / double(guide::kRecordScale));
                    out[f.index].child[q] = 0;
                    ++f.quadrant;
                } else {
                    const uint32_t index = uint32_t(out.size());
                    out.push_back(GuideDirNode{});
                    stack.push_back(Frame{c, index, 0}); // f may dangle now
                }
            }
            if (nodeTotal(out[root]) > 0.0f) {
                leafRoot[L] = root;
            } else {
                out.resize(root); // no energy: no distribution there
            }
        }
        nodes = std::move(out);
        m_sampling = std::any_of(leafRoot.begin(), leafRoot.end(), [](uint32_t r) { return r != kInvalidId; });
        ++m_iterations;
    } else if (!leaves.empty() && m_sampling) {
        for (size_t L = 0; L < leaves.size(); ++L)
            leafRoot[L] = leaves[L].sampleRoot;
    }
    if (!train) {
        for (size_t L = 0; L < leaves.size(); ++L)
            leaves[L].sampleRoot = leafRoot[L];
        leafCount.assign(leaves.size(), 0);
        return;
    }

    // 2. The spatial tree: a leaf with more records than the threshold
    //    splits (recursively, its records halved), within the node budget;
    //    the children start from its distribution.
    const double threshold = double(kSpatialThreshold) * std::sqrt(double(iterationSamples));
    std::vector<uint32_t> nodeRoot(spatial.size(), kInvalidId);
    std::vector<double> nodeCount(spatial.size(), 0.0);
    std::vector<int> nodeDepth(spatial.size(), 0);
    std::vector<double> nodeCarry(spatial.size(), 1.0); // the share of the leaf's energy a node keeps
    size_t leafTotal = 0;
    for (size_t n = 0; n < spatial.size(); ++n) {
        if (spatial[n].child != 0) {
            nodeDepth[spatial[n].child] = nodeDepth[n] + 1;
            nodeDepth[spatial[n].child + 1] = nodeDepth[n] + 1;
            continue;
        }
        const uint32_t L = spatial[n].leaf;
        nodeRoot[n] = L < leafRoot.size() ? leafRoot[L] : kInvalidId;
        nodeCount[n] = L < leafCount.size() ? double(leafCount[L]) : 0.0;
        ++leafTotal;
    }
    const size_t perLeaf = std::max<size_t>(85, nodes.size() / std::max<size_t>(1, leafTotal));
    std::vector<uint32_t> work;
    for (uint32_t n = 0; n < uint32_t(spatial.size()); ++n)
        if (spatial[n].child == 0)
            work.push_back(n);
    while (!work.empty()) {
        const uint32_t n = work.back();
        work.pop_back();
        if (nodeCount[n] <= threshold || nodeDepth[n] >= kMaxSpatialDepth || (leafTotal + 1) * perLeaf * 2 > kMaxNodes)
            continue;
        const uint32_t c = uint32_t(spatial.size());
        spatial[n].child = c;
        spatial.push_back(GuideSpatialNode{0, 0});
        spatial.push_back(GuideSpatialNode{0, 0});
        for (int i = 0; i < 2; ++i) {
            nodeRoot.push_back(nodeRoot[n]);
            nodeCount.push_back(nodeCount[n] * 0.5);
            nodeDepth.push_back(nodeDepth[n] + 1);
            nodeCarry.push_back(nodeCarry[n] * 0.5);
            work.push_back(c + uint32_t(i));
        }
        ++leafTotal;
    }
    leaves.clear();
    std::vector<double> leafCarry;
    for (uint32_t n = 0; n < uint32_t(spatial.size()); ++n) {
        if (spatial[n].child != 0)
            continue;
        spatial[n].leaf = uint32_t(leaves.size());
        leaves.push_back(GuideLeaf{nodeRoot[n], 0});
        leafCarry.push_back(nodeCarry[n]);
    }


    // 3. New building quadtrees (PPG): a quadrant holding more than
    //    kDirThreshold of the leaf's energy is subdivided, a leaf quadrant's
    //    energy spread evenly over its children; without energy a uniform
    //    tree (0.25^depth). Sums start at zero.
    buildChild.clear();
    buildLeaf.clear();
    std::vector<uint64_t> sums;
    struct Entry {
        uint32_t node, src;
        float uniform;
        int depth;
    };
    std::vector<Entry> stack;
    for (uint32_t L = 0; L < uint32_t(leaves.size()); ++L) {
        const auto alloc = [&] {
            const uint32_t i = uint32_t(buildLeaf.size());
            buildChild.insert(buildChild.end(), 4, 0u);
            sums.insert(sums.end(), 4, uint64_t(0));
            buildLeaf.push_back(L);
            return i;
        };
        const double carry = double(kCarry) * leafCarry[L] * double(guide::kRecordScale);
        const uint32_t src = leaves[L].sampleRoot;
        const float total = src != kInvalidId ? nodeTotal(nodes[src]) : 0.0f;
        leaves[L].buildRoot = alloc();
        stack.assign(1, Entry{leaves[L].buildRoot, src, 0.0f, 1});
        while (!stack.empty()) {
            const Entry e = stack.back();
            stack.pop_back();
            for (uint32_t q = 0; q < 4; ++q) {
                float s = e.uniform;
                uint32_t srcChild = kInvalidId;
                if (e.src != kInvalidId) {
                    s = nodes[e.src].sum[q];
                    srcChild = nodes[e.src].child[q] != 0 ? nodes[e.src].child[q] : kInvalidId;
                }
                const float fraction = total > 0.0f ? s / total : std::pow(0.25f, float(e.depth));
                if (e.depth >= guide::kMaxDepth || !(fraction > kDirThreshold)) {
                    if (total > 0.0f && s > 0.0f)
                        sums[4 * e.node + q] = uint64_t(std::llround(double(s) * carry));
                    continue;
                }
                const uint32_t c = alloc();
                buildChild[4 * e.node + q] = c;
                stack.push_back(Entry{c, srcChild, s * 0.25f, e.depth + 1});
            }
        }
    }
    buildSum = std::move(sums);
    leafCount.assign(leaves.size(), 0);
}

} // namespace nova
