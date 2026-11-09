#pragma once

#include "core/film/film.h"
#include "core/integrator/guide_field.h"
#include "core/integrator/settings.h"
#include "core/integrator/wavefront.h"
#include "core/scene/camera.h"
#include "core/scene/scene.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nova {

namespace gpu {
class GpuRenderer;
}
class Baker;

// Progressive renderer: every renderPass() adds one sample to every pixel.
// Tiles are distributed over TBB worker threads; each tile is one wave.
class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    void setScene(const Scene *scene) {
        m_scene = scene;
        m_lpeProgram = nullptr;
        m_guide.reset(); // another scene's field (versions are per scene)
    }
    void setCamera(const Camera &camera) { m_camera = camera; }
    const Camera &camera() const { return m_camera; }
    void setSettings(const RenderSettings &settings);
    const RenderSettings &settings() const { return m_settings; }
    // Maximum number of worker threads (0 = all hardware threads).
    void setThreadLimit(int threads);

    // Resizes the film; the render region becomes the whole film.
    void resize(int width, int height);
    // Restricts rendering to film pixels [x0, x1) x [y0, y1) (row 0 = bottom),
    // e.g. the host's data window. Pixels outside keep their clear values.
    void setRegion(const TileRect &region);
    const TileRect &region() const { return m_region; }
    // Restarts progressive rendering: an empty film, every pixel active.
    void reset();
    // IPR proxy passes (docs/design/M7.md D6): the first `depth` passes after
    // a reset render one pixel per 2^depth x 2^depth, ..., 2 x 2 block; their
    // samples are those pixels' first samples (0 = none, at most 3).
    void setProxyDepth(int depth) { m_proxyDepth = depth < 0 ? 0 : (depth > 3 ? 3 : depth); }
    // The next pass is the first proxy pass after a reset (the coarsest).
    bool firstProxyPass() const { return m_proxyDepth > 0 && m_pass == 0; }
    // The block size a display fills from its one rendered pixel after the
    // last pass (1: every pixel has samples of its own).
    int fillStride() const { return m_fillStride; }
    // Samples every still-active pixel has.
    uint32_t samplesDone() const { return m_level; }
    // Pixels adaptive sampling still renders (all of the region without it).
    uint32_t activePixels() const { return m_activeCount; }

    // Renders one sample per pixel of the region. `cancel` is polled per tile
    // (from worker threads); returns false if the pass was cancelled, in which
    // case the film must be reset(). An empty scene or region still counts as
    // a completed pass so that progressive rendering always converges.
    bool renderPass(const std::function<bool()> &cancel);

    // Runs body(rowBegin, rowEnd) over row ranges of [y0, y1) on the
    // renderer's worker threads, e.g. to copy the film into host buffers.
    void parallelRows(int y0, int y1, const std::function<void(int, int)> &body) const;

    bool converged() const {
        return m_level >= targetSamples() || (m_settings.adaptive && m_oracleReady && m_activeCount == 0);
    }
    // Path guiding (docs/design/M11-P9c.md): the field (null: guiding has
    // been off), and the training samples the film dropped (clearBuffers).
    const PathGuide *guide() const { return m_guide.get(); }
    uint32_t discardedSamples() const { return m_discarded; }
    // Whether the last pass gave pixels their first sample (and so wrote
    // first-hit AOVs).
    bool firstHitPass() const { return m_firstHitPass; }
    // Whether the GPU rendered the last pass (Nova XPU).
    bool lastPassOnGpu() const { return m_lastPassGpu; }
    // The film; after GPU passes its sums come back from the device first
    // (Nova XPU, docs/design/M10.md G7: only when read).
    const Film &film() {
        syncFilm();
        return m_film;
    }
    uint32_t passes() const { return m_film.samples; } // passes in the film, without syncing it

    // Checkpoints (docs/design/M11-P6.md 5): the film and the progress as
    // bytes, and back into a renderer set up the same way (size, region,
    // settings). A loaded render continues where the saved one stopped:
    // every pixel's sample numbers go on from its count, so the result
    // equals an uninterrupted render.
    void saveCheckpoint(std::vector<uint8_t> &out);
    bool loadCheckpoint(const uint8_t *data, size_t size);
    // Rays traced since the last reset() (closest-hit and occlusion queries).
    uint64_t raysTraced() const { return m_rays.load(std::memory_order_relaxed); }

private:
    struct ThreadQueues;

    void buildTiles();
    void resetPasses();
    // Adaptive sampling (docs/design/M7.md D4): the resolved minimum, the
    // per-pixel state, and the checkpoint evaluation after a pass.
    uint32_t oracleMinSamples() const;
    void initOracle();
    void updateOracle();
    // The light path expression tables for the current program and scene
    // (null without a program).
    const LpeTables *lpeTables();
    // A proxy, capped or adaptive pass's pixels per tile (m_tilePixels): one
    // per stride x stride block, below the maximum, still active.
    void selectPixels(int stride);
    // Nova XPU (docs/design/M10.md D15): renders the pass on the GPU (with
    // lists: m_tilePixels' pixels); false leaves it to the CPU.
    bool gpuPass(bool lists, uint32_t spp, const LpeTables *lpe, const PathGuide *guide);
    // The device film's sums into m_film if GPU passes added to them.
    void syncFilm();
    // The samples per pixel the film is rendered to: the settings' maximum,
    // less the training samples a guided render dropped (clearBuffers).
    uint32_t targetSamples() const {
        return m_discarded >= m_settings.maxSamples ? 1u : m_settings.maxSamples - m_discarded;
    }
    // Path guiding before a pass: the field (re)started when the scene or the
    // settings changed; null when guiding is off.
    const GuideView *prepareGuide(GuideView &view);
    // After a pass of spp samples per pixel: the iteration ends when due
    // (the GPU's records first); clearBuffers drops the training samples.
    void finishGuidePass(uint32_t spp, bool gpu);
    // The sample offset of a film restarted now (Film::sampleOffset).
    uint32_t restartOffset() const;
    // After a pass: the new sample of every pixel into the LPE planes with a
    // sample filter, as the change of the LPE sums since the last pass
    // (docs/design/M11-P6.md 4; such renders take one sample per pass).
    void takeLpeSamples();
    // Texture baking (core/bake/bake.h): one sample per pixel of the tile,
    // on the CPU; the bake meshes are prepared again when the settings or
    // the scene change.
    bool bakePass(const std::function<bool()> &cancel);

    const Scene *m_scene = nullptr;
    Camera m_camera;
    RenderSettings m_settings;
    PixelFilter m_filter; // tabulated from m_settings
    Film m_film;
    TileRect m_region;
    std::vector<TileRect> m_tiles;
    // A pass's work: whole tiles, or chunks of a tile's selected pixels.
    struct WorkItem {
        uint32_t tile, begin, count; // count 0: the whole tile
    };
    std::vector<WorkItem> m_items;
    std::vector<std::vector<uint32_t>> m_tilePixels; // the selected pixels per tile
    std::vector<uint32_t> m_gpuPixels;                // all of them, for the GPU
    int m_threadLimit = 0;
    bool m_firstHitPass = false;
    int m_proxyDepth = 0;
    uint32_t m_pass = 0;       // passes since the reset
    uint32_t m_fullPasses = 0; // passes over every (active) pixel
    uint32_t m_level = 0;      // samples of every active pixel
    int m_fillStride = 1;
    // Adaptive sampling, per film pixel: still rendered; evaluated in the last
    // pass; still noisy (or not evaluated yet); the count of the last
    // checkpoint and the sums there (the half of the next checkpoint); the
    // last error terms (squared difference to the half, mean luminance; RGB
    // and alpha).
    bool m_oracleReady = false;
    bool m_allActive = true;
    uint32_t m_activeCount = 0;
    std::vector<uint8_t> m_active, m_evaluated, m_noisy;
    std::vector<uint32_t> m_checked;
    std::vector<Spectrum> m_half;
    std::vector<float> m_halfAlpha, m_dsq, m_lum, m_dsqAlpha, m_lumAlpha;
    std::unique_ptr<ThreadQueues> m_queues;
    // The GPU device (created by the first XPU pass); none usable: the CPU
    // renders; the device film holds m_film's samples.
    std::unique_ptr<gpu::GpuRenderer> m_gpu;
    bool m_gpuFailed = false;
    bool m_gpuFilm = false;
    bool m_lastPassGpu = false;
    bool m_filmStale = false; // GPU passes added sums m_film lacks
    std::atomic<uint64_t> m_rays{0};
    // takeLpeSamples(): per filtered LPE plane the sums at the last pass,
    // and the pixels' counts then.
    std::vector<std::vector<Spectrum>> m_lpePrev;
    std::vector<uint32_t> m_lpePrevCount;
    LpeTables m_lpe;
    const lpe::Compiled *m_lpeProgram = nullptr; // the program m_lpe was built for
    uint64_t m_lpeVersion = 0;                   // and the scene version
    uint64_t m_lpeGeneration = 0;                // counts m_lpe's builds (the GPU's copy follows it)
    std::unique_ptr<PathGuide> m_guide;
    uint32_t m_discarded = 0; // training samples dropped from the film (clearBuffers)
    bool m_gpuGuide = false;  // the device holds records of the current training iteration
    std::unique_ptr<Baker> m_baker;
    const BakeSettings *m_bakeBuilt = nullptr; // the settings and scene version m_baker was built for
    uint64_t m_bakeVersion = 0;
};

} // namespace nova
