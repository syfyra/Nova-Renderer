#pragma once

// 4x4 matrix in double precision. The convention matches USD's GfMatrix4d:
// row vectors (p' = p * M), row-major storage, translation in m[3][0..2].

#include "core/math/vec.h"

namespace nova {

struct Mat4 {
    double m[4][4];

    NOVA_INLINE static Mat4 identity() {
        Mat4 r{};
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0;
        return r;
    }

    // Full projective transform (divides by w), used for NDC <-> world.
    NOVA_INLINE Vec3f transformPoint(Vec3f p) const {
        const double x = p.x * m[0][0] + p.y * m[1][0] + p.z * m[2][0] + m[3][0];
        const double y = p.x * m[0][1] + p.y * m[1][1] + p.z * m[2][1] + m[3][1];
        const double z = p.x * m[0][2] + p.y * m[1][2] + p.z * m[2][2] + m[3][2];
        const double w = p.x * m[0][3] + p.y * m[1][3] + p.z * m[2][3] + m[3][3];
        const double iw = w != 0.0 ? 1.0 / w : 1.0;
        return {float(x * iw), float(y * iw), float(z * iw)};
    }
    // Affine transform (ignores the projective column), used for object -> world.
    NOVA_INLINE Vec3f transformAffine(Vec3f p) const {
        return {float(p.x * m[0][0] + p.y * m[1][0] + p.z * m[2][0] + m[3][0]),
                float(p.x * m[0][1] + p.y * m[1][1] + p.z * m[2][1] + m[3][1]),
                float(p.x * m[0][2] + p.y * m[1][2] + p.z * m[2][2] + m[3][2])};
    }
    NOVA_INLINE Vec3f transformDir(Vec3f d) const {
        return {float(d.x * m[0][0] + d.y * m[1][0] + d.z * m[2][0]),
                float(d.x * m[0][1] + d.y * m[1][1] + d.z * m[2][1]),
                float(d.x * m[0][2] + d.y * m[1][2] + d.z * m[2][2])};
    }
    // Transforms a normal with the inverse transpose. `this` must be the
    // INVERSE of the object-to-world matrix.
    NOVA_INLINE Vec3f transformNormalByInverse(Vec3f n) const {
        return {float(n.x * m[0][0] + n.y * m[0][1] + n.z * m[0][2]),
                float(n.x * m[1][0] + n.y * m[1][1] + n.z * m[1][2]),
                float(n.x * m[2][0] + n.y * m[2][1] + n.z * m[2][2])};
    }
};

// Row-vector composition: p * (a * b) == (p * a) * b.
NOVA_INLINE Mat4 operator*(const Mat4 &a, const Mat4 &b) {
    Mat4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] +
                        a.m[i][3] * b.m[3][j];
    return r;
}

// General inverse; returns identity for singular matrices.
Mat4 inverse(const Mat4 &a);

} // namespace nova
