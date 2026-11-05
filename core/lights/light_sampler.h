#pragma once

// Light selection proportional to estimated light power, via Walker's alias
// method: O(1) sampling with flat arrays that map directly to device memory.
// M9 replaces this with a light BVH for scenes with many lights.

#include "core/math/common.h"

#include <cstdint>
#include <vector>

namespace nova {

// Kernel view of the alias table (non-owning).
struct LightSamplerView {
    const float *pmf = nullptr;       // selection probability per light (lookups for MIS)
    const float *threshold = nullptr; // keep-probability of each alias column
    const uint32_t *alias = nullptr;
    uint32_t count = 0;               // columns; 0 if no light can be sampled

    // Picks a light index for u in [0, 1); returns kInvalidId if there is none.
    NOVA_INLINE uint32_t sample(float u, float &pmfOut) const {
        if (count == 0) {
            pmfOut = 0.0f;
            return kInvalidId;
        }
        const float scaled = u * float(count);
        uint32_t column = uint32_t(scaled);
        column = column < count ? column : count - 1;
        const float remainder = scaled - float(column);
        const uint32_t index = remainder < threshold[column] ? column : alias[column];
        pmfOut = pmf[index];
        return index;
    }
};

class LightSampler {
public:
    // Builds the table from one weight per light. Lights with zero weight are
    // never selected (pmf 0); if all weights are zero nothing is sampled.
    void build(const std::vector<float> &weights);

    LightSamplerView view() const { return {m_pmf.data(), m_threshold.data(), m_alias.data(), m_count}; }

private:
    std::vector<float> m_pmf;
    std::vector<float> m_threshold;
    std::vector<uint32_t> m_alias;
    uint32_t m_count = 0;
};

} // namespace nova
