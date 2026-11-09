#pragma once

// Karma's image filters for baked maps (docs/design/M11-P8.md; the Karma
// Texture Baker authors them in karma:global:imagefilter), on float images
// of `channels` floats per pixel, row-major. Karma's maps are coverage
// averages: a texel's samples that miss the bake mesh add zero, so values
// are premultiplied by the coverage `alpha` (the Af map).

#include <cstdint>
#include <string>
#include <vector>

namespace nova {

// "shift": value' = swizzle(value) * scale + offset (times alpha when
// premultiplied, measured: a texel of coverage a gets a * offset). The
// swizzle names the source component of each output ("xzy").
struct ShiftFilter {
    float scale[3] = {1.0f, 1.0f, 1.0f};
    float offset[3] = {0.0f, 0.0f, 0.0f};
    int swizzle[3] = {0, 1, 2};
    bool premultiplied = false;
    bool runLast = false;
    std::vector<std::string> aovs;
};
void applyShift(float *pixels, size_t count, int channels, const float *alpha, const ShiftFilter &filter);

// "uv_enlarger" with the "diffusefill" scheme: texels with coverage are
// divided by it (measured exactly), the empty ones filled from the islands:
// `border` rings by the mean of their filled 8-neighbours, then the rest by
// a pull-push fill. Karma's fill of the empty texels follows the same idea
// but not the same weights (they differ by a few percent next to an island;
// the covered texels are exact).
void enlargeIslands(float *pixels, int width, int height, int channels, const float *alpha, int border);

} // namespace nova
