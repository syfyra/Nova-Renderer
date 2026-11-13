// UsdLux ShadowAPI as Karma renders it (docs/bug-audit-2026-11.md, section
// 20; measured with a card over a ground under each kind of light):
//   inputs:shadow:enable off     nothing shadows the light
//   inputs:shadow:color c        a shadowed sample still gives c of its light:
//                                L (c + (1 - c) T) for a transmittance T
//   inputs:shadow:distance D     occluders further than D from the lit point
//                                do not shadow
//   inputs:shadow:falloff F      over the last F before D shadows fade: an
//                                occluder at d shadows by ((D - d) / F)^gamma
// The light's own samples alone see these shadows, so a light with any of
// them is sampled by next-event estimation only (LightData::ownShadows), and
// a dome's BSDF samples become shadow rays of their own.

#include "test_helpers.h"

#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <functional>
#include <vector>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;

namespace {

// The mean radiance of the film (direct light only), seen straight down by
// an orthographic camera at `eye` that covers `halfWidth` to each side.
Spectrum meanRadiance(Scene &scene, Vec3f eye, float halfWidth, uint32_t spp, bool lightTree = false) {
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = spp;
    settings.maxDepth = 1;
    settings.lightTree = lightTree;
    settings.pixelFilterScale = 0.0f;
    const int size = 8;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera(eye, halfWidth, size, size));
    renderer.resize(size, size);
    renderToConvergence(renderer);
    Spectrum sum{0.0f, 0.0f, 0.0f};
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            sum += pixelMean(renderer.film(), x, y);
    return sum * (1.0f / float(size * size));
}

// A ground at z = 0 under a sun that travels along (-1, 0, -1), and a card
// at z = 1 over x in [2, 3], which shadows the ground at x in [1, 2]: its
// shadow rays meet the card after sqrt 2. `opacity`: the card's.
struct SunScene {
    Scene scene;
    LightId sun = 0;
    LightDesc desc;

    explicit SunScene(float opacity = 1.0f) {
        scene.updateMesh(scene.createMesh(), makeQuad(4.0f));
        MeshDesc card;
        card.points = {{2.0f, -4.0f, 1.0f}, {3.0f, -4.0f, 1.0f}, {3.0f, 4.0f, 1.0f}, {2.0f, 4.0f, 1.0f}};
        card.indices = {0, 1, 2, 0, 2, 3};
        const MeshId id = scene.createMesh();
        scene.updateMesh(id, card);
        if (opacity < 1.0f) {
            MaterialDesc veil;
            veil.inputs = lambertDefaults(spectrum(0.5f));
            veil.inputs.set(surface_field::opacity, spectrum(opacity));
            const MaterialId m = scene.createMaterial();
            scene.updateMaterial(m, veil);
            scene.setMeshMaterials(id, {m});
        }
        desc.type = LightType::Distant;
        desc.angleDeg = 0.0f;
        const float c = std::sqrt(0.5f);
        desc.transform.m[0][0] = c;
        desc.transform.m[0][2] = -c;
        desc.transform.m[2][0] = c;
        desc.transform.m[2][2] = c;
        sun = scene.createLight();
    }
    // The ground's radiance in the card's shadow over its radiance in the open, per channel.
    Spectrum shadowed(const std::function<void(LightDesc &)> &set, uint32_t spp = 256) {
        LightDesc d = desc;
        set(d);
        scene.updateLight(sun, d);
        scene.commit();
        const Spectrum lit = meanRadiance(scene, {-1.0f, 0.0f, 5.0f}, 0.5f, 16);
        const Spectrum shade = meanRadiance(scene, {1.5f, 0.0f, 0.5f}, 0.3f, spp);
        REQUIRE(lit.x > 0.0f);
        return {shade.x / lit.x, shade.y / lit.y, shade.z / lit.z};
    }
};

} // namespace

TEST_CASE("A light without ShadowAPI settings casts full shadows", "[lights][shadow]") {
    SunScene s;
    const Spectrum r = s.shadowed([](LightDesc &) {});
    CHECK(r.x == 0.0f);
    CHECK(r.y == 0.0f);
    CHECK(r.z == 0.0f);
    // Nothing of it in the light's record either: the usual strategies.
    CHECK_FALSE(s.scene.lights()[0].shadowShaped);
    CHECK_FALSE(s.scene.lights()[0].ownShadows());
}

TEST_CASE("A light with shadows off is shadowed by nothing", "[lights][shadow]") {
    SunScene s;
    const Spectrum r = s.shadowed([](LightDesc &d) { d.castsShadows = false; });
    CHECK_THAT(r.x, WithinAbs(1.0, 1e-4));
    CHECK_THAT(r.z, WithinAbs(1.0, 1e-4));
    CHECK(s.scene.lights()[0].ownShadows());
    // A white shadow colour is the same light.
    const Spectrum white = s.shadowed([](LightDesc &d) { d.shadowColor = spectrum(1.0f); });
    CHECK_THAT(white.y, WithinAbs(1.0, 1e-4));
}

TEST_CASE("A shadow colour is the share of the light that arrives in its shadow", "[lights][shadow]") {
    SunScene s;
    const Spectrum r = s.shadowed([](LightDesc &d) { d.shadowColor = {1.0f, 0.25f, 0.0f}; });
    CHECK_THAT(r.x, WithinAbs(1.0, 1e-4));
    CHECK_THAT(r.y, WithinAbs(0.25, 1e-4));
    CHECK_THAT(r.z, WithinAbs(0.0, 1e-6));

    // Behind a half-transparent card: c + (1 - c) / 2 (Karma: 0.3535, 0.2210, 0.1768 of 0.3535).
    SunScene veil(0.5f);
    const Spectrum plain = veil.shadowed([](LightDesc &) {}, 4096);
    CHECK_THAT(plain.y, WithinAbs(0.5, 0.02));
    const Spectrum tinted = veil.shadowed([](LightDesc &d) { d.shadowColor = {1.0f, 0.25f, 0.0f}; }, 4096);
    CHECK_THAT(tinted.x, WithinAbs(1.0, 1e-4));
    CHECK_THAT(tinted.y, WithinAbs(0.625, 0.02));
    CHECK_THAT(tinted.z, WithinAbs(0.5, 0.02));
}

TEST_CASE("Occluders beyond a light's shadow distance do not shadow", "[lights][shadow]") {
    SunScene s; // the card is sqrt 2 = 1.414 from the ground along the shadow rays
    CHECK_THAT(s.shadowed([](LightDesc &d) { d.shadowDistance = 0.5f; }).y, WithinAbs(1.0, 1e-4));
    CHECK_THAT(s.shadowed([](LightDesc &d) { d.shadowDistance = 1.4f; }).y, WithinAbs(1.0, 1e-4));
    CHECK(s.shadowed([](LightDesc &d) { d.shadowDistance = 1.43f; }).y == 0.0f);
    CHECK(s.shadowed([](LightDesc &d) { d.shadowDistance = 2.0f; }).y == 0.0f);
    // 0 and less: no limit (the schema's -1).
    CHECK(s.shadowed([](LightDesc &d) { d.shadowDistance = 0.0f; }).y == 0.0f);
}

TEST_CASE("Shadows fade over the falloff before the shadow distance", "[lights][shadow]") {
    SunScene s;
    const double d = std::sqrt(2.0);
    // Distance 2, falloff 1.5: the zone is 0.5 to 2 and the card at the share t = 0.609 of it.
    const double t = (d - 0.5) / 1.5;
    const Spectrum linear = s.shadowed(
        [](LightDesc &l) {
            l.shadowDistance = 2.0f;
            l.shadowFalloff = 1.5f;
        },
        16384);
    CHECK_THAT(linear.y, WithinAbs(t, 0.012)); // Karma: 0.2155 of 0.3535 = 0.6096
    const Spectrum squared = s.shadowed(
        [](LightDesc &l) {
            l.shadowDistance = 2.0f;
            l.shadowFalloff = 1.5f;
            l.shadowFalloffGamma = 2.0f;
        },
        16384);
    CHECK_THAT(squared.y, WithinAbs(1.0 - (1.0 - t) * (1.0 - t), 0.012)); // Karma: 0.2996 of 0.3535 = 0.8475
    // A card before the zone is a full shadow, a zone longer than the distance starts at the lit point.
    CHECK(s.shadowed([](LightDesc &l) {
               l.shadowDistance = 3.0f;
               l.shadowFalloff = 1.0f;
           }).y == 0.0f);
    const Spectrum longZone = s.shadowed(
        [](LightDesc &l) {
            l.shadowDistance = 2.0f;
            l.shadowFalloff = 4.0f;
        },
        16384);
    CHECK_THAT(longZone.y, WithinAbs(1.0 - (2.0 - d) / 4.0, 0.012));
    // With a shadow colour the faded shadow is tinted: c + (1 - c) t.
    const Spectrum tinted = s.shadowed(
        [](LightDesc &l) {
            l.shadowDistance = 2.0f;
            l.shadowFalloff = 1.5f;
            l.shadowColor = {0.5f, 0.0f, 1.0f};
        },
        16384);
    CHECK_THAT(tinted.x, WithinAbs(0.5 + 0.5 * t, 0.012));
    CHECK_THAT(tinted.y, WithinAbs(t, 0.012));
    CHECK_THAT(tinted.z, WithinAbs(1.0, 1e-3)); // the two means are sums of floats over other sample counts
}

// Lights that BSDF samples reach as well (area lights, domes): a ray that
// continues the path stops at the first surface, so only the light's own
// shadow rays may count it. A card right over the ground hides most of the
// light from the ground below it.
TEST_CASE("Area and dome lights with shadow settings light through their own rays alone", "[lights][shadow][mis]") {
    for (const LightType type : {LightType::Rect, LightType::Sphere, LightType::Dome}) {
        for (const bool tree : {false, true}) {
            const auto ground = [&](bool card, const std::function<void(LightDesc &)> &set) {
                Scene scene;
                scene.updateMesh(scene.createMesh(), makeQuad(4.0f));
                if (card) {
                    MeshDesc c = makeQuad(0.6f);
                    c.transform = translation({0.0f, 0.0f, 0.4f});
                    scene.updateMesh(scene.createMesh(), c);
                }
                LightDesc light;
                light.type = type;
                light.width = light.height = 2.0f;
                light.radius = 0.7f;
                light.intensity = 4.0f;
                light.transform = translation({0.0f, 0.0f, 2.5f});
                set(light);
                scene.updateLight(scene.createLight(), light);
                scene.commit();
                // From below the card: the ground under it.
                return double(meanRadiance(scene, {0.0f, 0.0f, 0.2f}, 0.15f, 4096, tree).y);
            };
            const double open = ground(false, [](LightDesc &) {});
            const double plain = ground(true, [](LightDesc &) {});
            const double off = ground(true, [](LightDesc &l) { l.castsShadows = false; });
            const double grey = ground(true, [](LightDesc &l) { l.shadowColor = spectrum(0.25f); });
            const double near = ground(true, [](LightDesc &l) { l.shadowDistance = 0.1f; });
            INFO("light type " << int(type) << (tree ? ", light tree" : "") << ": open " << open << ", shadowed " << plain
                               << ", shadows off " << off << ", shadow colour 0.25 " << grey << ", distance 0.1 "
                               << near);
            REQUIRE(open > 0.0);
            CHECK(plain < 0.5 * open); // the card does shadow
            CHECK_THAT(off, WithinAbs(open, 0.02 * open));
            CHECK_THAT(near, WithinAbs(open, 0.02 * open));
            CHECK_THAT(grey, WithinAbs(plain + 0.25 * (open - plain), 0.02 * open));
        }
    }
}
