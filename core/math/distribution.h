#pragma once

// Piecewise-constant distributions on [0, 1) and [0, 1)^2 for importance
// sampling tabulated functions such as environment maps (pbrt's
// PiecewiseConstant1D/2D). The host builds them; kernels use the flat views.

#include "core/math/vec.h"

#include <cstdint>
#include <vector>

namespace nova {

struct Distribution1DView {
    const float *func = nullptr; // count non-negative values
    const float *cdf = nullptr;  // count + 1 values from 0 to 1
    uint32_t count = 0;
    float integral = 0.0f;       // integral of func over [0, 1)

    // Continuous sample in [0, 1); `pdf` is the density at the sample.
    NOVA_INLINE float sample(float u, float &pdf, uint32_t &index) const {
        uint32_t lo = 0, hi = count; // cdf[lo] <= u < cdf[hi]
        while (hi - lo > 1) {
            const uint32_t mid = (lo + hi) / 2;
            if (cdf[mid] <= u)
                lo = mid;
            else
                hi = mid;
        }
        index = lo;
        const float width = cdf[lo + 1] - cdf[lo];
        float du = u - cdf[lo];
        if (width > 0.0f)
            du /= width;
        pdf = integral > 0.0f ? func[lo] / integral : 0.0f;
        const float x = (float(lo) + du) / float(count);
        return x < 1.0f ? x : 0x1.fffffep-1f;
    }

    NOVA_INLINE float pdf(float x) const {
        uint32_t i = uint32_t(x * float(count));
        i = i < count ? i : count - 1;
        return integral > 0.0f ? func[i] / integral : 0.0f;
    }
};

class Distribution1D {
public:
    Distribution1D() = default;
    explicit Distribution1D(const float *values, uint32_t count) { build(values, count); }
    // An all-zero function gives a uniform cdf and zero densities.
    void build(const float *values, uint32_t count);
    Distribution1DView view() const {
        return {m_func.data(), m_cdf.data(), uint32_t(m_func.size()), m_integral};
    }

private:
    std::vector<float> m_func;
    std::vector<float> m_cdf;
    float m_integral = 0.0f;
};

// 2D view over (u, v): a marginal distribution over the rows (v) and one
// conditional distribution per row (u).
struct Distribution2DView {
    const float *func = nullptr;        // nu * nv values, row-major (v rows)
    const float *conditionalCdf = nullptr; // nv rows of nu + 1 values
    const float *rowIntegral = nullptr; // nv conditional integrals
    const float *marginalCdf = nullptr; // nv + 1 values
    uint32_t nu = 0, nv = 0;
    float integral = 0.0f;              // integral of func over [0, 1)^2

    NOVA_INLINE Vec2f sample(Vec2f u, float &pdf) const {
        const Distribution1DView marginal{rowIntegral, marginalCdf, nv, integral};
        float pdfV = 0.0f, pdfU = 0.0f;
        uint32_t row = 0, column = 0;
        const float v = marginal.sample(u.y, pdfV, row);
        const Distribution1DView conditional{func + size_t(row) * nu, conditionalCdf + size_t(row) * (nu + 1),
                                             nu, rowIntegral[row]};
        const float x = conditional.sample(u.x, pdfU, column);
        pdf = pdfV * pdfU;
        return {x, v};
    }

    NOVA_INLINE float pdf(Vec2f p) const {
        uint32_t iu = uint32_t(p.x * float(nu)), iv = uint32_t(p.y * float(nv));
        iu = iu < nu ? iu : nu - 1;
        iv = iv < nv ? iv : nv - 1;
        return integral > 0.0f ? func[size_t(iv) * nu + iu] / integral : 0.0f;
    }
};

class Distribution2D {
public:
    // `values` holds nu * nv non-negative samples, row-major with v rows.
    void build(const float *values, uint32_t nu, uint32_t nv);
    Distribution2DView view() const {
        return {m_func.data(), m_conditionalCdf.data(), m_rowIntegral.data(), m_marginalCdf.data(), m_nu, m_nv, m_integral};
    }

private:
    std::vector<float> m_func;
    std::vector<float> m_conditionalCdf;
    std::vector<float> m_rowIntegral;
    std::vector<float> m_marginalCdf;
    uint32_t m_nu = 0, m_nv = 0;
    float m_integral = 0.0f;
};

} // namespace nova
