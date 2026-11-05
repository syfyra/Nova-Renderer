#include "core/lights/light_sampler.h"

#include <cmath>

namespace nova {

void LightSampler::build(const std::vector<float> &weights) {
    const size_t n = weights.size();
    m_pmf.assign(n, 0.0f);
    m_threshold.assign(n, 1.0f);
    m_alias.resize(n);
    m_count = 0;

    auto weight = [&](size_t i) { return std::isfinite(weights[i]) && weights[i] > 0.0f ? double(weights[i]) : 0.0; };
    double total = 0.0;
    for (size_t i = 0; i < n; ++i)
        total += weight(i);
    if (!(total > 0.0) || !std::isfinite(total))
        return;
    m_count = uint32_t(n);

    // Vose's alias method in double precision.
    std::vector<double> scaled(n);
    std::vector<uint32_t> small, large;
    for (size_t i = 0; i < n; ++i) {
        const double p = weight(i) / total;
        m_pmf[i] = float(p);
        scaled[i] = p * double(n);
        (scaled[i] < 1.0 ? small : large).push_back(uint32_t(i));
        m_alias[i] = uint32_t(i);
    }
    while (!small.empty() && !large.empty()) {
        const uint32_t s = small.back(), l = large.back();
        small.pop_back();
        m_threshold[s] = float(scaled[s]);
        m_alias[s] = l;
        scaled[l] -= 1.0 - scaled[s];
        if (scaled[l] < 1.0) {
            large.pop_back();
            small.push_back(l);
        }
    }
    // Columns left over are full up to rounding.
    for (uint32_t i : small)
        m_threshold[i] = 1.0f;
    for (uint32_t i : large)
        m_threshold[i] = 1.0f;
}

} // namespace nova
