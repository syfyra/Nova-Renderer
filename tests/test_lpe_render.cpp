// M6.2: light path expressions in the renderer (docs/design/M6.md D4-D6).
// Components that partition the paths sum to the beauty per pixel (with
// and without clamping), light groups split it, LPEs that tell no
// scattering classes apart leave the beauty bit-identical, components agree
// across the light strategies and with the light multipliers, and tags and
// labels select their objects, lights and lobes. M6.3: shadow;, unoccluded;,
// A and B.

#include "test_helpers.h"

#include "core/film/lpe_compile.h"
#include "core/integrator/renderer.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;
namespace sf = nova::surface_field;

namespace {

MaterialDesc material(SurfaceInputs in, std::string tag = {}) {
    MaterialDesc m;
    m.inputs = in;
    m.lpeTag = std::move(tag);
    return m;
}

// A floor (material tag "floor"), a coated plastic ball with sheen (object
// tag "ball"), a rough glass ball, an emissive ball (object tag "lamp");
// a rect light "key", a camera-visible dome "env" and an untagged distant
// light. Seen from above; the dome shows around the floor.
void buildScene(Scene &scene) {
    MeshDesc floor = makeQuad(3.0f);
    const MeshId floorId = scene.createMesh();
    scene.updateMesh(floorId, floor);
    const MaterialId floorMat = scene.createMaterial();
    scene.updateMaterial(floorMat, material(lambertDefaults(spectrum(0.5f)), "floor"));
    scene.setMeshMaterials(floorId, {floorMat});

    MeshDesc ball = makeSphere(1.0f, 48, 24);
    ball.transform = translation({-1.3f, 0.0f, 1.0f});
    const MeshId ballId = scene.createMesh();
    scene.updateMesh(ballId, ball);
    SurfaceInputs plastic = standardSurfaceDefaults();
    plastic.set(sf::baseColor, {0.7f, 0.3f, 0.2f});
    plastic.set(sf::specularRoughness, 0.3f);
    plastic.set(sf::coatWeight, 0.6f);
    plastic.set(sf::sheenWeight, 0.5f);
    const MaterialId ballMat = scene.createMaterial();
    scene.updateMaterial(ballMat, material(plastic, "unused"));
    scene.setMeshMaterials(ballId, {ballMat});
    scene.setMeshLpeTag(ballId, "ball");

    MeshDesc glass = makeSphere(0.8f, 48, 24);
    glass.transform = translation({1.3f, 0.0f, 0.8f});
    const MeshId glassId = scene.createMesh();
    scene.updateMesh(glassId, glass);
    SurfaceInputs glassIn = standardSurfaceDefaults();
    glassIn.set(sf::transmissionWeight, 1.0f);
    glassIn.set(sf::specularRoughness, 0.15f);
    const MaterialId glassMat = scene.createMaterial();
    scene.updateMaterial(glassMat, material(glassIn));
    scene.setMeshMaterials(glassId, {glassMat});

    MeshDesc lamp = makeSphere(0.3f, 24, 12);
    lamp.transform = translation({0.0f, 1.6f, 0.5f});
    const MeshId lampId = scene.createMesh();
    scene.updateMesh(lampId, lamp);
    SurfaceInputs unlit = unlitDefaults();
    unlit.set(sf::emissionColor, {3.0f, 2.0f, 1.0f});
    const MaterialId lampMat = scene.createMaterial();
    scene.updateMaterial(lampMat, material(unlit));
    scene.setMeshMaterials(lampId, {lampMat});
    scene.setMeshLpeTag(lampId, "lamp");

    LightDesc key;
    key.type = LightType::Rect;
    key.intensity = 6.0f;
    key.width = key.height = 1.5f;
    key.transform = translation({0.5f, -1.0f, 4.0f}); // emits down -Z
    key.lpeTag = "key";
    scene.updateLight(scene.createLight(), key);
    LightDesc env;
    env.type = LightType::Dome;
    env.intensity = 0.4f;
    env.cameraVisible = true;
    env.lpeTag = "env";
    scene.updateLight(scene.createLight(), env);
    LightDesc fill;
    fill.type = LightType::Distant;
    fill.intensity = 1.5f;
    fill.angleDeg = 5.0f;
    fill.transform = Mat4::identity(); // shines down -Z
    scene.updateLight(scene.createLight(), fill);
    scene.commit();
}

constexpr int kSize = 24;

// Renders the scene with `expressions` (none: no LPE program).
Film render(const Scene &scene, const std::vector<std::string> &expressions, uint32_t spp, float clamp = 10.0f,
            LightStrategy strategy = LightStrategy::MIS, uint32_t maxDepth = 6) {
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = spp;
    settings.maxDepth = maxDepth;
    settings.clampIndirect = clamp;
    settings.lightStrategy = strategy;
    if (!expressions.empty()) {
        auto compiled = std::make_shared<lpe::Compiled>(lpe::compile(expressions));
        for (size_t i = 0; i < expressions.size(); ++i) {
            INFO(expressions[i]);
            REQUIRE(compiled->errors[i].empty());
        }
        settings.lpe = compiled;
    }
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makePerspectiveCamera({0.0f, 0.0f, 9.0f}, 1.0, 1.0, kSize, kSize));
    renderer.resize(kSize, kSize);
    renderToConvergence(renderer);
    return renderer.film();
}

// Pixels where the outputs `first..first+count` do not sum to the beauty.
int partitionFailures(const Film &film, uint32_t first, uint32_t count) {
    int failures = 0;
    for (size_t i = 0; i < film.pixelCount(); ++i) {
        Spectrum sum{0.0f, 0.0f, 0.0f};
        for (uint32_t o = first; o < first + count; ++o)
            sum += film.lpe(o)[i];
        const Spectrum b = film.colorSum[i];
        const float tol = 1e-4f * std::fmax(maxComponent(absolute(b)), float(film.samples));
        if (maxComponent(absolute(sum - b)) > tol) {
            if (failures == 0)
                UNSCOPED_INFO("pixel " << i << ": components " << sum.x << " " << sum.y << " " << sum.z
                                       << ", beauty " << b.x << " " << b.y << " " << b.z);
            ++failures;
        }
    }
    return failures;
}

// The mean of an output (or of the beauty, output < 0) over the film.
Spectrum filmMean(const Film &film, int output) {
    Spectrum sum{0.0f, 0.0f, 0.0f};
    for (size_t i = 0; i < film.pixelCount(); ++i)
        sum += output < 0 ? film.colorSum[i] : film.lpe(uint32_t(output))[i];
    return sum / float(film.pixelCount() * film.samples);
}

const std::vector<std::string> kPartition = {
    "CL",           "CO",           "C<RD>[LO]",    "C<RG>[LO]",    "C<TD>[LO]",
    "C<TG>[LO]",    "C<RD>.+[LO]",  "C<RG>.+[LO]",  "C<TD>.+[LO]",  "C<TG>.+[LO]",
};

} // namespace

TEST_CASE("LPE components that partition the paths sum to the beauty", "[render][lpe]") {
    Scene scene;
    buildScene(scene);
    for (const float clamp : {0.0f, 10.0f, 0.3f}) {
        INFO("clamp " << clamp);
        const Film film = render(scene, kPartition, 16, clamp);
        REQUIRE(film.lpeOutputs == kPartition.size());
        CHECK(partitionFailures(film, 0, uint32_t(kPartition.size())) == 0);
        // Every family is present in this scene.
        for (size_t o = 0; o < kPartition.size(); ++o) {
            if (kPartition[o] == "C<TD>[LO]" || kPartition[o] == "C<TD>.+[LO]" || kPartition[o] == "C<TG>[LO]")
                continue; // no translucency or SSS; light behind glass is inside the ball
            INFO(kPartition[o]);
            CHECK(luminance(filmMean(film, int(o))) > 0.0f);
        }
    }
}

TEST_CASE("Light groups split the beauty", "[render][lpe]") {
    Scene scene;
    buildScene(scene);
    const std::vector<std::string> groups = {"C.*<L.'key'>", "C.*<L.'env'>", "C.*<L.[^'key''env']>", "C.*O"};
    const Film film = render(scene, groups, 16);
    CHECK(partitionFailures(film, 0, 4) == 0);
    for (int o = 0; o < 4; ++o) {
        INFO(groups[size_t(o)]);
        CHECK(luminance(filmMean(film, o)) > 0.0f);
    }
}

TEST_CASE("LPEs without scattering classes leave the beauty bit-identical", "[render][lpe]") {
    Scene scene;
    buildScene(scene);
    const Film plain = render(scene, {}, 8);
    for (const std::vector<std::string> &set :
         {std::vector<std::string>{"C.*[LO]"}, std::vector<std::string>{"C.*<L.'key'>", "C.*<L.'env'>", "C.*O"}}) {
        INFO(set.front());
        const Film film = render(scene, set, 8);
        int differ = 0;
        for (size_t i = 0; i < film.pixelCount(); ++i)
            differ += !(film.colorSum[i].x == plain.colorSum[i].x && film.colorSum[i].y == plain.colorSum[i].y &&
                        film.colorSum[i].z == plain.colorSum[i].z);
        CHECK(differ == 0);
    }
    // The beauty LPE itself is the beauty.
    const Film beauty = render(scene, {"C.*[LO]"}, 8);
    CHECK(partitionFailures(beauty, 0, 1) == 0);
}

TEST_CASE("LPE components agree across light strategies and with the light multipliers", "[render][lpe][mis]") {
    // Direct light from a large rect light on a plastic ball: the diffuse
    // and glossy components equal the beauty with the light's specular or
    // diffuse multiplier at 0, under every strategy (light samples split
    // exactly, BSDF samples by the class choice).
    Scene scene;
    const MeshId ball = scene.createMesh();
    scene.updateMesh(ball, makeSphere(1.0f, 96, 48));
    SurfaceInputs plastic = standardSurfaceDefaults();
    plastic.set(sf::specularRoughness, 0.35f);
    const MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, material(plastic));
    scene.setMeshMaterials(ball, {m});
    LightDesc rect; // (constant domes are never light-sampled)
    rect.type = LightType::Rect;
    rect.width = rect.height = 20.0f;
    rect.transform = translation({0.0f, 0.0f, 3.0f}); // emits down -Z, onto the ball
    const LightId light = scene.createLight();
    scene.updateLight(light, rect);
    scene.commit();

    const auto renderBall = [&](const std::vector<std::string> &expressions, LightStrategy strategy) {
        Renderer renderer;
        RenderSettings settings;
        settings.maxSamples = 64;
        settings.maxDepth = 1;
        settings.clampIndirect = 0.0f;
        settings.lightStrategy = strategy;
        if (!expressions.empty())
            settings.lpe = std::make_shared<lpe::Compiled>(lpe::compile(expressions));
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, 32, 32));
        renderer.resize(32, 32);
        renderToConvergence(renderer);
        return renderer.film();
    };
    const auto ballMean = [](const Film &film, int output) {
        double sum = 0.0;
        int n = 0;
        for (int y = 0; y < film.height; ++y)
            for (int x = 0; x < film.width; ++x) {
                const float px = (x + 0.5f) / film.width * 2.0f - 1.0f, py = (y + 0.5f) / film.height * 2.0f - 1.0f;
                if (px * px + py * py > 0.8f * 0.8f)
                    continue;
                const size_t i = size_t(y) * size_t(film.width) + size_t(x);
                sum += (output < 0 ? film.colorSum[i] : film.lpe(uint32_t(output))[i]).y / film.samples;
                ++n;
            }
        return sum / n;
    };

    const Film full = renderBall({}, LightStrategy::LightOnly);
    rect.specular = 0.0f;
    scene.updateLight(light, rect);
    scene.commit();
    const double diffuse = ballMean(renderBall({}, LightStrategy::LightOnly), -1);
    rect.specular = 1.0f;
    rect.diffuse = 0.0f;
    scene.updateLight(light, rect);
    scene.commit();
    const double glossy = ballMean(renderBall({}, LightStrategy::LightOnly), -1);
    rect.diffuse = 1.0f;
    scene.updateLight(light, rect);
    scene.commit();
    REQUIRE(diffuse > 0.1);
    REQUIRE(glossy > 0.01);
    CHECK_THAT(diffuse + glossy, WithinAbs(ballMean(full, -1), 1e-4));

    for (const LightStrategy strategy : {LightStrategy::LightOnly, LightStrategy::MIS, LightStrategy::BsdfOnly}) {
        INFO("strategy " << int(strategy));
        const Film film = renderBall({"C<RD>L", "C<RG>L"}, strategy);
        const double tol = strategy == LightStrategy::LightOnly ? 1e-4 : 0.01;
        CHECK_THAT(ballMean(film, 0), WithinAbs(diffuse, tol * std::fmax(diffuse, 1.0)));
        CHECK_THAT(ballMean(film, 1), WithinAbs(glossy, tol * std::fmax(diffuse, 1.0)));
        CHECK(partitionFailures(film, 0, 2) == 0);
    }
}

TEST_CASE("LPE tags and labels select objects, materials and lobes", "[render][lpe]") {
    Scene scene;
    buildScene(scene);
    const std::vector<std::string> expressions = {
        "C<..'ball'>.*[LO]",  // 0: the ball (object tag)
        "C<..'floor'>.*[LO]", // 1: the floor (material tag)
        "C<..'unused'>.*[LO]",// 2: the ball's material tag: the object's own tag wins
        "C<O.'lamp'>",        // 3: the lamp's emission
        "C<RG.'coat'>.*[LO]", // 4: the ball's coat
        "C<RD.'sheen'>.*[LO]",// 5: no lobe carries a sheen label (as in Karma)
        "C<RG>.*[LO]",        // 6: all glossy reflection
    };
    const Film film = render(scene, expressions, 16);
    CHECK(luminance(filmMean(film, 0)) > 0.0f);
    CHECK(luminance(filmMean(film, 1)) > 0.0f);
    CHECK(luminance(filmMean(film, 2)) == 0.0f);
    CHECK(luminance(filmMean(film, 3)) > 0.0f);
    CHECK(luminance(filmMean(film, 4)) > 0.0f);
    CHECK(luminance(filmMean(film, 5)) == 0.0f);
    CHECK(luminance(filmMean(film, 4)) < luminance(filmMean(film, 6)));
}

// --- M6.3: prefixes and the A and B events -----------------------------------

namespace {

struct LpeRender {
    std::shared_ptr<const lpe::Compiled> program;
    Film film;
    // Mean of an output over pixel columns [x0, x1) (all rows), and of
    // shadow;'s occluded fraction there.
    Spectrum mean(uint32_t output, int x0, int x1) const {
        Spectrum s{0.0f, 0.0f, 0.0f};
        for (int y = 0; y < film.height; ++y)
            for (int x = x0; x < x1; ++x)
                s += film.lpe(output)[size_t(y) * size_t(film.width) + size_t(x)];
        return s / float(film.height * (x1 - x0) * int(film.samples));
    }
    Spectrum occluded(uint32_t output, int x0, int x1) const {
        const int32_t slot = program->shadowSlot(output);
        REQUIRE(slot >= 0);
        Spectrum lit{0.0f, 0.0f, 0.0f}, all{0.0f, 0.0f, 0.0f};
        for (int y = 0; y < film.height; ++y)
            for (int x = x0; x < x1; ++x) {
                const size_t i = size_t(y) * size_t(film.width) + size_t(x);
                lit += film.lpeLitSum(uint32_t(slot))[i];
                all += film.lpe(output)[i];
            }
        return Spectrum{1.0f - lit.x / all.x, 1.0f - lit.y / all.y, 1.0f - lit.z / all.z};
    }
};

LpeRender renderLpes(const Scene &scene, const Camera &camera, int size, const std::vector<std::string> &expressions,
                     uint32_t spp, uint32_t maxDepth, LightStrategy strategy = LightStrategy::MIS) {
    LpeRender r;
    r.program = std::make_shared<lpe::Compiled>(lpe::compile(expressions));
    for (size_t i = 0; i < expressions.size(); ++i) {
        INFO(expressions[i]);
        REQUIRE(r.program->errors[i].empty());
    }
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = spp;
    settings.maxDepth = maxDepth;
    settings.clampIndirect = 0.0f;
    settings.lightStrategy = strategy;
    settings.lpe = r.program;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(camera);
    renderer.resize(size, size);
    renderToConvergence(renderer);
    r.film = renderer.film();
    return r;
}

MeshId addQuad(Scene &scene, float halfSize, Vec3f at, const MaterialDesc &m) {
    MeshDesc quad = makeQuad(halfSize);
    quad.transform = translation(at);
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, quad);
    const MaterialId mat = scene.createMaterial();
    scene.updateMaterial(mat, m);
    scene.setMeshMaterials(id, {mat});
    return id;
}

} // namespace

TEST_CASE("shadow; and unoccluded; split direct light by occlusion", "[render][lpe]") {
    // A delta distant light from 45 degrees over +X; a quad at height 2 shades
    // the floor for x > 0 (out of view itself).
    Scene scene;
    addQuad(scene, 20.0f, {0.0f, 0.0f, 0.0f}, material(lambertDefaults(spectrum(0.5f))));
    addQuad(scene, 4.0f, {6.0f, 0.0f, 2.0f}, material(lambertDefaults(spectrum(0.5f))));
    LightDesc sun;
    sun.type = LightType::Distant;
    sun.angleDeg = 0.0f;
    const float c = std::sqrt(0.5f);
    sun.transform = Mat4::identity(); // local +Z (towards the light) to (c, 0, c)
    sun.transform.m[0][0] = c;
    sun.transform.m[0][2] = -c;
    sun.transform.m[2][0] = c;
    sun.transform.m[2][2] = c;
    scene.updateLight(scene.createLight(), sun);
    scene.commit();
    REQUIRE(scene.lights()[0].delta);
    REQUIRE_THAT(scene.lights()[0].direction.x, WithinAbs(c, 1e-5));

    const int size = 16;
    const LpeRender r =
        renderLpes(scene, makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, size, size), size,
                   {"C<RD>L", "unoccluded;C<RD>L", "shadow;C<RD>L", "unoccluded;C<RG>L"}, 4, 1);
    const float lit = 0.5f / kPi * c; // albedo / pi * cos 45
    // Lit columns (x < 0) and shaded ones, away from the edge's filter.
    CHECK_THAT(r.mean(0, 0, 5).y, WithinAbs(lit, 1e-4));
    CHECK_THAT(r.mean(1, 0, 5).y, WithinAbs(lit, 1e-4));
    CHECK_THAT(r.occluded(2, 0, 5).y, WithinAbs(0.0, 1e-5));
    CHECK_THAT(r.mean(0, 11, 16).y, WithinAbs(0.0, 1e-6));
    CHECK_THAT(r.mean(1, 11, 16).y, WithinAbs(lit, 1e-4));
    CHECK_THAT(r.occluded(2, 11, 16).y, WithinAbs(1.0, 1e-5));
    CHECK(r.mean(3, 0, 16).y == 0.0f); // no glossy lobe
}

TEST_CASE("unoccluded; covers lights that light sampling never picks", "[render][lpe]") {
    // A constant dome over a floor: BSDF samples alone reach it. Without an
    // occluder the unoccluded light equals the lit one (shadow; is 0); with a
    // roof over the camera it is unchanged while the lit light is gone.
    for (const bool roof : {false, true}) {
        INFO("roof " << roof);
        Scene scene;
        addQuad(scene, 20.0f, {0.0f, 0.0f, 0.0f}, material(lambertDefaults(spectrum(0.5f))));
        if (roof)
            addQuad(scene, 400.0f, {0.0f, 0.0f, 2.0f}, material(lambertDefaults(spectrum(0.5f)))); // gaps: sin^2 < 3e-5
        LightDesc dome;
        dome.type = LightType::Dome;
        scene.updateLight(scene.createLight(), dome);
        scene.commit();
        REQUIRE(!(scene.lightSampler().pmf[0] > 0.0f));
        const LpeRender r = renderLpes(scene, makeOrthoCamera({0.0f, 0.0f, 1.0f}, 1.0f, 16, 16), 16,
                                       {"C<RD>L", "unoccluded;C<RD>L", "shadow;C<RD>L"}, 64, 1);
        CHECK_THAT(r.mean(0, 0, 16).y, WithinAbs(roof ? 0.0 : 0.5, 0.01));
        CHECK_THAT(r.mean(1, 0, 16).y, WithinAbs(0.5, 0.01));
        CHECK_THAT(r.occluded(2, 0, 16).y, WithinAbs(roof ? 1.0 : 0.0, roof ? 1e-3 : 1e-4));
    }
}

TEST_CASE("unoccluded; light sums match the beauty without occluders", "[render][lpe][mis]") {
    // A plastic ball under a large rect light: nothing is shadowed, so the
    // weight-1 light samples agree with the beauty's components for every
    // strategy (BSDF-only included: the light samples feed them alone).
    Scene scene;
    const MeshId ball = scene.createMesh();
    scene.updateMesh(ball, makeSphere(1.0f, 96, 48));
    SurfaceInputs plastic = standardSurfaceDefaults();
    plastic.set(sf::specularRoughness, 0.35f);
    const MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, material(plastic));
    scene.setMeshMaterials(ball, {m});
    LightDesc rect;
    rect.type = LightType::Rect;
    rect.width = rect.height = 20.0f;
    rect.transform = translation({0.0f, 0.0f, 3.0f});
    scene.updateLight(scene.createLight(), rect);
    scene.commit();
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, 32, 32);
    const LpeRender ref = renderLpes(scene, camera, 32, {"C<RD>L", "C<RG>L"}, 64, 1, LightStrategy::LightOnly);
    for (const LightStrategy strategy : {LightStrategy::MIS, LightStrategy::LightOnly, LightStrategy::BsdfOnly}) {
        INFO("strategy " << int(strategy));
        const LpeRender r = renderLpes(scene, camera, 32, {"unoccluded;C<RD>L", "unoccluded;C<RG>L", "shadow;C<RD>L"},
                                       64, 1, strategy);
        CHECK_THAT(r.mean(0, 8, 24).y, WithinAbs(ref.mean(0, 8, 24).y, 1e-4));
        CHECK_THAT(r.mean(1, 8, 24).y, WithinAbs(ref.mean(1, 8, 24).y, 1e-4));
        CHECK_THAT(r.occluded(2, 8, 24).y, WithinAbs(0.0, 1e-5));
    }
}

TEST_CASE("unoccluded; takes treated emitters' light samples, not their BSDF hits", "[render][lpe][mis][emitters]") {
    // An emissive quad treated as a light over a floor, seen from below it:
    // nothing is shadowed, so unoccluded;C<RD>L is the light samples' weight-1
    // estimate (the light-only render's C<RD>L, the same samples). BSDF
    // samples that hit the quad are MIS-weighted against those samples and
    // must not feed it as well (M6 D7; it read 10% high).
    Scene scene;
    addQuad(scene, 20.0f, {0.0f, 0.0f, 0.0f}, material(lambertDefaults(spectrum(0.5f))));
    SurfaceInputs glow = standardSurfaceDefaults();
    glow.set(sf::baseWeight, 0.0f);
    glow.set(sf::specularWeight, 0.0f);
    glow.set(sf::emissionWeight, 1.0f);
    glow.set(sf::emissionColor, Spectrum{2.0f, 1.0f, 0.5f});
    const MeshId lamp = addQuad(scene, 0.5f, {0.0f, 0.0f, 1.0f}, material(glow));
    MeshLightSource treat;
    treat.mode = 1;
    scene.setMeshLightSource(lamp, treat);
    scene.commit();
    REQUIRE(scene.anyEmitters());
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 0.5f}, 1.0f, 16, 16);
    const LpeRender ref = renderLpes(scene, camera, 16, {"C<RD>L"}, 64, 1, LightStrategy::LightOnly);
    const LpeRender r = renderLpes(scene, camera, 16, {"C<RD>L", "unoccluded;C<RD>L"}, 64, 1);
    REQUIRE(ref.mean(0, 0, 16).y > 0.01f);
    CHECK_THAT(r.mean(1, 0, 16).y, WithinAbs(ref.mean(0, 0, 16).y, 1e-4));
    CHECK_THAT(r.mean(0, 0, 16).y, WithinAbs(ref.mean(0, 0, 16).y, 0.03 * ref.mean(0, 0, 16).y));
}

TEST_CASE("B is the background behind the path, !CB the alpha", "[render][lpe]") {
    Scene scene;
    const MeshId ball = scene.createMesh();
    scene.updateMesh(ball, makeSphere(0.6f, 64, 32));
    LightDesc sun;
    sun.type = LightType::Distant;
    scene.updateLight(scene.createLight(), sun);
    scene.commit();
    const LpeRender r =
        renderLpes(scene, makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, 16, 16), 16, {"CB", "!CB", "C.*", "C.*B"}, 8, 4);
    CHECK(r.program->outputFlags[1] == lpe::OutputInvert);
    int edge = 0;
    for (size_t i = 0; i < r.film.pixelCount(); ++i) {
        INFO("pixel " << i);
        CHECK(r.film.lpe(0)[i].x + r.film.alphaSum[i] == float(r.film.samples)); // CB = 1 - alpha
        CHECK_THAT(r.film.lpe(2)[i].x, WithinAbs(r.film.colorSum[i].x, 1e-5)); // . never matches B
        edge += r.film.lpe(0)[i].x > 0.0f && r.film.alphaSum[i] > 0.0f;
    }
    CHECK(edge > 0);
    // Paths that scatter before escaping reach B too.
    CHECK(r.mean(3, 0, 16).x > r.mean(0, 0, 16).x);
}

TEST_CASE("A is the albedo of the event before it", "[render][lpe]") {
    // CDA on a Lambert ball is its albedo; on a plastic ball the diffuse and
    // glossy albedos add up to the closure's albedo (the first-hit AOV).
    Scene scene;
    const MeshId ball = scene.createMesh();
    scene.updateMesh(ball, makeSphere(1.0f, 96, 48));
    const MaterialId m = scene.createMaterial();
    scene.setMeshMaterials(ball, {m});
    LightDesc sun;
    sun.type = LightType::Distant;
    scene.updateLight(scene.createLight(), sun);
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 0.25f, 8, 8); // the ball's centre
    const std::vector<std::string> albedos = {"CDA", "C<RD>A", "C<RG>A", "C<TG>A", "CA"};

    scene.updateMaterial(m, material(lambertDefaults({0.5f, 0.25f, 0.125f})));
    scene.commit();
    LpeRender r = renderLpes(scene, camera, 8, albedos, 4, 2);
    CHECK_THAT(r.mean(0, 0, 8).x, WithinAbs(0.5, 1e-6));
    CHECK_THAT(r.mean(0, 0, 8).z, WithinAbs(0.125, 1e-6));
    CHECK(r.mean(1, 0, 8).x == r.mean(0, 0, 8).x);
    CHECK(r.mean(2, 0, 8).x == 0.0f);
    CHECK(r.mean(4, 0, 8).x == 0.0f); // A follows a scattering event

    SurfaceInputs plastic = standardSurfaceDefaults();
    plastic.set(sf::coatWeight, 0.5f);
    scene.updateMaterial(m, material(plastic));
    scene.commit();
    r = renderLpes(scene, camera, 8, albedos, 4, 2);
    double first = 0.0;
    for (size_t i = 0; i < r.film.pixelCount(); ++i)
        first += r.film.albedo[i].y / double(r.film.pixelCount());
    CHECK(r.mean(2, 0, 8).y > 0.02f);
    CHECK_THAT(r.mean(0, 0, 8).y + r.mean(2, 0, 8).y, WithinAbs(first, 0.01 * first));
    CHECK(r.mean(3, 0, 8).y == 0.0f);
}
