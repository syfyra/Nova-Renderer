// docs/design/M11-P4.md: light filters, Karma's blocker. The blocker's factor
// against the profiles measured with Karma CPU (the LOP's probe scenes in
// tests/scenes/karma_filters, rendered with husk; the values are pixel means
// of a ground lit straight down by a delta light, so the factor itself), the
// scene's filter records, links and selection bound, and renders: light
// samples, BSDF rays that reach lights (area, dome, a mirror's delta
// continuation) and camera-visible lights.

#include "test_helpers.h"

#include "core/integrator/renderer.h"
#include "core/lights/lights.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;

namespace {

LightFilterDesc blocker(BlockerShape shape) {
    LightFilterDesc d;
    d.shape = shape;
    d.intensity = 0.0f; // the LOP's default: a full block
    return d;
}

// Row-vector rotation about X by 90 degrees (local Y -> world Z) and its
// inverse: a filter rotated like the probes' "r = (90, 0, 0)".
Mat4 rotX90() {
    Mat4 m = Mat4::identity();
    m.m[1][1] = 0.0;
    m.m[1][2] = 1.0;
    m.m[2][1] = -1.0;
    m.m[2][2] = 0.0;
    return m;
}

float factorAt(const LightFilterDesc &d, Vec3f p, int channel = 0) {
    const Spectrum k = lightFilterFactor(makeLightFilterData(d), p);
    return channel == 0 ? k.x : channel == 1 ? k.y : k.z;
}

struct Profile {
    float x, karma;
};

// Karma's values along a line on the ground (y = 0): along +X, or +Z.
void checkProfile(const LightFilterDesc &d, std::initializer_list<Profile> points, bool alongZ = false,
                  int channel = 0) {
    for (const Profile &p : points) {
        const Vec3f at = alongZ ? Vec3f{0.0f, 0.0f, p.x} : Vec3f{p.x, 0.0f, 0.0f};
        INFO("at " << p.x << (alongZ ? " (z)" : " (x)"));
        CHECK_THAT(factorAt(d, at, channel), WithinAbs(p.karma, 2e-3));
    }
}

} // namespace

TEST_CASE("Blocker profiles match Karma's", "[lights][filters]") {
    SECTION("sphere: falloff in units of the radius, a smoothstep around it") {
        LightFilterDesc d = blocker(BlockerShape::Sphere);
        d.radius = 1.0f;
        d.falloff = 0.5f; // sph_fall05
        checkProfile(d, {{0.4125f, 0.0f}, {0.6125f, 0.0352f}, {0.7125f, 0.1164f}, {0.8125f, 0.2321f},
                         {1.0125f, 0.5189f}, {1.2125f, 0.7996f}, {1.4125f, 0.9784f}, {1.6125f, 1.0f}});
        d.radius = 2.0f;
        d.falloff = 1.0f; // sph_fall2
        checkProfile(d, {{0.5125f, 0.0451f}, {1.0125f, 0.1598f}, {2.0125f, 0.5047f}, {3.0125f, 0.8473f}});
        d.radius = 1.0f;
        d.falloff = 1.5f; // sph_fall15: the falloff stops at 1
        checkProfile(d, {{0.4625f, 0.1358f}, {1.0625f, 0.5469f}, {2.0375f, 1.0f}});
        d.falloff = 0.0f; // sph_ground: a hard edge
        checkProfile(d, {{0.9875f, 0.0f}, {1.0125f, 1.0f}});
    }
    SECTION("box: width, height, depth along X, Y, Z; falloff in its smallest half extent") {
        LightFilterDesc d = blocker(BlockerShape::Box);
        d.width = 4.0f;
        d.height = 6.0f;
        d.depth = 5.0f;
        d.roundedCorner = 0.0f;
        d.falloff = 0.5f; // box_big_fall
        checkProfile(d, {{0.9875f, 0.0f}, {1.5125f, 0.1633f}, {2.0375f, 0.5281f}, {2.4875f, 0.8367f}});
        checkProfile(d, {{1.4375f, 0.0f}, {2.4875f, 0.4906f}, {3.0125f, 0.8507f}}, true);
        d.width = d.height = d.depth = 1.0f; // box_small_fall
        checkProfile(d, {{0.3125f, 0.0430f}, {0.4625f, 0.3883f}, {0.6125f, 0.8147f}});
    }
    SECTION("box: rounded corners in units of its smallest half extent, at most that") {
        LightFilterDesc d = blocker(BlockerShape::Box);
        d.width = 3.0f;
        d.height = 2.0f;
        d.depth = 2.0f;
        d.roundedCorner = 0.5f; // box_round: corner circle (1, 0.5) radius 0.5 in the ground's cut
        CHECK(factorAt(d, {1.3f, 0.0f, 0.85f}) == 0.0f);
        CHECK(factorAt(d, {1.4f, 0.0f, 0.9f}) == 1.0f);
        CHECK(factorAt(d, {1.45f, 0.0f, 0.4f}) == 0.0f);
        d.roundedCorner = 1.5f; // box_round_huge: radius 1 (the half depth), circle (0.5, 0)
        CHECK(factorAt(d, {1.4375f, 0.0f, 0.3125f}) == 0.0f);
        CHECK(factorAt(d, {1.4625f, 0.0f, 0.3125f}) == 1.0f);
        d.width = 3.0f;
        d.height = 0.5f;
        d.depth = 2.0f;
        d.roundedCorner = 0.5f; // box_flat_round: radius 0.125 (half of the half height)
        CHECK(factorAt(d, {1.4875f, 0.0f, 0.9125f}) == 0.0f);
        CHECK(factorAt(d, {1.45f, 0.0f, 0.99f}) == 1.0f);
    }
    SECTION("capsule: a segment of length height along local Y, falloff in units of the radius") {
        LightFilterDesc d = blocker(BlockerShape::Capsule);
        d.radius = 0.5f;
        d.height = 3.0f;
        d.falloff = 0.5f;
        d.spaceFromWorld = inverse(rotX90()); // cap_fall: rotated to lie along Z
        checkProfile(d, {{0.2375f, 0.0f}, {0.3125f, 0.0430f}, {0.4625f, 0.3883f}, {0.6875f, 0.9570f}});
        checkProfile(d, {{1.7375f, 0.0f}, {1.8125f, 0.0433f}, {2.0375f, 0.6121f}, {2.2625f, 1.0f}}, true);
    }
    SECTION("intensity, exposure and tint set the factor inside; invert swaps the sides") {
        LightFilterDesc d = blocker(BlockerShape::Sphere);
        d.radius = 1.0f;
        d.falloff = 0.5f;
        d.intensity = 0.5f;
        d.tint = {1.0f, 0.5f, 0.25f}; // sph_fall_int
        checkProfile(d, {{0.0125f, 0.5f}, {1.0125f, 0.7594f}, {1.6125f, 1.0f}}, false, 0);
        checkProfile(d, {{0.0125f, 0.25f}, {1.0125f, 0.6392f}}, false, 1);
        checkProfile(d, {{0.0125f, 0.125f}, {1.0125f, 0.5790f}}, false, 2);
        d.tint = spectrum(1.0f);
        d.invert = true; // sph_fall_int_inv
        checkProfile(d, {{0.0125f, 1.0f}, {1.0125f, 0.7406f}, {1.9125f, 0.5f}});
        d.falloff = 0.0f;
        d.exposure = 1.0f; // sph_exp1: 0.5 x 2 inside, 1 outside
        d.invert = false;
        checkProfile(d, {{0.5f, 1.0f}, {2.5f, 1.0f}});
        d.intensity = 2.0f;
        d.exposure = 0.0f;
        d.invert = true; // sph_int2_inv: 1 inside, 2 outside
        checkProfile(d, {{0.5f, 1.0f}, {2.5f, 2.0f}});
    }
    SECTION("the shape lives in the filter's space") {
        LightFilterDesc d = blocker(BlockerShape::Sphere);
        d.radius = 1.0f;
        d.falloff = 0.5f;
        Mat4 s = Mat4::identity(); // sph_scaled_fall: the filter scaled (2, 1, 1)
        s.m[0][0] = 2.0;
        d.spaceFromWorld = inverse(s);
        checkProfile(d, {{1.0625f, 0.0029f}, {2.0375f, 0.5282f}, {2.9375f, 0.9972f}});
        checkProfile(d, {{0.6125f, 0.0351f}, {0.9875f, 0.4813f}}, true);
    }
}

// Link categories are the core's mechanism for filterLink; Hydra sets none, as
// Karma ignores filterLink (docs/design/M11-P4.md F6).
TEST_CASE("Filters of a light multiply; link categories select receivers", "[lights][filters]") {
    LightFilterDesc a = blocker(BlockerShape::Sphere), b = blocker(BlockerShape::Sphere);
    a.radius = b.radius = 1.0f;
    a.intensity = b.intensity = 0.5f;
    a.spaceFromWorld = inverse(translation({-0.5f, 0.0f, 0.0f}));
    b.spaceFromWorld = inverse(translation({0.5f, 0.0f, 0.0f}));
    LightFilterData f[2] = {makeLightFilterData(a), makeLightFilterData(b)};
    // two_filters: 0.25 where both overlap, 0.5 under one, 1 outside.
    CHECK(lightFilterProduct(f, 2, {{0.0f, 0.0f, 0.0f}, 0}).x == 0.25f);
    CHECK(lightFilterProduct(f, 2, {{-1.2f, 0.0f, 0.0f}, 0}).x == 0.5f);
    CHECK(lightFilterProduct(f, 2, {{2.0f, 0.0f, 0.0f}, 0}).x == 1.0f);
    // A filter limited to category bit 2 acts on receivers that hold it.
    f[1].link = uint64_t(1) << 2;
    CHECK(lightFilterProduct(f, 2, {{0.0f, 0.0f, 0.0f}, uint64_t(1) << 2}).x == 0.25f);
    CHECK(lightFilterProduct(f, 2, {{0.0f, 0.0f, 0.0f}, uint64_t(1) << 3}).x == 0.5f);
    CHECK(lightFilterProduct(f, 2, {{0.0f, 0.0f, 0.0f}, 0}).x == 0.5f);
}

TEST_CASE("Lights resolve their filters by key", "[lights][filters]") {
    Scene scene;
    MeshDesc groundA = makeQuad(1.0f), groundB = makeQuad(1.0f);
    groundB.transform = translation({3.0f, 0.0f, 0.0f});
    const MeshId a = scene.createMesh(), b = scene.createMesh();
    scene.updateMesh(a, groundA);
    scene.updateMesh(b, groundB);
    scene.setMeshCategories(a, {"groupA"});
    LightDesc sun; // a delta distant light towards +Z
    sun.angleDeg = 0.0f;
    sun.filters = {"/blk", "/missing"};
    const LightId sunId = scene.createLight();
    scene.updateLight(sunId, sun);
    LightDesc other = sun;
    other.filters.clear();
    scene.updateLight(scene.createLight(), other);
    scene.commit();
    // The filter arrives after the light: no effect until it does.
    REQUIRE(scene.lights()[0].filterCount == 0);
    const Vec3f inside{0.0f, 0.0f, 0.0f}, outside{2.0f, 0.0f, 0.0f};
    CHECK(sampleLight(scene.lights()[0], inside, {0.5f, 0.5f}).Li.x == 1.0f);

    LightFilterDesc d = blocker(BlockerShape::Sphere);
    d.radius = 1.0f;
    scene.setLightFilter("/blk", d);
    scene.commit();
    REQUIRE(scene.lights()[0].filterCount == 1);
    CHECK(scene.lights()[1].filterCount == 0);
    CHECK(sampleLight(scene.lights()[0], inside, {0.5f, 0.5f}).Li.x == 0.0f);
    CHECK(sampleLight(scene.lights()[0], outside, {0.5f, 0.5f}).Li.x == 1.0f);
    CHECK(sampleLight(scene.lights()[1], inside, {0.5f, 0.5f}).Li.x == 1.0f);
    // A blocker that only darkens leaves the selection weights alone.
    CHECK(scene.lightSampler().pmf[0] == scene.lightSampler().pmf[1]);

    SECTION("a link category: only the linked objects' receiver set (not used by Hydra, F6)") {
        d.link = "groupA";
        scene.setLightFilter("/blk", d);
        scene.commit();
        const LightLinks links = scene.lightLinks();
        const uint64_t ca = links.categories(scene.receiverSet(a, 0)), cb = links.categories(scene.receiverSet(b, 0));
        CHECK(sampleLight(scene.lights()[0], inside, {0.5f, 0.5f}, ca).Li.x == 0.0f);
        CHECK(sampleLight(scene.lights()[0], inside, {0.5f, 0.5f}, cb).Li.x == 1.0f);
        // Every light still lights every object.
        CHECK(links.links(0, scene.receiverSet(b, 0)));
        CHECK(links.links(1, scene.receiverSet(a, 0)));
    }
    SECTION("a brightening filter raises the light's selection weight by its bound") {
        d.intensity = 2.0f;
        d.exposure = 1.0f;
        d.invert = true; // 4x outside the sphere
        scene.setLightFilter("/blk", d);
        scene.commit();
        CHECK_THAT(scene.lightSampler().pmf[0] / scene.lightSampler().pmf[1], WithinAbs(4.0, 1e-4));
    }
    SECTION("removal") {
        scene.removeLightFilter("/blk");
        scene.commit();
        CHECK(scene.lights()[0].filterCount == 0);
        CHECK(sampleLight(scene.lights()[0], inside, {0.5f, 0.5f}).Li.x == 1.0f);
    }
}

namespace {

struct Pixels {
    std::vector<Spectrum> pixels;
    int width = 0;
    Spectrum at(int x, int y) const { return pixels[size_t(y) * size_t(width) + size_t(x)]; }
};

Pixels renderImage(const Scene &scene, const Camera &camera, int size, uint32_t spp, uint32_t maxDepth = 1) {
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = spp;
    settings.maxDepth = maxDepth;
    settings.pixelFilterScale = 0.0f; // point samples at pixel centres
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(camera);
    renderer.resize(size, size);
    while (!renderer.converged())
        renderer.renderPass({});
    const Film &film = renderer.film();
    Pixels image;
    image.width = size;
    for (size_t i = 0; i < film.colorSum.size(); ++i)
        image.pixels.push_back(film.colorSum[i] * (1.0f / float(film.count[i])));
    return image;
}

// World x (and y) of pixel column (row) i of an ortho view of half width h.
float pixelX(int i, int size, float h) { return (float(i) + 0.5f) / float(size) * 2.0f * h - h; }

Mat4 facingUp() { // a rect's emission (local -Z) to world +Z
    Mat4 m = Mat4::identity();
    m.m[1][1] = -1.0;
    m.m[2][2] = -1.0;
    return m;
}

} // namespace

TEST_CASE("Blockers act wherever light is gathered", "[lights][filters][render]") {
    const int size = 40;
    const float h = 2.0f;
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 5.0f}, h, size, size);
    LightFilterDesc d = blocker(BlockerShape::Sphere);
    d.radius = 1.0f;
    d.falloff = 0.5f;
    const LightFilterData f = makeLightFilterData(d);

    SECTION("light samples of a delta light: the factor at the shading point") {
        Scene scene;
        MeshDesc ground = makeQuad(4.0f);
        ground.setDisplayColor(spectrum(0.5f));
        scene.updateMesh(scene.createMesh(), ground);
        LightDesc sun;
        sun.angleDeg = 0.0f;
        sun.filters = {"blk"};
        scene.updateLight(scene.createLight(), sun);
        scene.setLightFilter("blk", d);
        scene.commit();
        const Pixels image = renderImage(scene, camera, size, 4);
        const float lit = 0.5f / kPi;
        for (int x = 0; x < size; ++x) {
            const float wx = pixelX(x, size, h);
            INFO("x " << wx);
            CHECK_THAT(image.at(x, size / 2).x, WithinAbs(lit * lightFilterFactor(f, {wx, pixelX(size / 2, size, h), 0.0f}).x, 1e-5));
        }
    }
    SECTION("area and dome lights: both strategies filtered alike, a blocked point gets nothing") {
        for (const LightType type : {LightType::Rect, LightType::Dome}) {
            INFO("light type " << int(type));
            Scene scene;
            MeshDesc ground = makeQuad(4.0f);
            ground.setDisplayColor(spectrum(0.5f));
            scene.updateMesh(scene.createMesh(), ground);
            LightDesc light;
            light.type = type;
            light.width = light.height = 3.0f;
            light.intensity = 2.0f;
            light.transform = translation({0.0f, 0.0f, 2.0f}); // emitting down -Z
            const LightId id = scene.createLight();
            scene.updateLight(id, light);
            scene.commit();
            const Pixels plain = renderImage(scene, camera, size, 64);
            light.filters = {"blk"};
            scene.updateLight(id, light);
            scene.setLightFilter("blk", d);
            scene.commit();
            const Pixels filtered = renderImage(scene, camera, size, 64);
            for (int x = 0; x < size; ++x) {
                const float wx = pixelX(x, size, h), wy = pixelX(size / 2, size, h);
                const float k = lightFilterFactor(f, {wx, wy, 0.0f}).x;
                INFO("x " << wx << ", factor " << k);
                // The same samples (the blocker only darkens: same selection),
                // each scaled by the factor at the shading point.
                CHECK_THAT(filtered.at(x, size / 2).x, WithinAbs(plain.at(x, size / 2).x * k, 1e-4 + 1e-4 * plain.at(x, size / 2).x));
            }
            CHECK(filtered.at(size / 2, size / 2).x == 0.0f);
        }
    }
    SECTION("a mirror's delta continuation: the factor at the mirror point") {
        Scene scene;
        MaterialDesc mirror;
        mirror.inputs = standardSurfaceDefaults();
        mirror.inputs.set(surface_field::baseColor, spectrum(1.0f));
        mirror.inputs.set(surface_field::metalness, 1.0f);
        mirror.inputs.set(surface_field::specularRoughness, 0.0f);
        const MaterialId mirrorId = scene.createMaterial();
        scene.updateMaterial(mirrorId, mirror);
        const MeshId groundId = scene.createMesh();
        scene.updateMesh(groundId, makeQuad(4.0f));
        scene.setMeshMaterials(groundId, {mirrorId});
        LightDesc rect; // facing the mirror, above the view's centre
        rect.type = LightType::Rect;
        rect.width = rect.height = 3.0f;
        rect.intensity = 2.0f;
        rect.transform = translation({0.0f, 0.0f, 2.0f});
        rect.filters = {"blk"};
        scene.updateLight(scene.createLight(), rect);
        LightFilterDesc small = blocker(BlockerShape::Sphere);
        small.radius = 0.7f;
        scene.setLightFilter("blk", small);
        scene.commit();
        const Pixels image = renderImage(scene, camera, size, 4, 2);
        CHECK(image.at(size / 2, size / 2).x == 0.0f); // the mirror point is blocked
        CHECK(image.at(size / 2 + 12, size / 2).x > 1.0f); // x = 1.25: the light's reflection
        // A blocker around the light only changes nothing (Karma: the light's
        // point is not tested).
        small.spaceFromWorld = inverse(translation({0.0f, 0.0f, 2.0f}));
        scene.setLightFilter("blk", small);
        scene.commit();
        const Pixels around = renderImage(scene, camera, size, 4, 2);
        CHECK(around.at(size / 2, size / 2).x > 1.0f);
    }
    SECTION("camera-visible lights: the factor at the camera ray's origin") {
        Scene scene;
        LightDesc rect; // facing the camera
        rect.type = LightType::Rect;
        rect.width = rect.height = 2.0f;
        rect.intensity = 5.0f;
        rect.cameraVisible = true;
        rect.transform = facingUp();
        rect.filters = {"blk"};
        scene.updateLight(scene.createLight(), rect);
        LightFilterDesc around = blocker(BlockerShape::Sphere);
        around.radius = 1.0f;
        around.spaceFromWorld = inverse(translation({0.0f, 0.0f, 5.0f})); // around the camera's plane centre
        scene.setLightFilter("blk", around);
        scene.commit();
        const Pixels image = renderImage(scene, camera, size, 1);
        CHECK(image.at(size / 2, size / 2).x == 0.0f);    // ray origin within 1 of the centre
        CHECK(image.at(size / 2 + 9, size / 2 + 9).x == 5.0f); // (0.95, 0.95): origin 1.34 away
        around.spaceFromWorld = Mat4::identity(); // around the light only
        scene.setLightFilter("blk", around);
        scene.commit();
        CHECK(renderImage(scene, camera, size, 1).at(size / 2, size / 2).x == 5.0f);
    }
}
