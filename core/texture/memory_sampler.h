#pragma once

// In-memory reference sampler for tests (docs/research/m3/textures.md 8):
// finest level only, closest or bilinear (cubic falls back to bilinear),
// the address modes of TexWrap with OIIO's semantics, a grey texture read as
// RGB replicates its value and a missing alpha is 1. For devices
// (textureInfo, textureLevels) a texture may also store coarser MIP levels.

#include "core/texture/texture.h"

#include <cstdint>
#include <vector>

namespace nova {

class MemorySampler final : public ITextureSampler {
public:
    // texels: rows from the top of the image (v = 1) down, `channels` floats
    // per texel. mips: stored levels 1, 2, ... (level l has max(1, width >>
    // l) x max(1, height >> l) texels), which only textureLevels reads.
    TextureId add(int width, int height, int channels, std::vector<float> texels,
                  std::vector<std::vector<float>> mips = {});
    // New data under an id add() returned (as Hydra prepares a changed file
    // again): its generation changes. Not while rendering.
    void replace(TextureId id, int width, int height, int channels, std::vector<float> texels);

    void sample2D(const TexSampler &sampler, const TexQuery &query, float *out, uint8_t *status) const override;
    bool textureInfo(TextureId id, TexInfo &out) const override;
    bool textureLevels(TextureId id, const TexLevelRequest &request, TexRowSink &sink) const override;
    uint32_t textureGeneration(TextureId id) const override;

    // Texels textureLevels read from stored levels so far.
    uint64_t texelsRead() const { return m_texelsRead; }

private:
    struct Texture {
        int width = 0, height = 0, channels = 0;
        std::vector<float> texels;
        std::vector<std::vector<float>> mips;
        uint32_t generation = 0; // replacements
    };
    // Stored levels of `tex` (level 0 and the mips of the right sizes).
    static uint32_t stored(const Texture &tex);
    std::vector<Texture> m_textures;
    mutable uint64_t m_texelsRead = 0;
};

} // namespace nova
