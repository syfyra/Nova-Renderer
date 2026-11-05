#include "core/math/frame.h"
#include "core/math/mat.h"
#include "core/math/rng.h"
#include "core/math/sampling.h"
#include "core/scene/ray.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace nova;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("Mat4 inverse round-trips points", "[math]") {
    Mat4 m = Mat4::identity();
    m.m[0][0] = 2.0; m.m[0][1] = 0.5;
    m.m[1][1] = 3.0;
    m.m[2][0] = -0.25; m.m[2][2] = 4.0;
    m.m[3][0] = 1.0; m.m[3][1] = -2.0; m.m[3][2] = 5.0;
    const Mat4 inv = inverse(m);
    const Vec3f p{0.3f, -0.7f, 1.1f};
    const Vec3f q = inv.transformPoint(m.transformPoint(p));
    CHECK_THAT(q.x, WithinAbs(p.x, 1e-5));
    CHECK_THAT(q.y, WithinAbs(p.y, 1e-5));
    CHECK_THAT(q.z, WithinAbs(p.z, 1e-5));

    const Mat4 id = m * inv;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            CHECK_THAT(id.m[r][c], WithinAbs(r == c ? 1.0 : 0.0, 1e-12));
}

TEST_CASE("Mat4 uses the row-vector convention", "[math]") {
    Mat4 scale = Mat4::identity();
    scale.m[0][0] = 2.0;
    Mat4 move = Mat4::identity();
    move.m[3][0] = 1.0;
    // p * (scale * move): scale first, then translate.
    const Vec3f p = (scale * move).transformAffine({1.0f, 0.0f, 0.0f});
    CHECK_THAT(p.x, WithinAbs(3.0, 1e-6));
}

TEST_CASE("Normals transform with the inverse transpose", "[math]") {
    Mat4 m = Mat4::identity();
    m.m[0][0] = 4.0; // stretch x
    const Mat4 inv = inverse(m);
    // Plane x + y = 0 has normal (1,1,0); stretched in x its normal tilts to y.
    const Vec3f n = normalize(inv.transformNormalByInverse({1.0f, 1.0f, 0.0f}));
    const Vec3f t = m.transformDir({1.0f, -1.0f, 0.0f}); // tangent stays orthogonal
    CHECK_THAT(dot(n, t), WithinAbs(0.0, 1e-6));
}

TEST_CASE("Frame is orthonormal for arbitrary normals", "[math]") {
    Rng rng = Rng::make(1, 2, 3);
    for (int i = 0; i < 1000; ++i) {
        const Vec3f n = sampleUniformSphere(rng.next2());
        const Frame f = Frame::fromNormal(n);
        CHECK_THAT(dot(f.t, f.b), WithinAbs(0.0, 1e-5));
        CHECK_THAT(dot(f.t, f.n), WithinAbs(0.0, 1e-5));
        CHECK_THAT(dot(f.b, f.n), WithinAbs(0.0, 1e-5));
        CHECK_THAT(length(f.t), WithinAbs(1.0, 1e-5));
        CHECK_THAT(length(f.b), WithinAbs(1.0, 1e-5));
        const Vec3f v = sampleUniformSphere(rng.next2());
        const Vec3f w = f.toWorld(f.toLocal(v));
        CHECK_THAT(w.x, WithinAbs(v.x, 1e-5));
        CHECK_THAT(w.y, WithinAbs(v.y, 1e-5));
        CHECK_THAT(w.z, WithinAbs(v.z, 1e-5));
    }
}

TEST_CASE("Ray origin offset moves to the normal side", "[math]") {
    const Vec3f n = normalize(Vec3f{0.3f, -0.5f, 0.8f});
    for (const Vec3f p : {Vec3f{0.0f, 0.0f, 0.0f}, Vec3f{1e-3f, -2e-3f, 0.0f}, Vec3f{123.0f, -4567.0f, 89.0f},
                          Vec3f{-1e5f, 3e4f, -2e3f}}) {
        const Vec3f q = offsetRayOrigin(p, n);
        CHECK(dot(q - p, n) > 0.0f);
        CHECK(length(q - p) < 1e-2f * std::fmax(1.0f, maxComponent(absolute(p))));
    }
}
