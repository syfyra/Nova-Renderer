#include "core/math/frame.h"
#include "core/math/mat.h"
#include "core/math/rng.h"
#include "core/math/sampling.h"
#include "core/scene/ray.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <limits>

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

// The offset is ulps of the point's largest coordinate along the normal:
// it scales with the scene (no step in units, which would be 15 % of a
// sphere of 0.0001 units), stays a few parts in a million of the distance
// from the origin (Wächter and Binder's constants gave 3 in 100 000: 2 units
// at 100 000), and does not depend on which axis the normal points along (a
// normal perpendicular to the far axis got none of that axis's ulps).
TEST_CASE("Ray origin offset moves to the normal side", "[math]") {
    for (const Vec3f n : {normalize(Vec3f{0.3f, -0.5f, 0.8f}), Vec3f{0.0f, 1.0f, 0.0f}, Vec3f{-1.0f, 0.0f, 0.0f}})
        for (const Vec3f p : {Vec3f{1e-4f, -2e-4f, 0.0f}, Vec3f{1e-3f, -2e-3f, 0.0f}, Vec3f{123.0f, -4567.0f, 89.0f},
                              Vec3f{-1e5f, 3e4f, -2e3f}, Vec3f{1e5f, 0.0f, 0.0f}, Vec3f{0.0f, 0.0f, 3e6f}}) {
            INFO("p = (" << p.x << ", " << p.y << ", " << p.z << "), n = (" << n.x << ", " << n.y << ", " << n.z << ")");
            const Vec3f q = offsetRayOrigin(p, n);
            const float reach = maxComponent(absolute(p));
            CHECK(dot(q - p, n) > 0.0f);
            // About kPointUlps ulps of the largest coordinate (the sum rounds
            // by up to half an ulp of each coordinate).
            CHECK(length(q - p) > 0.5f * kPointUlps * kFloatUlp * reach);
            CHECK(length(q - p) < 1.5f * kPointUlps * kFloatUlp * reach);
        }
    // At the origin itself the point has no rounding of its own: the least
    // step, so that a ray never starts exactly on an axis-aligned polygon
    // through the origin; what else is left is the intersector's error for
    // the shape, leaveSurface()'s.
    const Vec3f n = normalize(Vec3f{0.3f, -0.5f, 0.8f}), origin{0.0f, 0.0f, 0.0f};
    CHECK(dot(offsetRayOrigin(origin, n) - origin, n) > 0.0f);
    CHECK(length(offsetRayOrigin(origin, n) - origin) < 1e-29f);
    CHECK(length(leaveSurface(origin, n, 1.0f) - origin) > 1e-8f);
    // The first segment of a subsurface walk skips by the same scales.
    CHECK(walkEntryDistance(Vec3f{1e-4f, 0.0f, 0.0f}, 0.0f) < 1e-9f);
    CHECK(walkEntryDistance(Vec3f{1e-4f, 0.0f, 0.0f}, 1e-4f) > 0.0f);
}

TEST_CASE("toInt and clampToInt are defined for every float", "[math]") {
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    const int intMin = std::numeric_limits<int>::min();
    // In range: truncation, as the plain conversion.
    CHECK(toInt(0.0f) == 0);
    CHECK(toInt(-0.0f) == 0);
    CHECK(toInt(2.9f) == 2);
    CHECK(toInt(-2.9f) == -2);
    CHECK(toInt(2147483520.0f) == 2147483520); // the largest float below 2^31
    CHECK(toInt(-2147483648.0f) == intMin);
    // Out of range and NaN: INT_MIN (what x86's conversion gives).
    for (float x : {2147483648.0f, 3.0e9f, -3.0e9f, 1e30f, -1e30f, inf, -inf, nan})
        CHECK(toInt(x) == intMin);

    CHECK(clampToInt(3.7f, 0, 10) == 3);
    CHECK(clampToInt(-0.5f, 0, 10) == 0);
    CHECK(clampToInt(-5.0f, -3, 10) == -3);
    CHECK(clampToInt(10.0f, 0, 10) == 10);
    CHECK(clampToInt(1e12f, 0, 2047) == 2047);
    CHECK(clampToInt(-1e12f, 0, 2047) == 0);
    CHECK(clampToInt(inf, 0, 2047) == 2047);
    CHECK(clampToInt(-inf, 0, 2047) == 0);
    CHECK(clampToInt(nan, 5, 2047) == 5);
    CHECK(clampToInt(3.0e9f, 0, std::numeric_limits<int>::max()) == std::numeric_limits<int>::max());
}
