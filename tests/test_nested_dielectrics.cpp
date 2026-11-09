// Nested dielectrics (Karma's dielectricpriority, docs/design/M11-P9b.md):
// the per-path media, the media's index ratio on the dielectric lobe, and
// analytic renders of Karma's measured rules. The Karma comparisons are in
// tests/husk/check_karma_p9b.py; the XPU cases in test_gpu_render.cpp.

#include "test_helpers.h"

#include "core/bsdf/closure.h"
#include "core/bsdf/surface.h"
#include "core/integrator/nested.h"
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

MeshDesc boxMesh(Vec3f lo, Vec3f hi) {
    MeshDesc m;
    m.points = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    const uint32_t quads[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};
    for (const auto &q : quads)
        m.indices.insert(m.indices.end(), {q[0], q[1], q[2], q[0], q[2], q[3]});
    return m;
}

MaterialId material(Scene &scene, SurfaceInputs const &in) {
    MaterialDesc d;
    d.inputs = in;
    const MaterialId id = scene.createMaterial();
    scene.updateMaterial(id, d);
    return id;
}

// Smooth glass of index `ior` whose interior absorbs to `color` over a unit depth.
SurfaceInputs absorbingGlass(float ior, Spectrum color) {
    SurfaceInputs in = standardSurfaceDefaults();
    in.set(sf::baseWeight, 0.0f);
    in.set(sf::transmissionWeight, 1.0f);
    in.set(sf::specularRoughness, 0.0f);
    in.set(sf::specularIor, ior);
    in.set(sf::transmissionColor, color);
    in.set(sf::transmissionDepth, 1.0f);
    return in;
}

Spectrum filmMean(const Film &film) {
    Spectrum sum{0.0f, 0.0f, 0.0f};
    for (size_t i = 0; i < film.colorSum.size(); ++i)
        sum += film.colorSum[i] / float(film.count[i]);
    return sum * (1.0f / float(film.colorSum.size()));
}

// Glass A (index 1.5, green absorbed to 1/2 a unit) and B (index 2, blue)
// over an emitter of radiance 1, seen straight down at normal incidence:
// red is the Fresnel loss of the interfaces, green and blue the lengths
// inside A and B (Karma's measured probe, check_karma_p9b.py). `contain`:
// B (z 0..1) inside A (z -1..2); else A (z 0..2) over B (z -1..1).
// Priorities < 0: none authored. Karma's defaults: no internal reflection.
struct Stack {
    bool contain = false;
    int priorityA = -1, priorityB = -1;
    float refractLimit = 1e9f;
};

Spectrum renderStack(const Stack &st, RenderDevice device = RenderDevice::Cpu) {
    Scene scene;
    SurfaceInputs emit = standardSurfaceDefaults();
    emit.set(sf::baseWeight, 0.0f);
    emit.set(sf::specularWeight, 0.0f);
    emit.set(sf::emissionWeight, 1.0f);
    emit.set(sf::emissionColor, spectrum(1.0f));
    MeshDesc ground = makeQuad(6.0f);
    ground.transform = translation({0.0f, 0.0f, -3.0f});
    const MeshId g = scene.createMesh();
    scene.updateMesh(g, ground);
    scene.setMeshMaterials(g, {material(scene, emit)});
    const MaterialId a = material(scene, absorbingGlass(1.5f, {1.0f, 0.5f, 1.0f}));
    const MaterialId b = material(scene, absorbingGlass(2.0f, {1.0f, 1.0f, 0.5f}));
    const MeshId ma = scene.createMesh(), mb = scene.createMesh();
    if (st.contain) {
        scene.updateMesh(ma, boxMesh({-0.8f, -0.8f, -1.0f}, {0.8f, 0.8f, 2.0f}));
        scene.updateMesh(mb, boxMesh({-0.6f, -0.6f, 0.0f}, {0.6f, 0.6f, 1.0f}));
    } else {
        scene.updateMesh(ma, boxMesh({-0.8f, -0.8f, 0.0f}, {0.8f, 0.8f, 2.0f}));
        scene.updateMesh(mb, boxMesh({-0.7f, -0.7f, -1.0f}, {0.7f, 0.7f, 1.0f}));
    }
    scene.setMeshMaterials(ma, {a});
    scene.setMeshMaterials(mb, {b});
    if (st.priorityA >= 0) {
        RayLimitOverrides o;
        o.dielectricPriority = int16_t(st.priorityA);
        scene.setMeshRayLimits(ma, o);
    }
    if (st.priorityB >= 0) {
        RayLimitOverrides o;
        o.dielectricPriority = int16_t(st.priorityB);
        scene.setMeshRayLimits(mb, o);
    }
    RayLimits d;
    d.internalReflection = false;
    scene.setObjectDefaults(d);
    scene.commit();
    Renderer renderer;
    RenderSettings s;
    s.device = device;
    s.maxSamples = 64;
    s.maxDepth = 24;
    s.clampIndirect = 0.0f;
    s.pixelFilterScale = 0.0f;
    s.limits.limit[int(LimitClass::Refract)] = st.refractLimit;
    renderer.setScene(&scene);
    renderer.setSettings(s);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 0.4f, 8, 8));
    renderer.resize(8, 8);
    renderToConvergence(renderer);
    return filmMean(renderer.film());
}

void checkStack(Spectrum got, float red, float lengthA, float lengthB) {
    INFO("got " << got.x << " " << got.y << " " << got.z);
    CHECK_THAT(got.x, WithinAbs(red, 3e-3));
    CHECK_THAT(got.y, WithinAbs(red * std::pow(0.5f, lengthA), 3e-3));
    CHECK_THAT(got.z, WithinAbs(red * std::pow(0.5f, lengthB), 3e-3));
}

} // namespace

TEST_CASE("Nested media by priority", "[nested]") {
    NestedMedia m;
    m.clear();
    CHECK(!m.falseInterface(0));
    CHECK(m.otherIndex(0) == 1.0f);
    m.enter(1, 1.5f, {0.1f, 0.0f, 0.0f}); // the liquid
    CHECK(!m.falseInterface(1));
    CHECK(!m.falseInterface(0));
    CHECK(m.falseInterface(2));
    CHECK(m.otherIndex(0) == 1.5f); // a glass of priority 0 entered from the liquid
    CHECK(m.otherIndex(1) == 1.0f); // the liquid's own level: air beyond
    m.enter(0, 1.33f, {0.0f, 0.2f, 0.0f});
    CHECK(m.count == 2);
    CHECK(m.priority[0] == 0);
    CHECK(m.absorption().y == 0.2f);
    CHECK(m.falseInterface(1));
    CHECK(m.otherIndex(0) == 1.5f);
    // Equal priorities replace each other: leaving either empties the level.
    m.enter(0, 1.7f, {0.0f, 0.0f, 0.3f});
    CHECK(m.count == 2);
    CHECK(m.ior[0] == 1.7f);
    m.leave(0);
    CHECK(m.count == 1);
    CHECK(m.absorption().x == 0.1f);
    m.leave(5); // not there: nothing
    CHECK(m.count == 1);
    // Four levels at most: a fifth below them is dropped, above them the lowest goes.
    m.enter(3, 1.1f, {});
    m.enter(4, 1.2f, {});
    m.enter(6, 1.3f, {});
    CHECK(m.count == 4);
    m.enter(9, 1.4f, {});
    CHECK(m.count == 4);
    CHECK(m.priority[3] == 6);
    m.enter(2, 1.6f, {});
    CHECK(m.count == 4);
    CHECK(m.priority[1] == 2);
    CHECK(m.priority[3] == 4);
}

// The media's ratio bends the transmission and alone reflects totally; the
// reflectance keeps the surface's own index (Karma, measured).
TEST_CASE("Dielectric lobe between nested media", "[nested][bsdf]") {
    DielectricLobe glass{GgxDistribution::make(0.0f, 0.0f), 1.5f, spectrum(1.0f), spectrum(1.0f), DielectricLobe::kBoth};
    // Entering glass (1.5) from water (1.33).
    DielectricLobe nested = glass;
    nested.etaMedia = 1.5f / 1.33f;
    const Vec3f wo = normalize(Vec3f{0.6f, 0.0f, 0.8f});
    LobeSample s;
    REQUIRE(nested.sample(wo, {0.5f, 0.5f}, 0.99f, s)); // uc past the reflection share
    REQUIRE(s.transmit);
    // Snell with the media's ratio.
    CHECK_THAT(std::sqrt(1.0f - s.wi.z * s.wi.z), WithinAbs(0.6f * 1.33f / 1.5f, 1e-5));
    // Reflectance at normal incidence: the glass's own (0.04), not water/glass (0.0036).
    float cosT = 0.0f;
    CHECK_THAT(nested.reflectance(1.0f, 1.5f, 1.5f / 1.33f, cosT), WithinAbs(0.04f, 1e-5));
    CHECK_THAT(glass.reflectance(1.0f, 1.5f, 1.5f, cosT), WithinAbs(0.04f, 1e-5));
    LobeSample t;
    REQUIRE(nested.sample({0.0f, 0.0f, 1.0f}, {0.5f, 0.5f}, 0.5f, t));
    CHECK(t.transmit);
    // (1 - F) / (1 - pR) / eta^2 with pR = F = 0.04: 1 / eta_media^2.
    CHECK_THAT(t.fcos.x, WithinAbs(1.0f / sqr(1.5f / 1.33f), 1e-4));
    // Leaving glass into a denser medium (index 2): seen from inside the
    // glass at 60 degrees its own index reflects totally, the media's
    // ratio (2 / 1.5) transmits.
    DielectricLobe inside{GgxDistribution::make(0.0f, 0.0f), 1.0f / 1.5f, spectrum(1.0f), spectrum(1.0f),
                          DielectricLobe::kBoth};
    const Vec3f steep{std::sin(1.0472f), 0.0f, std::cos(1.0472f)};
    LobeSample tir;
    REQUIRE(inside.sample(steep, {0.5f, 0.5f}, 0.999f, tir));
    CHECK(!tir.transmit);
    inside.etaMedia = 2.0f / 1.5f;
    LobeSample through;
    REQUIRE(inside.sample(steep, {0.5f, 0.5f}, 0.999f, through));
    CHECK(through.transmit);
    CHECK_THAT(std::sqrt(1.0f - through.wi.z * through.wi.z), WithinAbs(std::sin(1.0472f) * 1.5f / 2.0f, 1e-4));
}

// Karma's measured rules (check_karma_p9b.py): equal priorities nest
// nothing (each surface against air, leaving any object leaves no medium);
// a lower priority's surfaces inside a higher one are passed through, the
// medium inside is the highest priority's, the reflectance stays each
// surface's own (normal incidence: 0.04 for A, 1/9 for B; Karma's default
// no internal reflection makes exits lossless).
TEST_CASE("Nested dielectrics: Karma's priorities", "[render][nested]") {
    const float tA = 0.96f, tB = 8.0f / 9.0f;
    SECTION("equal priorities") {
        checkStack(renderStack({false, -1, -1}), tA * tB, 1.0f, 1.0f);
        checkStack(renderStack({false, 3, 3}), tA * tB, 1.0f, 1.0f);
        checkStack(renderStack({true, -1, -1}), tA * tB, 1.0f, 1.0f);
    }
    SECTION("overlap") {
        checkStack(renderStack({false, 0, 1}), tA, 2.0f, 1.0f); // A wins: B's top is passed
        checkStack(renderStack({false, 1, 0}), tA * tB, 1.0f, 2.0f); // B wins: A's bottom is passed
    }
    SECTION("contained") {
        checkStack(renderStack({true, 0, 1}), tA, 3.0f, 0.0f); // B is not there
        checkStack(renderStack({true, 1, 0}), tA * tB, 2.0f, 1.0f);
        checkStack(renderStack({true, 2, 1}), tA * tB, 2.0f, 1.0f);
    }
    SECTION("refraction limits") {
        // Passed surfaces are not counted; past the limit a thin-wall
        // pass-through leaves or enters the medium and tints by the
        // transmission colour (one unit of absorption each).
        checkStack(renderStack({true, 0, 1, 1.0f}), tA, 4.0f, 0.0f);
        checkStack(renderStack({true, -1, -1, 2.0f}), tA * tB, 2.0f, 2.0f);
        checkStack(renderStack({true, 1, 0, 2.0f}), tA * tB, 3.0f, 2.0f);
    }
}

// Karma's fake caustics tint by the transmission colour per crossing, also
// for absorbing glass (measured: check_karma_p9b.py shadow_*), and ignore
// priorities.
TEST_CASE("Karma's fake caustics of absorbing glass", "[nested][caustics]") {
    const ShadingGeometry g{{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, false, 0.5f};
    ClosureList cl;
    buildClosure(absorbingGlass(1.5f, {1.0f, 0.5f, 1.0f}), g, {0.0f, 0.0f, 1.0f},
                 ClosureContext{&bsdfTables(), kXyzToRec709Grey}, cl);
    CHECK(cl.absorptionColor.y == 0.5f);
    ObjectShading karma;
    karma.fake.karma = true;
    const Spectrum t = fakeCausticFactor(cl, {0.0f, 0.0f, 1.0f}, karma, bsdfTables());
    CHECK_THAT(t.y / t.x, WithinAbs(0.5f, 1e-5));
    ObjectShading physical; // Nova's physical factor: the interfaces only
    const Spectrum n = fakeCausticFactor(cl, {0.0f, 0.0f, 1.0f}, physical, bsdfTables());
    CHECK_THAT(n.y / n.x, WithinAbs(1.0f, 1e-5));
}
