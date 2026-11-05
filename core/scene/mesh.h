#pragma once

#include "core/math/mat.h"
#include "core/math/spectrum.h"
#include "core/scene/attributes.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace nova {

using MeshId = uint32_t;

// Triangles: points, indices, normals. Sphere (docs/design/M3.md M3.6b): an
// analytic sphere around points[s] (one center per step, usually the origin)
// with radius sphereRadius[s]; the mesh's transform or instances make
// ellipsoids. A sphere is one primitive (uniform attributes: one value).
// Curves (docs/design/M9.md): the control points of curves of one basis,
// swept by a radius; their primitives are the curve segments. Points (M9
// D1): one sphere or disc per point (Embree's point types), radius
// pointRadius; each point is a primitive.
enum class MeshShape : uint8_t { Triangles, Sphere, Curves, Points };

// Karma's point styles: spheres, discs facing the ray, discs facing the
// authored normals.
enum class PointStyle : uint8_t { Sphere, Disc, OrientedDisc };

// The curve bases of UsdGeomBasisCurves (uniform cubics as Embree evaluates
// them; Catmull-Rom with tension 1/2).
enum class CurveBasis : uint8_t { Linear, Bezier, BSpline, CatmullRom };
// The cross-section: a round tube, a ribbon facing the ray, or a ribbon
// facing the authored normals (`normals` per control point).
enum class CurveStyle : uint8_t { Round, Flat, Oriented };

// Triangle mesh in object space, already triangulated by the host. Geometry is
// kept as flat arrays per attribute (SoA), which maps 1:1 to device buffers.
struct MeshDesc {
    // Object-space positions for every deformation step, step-major:
    // points[s * pointCount() + i]. Steps are uniform over the normalized
    // shutter [0, 1]; pointSteps == 1 is a static mesh.
    std::vector<Vec3f> points;
    uint32_t pointSteps = 1;
    MeshShape shape = MeshShape::Triangles;
    std::vector<float> sphereRadius; // Sphere: one radius per step
    std::vector<uint32_t> indices;      // 3 per triangle
    // Optional: one normal per point, or one per point and step (normalsPerStep()).
    std::vector<Vec3f> normals;
    Mat4 transform = Mat4::identity();  // object -> world (at the frame time)
    // Shading attributes (primvars) by name; Vertex rate indexes the points.
    std::vector<MeshAttribute> attributes;
    int32_t userId = -1;                // host id written to the primId AOV
    // Optional: the authored face of every triangle (empty: the triangle
    // index), for the face-index AOV (Karma's ray:primid).
    std::vector<int32_t> triangleFaces;
    bool visible = true;

    // Curves: the control point count of every curve (periodic and pinned
    // curves already expanded by the host), the radius at every control
    // point (or at every point and step), basis and cross-section. The host
    // calls prepareCurves() (core/geometry/curves.h), which fills the
    // segments (first control point and curve of each) and turns the curve
    // attributes into per-segment ones.
    std::vector<uint32_t> curveVertexCounts;
    std::vector<float> curveRadius;
    CurveBasis curveBasis = CurveBasis::Linear;
    CurveStyle curveStyle = CurveStyle::Round;
    std::vector<uint32_t> curveSegments;
    std::vector<uint32_t> segmentCurve;

    // Points: the radius of every point (or of every point and step), the
    // style (oriented discs read the normals).
    std::vector<float> pointRadius;
    PointStyle pointStyle = PointStyle::Sphere;

    uint32_t pointCount() const { return pointSteps > 0 ? uint32_t(points.size() / pointSteps) : 0u; }
    bool normalsPerStep() const { return pointSteps > 1 && normals.size() == points.size(); }
    size_t triangleCount() const { return indices.size() / 3; }
    // Primitives (hit prim ids): triangles, the one sphere, or curve segments.
    size_t primitiveCount() const {
        return shape == MeshShape::Sphere   ? 1
               : shape == MeshShape::Curves ? curveSegments.size()
               : shape == MeshShape::Points ? pointCount()
                                            : triangleCount();
    }
    bool isSphere() const { return shape == MeshShape::Sphere; }
    bool isCurves() const { return shape == MeshShape::Curves; }
    bool isPoints() const { return shape == MeshShape::Points; }

    // `name`, or with alternatives "a|b" the first of them the mesh has
    // (Houdini's Cd, else displayColor; docs/design/M5.md 3.3).
    const MeshAttribute *findAttribute(const std::string &name) const {
        for (size_t begin = 0; begin <= name.size();) {
            const size_t bar = std::min(name.find('|', begin), name.size());
            for (const MeshAttribute &a : attributes)
                if (a.name.size() == bar - begin && a.name.compare(0, a.name.size(), name, begin, bar - begin) == 0)
                    return &a;
            begin = bar + 1;
        }
        return nullptr;
    }
    // Adds or replaces an attribute.
    void setAttribute(MeshAttribute attribute) {
        for (MeshAttribute &a : attributes)
            if (a.name == attribute.name) {
                a = std::move(attribute);
                return;
            }
        attributes.push_back(std::move(attribute));
    }
    // A constant displayColor (the default material's base color).
    void setDisplayColor(Spectrum c) { setAttribute({"displayColor", AttributeRate::Constant, 3, {c.x, c.y, c.z}}); }
};

// Emissive meshes as lights (docs/design/M9.md D14): Karma's
// treat_as_lightsource (0 no, 1 yes, 2 auto: when the material emits, which
// 1 also does, triangle by triangle), the light's multipliers on diffuse and
// specular receivers, and its sampling quality.
struct MeshLightSource {
    int mode = 0;
    float diffuse = 1.0f, specular = 1.0f, samplingQuality = 1.0f;
    bool operator==(const MeshLightSource &) const = default;
};

} // namespace nova
