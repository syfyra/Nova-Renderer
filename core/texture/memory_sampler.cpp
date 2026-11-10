#include "core/texture/memory_sampler.h"

#include "core/texture/texel_stream.h"

#include <algorithm>
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

TextureId MemorySampler::add(int width, int height, int channels, std::vector<float> texels,
                             std::vector<std::vector<float>> mips) {
    m_textures.push_back({width, height, channels, std::move(texels), std::move(mips)});
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
    tex.mips.clear();
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
            const int ix = toInt(std::floor(x)), iy = toInt(std::floor(y));
            for (int c = 0; c < channels; ++c)
                out[size_t(c) * q.count + l] = texel(ix, iy, c);
            continue;
        }
        const float fx = x - 0.5f, fy = y - 0.5f;
        const int x0 = toInt(std::floor(fx)), y0 = toInt(std::floor(fy));
        const float ax = fx - float(x0), ay = fy - float(y0);
        for (int c = 0; c < channels; ++c) {
            const float top = texel(x0, y0, c) * (1.0f - ax) + texel(x0 + 1, y0, c) * ax;
            const float bottom = texel(x0, y0 + 1, c) * (1.0f - ax) + texel(x0 + 1, y0 + 1, c) * ax;
            out[size_t(c) * q.count + l] = top * (1.0f - ay) + bottom * ay;
        }
    }
}

uint32_t MemorySampler::stored(const Texture &tex) {
    uint32_t n = 1;
    for (const std::vector<float> &mip : tex.mips) {
        if (mip.size() != size_t(levelSize(tex.width, n)) * size_t(levelSize(tex.height, n)) * size_t(tex.channels))
            break;
        ++n;
    }
    return n;
}

bool MemorySampler::textureInfo(TextureId id, TexInfo &out) const {
    if (id >= m_textures.size())
        return false;
    const Texture &tex = m_textures[id];
    out = TexInfo{};
    out.half = false; // test data: exact
    TexTileInfo tile;
    tile.width = tex.width;
    tile.height = tex.height;
    tile.stored = stored(tex);
    out.tiles.push_back(tile);
    return tex.width > 0 && tex.height > 0 && tex.channels > 0;
}

bool MemorySampler::textureLevels(TextureId id, const TexLevelRequest &request, TexRowSink &sink) const {
    if (id >= m_textures.size() || request.udim != 0 || request.count == 0)
        return false;
    const Texture &tex = m_textures[id];
    const uint32_t last = stored(tex) - 1;
    std::vector<float> row;
    for (uint32_t level = request.first; level < request.first + request.count; ++level) {
        LevelWriter out(sink, level, levelSize(tex.width, level), levelSize(tex.height, level), request.half);
        // A level past the stored ones is made from the last stored one.
        const uint32_t from = std::min(level, last);
        const std::vector<float> &src = from == 0 ? tex.texels : tex.mips[from - 1];
        const int width = levelSize(tex.width, from), height = levelSize(tex.height, from);
        LevelReducer reduce(out, width, height);
        row.resize(size_t(width) * 4);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const float *p = &src[(size_t(y) * size_t(width) + size_t(x)) * size_t(tex.channels)];
                float *q = &row[size_t(x) * 4];
                for (int c = 0; c < 4; ++c)
                    q[c] = tex.channels >= 3 ? (c < tex.channels ? p[c] : (c == 3 ? 1.0f : 0.0f))
                                             : (c < 3 ? p[0] : (tex.channels == 2 ? p[1] : 1.0f));
            }
            m_texelsRead += uint64_t(width);
            if (!(from == level ? out.row(row.data()) : reduce.row(row.data())))
                return false;
        }
        if (!out.finish())
            return false;
    }
    return true;
}

} // namespace nova
