#include "core/film/crypto.h"

#include <cstring>

namespace nova {

namespace {

uint32_t rotl(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }

} // namespace

uint32_t murmurHash3(const void *data, size_t size, uint32_t seed) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    const size_t blocks = size / 4;
    const uint32_t c1 = 0xcc9e2d51u, c2 = 0x1b873593u;
    uint32_t h = seed;
    for (size_t i = 0; i < blocks; ++i) {
        uint32_t k = uint32_t(bytes[4 * i]) | uint32_t(bytes[4 * i + 1]) << 8 | uint32_t(bytes[4 * i + 2]) << 16 |
                     uint32_t(bytes[4 * i + 3]) << 24;
        k *= c1;
        k = rotl(k, 15);
        k *= c2;
        h ^= k;
        h = rotl(h, 13);
        h = h * 5u + 0xe6546b64u;
    }
    const uint8_t *tail = bytes + blocks * 4;
    uint32_t k = 0;
    switch (size & 3u) {
    case 3:
        k ^= uint32_t(tail[2]) << 16;
        [[fallthrough]];
    case 2:
        k ^= uint32_t(tail[1]) << 8;
        [[fallthrough]];
    case 1:
        k ^= uint32_t(tail[0]);
        k *= c1;
        k = rotl(k, 15);
        k *= c2;
        h ^= k;
        break;
    default:
        break;
    }
    h ^= uint32_t(size);
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

uint32_t cryptoHash(const std::string &name) {
    uint32_t h = murmurHash3(name.data(), name.size());
    const uint32_t exponent = (h >> 23) & 0xffu;
    if (exponent == 0u || exponent == 255u) // no denormals, infinities or NaNs as floats
        h ^= 1u << 23;
    return h;
}

float cryptoFloat(uint32_t id) {
    float f;
    std::memcpy(&f, &id, sizeof(f));
    return f;
}

Vec3f cryptoColor(uint32_t id) {
    constexpr float kInv = 1.0f / 4294967296.0f;
    return {float(id) * kInv, float(uint32_t(id << 8)) * kInv, float(uint32_t(id << 16)) * kInv};
}

} // namespace nova
