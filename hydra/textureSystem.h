#pragma once

// Surface textures (docs/design/M3.md K9, docs/research/m3/textures.md 6):
// Nova's private OIIO TextureSystem on the process-wide shared ImageCache
// (husk's --oiio-* limits apply; flip_t = 1 gives MaterialX/USD v-up incl.
// UDIM rows, gray_to_rgb = 1), the registry of the textures materials use,
// and the core's ITextureSampler.
//
// A texture is a (file, color space hint, layer); Acquire prepares the file
// (textureConvert.h) and returns a stable id. UDIM follows Karma: tile =
// 1001 + floor(u) + 10 floor(v) for 0 <= u < 10, v >= 0, else the fallback;
// a missing tile gives the fallback silently. A missing file gives one
// warning and the fallback. Read errors during rendering are reported once
// per texture.

#include "textureConvert.h"

#include "core/texture/texture.h"

#include <OpenImageIO/texture.h>

#include <pxr/pxr.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaTextureSystem final : public nova::ITextureSampler {
public:
    HdNovaTextureSystem();
    ~HdNovaTextureSystem() override;
    HdNovaTextureSystem(HdNovaTextureSystem const &) = delete;
    HdNovaTextureSystem &operator=(HdNovaTextureSystem const &) = delete;

    // Not while rendering (prims stop the render thread before editing).
    void SetOptions(HdNovaTextureOptions const &options);
    // The texture cache's memory limit in MB (OpenImageIO's max_memory_MB).
    float CacheMemoryMB() const;
    HdNovaTextureOptions GetOptions() const;

    // Thread-safe (materials sync in parallel); not while rendering. An
    // empty path gives kNoTexture. The same request returns the same id
    // until its source changes on disk.
    nova::TextureId Acquire(HdNovaTextureRequest const &request);

    void sample2D(nova::TexSampler const &sampler, nova::TexQuery const &query, float *out,
                  uint8_t *status) const override;
    // The prepared file's shape (every UDIM tile's stored MIP levels) for
    // the GPU, from the files' headers.
    bool textureInfo(nova::TextureId id, nova::TexInfo &out) const override;
    // Reads only the requested levels of one tile, a band of rows at a time
    // (ImageInput tiles or scanlines); a level the file does not store is
    // made from its last stored level while that is read.
    bool textureLevels(nova::TextureId id, nova::TexLevelRequest const &request,
                       nova::TexRowSink &sink) const override;
    // Changes each time the texture is prepared again under its id (its
    // source changed on disk), so a device copy of its data is refreshed.
    uint32_t textureGeneration(nova::TextureId id) const override;

    // Textures acquired so far whose file was missing.
    uint32_t MissingCount() const { return _missing.load(); }
    // Texels textureLevels read from files so far.
    uint64_t DeviceTexelsRead() const { return _deviceTexelsRead.load(); }
    // OIIO's statistics (level 1..5) on stderr.
    void PrintStats(int level) const;

private:
    struct Entry {
        HdNovaTextureRequest request;
        HdNovaPreparedTexture prepared;
        std::string signature; // the source files' sizes and times when prepared
        OIIO::TextureSystem::TextureHandle *handle = nullptr;
        uint32_t generation = 0; // times prepared
        int channels = 0;        // the file's channels (0: not known, a UDIM set)
        bool good = false;
        bool countedMissing = false; // counted in _missing
        mutable std::atomic<bool> reported{false};
    };
    std::string Key(HdNovaTextureRequest const &request) const;
    // The files of a texture for the GPU: (UDIM number or 0, path).
    std::vector<std::pair<uint32_t, std::string>> DeviceFiles(Entry const &e) const;

    OIIO::ImageCache *_cache = nullptr;
    float _cacheMemoryMB = 0.0f; // the cache's limit as found (restored when nova:textureCacheMemory returns to 0)
    OIIO::TextureSystem *_ts = nullptr;
    mutable std::mutex _mutex;
    std::deque<Entry> _entries; // by TextureId; stable addresses
    std::unordered_map<std::string, nova::TextureId> _byKey;
    HdNovaTextureOptions _options;
    std::atomic<uint32_t> _missing{0};
    mutable std::atomic<uint64_t> _deviceTexelsRead{0};
};

PXR_NAMESPACE_CLOSE_SCOPE
