#include "core/volume/shell.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace nova {

namespace {

constexpr uint32_t kLeafSize = 4;

Vec3f vmin(Vec3f a, Vec3f b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
Vec3f vmax(Vec3f a, Vec3f b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

struct Builder {
    const std::vector<Vec3f> &corners; // three per input triangle
    std::vector<uint32_t> order;
    std::vector<Vec3f> centroid;
    std::vector<ShellNode> nodes;

    void bounds(uint32_t begin, uint32_t end, Vec3f &lo, Vec3f &hi, Vec3f &clo, Vec3f &chi) const {
        lo = clo = Vec3f{3.0e38f, 3.0e38f, 3.0e38f};
        hi = chi = Vec3f{-3.0e38f, -3.0e38f, -3.0e38f};
        for (uint32_t i = begin; i < end; ++i) {
            const uint32_t t = order[i];
            for (int k = 0; k < 3; ++k) {
                lo = vmin(lo, corners[size_t(t) * 3 + size_t(k)]);
                hi = vmax(hi, corners[size_t(t) * 3 + size_t(k)]);
            }
            clo = vmin(clo, centroid[t]);
            chi = vmax(chi, centroid[t]);
        }
    }

    // Builds the subtree of order[begin, end) into nodes[index]: a median
    // split along the centroids' longest axis (a depth of about log2(n / 4),
    // far below the traversal stack's 64 for any mesh that fits in memory).
    void build(uint32_t index, uint32_t begin, uint32_t end, int depth) {
        Vec3f lo, hi, clo, chi;
        bounds(begin, end, lo, hi, clo, chi);
        nodes[index].lower = lo;
        nodes[index].upper = hi;
        const Vec3f ext = chi - clo;
        const int a = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
        const float spread = a == 0 ? ext.x : (a == 1 ? ext.y : ext.z);
        if (end - begin <= kLeafSize || !(spread > 0.0f) || depth >= kShellStack / 2 - 2) {
            nodes[index].first = begin;
            nodes[index].count = end - begin;
            return;
        }
        const uint32_t mid = begin + (end - begin) / 2;
        std::nth_element(order.begin() + begin, order.begin() + mid, order.begin() + end, [&](uint32_t x, uint32_t y) {
            const Vec3f &p = centroid[x], &q = centroid[y];
            return a == 0 ? p.x < q.x : (a == 1 ? p.y < q.y : p.z < q.z);
        });
        const uint32_t left = uint32_t(nodes.size());
        nodes.emplace_back();
        nodes.emplace_back();
        nodes[index].first = left;
        nodes[index].count = 0;
        build(left, begin, mid, depth + 1);
        build(left + 1, mid, end, depth + 1);
    }
};

} // namespace

MediumShellPtr makeTriangleShell(const Vec3f *points, size_t pointCount, const uint32_t *indices,
                                 size_t triangleCount) {
    auto shell = std::make_shared<MediumShell>();
    shell->kind = ShellKind::Triangles;
    std::vector<Vec3f> corners;
    corners.reserve(triangleCount * 3);
    // Points at the same position are one for closedness (meshes split at
    // seams or poles are still closed: crossings depend on positions only).
    std::vector<uint32_t> weld(pointCount);
    {
        struct Key {
            float x, y, z;
            bool operator==(const Key &o) const { return x == o.x && y == o.y && z == o.z; }
        };
        struct Hash {
            size_t operator()(const Key &k) const {
                const std::hash<float> h;
                return h(k.x) ^ (h(k.y) * 31u) ^ (h(k.z) * 131u);
            }
        };
        std::unordered_map<Key, uint32_t, Hash> first;
        const auto plain = [](float x) { return x == 0.0f ? 0.0f : x; }; // -0 and 0 hash alike
        for (size_t p = 0; p < pointCount; ++p)
            weld[p] = first.emplace(Key{plain(points[p].x), plain(points[p].y), plain(points[p].z)}, uint32_t(p))
                          .first->second;
    }
    std::unordered_map<uint64_t, uint32_t> edges; // undirected edge -> uses
    for (size_t t = 0; t < triangleCount; ++t) {
        uint32_t i[3] = {indices[t * 3], indices[t * 3 + 1], indices[t * 3 + 2]};
        if (i[0] >= pointCount || i[1] >= pointCount || i[2] >= pointCount)
            continue;
        for (uint32_t &k : i)
            k = weld[k];
        const Vec3f a = points[i[0]], b = points[i[1]], c = points[i[2]];
        const Vec3f n = cross(b - a, c - a);
        if (!(dot(n, n) > 0.0f) || !std::isfinite(dot(n, n)))
            continue;
        corners.push_back(a);
        corners.push_back(b);
        corners.push_back(c);
        for (int k = 0; k < 3; ++k) {
            const uint32_t p = i[k], q = i[(k + 1) % 3];
            const uint64_t key = (uint64_t(std::min(p, q)) << 32) | uint64_t(std::max(p, q));
            ++edges[key];
        }
    }
    for (const auto &[key, uses] : edges)
        shell->openEdges += uses != 2 ? 1u : 0u;
    const uint32_t count = uint32_t(corners.size() / 3);
    if (count == 0)
        return nullptr;
    Builder b{corners, {}, {}, {}};
    b.order.resize(count);
    b.centroid.resize(count);
    for (uint32_t t = 0; t < count; ++t) {
        b.order[t] = t;
        b.centroid[t] = (corners[size_t(t) * 3] + corners[size_t(t) * 3 + 1] + corners[size_t(t) * 3 + 2]) * (1.0f / 3.0f);
    }
    b.nodes.reserve(size_t(count) / 2 + 2);
    b.nodes.emplace_back();
    b.build(0, 0, count, 0);
    shell->triangles.resize(size_t(count) * 3);
    for (uint32_t i = 0; i < count; ++i)
        for (int k = 0; k < 3; ++k)
            shell->triangles[size_t(i) * 3 + size_t(k)] = corners[size_t(b.order[i]) * 3 + size_t(k)];
    shell->nodes = std::move(b.nodes);
    shell->lower = shell->nodes[0].lower;
    shell->upper = shell->nodes[0].upper;
    return shell;
}

MediumShellPtr makeSphereShell(Vec3f center, float radius) {
    if (!(radius > 0.0f) || !std::isfinite(radius))
        return nullptr;
    auto shell = std::make_shared<MediumShell>();
    shell->kind = ShellKind::Sphere;
    shell->center = center;
    shell->radius = radius;
    shell->lower = center - Vec3f{radius, radius, radius};
    shell->upper = center + Vec3f{radius, radius, radius};
    return shell;
}

} // namespace nova
