#pragma once

// Plain-array views of the scene for kernels (docs/design/M10.md D3, D4):
// the same member names as MeshDesc and MeshRecord, so the shared hit code
// (core/scene/hit.h) runs on them unchanged. The CPU builds views of its own
// records (tests); the GPU backend points them at device memory.

#include "core/math/common.h"
#include "core/math/xform.h"
#include "core/scene/mesh.h"
#include "core/scene/motion.h"

#include <cstddef>
#include <cstdint>

namespace nova {

// A read-only array: pointer and count, with the parts of std::vector's
// interface the shared code uses.
template <class T>
struct Span {
    const T *ptr = nullptr;
    size_t count = 0;

    NOVA_INLINE const T &operator[](size_t i) const { return ptr[i]; }
    NOVA_INLINE size_t size() const { return count; }
    NOVA_INLINE bool empty() const { return count == 0; }
    NOVA_INLINE const T *data() const { return ptr; }
};

// A host container's span (std::vector and the like).
template <class V>
auto spanOf(const V &v) -> Span<typename V::value_type> {
    return {v.data(), v.size()};
}

// MeshDesc's geometry as plain arrays.
struct MeshDescView {
    Span<Vec3f> points;
    uint32_t pointSteps = 1;
    MeshShape shape = MeshShape::Triangles;
    Span<float> sphereRadius;
    Span<uint32_t> indices;
    Span<Vec3f> normals;
    Span<uint32_t> curveSegments;
    Span<uint32_t> segmentCurve;
    Span<float> curveRadius;
    CurveBasis curveBasis = CurveBasis::Linear;
    CurveStyle curveStyle = CurveStyle::Round;
    Span<float> pointRadius;
    PointStyle pointStyle = PointStyle::Sphere;

    NOVA_INLINE uint32_t pointCount() const { return pointSteps > 0 ? uint32_t(points.size() / pointSteps) : 0u; }
    NOVA_INLINE bool normalsPerStep() const { return pointSteps > 1 && normals.size() == points.size(); }
    NOVA_INLINE size_t triangleCount() const { return indices.size() / 3; }
    NOVA_INLINE bool isSphere() const { return shape == MeshShape::Sphere; }
    NOVA_INLINE bool isCurves() const { return shape == MeshShape::Curves; }
    NOVA_INLINE bool isPoints() const { return shape == MeshShape::Points; }
};

// MeshRecord's instances and motion as plain arrays.
struct MeshRecordView {
    MeshDescView desc;
    Span<Xform34> instances;
    Span<SrtTransform> motionKeys;
    uint32_t motionSteps = 1;
    // The keys interpolate as OptiX does (nlerp, docs/design/M10.md D6): the
    // GPU's hit reconstruction then matches its traversal.
    uint8_t nlerp = 0;

    NOVA_INLINE uint32_t instanceCount() const {
        return motionSteps > 1 ? uint32_t(motionKeys.size() / motionSteps) : uint32_t(instances.size());
    }
};

// A view of a host mesh (desc: a MeshDesc; rec: a MeshRecord or anything
// with its instance members).
template <class Desc>
MeshDescView meshDescView(const Desc &d) {
    MeshDescView v;
    v.points = spanOf(d.points);
    v.pointSteps = d.pointSteps;
    v.shape = d.shape;
    v.sphereRadius = spanOf(d.sphereRadius);
    v.indices = spanOf(d.indices);
    v.normals = spanOf(d.normals);
    v.curveSegments = spanOf(d.curveSegments);
    v.segmentCurve = spanOf(d.segmentCurve);
    v.curveRadius = spanOf(d.curveRadius);
    v.curveBasis = d.curveBasis;
    v.curveStyle = d.curveStyle;
    v.pointRadius = spanOf(d.pointRadius);
    v.pointStyle = d.pointStyle;
    return v;
}

template <class Rec>
MeshRecordView meshRecordView(const Rec &r) {
    MeshRecordView v;
    v.desc = meshDescView(r.desc);
    v.instances = spanOf(r.instances);
    v.motionKeys = spanOf(r.motionKeys);
    v.motionSteps = r.motionSteps;
    return v;
}

} // namespace nova
