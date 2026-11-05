#include "core/integrator/volume_media.h"

#include <atomic>

namespace nova {

namespace {

std::atomic<uint64_t> g_violations{0};

// The scene's volumes as medium_track.h's Media: the records and this
// thread's evaluators (the last few looked up stay at hand).
class HostMedia {
public:
    HostMedia(const Scene &scene, VolumeEvalCache &cache, const ShadeGlobals &globals)
        : m_scene(scene), m_cache(cache), m_globals(globals) {}

    uint32_t activeCount() const { return uint32_t(m_scene.activeVolumes().size()); }
    VolumeId active(uint32_t i) const { return m_scene.activeVolumes()[i]; }
    MediumVolume volume(VolumeId id) const { return mediumVolume(m_scene.volumeRecord(id)); }
    Affine3f volumeToWorld(VolumeId id, float time) const { return m_scene.volumeToWorld(id, time); }
    VolumePoint evaluate(VolumeId id, Vec3f p, Vec3f w, Vec3f dir, float time, uint8_t rayType, uint32_t depth) {
        return evaluator(id)(p, w, dir, time, rayType, depth);
    }
    // Counts a tentative collision whose extinction exceeds the majorant.
    void violation(const Spectrum &e, float sigma) {
        if (e.x > sigma || e.y > sigma || e.z > sigma)
            g_violations.fetch_add(1, std::memory_order_relaxed);
    }

private:
    VolumeEvaluator &evaluator(VolumeId id) {
        for (int i = 0; i < m_known; ++i)
            if (m_ids[i] == id)
                return *m_evals[i];
        VolumeEvaluator &e = m_cache.get(m_scene, id, m_globals);
        if (m_known < kKnown) {
            m_ids[m_known] = id;
            m_evals[m_known++] = &e;
        }
        return e;
    }

    static constexpr int kKnown = 8;
    const Scene &m_scene;
    VolumeEvalCache &m_cache;
    const ShadeGlobals &m_globals;
    VolumeId m_ids[kKnown] = {};
    VolumeEvaluator *m_evals[kKnown] = {};
    int m_known = 0;
};

// This thread's lists for the trackers (one tracker per thread at a time).
std::vector<VolumeSpan> &spanList() {
    thread_local std::vector<VolumeSpan> list;
    return list;
}
std::vector<MediumActive> &activeList() {
    thread_local std::vector<MediumActive> list;
    return list;
}

// std::vector with erase by index, as medium_track.h's lists.
template <class T>
struct ListRef {
    std::vector<T> &v;
    void clear() { v.clear(); }
    size_t size() const { return v.size(); }
    bool empty() const { return v.empty(); }
    void push_back(const T &x) { v.push_back(x); }
    void erase(size_t i) { v.erase(v.begin() + std::ptrdiff_t(i)); }
    T &operator[](size_t i) { return v[i]; }
    const T &operator[](size_t i) const { return v[i]; }
    T &front() { return v.front(); }
    const T &front() const { return v.front(); }
};

} // namespace

uint64_t majorantViolations() { return g_violations.load(std::memory_order_relaxed); }

MediumVolume mediumVolume(const VolumeRecord &rec) {
    MediumVolume v;
    v.lower = rec.lower;
    v.upper = rec.upper;
    v.layout = &rec.layout;
    v.majorant = rec.majorant.data();
    v.extinctionMajorant = rec.extinctionMajorant.empty() ? nullptr : rec.extinctionMajorant.data();
    v.lightIndex = rec.lightIndex;
    v.lightCells = rec.lightCells.view();
    v.cellVolume = rec.cellVolume;
    if (rec.reference >= 0 && size_t(rec.reference) < rec.fieldObjectToIndex.size())
        v.objectToIndex = rec.fieldObjectToIndex[size_t(rec.reference)];
    return v;
}

VolumeEvaluator &VolumeEvalCache::get(const Scene &scene, VolumeId id, const ShadeGlobals &globals) {
    if (m_scene != &scene || m_version != scene.version() || m_frame != globals.frame || m_fps != globals.fps) {
        m_evaluators.clear();
        m_scene = &scene;
        m_version = scene.version();
        m_frame = globals.frame;
        m_fps = globals.fps;
    }
    if (m_evaluators.size() <= id)
        m_evaluators.resize(size_t(id) + 1);
    if (!m_evaluators[id])
        m_evaluators[id] = std::make_unique<VolumeEvaluator>(scene, id, globals);
    return *m_evaluators[id];
}

void volumeSpans(const Scene &scene, Vec3f o, Vec3f d, float tEnd, float time, std::vector<VolumeSpan> &spans) {
    VolumeEvalCache unused;
    const ShadeGlobals globals;
    const HostMedia media(scene, unused, globals);
    ListRef<VolumeSpan> list{spans};
    nova::volumeSpans(media, o, d, tEnd, time, list);
}

int mediumAt(const Scene &scene, VolumeEvalCache &cache, const ShadeGlobals &globals, const MediumRay &ray, float t,
             MediumPart *parts, int max, Spectrum &extinction) {
    HostMedia media(scene, cache, globals);
    return nova::mediumAt(media, ray, t, parts, max, extinction);
}

bool mediumExtent(const Scene &scene, const MediumRay &ray, float &a, float &b) {
    VolumeEvalCache unused;
    const ShadeGlobals globals;
    const HostMedia media(scene, unused, globals);
    ListRef<VolumeSpan> spans{spanList()};
    return nova::mediumExtent(media, spans, ray, a, b);
}

Spectrum trackMedia(const Scene &scene, VolumeEvalCache &cache, const ShadeGlobals &globals, const MediumRay &ray,
                    float u0, Rng &rng, EmissionGather *gather, const float *crossT, Spectrum *crossOut,
                    int crossCount) {
    HostMedia media(scene, cache, globals);
    ListRef<VolumeSpan> spans{spanList()};
    ListRef<MediumActive> active{activeList()};
    return nova::trackMedia(media, spans, active, ray, u0, rng, gather, crossT, crossOut, crossCount);
}

MediumFlight flyMedia(const Scene &scene, VolumeEvalCache &cache, const ShadeGlobals &globals, const MediumRay &ray,
                      Spectrum beta, float u0, Rng &rng, EmissionGather *gather, Spectrum *alphaT,
                      const float *crossT, Spectrum *crossOut, int crossCount) {
    thread_local std::vector<MediumHit> hits;
    HostMedia media(scene, cache, globals);
    ListRef<VolumeSpan> spans{spanList()};
    ListRef<MediumActive> active{activeList()};
    ListRef<MediumHit> parts{hits};
    return nova::flyMedia(media, spans, active, parts, ray, beta, u0, rng, gather, alphaT, crossT, crossOut,
                          crossCount);
}

} // namespace nova
