#pragma once

// Cryptomatte (docs/design/M6.md D10; docs/research/m6/cryptomatte.md). An
// id is MurmurHash3_x86_32 (seed 0) of the name's UTF-8 bytes with the
// Cryptomatte specification's exponent fixup (a hash whose float exponent
// bits are 0 or 255 gets bit 23 flipped), stored in images as the float
// with those bits. Coverage: per camera sample the first visible surface's id
// gets weight 1; a pixel keeps up to kCryptoSlots ids.

#include "core/math/vec.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nova {

// MurmurHash3_x86_32.
uint32_t murmurHash3(const void *data, size_t size, uint32_t seed = 0);
// The Cryptomatte id of a name (MurmurHash3 with the exponent fixup).
uint32_t cryptoHash(const std::string &name);
// The float an id is stored as (the same bits).
float cryptoFloat(uint32_t id);
// The preview colour of an id: (h, h << 8, h << 16) as fractions of 2^32.
Vec3f cryptoColor(uint32_t id);

// What a cryptomatte names: the object (Karma's __name), the bound material
// (__materialname) or a string/int primvar's value.
enum class CryptoSource : uint8_t { Object, Material, Primvar };

struct CryptoRequest {
    CryptoSource source = CryptoSource::Object;
    std::string primvar; // Primvar
    bool operator==(const CryptoRequest &) const = default;
};

constexpr int kCryptoSlots = 8;

// Per pixel kCryptoSlots (id, weight) pairs, the weight summed over the
// pixel's samples (0: a free slot). Ids beyond the slots are dropped.
struct CryptoPlane {
    CryptoRequest request;
    std::vector<uint32_t> ids;
    std::vector<float> weights;

    void resize(size_t pixels) {
        ids.assign(pixels * kCryptoSlots, 0u);
        weights.assign(pixels * kCryptoSlots, 0.0f);
    }
    void add(size_t pixel, uint32_t id, float weight) {
        uint32_t *i = ids.data() + pixel * kCryptoSlots;
        float *w = weights.data() + pixel * kCryptoSlots;
        for (int s = 0; s < kCryptoSlots; ++s) {
            if (w[s] == 0.0f) {
                i[s] = id;
                w[s] = weight;
                return;
            }
            if (i[s] == id) {
                w[s] += weight;
                return;
            }
        }
    }
};

// A string primvar's ids on a mesh: one (constant) or one per authored face.
struct CryptoAttribute {
    std::string name;
    bool uniform = false;
    std::vector<uint32_t> ids;
};

} // namespace nova
