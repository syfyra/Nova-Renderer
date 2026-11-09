#include "core/bake/mikktspace.h"

#include "core/math/frame.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <numeric>
#include <unordered_map>

namespace nova {
namespace {

// A welded vertex: position, normal and texture coordinate compared exactly
// (-0 equals 0, as the reference code's float compares).
struct WeldKey {
    float f[8];
    bool operator==(const WeldKey &o) const { return std::memcmp(f, o.f, sizeof(f)) == 0; }
};
struct WeldHash {
    size_t operator()(const WeldKey &k) const {
        uint64_t h = 1469598103934665603ull;
        const unsigned char *b = reinterpret_cast<const unsigned char *>(k.f);
        for (size_t i = 0; i < sizeof(k.f); ++i)
            h = (h ^ b[i]) * 1099511628211ull;
        return size_t(h);
    }
};

struct Tri {
    uint32_t c[3];  // corners
    uint32_t v[3];  // welded vertices
    Vec3f os{};     // unit texture-space s direction times the orientation sign
    bool orient = true;
    bool valid = false; // has texture area and a direction
};

uint32_t find(std::vector<uint32_t> &parent, uint32_t i) {
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

Vec3f project(Vec3f v, Vec3f n) {
    const Vec3f p = v - n * dot(n, v);
    const float l = length(p);
    return l > FLT_MIN ? p * (1.0f / l) : p;
}

} // namespace

std::vector<MikkTangent> mikkTangents(const MikkMesh &mesh) {
    const size_t corners = mesh.P.size();
    std::vector<MikkTangent> out(corners);
    if (corners == 0 || mesh.N.size() != corners || mesh.uv.size() != corners)
        return out;

    // Weld the corners.
    std::vector<uint32_t> vertexOf(corners);
    {
        std::unordered_map<WeldKey, uint32_t, WeldHash> weld;
        weld.reserve(corners);
        for (size_t c = 0; c < corners; ++c) {
            WeldKey k;
            const float vals[8] = {mesh.P[c].x, mesh.P[c].y, mesh.P[c].z, mesh.N[c].x,
                                   mesh.N[c].y, mesh.N[c].z, mesh.uv[c].x, mesh.uv[c].y};
            for (int i = 0; i < 8; ++i)
                k.f[i] = vals[i] + 0.0f; // -0 -> +0
            vertexOf[c] = weld.emplace(k, uint32_t(weld.size())).first->second;
        }
    }

    // Triangles: quads split along their shorter texture-space diagonal.
    std::vector<Tri> tris;
    tris.reserve(mesh.faces.size() * 2);
    const auto addTri = [&](uint32_t a, uint32_t b, uint32_t c) {
        Tri t;
        t.c[0] = a;
        t.c[1] = b;
        t.c[2] = c;
        for (int k = 0; k < 3; ++k)
            t.v[k] = vertexOf[t.c[k]];
        tris.push_back(t);
    };
    for (const MikkFace &f : mesh.faces) {
        if (f.count == 3) {
            addTri(f.corner[0], f.corner[1], f.corner[2]);
        } else if (f.count == 4) {
            const uint32_t *q = f.corner;
            const auto d2 = [](Vec2f a, Vec2f b) { return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y); };
            const float t02 = d2(mesh.uv[q[2]], mesh.uv[q[0]]), t13 = d2(mesh.uv[q[3]], mesh.uv[q[1]]);
            bool diag02;
            if (t02 < t13)
                diag02 = true;
            else if (t13 < t02)
                diag02 = false;
            else {
                const Vec3f a = mesh.P[q[2]] - mesh.P[q[0]], b = mesh.P[q[3]] - mesh.P[q[1]];
                diag02 = !(dot(b, b) < dot(a, a));
            }
            if (diag02) {
                addTri(q[0], q[1], q[2]);
                addTri(q[0], q[2], q[3]);
            } else {
                addTri(q[0], q[1], q[3]);
                addTri(q[1], q[2], q[3]);
            }
        }
    }

    // Per triangle: the texture-space s derivative and the orientation.
    for (Tri &t : tris) {
        const Vec3f v1 = mesh.P[t.c[0]], v2 = mesh.P[t.c[1]], v3 = mesh.P[t.c[2]];
        const Vec2f t1 = mesh.uv[t.c[0]], t2 = mesh.uv[t.c[1]], t3 = mesh.uv[t.c[2]];
        const float t21x = t2.x - t1.x, t21y = t2.y - t1.y, t31x = t3.x - t1.x, t31y = t3.y - t1.y;
        const Vec3f d1 = v2 - v1, d2 = v3 - v1;
        const float area = t21x * t31y - t21y * t31x;
        const Vec3f os = d1 * t31y - d2 * t21y;
        t.orient = area > 0.0f;
        const float len = length(os);
        if (std::fabs(area) > FLT_MIN && len > FLT_MIN) {
            t.os = os * ((t.orient ? 1.0f : -1.0f) / len);
            t.valid = true;
        }
    }

    // Triangles around each welded vertex.
    const uint32_t vertexCount =
        corners ? *std::max_element(vertexOf.begin(), vertexOf.end()) + 1u : 0u;
    std::vector<uint32_t> start(vertexCount + 1, 0);
    for (const Tri &t : tris)
        for (int k = 0; k < 3; ++k)
            ++start[t.v[k] + 1];
    for (uint32_t i = 0; i < vertexCount; ++i)
        start[i + 1] += start[i];
    std::vector<uint32_t> fill(start.begin(), start.end() - 1);
    std::vector<uint32_t> around(start[vertexCount]); // tri * 3 + corner
    for (uint32_t i = 0; i < uint32_t(tris.size()); ++i)
        for (int k = 0; k < 3; ++k)
            around[fill[tris[i].v[k]]++] = i * 3 + uint32_t(k);

    std::vector<MikkTangent> perTriCorner(tris.size() * 3);
    std::vector<uint8_t> done(tris.size() * 3, 0);
    std::vector<uint32_t> parent;
    for (uint32_t v = 0; v < vertexCount; ++v) {
        const uint32_t b = start[v], e = start[v + 1], n = e - b;
        if (n == 0)
            continue;
        // Groups: valid triangles of the same orientation joined through an
        // edge (v, w) both contain.
        parent.resize(n);
        std::iota(parent.begin(), parent.end(), 0u);
        for (uint32_t i = 0; i < n; ++i) {
            const Tri &ti = tris[around[b + i] / 3];
            if (!ti.valid)
                continue;
            for (uint32_t j = i + 1; j < n; ++j) {
                const Tri &tj = tris[around[b + j] / 3];
                if (!tj.valid || tj.orient != ti.orient)
                    continue;
                bool shared = false;
                for (int ki = 0; ki < 3 && !shared; ++ki)
                    if (ti.v[ki] != v)
                        for (int kj = 0; kj < 3; ++kj)
                            if (tj.v[kj] == ti.v[ki]) {
                                shared = true;
                                break;
                            }
                if (shared)
                    parent[find(parent, i)] = find(parent, j);
            }
        }
        // Each group's angle-weighted tangent.
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t entry = around[b + i];
            if (done[entry] || !tris[entry / 3].valid)
                continue;
            const uint32_t root = find(parent, i);
            Vec3f sum{0.0f, 0.0f, 0.0f};
            const Vec3f nrm = mesh.N[tris[entry / 3].c[entry % 3]];
            for (uint32_t j = 0; j < n; ++j) {
                if (find(parent, j) != root)
                    continue;
                const uint32_t ej = around[b + j];
                const Tri &t = tris[ej / 3];
                const int k = int(ej % 3);
                const Vec3f p = mesh.P[t.c[k]];
                const Vec3f e1 = project(mesh.P[t.c[(k + 2) % 3]] - p, nrm);
                const Vec3f e2 = project(mesh.P[t.c[(k + 1) % 3]] - p, nrm);
                const float angle = std::acos(std::clamp(dot(e1, e2), -1.0f, 1.0f));
                sum += project(t.os, nrm) * angle;
            }
            const float l = length(sum);
            MikkTangent mt;
            mt.t = l > FLT_MIN ? sum * (1.0f / l) : Frame::fromNormal(nrm).t;
            mt.sign = tris[entry / 3].orient ? 1.0f : -1.0f;
            for (uint32_t j = 0; j < n; ++j)
                if (find(parent, j) == root && tris[around[b + j] / 3].valid) {
                    perTriCorner[around[b + j]] = mt;
                    done[around[b + j]] = 1;
                }
        }
        // Triangles without texture area: another group's frame at the vertex.
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t entry = around[b + i];
            if (done[entry])
                continue;
            const Vec3f nrm = mesh.N[tris[entry / 3].c[entry % 3]];
            MikkTangent mt{Frame::fromNormal(nrm).t, 1.0f};
            for (uint32_t j = 0; j < n; ++j)
                if (done[around[b + j]] && tris[around[b + j] / 3].valid) {
                    mt = perTriCorner[around[b + j]];
                    break;
                }
            perTriCorner[entry] = mt;
            done[entry] = 1;
        }
    }

    std::vector<uint8_t> set(corners, 0);
    for (size_t i = 0; i < tris.size(); ++i)
        for (int k = 0; k < 3; ++k) {
            const uint32_t c = tris[i].c[k];
            if (!set[c]) {
                out[c] = perTriCorner[i * 3 + size_t(k)];
                set[c] = 1;
            }
        }
    return out;
}

} // namespace nova
