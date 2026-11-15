// The line light: a cylinder light with treatAsLine, the Light LOP's type
// "Line" (docs/bug-audit-2026-11.md, section 23). The attribute was not read:
// the light rendered as the cylinder of its radius, 19 times darker than
// Karma's line at a radius of 0.1.
//
// Karma's law, measured on a lit ground under lines of three lengths and two
// heights (every pixel of four images within 2 parts in 10 000): a point
// light at the line's middle whose light at a point is the light's colour
// times its intensity times the angle, in radians, under which that point
// sees the line. The radius and normalize do nothing, the camera does not see
// it, and its shadows are the middle's.

#include "test_helpers.h"

#include "core/lights/lights.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// A line of `length` along x, `height` over the plane z = 0.
LightDesc line(float length, float height) {
    LightDesc d;
    d.type = LightType::Cylinder;
    d.treatAsLine = true;
    d.intensity = 10.0f;
    d.length = length;
    d.radius = 0.1f;
    d.transform = translation({0.0f, 0.0f, height});
    return d;
}

// The angle under which (x, y, 0) sees the line.
double seenAngle(double length, double height, double x, double y) {
    const double a[3] = {-0.5 * length - x, -y, height}, b[3] = {0.5 * length - x, -y, height};
    const double la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    const double lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    return std::acos(std::clamp((a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb), -1.0, 1.0));
}

// What the plane z = 0 receives at (x, y): intensity x angle x the cosine to the line's middle.
double irradiance(double intensity, double length, double height, double x, double y) {
    return intensity * seenAngle(length, height, x, y) * height / std::sqrt(x * x + y * y + height * height);
}

// The film of a ground of the default material (a matte 0.18) under `scene`'s
// lights, seen from above at one sample through each pixel's centre.
Film ground(Scene &scene, float halfWidth, int size, bool lightTree) {
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = 1;
    settings.maxDepth = 1;
    settings.lightTree = lightTree;
    settings.pixelFilterScale = 0.0f;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 9.0f}, halfWidth, size, size));
    renderer.resize(size, size);
    renderToConvergence(renderer);
    return renderer.film();
}

} // namespace

TEST_CASE("A line light is a point light at its middle with the angle it is seen under", "[lights][line]") {
    const LightData l = makeLightData(line(2.0f, 2.0f));
    CHECK(l.delta);
    CHECK_FALSE(l.cameraVisible);
    CHECK(l.emission.x == 10.0f);

    // The radius and normalize do nothing (Karma: the same image at a radius of 0.01, 0.1 and 0.2, normalized or not).
    LightDesc other = line(2.0f, 2.0f);
    other.radius = 0.5f;
    other.normalize = true;
    other.cameraVisible = true;
    const LightData o = makeLightData(other);
    CHECK(o.emission.x == 10.0f);
    CHECK(o.delta);
    CHECK_FALSE(o.cameraVisible);

    // A sample from any point: the direction of the middle, the light of the angle; the numbers do not matter.
    for (const Vec3f p : {Vec3f{0.0f, 0.0f, 0.0f}, Vec3f{-2.75f, 0.0f, 0.0f}, Vec3f{0.0f, 2.0f, 0.0f}, Vec3f{1.3f, -0.7f, 0.5f},
                          Vec3f{3.0f, 0.0f, 2.0f}}) {
        const LightSample s = sampleLight(l, p, {0.31f, 0.77f}), t = sampleLight(l, p, {0.92f, 0.05f});
        INFO("from (" << p.x << ", " << p.y << ", " << p.z << ")");
        const Vec3f d = Vec3f{0.0f, 0.0f, 2.0f} - p;
        const double a[3] = {-1.0 - p.x, -p.y, 2.0 - p.z}, b[3] = {1.0 - p.x, -p.y, 2.0 - p.z};
        const double angle = std::acos((a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) /
                                       (std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) *
                                        std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2])));
        if (!(angle > 1e-6)) { // on the line's own axis: nothing of it is seen
            CHECK_FALSE(s.pdf > 0.0f);
            continue;
        }
        CHECK(s.delta);
        CHECK(s.pdf == 1.0f);
        CHECK(length(s.wi - normalize(d)) < 1e-6f);
        CHECK_THAT(double(s.distance), WithinRel(double(length(d)), 1e-3));
        CHECK_THAT(double(s.Li.x), WithinRel(10.0 * angle, 1e-5));
        CHECK(s.Li.x == t.Li.x);
        CHECK(s.wi.x == t.wi.x);
    }

    // No ray meets it: the loops over the area lights a ray may cross (forEachAreaLightCrossing, the camera's)
    // leave out lights without area, as they do a point light. A ray through the line's middle:
    CHECK_FALSE((isAreaLight(l.type) && !l.delta));
    int crossed = 0;
    forEachAreaLightCrossing(&l, 1, nullptr, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, kInf,
                             [&](uint32_t, float, Vec3f, bool) { ++crossed; });
    CHECK(crossed == 0);

    // Karma's ground of albedo 0.5 under the line's middle, the mean of the
    // half unit square its image's middle 10 x 10 pixels cover: 1.45943 for
    // a length of 2 at a height of 2, 0.77007 for 1 at 2, 2.47850 for 4 at 2,
    // 0.77733 for 2 at 4.
    const auto patch = [](double length, double height) {
        double sum = 0.0;
        for (int j = 0; j < 10; ++j)
            for (int i = 0; i < 10; ++i)
                sum += irradiance(10.0, length, height, (i + 0.5) * 0.05 - 0.25, (j + 0.5) * 0.05 - 0.25);
        return sum / 100.0 * 0.5 / kPi;
    };
    CHECK_THAT(patch(2.0, 2.0), WithinRel(1.45943, 2e-3));
    CHECK_THAT(patch(1.0, 2.0), WithinRel(0.77007, 2e-3));
    CHECK_THAT(patch(4.0, 2.0), WithinRel(2.47850, 2e-3));
    CHECK_THAT(patch(2.0, 4.0), WithinRel(0.77733, 2e-3));
}

TEST_CASE("A line light lights and shadows as its law says", "[lights][line][render]") {
    for (const bool lightTree : {false, true}) {
        INFO("light tree " << lightTree);
        Scene scene;
        scene.updateMesh(scene.createMesh(), makeQuad(5.0f));
        // A card under the middle, a unit below it: the middle's shadow of it is the square of 2 units.
        MeshDesc card;
        card.points = {{-0.5f, -0.5f, 1.0f}, {0.5f, -0.5f, 1.0f}, {0.5f, 0.5f, 1.0f}, {-0.5f, 0.5f, 1.0f}};
        card.indices = {0, 1, 2, 0, 2, 3};
        const MeshId cardId = scene.createMesh();
        scene.updateMesh(cardId, card);
        const LightId light = scene.createLight();
        scene.updateLight(light, line(3.0f, 2.0f));
        // A second light, so that a tree has something to choose from; it lights nothing here.
        LightDesc far = line(1.0f, 2.0f);
        far.intensity = 0.0f;
        far.transform = translation({40.0f, 0.0f, 2.0f});
        scene.updateLight(scene.createLight(), far);
        scene.commit();
        const int size = 20; // pixels of 0.4 units, centres at -3.8 .. 3.8
        const Film film = ground(scene, 4.0f, size, lightTree);
        int lit = 0, shadowed = 0;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const double px = (x + 0.5) * 0.4 - 4.0, py = (y + 0.5) * 0.4 - 4.0;
                const float got = pixelMean(film, x, y).x;
                INFO("pixel (" << x << ", " << y << ") at (" << px << ", " << py << ")");
                if (std::fabs(px) < 0.5 && std::fabs(py) < 0.5) {
                    continue; // the card itself (lit from above, in its own way)
                } else if (std::fabs(px) < 0.95 && std::fabs(py) < 0.95) {
                    CHECK(got == 0.0f); // in the middle's shadow, whatever of the line a point sees beside the card
                    ++shadowed;
                } else if (std::fabs(px) > 1.05 || std::fabs(py) > 1.05) {
                    CHECK_THAT(double(got), WithinRel(irradiance(10.0, 3.0, 2.0, px, py) * 0.18 / kPi, 2e-4));
                    ++lit;
                }
            }
        CHECK(shadowed >= 8);
        CHECK(lit >= 300);
    }
}
