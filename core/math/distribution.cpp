#include "core/math/distribution.h"

#include <cmath>

namespace nova {

namespace {

// cdf (count + 1 entries) of `func`; returns the integral over [0, 1).
float buildCdf(const float *func, uint32_t count, float *cdf) {
    double sum = 0.0;
    cdf[0] = 0.0f;
    std::vector<double> running(count + 1, 0.0);
    for (uint32_t i = 0; i < count; ++i) {
        sum += double(func[i]) / double(count);
        running[i + 1] = sum;
    }
    for (uint32_t i = 1; i <= count; ++i)
        cdf[i] = sum > 0.0 ? float(running[i] / sum) : float(double(i) / double(count));
    cdf[count] = 1.0f;
    return float(sum);
}

float sanitize(float v) { return std::isfinite(v) && v > 0.0f ? v : 0.0f; }

} // namespace

void Distribution1D::build(const float *values, uint32_t count) {
    m_func.resize(count);
    for (uint32_t i = 0; i < count; ++i)
        m_func[i] = sanitize(values[i]);
    m_cdf.resize(size_t(count) + 1);
    m_integral = count > 0 ? buildCdf(m_func.data(), count, m_cdf.data()) : 0.0f;
}

void Distribution2D::build(const float *values, uint32_t nu, uint32_t nv) {
    m_nu = nu;
    m_nv = nv;
    m_func.resize(size_t(nu) * nv);
    for (size_t i = 0; i < m_func.size(); ++i)
        m_func[i] = sanitize(values[i]);
    m_conditionalCdf.resize(size_t(nu + 1) * nv);
    m_rowIntegral.resize(nv);
    for (uint32_t v = 0; v < nv; ++v)
        m_rowIntegral[v] = buildCdf(m_func.data() + size_t(v) * nu, nu, m_conditionalCdf.data() + size_t(v) * (nu + 1));
    m_marginalCdf.resize(size_t(nv) + 1);
    m_integral = nv > 0 && nu > 0 ? buildCdf(m_rowIntegral.data(), nv, m_marginalCdf.data()) : 0.0f;
}

} // namespace nova
