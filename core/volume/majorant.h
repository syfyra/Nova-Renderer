#pragma once

// Majorant grids (docs/design/M8.md D7; research A 4.4, E1, E2): a dense
// grid of cells over a reference grid's index space, k voxels per cell. Cell
// c covers the index-space points [lo + c k, lo + (c+1) k); a trilinear
// lookup there reads voxels up to one past that range, so a cell's range of a
// field is taken over every stored value whose support reaches the cell, and
// the background. NanoVDB's own node statistics are not majorants for
// trilinear lookups (research A E2: 0.37 % of samples above their leaf max).

#include "core/math/vec.h"
#include "core/scene/motion.h"

#include <nanovdb/NanoVDB.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nova {

struct MajorantLayout {
    static constexpr int32_t kBaseCellSize = 8;           // voxels per cell edge (NanoVDB's leaf size)
    static constexpr size_t kMaxCells = 128 * 128 * 128; // the cell size doubles beyond this

    int32_t lo[3] = {0, 0, 0};
    int32_t res[3] = {0, 0, 0};
    int32_t k = kBaseCellSize;

    NOVA_INLINE bool empty() const { return res[0] <= 0 || res[1] <= 0 || res[2] <= 0; }
    NOVA_INLINE size_t cellCount() const { return empty() ? 0 : size_t(res[0]) * size_t(res[1]) * size_t(res[2]); }
    NOVA_INLINE size_t index(int32_t x, int32_t y, int32_t z) const {
        return (size_t(z) * size_t(res[1]) + size_t(y)) * size_t(res[0]) + size_t(x);
    }
    // The cell holding index-space point p; false outside the grid.
    NOVA_INLINE bool cellOf(Vec3f p, int32_t c[3]) const {
        const float q[3] = {p.x, p.y, p.z};
        for (int a = 0; a < 3; ++a) {
            const float g = (q[a] - float(lo[a])) / float(k);
            if (!(g >= 0.0f) || !(g < float(res[a])))
                return false;
            c[a] = int32_t(g);
        }
        return true;
    }
    // The index-space box the cells cover.
    Vec3f lower() const { return {float(lo[0]), float(lo[1]), float(lo[2])}; }
    Vec3f upper() const {
        return {float(lo[0] + res[0] * k), float(lo[1] + res[1] * k), float(lo[2] + res[2] * k)};
    }
};

// The cells a ray crosses, front to back (Amanatides & Woo; pbrt-v4's
// DDAMajorantIterator has the same structure). The ray is in the layout's
// index space with a direction that is not normalized, so t stays the
// world-space distance under any affine map.
struct MajorantDda {
    const MajorantLayout *layout = nullptr;
    float tCur = 0.0f, tEnd = 0.0f;
    int32_t cell[3] = {0, 0, 0}, step[3] = {0, 0, 0}, stop[3] = {0, 0, 0};
    float tNext[3] = {0.0f, 0.0f, 0.0f}, dt[3] = {0.0f, 0.0f, 0.0f};

    // False if [t0, t1) misses the cells.
    NOVA_INLINE bool init(const MajorantLayout &l, Vec3f o, Vec3f d, float t0, float t1) {
        layout = &l;
        const float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
        for (int a = 0; a < 3; ++a) {
            const float lo = float(l.lo[a]), hi = float(l.lo[a] + l.res[a] * l.k);
            if (dd[a] == 0.0f) {
                if (oo[a] < lo || oo[a] >= hi)
                    return false;
                continue;
            }
            float ta = (lo - oo[a]) / dd[a], tb = (hi - oo[a]) / dd[a];
            if (ta > tb) {
                const float s = ta;
                ta = tb;
                tb = s;
            }
            t0 = ta > t0 ? ta : t0;
            t1 = tb < t1 ? tb : t1;
        }
        if (!(t0 < t1))
            return false;
        tCur = t0;
        tEnd = t1;
        for (int a = 0; a < 3; ++a) {
            const float g = (oo[a] + dd[a] * t0 - float(l.lo[a])) / float(l.k);
            int32_t c = int32_t(g >= 0.0f ? g : 0.0f);
            c = c < l.res[a] - 1 ? c : l.res[a] - 1;
            cell[a] = c;
            if (dd[a] == 0.0f) {
                tNext[a] = kInf;
                dt[a] = kInf;
                step[a] = 0;
                stop[a] = -2;
            } else if (dd[a] > 0.0f) {
                tNext[a] = t0 + (float(c + 1) - g) * float(l.k) / dd[a];
                dt[a] = float(l.k) / dd[a];
                step[a] = 1;
                stop[a] = l.res[a];
            } else {
                tNext[a] = t0 + (float(c) - g) * float(l.k) / dd[a];
                dt[a] = -float(l.k) / dd[a];
                step[a] = -1;
                stop[a] = -1;
            }
        }
        return true;
    }
    // The next cell: its index and [t0, t1); false at the end.
    NOVA_INLINE bool next(size_t &index, float &t0, float &t1) {
        if (!(tCur < tEnd))
            return false;
        const int a = tNext[0] < tNext[1] ? (tNext[0] < tNext[2] ? 0 : 2) : (tNext[1] < tNext[2] ? 1 : 2);
        t0 = tCur;
        t1 = tNext[a] < tEnd ? tNext[a] : tEnd;
        index = layout->index(cell[0], cell[1], cell[2]);
        tCur = t1;
        if (tNext[a] >= tEnd) {
            tCur = tEnd;
        } else {
            cell[a] += step[a];
            if (cell[a] == stop[a])
                tCur = tEnd;
            tNext[a] += dt[a];
        }
        return true;
    }
};

// Cells covering the index-space box [lower, upper]: k = 8, doubled until
// there are at most kMaxCells. An empty box gives an empty layout.
MajorantLayout majorantLayout(Vec3f lower, Vec3f upper);

// The box of index-space points where a field's trilinear value can differ
// from its background (its stored values dilated by one voxel), mapped by
// `fieldToRef` into a reference index space. False if it stores nothing but
// the background.
bool fieldSupport(const nanovdb::FloatGrid &field, const Affine3f &fieldToRef, Vec3f &lower, Vec3f &upper);

// Per cell the range [lo, hi] of a field's trilinear values (the background
// included in every cell). `fieldToRef` maps the field's index space into
// the layout's.
struct CellRanges {
    std::vector<float> lo, hi;
};
void fieldCellRanges(const nanovdb::FloatGrid &field, const Affine3f &fieldToRef, const MajorantLayout &layout,
                     CellRanges &out);

// Widens every cell's range to those of the cells up to `radius` cells
// away along each axis (velocity blur: lookups moved by up to that much).
void dilateCellRanges(const MajorantLayout &layout, int32_t radius, CellRanges &ranges);

} // namespace nova
