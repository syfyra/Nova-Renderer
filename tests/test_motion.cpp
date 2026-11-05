#include "test_helpers.h"

#include "core/math/rng.h"
#include "core/math/sobol.h"
#include "core/scene/motion.h"
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

Mat4 rotationZ(double degrees, Vec3f t = {0.0f, 0.0f, 0.0f}) {
    const double a = degrees * kPi / 180.0;
    Mat4 m = translation(t);
    m.m[0][0] = std::cos(a);
    m.m[0][1] = std::sin(a);
    m.m[1][0] = -std::sin(a);
    m.m[1][1] = std::cos(a);
    return m;
}

// Fraction of stratified ray times at which a ray down -Z at (x, y) hits.
double hitFraction(const Scene &scene, float x, float y, int n = 4096) {
    int hits = 0;
    SurfaceHit hit;
    for (int i = 0; i < n; ++i)
        hits += scene.intersect(Ray{{x, y, 5.0f}, {0.0f, 0.0f, -1.0f}}, hit, (float(i) + 0.5f) / float(n)) ? 1 : 0;
    return double(hits) / n;
}

} // namespace

TEST_CASE("Motion segments follow Embree's arithmetic", "[motion]") {
    float f = 0.0f;
    CHECK(motionSegment(0.0f, 2, f) == 0);
    CHECK(f == 0.0f);
    CHECK(motionSegment(1.0f, 2, f) == 0);
    CHECK(f == 1.0f);
    CHECK(motionSegment(0.25f, 3, f) == 0);
    CHECK(f == 0.5f);
    CHECK(motionSegment(0.75f, 3, f) == 1);
    CHECK(f == 0.5f);
    CHECK(motionSegment(1.0f, 5, f) == 3);
    CHECK(f == 1.0f);
}

TEST_CASE("Decomposed transforms round-trip and interpolate rotations", "[motion]") {
    Rng rng = Rng::make(21, 0);
    auto r = [&] { return 2.0 * double(rng.next()) - 1.0; };
    double worst = 0.0;
    for (int k = 0; k < 2000; ++k) {
        Mat4 m = Mat4::identity();
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                m.m[i][j] = r() * 2.0 + (i == j ? 2.5 : 0.0); // well conditioned, with shear
        if (k % 3 == 0)
            for (int j = 0; j < 3; ++j)
                m.m[2][j] = -m.m[2][j]; // mirror
        for (int j = 0; j < 3; ++j)
            m.m[3][j] = r() * 50.0;
        const Affine3f a = srtToAffine(decomposeSrt(m));
        const Vec3f rows[4] = {a.x, a.y, a.z, a.t};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 3; ++j)
                worst = std::fmax(worst, std::fabs(double(rows[i][j]) - m.m[i][j]) / (i == 3 ? 50.0 : 5.0));
    }
    CHECK(worst < 2e-5);

    // Rotation keys are slerped: halfway between 0 and 90 degrees about Z
    // the x axis is (cos 45, sin 45, 0) with unit length.
    const SrtTransform k0 = decomposeSrt(rotationZ(0.0)), k1 = decomposeSrt(rotationZ(90.0));
    const Affine3f half = interpolateSrt(k0, k1, 0.5f);
    CHECK_THAT(half.x.x, WithinAbs(0.70711, 1e-5));
    CHECK_THAT(half.x.y, WithinAbs(0.70711, 1e-5));
    CHECK_THAT(length(half.x), WithinAbs(1.0, 1e-5));
}

// A unit quad moving by one width during the shutter: a ray at x covers it
// for the swept fraction of the shutter (Embree's linear interpolation).
TEST_CASE("Transform motion blur covers the swept fraction", "[motion]") {
    Scene scene;
    const MeshId id = scene.createMesh();
    MeshDesc quad = makeQuad(0.5f); // [-0.5, 0.5]^2
    scene.updateMesh(id, quad);
    scene.updateMeshMotion(id, {translation({0.5f, 0.5f, 0.0f}), translation({1.5f, 0.5f, 0.0f})}); // x in [0,1] -> [1,2]
    scene.commit();
    CHECK_THAT(hitFraction(scene, 1.5f, 0.5f), WithinAbs(0.5, 1e-3));
    CHECK_THAT(hitFraction(scene, 0.25f, 0.5f), WithinAbs(0.25, 1e-3));
    CHECK_THAT(hitFraction(scene, 1.0f, 0.5f), WithinAbs(1.0, 1e-3));

    SECTION("three steps: a corner turn") {
        scene.updateMeshMotion(id, {translation({0.5f, 0.5f, 0.0f}), translation({1.5f, 0.5f, 0.0f}),
                                    translation({2.5f, 1.5f, 0.0f})});
        scene.commit();
        SurfaceHit hit;
        CHECK(scene.intersect(Ray{{1.0f, 0.5f, 5.0f}, {0, 0, -1}}, hit, 0.25f)); // center (1, 0.5)
        CHECK_FALSE(scene.intersect(Ray{{2.4f, 1.4f, 5.0f}, {0, 0, -1}}, hit, 0.25f));
        CHECK(scene.intersect(Ray{{2.4f, 1.4f, 5.0f}, {0, 0, -1}}, hit, 0.75f)); // center (2, 1)
        CHECK(scene.occluded(Ray{{2.4f, 1.4f, 5.0f}, {0, 0, -1}}, 0.75f));
        CHECK_FALSE(scene.occluded(Ray{{2.4f, 1.4f, 5.0f}, {0, 0, -1}}, 0.25f));
    }
    SECTION("a single step is static again") {
        scene.updateMeshMotion(id, {translation({0.5f, 0.5f, 0.0f})});
        scene.commit();
        CHECK(hitFraction(scene, 0.25f, 0.5f) == 1.0);
        CHECK(hitFraction(scene, 1.5f, 0.5f) == 0.0);
    }
}

TEST_CASE("Deformation motion blur covers the swept fraction", "[motion]") {
    Scene scene;
    MeshDesc quad;
    quad.pointSteps = 2;
    quad.points = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},  // step 0: x in [0, 1]
                   {1, 0, 0}, {2, 0, 0}, {2, 1, 0}, {1, 1, 0}}; // step 1: x in [1, 2]
    quad.indices = {0, 1, 2, 0, 2, 3};
    scene.updateMesh(scene.createMesh(), quad);
    scene.commit();
    CHECK_THAT(hitFraction(scene, 1.5f, 0.5f), WithinAbs(0.5, 1e-3));
    CHECK_THAT(hitFraction(scene, 0.25f, 0.5f), WithinAbs(0.25, 1e-3));
}

// Rotation blur interpolates the rotation: a bar from r = 0.1 to 1 turning
// 0 -> 90 degrees sweeps the quarter annulus. A matrix lerp would shrink the
// bar to 0.71 of its length halfway.
TEST_CASE("Rotation blur sweeps an arc", "[motion]") {
    Scene scene;
    MeshDesc bar;
    bar.points = {{0.1f, -0.05f, 0.0f}, {1.0f, -0.05f, 0.0f}, {1.0f, 0.05f, 0.0f}, {0.1f, 0.05f, 0.0f}};
    bar.indices = {0, 1, 2, 0, 2, 3};
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, bar);
    scene.updateMeshMotion(id, {rotationZ(0.0), rotationZ(90.0)});
    scene.commit();
    // Along the 45 degree diagonal at radius r the bar (width 0.1) passes
    // in a fraction w / (r pi / 2) of the shutter.
    for (const float r : {0.3f, 0.6f, 0.95f}) {
        const float c = r * 0.70710678f;
        CHECK_THAT(hitFraction(scene, c, c, 8192), WithinRel(0.1 / (double(r) * kPi / 2.0), 0.03));
    }
}

// Hits on moving, deforming geometry are rebuilt at the ray's time exactly
// where Embree found them.
TEST_CASE("Moving hits are reconstructed at the ray time", "[motion]") {
    Scene scene;
    MeshDesc mesh = makeSphere(1.0f, 64, 32);
    const size_t n = mesh.points.size();
    mesh.pointSteps = 3;
    for (uint32_t s = 1; s < 3; ++s)
        for (size_t i = 0; i < n; ++i)
            mesh.points.push_back(mesh.points[i] * (1.0f + 0.2f * float(s)));
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, mesh);
    scene.updateMeshMotion(id, {rotationZ(10.0, {580.0f, 100.0f, 0.0f}), rotationZ(60.0, {583.0f, 101.0f, 1.0f})});
    scene.commit();

    double worst = 0.0;
    int hits = 0;
    SurfaceHit hit;
    for (int i = 0; i < 2000; ++i) {
        const float time = (float(i) + 0.5f) / 2000.0f;
        const Vec3f target{581.5f + 0.4f * std::sin(float(i)), 100.5f + 0.4f * std::cos(float(i) * 1.3f), 0.0f};
        const Ray ray{{target.x, target.y, 10.0f}, {0.0f, 0.0f, -1.0f}};
        if (!scene.intersect(ray, hit, time))
            continue;
        ++hits;
        const Vec3f expected = ray.org + ray.dir * hit.t;
        worst = std::fmax(worst, double(length(hit.P - expected)));
    }
    CHECK(hits > 1000);
    CHECK(worst < 4e-4); // a few ulp at |P| ~ 590
}

namespace {

// Lambertian box face lit by a delta distant light along +X, seen by an
// orthographic camera looking along +X: every covered pixel must show
// albedo / pi * E, unshadowed, although the box moves during the shutter.
Camera cameraAlongX(float halfWidth, int size) {
    Camera c = makeOrthoCamera({0.0f, 0.0f, 0.0f}, halfWidth, size, size);
    Mat4 look = Mat4::identity(); // view -Z -> world +X, view +Y -> world +Y
    look.m[0][0] = 0.0;
    look.m[0][2] = -1.0;
    look.m[2][0] = -1.0;
    look.m[2][2] = 0.0;
    look.m[3][0] = -10.0;
    c.viewToWorld = look;
    c.worldToView = inverse(look);
    return c;
}

} // namespace

TEST_CASE("Shadow rays use the path's time", "[motion][render]") {
    Scene scene;
    MeshDesc face; // the -X face of a 1x1x1 box at x = 0, facing -X
    face.points = {{0, -0.5f, -0.5f}, {0, 0.5f, -0.5f}, {0, 0.5f, 0.5f}, {0, -0.5f, 0.5f},
                   {1, -0.5f, -0.5f}, {1, 0.5f, -0.5f}, {1, 0.5f, 0.5f}, {1, -0.5f, 0.5f}};
    face.indices = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7}; // front face and the back face
    face.setDisplayColor(spectrum(0.5f));
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, face);
    // Moves away from the light by two box widths during the shutter: a
    // shadow ray traced at another time than its hit would hit the box.
    scene.updateMeshMotion(id, {translation({0.0f, 0.0f, 0.0f}), translation({2.0f, 0.0f, 0.0f})});
    LightDesc sun;
    sun.type = LightType::Distant;
    sun.intensity = 2.0f;
    sun.angleDeg = 0.0f;
    sun.transform = Mat4::identity();
    sun.transform.m[2][0] = -1.0; // local +Z (towards the light) -> world -X
    sun.transform.m[2][2] = 0.0;
    sun.transform.m[0][2] = 1.0;
    sun.transform.m[0][0] = 0.0;
    scene.updateLight(scene.createLight(), sun);
    scene.commit();

    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = 64;
    settings.maxDepth = 1;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(cameraAlongX(2.0f, 16));
    renderer.resize(16, 16);
    renderToConvergence(renderer);
    const Film &film = renderer.film();
    int covered = 0;
    bool consistent = true;
    for (size_t i = 0; i < film.pixelCount(); ++i) {
        if (film.alphaSum[i] <= 0.0f)
            continue;
        ++covered;
        const double perHit = film.colorSum[i].x / film.alphaSum[i];
        consistent &= std::fabs(perHit - 0.5 / kPi * 2.0) < 1e-4;
    }
    CHECK(covered > 20);
    CHECK(consistent);
}

// White furnace with a moving, rotating and deforming sphere: inconsistent
// hit reconstruction would leak or self-shadow and darken it.
TEST_CASE("White furnace with motion blur", "[motion][furnace]") {
    Scene scene;
    MeshDesc sphere = makeSphere(1.0f, 128, 64);
    sphere.setDisplayColor(spectrum(1.0f));
    const size_t n = sphere.points.size();
    sphere.pointSteps = 2;
    sphere.normals.resize(2 * n);
    for (size_t i = 0; i < n; ++i) {
        sphere.points.push_back(sphere.points[i] * 1.3f);
        sphere.normals[n + i] = sphere.normals[i];
    }
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, sphere);
    scene.updateMeshMotion(id, {rotationZ(0.0, {-0.5f, 0.0f, 0.0f}), rotationZ(70.0, {0.5f, 0.0f, 0.0f})});
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
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 2.0f, 32, 32));
    renderer.resize(32, 32);
    renderToConvergence(renderer);
    const Film &film = renderer.film();
    double sum = 0.0;
    for (size_t i = 0; i < film.pixelCount(); ++i)
        sum += film.colorSum[i].y / film.samples;
    CHECK_THAT(sum / double(film.pixelCount()), WithinAbs(1.0, 0.01));
}

// Moving instances: each instance follows its own keys, the instanceId AOV
// still reports the instance.
TEST_CASE("Instances move with their own keys", "[motion][instancing]") {
    Scene scene;
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, makeQuad(0.5f));
    std::vector<SrtTransform> keys(4); // 2 steps x 2 instances, step-major
    keys[0] = decomposeSrt(translation({0.5f, 0.5f, 0.0f}));  // instance 0, step 0
    keys[1] = decomposeSrt(translation({0.5f, 5.5f, 0.0f}));  // instance 1, step 0
    keys[2] = decomposeSrt(translation({1.5f, 0.5f, 0.0f}));  // instance 0, step 1
    keys[3] = decomposeSrt(translation({0.5f, 6.5f, 0.0f}));  // instance 1, step 1 (moves in y)
    scene.setMeshInstanceMotion(id, keys, 2);
    scene.commit();
    CHECK_THAT(hitFraction(scene, 1.5f, 0.5f), WithinAbs(0.5, 1e-3));
    CHECK_THAT(hitFraction(scene, 0.5f, 6.5f), WithinAbs(0.5, 1e-3));
    SurfaceHit hit;
    REQUIRE(scene.intersect(Ray{{0.5f, 6.9f, 5.0f}, {0, 0, -1}}, hit, 0.9f));
    CHECK(hit.instance == 1);
}
