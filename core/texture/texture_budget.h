#pragma once

// The device texture budget (docs/design/M10.md D10, nova:gpuTextureBudget):
// which MIP levels of each texture tile a device keeps, planned over all the
// textures a scene samples from their shapes (ITextureSampler::textureInfo)
// before any texel is read, so that only the kept levels are read
// (textureLevels).
//
// A tile keeping level `first` on holds its stored levels first to
// stored - 1, or, past them, the single level `first` made from the last
// stored one. When everything fits, every tile keeps all its stored levels.
// Otherwise the textures are capped: the largest power-of-two size such that
// they fit with every tile's first level at most that size along both axes
// (its finer levels dropped; smaller textures keep everything). Then, in the
// order the textures are given (first use), a texture the cap reduced gets
// one level more where that still fits, all its tiles at once (a UDIM set
// stays uniform). If even 1 x 1 levels do not fit, every tile keeps only its
// coarsest level.

#include "core/texture/texture.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nova {

// What one tile keeps.
struct TexTilePlan {
    uint32_t first = 0; // its finest level
    uint32_t count = 0; // levels from `first` on
    size_t bytes = 0;   // on the device
    bool operator==(const TexTilePlan &o) const { return first == o.first && count == o.count && bytes == o.bytes; }
};

// Bytes per texel on the device: RGBA half or float.
inline size_t texelBytes(const TexInfo &info) { return info.half ? 8 : 16; }

// The levels a tile keeps from level `first` on.
TexTilePlan tilePlan(const TexTileInfo &tile, uint32_t first, size_t texel);

struct TexBudgetPlan {
    std::vector<std::vector<TexTilePlan>> textures; // per texture given, per tile of its TexInfo
    size_t bytes = 0;     // what the plan keeps
    size_t fullBytes = 0; // every stored level of every tile
    bool reduced = false; // some tile drops levels
    int cap = -1;         // log2 of the size cap (-1: none)
};

// `textures` in order of first use; a null entry (a texture without data)
// gets no tiles.
TexBudgetPlan planTextureBudget(const std::vector<const TexInfo *> &textures, size_t budget);

} // namespace nova
