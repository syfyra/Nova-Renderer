// Furnace tests through the full renderer. In an enclosure of uniform
// radiance 1, white Lambertian geometry of any shape must render as 1 at
// every pixel with unlimited path length: every estimator (light sampling,
// BSDF sampling, MIS, light selection, Russian roulette) must be unbiased.

#include "test_helpers.h"

#include "core/integrator/renderer.h"
#include "core/math/rng.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

Mat4 rotX(double deg) {
    const double a = deg * kPi / 180.0;
    Mat4 m = Mat4::identity();
    m.m[1][1] = std::cos(a);
    m.m[1][2] = std::sin(a);
    m.m[2][1] = -std::sin(a);
    m.m[2][2] = std::cos(a);
    return m;
}

Mat4 rotY(double deg) {
    const double a = deg * kPi / 180.0;
    Mat4 m = Mat4::identity();
    m.m[0][0] = std::cos(a);
    m.m[0][2] = -std::sin(a);
    m.m[2][0] = std::sin(a);
    m.m[2][2] = std::cos(a);
    return m;
}

struct FilmStats {
    double mean = 0.0, min = 1e30, max = -1e30;
};

FilmStats render(const Scene &scene, const Camera &camera, int size, uint32_t spp, uint32_t maxDepth,
                 LightStrategy strategy = LightStrategy::MIS, uint32_t rrStartDepth = 3) {
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = spp;
    settings.maxDepth = maxDepth;
    settings.clampIndirect = 0.0f;
    settings.lightStrategy = strategy;
    settings.rrStartDepth = rrStartDepth;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(camera);
    renderer.resize(size, size);
    renderToConvergence(renderer);
    FilmStats s;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const double v = pixelMean(renderer.film(), x, y).y;
            s.mean += v / (double(size) * size);
            s.min = std::fmin(s.min, v);
            s.max = std::fmax(s.max, v);
        }
    return s;
}

// Open box (no top) of side 1 around the origin: concave, so paths bounce
// inside it and Russian roulette and the path-length limit matter.
MeshDesc makeCup() {
    MeshDesc cup;
    cup.points = {{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, 0.5f},
                  {-0.5f, 0.5f, -0.5f},  {0.5f, 0.5f, -0.5f},  {0.5f, 0.5f, 0.5f},  {-0.5f, 0.5f, 0.5f}};
    const uint32_t quads[5][4] = {{0, 1, 2, 3}, {0, 4, 5, 1}, {1, 5, 6, 2}, {2, 6, 7, 3}, {3, 7, 4, 0}};
    for (const auto &q : quads)
        cup.indices.insert(cup.indices.end(), {q[0], q[1], q[2], q[0], q[2], q[3]});
    cup.setDisplayColor(spectrum(1.0f));
    return cup;
}

constexpr const char *kStrategyNames[] = {"MIS", "light only", "BSDF only"};

} // namespace

// Two textured domes whose textures add up to 1 everywhere: each one alone
// is importance sampled (with MIS compensation), selected by power, and
// combined with BSDF samples, yet the sum is a uniform environment.
TEST_CASE("MIS white furnace with complementary textured domes", "[render][furnace][mis]") {
    Scene scene;
    MeshDesc ball = makeSphere(1.0f, 128, 64);
    ball.setDisplayColor(spectrum(1.0f));
    scene.updateMesh(scene.createMesh(), ball);
    auto a = std::make_shared<Image>(), b = std::make_shared<Image>();
    a->width = b->width = 64;
    a->height = b->height = 32;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 64; ++x) {
            const float u = (float(x) + 0.5f) / 64.0f, v = (float(y) + 0.5f) / 32.0f;
            float t = 0.05f + 0.9f * std::exp(-((u - 0.3f) * (u - 0.3f) + (v - 0.3f) * (v - 0.3f)) / 0.01f);
            if (x >= 40 && x < 44 && y >= 20 && y < 23)
                t = 0.98f; // a hard-edged patch as well
            a->pixels.push_back(spectrum(t));
            b->pixels.push_back(spectrum(1.0f - t));
        }
    for (const auto &image : {a, b}) {
        LightDesc dome;
        dome.type = LightType::Dome;
        dome.texture = image;
        dome.cameraVisible = true;
        scene.updateLight(scene.createLight(), dome);
    }
    scene.commit();

    for (int k = 0; k < 3; ++k) {
        INFO(kStrategyNames[k]);
        const FilmStats s = render(scene, makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.2f, 32, 32), 32, 256, 64,
                                   LightStrategy(k));
        CHECK_THAT(s.mean, WithinAbs(1.0, 0.005));
    }
}

// Six rect lights (L = 1) face inwards on the faces of the cube [-3, 3]^3:
// an area-light furnace (an enclosing sphere light does not work, sphere
// and cylinder lights emit outwards only). White sphere inside a white cup.
TEST_CASE("Area-light white furnace", "[render][furnace][lights]") {
    Scene scene;
    MeshDesc ball = makeSphere(0.3f, 128, 64);
    ball.setDisplayColor(spectrum(1.0f));
    ball.transform = translation({0.0f, -0.15f, 0.0f});
    scene.updateMesh(scene.createMesh(), ball);
    MeshDesc cup = makeCup();
    cup.transform = rotX(50.0); // the opening (+Y) tilts towards the camera (+Z)
    scene.updateMesh(scene.createMesh(), cup);

    // Rect lights emit along local -Z; the rotation turns -Z inwards.
    const struct {
        Mat4 rotation;
        Vec3f center;
    } walls[6] = {{Mat4::identity(), {0, 0, 3}}, {rotY(180), {0, 0, -3}}, {rotY(90), {3, 0, 0}},
                  {rotY(-90), {-3, 0, 0}},       {rotX(-90), {0, 3, 0}},  {rotX(90), {0, -3, 0}}};
    for (const auto &w : walls) {
        LightDesc rect;
        rect.type = LightType::Rect;
        rect.width = rect.height = 6.0f;
        rect.cameraVisible = true;
        rect.transform = w.rotation * translation(w.center);
        REQUIRE(dot(makeLightData(rect).normal, w.center) < 0.0f); // emits inwards
        scene.updateLight(scene.createLight(), rect);
    }
    scene.commit();
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 2.0f}, 1.0f, 32, 32);

    for (int k = 0; k < 3; ++k) {
        for (const uint32_t rr : {3u, 1000u}) { // with and without Russian roulette
            INFO(kStrategyNames[k] << ", Russian roulette from depth " << rr);
            const FilmStats s = render(scene, camera, 32, 256, 64, LightStrategy(k), rr);
            CHECK_THAT(s.mean, WithinAbs(1.0, k == 1 ? 0.01 : 0.005)); // light-only is the noisiest
        }
    }
    // BSDF sampling without roulette: every path has weight exactly 1, so
    // only truncated paths and shading-normal rejections deviate.
    CHECK(render(scene, camera, 32, 64, 64, LightStrategy::BsdfOnly, 1000).min > 0.98);
    // Truncation loses energy inside the cup.
    const FilmStats twoBounces = render(scene, camera, 32, 64, 2);
    CHECK(twoBounces.mean < 0.99);
    CHECK(twoBounces.min < 0.9);
}

// Direct light from many lights of very different power: light selection
// (alias table) and MIS must add up to the sum of the analytic terms
// E_i = pi L_i (r_i / d_i)^2 cos(theta_i) of sphere lights above the plane.
TEST_CASE("Many lights add up", "[render][lights][mis]") {
    Scene scene;
    MeshDesc quad = makeQuad(100.0f);
    quad.setDisplayColor(spectrum(0.5f));
    scene.updateMesh(scene.createMesh(), quad);
    double irradiance = 0.0;
    Rng rng = Rng::make(5, 1);
    for (int i = 0; i < 12; ++i) {
        LightDesc light;
        light.type = LightType::Sphere;
        light.radius = 0.05f + 0.2f * rng.next();
        light.intensity = 0.5f + 20.0f * rng.next() * rng.next();
        const Vec3f c{4.0f * (rng.next() - 0.5f), 4.0f * (rng.next() - 0.5f), 1.0f + 2.0f * rng.next()};
        light.transform = translation(c);
        scene.updateLight(scene.createLight(), light);
        const double d2 = double(dot(c, c)), d = std::sqrt(d2);
        irradiance += kPi * light.intensity * (double(light.radius) * light.radius / d2) * (c.z / d);
    }
    scene.commit();
    const FilmStats s = render(scene, makeOrthoCamera({0.0f, 0.0f, 5.0f}, 0.002f, 4, 4), 4, 1024, 1);
    CHECK_THAT(s.mean, WithinRel(0.5 / kPi * irradiance, 0.01));
}
