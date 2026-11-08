// Karma's shading defaults in the integrator (docs/design/M11-P1.md): no
// reflection inside refracting surfaces, glossy roughness constrained along
// paths and clamped on true-caustic paths, Karma's colour limits. Analytic
// scenes; the Karma comparisons are in tests/husk/check_karma_props.py.

#include "test_helpers.h"

#include "core/bsdf/closure.h"
#include "core/bsdf/surface.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using namespace nova;
using namespace nova::test;
namespace sf = nova::surface_field;
using Catch::Matchers::WithinAbs;

namespace {

MaterialId material(Scene &scene, SurfaceInputs const &in) {
    MaterialDesc d;
    d.inputs = in;
    const MaterialId id = scene.createMaterial();
    scene.updateMaterial(id, d);
    return id;
}

// A square at height z facing up (or down), with a material.
MeshId square(Scene &scene, float z, float half, MaterialId m, bool down = false) {
    MeshDesc q;
    q.points = {{-half, -half, z}, {half, -half, z}, {half, half, z}, {-half, half, z}};
    q.indices = down ? std::vector<uint32_t>{0, 2, 1, 0, 3, 2} : std::vector<uint32_t>{0, 1, 2, 0, 2, 3};
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, q);
    scene.setMeshMaterials(id, {m});
    return id;
}

SurfaceInputs emissive(float radiance) {
    SurfaceInputs in = standardSurfaceDefaults();
    in.set(sf::baseWeight, 0.0f);
    in.set(sf::specularWeight, 0.0f);
    in.set(sf::emissionWeight, 1.0f);
    in.set(sf::emissionColor, spectrum(radiance));
    return in;
}

SurfaceInputs glass() {
    SurfaceInputs in = standardSurfaceDefaults();
    in.set(sf::baseWeight, 0.0f);
    in.set(sf::transmissionWeight, 1.0f);
    in.set(sf::specularRoughness, 0.0f);
    return in;
}

double meanOf(const Film &film) {
    double sum = 0.0;
    for (size_t i = 0; i < film.colorSum.size(); ++i)
        sum += luminance(film.colorSum[i]) / float(film.count[i]);
    return sum / double(film.colorSum.size());
}

double render(const Scene &scene, RenderSettings settings, const Camera &camera, int size) {
    Renderer renderer;
    settings.pixelFilterScale = 0.0f;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(camera);
    renderer.resize(size, size);
    renderToConvergence(renderer);
    return meanOf(renderer.film());
}

} // namespace

// A smooth glass slab over an emitter of radiance 1, seen straight down:
// with internal reflection the exit loses F and the light reflected inside
// bounces between the faces, (1 - F)^2 / (1 - F^2); without (Karma's
// default), the exit transmits all, 1 - F (measured: a Karma slab
// transmits 0.96, docs/research/m4/nodes-and-limits.md A3).
TEST_CASE("Internal reflection off: exits keep the Fresnel share", "[render][karma][internal]") {
    const auto slab = [](bool internal) {
        Scene scene;
        square(scene, 0.0f, 20.0f, material(scene, emissive(1.0f)));
        const MaterialId g = material(scene, glass());
        square(scene, 1.0f, 20.0f, g);
        square(scene, 0.9f, 20.0f, g, true);
        RayLimits d;
        d.internalReflection = internal;
        scene.setObjectDefaults(d);
        scene.commit();
        RenderSettings s;
        s.maxSamples = 256;
        s.maxDepth = 24;
        s.clampIndirect = 0.0f;
        return render(scene, s, makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, 8, 8), 8);
    };
    const double F = 0.04;
    CHECK_THAT(slab(true), WithinAbs((1.0 - F) * (1.0 - F) / (1.0 - F * F), 2e-3));
    CHECK_THAT(slab(false), WithinAbs(1.0 - F, 2e-3));
}

TEST_CASE("Glossy lobes take a least roughness", "[bsdf][karma][roughness]") {
    SurfaceInputs mirror = standardSurfaceDefaults();
    mirror.set(sf::metalness, 1.0f);
    mirror.set(sf::specularRoughness, 0.0f);
    const ShadingGeometry g{{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, false, 0.5f};
    const Vec3f wo = normalize(Vec3f{0.3f, 0.0f, 1.0f});
    ClosureList cl;
    buildClosure(mirror, g, wo, ClosureContext{&bsdfTables(), kXyzToRec709Grey}, cl);
    REQUIRE(cl.count == 1);
    REQUIRE(cl.nonDeltaMask == 0); // a mirror: delta only
    clampLobeRoughness(cl, 0.09f);
    CHECK_THAT(glossyAlpha(cl.lobe[0]), WithinAbs(0.09, 1e-6));
    CHECK(cl.nonDeltaMask == 1); // now light-sampled
    clampLobeRoughness(cl, 0.01f); // never smoother
    CHECK_THAT(glossyAlpha(cl.lobe[0]), WithinAbs(0.09, 1e-6));
}

// Karma's colour limits (measured): what the camera sees directly is
// clamped by colorlimit (clampDirect); emission of objects that are not
// lights found by the first bounce counts as indirect (clampEmissionIndirect).
TEST_CASE("Karma's colour limits", "[render][karma][clamp]") {
    {
        Scene scene;
        square(scene, 0.0f, 20.0f, material(scene, emissive(20.0f)));
        scene.commit();
        RenderSettings s;
        s.maxSamples = 4;
        s.clampIndirect = 0.0f;
        const Camera cam = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, 4, 4);
        CHECK_THAT(render(scene, s, cam, 4), WithinAbs(20.0, 1e-4));
        s.clampDirect = 1.0f;
        CHECK_THAT(render(scene, s, cam, 4), WithinAbs(1.0, 1e-4));
    }
    // A grey ground under a large emissive ceiling (radiance 20, not a
    // light): its light arrives by BSDF rays only, 0.5 x 20 = 10 per
    // sample that reaches the ceiling.
    Scene scene;
    SurfaceInputs grey = lambertDefaults(spectrum(0.5f));
    square(scene, 0.0f, 50.0f, material(scene, grey));
    square(scene, 1.0f, 50.0f, material(scene, emissive(20.0f)), true);
    scene.commit();
    RenderSettings s;
    s.maxSamples = 64;
    s.maxDepth = 2; // the first bounce's ray sees emission
    s.clampIndirect = 0.5f;
    const Camera cam = makeOrthoCamera({0.0f, 0.0f, 0.5f}, 0.5f, 4, 4);
    const double direct = render(scene, s, cam, 4); // Nova: the first bounce is direct light
    CHECK(direct > 9.0);
    s.clampEmissionIndirect = true;
    const double indirect = render(scene, s, cam, 4);
    CHECK_THAT(indirect / direct, WithinAbs(0.05, 2e-3));
}
