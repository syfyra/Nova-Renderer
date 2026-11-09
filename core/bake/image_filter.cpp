#include "core/bake/image_filter.h"

#include <algorithm>
#include <cmath>

namespace nova {

void applyShift(float *pixels, size_t count, int channels, const float *alpha, const ShiftFilter &f) {
    const int n = std::min(channels, 3);
    for (size_t i = 0; i < count; ++i) {
        float *p = pixels + i * size_t(channels);
        float in[3] = {0.0f, 0.0f, 0.0f};
        for (int c = 0; c < n; ++c)
            in[c] = p[c];
        const float a = f.premultiplied && alpha ? alpha[i] : 1.0f;
        for (int c = 0; c < n; ++c) {
            const int s = f.swizzle[c] < n ? f.swizzle[c] : c;
            p[c] = in[s] * f.scale[c] + f.offset[c] * a;
        }
    }
}

void enlargeIslands(float *pixels, int width, int height, int channels, const float *alpha, int border) {
    if (width <= 0 || height <= 0 || channels <= 0)
        return;
    const size_t count = size_t(width) * size_t(height);
    std::vector<uint8_t> filled(count, 0);
    bool any = false;
    for (size_t i = 0; i < count; ++i) {
        if (alpha[i] > 0.0f) {
            const float inv = 1.0f / alpha[i];
            for (int c = 0; c < channels; ++c)
                pixels[i * size_t(channels) + size_t(c)] *= inv;
            filled[i] = 1;
            any = true;
        }
    }
    if (!any)
        return;
    // Rings: each empty texel next to filled ones takes their mean.
    std::vector<float> next;
    std::vector<uint8_t> nextFilled;
    std::vector<size_t> ring;
    for (int r = 0; r < border; ++r) {
        ring.clear();
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const size_t i = size_t(y) * size_t(width) + size_t(x);
                if (filled[i])
                    continue;
                bool near = false;
                for (int dy = -1; dy <= 1 && !near; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if ((dx || dy) && xx >= 0 && yy >= 0 && xx < width && yy < height &&
                            filled[size_t(yy) * size_t(width) + size_t(xx)]) {
                            near = true;
                            break;
                        }
                    }
                if (near)
                    ring.push_back(i);
            }
        if (ring.empty())
            break;
        std::vector<float> values(ring.size() * size_t(channels), 0.0f);
        for (size_t k = 0; k < ring.size(); ++k) {
            const int x = int(ring[k] % size_t(width)), y = int(ring[k] / size_t(width));
            int n = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    if (!(dx || dy) || xx < 0 || yy < 0 || xx >= width || yy >= height)
                        continue;
                    const size_t j = size_t(yy) * size_t(width) + size_t(xx);
                    if (!filled[j])
                        continue;
                    for (int c = 0; c < channels; ++c)
                        values[k * size_t(channels) + size_t(c)] += pixels[j * size_t(channels) + size_t(c)];
                    ++n;
                }
            for (int c = 0; c < channels; ++c)
                values[k * size_t(channels) + size_t(c)] /= float(n);
        }
        for (size_t k = 0; k < ring.size(); ++k) {
            for (int c = 0; c < channels; ++c)
                pixels[ring[k] * size_t(channels) + size_t(c)] = values[k * size_t(channels) + size_t(c)];
            filled[ring[k]] = 1;
        }
    }
    // Pull-push for the rest: a pyramid of weighted 2x2 means, then every
    // empty texel takes its coarser level's value (nearest).
    struct Level {
        int w, h;
        std::vector<float> v; // premultiplied by w
        std::vector<float> wt;
    };
    std::vector<Level> levels;
    {
        Level l0{width, height, std::vector<float>(count * size_t(channels)), std::vector<float>(count)};
        for (size_t i = 0; i < count; ++i) {
            l0.wt[i] = filled[i] ? 1.0f : 0.0f;
            for (int c = 0; c < channels; ++c)
                l0.v[i * size_t(channels) + size_t(c)] = filled[i] ? pixels[i * size_t(channels) + size_t(c)] : 0.0f;
        }
        levels.push_back(std::move(l0));
    }
    while (levels.back().w > 1 || levels.back().h > 1) {
        const Level &f = levels.back();
        Level c{(f.w + 1) / 2, (f.h + 1) / 2, {}, {}};
        c.v.assign(size_t(c.w) * size_t(c.h) * size_t(channels), 0.0f);
        c.wt.assign(size_t(c.w) * size_t(c.h), 0.0f);
        for (int y = 0; y < f.h; ++y)
            for (int x = 0; x < f.w; ++x) {
                const size_t i = size_t(y) * size_t(f.w) + size_t(x), o = size_t(y / 2) * size_t(c.w) + size_t(x / 2);
                const float w = f.wt[i];
                if (w <= 0.0f)
                    continue;
                c.wt[o] += w;
                for (int k = 0; k < channels; ++k)
                    c.v[o * size_t(channels) + size_t(k)] += f.v[i * size_t(channels) + size_t(k)] * w;
            }
        for (size_t o = 0; o < c.wt.size(); ++o)
            if (c.wt[o] > 0.0f) {
                for (int k = 0; k < channels; ++k)
                    c.v[o * size_t(channels) + size_t(k)] /= c.wt[o];
                c.wt[o] = 1.0f;
            }
        levels.push_back(std::move(c));
    }
    for (size_t l = levels.size() - 1; l-- > 0;) {
        Level &f = levels[l];
        const Level &c = levels[l + 1];
        for (int y = 0; y < f.h; ++y)
            for (int x = 0; x < f.w; ++x) {
                const size_t i = size_t(y) * size_t(f.w) + size_t(x);
                if (f.wt[i] > 0.0f)
                    continue;
                const size_t o = size_t(y / 2) * size_t(c.w) + size_t(x / 2);
                for (int k = 0; k < channels; ++k)
                    f.v[i * size_t(channels) + size_t(k)] = c.v[o * size_t(channels) + size_t(k)];
                f.wt[i] = c.wt[o];
            }
    }
    for (size_t i = 0; i < count; ++i)
        if (!filled[i])
            for (int c = 0; c < channels; ++c)
                pixels[i * size_t(channels) + size_t(c)] = levels[0].v[i * size_t(channels) + size_t(c)];
}

} // namespace nova
