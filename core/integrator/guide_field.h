#pragma once

// The path guiding field on the host (docs/design/M11-P9c.md): the arrays
// guiding.h reads, the training schedule and the refinement between
// iterations. The Renderer owns one; passes read view(), the GPU copies the
// arrays when generation() changes and adds its record sums back
// (GpuRenderer::downloadGuide) before an iteration ends.

#include "core/integrator/guiding.h"
#include "core/integrator/settings.h"

#include <cstdint>
#include <vector>

namespace nova {

class PathGuide {
public:
    // Records a spatial leaf takes before it splits after iteration k:
    // kSpatialThreshold x sqrt(the iteration's samples per pixel) (PPG's c
    // sqrt(2^k); PPG's c 12000 is for larger images, measured: 1000 guides
    // a 160 x 120 room best); a quadrant with more than kDirThreshold of its
    // leaf's energy is subdivided; the quadtrees of all leaves stay under
    // kMaxNodes nodes. A new building quadtree starts with kCarry of the
    // energy learned so far (each iteration twice as long: every sample
    // weighs the same; PPG starts from zero).
    static constexpr float kSpatialThreshold = 1000.0f;
    static constexpr float kDirThreshold = 0.01f;
    static constexpr size_t kMaxNodes = size_t(1) << 20;
    static constexpr float kCarry = 0.5f;
    static constexpr int kMaxSpatialDepth = 48;

    // Starts over over the box [lower, upper] (made a cube; an empty box: a
    // unit cube at the origin): no distribution yet, training from now on.
    void reset(Vec3f lower, Vec3f upper, const GuidingSettings &settings, uint64_t sceneVersion);
    bool valid() const { return m_valid; }
    // The field was made for these settings and this scene version.
    bool matches(const GuidingSettings &settings, uint64_t sceneVersion) const {
        return m_valid && settings == m_settings && sceneVersion == m_version;
    }
    // What the next pass reads and records into.
    GuideView view() const;
    bool training() const { return m_training; }
    bool sampling() const { return m_sampling; }
    // Whether a pass of spp samples per pixel ends a training iteration (the
    // GPU's sums must be in the host's arrays by then).
    bool iterationEnds(uint32_t spp) const;
    // After a pass of spp samples per pixel: ends the iteration when due
    // (refines the field). Returns true when training ended with this pass.
    bool endPass(uint32_t spp);
    // Samples per pixel trained since reset (statistics).
    uint32_t trainedSamples() const { return m_samples; }
    // Changes whenever the arrays do (the GPU's copies follow it).
    uint64_t generation() const { return m_generation; }
    // Refinements done (statistics, tests).
    uint32_t iterations() const { return m_iterations; }

    // The field (guiding.h's layout).
    std::vector<GuideSpatialNode> spatial;
    std::vector<GuideLeaf> leaves;
    std::vector<GuideDirNode> nodes;  // sampling quadtrees
    std::vector<uint32_t> buildChild; // building quadtrees, 4 children per node
    std::vector<uint32_t> buildLeaf;  // per building node: its spatial leaf
    std::vector<uint64_t> buildSum;   // 4 fixed-point sums per building node
    std::vector<uint64_t> leafCount;  // records per spatial leaf

private:
    // The building quadtrees become the sampling ones; with `train` the
    // spatial tree splits and new building quadtrees follow the sampling
    // ones' energy.
    void refine(bool train);
    uint32_t iterationLength() const;

    GuidingSettings m_settings;
    uint64_t m_version = 0;
    bool m_valid = false;
    Vec3f m_origin{0.0f, 0.0f, 0.0f};
    float m_size = 1.0f;
    uint32_t m_samples = 0;      // trained samples per pixel
    uint32_t m_iterStart = 0;    // m_samples when the iteration began
    uint32_t m_iteration = 0;    // k: the iteration covers 2^k samples per pixel
    uint32_t m_iterations = 0;
    bool m_training = false;
    bool m_sampling = false;
    uint64_t m_generation = 1;
};

} // namespace nova
