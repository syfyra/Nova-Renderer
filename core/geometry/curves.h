#pragma once

// Curves (docs/design/M9.md): the bases of UsdGeomBasisCurves evaluated as
// Embree evaluates them, the segment layout and the conversion of curve
// attributes to per-segment form. A segment of a cubic basis spans four
// consecutive control points (Bezier segments start every third point,
// B-spline and Catmull-Rom segments every point); a linear segment spans two.

#include "core/math/common.h"
#include "core/math/vec.h"
#include "core/scene/mesh.h"

#include <cstdint>

namespace nova {

// Control points per segment, and between the starts of neighbouring segments.
NOVA_INLINE int curveSegmentPoints(CurveBasis basis) { return basis == CurveBasis::Linear ? 2 : 4; }
NOVA_INLINE uint32_t curveSegmentStride(CurveBasis basis) { return basis == CurveBasis::Bezier ? 3u : 1u; }

// Segments of a curve of n control points (0 if it has too few; Bezier
// curves need 3k + 1 points, extra points are ignored).
NOVA_INLINE uint32_t curveSegmentCount(CurveBasis basis, uint32_t n) {
    switch (basis) {
    case CurveBasis::Linear:
        return n >= 2 ? n - 1 : 0;
    case CurveBasis::Bezier:
        return n >= 4 ? (n - 1) / 3 : 0;
    default: // B-spline, Catmull-Rom
        return n >= 4 ? n - 3 : 0;
    }
}

// The weights of a segment's control points at u in [0, 1] (w), and their
// derivatives with respect to u (d). Linear segments use w[0..1].
struct CurveWeights {
    float w[4];
    float d[4];
};

NOVA_INLINE CurveWeights curveWeights(CurveBasis basis, float u) {
    CurveWeights c{};
    const float s = 1.0f - u, u2 = u * u, u3 = u2 * u;
    switch (basis) {
    case CurveBasis::Linear:
        c.w[0] = s;
        c.w[1] = u;
        c.d[0] = -1.0f;
        c.d[1] = 1.0f;
        break;
    case CurveBasis::Bezier:
        c.w[0] = s * s * s;
        c.w[1] = 3.0f * u * s * s;
        c.w[2] = 3.0f * u2 * s;
        c.w[3] = u3;
        c.d[0] = -3.0f * s * s;
        c.d[1] = 3.0f * s * s - 6.0f * u * s;
        c.d[2] = 6.0f * u * s - 3.0f * u2;
        c.d[3] = 3.0f * u2;
        break;
    case CurveBasis::BSpline:
        c.w[0] = s * s * s / 6.0f;
        c.w[1] = (3.0f * u3 - 6.0f * u2 + 4.0f) / 6.0f;
        c.w[2] = (-3.0f * u3 + 3.0f * u2 + 3.0f * u + 1.0f) / 6.0f;
        c.w[3] = u3 / 6.0f;
        c.d[0] = -0.5f * s * s;
        c.d[1] = 1.5f * u2 - 2.0f * u;
        c.d[2] = -1.5f * u2 + u + 0.5f;
        c.d[3] = 0.5f * u2;
        break;
    case CurveBasis::CatmullRom:
        c.w[0] = 0.5f * (-u3 + 2.0f * u2 - u);
        c.w[1] = 0.5f * (3.0f * u3 - 5.0f * u2 + 2.0f);
        c.w[2] = 0.5f * (-3.0f * u3 + 4.0f * u2 + u);
        c.w[3] = 0.5f * (u3 - u2);
        c.d[0] = 0.5f * (-3.0f * u2 + 4.0f * u - 1.0f);
        c.d[1] = 0.5f * (9.0f * u2 - 10.0f * u);
        c.d[2] = 0.5f * (-9.0f * u2 + 8.0f * u + 1.0f);
        c.d[3] = 0.5f * (3.0f * u2 - 2.0f * u);
        break;
    }
    return c;
}

// A point of a segment: its centre, the derivative along u and the radius
// (object space, at a ray time).
struct CurvePoint {
    Vec3f center, tangent;
    float radius = 0.0f;
};

// The segment `segment` of a prepared curve mesh at u, with its control
// points interpolated between deformation steps s0 and s0 + 1 by f (as
// Embree does; steps == 1: static). A MeshDesc, or a device view with the
// same members (core/scene/hit.h).
template <class Mesh>
NOVA_INLINE CurvePoint evaluateCurve(const Mesh &desc, uint32_t segment, float u, size_t s0, float f) {
    const uint32_t start = desc.curveSegments[segment];
    const int k = curveSegmentPoints(desc.curveBasis);
    const size_t n = desc.pointCount();
    const bool moving = desc.pointSteps > 1;
    const bool radiusSteps = moving && desc.curveRadius.size() == desc.points.size();
    const CurveWeights wt = curveWeights(desc.curveBasis, u);
    CurvePoint c;
    c.center = c.tangent = Vec3f{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < k; ++i) {
        const size_t v = size_t(start) + size_t(i);
        const Vec3f p = moving ? lerp(desc.points[s0 * n + v], desc.points[(s0 + 1) * n + v], f) : desc.points[v];
        const float r = radiusSteps ? desc.curveRadius[s0 * n + v] * (1.0f - f) + desc.curveRadius[(s0 + 1) * n + v] * f
                                    : desc.curveRadius[v];
        c.center += p * wt.w[i];
        c.tangent += p * wt.d[i];
        c.radius += r * wt.w[i];
    }
    return c;
}

// Fills curveSegments and segmentCurve from curveVertexCounts and turns the
// curve attributes into per-segment ones (Uniform per curve -> per segment;
// Vertex and Varying -> Corner (start, end, end), the vertex values taken
// on the basis at the segment's ends). Leaves a prepared mesh alone. False
// if the counts do not match the points (the mesh then has no segments).
bool prepareCurves(MeshDesc &desc);

} // namespace nova
