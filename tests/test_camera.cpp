#include "test_helpers.h"

#include "core/math/sobol.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// Hydra's units: focal length 5 and horizontal aperture 3.6 in world units
// (USD 50 mm and 36 mm), 256 x 128 pixels.
constexpr double kFocal = 5.0, kAperture = 3.6;
constexpr int kWidth = 256, kHeight = 128;

Camera lensCamera(float fStop, float focusDistance) {
    Camera c = makePerspectiveCamera({0.0f, 0.0f, 0.0f}, kFocal, kAperture, kWidth, kHeight);
    c.lensRadius = float(kFocal / (2.0 * fStop));
    c.focusDistance = focusDistance;
    return c;
}

} // namespace

TEST_CASE("The lens center gives the pinhole ray", "[camera]") {
    Camera pinhole = makePerspectiveCamera({1.0f, 2.0f, 3.0f}, kFocal, kAperture, kWidth, kHeight);
    Camera lens = pinhole;
    lens.lensRadius = 0.5f;
    lens.focusDistance = 7.0f;
    for (const Vec2f ndc : {Vec2f{0.0f, 0.0f}, Vec2f{0.7f, -0.3f}, Vec2f{-0.9f, 0.95f}}) {
        const Ray a = pinhole.generateRay(ndc);
        const Ray b = lens.generateRay(ndc, {0.5f, 0.5f});
        CHECK(length(a.org - b.org) < 1e-6f);
        CHECK(length(a.dir - b.dir) < 1e-6f);
        CHECK_THAT(double(b.tMax), WithinRel(double(a.tMax), 1e-6));
    }
}

// Every lens sample of one image point passes through the same point on the
// planar focus plane, and the lens is sampled uniformly (E[r^2] = R^2 / 2).
TEST_CASE("Thin lens rays converge on the focus plane", "[camera]") {
    const Camera c = lensCamera(2.8f, 10.0f);
    const double R = kFocal / (2.0 * 2.8);
    const Vec2f ndc{0.4f, -0.6f};
    const Ray center = c.generateRay(ndc); // starts on the near plane
    const float tCenter = (10.0f + center.org.z) / -center.dir.z;
    const Vec3f focus = center.org + center.dir * tCenter;
    CHECK_THAT(focus.z, WithinAbs(-10.0, 1e-4));

    double r2 = 0.0, worst = 0.0;
    const int n = 4096;
    for (int i = 0; i < n; ++i) {
        const Ray ray = c.generateRay(ndc, Sampler::make(i, 0, 0).get2D(stream::kLens));
        const double t = (10.0 - double(-ray.org.z)) / double(-ray.dir.z);
        const Vec3f p = ray.org + ray.dir * float(t);
        worst = std::fmax(worst, double(length(p - focus)));
        // Lens point: back along the ray to the lens plane z = 0.
        const Vec3f l = ray.org - ray.dir * (ray.org.z / ray.dir.z);
        r2 += double(l.x * l.x + l.y * l.y);
        REQUIRE(std::sqrt(double(l.x * l.x + l.y * l.y)) <= R * (1.0 + 1e-5));
    }
    CHECK(worst < 1e-4);
    CHECK_THAT(r2 / n, WithinRel(R * R / 2.0, 0.01));
}

namespace {

// Coverage (alpha) of each pixel column for a card covering x >= 0 at depth
// `cardZ` in front of the camera, averaged over all rows. Point pixel
// filter: the pinhole ray passes through the pixel center.
std::vector<double> edgeCoverage(const Camera &camera, float cardZ, uint32_t samples) {
    Scene scene;
    MeshDesc card;
    card.points = {{0.0f, -50.0f, cardZ}, {50.0f, -50.0f, cardZ}, {50.0f, 50.0f, cardZ}, {0.0f, 50.0f, cardZ}};
    card.indices = {0, 1, 2, 0, 2, 3};
    scene.updateMesh(scene.createMesh(), card);
    scene.commit();

    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = samples;
    settings.maxDepth = 1;
    settings.pixelFilterScale = 0.0f;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(camera);
    renderer.resize(kWidth, kHeight);
    renderToConvergence(renderer);
    const Film &film = renderer.film();
    std::vector<double> alpha(kWidth, 0.0);
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x)
            alpha[size_t(x)] += film.alphaSum[size_t(y) * kWidth + size_t(x)] / film.samples / kHeight;
    return alpha;
}

std::vector<double> edgeCoverage(float fStop, float focusDistance, float cardZ, uint32_t samples) {
    return edgeCoverage(lensCamera(fStop, focusDistance), cardZ, samples);
}

// Row-vector rotation about +Y by `degrees` (x turns towards -Z).
Mat4 rotationY(double degrees) {
    const double a = degrees * kPi / 180.0;
    Mat4 m = Mat4::identity();
    m.m[0][0] = std::cos(a);
    m.m[0][2] = -std::sin(a);
    m.m[2][0] = std::sin(a);
    m.m[2][2] = std::cos(a);
    return m;
}

} // namespace

// Rays at shutter time t follow the keys piecewise: translation linearly,
// rotation along the arc, the view-space ray unchanged.
TEST_CASE("Camera motion interpolates between the keys", "[camera][motion]") {
    const Camera still = makePerspectiveCamera({0.0f, 0.0f, 0.0f}, kFocal, kAperture, kWidth, kHeight);
    const Vec2f ndc{0.3f, -0.2f};
    const Ray reference = still.generateRay(ndc);

    Camera moving = still;
    const Mat4 slide[3] = {translation({0.0f, 0.0f, 0.0f}), translation({1.0f, 0.0f, 0.0f}),
                           translation({3.0f, 0.0f, 0.0f})};
    moving.setMotion(slide, 3);
    REQUIRE(moving.motionKeys == 3);
    const std::pair<float, float> cases[] = {{0.0f, 0.0f}, {0.25f, 0.5f}, {0.5f, 1.0f}, {0.75f, 2.0f}, {1.0f, 3.0f}};
    for (const auto &[t, x] : cases) {
        const Ray r = moving.generateRay(ndc, {0.5f, 0.5f}, t);
        CHECK(length(r.org - (reference.org + Vec3f{x, 0.0f, 0.0f})) < 1e-5f);
        CHECK(length(r.dir - reference.dir) < 1e-6f);
    }

    Camera turning = still;
    const Mat4 turn[2] = {Mat4::identity(), rotationY(90.0)};
    turning.setMotion(turn, 2);
    const Ray centre = turning.generateRay({0.0f, 0.0f}, {0.5f, 0.5f}, 0.5f);
    const float h = std::sqrt(0.5f);
    CHECK(length(centre.dir - Vec3f{-h, 0.0f, -h}) < 1e-5f);

    // One key is no motion: viewToWorld applies.
    Camera single = still;
    single.setMotion(slide + 1, 1);
    CHECK(single.motionKeys == 0);
}

// The depth AOV is measured from the camera the first-hit rays start from,
// the camera at frameTime. A camera moving along its view axis with the
// shutter [0.25, 0.75] after a frame where it is 10 units from a wall: the
// frame is outside the shutter, so frameTime clamps to the shutter open,
// where the camera is 9.75 units away; a centred shutter keeps 10.
TEST_CASE("Depth is measured from the camera at the frame time", "[camera][motion]") {
    Camera c = makePerspectiveCamera({0.0f, 0.0f, 8.0f}, kFocal, kAperture, kWidth, kHeight);
    const Vec3f wall{0.0f, 0.0f, -2.0f};
    CHECK_THAT(c.viewDepth(wall), WithinAbs(10.0, 1e-5));

    const Mat4 late[2] = {translation({0.0f, 0.0f, 7.75f}), translation({0.0f, 0.0f, 7.25f})};
    c.frameTime = 0.0f;
    c.setMotion(late, 2);
    CHECK_THAT(c.viewDepth(wall), WithinAbs(9.75, 1e-5));
    const Ray ray = c.generateRay({0.0f, 0.0f}, {0.5f, 0.5f}, c.frameTime);
    CHECK_THAT(ray.org.z, WithinAbs(7.75 - 0.1, 1e-4)); // starts at the near plane of that camera

    const Mat4 centred[2] = {translation({0.0f, 0.0f, 8.25f}), translation({0.0f, 0.0f, 7.75f})};
    c.frameTime = 0.5f;
    c.setMotion(centred, 2);
    CHECK_THAT(c.viewDepth(wall), WithinAbs(10.0, 1e-5));
}

// A camera sliding 1 unit sideways during the shutter sweeps the image of a
// static edge at depth 10 across f W / (z A) = 35.56 pixels: the coverage is
// a linear ramp of that width centred on the edge.
TEST_CASE("Camera motion blurs a static edge", "[camera][motion]") {
    Camera c = makePerspectiveCamera({0.0f, 0.0f, 0.0f}, kFocal, kAperture, kWidth, kHeight);
    const Mat4 steps[2] = {translation({-0.5f, 0.0f, 0.0f}), translation({0.5f, 0.0f, 0.0f})};
    c.setMotion(steps, 2);
    const double sweep = kFocal * kWidth / (10.0 * kAperture);
    const std::vector<double> alpha = edgeCoverage(c, -10.0f, 64);
    double worst = 0.0;
    for (int x = 96; x < 160; ++x) {
        const double expected = std::clamp(0.5 + (x + 0.5 - 128.0) / sweep, 0.0, 1.0);
        worst = std::fmax(worst, std::fabs(alpha[size_t(x)] - expected));
    }
    CHECK(worst < 0.01);
}

// The card edge sits on the boundary between columns 127 and 128.
TEST_CASE("Depth of field keeps the focus plane sharp", "[camera][dof]") {
    const std::vector<double> alpha = edgeCoverage(2.0f, 10.0f, -10.0f, 16);
    CHECK(alpha[127] == 0.0);
    CHECK(alpha[128] == 1.0);
    CHECK(alpha[100] == 0.0);
    CHECK(alpha[160] == 1.0);
}

// A defocused edge is blurred by the circle of confusion: on the image a
// point at depth z spreads over a disk of radius
//   rho = f^2 |z - s| W / (2 N z s A),
// 22.22 pixels here (Karma measures 22.12 to 22.25), and the coverage across
// the edge is the edge-spread function of that disk.
TEST_CASE("Depth of field blurs a defocused edge by the circle of confusion", "[camera][dof]") {
    const double f = kFocal, s = 10.0, z = 20.0, N = 2.0;
    const double rho = f * f * std::fabs(z - s) * kWidth / (2.0 * N * z * s * kAperture);
    CHECK_THAT(rho, WithinRel(22.222, 1e-3));
    const std::vector<double> alpha = edgeCoverage(float(N), float(s), float(-z), 256);
    for (int x = 100; x < 156; x += 3) {
        const double d = (x + 0.5 - 128.0) / rho; // signed distance from the edge in units of rho
        const double c = std::fmin(1.0, std::fmax(-1.0, d));
        const double expected = 0.5 + (std::asin(c) + c * std::sqrt(1.0 - c * c)) / kPi;
        CHECK_THAT(alpha[size_t(x)], WithinAbs(expected, 0.01));
    }
}
