#pragma once

// Affine transforms as 3 x 4 floats for instances: compact (48 bytes), POD
// and directly shareable with Embree and OptiX.

#include "core/math/mat.h"

namespace nova {

// Object-to-world transform in the row-vector convention of Mat4/GfMatrix4d:
// p' = p.x * c[0] + p.y * c[1] + p.z * c[2] + c[3], i.e. c[0..2] are the
// images of the x, y and z axes and c[3] is the translation. The memory
// layout is Embree's RTC_FORMAT_FLOAT3X4_COLUMN_MAJOR (and the transpose of
// OptiX's 3 x 4 row-major instance transform).
struct Xform34 {
    float c[4][3];
};
static_assert(sizeof(Xform34) == 48, "Xform34 must stay a tightly packed POD");

NOVA_INLINE Xform34 toXform34(const Mat4 &m) {
    Xform34 x;
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 3; ++k)
            x.c[r][k] = float(m.m[r][k]);
    return x;
}

NOVA_INLINE Vec3f xformPoint(const Xform34 &x, Vec3f p) {
    // Accumulates in double like Mat4::transformAffine (stable far from the origin).
    return {float(double(p.x) * x.c[0][0] + double(p.y) * x.c[1][0] + double(p.z) * x.c[2][0] + x.c[3][0]),
            float(double(p.x) * x.c[0][1] + double(p.y) * x.c[1][1] + double(p.z) * x.c[2][1] + x.c[3][1]),
            float(double(p.x) * x.c[0][2] + double(p.y) * x.c[1][2] + double(p.z) * x.c[2][2] + x.c[3][2])};
}

NOVA_INLINE Vec3f xformVector(const Xform34 &x, Vec3f d) {
    return {d.x * x.c[0][0] + d.y * x.c[1][0] + d.z * x.c[2][0],
            d.x * x.c[0][1] + d.y * x.c[1][1] + d.z * x.c[2][1],
            d.x * x.c[0][2] + d.y * x.c[1][2] + d.z * x.c[2][2]};
}

// Normal transform with the cofactor matrix: n' = n.x (b x c) + n.y (c x a)
// + n.z (a x b) with a, b, c the axis images. cof(M) = det(M) times the
// inverse transpose, so no inverse is needed and singular matrices are
// harmless. Since cross(e1 M, e2 M) = cross(e1, e2) cof(M), the result has
// the orientation of the world-space winding normal, also for mirroring
// transforms (det < 0). Not normalized.
NOVA_INLINE Vec3f xformNormal(const Xform34 &x, Vec3f n) {
    const Vec3f a{x.c[0][0], x.c[0][1], x.c[0][2]};
    const Vec3f b{x.c[1][0], x.c[1][1], x.c[1][2]};
    const Vec3f c{x.c[2][0], x.c[2][1], x.c[2][2]};
    return cross(b, c) * n.x + cross(c, a) * n.y + cross(a, b) * n.z;
}

// Determinant of the linear part; negative for mirroring transforms.
NOVA_INLINE float xformDeterminant(const Xform34 &x) {
    const Vec3f a{x.c[0][0], x.c[0][1], x.c[0][2]};
    const Vec3f b{x.c[1][0], x.c[1][1], x.c[1][2]};
    const Vec3f c{x.c[2][0], x.c[2][1], x.c[2][2]};
    return dot(a, cross(b, c));
}

} // namespace nova
