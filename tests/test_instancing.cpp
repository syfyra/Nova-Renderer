#include "test_helpers.h"

#include "core/math/rng.h"
#include "core/math/xform.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

Xform34 translate34(Vec3f t) { return toXform34(translation(t)); }

Xform34 scale34(Vec3f s, Vec3f t = {0.0f, 0.0f, 0.0f}) {
    Mat4 m = translation(t);
    m.m[0][0] = s.x;
    m.m[1][1] = s.y;
    m.m[2][2] = s.z;
    return toXform34(m);
}

} // namespace

// The 3 x 4 instance transform agrees with Mat4, and its cofactor normal
// transform is the inverse transpose (up to scale), oriented like the
// transformed winding even for mirroring transforms.
TEST_CASE("Xform34 matches Mat4 and transforms normals with the cofactor", "[instancing]") {
    Rng rng = Rng::make(11, 0);
    auto r = [&] { return 2.0 * double(rng.next()) - 1.0; };
    bool points = true, parallel = true, oriented = true;
    for (int k = 0; k < 2000; ++k) {
        Mat4 m = Mat4::identity();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                m.m[i][j] = r() * 2.0;
        if (k % 2) // mirror half of them
            for (int j = 0; j < 3; ++j)
                m.m[0][j] = -m.m[0][j];
        m.m[3][0] = r() * 100.0;
        m.m[3][1] = r() * 100.0;
        m.m[3][2] = r() * 100.0;
        const Xform34 x = toXform34(m);
        const Vec3f p{float(r()), float(r()), float(r())};
        const Vec3f a = xformPoint(x, p), b = m.transformAffine(p);
        points &= length(a - b) <= 1e-4f * (1.0f + length(b));

        // Inverse transpose of the linear part, by way of Mat4's inverse.
        const Vec3f n = normalize(Vec3f{float(r()), float(r()), float(r())});
        const Vec3f cof = normalize(xformNormal(x, n));
        const Vec3f invT = normalize(inverse(m).transformNormalByInverse(n));
        parallel &= std::fabs(std::fabs(dot(cof, invT)) - 1.0f) < 1e-3f;

        // A triangle in the plane perpendicular to n: the transformed winding
        // normal points along the cofactor normal.
        const Frame f = Frame::fromNormal(n);
        const Vec3f e1 = xformVector(x, f.t), e2 = xformVector(x, f.b);
        oriented &= dot(cross(e1, e2), cof) > 0.0f;
    }
    CHECK(points);
    CHECK(parallel);
    CHECK(oriented);
}

TEST_CASE("Instances of one mesh are hit with their index", "[instancing]") {
    Scene scene;
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, makeQuad(0.5f));
    scene.setMeshInstances(id, {translate34({-3, 0, 0}), translate34({0, 0, 0}), translate34({3, 0, 0})});
    scene.commit();

    SurfaceHit hit;
    for (int i = 0; i < 3; ++i) {
        REQUIRE(scene.intersect(Ray{{float(i - 1) * 3.0f, 0.0f, 1.0f}, {0, 0, -1}}, hit));
        CHECK(hit.mesh == id);
        CHECK(hit.instance == uint32_t(i));
        CHECK_THAT(hit.P.x, WithinAbs((i - 1) * 3.0, 1e-5));
    }
    CHECK_FALSE(scene.intersect(Ray{{1.5f, 0.0f, 1.0f}, {0, 0, -1}}, hit)); // between the instances

    // The scene bounds cover every instance.
    CHECK_THAT(scene.sceneRadius(), WithinAbs(0.5 * std::sqrt(7.0 * 7.0 + 1.0), 1e-4));

    SECTION("geometry edits keep the instances") {
        scene.updateMesh(id, makeQuad(1.0f));
        scene.commit();
        REQUIRE(scene.intersect(Ray{{3.9f, 0.0f, 1.0f}, {0, 0, -1}}, hit));
        CHECK(hit.instance == 2);
    }
    SECTION("the list can grow, shrink and empty") {
        scene.setMeshInstances(id, {translate34({0, 5, 0})});
        scene.commit();
        CHECK_FALSE(scene.intersect(Ray{{-3.0f, 0.0f, 1.0f}, {0, 0, -1}}, hit));
        REQUIRE(scene.intersect(Ray{{0.0f, 5.0f, 1.0f}, {0, 0, -1}}, hit));
        CHECK(hit.instance == 0);
        scene.setMeshInstances(id, {});
        scene.commit();
        CHECK_FALSE(scene.intersect(Ray{{0.0f, 5.0f, 1.0f}, {0, 0, -1}}, hit));
        scene.setMeshInstances(id, {translate34({0, 0, 0}), translate34({0, 9, 0})});
        scene.commit();
        REQUIRE(scene.intersect(Ray{{0.0f, 9.0f, 1.0f}, {0, 0, -1}}, hit));
        CHECK(hit.instance == 1);
    }
    SECTION("a single transform leaves instanced mode") {
        scene.updateMeshTransform(id, translation({0.0f, -7.0f, 0.0f}));
        scene.commit();
        CHECK_FALSE(scene.intersect(Ray{{3.0f, 0.0f, 1.0f}, {0, 0, -1}}, hit));
        REQUIRE(scene.intersect(Ray{{0.0f, -7.0f, 1.0f}, {0, 0, -1}}, hit));
        CHECK(hit.instance == 0);
    }
    SECTION("visibility applies to all instances") {
        scene.setMeshVisible(id, false);
        scene.commit();
        CHECK_FALSE(scene.intersect(Ray{{0.0f, 0.0f, 1.0f}, {0, 0, -1}}, hit));
        scene.setMeshVisible(id, true);
        scene.commit();
        CHECK(scene.intersect(Ray{{0.0f, 0.0f, 1.0f}, {0, 0, -1}}, hit));
    }
}

// A sphere instanced with scale (1, 2, 1) is an ellipsoid: the shading normal
// must be the ellipsoid's, normalize(x, y / 4, z), not the transformed sphere
// normal. A mirrored instance keeps outward normals.
TEST_CASE("Instance normals follow non-uniform and mirroring transforms", "[instancing]") {
    Scene scene;
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, makeSphere(1.0f, 256, 128));
    scene.setMeshInstances(id, {scale34({1, 2, 1}), scale34({-1, 1, 1}, {10, 0, 0})});
    scene.commit();

    SurfaceHit hit;
    // Hit the ellipsoid at a point with x = 0.5, y = 1.0 from +Z.
    REQUIRE(scene.intersect(Ray{{0.5f, 1.0f, 10.0f}, {0, 0, -1}}, hit));
    CHECK(hit.instance == 0);
    const Vec3f p = hit.P;
    const Vec3f expected = normalize(Vec3f{p.x, p.y / 4.0f, p.z});
    CHECK(dot(hit.N, expected) > 0.9995f);

    // The mirrored sphere: the normal at its +Z pole still points outwards.
    REQUIRE(scene.intersect(Ray{{10.0f, 0.0f, 10.0f}, {0, 0, -1}}, hit));
    CHECK(hit.instance == 1);
    CHECK(hit.N.z > 0.999f);
}

// First-hit AOVs report the instance index; the background is -1.
TEST_CASE("The instanceId AOV holds the instance index", "[instancing]") {
    Scene scene;
    MeshDesc quad = makeQuad(0.5f);
    quad.userId = 4;
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, quad);
    scene.setMeshInstances(id, {translate34({-3, 0, 0}), translate34({0, 0, 0}), translate34({3, 0, 0})});
    scene.commit();

    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = 1;
    settings.maxDepth = 1;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 10.0f}, 5.0f, 100, 100)); // 10 pixels per unit
    renderer.resize(100, 100);
    renderToConvergence(renderer);
    const Film &film = renderer.film();
    auto at = [&](float x, float y) { // world position -> film index
        return size_t((y + 5.0f) * 10.0f) * 100 + size_t((x + 5.0f) * 10.0f);
    };
    CHECK(film.instanceId[at(-3.0f, 0.0f)] == 0);
    CHECK(film.instanceId[at(0.0f, 0.0f)] == 1);
    CHECK(film.instanceId[at(3.0f, 0.0f)] == 2);
    CHECK(film.instanceId[at(1.5f, 0.0f)] == -1);
    CHECK(film.primId[at(3.0f, 0.0f)] == 4);
    CHECK(film.primId[at(1.5f, 0.0f)] == -1);
}

// White furnace with a plain, a stretched and a mirrored instance: the
// normals must stay consistent, every pixel converges to the dome radiance.
TEST_CASE("White furnace with instances", "[instancing][furnace]") {
    Scene scene;
    MeshDesc sphere = makeSphere(1.0f, 128, 64);
    sphere.setDisplayColor(spectrum(1.0f));
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, sphere);
    scene.setMeshInstances(id, {translate34({-3, 0, 0}), scale34({1, 3, 1}), scale34({-1, 1, 1}, {3, 0, 0})});
    LightDesc dome;
    dome.type = LightType::Dome;
    dome.cameraVisible = true;
    scene.updateLight(scene.createLight(), dome);
    scene.commit();

    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = 256;
    settings.maxDepth = 64;
    settings.clampIndirect = 0.0f;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 10.0f}, 4.5f, 36, 36));
    renderer.resize(36, 36);
    renderToConvergence(renderer);
    const Film &film = renderer.film();
    double sum = 0.0;
    bool finite = true;
    for (size_t i = 0; i < film.pixelCount(); ++i) {
        const Spectrum c = film.colorSum[i] / float(film.samples);
        finite &= isFinite(c);
        sum += c.y;
    }
    CHECK(finite);
    CHECK_THAT(sum / double(film.pixelCount()), WithinAbs(1.0, 0.01));
}
