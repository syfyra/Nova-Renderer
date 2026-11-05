#pragma once

#include "core/math/vec.h"

namespace nova {

// PCG hash (Jarzynski & Olano 2020).
NOVA_INLINE uint32_t pcgHash(uint32_t v) {
    const uint32_t state = v * 747796405u + 2891336453u;
    const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Stateless-seeded sample stream: deterministic for (pixel, sample, seed),
// so progressive and IPR renders are reproducible regardless of threading.
// The whole state is one 32-bit word, which fits the SoA path state.
struct Rng {
    uint32_t state;

    NOVA_INLINE static Rng make(uint32_t pixel, uint32_t sample, uint32_t seed = 0) {
        return Rng{pcgHash(pixel ^ pcgHash(sample + 0x9E3779B9u * (seed + 1u)))};
    }
    NOVA_INLINE float next() {
        state = pcgHash(state);
        return float(state >> 8) * (1.0f / 16777216.0f); // [0, 1)
    }
    NOVA_INLINE Vec2f next2() {
        const float a = next();
        return {a, next()};
    }
};

} // namespace nova
