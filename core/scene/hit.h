#pragma once

// Surface data at ray hits and its reconstruction from the accelerator's
// hit record (mesh, primitive, barycentrics or curve u, instance, t), shared
// by the CPU scene and the GPU (docs/design/M10.md D3). The functions are
// templates over a mesh record type with the members of MeshRecord that
// they read: `desc` (MeshDesc's points, pointSteps, indices, normals,
// sphereRadius, the curve and point data, shape queries, pointCount(),
// normalsPerStep()), `instances`, `motionKeys`, `motionSteps`,
// instanceCount() and `key` (the object's stable key). The CPU passes
// MeshRecord, the GPU a view of plain arrays with the same names.

#include "core/geometry/curves.h"
#include "core/math/frame.h"
#include "core/math/xform.h"
#include "core/scene/mesh.h"
#include "core/scene/motion.h"
#include "core/scene/object_key.h"
#include "core/texture/ray_cone.h"

#include <cmath>
#include <cstdint>

namespace nova {

// World-space surface data at a ray hit.
struct SurfaceHit {
    float t = kInf;
    Vec3f P{};
    Vec3f Ng{};  // unit geometric normal, oriented by the authored triangle winding
    Vec3f N{};   // unit shading normal
    Vec2f uv{};  // barycentrics
    Vec3f Pobject{}; // position in the mesh's own (prototype) space
    Vec3f Nobject{}; // unit shading normal in the mesh's own space
    // World-space gradients of uv over the surface (texture footprints: an
    // attribute at P + a is the attribute at uv + (dudP.a, dvdP.a)).
    Vec3f dudP{}, dvdP{};
    // Largest absolute world coordinate of the hit triangle's corners: the
    // scale of the float error of the ray's hit distance (Embree intersects
    // relative to the corners), see nearestCameraLight().
    float extent = 0.0f;
    MeshId mesh = kInvalidId;
    uint32_t instance = 0; // index into the mesh's instance list (the instanceId AOV)
    uint32_t prim = kInvalidId;
    // Curves (MeshShape::Curves): the unit tangent along the curve, the
    // radius there and the hit's signed offset across it in [-1, 1] (seen
    // along the ray: 0 at the axis). uv is (u along the segment, 0).
    bool curve = false;
    Vec3f tangent{};
    float curveRadius = 0.0f;
    float curveOffset = 0.0f;
    // Round linear curves: the least offset of rays leaving the tube
    // (tubeRayOrigin's minEps), covering the error of the cone test; 0 else.
    float curveExitEps = 0.0f;
    float strandRandom = 0.5f; // per curve in [0, 1) (hair's randomize controls, M9 D6)
};

namespace hit_detail {

NOVA_INLINE Affine3f toAffine(const Xform34 &x) {
    Affine3f a;
    a.x = {x.c[0][0], x.c[0][1], x.c[0][2]};
    a.y = {x.c[1][0], x.c[1][1], x.c[1][2]};
    a.z = {x.c[2][0], x.c[2][1], x.c[2][2]};
    a.t = {x.c[3][0], x.c[3][1], x.c[3][2]};
    return a;
}

// The inverse of the affine point map p -> x p.x + y p.y + z p.z + t
// (Cramer's rule on the cofactors).
NOVA_INLINE Vec3f inversePoint(const Affine3f &a, Vec3f q) {
    const Vec3f d = q - a.t;
    const float det = a.determinant();
    const float inv = det != 0.0f ? 1.0f / det : 0.0f;
    return Vec3f{dot(d, cross(a.y, a.z)), dot(d, cross(a.z, a.x)), dot(d, cross(a.x, a.y))} * inv;
}

NOVA_INLINE uint32_t hashMix(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

NOVA_INLINE float clampf(float x, float lo, float hi) { return x < lo ? lo : (hi < x ? hi : x); }

} // namespace hit_detail

// A moving instance's transform at a ray time: Embree's interpolation, or
// OptiX's where the record asks for it (MeshRecordView::nlerp).
template <class Rec>
NOVA_INLINE Affine3f motionAt(const Rec &rec, uint32_t instance, float time) {
    if constexpr (requires { rec.nlerp; }) {
        if (rec.nlerp)
            return srtAtTimeNlerp(&rec.motionKeys[instance], rec.motionSteps, rec.instanceCount(), time);
    }
    return srtAtTime(&rec.motionKeys[instance], rec.motionSteps, rec.instanceCount(), time);
}

// The object-to-world transform of an instance at a ray time.
template <class Rec>
NOVA_INLINE Affine3f instanceToWorld(const Rec &rec, uint32_t instance, float time) {
    if (rec.motionSteps > 1)
        return motionAt(rec, instance, time);
    return hit_detail::toAffine(rec.instances[instance]);
}

// Analytic sphere hit (docs/design/M3.md M3.6b): the ray's hit point, in the
// sphere's own space, snapped onto the sphere; exact normals. st: longitude
// around +Y (u, from +X towards -Z) and latitude (v, 0 at the bottom pole).
template <class Rec>
NOVA_INLINE SurfaceHit resolveSphereHit(const Rec &rec, MeshId meshId, float t, uint32_t instance, float time,
                                        Vec3f org, Vec3f dir) {
    const auto &m = rec.desc;
    Vec3f center = m.points[0];
    float radius = m.sphereRadius[0];
    if (m.points.size() > 1) {
        float fp = 0.0f;
        const size_t s0 = motionSegment(time, uint32_t(m.points.size()), fp);
        center = lerp(m.points[s0], m.points[s0 + 1], fp);
        radius = lerp(m.sphereRadius[s0], m.sphereRadius[s0 + 1], fp);
    }
    const Affine3f xf = rec.motionSteps > 1 ? motionAt(rec, instance, time)
                                            : hit_detail::toAffine(rec.instances[instance]);
    Vec3f d = hit_detail::inversePoint(xf, org + dir * t) - center;
    const float len = length(d);
    d = len > 0.0f ? d * (1.0f / len) : Vec3f{0.0f, 1.0f, 0.0f};
    const Vec3f pObject = center + d * radius;
    const float handed = xf.determinant() < 0.0f ? -1.0f : 1.0f;

    SurfaceHit hit;
    hit.t = t;
    hit.P = xf.point(pObject);
    hit.N = normalize(xf.normal(d) * handed);
    hit.Ng = hit.N;
    hit.Pobject = pObject;
    hit.Nobject = d;
    const float u = std::atan2(-d.z, d.x) * (0.5f / kPi);
    hit.uv = {u < 0.0f ? u + 1.0f : u, std::acos(nova::fmin(nova::fmax(-d.y, -1.0f), 1.0f)) * (1.0f / kPi)};
    // Gradients of (u, v) on the sphere in object space, then by the inverse
    // transpose (the cofactor transform over the determinant) to world space.
    const float det = xf.determinant();
    if (radius > 0.0f && det != 0.0f) {
        const float xz2 = d.x * d.x + d.z * d.z;
        const Vec3f gu = xz2 > 1e-12f ? Vec3f{d.z, 0.0f, -d.x} * (1.0f / (2.0f * kPi * xz2 * radius)) : Vec3f{};
        const float sinT = std::sqrt(nova::fmax(1.0f - d.y * d.y, 0.0f));
        const Vec3f gv = sinT > 1e-6f ? (Vec3f{0.0f, 1.0f, 0.0f} - d * d.y) * (1.0f / (kPi * sinT * radius)) : Vec3f{};
        hit.dudP = xf.normal(gu) * (1.0f / det);
        hit.dvdP = xf.normal(gv) * (1.0f / det);
    }
    hit.extent = maxComponent(absolute(hit.P)) + radius * nova::fmax(nova::fmax(length(xf.x), length(xf.y)), length(xf.z));
    hit.mesh = meshId;
    hit.instance = instance;
    hit.prim = 0;
    return hit;
}

template <class Rec>
NOVA_INLINE SurfaceHit resolveCurveHit(const Rec &rec, MeshId meshId, uint32_t prim, Vec2f uv, float t,
                                       uint32_t instance, float time, Vec3f org, Vec3f dir) {
    const auto &m = rec.desc;
    size_t s0 = 0;
    float fp = 0.0f;
    if (m.pointSteps > 1)
        s0 = motionSegment(time, m.pointSteps, fp);
    const float u = hit_detail::clampf(uv.x, 0.0f, 1.0f);
    const CurvePoint c = evaluateCurve(m, prim, u, s0, fp);
    const Affine3f xf = rec.motionSteps > 1 ? motionAt(rec, instance, time)
                                            : hit_detail::toAffine(rec.instances[instance]);
    const float scale = std::cbrt(std::fabs(xf.determinant()));
    const Vec3f center = xf.point(c.center), dP = xf.vector(c.tangent);
    const float radius = c.radius * scale;

    SurfaceHit hit;
    hit.t = t;
    hit.P = org + dir * t;
    hit.curve = true;
    hit.curveRadius = radius;
    // The tangent; a degenerate one (a cusp) takes any direction across the ray.
    Vec3f T = dP;
    if (!(dot(T, T) > 0.0f) || !isFinite(T))
        T = Frame::fromNormal(dir).t;
    T = normalize(T);
    hit.tangent = T;
    const auto across = [&](Vec3f v) { return v - T * dot(v, T); };
    // The normal: round tubes from the axis to the hit, ribbons facing the
    // ray (oriented ribbons: their authored normals, turned to the ray).
    Vec3f n = across(-dir);
    // The end caps of linear curves are spheres around the end points
    // (Embree's u is 0 or 1 there): their normal is radial in 3D.
    const bool roundLinear = m.curveStyle == CurveStyle::Round && m.curveBasis == CurveBasis::Linear;
    const bool cap = roundLinear && (uv.x <= 0.0f || uv.x >= 1.0f);
    bool radial = false; // n runs from the axis (a cap's centre) to the hit
    if (m.curveStyle == CurveStyle::Round) {
        const Vec3f r = cap ? hit.P - center : across(hit.P - center);
        if (dot(r, r) > 1e-12f * radius * radius) {
            n = r;
            radial = true;
        }
    } else if (m.curveStyle == CurveStyle::Oriented && !m.normals.empty() && m.curveBasis != CurveBasis::Linear) {
        const CurveWeights wt = curveWeights(m.curveBasis, u);
        const size_t np = m.pointCount();
        Vec3f no{0.0f, 0.0f, 0.0f};
        for (int i = 0; i < 4; ++i) {
            const size_t v = size_t(m.curveSegments[prim]) + size_t(i);
            no += (m.normalsPerStep() ? lerp(m.normals[s0 * np + v], m.normals[(s0 + 1) * np + v], fp) : m.normals[v]) *
                  wt.w[i];
        }
        const Vec3f nw = across(xf.normal(no));
        if (dot(nw, nw) > 0.0f)
            n = dot(nw, dir) > 0.0f ? -nw : nw;
    }
    if (!(dot(n, n) > 0.0f) || !isFinite(n)) {
        n = across(Frame::fromNormal(T).t);
        radial = false;
    }
    hit.Ng = hit.N = normalize(n);
    const Affine3f toObject = inverseAffine(xf);
    // Round linear curves (M9 D4): Embree's cone and sphere tests subtract
    // terms of the squared distance to the segment's end points to get the
    // squared radius, so their error grows with the segment's length over
    // the radius (long thin tubes hit themselves again). The hit moves onto
    // the tube (in object space, where the tube is round: radially, keeping
    // its place along the axis), and rays leave it by at least 2^-21 (8
    // float roundings) of that squared distance over the radius: the cone
    // test's error for the leaving ray, whose origin Embree moves next to
    // the segment's centre, within max(u, 1 - u) of its length from either
    // end point. Short segments keep D4's 1e-3 r.
    if (roundLinear && radius > 0.0f) {
        if (radial) {
            const Vec3f po = toObject.point(hit.P) - c.center;
            const float tt = dot(c.tangent, c.tangent);
            const Vec3f along = cap || !(tt > 0.0f) ? Vec3f{0.0f, 0.0f, 0.0f} : c.tangent * (dot(po, c.tangent) / tt);
            const Vec3f out = po - along;
            if (const float oo = dot(out, out); oo > 0.0f && c.radius > 0.0f)
                hit.P = xf.point(c.center + along + out * (c.radius / std::sqrt(oo)));
        }
        const float reach = nova::fmax(u, 1.0f - u) * length(dP) + radius;
        hit.curveExitEps = reach * reach * (1.0f / 2097152.0f) / radius;
    }
    // The offset across the fiber as the ray sees it (the hair BSDF's h).
    const Vec3f b = cross(T, dir);
    if (const float bb = dot(b, b); bb > 0.0f && radius > 0.0f)
        hit.curveOffset = hit_detail::clampf(dot(hit.P - center, b) / (std::sqrt(bb) * radius), -1.0f, 1.0f);
    hit.uv = {u, 0.0f}; // attributes interpolate along the segment
    if (const float tt = dot(dP, dP); tt > 0.0f)
        hit.dudP = dP * (1.0f / tt);
    if (radius > 0.0f && dot(b, b) > 0.0f)
        hit.dvdP = normalize(b) * (0.5f / radius);
    hit.extent = maxComponent(absolute(center)) + radius;
    hit.mesh = meshId;
    hit.instance = instance;
    hit.prim = prim;
    // Per curve of the object (its stable key, not its id: object_key.h) and
    // instance.
    const uint32_t curveIndex = prim < m.segmentCurve.size() ? m.segmentCurve[prim] : 0u;
    const uint32_t object = hit_detail::hashMix(objectHash(rec.key) + 0x7F4A7C15u * (instance + 1u));
    hit.strandRandom = float(hit_detail::hashMix(curveIndex * 0x9E3779B1u ^ object) >> 8) * (1.0f / 16777216.0f);
    hit.Pobject = toObject.point(hit.P);
    hit.Nobject = normalize(toObject.vector(hit.N));
    return hit;
}

// A point (docs/design/M9.md D1): a sphere around its centre, a disc facing
// the ray, or a disc facing its authored normal (turned to the ray).
template <class Rec>
NOVA_INLINE SurfaceHit resolvePointHit(const Rec &rec, MeshId meshId, uint32_t prim, float t, uint32_t instance,
                                       float time, Vec3f org, Vec3f dir) {
    const auto &m = rec.desc;
    const size_t n = m.pointCount();
    size_t s0 = 0;
    float fp = 0.0f;
    const bool moving = m.pointSteps > 1;
    if (moving)
        s0 = motionSegment(time, m.pointSteps, fp);
    const auto step = [&](const auto &v, size_t i) { return moving ? lerp(v[s0 * n + i], v[(s0 + 1) * n + i], fp) : v[i]; };
    const Vec3f c = step(m.points, prim);
    const float r = m.pointRadius.size() == m.points.size() && moving ? step(m.pointRadius, prim) : m.pointRadius[prim];
    const Affine3f xf = rec.motionSteps > 1 ? motionAt(rec, instance, time)
                                            : hit_detail::toAffine(rec.instances[instance]);
    const Vec3f center = xf.point(c);
    const float radius = r * std::cbrt(std::fabs(xf.determinant()));

    SurfaceHit hit;
    hit.t = t;
    hit.P = org + dir * t;
    Vec3f N = -dir;
    if (m.pointStyle == PointStyle::Sphere) {
        const Vec3f d = hit.P - center;
        if (dot(d, d) > 0.0f)
            N = d;
    } else if (m.pointStyle == PointStyle::OrientedDisc && !m.normals.empty()) {
        const Vec3f no = m.normalsPerStep() ? step(m.normals, prim) : m.normals[prim < m.normals.size() ? prim : 0];
        const Vec3f nw = xf.normal(no);
        if (dot(nw, nw) > 0.0f)
            N = dot(nw, dir) > 0.0f ? -nw : nw;
    }
    hit.N = hit.Ng = normalize(N);
    hit.uv = {0.0f, 0.0f};
    hit.extent = maxComponent(absolute(center)) + radius;
    hit.mesh = meshId;
    hit.instance = instance;
    hit.prim = prim;
    const Affine3f toObject = inverseAffine(xf);
    hit.Pobject = toObject.point(hit.P);
    hit.Nobject = normalize(toObject.vector(hit.N));
    return hit;
}

// Surface data of a hit on any shape; `time` (normalized shutter) must be
// the ray's time so moving geometry is rebuilt where the accelerator
// intersected it. Spheres, curves and points need the ray (origin,
// direction); triangles ignore it.
template <class Rec>
NOVA_INLINE SurfaceHit resolveMeshHit(const Rec &rec, MeshId meshId, uint32_t prim, Vec2f uv, float t,
                                      uint32_t instance, float time, Vec3f org, Vec3f dir) {
    const auto &m = rec.desc;
    if (m.isSphere())
        return resolveSphereHit(rec, meshId, t, instance, time, org, dir);
    if (m.isCurves())
        return resolveCurveHit(rec, meshId, prim, uv, t, instance, time, org, dir);
    if (m.isPoints())
        return resolvePointHit(rec, meshId, prim, t, instance, time, org, dir);
    const uint32_t *tri = &m.indices[size_t(prim) * 3];

    // Object-space corners at `time`, interpolated exactly like Embree.
    const size_t n = m.pointCount();
    size_t s0 = 0, s1 = 0;
    float fp = 0.0f;
    if (m.pointSteps > 1) {
        s0 = motionSegment(time, m.pointSteps, fp);
        s1 = s0 + 1;
    }
    Vec3f o[3];
    for (int c = 0; c < 3; ++c)
        o[c] = m.pointSteps > 1 ? lerp(m.points[s0 * n + tri[c]], m.points[s1 * n + tri[c]], fp) : m.points[tri[c]];

    // Interpolating world-space vertices is more accurate than org + t * dir,
    // which matters for the ULP-based ray offsets.
    Vec3f p0, p1, p2;
    float det;
    Affine3f moving;
    const bool isMoving = rec.motionSteps > 1;
    if (isMoving) {
        moving = motionAt(rec, instance, time);
        p0 = moving.point(o[0]);
        p1 = moving.point(o[1]);
        p2 = moving.point(o[2]);
        det = moving.determinant();
    } else {
        const Xform34 &xf = rec.instances[instance];
        p0 = xformPoint(xf, o[0]);
        p1 = xformPoint(xf, o[1]);
        p2 = xformPoint(xf, o[2]);
        det = xformDeterminant(xf);
    }
    const float w = 1.0f - uv.x - uv.y;
    // A mirroring instance reverses the winding; the sign keeps the authored
    // orientation for Ng and N alike (cofactor normals follow the winding).
    const float handed = det < 0.0f ? -1.0f : 1.0f;

    SurfaceHit hit;
    hit.t = t;
    hit.P = p0 * w + p1 * uv.x + p2 * uv.y;
    const Vec3f e1 = p1 - p0, e2 = p2 - p0, nn = cross(e1, e2);
    if (const float nn2 = dot(nn, nn); nn2 > 0.0f) {
        hit.dudP = cross(e2, nn) * (1.0f / nn2);
        hit.dvdP = cross(nn, e1) * (1.0f / nn2);
    }
    hit.Ng = normalize(nn) * handed;
    hit.N = hit.Ng;
    hit.uv = uv;
    hit.extent = maxComponent(componentMax(absolute(p0), componentMax(absolute(p1), absolute(p2))));
    hit.mesh = meshId;
    hit.instance = instance;
    hit.prim = prim;
    hit.Pobject = o[0] * w + o[1] * uv.x + o[2] * uv.y;
    hit.Nobject = normalize(cross(o[1] - o[0], o[2] - o[0]));

    if (!m.normals.empty()) {
        Vec3f nc[3];
        if (m.normalsPerStep()) {
            for (int c = 0; c < 3; ++c)
                nc[c] = lerp(m.normals[s0 * n + tri[c]], m.normals[s1 * n + tri[c]], fp);
        } else {
            for (int c = 0; c < 3; ++c)
                nc[c] = m.normals[tri[c]];
        }
        const Vec3f nObject = nc[0] * w + nc[1] * uv.x + nc[2] * uv.y;
        if (dot(nObject, nObject) > 0.0f && isFinite(nObject))
            hit.Nobject = normalize(nObject);
        // The cofactor transform times the handedness is the inverse-transpose direction.
        const Vec3f nw = (isMoving ? moving.normal(nObject) : xformNormal(rec.instances[instance], nObject)) * handed;
        if (dot(nw, nw) > 0.0f && isFinite(nw))
            hit.N = normalize(nw);
    }
    return hit;
}

// Mean curvature at a hit relative to its Ng (ray cones; 0 for flat
// shading): from the corner normals, 1/radius on spheres.
template <class Rec>
NOVA_INLINE float meshCurvature(const Rec &rec, const SurfaceHit &hit, float time) {
    const auto &m = rec.desc;
    const Affine3f xf = instanceToWorld(rec, hit.instance, time);
    if (m.isCurves()) // round tubes: around the fibre; ribbons: flat
        return m.curveStyle == CurveStyle::Round && hit.curveRadius > 0.0f ? 1.0f / hit.curveRadius : 0.0f;
    if (m.isPoints()) { // spheres; discs are flat
        if (m.pointStyle != PointStyle::Sphere || hit.prim >= m.pointRadius.size())
            return 0.0f;
        const float r = m.pointRadius[hit.prim] * std::cbrt(std::fabs(xf.determinant()));
        return r > 0.0f ? 1.0f / r : 0.0f;
    }
    if (m.isSphere()) {
        float radius = m.sphereRadius[0];
        if (m.sphereRadius.size() > 1) {
            float fp = 0.0f;
            const size_t s0 = motionSegment(time, uint32_t(m.sphereRadius.size()), fp);
            radius = lerp(m.sphereRadius[s0], m.sphereRadius[s0 + 1], fp);
        }
        const float scale = std::cbrt(std::fabs(xf.determinant()));
        return radius > 0.0f && scale > 0.0f ? 1.0f / (radius * scale) : 0.0f;
    }
    if (m.normals.empty())
        return 0.0f;
    const uint32_t *tri = &m.indices[size_t(hit.prim) * 3];
    const size_t n = m.pointCount();
    size_t s0 = 0, s1 = 0;
    float fp = 0.0f;
    if (m.pointSteps > 1) {
        s0 = motionSegment(time, m.pointSteps, fp);
        s1 = s0 + 1;
    }
    Vec3f p[3], nw[3];
    const float handed = xf.determinant() < 0.0f ? -1.0f : 1.0f;
    for (int c = 0; c < 3; ++c) {
        const Vec3f o = m.pointSteps > 1 ? lerp(m.points[s0 * n + tri[c]], m.points[s1 * n + tri[c]], fp) : m.points[tri[c]];
        const Vec3f no = m.normalsPerStep() ? lerp(m.normals[s0 * n + tri[c]], m.normals[s1 * n + tri[c]], fp)
                                            : m.normals[tri[c]];
        p[c] = xf.point(o);
        const Vec3f w = xf.normal(no) * handed;
        if (!(dot(w, w) > 0.0f))
            return 0.0f;
        nw[c] = normalize(w);
    }
    const float k = triangleCurvature(p[0], p[1], p[2], nw[0], nw[1], nw[2]);
    // Relative to Ng: corner normals against the winding flip the sign.
    return isFinite(k) ? (dot(nw[0] + nw[1] + nw[2], hit.Ng) < 0.0f ? -k : k) : 0.0f;
}

} // namespace nova
