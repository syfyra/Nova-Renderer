#include "core/texture/texel_stream.h"

#include <cstring>

namespace nova {

uint16_t floatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u, biased = (x >> 23) & 0xFFu;
    uint32_t mant = x & 0x7FFFFFu;
    if (biased == 0xFFu)
        return uint16_t(sign | 0x7C00u | (mant ? 0x200u : 0u));
    const int32_t exp = int32_t(biased) - 127 + 15;
    if (exp >= 31)
        return uint16_t(sign | 0x7C00u);
    if (exp <= 0) { // subnormal or zero
        if (exp < -10)
            return uint16_t(sign);
        mant |= 0x800000u;
        const uint32_t shift = uint32_t(14 - exp), half = 1u << (shift - 1);
        uint32_t h = mant >> shift;
        const uint32_t rest = mant & ((1u << shift) - 1u);
        if (rest > half || (rest == half && (h & 1u)))
            ++h;
        return uint16_t(sign | h);
    }
    uint32_t h = (uint32_t(exp) << 10) | (mant >> 13);
    const uint32_t rest = mant & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (h & 1u)))
        ++h; // a carry into the exponent rounds up correctly
    return uint16_t(sign | h);
}

LevelWriter::LevelWriter(TexRowSink &sink, uint32_t level, int width, int height, bool half)
    : m_sink(sink), m_level(level), m_width(width), m_height(height), m_half(half) {
    m_ok = width > 0 && height > 0;
    const size_t band = size_t(std::max(width, 0)) * size_t(std::min(height, kBandRows)) * 4;
    if (half)
        m_halves.resize(band);
    else
        m_floats.resize(band);
}

bool LevelWriter::row(const float *rgba) {
    if (!m_ok || m_y + m_rows >= m_height)
        return m_ok = false;
    const size_t n = size_t(m_width) * 4, at = size_t(m_rows) * n;
    if (m_half) {
        uint16_t *q = m_halves.data() + at;
        for (size_t i = 0; i < n; ++i)
            q[i] = floatToHalf(rgba[i]);
    } else {
        std::memcpy(m_floats.data() + at, rgba, n * sizeof(float));
    }
    if (++m_rows == kBandRows)
        return flush();
    return true;
}

bool LevelWriter::flush() {
    if (!m_ok || m_rows == 0)
        return m_ok;
    TexRows r;
    r.level = m_level;
    r.width = m_width;
    r.height = m_height;
    r.y = m_y;
    r.rows = m_rows;
    r.texels = m_half ? static_cast<const void *>(m_halves.data()) : static_cast<const void *>(m_floats.data());
    m_ok = m_sink.put(r);
    m_y += m_rows;
    m_rows = 0;
    return m_ok;
}

bool LevelWriter::finish() { return flush() && m_y == m_height; }

LevelReducer::LevelReducer(LevelWriter &out, int sourceWidth, int sourceHeight)
    : m_out(out), m_sourceWidth(sourceWidth), m_sourceHeight(sourceHeight) {
    const int w = out.width(), h = out.height();
    m_ok = w > 0 && h > 0 && sourceWidth >= w && sourceHeight >= h;
    if (!m_ok)
        return;
    m_column.resize(size_t(sourceWidth));
    m_columnCount.assign(size_t(w), 0);
    for (int x = 0; x < sourceWidth; ++x) {
        m_column[size_t(x)] = int(int64_t(x) * w / sourceWidth);
        ++m_columnCount[size_t(m_column[size_t(x)])];
    }
    m_sum.assign(size_t(w) * 4, 0.0);
    m_row.resize(size_t(w) * 4);
}

bool LevelReducer::row(const float *rgba) {
    if (!m_ok || m_sourceY >= m_sourceHeight)
        return m_ok = false;
    const int target = int(int64_t(m_sourceY) * m_out.height() / m_sourceHeight);
    if (target != m_target && !emit())
        return false;
    m_target = target;
    for (int x = 0; x < m_sourceWidth; ++x) {
        double *s = &m_sum[size_t(m_column[size_t(x)]) * 4];
        const float *p = rgba + size_t(x) * 4;
        s[0] += p[0];
        s[1] += p[1];
        s[2] += p[2];
        s[3] += p[3];
    }
    ++m_summed;
    if (++m_sourceY == m_sourceHeight)
        return emit();
    return true;
}

bool LevelReducer::emit() {
    for (size_t x = 0; x < m_columnCount.size(); ++x) {
        const double n = double(m_columnCount[x]) * double(m_summed);
        for (size_t c = 0; c < 4; ++c)
            m_row[x * 4 + c] = float(m_sum[x * 4 + c] / n);
    }
    std::fill(m_sum.begin(), m_sum.end(), 0.0);
    m_summed = 0;
    return m_ok = m_out.row(m_row.data());
}

} // namespace nova
