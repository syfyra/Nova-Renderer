// Analytic spheres (docs/design/M3.md, M3.6b): exact silhouettes and normals
// of spheres and ellipsoids (a non-uniform transform), an animated radius,
// instanced spheres, attributes and the sphere's own st.

#include "test_helpers.h"

#include "core/integrator/renderer.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <random>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;

namespace {

MeshDesc makeAnalyticSphere(float radius) {
    MeshDesc m;
    m.shape = MeshShape::Sphere;
    m.points = {{0.0f, 0.0f, 0.0f}};
    m.sphereRadius = {radius};
    return m;
}

// Analytic ray / axis-aligned ellipsoid (semi-axes a) centered at c.
bool ellipsoidHit(Vec3f o, Vec3f d, Vec3f c, Vec3f a, float &t) {
    const Vec3f oc{(o.x - c.x) / a.x, (o.y - c.y) / a.y, (o.z - c.z) / a.z};
    const Vec3f dd{d.x / a.x, d.y / a.y, d.z / a.z};
    const double A = dot(dd, dd), B = 2.0 * dot(oc, dd), C = dot(oc, oc) - 1.0;
    const double disc = B * B - 4.0 * A * C;
    if (disc < 0.0)
        return false;
    const double s = std::sqrt(disc), t0 = (-B - s) / (2.0 * A), t1 = (-B + s) / (2.0 * A);
    t = float(t0 > 1e-4 ? t0 : t1);
    return t > 1e-4;
}

} // namespace

TEST_CASE("Analytic ellipsoids hit exactly", "[spheres]") {
    Scene scene;
    MeshDesc sphere = makeAnalyticSphere(2.0f);
    sphere.transform = Mat4::identity();
    sphere.transform.m[0][0] = 1.0; // scale (1, 0.5, 1.5) then translate (1, -2, 3): semi-axes (2, 1, 3)
    sphere.transform.m[1][1] = 0.5;
    sphere.transform.m[2][2] = 1.5;
    sphere.transform.m[3][0] = 1.0;
    sphere.transform.m[3][1] = -2.0;
    sphere.transform.m[3][2] = 3.0;
    scene.updateMesh(scene.createMesh(), sphere);
    scene.commit();
    const Vec3f c{1.0f, -2.0f, 3.0f}, a{2.0f, 1.0f, 3.0f};

    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    int hits = 0, misses = 0;
    double worstT = 0.0, worstN = 0.0, worstSurface = 0.0;
    for (int i = 0; i < 2000; ++i) {
        // Rays from a box around the ellipsoid towards points near it.
        const Vec3f o = c + Vec3f{u(rng) * 8.0f, u(rng) * 8.0f, u(rng) * 8.0f} + Vec3f{0.0f, 0.0f, 12.0f};
        const Vec3f target = c + Vec3f{u(rng) * 2.4f, u(rng) * 1.2f, u(rng) * 3.4f};
        const Vec3f d = normalize(target - o);
        float tExact = 0.0f;
        const bool expected = ellipsoidHit(o, d, c, a, tExact);
        SurfaceHit hit;
        const bool got = scene.intersect(Ray{o, d, kInf}, hit);
        if (got != expected) {
            ++misses; // only grazing rays may disagree
            continue;
        }
        if (!got)
            continue;
        ++hits;
        worstT = std::fmax(worstT, std::fabs(hit.t - tExact) / tExact);
        const Vec3f q = hit.P - c;
        worstSurface = std::fmax(worstSurface, std::fabs(std::sqrt(sqr(q.x / a.x) + sqr(q.y / a.y) + sqr(q.z / a.z)) - 1.0));
        const Vec3f nExact = normalize(Vec3f{q.x / (a.x * a.x), q.y / (a.y * a.y), q.z / (a.z * a.z)});
        worstN = std::fmax(worstN, double(1.0f - dot(hit.N, nExact)));
        CHECK(dot(hit.Ng, hit.N) > 0.999f);
    }
    CHECK(hits > 500);
    CHECK(misses <= 2);
    CHECK(worstT < 1e-4);
    CHECK(worstSurface < 1e-5);
    CHECK(worstN < 1e-6);
}

TEST_CASE("Sphere surface data", "[spheres]") {
    Scene scene;
    MeshDesc sphere = makeAnalyticSphere(1.0f);
    sphere.setDisplayColor({0.2f, 0.4f, 0.6f});
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, sphere);
    scene.commit();
    SurfaceHit hit;
    REQUIRE(scene.intersect(Ray{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}, kInf}, hit));
    CHECK_THAT(hit.t, WithinAbs(4.0, 1e-5));
    CHECK_THAT(hit.N.z, WithinAbs(1.0, 1e-6));
    CHECK_THAT(hit.Pobject.z, WithinAbs(1.0, 1e-6));
    CHECK_THAT(hit.uv.x, WithinAbs(0.75, 1e-6)); // st: longitude from +X towards -Z
    CHECK_THAT(hit.uv.y, WithinAbs(0.5, 1e-6));  //     latitude from the bottom pole
    REQUIRE(scene.intersect(Ray{{0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, kInf}, hit));
    CHECK_THAT(hit.uv.y, WithinAbs(1.0, 1e-6));
    // displayColor through the default material's primvar read.
    const AttributeRef *refs = scene.attributeRefs(id, kDefaultMaterialId);
    REQUIRE(refs);
    float c[3];
    REQUIRE(fetchAttribute(refs[0], nullptr, hit.prim, hit.instance, hit.uv.x, hit.uv.y, 3, c));
    CHECK(c[1] == 0.4f);
}

TEST_CASE("An animated radius follows the ray time", "[spheres][motion]") {
    Scene scene;
    MeshDesc sphere = makeAnalyticSphere(1.0f);
    sphere.points = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    sphere.sphereRadius = {1.0f, 2.0f};
    sphere.pointSteps = 2;
    scene.updateMesh(scene.createMesh(), sphere);
    scene.commit();
    for (const float time : {0.0f, 0.25f, 1.0f}) {
        SurfaceHit hit;
        REQUIRE(scene.intersect(Ray{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}, kInf}, hit, time));
        CHECK_THAT(hit.t, WithinAbs(5.0 - (1.0 + time), 1e-4));
        CHECK_THAT(hit.P.z, WithinAbs(1.0 + time, 1e-4));
    }
}

TEST_CASE("Instanced spheres", "[spheres][instancing]") {
    Scene scene;
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, makeAnalyticSphere(0.4f));
    std::vector<Xform34> instances;
    for (int i = 0; i < 5; ++i)
        instances.push_back(toXform34(translation({float(i) * 2.0f, 0.0f, 0.0f})));
    scene.setMeshInstances(id, instances);
    scene.commit();
    for (int i = 0; i < 5; ++i) {
        SurfaceHit hit;
        REQUIRE(scene.intersect(Ray{{float(i) * 2.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}, kInf}, hit));
        CHECK(hit.instance == uint32_t(i));
        CHECK_THAT(hit.P.z, WithinAbs(0.4, 1e-5));
        CHECK_THAT(hit.P.x, WithinAbs(float(i) * 2.0f, 1e-5));
    }
    SurfaceHit hit;
    CHECK_FALSE(scene.intersect(Ray{{1.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}, kInf}, hit)); // between the spheres
}

TEST_CASE("A Lambert sphere in a white dome is its albedo", "[spheres][render]") {
    // The exact shape (no faceting) gives the analytic value up to the rim.
    Scene scene;
    MeshDesc sphere = makeAnalyticSphere(1.0f);
    sphere.setDisplayColor(spectrum(0.5f));
    scene.updateMesh(scene.createMesh(), sphere);
    LightDesc dome;
    dome.type = LightType::Dome;
    dome.cameraVisible = true;
    scene.updateLight(scene.createLight(), dome);
    scene.commit();
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = 64;
    settings.maxDepth = 8;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, 32, 32));
    renderer.resize(32, 32);
    renderToConvergence(renderer);
    double sum = 0.0;
    int n = 0;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const float px = (x + 0.5f) / 16.0f - 1.0f, py = (y + 0.5f) / 16.0f - 1.0f;
            if (px * px + py * py > 0.85f * 0.85f)
                continue;
            sum += pixelMean(renderer.film(), x, y).y;
            ++n;
        }
    CHECK_THAT(sum / n, WithinAbs(0.5, 0.004));
}
