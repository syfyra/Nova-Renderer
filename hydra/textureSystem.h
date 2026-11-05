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

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaTextureSystem final : public nova::ITextureSampler {
public:
    HdNovaTextureSystem();
    ~HdNovaTextureSystem() override;
    HdNovaTextureSystem(HdNovaTextureSystem const &) = delete;
    HdNovaTextureSystem &operator=(HdNovaTextureSystem const &) = delete;

    // Not while rendering (prims stop the render thread before editing).
    void SetOptions(HdNovaTextureOptions const &options);
    HdNovaTextureOptions GetOptions() const;

    // Thread-safe (materials sync in parallel); not while rendering. An
    // empty path gives kNoTexture. The same request returns the same id
    // until its source changes on disk.
    nova::TextureId Acquire(HdNovaTextureRequest const &request);

    void sample2D(nova::TexSampler const &sampler, nova::TexQuery const &query, float *out,
                  uint8_t *status) const override;
    // The prepared file's MIP levels (every UDIM tile's) for the GPU. A set
    // that does not fit in host memory drops its finest levels.
    bool textureData(nova::TextureId id, nova::TexData &out) const override;
    // Changes each time the texture is prepared again under its id (its
    // source changed on disk), so a device copy of its data is refreshed.
    uint32_t textureGeneration(nova::TextureId id) const override;

    // Textures acquired so far whose file was missing.
    uint32_t MissingCount() const { return _missing.load(); }
    // OIIO's statistics (level 1..5) on stderr.
    void PrintStats(int level) const;

private:
    struct Entry {
        HdNovaTextureRequest request;
        HdNovaPreparedTexture prepared;
        std::string signature; // the source files' sizes and times when prepared
        OIIO::TextureSystem::TextureHandle *handle = nullptr;
        uint32_t generation = 0; // times prepared
        bool good = false;
        bool countedMissing = false; // counted in _missing
        mutable std::atomic<bool> reported{false};
    };
    std::string Key(HdNovaTextureRequest const &request) const;

    OIIO::ImageCache *_cache = nullptr;
    OIIO::TextureSystem *_ts = nullptr;
    mutable std::mutex _mutex;
    std::deque<Entry> _entries; // by TextureId; stable addresses
    std::unordered_map<std::string, nova::TextureId> _byKey;
    HdNovaTextureOptions _options;
    std::atomic<uint32_t> _missing{0};
};

PXR_NAMESPACE_CLOSE_SCOPE
