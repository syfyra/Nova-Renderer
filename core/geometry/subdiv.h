#pragma once

// Subdivision surfaces (host-side preprocessing, not used by kernels).
// Tessellates the limit surface of a Catmull-Clark/Loop/bilinear control mesh
// into the triangle MeshDesc the integrator already renders, with OpenSubdiv
// 3.7 (Bfr) from Houdini: $HFS/toolkit/include/opensubdiv, static
// $HFS/custom/houdini/dsolib/libosdCPU_md.lib. No USD types: hydra/ maps
// HdMeshTopology + PxOsdSubdivTags to SubdivMeshDesc.

#include "core/math/spectrum.h"
#include "core/math/vec.h"
#include "core/scene/mesh.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nova {

enum class SubdivScheme : uint8_t { None, CatmullClark, Loop, Bilinear };
// Same order as Sdc::Options::VtxBoundaryInterpolation (USD interpolateBoundary).
enum class SubdivVtxBoundary : uint8_t { None, EdgeOnly, EdgeAndCorner };
// Same order as Sdc::Options::FVarLinearInterpolation (USD faceVaryingLinearInterpolation).
enum class SubdivFVarLinear : uint8_t { None, CornersOnly, CornersPlus1, CornersPlus2, Boundaries, All };
// Sdc::Options::TriangleSubdivision (USD triangleSubdivisionRule), Catmull-Clark only.
enum class SubdivTriangleRule : uint8_t { CatmullClark, Smooth };
enum class SubdivCreaseMethod : uint8_t { Uniform, Chaikin };

// Hydra/USD primvar interpolation.
enum class PrimvarInterp : uint8_t { Constant, Uniform, Varying, Vertex, FaceVarying };

// The parameters of the faces as a corner attribute named kFaceParametersName
// (core/scene/attributes.h): the texture coordinates of geometry without st,
// as Karma has them (measured, docs/bug-audit-2026-11.md section 21).
//   Surface  a subdivision surface's: a quad's own (u, v), and for a face of
//            another count each of the quads it is made of, from the face's
//            vertex (0, 0) to its centre (1, 1);
//   Polygon  a polygon's (what is diced for displacement, and Loop meshes): a
//            quad's own (u, v), a triangle's u = w1 / (w1 + w2), v = w1 + w2;
//            faces of more vertices get (0, 0).
enum class SubdivFaceParameters : uint8_t { None, Surface, Polygon };

// A primvar of the control mesh: `width` floats per value.
struct SubdivPrimvar {
    std::string name;
    PrimvarInterp interp = PrimvarInterp::Constant;
    uint8_t width = 1;
    std::vector<float> values;
    std::vector<int32_t> indices; // faceVarying: value index per face-vertex; empty = identity
};

// Control mesh with USD conventions already applied by the host.
struct SubdivMeshDesc {
    SubdivScheme scheme = SubdivScheme::CatmullClark;
    SubdivVtxBoundary vtxBoundary = SubdivVtxBoundary::EdgeAndCorner;
    SubdivFVarLinear fvarLinear = SubdivFVarLinear::CornersPlus1;
    SubdivTriangleRule triangleRule = SubdivTriangleRule::CatmullClark;
    SubdivCreaseMethod creaseMethod = SubdivCreaseMethod::Uniform;
    bool leftHanded = false;               // USD orientation != rightHanded
    SubdivFaceParameters faceParameters = SubdivFaceParameters::None;

    std::vector<int32_t> faceVertexCounts;
    std::vector<int32_t> faceVertexIndices;
    std::vector<int32_t> holeIndices;      // base faces; out-of-range entries are ignored
    std::vector<int32_t> creaseEdges;      // 2 vertex indices per sharp edge
    std::vector<float> creaseSharpness;    // 1 per sharp edge (>= 10: infinitely sharp)
    std::vector<int32_t> cornerVertices;
    std::vector<float> cornerSharpness;

    // Control points (object space) for every deformation step, step-major
    // as in MeshDesc: points[s * (points.size() / pointSteps) + i].
    std::vector<Vec3f> points;
    uint32_t pointSteps = 1;
    std::vector<SubdivPrimvar> primvars;
};

struct SubdivStats {
    uint32_t triangles = 0;
    uint32_t splitVertices = 0; // copies with their own normal at infinitely sharp features
    uint32_t skippedFaces = 0;  // non-hole faces without a limit surface (degenerate topology)
    uint32_t droppedFaces = 0;  // faces with fewer than 3 vertices, ignored
};

// Triangles produced at `rate` (upper bound; holes not subtracted). Catmull-Clark
// and bilinear: sum(faceVertexCounts) * rate^2 / 2, Loop: numFaces * rate^2.
uint64_t subdivTriangleEstimate(const SubdivMeshDesc &desc, int rate);
// The same from the counts alone: `faces` faces with `faceVertices` indices.
uint64_t subdivTriangleEstimate(SubdivScheme scheme, size_t faces, size_t faceVertices, int rate);

// Tessellation rate for subdivision `level` (>= 1) within a triangle budget:
// the largest 2^l, 1 <= l <= level, whose estimate fits, or 0 if even level 1
// exceeds the budget (the caller then renders the control mesh).
int chooseSubdivRate(SubdivScheme scheme, size_t faces, size_t faceVertices, int level, uint64_t budget);

// Evaluates the limit surface of every base face on a uniform grid with `rate`
// segments per base edge (rate = 2^L gives the vertices of L uniform
// refinements) and writes a watertight triangle mesh with limit normals into
// `out` (points and normals for every deformation step, indices and the
// primvars as attributes: vertex and varying ones per output point on the
// limit surface (vertex) or bilinearly (varying), faceVarying ones per
// triangle corner, uniform ones per triangle, constant ones as they are;
// transform, visible and userId are not touched). The topology, and so the indices, are the same for all steps.
// Returns false with a message for topology OpenSubdiv cannot handle; the
// caller then renders the control mesh.
// `triangleFaces` (optional) receives the authored face of every triangle.
bool tessellateSubdiv(const SubdivMeshDesc &desc, int rate, MeshDesc &out, std::string *error = nullptr,
                      SubdivStats *stats = nullptr, std::vector<int32_t> *triangleFaces = nullptr);

// Non-uniform tessellation (displacement dicing, docs/design/M4.md 3.6).
struct TessellateOptions {
    // Segments of the control edge between two control points (1 .. 1024);
    // both faces of an edge use its rate, so the mesh stays watertight. Empty:
    // the uniform rate everywhere.
    std::function<int(int32_t v0, int32_t v1)> edgeRate;
    std::vector<int32_t> *triangleFaces = nullptr; // the authored face of every triangle
    // The point every split copy (sharp features) shares its position with:
    // (copy, source) pairs, so later edits of positions keep them together.
    std::vector<std::pair<int32_t, int32_t>> *copies = nullptr;
};
bool tessellateSubdiv(const SubdivMeshDesc &desc, int rate, MeshDesc &out, const TessellateOptions &options,
                      std::string *error = nullptr, SubdivStats *stats = nullptr);

} // namespace nova
