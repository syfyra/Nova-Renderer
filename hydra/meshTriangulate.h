#pragma once

// Triangles of concave polygons.
//
// HdMeshUtil triangulates a face as a fan from its first vertex. Where the fan
// leaves the polygon (a concave face that is not star-shaped from that vertex)
// its triangles cover what the face does not: an L seen from above covered
// 10.4 units of area where it has 7. HdNovaConcaveFaces finds those faces and
// triangulates them by clipping ears. A polygon of n vertices has n - 2
// triangles either way, so the fan's arrays are patched in place, and what is
// keyed to them (the face of a triangle, the order of the faces) stays.

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/types.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/pxr.h>

#include <cstddef>
#include <cstdint>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaConcaveFaces {
public:
    // Which faces: those whose fan leaves them (a concave face can be
    // star-shaped from its first vertex, and then its fan is right), or every
    // face with a corner that turns the other way (for split()).
    enum class Faces { FanLeaves, Concave };

    // Those faces of `topology` at `points`, with their triangles. Faces
    // HdMeshUtil skips (holes, fewer than 3 vertices) are skipped; faces that
    // are degenerate or reach past `points` keep the fan.
    HdNovaConcaveFaces(HdMeshTopology const &topology, VtVec3fArray const &points, Faces faces = Faces::FanLeaves);

    bool empty() const { return _faces.empty(); }
    size_t size() const { return _faces.size(); }

    // Patches the triangles of HdMeshUtil::ComputeTriangleIndices.
    void fixTriangles(VtVec3iArray *triangles) const;

    // Patches a face-varying primvar that HdMeshUtil triangulated:
    // `triangulated` holds `width` floats for each of the `triangulatedCount`
    // triangle corners, `authored` `width` floats for each of the
    // `authoredCount` face-vertices.
    void fixFaceVarying(float const *authored, size_t authoredCount, size_t width, float *triangulated,
                        size_t triangulatedCount) const;

    // `topology` (the one given to the constructor) with each of these faces
    // replaced by its triangles, for what does not triangulate faces as a fan:
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
    std::vector<Face> _faces;
    std::vector<uint32_t> _corners; // 3 per triangle: vertices of the face, counted from its first, in its winding
    VtIntArray _vertexIndices;
    bool _flip = false; // HdMeshUtil swaps the last two corners of a triangle for left-handed meshes
};

PXR_NAMESPACE_CLOSE_SCOPE
