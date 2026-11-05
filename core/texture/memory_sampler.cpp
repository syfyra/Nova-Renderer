#include "core/texture/memory_sampler.h"

#include <cmath>

namespace nova {

namespace {

// Texel index i on an axis of n texels; false for Black outside the image.
bool wrapIndex(TexWrap wrap, int n, int &i) {
    switch (wrap) {
    case TexWrap::Periodic:
        i = ((i % n) + n) % n;
        return true;
    case TexWrap::Mirror: {
        const int period = 2 * n;
        int m = ((i % period) + period) % period;
        i = m < n ? m : period - 1 - m;
        return true;
    }
    case TexWrap::Black:
        return i >= 0 && i < n;
    default: // Clamp, Constant (the range test happens per lane)
        i = i < 0 ? 0 : (i >= n ? n - 1 : i);
        return true;
    }
}

} // namespace

TextureId MemorySampler::add(int width, int height, int channels, std::vector<float> texels) {
    m_textures.push_back({width, height, channels, std::move(texels)});
    return TextureId(m_textures.size() - 1);
}

void MemorySampler::replace(TextureId id, int width, int height, int channels, std::vector<float> texels) {
    if (id >= m_textures.size())
        return;
    Texture &tex = m_textures[id];
    tex.width = width;
    tex.height = height;
    tex.channels = channels;
    tex.texels = std::move(texels);
    ++tex.generation;
}

uint32_t MemorySampler::textureGeneration(TextureId id) const {
    return id < m_textures.size() ? m_textures[id].generation : 0u;
}

void MemorySampler::sample2D(const TexSampler &sampler, const TexQuery &q, float *out, uint8_t *status) const {
    const TexSlot &slot = sampler.slot;
    const int channels = slot.channels;
    if (sampler.texture >= m_textures.size()) {
        for (uint32_t l = 0; l < q.count; ++l)
            status[l] = 1;
        return;
    }
    const Texture &tex = m_textures[sampler.texture];
    // One texel's channel c as the slot reads it.
    auto texel = [&](int x, int y, int c) -> float {
        if (!wrapIndex(slot.wrapS, tex.width, x) || !wrapIndex(slot.wrapT, tex.height, y))
            return 0.0f;
        const float *p = &tex.texels[(size_t(y) * size_t(tex.width) + size_t(x)) * size_t(tex.channels)];
        if (tex.channels >= 3 || channels < 3)
            return c < tex.channels ? p[c] : (c == 3 ? 1.0f : 0.0f);
        // grey (and alpha) read as RGB(A)
        if (c < 3)
            return p[0];
        return tex.channels == 2 ? p[1] : 1.0f;
    };
    for (uint32_t l = 0; l < q.count; ++l) {
        const float s = q.s[l], t = q.t[l];
        if ((slot.wrapS == TexWrap::Constant && !(s >= 0.0f && s <= 1.0f)) ||
            (slot.wrapT == TexWrap::Constant && !(t >= 0.0f && t <= 1.0f))) {
            status[l] = 1;
            continue;
        }
        status[l] = 0;
        // Texel space: x right, y down from the top row.
        const float x = s * float(tex.width), y = (1.0f - t) * float(tex.height);
        if (slot.filter == TexFilter::Closest) {
            const int ix = int(std::floor(x)), iy = int(std::floor(y));
            for (int c = 0; c < channels; ++c)
                out[size_t(c) * q.count + l] = texel(ix, iy, c);
            continue;
        }
        const float fx = x - 0.5f, fy = y - 0.5f;
        const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
        const float ax = fx - float(x0), ay = fy - float(y0);
        for (int c = 0; c < channels; ++c) {
            const float top = texel(x0, y0, c) * (1.0f - ax) + texel(x0 + 1, y0, c) * ax;
            const float bottom = texel(x0, y0 + 1, c) * (1.0f - ax) + texel(x0 + 1, y0 + 1, c) * ax;
            out[size_t(c) * q.count + l] = top * (1.0f - ay) + bottom * ay;
        }
    }
}

bool MemorySampler::textureData(TextureId id, TexData &out) const {
    if (id >= m_textures.size())
        return false;
    const Texture &tex = m_textures[id];
    out = TexData{};
    out.half = false; // test data: exact
    TexLevel level;
    level.width = tex.width;
    level.height = tex.height;
    level.texels.resize(size_t(tex.width) * size_t(tex.height) * 4);
    for (size_t i = 0; i < size_t(tex.width) * size_t(tex.height); ++i) {
        const float *p = &tex.texels[i * size_t(tex.channels)];
        float *q = &level.texels[i * 4];
        for (int c = 0; c < 4; ++c)
            q[c] = tex.channels >= 3 ? (c < tex.channels ? p[c] : (c == 3 ? 1.0f : 0.0f))
                                     : (c < 3 ? p[0] : (tex.channels == 2 ? p[1] : 1.0f));
    }
    out.tiles.push_back({0, {std::move(level)}});
    return true;
}

} // namespace nova
