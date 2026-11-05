#include "core/volume/majorant.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nova {

namespace {

// Stored values of a grid as regions of voxels [a, b] (inclusive) whose
// values lie in [lo, hi]: leaf voxels one by one (`perVoxel`) or a leaf at a
// time, and the constant tiles of the internal and root nodes (root tiles
// clamped to the grid's index box). Values equal to the background are
// skipped: every cell includes the background anyway.
struct Region {
    nanovdb::Coord a, b;
    float lo, hi;
};

template <typename F>
void forEachStoredRegion(const nanovdb::FloatGrid &grid, bool perVoxel, F &&f) {
    const auto &tree = grid.tree();
    const float background = tree.background();
    const auto *leaf = tree.getFirstLeaf();
    for (uint32_t i = 0; i < tree.nodeCount(0); ++i, ++leaf) {
        if (perVoxel) {
            for (uint32_t n = 0; n < 512; ++n) {
                const float v = leaf->getValue(n);
                if (v != background) {
                    const nanovdb::Coord c = leaf->offsetToGlobalCoord(n);
                    f(Region{c, c, v, v});
                }
            }
            continue;
        }
        float lo = std::numeric_limits<float>::infinity(), hi = -lo;
        for (uint32_t n = 0; n < 512; ++n) {
            const float v = leaf->getValue(n);
            if (v != background) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
        if (lo <= hi)
            f(Region{leaf->origin(), leaf->origin() + nanovdb::Coord(7), lo, hi});
    }
    const auto *lower = tree.getFirstLower();
    for (uint32_t i = 0; i < tree.nodeCount(1); ++i, ++lower)
        for (uint32_t n = 0; n < 4096; ++n)
            if (!lower->isChild(n)) {
                const float v = lower->data()->getValue(n);
                if (v != background) {
                    const nanovdb::Coord c = lower->offsetToGlobalCoord(n);
                    f(Region{c, c + nanovdb::Coord(7), v, v});
                }
            }
    const auto *upper = tree.getFirstUpper();
    for (uint32_t i = 0; i < tree.nodeCount(2); ++i, ++upper)
        for (uint32_t n = 0; n < 32768; ++n)
            if (!upper->isChild(n)) {
                const float v = upper->data()->getValue(n);
                if (v != background) {
                    const nanovdb::Coord c = upper->offsetToGlobalCoord(n);
                    f(Region{c, c + nanovdb::Coord(127), v, v});
                }
            }
    const auto &root = tree.root();
    const nanovdb::CoordBBox box = grid.indexBBox();
    for (uint32_t n = 0; n < root.tileCount(); ++n) {
        const auto *tile = root.data()->tile(n);
        if (tile->isChild() || tile->value == background)
            continue;
        nanovdb::Coord ca = tile->origin(), cb = ca + nanovdb::Coord(4095);
        ca.maxComponent(box.min());
        cb.minComponent(box.max());
        if (ca[0] <= cb[0] && ca[1] <= cb[1] && ca[2] <= cb[2])
            f(Region{ca, cb, tile->value, tile->value});
    }
}

bool isIdentity(const Affine3f &m) {
    const float e = 1e-5f;
    const auto near = [e](Vec3f a, Vec3f b) {
        return std::fabs(a.x - b.x) <= e && std::fabs(a.y - b.y) <= e && std::fabs(a.z - b.z) <= e;
    };
    return near(m.x, {1.0f, 0.0f, 0.0f}) && near(m.y, {0.0f, 1.0f, 0.0f}) && near(m.z, {0.0f, 0.0f, 1.0f}) &&
           near(m.t, {0.0f, 0.0f, 0.0f});
}

// The image of the index box [a, b] under an affine map (its 8 corners).
void mapBox(const Affine3f &m, Vec3f a, Vec3f b, Vec3f &lower, Vec3f &upper) {
    const float inf = std::numeric_limits<float>::infinity();
    lower = {inf, inf, inf};
    upper = {-inf, -inf, -inf};
    for (int c = 0; c < 8; ++c) {
        const Vec3f p = m.point({(c & 1) ? b.x : a.x, (c & 2) ? b.y : a.y, (c & 4) ? b.z : a.z});
        lower = {std::min(lower.x, p.x), std::min(lower.y, p.y), std::min(lower.z, p.z)};
        upper = {std::max(upper.x, p.x), std::max(upper.y, p.y), std::max(upper.z, p.z)};
    }
}

} // namespace

MajorantLayout majorantLayout(Vec3f lower, Vec3f upper) {
    MajorantLayout l;
    if (!(lower.x <= upper.x && lower.y <= upper.y && lower.z <= upper.z) || !std::isfinite(lower.x) ||
        !std::isfinite(upper.x) || !std::isfinite(lower.y) || !std::isfinite(upper.y) || !std::isfinite(lower.z) ||
        !std::isfinite(upper.z))
        return l;
    const float lo[3] = {std::floor(lower.x), std::floor(lower.y), std::floor(lower.z)};
    const float hi[3] = {std::ceil(upper.x), std::ceil(upper.y), std::ceil(upper.z)};
    for (int32_t k = MajorantLayout::kBaseCellSize;; k *= 2) {
        size_t cells = 1;
        for (int a = 0; a < 3; ++a) {
            l.lo[a] = int32_t(lo[a]);
            const double extent = std::max(1.0, double(hi[a]) - double(lo[a]));
            l.res[a] = int32_t(std::ceil(extent / double(k)));
            cells *= size_t(l.res[a]);
        }
        l.k = k;
        if (cells <= MajorantLayout::kMaxCells || k >= (1 << 20))
            return l;
    }
}

bool fieldSupport(const nanovdb::FloatGrid &field, const Affine3f &fieldToRef, Vec3f &lower, Vec3f &upper) {
    nanovdb::Coord a(std::numeric_limits<int32_t>::max()), b(std::numeric_limits<int32_t>::min());
    bool any = false;
    forEachStoredRegion(field, true, [&](const Region &r) {
        a.minComponent(r.a);
        b.maxComponent(r.b);
        any = true;
    });
    if (!any)
        return false;
    // A voxel's trilinear support is the open box of one voxel around it.
    mapBox(fieldToRef, {float(a[0] - 1), float(a[1] - 1), float(a[2] - 1)},
           {float(b[0] + 1), float(b[1] + 1), float(b[2] + 1)}, lower, upper);
    return true;
}

void fieldCellRanges(const nanovdb::FloatGrid &field, const Affine3f &fieldToRef, const MajorantLayout &layout,
                     CellRanges &out) {
    const float background = field.tree().background();
    out.lo.assign(layout.cellCount(), background);
    out.hi.assign(layout.cellCount(), background);
    if (layout.empty())
        return;
    const int32_t k = layout.k;
    const auto widen = [&](int32_t c0[3], int32_t c1[3], float lo, float hi) {
        for (int32_t z = c0[2]; z <= c1[2]; ++z)
            for (int32_t y = c0[1]; y <= c1[1]; ++y)
                for (int32_t x = c0[0]; x <= c1[0]; ++x) {
                    const size_t i = layout.index(x, y, z);
                    out.lo[i] = std::min(out.lo[i], lo);
                    out.hi[i] = std::max(out.hi[i], hi);
                }
    };
    // Cells [c0, c1] per axis, clipped to the grid; false if none.
    const auto clip = [&](int32_t c0[3], int32_t c1[3]) {
        for (int a = 0; a < 3; ++a) {
            if (c1[a] < 0 || c0[a] >= layout.res[a])
                return false;
            c0[a] = std::max(c0[a], 0);
            c1[a] = std::min(c1[a], layout.res[a] - 1);
        }
        return true;
    };
    if (isIdentity(fieldToRef)) {
        // The field's own index space: voxel v (relative to lo) reaches the
        // points (v - 1, v + 1), i.e. the cells floor((v - 1) / k) to
        // floor(v / k).
        const auto floorDiv = [](int32_t n, int32_t d) { return n >= 0 ? n / d : -((-n + d - 1) / d); };
        forEachStoredRegion(field, true, [&](const Region &r) {
            int32_t c0[3], c1[3];
            for (int a = 0; a < 3; ++a) {
                c0[a] = floorDiv(r.a[a] - layout.lo[a] - 1, k);
                c1[a] = floorDiv(r.b[a] - layout.lo[a], k);
            }
            if (clip(c0, c1))
                widen(c0, c1, r.lo, r.hi);
        });
        return;
    }
    // Another index space: a leaf at a time, its support mapped as a box.
    forEachStoredRegion(field, false, [&](const Region &r) {
        Vec3f lower, upper;
        mapBox(fieldToRef, {float(r.a[0] - 1), float(r.a[1] - 1), float(r.a[2] - 1)},
               {float(r.b[0] + 1), float(r.b[1] + 1), float(r.b[2] + 1)}, lower, upper);
        const float l[3] = {lower.x, lower.y, lower.z}, u[3] = {upper.x, upper.y, upper.z};
        int32_t c0[3], c1[3];
        for (int a = 0; a < 3; ++a) {
            const float g0 = std::floor((l[a] - float(layout.lo[a])) / float(k));
            const float g1 = std::floor((u[a] - float(layout.lo[a])) / float(k));
            c0[a] = int32_t(std::clamp(g0, -1.0f, float(layout.res[a])));
            c1[a] = int32_t(std::clamp(g1, -1.0f, float(layout.res[a])));
        }
        if (clip(c0, c1))
            widen(c0, c1, r.lo, r.hi);
    });
}

void dilateCellRanges(const MajorantLayout &layout, int32_t radius, CellRanges &ranges) {
    if (radius <= 0 || layout.empty())
        return;
    // Separable: the running min and max along each axis in turn.
    std::vector<float> lo(ranges.lo.size()), hi(ranges.hi.size());
    for (int axis = 0; axis < 3; ++axis) {
        const int32_t n = layout.res[axis];
        const size_t stride = axis == 0 ? 1 : axis == 1 ? size_t(layout.res[0]) : size_t(layout.res[0]) * size_t(layout.res[1]);
        for (size_t i = 0; i < ranges.lo.size(); ++i) {
            const int32_t c = int32_t((i / stride) % size_t(n));
            const int32_t a = std::max(c - radius, 0), b = std::min(c + radius, n - 1);
            float l = ranges.lo[i], h = ranges.hi[i];
            for (int32_t j = a; j <= b; ++j) {
                const size_t s = i + (size_t(j) - size_t(c)) * stride; // wraps for j < c, as intended
                l = std::min(l, ranges.lo[s]);
                h = std::max(h, ranges.hi[s]);
            }
            lo[i] = l;
            hi[i] = h;
        }
        ranges.lo.swap(lo);
        ranges.hi.swap(hi);
    }
}

} // namespace nova
