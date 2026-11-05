// Per-ray-type limits (docs/design/M4.md 3.2, M4.1), checked analytically
// against the semantics measured on Karma (docs/research/m4/nodes-and-limits.md
// 1.2, probes A1-A7):
//   A1 a closed box of emissive Lambert walls (rho 0.5, E 1) has uniform
//      radiance, so a path through K diffuse events sees sum_{k <= K} rho^k:
//      a diffuse limit L gives K = L (1, 1.5, 1.75, 1.875), fractional limits
//      the mixtures;
//   A2 two parallel emissive mirrors (F 0.5 at normal incidence): the same
//      sums for the reflection limit;
//   A3 a stack of glass planes: an emissive backdrop is seen through n
//      planes only if n <= refract limit, with thin walls if n <= 2 limit;
//   A4 the ray past a limit gathers lights (the dome) but no emission;
//   A6 the limit of the object where the event happens applies;
//   A7 sheen counts as diffuse, coat as reflection.
// maxDepth only caps the path; its last ray gathers lights only as well.

#include "test_helpers.h"

#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <memory>
#include <vector>

using namespace nova;
using namespace nova::test;
namespace sf = nova::surface_field;
using Catch::Matchers::WithinAbs;

namespace {

MeshDesc quadAt(Vec3f a, Vec3f b, Vec3f c, Vec3f d) {
    MeshDesc m;
    m.points = {a, b, c, d};
    m.indices = {0, 1, 2, 0, 2, 3};
    return m;
}

void addMesh(Scene &scene, const MeshDesc &mesh, const SurfaceInputs &in, const RayLimitOverrides *limits = nullptr) {
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, mesh);
    MaterialDesc d;
    d.inputs = in;
    const MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, d);
    scene.setMeshMaterials(id, {m});
    if (limits)
        scene.setMeshRayLimits(id, *limits);
}

// The six inward-facing walls of the box [-1, 1]^3.
void addBox(Scene &scene, const SurfaceInputs &walls) {
    const float s = 1.0f;
    const Vec3f p[8] = {{-s, -s, -s}, {s, -s, -s}, {s, s, -s}, {-s, s, -s},
                        {-s, -s, s},  {s, -s, s},  {s, s, s},  {-s, s, s}};
    const int faces[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7}, {1, 5, 6, 2}, {4, 5, 1, 0}, {3, 2, 6, 7}};
    for (const auto &f : faces)
        addMesh(scene, quadAt(p[f[0]], p[f[1]], p[f[2]], p[f[3]]), walls);
}

SurfaceInputs emissive(SurfaceInputs in) {
    in.set(sf::emissionWeight, 1.0f);
    in.set(sf::emissionColor, spectrum(1.0f));
    return in;
}

SurfaceInputs lambertWalls() {
    SurfaceInputs in = standardSurfaceDefaults();
    in.set(sf::baseColor, spectrum(0.5f));
    in.set(sf::specularWeight, 0.0f);
    return emissive(in);
}

// Mean green value seen by an orthographic camera at z = 0.5 looking down -Z
// (inside the box), or at `eye`.
double render(Scene &scene, RayLimits limits, uint32_t spp = 16, uint32_t maxDepth = 64, Vec3f eye = {0.0f, 0.0f, 0.5f},
              float halfWidth = 0.25f, int size = 8) {
    scene.commit();
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = spp;
    settings.maxDepth = maxDepth;
    settings.rrStartDepth = 1000; // no roulette: the sums are exact per path
    settings.clampIndirect = 0.0f;
    settings.pixelFilterScale = 0.0f;
    settings.limits = limits;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera(eye, halfWidth, size, size));
    renderer.resize(size, size);
    renderToConvergence(renderer);
    double sum = 0.0;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            sum += pixelMean(renderer.film(), x, y).y;
    return sum / double(size * size);
}

RayLimits limits(float diffuse, float reflect, float refract, bool thinWall = true) {
    RayLimits l;
    l.limit[int(LimitClass::Diffuse)] = diffuse;
    l.limit[int(LimitClass::Reflect)] = reflect;
    l.limit[int(LimitClass::Refract)] = refract;
    l.thinWallAtRefractLimit = thinWall;
    return l;
}

} // namespace

TEST_CASE("Diffuse limits count diffuse events (A1)", "[render][limits]") {
    Scene scene;
    addBox(scene, lambertWalls());
    const double expected[] = {1.0, 1.5, 1.75, 1.875};
    for (int L = 0; L < 4; ++L) {
        INFO("diffuse limit " << L);
        CHECK_THAT(render(scene, limits(float(L), 10.0f, 10.0f)), WithinAbs(expected[L], 1e-4));
    }
    // Fractional limits: the mixtures of the neighbouring integers.
    CHECK_THAT(render(scene, limits(0.5f, 10.0f, 10.0f), 256), WithinAbs(1.25, 4e-3));
    CHECK_THAT(render(scene, limits(1.25f, 10.0f, 10.0f), 256), WithinAbs(1.5625, 4e-3));
    // maxDepth caps all events; its last ray gathers lights only as well
    // (maxDepth N is diffuse limit N - 1).
    CHECK_THAT(render(scene, RayLimits{}, 16, 1), WithinAbs(1.0, 1e-4));
    CHECK_THAT(render(scene, RayLimits{}, 16, 3), WithinAbs(1.75, 1e-4));
}

TEST_CASE("Reflection limits count reflection events (A2)", "[render][limits]") {
    // Mirrors at z = +-1 facing each other, reflectivity 0.5 at normal
    // incidence, emission 1; the camera between them looks down -Z.
    Scene scene;
    SurfaceInputs mirror = standardSurfaceDefaults();
    mirror.set(sf::metalness, 1.0f);
    mirror.set(sf::baseColor, spectrum(0.5f));
    mirror.set(sf::specularColor, spectrum(1.0f));
    mirror.set(sf::specularRoughness, 0.0f);
    mirror = emissive(mirror);
    addMesh(scene, quadAt({-5, -5, -1}, {5, -5, -1}, {5, 5, -1}, {-5, 5, -1}), mirror);
    addMesh(scene, quadAt({-5, -5, 1}, {-5, 5, 1}, {5, 5, 1}, {5, -5, 1}), mirror);
    const double expected[] = {1.0, 1.5, 1.75, 1.875};
    for (int L = 0; L < 4; ++L) {
        INFO("reflect limit " << L);
        CHECK_THAT(render(scene, limits(0.0f, float(L), 0.0f), 4, 64, {0.0f, 0.0f, 0.0f}), WithinAbs(expected[L], 1e-4));
    }
}

TEST_CASE("Refraction limits and thin walls (A3)", "[render][limits]") {
    // n index-1.5 glass planes at z = -0.1 i over an emissive backdrop at
    // z = -3; reflections are cut at once (reflect limit 0).
    SurfaceInputs glass = standardSurfaceDefaults();
    glass.set(sf::baseWeight, 0.0f);
    glass.set(sf::transmissionWeight, 1.0f);
    glass.set(sf::specularRoughness, 0.0f);
    SurfaceInputs backdrop = unlitDefaults();
    auto seen = [&](int planes, float refract, bool thinWall) {
        Scene scene;
        for (int i = 0; i < planes; ++i) {
            const float z = -0.1f * float(i + 1);
            if (i % 2 == 0)
                addMesh(scene, quadAt({-2, -2, z}, {2, -2, z}, {2, 2, z}, {-2, 2, z}), glass);
            else
                addMesh(scene, quadAt({-2, -2, z}, {-2, 2, z}, {2, 2, z}, {2, -2, z}), glass);
        }
        addMesh(scene, quadAt({-5, -5, -3}, {5, -5, -3}, {5, 5, -3}, {-5, 5, -3}), backdrop);
        return render(scene, limits(0.0f, 0.0f, refract, thinWall), 4, 64, {0.0f, 0.0f, 5.0f});
    };
    for (int n = 1; n <= 4; ++n)
        for (int L = 0; L <= 4; ++L) {
            INFO(n << " planes, refract limit " << L);
            CHECK((seen(n, float(L), false) > 0.01) == (n <= L));
            CHECK((seen(n, float(L), true) > 0.01) == (n <= 2 * L));
        }
}

TEST_CASE("The ray past a limit sees lights, not emission (A4, A6)", "[render][limits]") {
    // A mirror (z = 0, facing +Z) seen from z = 5 reflects the camera rays
    // back up to an emissive quad at z = 10 facing down (behind the camera's
    // near plane), or to a white dome.
    SurfaceInputs mirror = standardSurfaceDefaults();
    mirror.set(sf::metalness, 1.0f);
    mirror.set(sf::baseColor, spectrum(1.0f));
    mirror.set(sf::specularRoughness, 0.0f);
    auto scene = [&](bool emitter, bool dome, const RayLimitOverrides *left) {
        auto s = std::make_unique<Scene>();
        addMesh(*s, quadAt({-2, -2, 0}, {0, -2, 0}, {0, 2, 0}, {-2, 2, 0}), mirror, left);
        addMesh(*s, quadAt({0, -2, 0}, {2, -2, 0}, {2, 2, 0}, {0, 2, 0}), mirror);
        if (emitter)
            addMesh(*s, quadAt({-5, -5, 10}, {-5, 5, 10}, {5, 5, 10}, {5, -5, 10}), unlitDefaults());
        if (dome) {
            LightDesc d;
            d.type = LightType::Dome;
            s->updateLight(s->createLight(), d);
        }
        return s;
    };
    auto pixels = [](Scene &s, RayLimits l, int half) { // 0: all, 1: left half, 2: right half
        s.commit();
        Renderer renderer;
        RenderSettings settings;
        settings.maxSamples = 4;
        settings.maxDepth = 64;
        settings.pixelFilterScale = 0.0f;
        settings.limits = l;
        renderer.setScene(&s);
        renderer.setSettings(settings);
        renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, 8, 8));
        renderer.resize(8, 8);
        renderToConvergence(renderer);
        double sum = 0.0;
        int n = 0;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                if (half == 0 || (half == 1) == (x < 4)) {
                    sum += pixelMean(renderer.film(), x, y).y;
                    ++n;
                }
        return sum / n;
    };
    // Emission: through a full reflection only.
    const auto emitter = scene(true, false, nullptr);
    CHECK(pixels(*emitter, limits(1, 0, 4), 0) == 0.0);
    CHECK(pixels(*emitter, limits(1, 1, 4), 0) > 0.9);
    // Lights: also through the light-only ray.
    const auto dome = scene(false, true, nullptr);
    CHECK(pixels(*dome, limits(1, 0, 4), 0) > 0.9);
    // Per object: the left mirror allows no reflection.
    RayLimitOverrides left;
    left.limit[int(LimitClass::Reflect)] = 0.0f;
    const auto split = scene(true, false, &left);
    CHECK(pixels(*split, limits(1, 4, 4), 1) == 0.0);
    CHECK(pixels(*split, limits(1, 4, 4), 2) > 0.9);
    RayLimitOverrides right;
    right.limit[int(LimitClass::Reflect)] = 4.0f;
    const auto split2 = scene(true, false, &right);
    CHECK(pixels(*split2, limits(1, 0, 4), 1) > 0.9);
    CHECK(pixels(*split2, limits(1, 0, 4), 2) == 0.0);
}

TEST_CASE("Sheen counts as diffuse, coat as reflection (A7)", "[render][limits]") {
    SurfaceInputs sheen = standardSurfaceDefaults();
    sheen.set(sf::baseWeight, 0.0f);
    sheen.set(sf::specularWeight, 0.0f);
    sheen.set(sf::sheenWeight, 1.0f);
    sheen.set(sf::sheenColor, spectrum(1.0f));
    Scene a;
    addBox(a, emissive(sheen));
    const double s0 = render(a, limits(0, 0, 0)), sd = render(a, limits(2, 0, 0)), sr = render(a, limits(0, 2, 0));
    CHECK_THAT(s0, WithinAbs(1.0, 1e-4));
    CHECK(sd > 1.01);
    CHECK_THAT(sr, WithinAbs(1.0, 1e-4));

    SurfaceInputs coat = standardSurfaceDefaults();
    coat.set(sf::baseWeight, 0.0f);
    coat.set(sf::specularWeight, 0.0f);
    coat.set(sf::coatWeight, 1.0f);
    Scene b;
    addBox(b, emissive(coat));
    const double c0 = render(b, limits(0, 0, 0)), cd = render(b, limits(2, 0, 0)), cr = render(b, limits(0, 2, 0));
    CHECK(cd == c0);
    CHECK(cr > c0 + 0.01);
}
