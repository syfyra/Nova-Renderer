#pragma once

// In-memory reference sampler for tests (docs/research/m3/textures.md 8):
// finest level only, closest or bilinear (cubic falls back to bilinear),
// the address modes of TexWrap with OIIO's semantics, a grey texture read as
// RGB replicates its value and a missing alpha is 1.

#include "core/texture/texture.h"

#include <vector>

namespace nova {

class MemorySampler final : public ITextureSampler {
public:
    // texels: rows from the top of the image (v = 1) down, `channels` floats
    // per texel.
    TextureId add(int width, int height, int channels, std::vector<float> texels);
    // New data under an id add() returned (as Hydra prepares a changed file
    // again): its generation changes. Not while rendering.
    void replace(TextureId id, int width, int height, int channels, std::vector<float> texels);

    void sample2D(const TexSampler &sampler, const TexQuery &query, float *out, uint8_t *status) const override;
    bool textureData(TextureId id, TexData &out) const override;
    uint32_t textureGeneration(TextureId id) const override;

private:
    struct Texture {
        int width = 0, height = 0, channels = 0;
        std::vector<float> texels;
        uint32_t generation = 0; // replacements
    };
    std::vector<Texture> m_textures;
};

} // namespace nova
