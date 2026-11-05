#pragma once

// Motion blur helpers shared by the host and the kernels. Motion is sampled
// at uniform steps over the normalized shutter [0, 1] (0 = shutter open,
// 1 = shutter close); the host resamples whatever the scene provides. This
// matches Embree's default time range and OptiX's motion key layout.

#include "core/math/mat.h"

#include <cmath>
#include <cstdint>

namespace nova {

constexpr uint32_t kMaxMotionSteps = 16; // Embree allows up to 129

// Same arithmetic as Embree's time segment lookup (kernels/common/default.h),
// so the geometry rebuilt for shading is the geometry Embree intersected.
// steps >= 2; returns the segment and the fraction within it.
NOVA_INLINE uint32_t motionSegment(float time, uint32_t steps, float &frac) {
    const float segments = float(steps - 1);
    const float scaled = time * segments;
    const float seg = nova::fmin(nova::fmax(std::floor(scaled), 0.0f), segments - 1.0f);
    frac = scaled - seg;
    return uint32_t(seg);
}

// Decomposed affine transform M = T * R * S (column vectors), S upper
// triangular with a pivot shift: S = [sx sxy sxz px; 0 sy syz py; 0 0 sz pz].
// The memory layout is exactly Embree's RTCQuaternionDecomposition (and maps
// field by field to OptiX's OptixSRTData), so key arrays can be shared.
struct SrtTransform {
    float sx = 1.0f, sy = 1.0f, sz = 1.0f;
    float sxy = 0.0f, sxz = 0.0f, syz = 0.0f;
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float qw = 1.0f, qx = 0.0f, qy = 0.0f, qz = 0.0f; // rotation quaternion (real first)
    float tx = 0.0f, ty = 0.0f, tz = 0.0f;
};
static_assert(sizeof(SrtTransform) == 64, "SrtTransform must match RTCQuaternionDecomposition");

// Affine transform in the row-vector convention: p' = p.x x + p.y y + p.z z + t
// (x, y, z are the images of the object axes).
struct Affine3f {
    Vec3f x{1.0f, 0.0f, 0.0f}, y{0.0f, 1.0f, 0.0f}, z{0.0f, 0.0f, 1.0f}, t{0.0f, 0.0f, 0.0f};

    NOVA_INLINE Vec3f point(Vec3f p) const { return x * p.x + y * p.y + z * p.z + t; }
    NOVA_INLINE Vec3f vector(Vec3f v) const { return x * v.x + y * v.y + z * v.z; }
    // Cofactor normal transform (det times the inverse transpose; not
    // normalized): oriented like the transformed winding, as xformNormal.
    NOVA_INLINE Vec3f normal(Vec3f n) const { return cross(y, z) * n.x + cross(z, x) * n.y + cross(x, y) * n.z; }
    NOVA_INLINE float determinant() const { return dot(x, cross(y, z)); }
};

NOVA_INLINE Affine3f srtToAffine(const SrtTransform &s, float qw, float qx, float qy, float qz) {
    // Columns of R for the unit quaternion (qw, qx, qy, qz).
    const Vec3f r0{1.0f - 2.0f * (qy * qy + qz * qz), 2.0f * (qx * qy + qw * qz), 2.0f * (qx * qz - qw * qy)};
    const Vec3f r1{2.0f * (qx * qy - qw * qz), 1.0f - 2.0f * (qx * qx + qz * qz), 2.0f * (qy * qz + qw * qx)};
    const Vec3f r2{2.0f * (qx * qz + qw * qy), 2.0f * (qy * qz - qw * qx), 1.0f - 2.0f * (qx * qx + qy * qy)};
    Affine3f a; // the image of axis j is column j of R * S
    a.x = r0 * s.sx;
    a.y = r0 * s.sxy + r1 * s.sy;
    a.z = r0 * s.sxz + r1 * s.syz + r2 * s.sz;
    a.t = r0 * s.px + r1 * s.py + r2 * s.pz + Vec3f{s.tx, s.ty, s.tz};
    return a;
}

NOVA_INLINE Affine3f srtToAffine(const SrtTransform &s) {
    const float n = 1.0f / std::sqrt(s.qw * s.qw + s.qx * s.qx + s.qy * s.qy + s.qz * s.qz);
    return srtToAffine(s, s.qw * n, s.qx * n, s.qy * n, s.qz * n);
}

// Embree's interpolation of quaternion-decomposed transforms (affinespace.h
// slerp): S, the shift and T are lerped, R is slerped along the shortest arc
// (normalized lerp when nearly parallel).
NOVA_INLINE Affine3f interpolateSrt(const SrtTransform &a, const SrtTransform &b, float f) {
    const auto mix = [f](float u, float v) { return (1.0f - f) * u + f * v; };
    float q0[4] = {a.qw, a.qx, a.qy, a.qz}, q1[4] = {b.qw, b.qx, b.qy, b.qz};
    const float n0 = 1.0f / std::sqrt(q0[0] * q0[0] + q0[1] * q0[1] + q0[2] * q0[2] + q0[3] * q0[3]);
    const float n1 = 1.0f / std::sqrt(q1[0] * q1[0] + q1[1] * q1[1] + q1[2] * q1[2] + q1[3] * q1[3]);
    for (int i = 0; i < 4; ++i) {
        q0[i] *= n0;
        q1[i] *= n1;
    }
    float c = q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3];
    if (c < 0.0f) {
        for (float &v : q1)
            v = -v;
        c = -c;
    }
    float q[4];
    if (c > 0.9995f) {
        for (int i = 0; i < 4; ++i)
            q[i] = mix(q0[i], q1[i]);
    } else {
        const float phi = f * std::acos(c);
        float p[4];
        for (int i = 0; i < 4; ++i)
            p[i] = c * q0[i] - q1[i];
        const float pn = 1.0f / std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2] + p[3] * p[3]);
        const float sp = std::sin(phi) * pn, cp = std::cos(phi);
        for (int i = 0; i < 4; ++i)
            q[i] = cp * q0[i] - sp * p[i];
    }
    const float qn = 1.0f / std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    SrtTransform s;
    s.sx = mix(a.sx, b.sx);
    s.sy = mix(a.sy, b.sy);
    s.sz = mix(a.sz, b.sz);
    s.sxy = mix(a.sxy, b.sxy);
    s.sxz = mix(a.sxz, b.sxz);
    s.syz = mix(a.syz, b.syz);
    s.px = mix(a.px, b.px);
    s.py = mix(a.py, b.py);
    s.pz = mix(a.pz, b.pz);
    s.tx = mix(a.tx, b.tx);
    s.ty = mix(a.ty, b.ty);
    s.tz = mix(a.tz, b.tz);
    return srtToAffine(s, q[0] * qn, q[1] * qn, q[2] * qn, q[3] * qn);
}

// Transform at `time` from `steps` uniform keys spaced `stride` apart.
NOVA_INLINE Affine3f srtAtTime(const SrtTransform *keys, uint32_t steps, uint32_t stride, float time) {
    if (steps < 2)
        return srtToAffine(keys[0]);
    float f;
    const uint32_t s = motionSegment(time, steps, f);
    return interpolateSrt(keys[size_t(s) * stride], keys[size_t(s + 1) * stride], f);
}

// OptiX's interpolation of SRT motion keys (docs/design/M10.md D6): every
// component lerped, the quaternion then normalized (nlerp; consecutive keys
// must lie in one hemisphere, which the GPU's upload ensures).
NOVA_INLINE Affine3f interpolateSrtNlerp(const SrtTransform &a, const SrtTransform &b, float f) {
    const auto mix = [f](float u, float v) { return (1.0f - f) * u + f * v; };
    SrtTransform s;
    s.sx = mix(a.sx, b.sx);
    s.sy = mix(a.sy, b.sy);
    s.sz = mix(a.sz, b.sz);
    s.sxy = mix(a.sxy, b.sxy);
    s.sxz = mix(a.sxz, b.sxz);
    s.syz = mix(a.syz, b.syz);
    s.px = mix(a.px, b.px);
    s.py = mix(a.py, b.py);
    s.pz = mix(a.pz, b.pz);
    s.qw = mix(a.qw, b.qw);
    s.qx = mix(a.qx, b.qx);
    s.qy = mix(a.qy, b.qy);
    s.qz = mix(a.qz, b.qz);
    s.tx = mix(a.tx, b.tx);
    s.ty = mix(a.ty, b.ty);
    s.tz = mix(a.tz, b.tz);
    return srtToAffine(s);
}

NOVA_INLINE Affine3f srtAtTimeNlerp(const SrtTransform *keys, uint32_t steps, uint32_t stride, float time) {
    if (steps < 2)
        return srtToAffine(keys[0]);
    float f;
    const uint32_t s = motionSegment(time, steps, f);
    return interpolateSrtNlerp(keys[size_t(s) * stride], keys[size_t(s + 1) * stride], f);
}

// Host side: Gram-Schmidt QR of the linear part (the images of the axes); a
// mirror goes into sz so R stays a proper rotation. Exact for any
// non-singular affine matrix, including shear.
inline SrtTransform decomposeSrt(const Mat4 &m) {
    double c[3][3]; // c[j] = image of axis j = row j of the row-vector matrix
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i)
            c[j][i] = m.m[j][i];
    double q[3][3], u[3][3] = {};
    for (int j = 0; j < 3; ++j) {
        double v[3] = {c[j][0], c[j][1], c[j][2]};
        for (int k = 0; k < j; ++k) {
            const double d = q[k][0] * c[j][0] + q[k][1] * c[j][1] + q[k][2] * c[j][2];
            u[k][j] = d;
            for (int i = 0; i < 3; ++i)
                v[i] -= d * q[k][i];
        }
        const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        u[j][j] = n;
        for (int i = 0; i < 3; ++i)
            q[j][i] = n > 0.0 ? v[i] / n : (i == j ? 1.0 : 0.0);
    }
    const double det = q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1]) -
                       q[1][0] * (q[0][1] * q[2][2] - q[0][2] * q[2][1]) +
                       q[2][0] * (q[0][1] * q[1][2] - q[0][2] * q[1][1]);
    if (det < 0.0) {
        for (int i = 0; i < 3; ++i)
            q[2][i] = -q[2][i];
        u[2][2] = -u[2][2];
    }
    // R(i, j) = q[j][i]; Shepperd's quaternion extraction.
    const double R00 = q[0][0], R11 = q[1][1], R22 = q[2][2];
    const double R01 = q[1][0], R10 = q[0][1], R02 = q[2][0], R20 = q[0][2], R12 = q[2][1], R21 = q[1][2];
    double w, x, y, z;
    const double tr = R00 + R11 + R22;
    if (tr > 0.0) {
        const double s = std::sqrt(tr + 1.0) * 2.0;
        w = 0.25 * s;
        x = (R21 - R12) / s;
        y = (R02 - R20) / s;
        z = (R10 - R01) / s;
    } else if (R00 > R11 && R00 > R22) {
        const double s = std::sqrt(1.0 + R00 - R11 - R22) * 2.0;
        w = (R21 - R12) / s;
        x = 0.25 * s;
        y = (R01 + R10) / s;
        z = (R02 + R20) / s;
    } else if (R11 > R22) {
        const double s = std::sqrt(1.0 + R11 - R00 - R22) * 2.0;
        w = (R02 - R20) / s;
        x = (R01 + R10) / s;
        y = 0.25 * s;
        z = (R12 + R21) / s;
    } else {
        const double s = std::sqrt(1.0 + R22 - R00 - R11) * 2.0;
        w = (R10 - R01) / s;
        x = (R02 + R20) / s;
        y = (R12 + R21) / s;
        z = 0.25 * s;
    }
    SrtTransform r;
    r.sx = float(u[0][0]);
    r.sxy = float(u[0][1]);
    r.sxz = float(u[0][2]);
    r.sy = float(u[1][1]);
    r.syz = float(u[1][2]);
    r.sz = float(u[2][2]);
    r.qw = float(w);
    r.qx = float(x);
    r.qy = float(y);
    r.qz = float(z);
    r.tx = float(m.m[3][0]);
    r.ty = float(m.m[3][1]);
    r.tz = float(m.m[3][2]);
    return r;
}

// Puts consecutive keys into one quaternion hemisphere, so every segment
// rotates along its shortest arc without special cases.
inline void alignQuaternions(SrtTransform *keys, uint32_t steps, uint32_t stride = 1) {
    for (uint32_t s = 1; s < steps; ++s) {
        SrtTransform &k = keys[size_t(s) * stride];
        const SrtTransform &p = keys[size_t(s - 1) * stride];
        if (k.qw * p.qw + k.qx * p.qx + k.qy * p.qy + k.qz * p.qz < 0.0f) {
            k.qw = -k.qw;
            k.qx = -k.qx;
            k.qy = -k.qy;
            k.qz = -k.qz;
        }
    }
}

// The inverse of an affine map (identity if singular).
NOVA_INLINE Affine3f inverseAffine(const Affine3f &a) {
    const float det = a.determinant();
    if (!(det != 0.0f) || !isFinite(det))
        return Affine3f{};
    const float inv = 1.0f / det;
    // Rows of the inverse linear part are the cofactor columns over det.
    const Vec3f r0 = cross(a.y, a.z) * inv, r1 = cross(a.z, a.x) * inv, r2 = cross(a.x, a.y) * inv;
    Affine3f r;
    r.x = {r0.x, r1.x, r2.x};
    r.y = {r0.y, r1.y, r2.y};
    r.z = {r0.z, r1.z, r2.z};
    r.t = -(r.x * a.t.x + r.y * a.t.y + r.z * a.t.z);
    return r;
}

} // namespace nova
