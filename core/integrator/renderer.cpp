#include "core/integrator/renderer.h"

#include "core/bake/bake.h"
#include "core/film/lpe_compile.h"
#if NOVA_HAS_GPU
#include "core/gpu/gpu_renderer.h"
#else
namespace nova::gpu {
class GpuRenderer {};
} // namespace nova::gpu
#endif

#include <tbb/blocked_range.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

#include <xmmintrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <new>

namespace nova {

namespace {

constexpr int kTileSize = 32;

bool isPow2(uint32_t n) { return n != 0 && (n & (n - 1)) == 0; }

uint32_t ceilPow2(uint32_t n) {
    uint32_t p = 1;
    while (p < n && p < (1u << 31))
        p <<= 1;
    return p;
}

// Cycles' normalization of the error: absolute in a gamma-2 space below 1,
// relative above (docs/research/m7/adaptive-sampling.md 5.1).
float oracleNorm(float s) { return s < 1.0f ? std::sqrt(std::max(s, 0.0f)) : s; }

// Flushes denormals to zero while rendering (Embree's recommendation; they
// slow down traversal) and restores the thread's state afterwards, because
// the TBB workers are shared with the host application.
class ScopedFlushDenormals {
public:
    ScopedFlushDenormals() : m_saved(_mm_getcsr()) { _mm_setcsr(m_saved | 0x8040u); } // FTZ | DAZ
    ~ScopedFlushDenormals() { _mm_setcsr(m_saved); }
    ScopedFlushDenormals(const ScopedFlushDenormals &) = delete;
    ScopedFlushDenormals &operator=(const ScopedFlushDenormals &) = delete;

private:
    unsigned int m_saved;
};

} // namespace

struct Renderer::ThreadQueues {
    tbb::enumerable_thread_specific<WaveQueues> perThread;
    tbb::task_arena arena{tbb::task_arena::automatic};
};

Renderer::Renderer() : m_queues(std::make_unique<ThreadQueues>()) {
    m_filter = makePixelFilter(m_settings.pixelFilter, m_settings.pixelFilterScale);
}
Renderer::~Renderer() = default;

void Renderer::setSettings(const RenderSettings &settings) {
    if (settings.pixelFilter != m_settings.pixelFilter || settings.pixelFilterScale != m_settings.pixelFilterScale)
        m_filter = makePixelFilter(settings.pixelFilter, settings.pixelFilterScale);
    if (settings.lpe != m_settings.lpe) {
        const lpe::Compiled *c = settings.lpe.get();
        const bool ok = c && c->ok();
        m_film.setLpeOutputs(ok ? uint32_t(c->outputFlags.size()) : 0u, ok ? c->shadowCount() : 0u);
        m_lpeProgram = nullptr;
    }
    if (settings.utility != m_settings.utility)
        m_film.setUtility(settings.utility ? *settings.utility : std::vector<UtilityRequest>{});
    if (settings.crypto != m_settings.crypto)
        m_film.setCrypto(settings.crypto ? *settings.crypto : std::vector<CryptoRequest>{});
    m_settings = settings;
    // Karma computes outlines only for a render with an outline AOV
    // (docs/design/M11-P9a.md): one of the outline planes.
    m_settings.outline.requested = false;
    if (settings.utility)
        for (const UtilityRequest &r : *settings.utility)
            m_settings.outline.requested |= r.kind >= UtilityKind::Outline && r.kind <= UtilityKind::OutlineDepth;
}

const LpeTables *Renderer::lpeTables() {
    const lpe::Compiled *c = m_settings.lpe.get();
    if (!m_scene || !c || !c->ok() || m_film.lpeOutputs == 0)
        return nullptr;
    if (c == m_lpeProgram && m_scene->version() == m_lpeVersion)
        return &m_lpe;
    LpeTables &t = m_lpe;
    t.program = c->program();
    t.meshTag.assign(m_scene->meshCount(), 0);
    for (MeshId id = 0; id < m_scene->meshCount(); ++id) {
        const std::string &tag = m_scene->meshRecord(id).lpeTag;
        t.meshTag[id] = tag.empty() ? LpeTables::kMaterialTag : uint16_t(c->tagId(tag));
    }
    t.materialTag.assign(m_scene->materialCount(), 0);
    for (MaterialId id = 0; id < m_scene->materialCount(); ++id)
        if (m_scene->hasMaterial(id))
            t.materialTag[id] = uint16_t(c->tagId(m_scene->material(id).desc.lpeTag));
    t.volumeTag.assign(m_scene->volumeCount(), 0);
    for (VolumeId id = 0; id < m_scene->volumeCount(); ++id) {
        const VolumeDesc &v = m_scene->volumeRecord(id).desc;
        if (!v.lpeTag.empty())
            t.volumeTag[id] = uint16_t(c->tagId(v.lpeTag));
        else if (v.material < t.materialTag.size())
            t.volumeTag[id] = t.materialTag[v.material];
    }
    const uint32_t lights = uint32_t(m_scene->lights().size()), analytic = m_scene->analyticLightCount();
    t.lightTag.resize(lights);
    for (uint32_t i = 0; i < analytic; ++i)
        t.lightTag[i] = uint16_t(c->tagId(m_scene->lightLpeTag(i)));
    for (uint32_t i = analytic; i < lights; ++i) { // emissive triangles (M9 D14): their object's tag
        const LightData &l = m_scene->lights()[i];
        t.lightTag[i] = uint16_t(t.objectTag(l.mesh, m_scene->materialIdOf(l.mesh, l.prim)));
    }
    for (size_t l = 0; l < size_t(LobeLabel::kCount); ++l)
        t.label[l] = uint16_t(c->labelId(lpeLabelName(LobeLabel(l))));
    t.albedoSymbol = t.program.symbol(lpe::TypeAlbedo, lpe::ScatterNone, 0, 0);
    t.backgroundSymbol = t.program.symbol(lpe::TypeBackground, lpe::ScatterNone, 0, 0);
    t.albedoAfter.assign(t.program.stateCount, 0);
    t.albedo = false;
    for (lpe::State s = 1; s < t.program.stateCount; ++s) {
        t.albedoAfter[s] = t.program.outputCount(t.program.step(s, t.albedoSymbol)) > 0;
        t.albedo |= t.albedoAfter[s] != 0;
    }
    t.unit = false;
    t.shadowSlot.resize(c->outputFlags.size());
    for (size_t o = 0; o < c->outputFlags.size(); ++o) {
        t.shadowSlot[o] = c->shadowSlot(o);
        t.unit |= c->errors[o].empty() && (c->outputFlags[o] & (lpe::OutputUnoccluded | lpe::OutputShadow));
    }
    t.plainBegin.assign(1, 0);
    t.unitBegin.assign(1, 0);
    t.litBegin.assign(1, 0);
    t.plainOuts.clear();
    t.unitOuts.clear();
    t.litSlots.clear();
    for (lpe::State s = 0; s < t.program.stateCount; ++s) {
        const uint16_t *outs = t.program.outputs(s);
        for (uint32_t i = 0; i < t.program.outputCount(s); ++i) {
            const uint16_t o = outs[i];
            // holdout; outputs are fed like the others: only paths that
            // hit a holdout reach their states (Program::holdoutStart).
            const uint8_t flags = t.program.outputFlags[o];
            if (flags & (lpe::OutputUnoccluded | lpe::OutputShadow))
                t.unitOuts.push_back(o);
            else
                t.plainOuts.push_back(o);
            if ((flags & lpe::OutputShadow) && !(flags & lpe::OutputUnoccluded) && t.shadowSlot[o] >= 0)
                t.litSlots.push_back(uint16_t(t.shadowSlot[o]));
        }
        t.plainBegin.push_back(uint32_t(t.plainOuts.size()));
        t.unitBegin.push_back(uint32_t(t.unitOuts.size()));
        t.litBegin.push_back(uint32_t(t.litSlots.size()));
    }
    m_lpeProgram = c;
    m_lpeVersion = m_scene->version();
    ++m_lpeGeneration; // never 0 (GpuRenderer: unknown tables)
    return &t;
}

void Renderer::setThreadLimit(int threads) {
    threads = std::max(threads, 0);
    if (threads == m_threadLimit)
        return;
    m_threadLimit = threads;
    m_queues->arena.terminate();
    m_queues->arena.initialize(threads > 0 ? threads : int(tbb::task_arena::automatic));
}

void Renderer::resetPasses() {
    m_rays = 0;
    m_pass = 0;
    m_fullPasses = 0;
    m_level = 0;
    m_fillStride = 1;
    m_oracleReady = false; // initOracle() at the next pass
}

void Renderer::reset() {
    m_film.clear();
    m_gpuFilm = false;
    m_filmStale = false;
    resetPasses();
}

void Renderer::resize(int width, int height) {
    if (width == m_film.width && height == m_film.height)
        return;
    m_film.resize(width, height);
    m_gpuFilm = false;
    m_filmStale = false;
    resetPasses();
    m_region = {0, 0, m_film.width, m_film.height};
    buildTiles();
}

void Renderer::setRegion(const TileRect &region) {
    TileRect r;
    r.x0 = std::clamp(region.x0, 0, m_film.width);
    r.y0 = std::clamp(region.y0, 0, m_film.height);
    r.x1 = std::clamp(region.x1, r.x0, m_film.width);
    r.y1 = std::clamp(region.y1, r.y0, m_film.height);
    if (r.x0 == m_region.x0 && r.y0 == m_region.y0 && r.x1 == m_region.x1 && r.y1 == m_region.y1)
        return;
    m_region = r;
    buildTiles();
}

void Renderer::buildTiles() {
    m_tiles.clear();
    // The old region's selected pixels go too: selectPixels() runs only with
    // tiles, and a GPU list pass would render (and count) them otherwise.
    m_tilePixels.clear();
    for (int y = m_region.y0; y < m_region.y1; y += kTileSize)
        for (int x = m_region.x0; x < m_region.x1; x += kTileSize)
            m_tiles.push_back({x, y, std::min(x + kTileSize, m_region.x1), std::min(y + kTileSize, m_region.y1)});
}

uint32_t Renderer::oracleMinSamples() const {
    const uint32_t maxSamples = std::max(1u, m_settings.maxSamples);
    const uint32_t m = m_settings.oracleMinSamples > 0
                           ? m_settings.oracleMinSamples
                           : std::max(16u, uint32_t(std::ceil(std::sqrt(double(maxSamples)))));
    return ceilPow2(std::max(2u, m));
}

void Renderer::initOracle() {
    m_activeCount = uint32_t(m_region.pixelCount());
    m_allActive = true;
    m_oracleReady = true;
    if (!m_settings.adaptive) { // no per-pixel state (about 40 bytes a pixel)
        for (std::vector<uint8_t> *v : {&m_active, &m_evaluated, &m_noisy})
            std::vector<uint8_t>().swap(*v);
        std::vector<uint32_t>().swap(m_checked);
        std::vector<Spectrum>().swap(m_half);
        for (std::vector<float> *v : {&m_halfAlpha, &m_dsq, &m_lum, &m_dsqAlpha, &m_lumAlpha})
            std::vector<float>().swap(*v);
        return;
    }
    const size_t n = m_film.pixelCount();
    m_active.assign(n, 1);
    m_evaluated.assign(n, 0);
    m_noisy.assign(n, 1);
    m_checked.assign(n, 0);
    m_half.assign(n, Spectrum{0.0f, 0.0f, 0.0f});
    for (std::vector<float> *v : {&m_halfAlpha, &m_dsq, &m_lum, &m_dsqAlpha, &m_lumAlpha})
        v->assign(n, 0.0f);
}

// After a pass: every active pixel whose count reached a power of two from
// half the minimum on keeps its sums as the next checkpoint's half; from the
// minimum on it compares its mean with the half's (the prefix halves are the
// stratified ones with Nova's sampler). The error is pooled over 3 x 3
// pixels; a pixel stops when no pixel of its 3 x 3 neighbourhood is still
// noisy (docs/research/m7/adaptive-sampling.md 5.1, rule pool3nd). Only
// counts at fixed checkpoints decide, so renders stay deterministic.
void Renderer::updateOracle() {
    syncFilm(); // the checkpoints compare sums
    const uint32_t m = oracleMinSamples();
    const float limit = 5.0f * m_settings.oracleThreshold;
    const TileRect r = m_region;
    const int width = m_film.width;
    const auto at = [&](int x, int y) { return size_t(y) * size_t(width) + size_t(x); };
    std::atomic<uint32_t> evaluated{0};
    parallelRows(r.y0, r.y1, [&](int y0, int y1) {
        uint32_t local = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = r.x0; x < r.x1; ++x) {
                const size_t p = at(x, y);
                m_evaluated[p] = 0;
                const uint32_t c = m_film.count[p];
                if (!m_active[p] || c == m_checked[p] || !isPow2(c) || 2 * c < m)
                    continue;
                if (c >= m && m_checked[p] == c / 2) {
                    const float inv = 1.0f / float(c), invHalf = 2.0f / float(c);
                    const Spectrum I = m_film.colorSum[p] * inv, H = m_half[p] * invHalf;
                    const float d = std::fabs(I.x - H.x) + std::fabs(I.y - H.y) + std::fabs(I.z - H.z);
                    const float a = m_film.alphaSum[p] * inv, ah = m_halfAlpha[p] * invHalf;
                    m_dsq[p] = d * d;
                    m_lum[p] = I.x + I.y + I.z;
                    m_dsqAlpha[p] = (a - ah) * (a - ah);
                    m_lumAlpha[p] = a;
                    m_evaluated[p] = 1;
                    ++local;
                }
                m_half[p] = m_film.colorSum[p];
                m_halfAlpha[p] = m_film.alphaSum[p];
                m_checked[p] = c;
            }
        evaluated += local;
    });
    if (evaluated == 0)
        return;
    // The pooled error (3 x 3, the region's edge pixels repeated).
    parallelRows(r.y0, r.y1, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = r.x0; x < r.x1; ++x) {
                const size_t p = at(x, y);
                if (!m_evaluated[p])
                    continue;
                float dsq = 0.0f, lum = 0.0f, dsqA = 0.0f, lumA = 0.0f;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const size_t q = at(std::clamp(x + dx, r.x0, r.x1 - 1), std::clamp(y + dy, r.y0, r.y1 - 1));
                        dsq += m_dsq[q];
                        lum += m_lum[q];
                        dsqA += m_dsqAlpha[q];
                        lumA += m_lumAlpha[q];
                    }
                const float e = std::sqrt(dsq / 9.0f) / (1e-4f + oracleNorm(lum / 9.0f));
                const float eA = std::sqrt(dsqA / 9.0f) / (1e-4f + oracleNorm(lumA / 9.0f));
                m_noisy[p] = e >= limit || eA >= limit;
            }
    });
    // A pixel stops when its 3 x 3 neighbourhood is quiet.
    std::atomic<uint32_t> stopped{0};
    parallelRows(r.y0, r.y1, [&](int y0, int y1) {
        uint32_t local = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = r.x0; x < r.x1; ++x) {
                const size_t p = at(x, y);
                if (!m_evaluated[p])
                    continue;
                bool quiet = true;
                for (int dy = -1; dy <= 1 && quiet; ++dy)
                    for (int dx = -1; dx <= 1 && quiet; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx >= r.x0 && nx < r.x1 && ny >= r.y0 && ny < r.y1 && m_noisy[at(nx, ny)])
                            quiet = false;
                    }
                if (quiet) {
                    m_active[p] = 0;
                    ++local;
                }
            }
        stopped += local;
    });
    m_activeCount -= stopped.load();
    if (stopped.load() > 0)
        m_allActive = false;
}

void Renderer::selectPixels(int stride) {
    const uint32_t maxSamples = m_settings.maxSamples;
    const bool adaptive = m_settings.adaptive;
    m_tilePixels.resize(m_tiles.size());
    m_queues->arena.execute([&] {
        tbb::parallel_for(size_t(0), m_tiles.size(), [&](size_t t) {
            const TileRect &tile = m_tiles[t];
            std::vector<uint32_t> &list = m_tilePixels[t];
            list.clear();
            for (int y = tile.y0; y < tile.y1; ++y)
                for (int x = tile.x0; x < tile.x1; ++x) {
                    const uint32_t p = uint32_t(y) * uint32_t(m_film.width) + uint32_t(x);
                    if ((x - m_region.x0) % stride == 0 && (y - m_region.y0) % stride == 0 &&
                        m_film.count[p] < maxSamples && (!adaptive || m_active[p]))
                        list.push_back(p);
                }
        });
    });
}

void Renderer::syncFilm() {
#if NOVA_HAS_GPU
    if (!m_filmStale)
        return;
    m_filmStale = false;
    if (m_gpu && m_gpu->downloadFilm(m_film))
        return;
    // The device's sums are lost: the render starts over on the CPU.
    std::fprintf(stderr, "[Nova] XPU: GPU error (%s): restarting on the CPU\n",
                 m_gpu ? m_gpu->lastError().c_str() : "no device");
    m_gpu.reset();
    m_gpuFailed = true;
    m_gpuFilm = false;
    m_film.clear();
    resetPasses();
#endif
}

bool Renderer::gpuPass(bool lists, uint32_t spp, const LpeTables *lpe) {
#if NOVA_HAS_GPU
    if (m_gpuFailed || !m_scene)
        return false;

    if (!m_gpu) {
        m_gpu = std::make_unique<gpu::GpuRenderer>();
        std::string error;
        if (!m_gpu->init(error)) {
            std::fprintf(stderr, "[Nova] XPU: no usable GPU (%s): rendering on the CPU\n", error.c_str());
            m_gpu.reset();
            m_gpuFailed = true;
            return false;
        }
        std::fprintf(stderr, "[Nova] XPU: rendering on %s\n", m_gpu->deviceName().c_str());
    }
    // First-hit AOVs (depth, normal, albedo, ids, utility planes) come from
    // their own rays through the pixel centers, not from the samples: the
    // CPU traces them as it would (G4, deviation from D9: exact and cheap).
    // In proxy, capped and adaptive passes, those of the pass's pixels.
    if (m_firstHitPass) {
        std::atomic<uint64_t> firstRays{0};
        m_queues->arena.execute([&] {
            tbb::parallel_for(size_t(0), m_tiles.size(), [&](size_t t) {
                const ScopedFlushDenormals ftz;
                if (lists && m_tilePixels[t].empty())
                    return;
                firstRays.fetch_add(renderFirstHits(*m_scene, m_camera, m_settings, m_tiles[t], m_film,
                                                    m_queues->perThread.local(),
                                                    lists ? m_tilePixels[t].data() : nullptr,
                                                    lists ? uint32_t(m_tilePixels[t].size()) : 0u),
                                    std::memory_order_relaxed);
            });
        });
        m_rays.fetch_add(firstRays.load(), std::memory_order_relaxed);
    }
    if (lists) {
        m_gpuPixels.clear();
        for (const std::vector<uint32_t> &list : m_tilePixels)
            m_gpuPixels.insert(m_gpuPixels.end(), list.begin(), list.end());
    }
    uint64_t rays = 0;
    m_gpu->setTextureBudget(m_settings.gpuTextureBudget);
    bool sceneOk;
    try {
        sceneOk = m_gpu->setScene(*m_scene);
    } catch (const std::bad_alloc &) {
        sceneOk = false; // host memory for the device scene (e.g. texture staging): continue on the CPU
    }
    if (!sceneOk || (!m_gpuFilm && !m_gpu->setFilm(m_film)) ||
        !m_gpu->renderPass(m_camera, m_settings, m_filter, m_region, spp, m_film, rays, lpe,
                           lists ? &m_gpuPixels : nullptr, m_lpeGeneration)) {
        std::fprintf(stderr, "[Nova] XPU: GPU error (%s): continuing on the CPU\n", m_gpu->lastError().c_str());
        m_gpu.reset();
        m_gpuFailed = true;
        m_gpuFilm = false;
        if (m_filmStale) { // the device held sums the host lacks: start over
            m_filmStale = false;
            m_film.clear();
            resetPasses();
        }
        return false;
    }
    m_gpuFilm = true;
    m_filmStale = true;
    m_rays.fetch_add(rays, std::memory_order_relaxed);
    return true;
#else
    (void)lists;
    (void)spp;
    (void)lpe;
    if (!m_gpuFailed) {
        m_gpuFailed = true;
        std::fprintf(stderr, "[Nova] XPU: this build has no GPU backend: rendering on the CPU\n");
    }
    return false;
#endif
}

bool Renderer::bakePass(const std::function<bool()> &cancel) {
    std::atomic<bool> cancelled{false};
    if (!m_baker || m_bakeBuilt != m_settings.bake.get() || m_bakeVersion != m_scene->version()) {
        if (!m_baker)
            m_baker = std::make_unique<Baker>();
        m_baker->build(*m_scene, *m_settings.bake);
        m_bakeBuilt = m_settings.bake.get();
        m_bakeVersion = m_scene->version();
    }
    syncFilm();
    m_gpuFilm = false;
    m_lastPassGpu = false;
    m_firstHitPass = m_fullPasses == 0;
    // Lighting outputs path trace from the shading points (the wavefront
    // with the bake's camera hits); otherwise only the maps.
    const bool lighting = m_settings.bake->lighting;
    const LpeTables *lpe = lighting ? lpeTables() : nullptr;
    if (!m_tiles.empty()) {
        m_queues->arena.execute([&] {
            tbb::parallel_for(tbb::blocked_range<size_t>(0, m_tiles.size(), 1), [&](const tbb::blocked_range<size_t> &range) {
                const ScopedFlushDenormals ftz;
                WaveQueues &queues = m_queues->perThread.local();
                thread_local BakeScratch scratch;
                for (size_t i = range.begin(); i != range.end(); ++i) {
                    if (cancelled.load(std::memory_order_relaxed))
                        return;
                    if (cancel && cancel()) {
                        cancelled = true;
                        return;
                    }
                    if (lighting) {
                        m_rays.fetch_add(renderWave(*m_scene, m_camera, m_settings, m_filter, m_tiles[i], nullptr, 0,
                                                    m_film, queues, lpe, 1, m_baker.get()),
                                         std::memory_order_relaxed);
                        continue;
                    }
                    uint64_t rays = m_baker->generate(*m_scene, m_camera, m_filter, m_settings.seed, m_tiles[i], nullptr,
                                                      0, 1, m_film, scratch);
                    rays += m_baker->accumulate(*m_scene, m_settings, m_camera, m_settings.seed, m_film, scratch, true);
                    m_rays.fetch_add(rays, std::memory_order_relaxed);
                }
            });
        });
    }
    if (cancelled)
        return false;
    ++m_film.samples;
    ++m_pass;
    ++m_fullPasses;
    ++m_level;
    m_fillStride = 1;
    return true;
}

bool Renderer::renderPass(const std::function<bool()> &cancel) {
    if (m_settings.bake && m_scene)
        return bakePass(cancel);
    std::atomic<bool> cancelled{false};
    // A proxy pass renders one pixel per stride x stride block; after them,
    // pixels with extra proxy samples reach the maximum first and drop out.
    const int stride = int(m_pass) < m_proxyDepth ? 1 << (m_proxyDepth - int(m_pass)) : 1;
    const uint32_t maxSamples = m_settings.maxSamples;
    const bool capped = m_level + (m_pass - m_fullPasses) >= maxSamples;
    const bool adaptive = m_settings.adaptive;
    if (!m_oracleReady)
        initOracle();
    // Samples per pixel: one; in final renders with adaptive sampling, when
    // few pixels are left, up to the next checkpoint (waves stay full).
    uint32_t spp = 1;
    if (adaptive && stride == 1 && m_proxyDepth == 0 && m_activeCount > 0 && m_level < maxSamples) {
        const uint32_t regionPixels = uint32_t(m_region.pixelCount());
        uint32_t next = 1;
        while (next <= m_level)
            next <<= 1;
        spp = std::min({std::max(1u, regionPixels / m_activeCount), next - m_level, maxSamples - m_level});
    }
    const bool lists = stride > 1 || capped || (adaptive && !m_allActive);
    m_firstHitPass = stride > 1 || m_fullPasses == 0;
    if (lists && m_scene && !m_tiles.empty())
        selectPixels(stride);
    const LpeTables *lpe = lpeTables();
    const bool gpu = m_settings.device == RenderDevice::Xpu && gpuPass(lists, spp, lpe);
    m_lastPassGpu = gpu;

    if (!gpu && m_scene && !m_tiles.empty()) {
        syncFilm();
        m_gpuFilm = false; // the CPU adds samples the device film lacks
        // Work items: whole tiles; in proxy, capped and adaptive passes the
        // tiles' selected pixels, in chunks small enough to keep every thread
        // busy when few pixels are left. A pixel's samples always stay in one
        // wave (and a wave in one tile), so the result does not depend on
        // the chunking.
        m_items.clear();
        if (!lists) {
            for (uint32_t t = 0; t < uint32_t(m_tiles.size()); ++t)
                m_items.push_back({t, 0, 0});
        } else {
            size_t total = 0;
            for (const std::vector<uint32_t> &list : m_tilePixels)
                total += list.size();
            const size_t threads = size_t(std::max(1, m_queues->arena.max_concurrency()));
            const uint32_t chunk = uint32_t(std::clamp<size_t>(total / (threads * 4), 16, size_t(kTileSize) * kTileSize));
            for (uint32_t t = 0; t < uint32_t(m_tiles.size()); ++t)
                for (uint32_t b = 0; b < uint32_t(m_tilePixels[t].size()); b += chunk)
                    m_items.push_back({t, b, std::min(chunk, uint32_t(m_tilePixels[t].size()) - b)});
        }
        m_queues->arena.execute([&] {
            tbb::parallel_for(tbb::blocked_range<size_t>(0, m_items.size(), 1), [&](const tbb::blocked_range<size_t> &range) {
                const ScopedFlushDenormals ftz;
                WaveQueues &queues = m_queues->perThread.local();
                for (size_t i = range.begin(); i != range.end(); ++i) {
                    if (cancelled.load(std::memory_order_relaxed))
                        return;
                    if (cancel && cancel()) {
                        cancelled = true;
                        return;
                    }
                    const WorkItem &item = m_items[i];
                    const uint32_t *pixels = item.count ? m_tilePixels[item.tile].data() + item.begin : nullptr;
                    m_rays.fetch_add(renderWave(*m_scene, m_camera, m_settings, m_filter, m_tiles[item.tile], pixels,
                                                item.count, m_film, queues, lpe, spp),
                                     std::memory_order_relaxed);
                }
            });
        });
    }

    if (cancelled)
        return false;
    ++m_film.samples;
    ++m_pass;
    if (stride == 1) {
        ++m_fullPasses;
        m_level += spp;
        if (adaptive)
            updateOracle();
    }
    m_fillStride = stride;
    return true;
}

void Renderer::parallelRows(int y0, int y1, const std::function<void(int, int)> &body) const {
    if (y1 <= y0)
        return;
    m_queues->arena.execute([&] {
        tbb::parallel_for(tbb::blocked_range<int>(y0, y1, 8),
                          [&](const tbb::blocked_range<int> &rows) { body(rows.begin(), rows.end()); });
    });
}

} // namespace nova
