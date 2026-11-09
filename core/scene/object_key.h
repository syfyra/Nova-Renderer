#pragma once

// Stable object keys (docs/bug-audit-2026-11.md, D-1). Hydra creates Nova's
// meshes, lights and volumes in its sync order, which differs from run to
// run (rprims sync in parallel, sprims come out of hash maps), so their ids
// must not reach anything that changes pixels. A host gives every object a
// key that does not depend on that order (Hydra: its prim path, and the
// instance for instanced lights); per-object random decisions (stochastic
// opacity, the per-strand random number) hash the key, and the lists that
// random numbers index (lights, emissive triangles, volumes) are in key
// order. An object created without a key keys by its id, so scenes built
// through the core API keep their old images.

#include "core/math/common.h"

#include <cstdint>
#include <string_view>

namespace nova {

// splitmix64's finalizer.
NOVA_INLINE uint64_t mixKey(uint64_t h) {
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebull;
    h ^= h >> 31;
    return h;
}

// The key of a named object (FNV-1a over the name's bytes) and of its
// instance `index`; the same in every run, process and build.
inline uint64_t objectKey(std::string_view name, uint64_t index = 0) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char c : name) {
        h ^= uint64_t(uint8_t(c));
        h *= 0x100000001b3ull;
    }
    return mixKey(h ^ mixKey(index + 0x9E3779B97F4A7C15ull));
}

// The 32 bits per-object hashes take: a key below 2^32 (an id) is itself.
NOVA_INLINE uint32_t objectHash(uint64_t key) { return uint32_t(key) ^ uint32_t(key >> 32); }

} // namespace nova
