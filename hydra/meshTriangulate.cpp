#include "meshTriangulate.h"

#include <pxr/imaging/hd/tokens.h>

#include <algorithm>
#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

struct P2 {
    double x, y;
};

double cross(P2 a, P2 b, P2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

// Inside the triangle a, b, c (counter-clockwise) or on its border.
bool inTriangle(P2 p, P2 a, P2 b, P2 c) {
    return cross(a, b, p) >= 0.0 && cross(b, c, p) >= 0.0 && cross(c, a, p) >= 0.0;
}

// Clips ears off the counter-clockwise polygon `p`; appends 3 corners per
// triangle, n - 2 triangles in all. A polygon that crosses itself has no
// triangulation: what is left when no ear is found becomes a fan.
void clipEars(std::vector<P2> const &p, std::vector<uint32_t> &left, std::vector<uint32_t> &out) {
    const uint32_t n = uint32_t(p.size());
    left.resize(n);
    double doubleArea = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        left[i] = i;
        if (i >= 2)
            doubleArea += cross(p[0], p[i - 1], p[i]);
    }
    // Below this a triangle counts as flat: three vertices in a line.
    const double flat = 1e-10 * std::fabs(doubleArea);
    size_t from = 0; // the search goes on where the last ear was cut
    while (left.size() > 3) {
        const size_t m = left.size();
        size_t ear = m;
        // A corner that turns left and holds no other vertex is an ear.
        for (size_t s = 0; s < m && ear == m; ++s) {
            const size_t i = (from + s) % m;
            const uint32_t a = left[(i + m - 1) % m], b = left[i], c = left[(i + 1) % m];
            if (cross(p[a], p[b], p[c]) <= flat)
                continue;
            bool free = true;
            for (size_t k = 0; k < m && free; ++k) {
                const uint32_t q = left[k];
                if (q == a || q == b || q == c)
                    continue;
                // A vertex at the place of a corner (a polygon that touches itself) is not inside.
                if ((p[q].x == p[a].x && p[q].y == p[a].y) || (p[q].x == p[b].x && p[q].y == p[b].y) ||
                    (p[q].x == p[c].x && p[q].y == p[c].y))
                    continue;
                free = !inTriangle(p[q], p[a], p[b], p[c]);
            }
            if (free)
                ear = i;
        }
        // Else a vertex in a line with its neighbours: its triangle has no area and leaves the rest as it was.
        for (size_t s = 0; s < m && ear == m; ++s) {
            const size_t i = (from + s) % m;
            if (std::fabs(cross(p[left[(i + m - 1) % m]], p[left[i]], p[left[(i + 1) % m]])) <= flat)
                ear = i;
        }
        if (ear == m)
            break;
        out.insert(out.end(), {left[(ear + m - 1) % m], left[ear], left[(ear + 1) % m]});
        left.erase(left.begin() + std::ptrdiff_t(ear));
        from = ear == 0 ? left.size() - 1 : ear - 1;
    }
    for (size_t j = 0; j + 2 < left.size(); ++j) // from the last vertex, as every fan here
        out.insert(out.end(), {left.back(), left[j], left[j + 1]});
}

} // namespace

HdNovaFaceTriangles::HdNovaFaceTriangles(HdMeshTopology const &topology, VtVec3fArray const &points, Faces faces)
    : _counts(topology.GetFaceVertexCounts()), _vertexIndices(topology.GetFaceVertexIndices()),
      _holes(topology.GetHoleIndices()), _flip(topology.GetOrientation() != HdTokens->rightHanded) {
    VtIntArray const &counts = _counts;
    VtIntArray const &holes = _holes;
    const int *verts = _vertexIndices.cdata();
    const GfVec3f *pts = points.cdata();
    const size_t numVerts = _vertexIndices.size(), numPoints = points.size();

    std::vector<P2> flat;
    std::vector<uint32_t> left;
    size_t v = 0, hole = 0;
    uint32_t triangle = 0;
    for (size_t f = 0; f < counts.size(); ++f) {
        const int nv = counts[f];
        if (nv < 3) {
            // No triangles.
        } else if (hole < holes.size() && holes[hole] == int(f)) {
            ++hole;
        } else {
            const size_t n = size_t(nv);
            _polygons = _polygons || nv > 3;
            bool usable = nv > 3 && v + n <= numVerts;
            for (size_t i = 0; i < n && usable; ++i)
                usable = verts[v + i] >= 0 && size_t(verts[v + i]) < numPoints;
            if (usable) {
                // The polygon's normal, twice its area long (Newell), and the triangles of the fan
                // from the last vertex against it.
                const GfVec3f &p0 = pts[verts[v + n - 1]];
                double nx = 0.0, ny = 0.0, nz = 0.0;
                for (size_t i = 0; i + 2 < n; ++i) {
                    const GfVec3f a = pts[verts[v + i]] - p0, b = pts[verts[v + i + 1]] - p0;
                    nx += double(a[1]) * b[2] - double(a[2]) * b[1];
                    ny += double(a[2]) * b[0] - double(a[0]) * b[2];
                    nz += double(a[0]) * b[1] - double(a[1]) * b[0];
                }
                const double length2 = nx * nx + ny * ny + nz * nz;
                bool leaves = false;
                for (size_t i = 0; i + 2 < n && !leaves && length2 > 0.0; ++i) {
                    const GfVec3f a = pts[verts[v + i]] - p0, b = pts[verts[v + i + 1]] - p0;
                    const double along = (double(a[1]) * b[2] - double(a[2]) * b[1]) * nx +
                                         (double(a[2]) * b[0] - double(a[0]) * b[2]) * ny +
                                         (double(a[0]) * b[1] - double(a[1]) * b[0]) * nz;
                    leaves = along < -1e-6 * length2; // a fan triangle turned over: the fan left the polygon
                }
                // Or a corner that turns the other way.
                for (size_t i = 0; i < n && !leaves && faces == Faces::Concave && length2 > 0.0; ++i) {
                    const GfVec3f &p = pts[verts[v + i]];
                    const GfVec3f a = p - pts[verts[v + (i + n - 1) % n]], b = pts[verts[v + (i + 1) % n]] - p;
                    const double along = (double(a[1]) * b[2] - double(a[2]) * b[1]) * nx +
                                         (double(a[2]) * b[0] - double(a[0]) * b[2]) * ny +
                                         (double(a[0]) * b[1] - double(a[1]) * b[0]) * nz;
                    leaves = along < -1e-6 * length2;
                }
                if (leaves) {
                    // Seen along the normal's largest component, counter-clockwise.
                    const double ax = std::fabs(nx), ay = std::fabs(ny), az = std::fabs(nz);
                    const int drop = ax >= ay && ax >= az ? 0 : (ay >= az ? 1 : 2);
                    int iu = (drop + 1) % 3, iv = (drop + 2) % 3;
                    if ((drop == 0 ? nx : drop == 1 ? ny : nz) < 0.0)
                        std::swap(iu, iv);
                    flat.resize(n);
                    for (size_t i = 0; i < n; ++i) {
                        const GfVec3f &p = pts[verts[v + i]];
                        flat[i] = {double(p[iu]), double(p[iv])};
                    }
                    const uint32_t firstCorner = uint32_t(_corners.size());
                    clipEars(flat, left, _corners);
                    _faces.push_back({triangle, uint32_t(v), firstCorner, uint32_t(n - 2)});
                }
            }
            triangle += uint32_t(nv - 2);
        }
        v += size_t(std::max(nv, 0));
    }
}

template <class Visit> void HdNovaFaceTriangles::forEachPolygon(Visit &&visit) const {
    const size_t numVerts = _vertexIndices.size();
    size_t v = 0, hole = 0, next = 0;
    uint32_t triangle = 0;
    for (size_t f = 0; f < _counts.size(); ++f) { // as HdMeshUtil counts faces and triangles
        const int nv = _counts[f];
        if (nv < 3) {
            // No triangles.
        } else if (hole < _holes.size() && _holes[hole] == int(f)) {
            ++hole;
        } else {
            const Face *clipped = next < _faces.size() && _faces[next].firstVertex == v ? &_faces[next++] : nullptr;
            if (nv > 3 && v + size_t(nv) <= numVerts)
                visit(triangle, v, size_t(nv), clipped);
            triangle += uint32_t(nv - 2);
        }
        v += size_t(std::max(nv, 0));
    }
}

void HdNovaFaceTriangles::fixTriangles(VtVec3iArray *triangles) const {
    if (!_polygons)
        return;
    GfVec3i *out = triangles->data();
    const size_t count = triangles->size();
    const int *verts = _vertexIndices.cdata();
    forEachPolygon([&](uint32_t first, size_t v, size_t n, Face const *clipped) {
        if (size_t(first) + n - 2 > count)
            return;
        for (size_t k = 0; k + 2 < n; ++k) {
            // The corners of triangle k, counted from the face's first vertex: Karma's fan, or the ears'.
            const uint32_t fan[3] = {uint32_t(n - 1), uint32_t(k), uint32_t(k + 1)};
            const uint32_t *c = clipped ? &_corners[size_t(clipped->firstCorner) + k * 3] : fan;
            const int a = verts[v + c[0]], b = verts[v + c[1]], d = verts[v + c[2]];
            out[first + k] = _flip ? GfVec3i(a, d, b) : GfVec3i(a, b, d);
        }
    });
}

void HdNovaFaceTriangles::fixFaceVarying(float const *authored, size_t authoredCount, size_t width,
                                         float *triangulated, size_t triangulatedCount) const {
    if (!_polygons)
        return;
    forEachPolygon([&](uint32_t firstTriangle, size_t v, size_t n, Face const *clipped) {
        for (size_t k = 0; k + 2 < n; ++k) {
            const size_t first = (size_t(firstTriangle) + k) * 3;
            if (first + 3 > triangulatedCount)
                return;
            const uint32_t fan[3] = {uint32_t(n - 1), uint32_t(k), uint32_t(k + 1)};
            const uint32_t *c = clipped ? &_corners[size_t(clipped->firstCorner) + k * 3] : fan;
            const size_t from[3] = {v + c[0], v + c[_flip ? 2 : 1], v + c[_flip ? 1 : 2]};
            for (int i = 0; i < 3; ++i)
                if (from[i] < authoredCount)
                    std::copy_n(authored + from[i] * width, width, triangulated + (first + size_t(i)) * width);
        }
    });
}

HdNovaFaceTriangles::Split HdNovaFaceTriangles::split(HdMeshTopology const &topology) const {
    Split s;
    VtIntArray const &counts = topology.GetFaceVertexCounts();
    VtIntArray const &holes = topology.GetHoleIndices();
    const int *verts = _vertexIndices.cdata();
    const size_t numVerts = _vertexIndices.size();
    size_t v = 0, hole = 0, next = 0;
    for (size_t f = 0; f < counts.size(); ++f) {
        const size_t n = size_t(std::max(counts[f], 0));
        if (hole < holes.size() && holes[hole] == int(f) && counts[f] >= 3) {
            s.holes.push_back(int(s.counts.size()));
            ++hole;
        }
        if (next < _faces.size() && _faces[next].firstVertex == v) {
            Face const &face = _faces[next++];
            for (uint32_t k = 0; k < face.triangles; ++k) {
                s.counts.push_back(3);
                s.faces.push_back(int32_t(f));
                for (int i = 0; i < 3; ++i) {
                    const size_t corner = v + _corners[size_t(face.firstCorner) + size_t(k) * 3 + size_t(i)];
                    s.indices.push_back(verts[corner]);
                    s.corners.push_back(int32_t(corner));
                }
            }
        } else {
            s.counts.push_back(counts[f]);
            s.faces.push_back(int32_t(f));
            for (size_t i = 0; i < n && v + i < numVerts; ++i) {
                s.indices.push_back(verts[v + i]);
                s.corners.push_back(int32_t(v + i));
            }
        }
        v += n;
    }
    return s;
}

PXR_NAMESPACE_CLOSE_SCOPE
