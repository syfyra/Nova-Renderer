#include "core/texture/texture_budget.h"

#include "core/texture/texel_stream.h"

#include <algorithm>

namespace nova {

namespace {

// The first level of a tile that is at most 2^cap texels along both axes.
uint32_t firstUnder(const TexTileInfo &tile, int cap) {
    const int limit = 1 << cap;
    uint32_t level = 0;
    while (levelSize(tile.width, level) > limit || levelSize(tile.height, level) > limit)
        ++level;
    return level;
}

// The cap under which a tile keeps level 0.
int fullCap(const TexTileInfo &tile) {
    int cap = 0;
    while (cap < 30 && ((1 << cap) < tile.width || (1 << cap) < tile.height))
        ++cap;
    return cap;
}

} // namespace

TexTilePlan tilePlan(const TexTileInfo &tile, uint32_t first, size_t texel) {
    TexTilePlan p;
    p.first = first;
    const uint32_t stored = std::max(1u, tile.stored);
    p.count = first < stored ? stored - first : 1u;
    for (uint32_t l = first; l < first + p.count; ++l)
        p.bytes += size_t(levelSize(tile.width, l)) * size_t(levelSize(tile.height, l)) * texel;
    return p;
}

TexBudgetPlan planTextureBudget(const std::vector<const TexInfo *> &textures, size_t budget) {
    TexBudgetPlan plan;
    plan.textures.resize(textures.size());
    // Every tile at the cap (-1: none) and the bytes that takes.
    const auto atCap = [&](int cap, std::vector<std::vector<TexTilePlan>> *out) {
        size_t total = 0;
        for (size_t i = 0; i < textures.size(); ++i) {
            if (!textures[i])
                continue;
            const size_t texel = texelBytes(*textures[i]);
            if (out)
                (*out)[i].clear();
            for (const TexTileInfo &tile : textures[i]->tiles) {
                const TexTilePlan p = tilePlan(tile, cap < 0 ? 0u : firstUnder(tile, cap), texel);
                total += p.bytes;
                if (out)
                    (*out)[i].push_back(p);
            }
        }
        return total;
    };
    plan.fullBytes = atCap(-1, &plan.textures);
    plan.bytes = plan.fullBytes;
    if (plan.fullBytes <= budget)
        return plan;

    // The largest cap that fits (0, 1 x 1 levels, if none does).
    int top = 0;
    for (const TexInfo *t : textures)
        if (t)
            for (const TexTileInfo &tile : t->tiles)
                top = std::max(top, fullCap(tile));
    int cap = top - 1;
    while (cap > 0 && atCap(cap, nullptr) > budget)
        --cap;
    cap = std::max(cap, 0);
    plan.cap = cap;
    plan.reduced = true;
    plan.bytes = atCap(cap, &plan.textures);

    // One level more for the textures the cap reduced, in order of first
    // use, while that fits.
    size_t left = plan.bytes < budget ? budget - plan.bytes : 0;
    for (size_t i = 0; i < textures.size(); ++i) {
        if (!textures[i])
            continue;
        const size_t texel = texelBytes(*textures[i]);
        std::vector<TexTilePlan> finer = plan.textures[i];
        size_t more = 0;
        for (size_t k = 0; k < finer.size(); ++k) {
            if (finer[k].first == 0)
                continue;
            const TexTilePlan p = tilePlan(textures[i]->tiles[k], finer[k].first - 1, texel);
            more += p.bytes - finer[k].bytes;
            finer[k] = p;
        }
        if (more == 0 || more > left)
            continue;
        left -= more;
        plan.bytes += more;
        plan.textures[i] = std::move(finer);
    }
    return plan;
}

} // namespace nova
