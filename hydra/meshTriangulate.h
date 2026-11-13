#pragma once

// The triangles of polygons.
//
// HdMeshUtil triangulates a face as a fan from its first vertex. Karma fans
// from the face's last vertex (measured for 4 to 6 vertices, whatever the
// shape: a quad 0 1 2 3 is 0 1 3 and 1 2 3, docs/bug-audit-2026-11.md section
// 20), so a quad that is not flat folds along its other diagonal, and a
// primvar that is not affine over a face is interpolated otherwise.
// HdNovaFaceTriangles gives every face of more than three vertices Karma's
// fan.
//
// Where that fan leaves the polygon (a concave face that is not star-shaped
// from the vertex) its triangles would cover what the face does not: an L
// seen from above covered 10.4 units of area where it has 7. Those faces are
// triangulated by clipping ears.
//
// A polygon of n vertices has n - 2 triangles either way, so HdMeshUtil's
// arrays are patched in place, and what is keyed to them (the face of a
// triangle, the order of the faces) stays.

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/types.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/pxr.h>

#include <cstddef>
#include <cstdint>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaFaceTriangles {
public:
    // Which faces have their ears clipped: those whose fan leaves them (a
    // concave face can be star-shaped from its last vertex, and then its fan
    // is right), or every face with a corner that turns the other way (for
    // split()).
    enum class Faces { FanLeaves, Concave };

    // The faces of `topology` at `points`. Faces HdMeshUtil skips (holes,
    // fewer than 3 vertices) are skipped; faces that are degenerate or reach
    // past `points` keep a fan.
    HdNovaFaceTriangles(HdMeshTopology const &topology, VtVec3fArray const &points, Faces faces = Faces::FanLeaves);

    // The faces with clipped ears: none, how many.
    bool empty() const { return _faces.empty(); }
    size_t size() const { return _faces.size(); }

    // Patches the triangles of HdMeshUtil::ComputeTriangleIndices: Karma's fan
    // for every face of more than three vertices, the clipped ears' triangles
    // for the faces that have them.
    void fixTriangles(VtVec3iArray *triangles) const;

    // Patches a face-varying primvar that HdMeshUtil triangulated:
    // `triangulated` holds `width` floats for each of the `triangulatedCount`
    // triangle corners, `authored` `width` floats for each of the
    // `authoredCount` face-vertices.
    void fixFaceVarying(float const *authored, size_t authoredCount, size_t width, float *triangulated,
                        size_t triangulatedCount) const;

    // `topology` (the one given to the constructor) with each of the faces with
    // clipped ears replaced by its triangles, for what does not triangulate faces as a fan:
    // dicing patches a face around its centre, which leaves a concave face as
    // the fan does.
    struct Split {
        VtIntArray counts, indices, holes;
        std::vector<int32_t> faces;   // the authored face of each face
        std::vector<int32_t> corners; // the authored face-vertex of each face-vertex
    };
    Split split(HdMeshTopology const &topology) const;

private:
    struct Face {
        uint32_t firstTriangle; // among the triangles of the mesh
        uint32_t firstVertex;   // among the face-vertices of the mesh
        uint32_t firstCorner;   // in _corners
        uint32_t triangles;
    };
    // Calls visit(firstTriangle, firstVertex, vertexCount, clipped) for every
    // face of more than three vertices that HdMeshUtil triangulates; clipped:
    // its entry of _faces, or null (Karma's fan).
    template <class Visit> void forEachPolygon(Visit &&visit) const;

    std::vector<Face> _faces;       // the faces with clipped ears, in order
    std::vector<uint32_t> _corners; // 3 per triangle: vertices of the face, counted from its first, in its winding
    VtIntArray _counts, _vertexIndices, _holes;
    bool _polygons = false; // some face has more than three vertices
    bool _flip = false; // HdMeshUtil swaps the last two corners of a triangle for left-handed meshes
};

PXR_NAMESPACE_CLOSE_SCOPE
