#pragma once

// The parts every ITextureSampler::textureLevels shares (core/texture/
// texture.h, docs/design/M10.md D10): a level's RGBA float rows converted to
// the device's format and handed to the sink in bands, and a level made from
// a finer one by a box filter while the finer rows go by. Host memory: one
// band, and one row of sums.

#include "core/texture/texture.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace nova {

// IEEE half of f, rounded to nearest even.
uint16_t floatToHalf(float f);

// Texels along one axis of level `level` when level 0 has `size`.
inline int levelSize(int size, uint32_t level) { return level >= 31 ? 1 : std::max(1, size >> level); }

// Takes one level's rows, from the top down, and hands them to the sink in
// bands of kBandRows rows.
class LevelWriter {
public:
    static constexpr int kBandRows = 64;
    LevelWriter(TexRowSink &sink, uint32_t level, int width, int height, bool half);
    int width() const { return m_width; }
    int height() const { return m_height; }
    // The next row: width() RGBA floats. False once the sink stopped or
    // after the last row.
    bool row(const float *rgba);
    // Hands the last band over: true when every row came and the sink took
    // them all.
    bool finish();

private:
    bool flush();
    TexRowSink &m_sink;
    uint32_t m_level;
    int m_width, m_height;
    bool m_half;
    std::vector<uint16_t> m_halves; // the band (half)
    std::vector<float> m_floats;    // the band (float)
    int m_y = 0;    // the band's first row
    int m_rows = 0; // rows in the band
    bool m_ok = true;
};

// Makes a writer's level from a finer level (sourceWidth x sourceHeight, at
// least the writer's size) as the finer rows come, from the top down. Texel
// i of n along an axis is the mean of the finer texels j of m with
// floor(j * n / m) = i (a box filter; 2 x 2 texels per texel between two
// levels of a chain).
class LevelReducer {
public:
    LevelReducer(LevelWriter &out, int sourceWidth, int sourceHeight);
    // The next finer row: sourceWidth RGBA floats. False when the writer
    // failed or the size is not that of a finer level.
    bool row(const float *rgba);

private:
    bool emit();
    LevelWriter &m_out;
    int m_sourceWidth, m_sourceHeight;
    bool m_ok;
    std::vector<int> m_column;      // per finer column: its texel
    std::vector<int> m_columnCount; // per texel: its finer columns
    std::vector<double> m_sum;      // the current row's sums (RGBA)
    std::vector<float> m_row;
    int m_sourceY = 0;   // the next finer row
    int m_target = 0;    // the row being summed
    int m_summed = 0;    // finer rows in it
};

} // namespace nova
