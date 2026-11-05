// Ray state in shading (docs/design/M4.md 3.1, M4.0): the globals RayType
// (Karma's ray classes), RayDepth, HitDist and Backface, and the ray type
// seen by opacity programs on shadow rays.
//
// The class probe: a quad fills the view of an orthographic camera inside a
// large unlit sphere whose emission is 0.1 (RayType + 1). Each quad material
// scatters with a single event class, so the image divided by the same
// render with emission 1 is exactly 0.1 (class + 1): the paths are the same,
// only the emission seen at their end differs.

#include "test_helpers.h"

#include "core/bsdf/closure.h"
#include "core/scene/scene.h"
#include "core/shading/compiler.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <functional>
#include <vector>

using namespace nova;
using namespace nova::test;
namespace sh = nova::shading;
namespace sf = nova::surface_field;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// An unlit material whose emission is `emission(builder)` (3 wide or a splat).
MaterialDesc unlitProgram(const std::function<sh::Ref(sh::IrBuilder &)> &emission) {
    sh::IrGraph g;
    g.model = SurfaceModel::Unlit;
    sh::IrBuilder b(g);
    b.output(sf::emissionColor, 3, emission(b));
    MaterialDesc d;
    d.inputs = unlitDefaults();
    d.program = sh::compile(g).program;
    REQUIRE(d.program);
    return d;
}

MeshDesc analyticSphere(float radius) {
    MeshDesc m;
    m.shape = MeshShape::Sphere;
    m.points = {{0.0f, 0.0f, 0.0f}};
    m.sphereRadius = {radius};
    return m;
}

// Mean green value of a size x size render of `scene` (orthographic, from z = 5).
double renderMean(Scene &scene, int size, uint32_t spp, float halfWidth, uint32_t maxDepth = 2) {
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = spp;
    settings.maxDepth = maxDepth;
    settings.clampIndirect = 0.0f;
    settings.pixelFilterScale = 0.0f;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, halfWidth, size, size));
    renderer.resize(size, size);
    renderToConvergence(renderer);
    double sum = 0.0;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            sum += pixelMean(renderer.film(), x, y).y;
    return sum / double(size * size);
}

// The quad (or nothing) inside the probe sphere; `emission` is the sphere's.
double probe(const SurfaceInputs *quadMaterial, const MaterialDesc &emission) {
    Scene scene;
    if (quadMaterial) {
        const MeshId quad = scene.createMesh();
        scene.updateMesh(quad, makeQuad(1.0f));
        MaterialDesc d;
        d.inputs = *quadMaterial;
        const MaterialId m = scene.createMaterial();
        scene.updateMaterial(m, d);
        scene.setMeshMaterials(quad, {m});
    }
    const MeshId sphere = scene.createMesh();
    scene.updateMesh(sphere, analyticSphere(10.0f)); // inside the camera's far plane (z = -15)
    const MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, emission);
    scene.setMeshMaterials(sphere, {m});
    scene.commit();
    return renderMean(scene, 8, 16, 0.5f);
}

} // namespace

TEST_CASE("Rays carry the class of the event that spawned them", "[render][raystate]") {
    const MaterialDesc typed = unlitProgram([](sh::IrBuilder &b) {
        return b.mul(b.add(b.global(sh::Global::RayType), b.constant(1.0f)), b.constant(0.1f));
    });
    MaterialDesc one;
    one.inputs = unlitDefaults();

    struct Case {
        const char *name;
        SurfaceInputs in;
        RayClass expected;
    };
    std::vector<Case> cases;
    cases.push_back({"Lambert", lambertDefaults(spectrum(1.0f)), RayClass::Diffuse});
    SurfaceInputs mirror = standardSurfaceDefaults();
    mirror.set(sf::metalness, 1.0f);
    mirror.set(sf::baseColor, spectrum(1.0f));
    mirror.set(sf::specularRoughness, 0.0f);
    cases.push_back({"mirror", mirror, RayClass::Reflect});
    SurfaceInputs rough = mirror;
    rough.set(sf::specularRoughness, 0.6f);
    cases.push_back({"rough metal", rough, RayClass::Reflect});
    SurfaceInputs matched = standardSurfaceDefaults();
    matched.set(sf::transmissionWeight, 1.0f);
    matched.set(sf::specularIor, 1.0f);
    matched.set(sf::specularRoughness, 0.0f);
    cases.push_back({"index-matched glass", matched, RayClass::Refract});
    SurfaceInputs sheen = standardSurfaceDefaults();
    sheen.set(sf::baseWeight, 0.0f);
    sheen.set(sf::specularWeight, 0.0f);
    sheen.set(sf::sheenWeight, 1.0f);
    sheen.set(sf::sheenColor, spectrum(1.0f));
    cases.push_back({"sheen (Karma: diffuse)", sheen, RayClass::Diffuse});
    SurfaceInputs sss = standardSurfaceDefaults();
    sss.set(sf::specularWeight, 0.0f);
    sss.set(sf::subsurfaceWeight, 1.0f);
    sss.set(sf::subsurfaceColor, spectrum(1.0f));
    sss.set(sf::thinWalled, 1.0f);
    cases.push_back({"thin-walled subsurface", sss, RayClass::Sss});

    for (const Case &c : cases) {
        const double a = probe(&c.in, typed), b = probe(&c.in, one);
        INFO(c.name << ": " << a << " / " << b);
        REQUIRE(b > 0.01);
        CHECK_THAT(a / b, WithinAbs(0.1 * (double(c.expected) + 1.0), 1e-4));
    }
    // Seen directly: camera rays.
    CHECK_THAT(probe(nullptr, typed), WithinAbs(0.1, 1e-6));
}

TEST_CASE("Ray depth, hit distance and back faces", "[render][raystate]") {
    // RayDepth: the probe sphere seen directly (0) and through a Lambert quad (1).
    const MaterialDesc depth = unlitProgram([](sh::IrBuilder &b) {
        return b.mul(b.add(b.global(sh::Global::RayDepth), b.constant(1.0f)), b.constant(0.1f));
    });
    MaterialDesc one;
    one.inputs = unlitDefaults();
    const SurfaceInputs lambert = lambertDefaults(spectrum(1.0f));
    CHECK_THAT(probe(nullptr, depth), WithinAbs(0.1, 1e-6));
    CHECK_THAT(probe(&lambert, depth) / probe(&lambert, one), WithinAbs(0.2, 1e-4));

    // HitDist: camera rays start at z = 5, the quad lies at z = 0.
    // Backface: the camera looks at the quad's front, then at its back.
    auto quadValue = [](const MaterialDesc &material, bool flipped) {
        Scene scene;
        MeshDesc quad = makeQuad(1.0f);
        if (flipped)
            quad.indices = {0, 2, 1, 0, 3, 2};
        const MeshId id = scene.createMesh();
        scene.updateMesh(id, quad);
        const MaterialId m = scene.createMaterial();
        scene.updateMaterial(m, material);
        scene.setMeshMaterials(id, {m});
        scene.commit();
        return renderMean(scene, 4, 1, 0.5f);
    };
    const MaterialDesc dist = unlitProgram([](sh::IrBuilder &b) { return b.global(sh::Global::HitDist); });
    CHECK_THAT(quadValue(dist, false), WithinAbs(5.0, 1e-4));
    const MaterialDesc back = unlitProgram([](sh::IrBuilder &b) { return b.global(sh::Global::Backface); });
    CHECK(quadValue(back, false) == 0.0);
    CHECK(quadValue(back, true) == 1.0);
}

TEST_CASE("Opacity programs see shadow rays", "[render][raystate][opacity]") {
    // Ground z = 0 under a distant light travelling along (-1, 0, -1); a
    // blocker at z = 1, x in [2, 3] (outside the view) shades x in [1, 2].
    // Its opacity is a ray switch: `shadow` for shadow rays, 0 otherwise.
    auto shadowRatio = [](float shadowOpacity) {
        Scene scene;
        const MeshId ground = scene.createMesh();
        scene.updateMesh(ground, makeQuad(4.0f));
        const MeshId blocker = scene.createMesh();
        MeshDesc plane;
        plane.points = {{2.0f, -4.0f, 1.0f}, {3.0f, -4.0f, 1.0f}, {3.0f, 4.0f, 1.0f}, {2.0f, 4.0f, 1.0f}};
        plane.indices = {0, 1, 2, 0, 2, 3};
        scene.updateMesh(blocker, plane);
        sh::IrGraph g;
        g.model = SurfaceModel::Lambert;
        sh::IrBuilder b(g);
        const sh::Ref shadow =
            b.binary(sh::Op::Equal, b.global(sh::Global::RayType), b.constant(float(RayClass::Shadow)));
        b.output(sf::opacity, 3, b.ternary(sh::Op::Select, shadow, b.constant(shadowOpacity), b.constant(0.0f)));
        MaterialDesc d;
        d.inputs = lambertDefaults(spectrum(0.5f));
        d.program = sh::compile(g).program;
        REQUIRE(d.program);
        REQUIRE(d.program->opacity);
        const MaterialId m = scene.createMaterial();
        scene.updateMaterial(m, d);
        scene.setMeshMaterials(blocker, {m});
        LightDesc sun;
        sun.type = LightType::Distant;
        sun.angleDeg = 0.0f;
        const double c = std::sqrt(0.5), sn = std::sqrt(0.5);
        sun.transform.m[0][0] = c;
        sun.transform.m[0][2] = -sn;
        sun.transform.m[2][0] = sn;
        sun.transform.m[2][2] = c;
        scene.updateLight(scene.createLight(), sun);
        scene.commit();
        Renderer renderer;
        RenderSettings settings;
        settings.maxSamples = 512;
        settings.maxDepth = 1;
        settings.pixelFilterScale = 0.0f;
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        const int size = 16; // x from -2 to 2
        renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 2.0f, size, size));
        renderer.resize(size, size);
        renderToConvergence(renderer);
        double lit = 0.0, shaded = 0.0;
        int nl = 0, ns = 0;
        for (int y = 2; y < 14; ++y)
            for (int x = 0; x < size; ++x) {
                const float wx = (float(x) + 0.5f) / 4.0f - 2.0f;
                const double v = pixelMean(renderer.film(), x, y).y;
                if (wx > 1.1f && wx < 1.9f) {
                    shaded += v;
                    ++ns;
                } else if (wx < 0.9f) {
                    lit += v;
                    ++nl;
                }
            }
        return (shaded / ns) / (lit / nl);
    };
    CHECK_THAT(shadowRatio(0.5f), WithinAbs(0.5, 0.015));
    CHECK_THAT(shadowRatio(0.0f), WithinAbs(1.0, 1e-6));
    CHECK_THAT(shadowRatio(1.0f), WithinAbs(0.0, 1e-6));
}
