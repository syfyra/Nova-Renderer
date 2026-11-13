// IES profiles on lights (UsdLuxShapingAPI's shaping:ies:*), as Karma
// renders them (core/lights/ies.h; docs/bug-audit-2026-11.md, section 22;
// measured with a point light one unit over a ground):
//   the candela of the direction multiplies the emission (the file's
//   multiplier counts, its lamp count, lumens and ballast factors do not);
//   the vertical angle is measured from the light's -Z, the horizontal one
//   from its -X towards -Y; beyond the file's vertical angles its first or
//   last row holds;
//   normalize divides by the mean over the file's vertical range;
//   angleScale s reads at theta / s (s > 0), at 180 - (180 - theta) / -s
//   (s < 0).

#include "test_helpers.h"

#include "core/lights/ies.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

std::string lobeFile() {
    std::ifstream file(std::string(NOVA_TEST_DIR) + "/data/ies/lobe.ies", std::ios::binary);
    REQUIRE(file);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

// A file of the given angles whose candela is value(vertical, horizontal).
template <class Value>
std::string fileOf(const std::vector<float> &vertical, const std::vector<float> &horizontal, Value &&value,
                   const char *numbers = "1 1000 1", const char *second = "1 1 100", const char *tilt = "TILT=NONE") {
    std::ostringstream s;
    s << "IESNA:LM-63-1995\n[TEST] made by a test\n" << tilt << "\n"
      << numbers << " " << vertical.size() << " " << horizontal.size() << " 1 2 0 0 0\n" << second << "\n";
    for (float v : vertical)
        s << v << " ";
    s << "\n";
    for (float h : horizontal)
        s << h << " ";
    s << "\n";
    for (float h : horizontal) {
        for (float v : vertical)
            s << value(v, h) << " ";
        s << "\n";
    }
    return s.str();
}

float lobe(float v, float h) {
    const float c = std::max(0.0f, std::cos((h - 30.0f) * kPi / 180.0f));
    return 100.0f + 900.0f * std::exp(-((v - 60.0f) / 15.0f) * ((v - 60.0f) / 15.0f)) * c * c * c * c + (v > 120.0f ? 50.0f : 0.0f);
}

} // namespace

TEST_CASE("An IES file is read as Karma reads it", "[lights][ies]") {
    std::string error;
    // The test's file: a lobe 60 degrees from the nadir, 30 degrees around, on a floor of 100.
    const std::shared_ptr<const IesProfile> p = parseIesProfile(lobeFile(), &error);
    REQUIRE(p);
    CHECK(p->photometricType == 1);
    CHECK(p->width > 1);
    CHECK_THAT(iesCandela(*p, 0.0f, 0.0f), WithinAbs(100.0, 0.01));
    CHECK_THAT(iesCandela(*p, 60.0f, 210.0f), WithinAbs(100.0, 0.01));
    CHECK_THAT(iesCandela(*p, 150.0f, 100.0f), WithinAbs(150.0, 0.01));
    // The lobe's top is 1000 in the file and 957.5 in Karma, which draws a table smoothed.
    CHECK_THAT(iesCandela(*p, 60.0f, 30.0f), WithinAbs(957.5, 3.0));
    CHECK_THAT(p->maxCandela, WithinAbs(957.5, 3.0));
    // Karma divides a normalized profile of this file by 144.87.
    CHECK_THAT(p->mean, WithinRel(144.87, 2e-3));
    CHECK_THAT(p->sphereMean, WithinRel(144.87, 2e-3));

    // How Karma draws a table between its entries (measured): one entry of 1000 among zeros, in
    // steps of 5 degrees, is 790 at its angle, 497 half a step away, 105 a step away and gone 1.63 steps
    // away; in steps of the table, so each side of an entry has its own where they differ.
    std::vector<float> fives;
    for (int i = 0; i <= 36; ++i)
        fives.push_back(5.0f * float(i));
    const auto spike = parseIesProfile(fileOf(fives, {0}, [](float v, float) { return v == 30.0f ? 1000.0f : 0.0f; }), &error);
    REQUIRE(spike);
    CHECK_THAT(iesCandela(*spike, 30.0f, 0.0f), WithinAbs(790.0, 3.0));
    CHECK_THAT(iesCandela(*spike, 27.5f, 0.0f), WithinAbs(497.0, 5.0));
    CHECK_THAT(iesCandela(*spike, 32.5f, 0.0f), WithinAbs(497.0, 5.0));
    CHECK_THAT(iesCandela(*spike, 25.0f, 0.0f), WithinAbs(105.0, 6.0));
    CHECK_THAT(iesCandela(*spike, 35.0f, 0.0f), WithinAbs(105.0, 6.0));
    CHECK_THAT(iesCandela(*spike, 23.15f, 0.0f), WithinAbs(9.0, 3.0)); // Karma: 9
    CHECK(std::fabs(iesCandela(*spike, 21.5f, 0.0f)) < 1e-3f);
    CHECK(std::fabs(iesCandela(*spike, 38.5f, 0.0f)) < 1e-3f);
    const auto uneven = parseIesProfile(
        fileOf({0, 5, 10, 15, 20, 25, 30, 31, 32, 33, 34, 35, 40, 90, 180}, {0}, [](float v, float) { return v == 30.0f ? 1000.0f : 0.0f; }), &error);
    REQUIRE(uneven);
    CHECK_THAT(iesCandela(*uneven, 27.5f, 0.0f), WithinAbs(497.0, 8.0)); // Karma: 517 at 27.59, 472 at 27.36
    CHECK_THAT(iesCandela(*uneven, 30.5f, 0.0f), WithinAbs(497.0, 8.0)); // 554 at 30.43
    CHECK_THAT(iesCandela(*uneven, 31.0f, 0.0f), WithinAbs(105.0, 8.0));
    CHECK(std::fabs(iesCandela(*uneven, 32.0f, 0.0f)) < 1e-3f);
    // A step from 0 to 1000 at 30 degrees: 110 at 25, 500 half-way, 896 at 30 (Karma's numbers).
    const auto step = parseIesProfile(fileOf(fives, {0}, [](float v, float) { return v >= 30.0f ? 1000.0f : 0.0f; }), &error);
    REQUIRE(step);
    CHECK_THAT(iesCandela(*step, 25.0f, 0.0f), WithinAbs(110.0, 6.0));
    CHECK_THAT(iesCandela(*step, 27.5f, 0.0f), WithinAbs(500.0, 3.0));
    CHECK_THAT(iesCandela(*step, 30.0f, 0.0f), WithinAbs(896.0, 6.0));
    CHECK_THAT(iesCandela(*step, 33.0f, 0.0f), WithinAbs(1000.0, 3.0));
    // A spike at the nadir: the rows before the first are the first (Karma: 845 at 0.43, 768 at 1, 105 at 5 degrees).
    const auto nadir = parseIesProfile(fileOf(fives, {0}, [](float v, float) { return v == 0.0f ? 1000.0f : 0.0f; }), &error);
    REQUIRE(nadir);
    CHECK_THAT(iesCandela(*nadir, 0.43f, 0.0f), WithinAbs(845.0, 8.0));
    CHECK_THAT(iesCandela(*nadir, 1.0f, 0.0f), WithinAbs(768.0, 8.0));
    CHECK_THAT(iesCandela(*nadir, 5.0f, 0.0f), WithinAbs(105.0, 6.0));
    // The same in the horizontal angle (Karma: 790 at the entry, 497 half a step away, 107 a step away).
    std::vector<float> tens;
    for (int i = 0; i <= 36; ++i)
        tens.push_back(10.0f * float(i));
    const auto ring = parseIesProfile(fileOf({0, 45, 90, 135, 180}, tens, [](float, float h) { return h == 90.0f ? 1000.0f : 0.0f; }), &error);
    REQUIRE(ring);
    CHECK_THAT(iesCandela(*ring, 45.0f, 90.0f), WithinAbs(790.0, 3.0));
    CHECK_THAT(iesCandela(*ring, 45.0f, 85.0f), WithinAbs(497.0, 5.0));
    CHECK_THAT(iesCandela(*ring, 45.0f, 100.0f), WithinAbs(107.0, 6.0));
    CHECK(std::fabs(iesCandela(*ring, 45.0f, 70.0f)) < 1e-3f);

    // The multiplier counts; lamps, lumens and the ballast factors do not (Karma, measured).
    const auto flat = [](float, float) { return 200.0f; };
    for (const char *numbers : {"1 1000 1", "3 1000 1", "1 -1 1", "1 50000 1"}) {
        const auto q = parseIesProfile(fileOf({0, 45, 90, 135, 180}, {0}, flat, numbers), &error);
        REQUIRE(q);
        CHECK(q->width == 1);
        CHECK_THAT(iesCandela(*q, 77.0f, 123.0f), WithinAbs(200.0, 1e-3));
        CHECK_THAT(q->mean, WithinAbs(200.0, 1e-2));
    }
    const auto scaled = parseIesProfile(fileOf({0, 45, 90, 135, 180}, {0}, flat, "1 1000 2.5", "0.5 0.25 100"), &error);
    REQUIRE(scaled);
    CHECK_THAT(iesCandela(*scaled, 10.0f, 0.0f), WithinAbs(500.0, 1e-3));

    // A file that ends at 90 degrees: its last row holds above, and its mean is the lower hemisphere's.
    std::vector<float> lower;
    for (int i = 0; i <= 90; ++i)
        lower.push_back(float(i));
    const auto down = parseIesProfile(fileOf(lower, {0}, [](float v, float) { return 90.0f - v + 3.0f; }), &error);
    REQUIRE(down);
    CHECK_THAT(iesCandela(*down, 45.0f, 0.0f), WithinAbs(48.0, 1e-2)); // straight lines stay straight
    CHECK_THAT(iesCandela(*down, 100.0f, 0.0f), WithinAbs(3.0, 1e-3));
    CHECK_THAT(iesCandela(*down, 170.0f, 0.0f), WithinAbs(3.0, 1e-3));
    // The mean of 93 - theta over the hemisphere: 93 - 180 / pi (the integral of theta sin theta to 90 degrees is 1).
    CHECK_THAT(down->mean, WithinAbs(93.0 - 180.0 / kPi, 0.05));
    CHECK(down->sphereMean < down->mean);
    // One that starts at 90 degrees: its first row holds below.
    const auto up = parseIesProfile(fileOf({90, 120, 150, 180}, {0}, [](float v, float) { return v; }), &error);
    REQUIRE(up);
    CHECK_THAT(iesCandela(*up, 20.0f, 0.0f), WithinAbs(90.0, 1e-3));
    CHECK_THAT(iesCandela(*up, 135.0f, 0.0f), WithinAbs(135.0, 0.2));

    // The horizontal angles a file gives stand for the whole circle.
    const auto wave = [](float, float h) { return 10.0f + h; };
    const auto quadrant = parseIesProfile(fileOf({0, 90, 180}, {0, 15, 30, 45, 60, 75, 90}, wave), &error);
    REQUIRE(quadrant);
    CHECK_THAT(iesCandela(*quadrant, 90.0f, 40.0f), WithinAbs(50.0, 0.05));
    for (const float h : {140.0f, 220.0f, 320.0f}) // 40 degrees from the plane through 0 and 180
        CHECK_THAT(iesCandela(*quadrant, 90.0f, h), WithinAbs(iesCandela(*quadrant, 90.0f, 40.0f), 0.05));
    CHECK_THAT(iesCandela(*quadrant, 90.0f, 270.0f), WithinAbs(iesCandela(*quadrant, 90.0f, 90.0f), 1e-3));
    const auto halves = parseIesProfile(fileOf({0, 90, 180}, {0, 30, 60, 90, 120, 150, 180}, wave), &error);
    REQUIRE(halves);
    CHECK_THAT(iesCandela(*halves, 90.0f, 100.0f), WithinAbs(110.0, 0.05));
    CHECK_THAT(iesCandela(*halves, 90.0f, 260.0f), WithinAbs(110.0, 0.05));
    CHECK_THAT(iesCandela(*halves, 90.0f, 359.0f), WithinAbs(iesCandela(*halves, 90.0f, 1.0f), 0.05));
    const auto across = parseIesProfile(fileOf({0, 90, 180}, {90, 120, 150, 180, 210, 240, 270}, wave), &error);
    REQUIRE(across);
    CHECK_THAT(iesCandela(*across, 90.0f, 170.0f), WithinAbs(180.0, 0.05));
    CHECK_THAT(iesCandela(*across, 90.0f, 10.0f), WithinAbs(180.0, 0.05));                                // 170 mirrored
    CHECK_THAT(iesCandela(*across, 90.0f, 300.0f), WithinAbs(iesCandela(*across, 90.0f, 240.0f), 0.05)); // and 240
    const auto round = parseIesProfile(
        fileOf({0, 90, 180}, {0, 60, 120, 180, 240, 300, 360}, [](float, float h) { return 100.0f + 50.0f * std::sin(h * kPi / 180.0f); }), &error);
    REQUIRE(round);
    CHECK_THAT(iesCandela(*round, 90.0f, 359.5f), WithinAbs(iesCandela(*round, 90.0f, 0.5f), 1.5)); // around the seam
    CHECK(iesCandela(*round, 90.0f, 90.0f) > 130.0f);
    CHECK(iesCandela(*round, 90.0f, 270.0f) < 70.0f);

    // The lamp's tilt data, commas, and text after the numbers.
    std::string tilted = fileOf({0, 90, 180}, {0}, flat, "1 1000 1", "1 1 100", "TILT=INCLUDE\n1\n3\n0 45 90\n1 0.9 0.8");
    std::replace(tilted.begin() + std::ptrdiff_t(tilted.find("TILT")), tilted.end(), ' ', ',');
    tilted += "END\n";
    const auto withTilt = parseIesProfile(tilted, &error);
    REQUIRE(withTilt);
    CHECK_THAT(iesCandela(*withTilt, 90.0f, 0.0f), WithinAbs(200.0, 1e-3));

    // What is no profile.
    CHECK_FALSE(parseIesProfile("", &error));
    CHECK_FALSE(parseIesProfile("IESNA:LM-63-1995\n[TEST] no tilt line\n1 2 3\n", &error));
    CHECK(error.find("TILT") != std::string::npos);
    const std::string whole = fileOf({0, 90, 180}, {0, 180}, flat);
    CHECK_FALSE(parseIesProfile(whole.substr(0, whole.size() - 12), &error)); // a value short
    CHECK_FALSE(parseIesProfile(fileOf({0, 90, 45}, {0}, flat), &error));     // angles that do not rise
    CHECK_FALSE(parseIesProfile(fileOf({0, 90, 200}, {0}, flat), &error));    // beyond 180 degrees
}

namespace {

// A point light at (0, 0, 1) over a white ground at z = 0, seen from above over x, y in -1.5 .. 1.5: the
// radiance of every pixel.
std::vector<float> ground(const LightDesc &light, RenderDevice device, int size, bool &onGpu) {
    Scene scene;
    const MeshId mesh = scene.createMesh();
    scene.updateMesh(mesh, makeQuad(8.0f));
    MaterialDesc white;
    white.inputs = lambertDefaults(spectrum(1.0f));
    const MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, white);
    scene.setMeshMaterials(mesh, {m});
    scene.updateLight(scene.createLight(), light);
    scene.commit();
    Renderer renderer;
    RenderSettings settings;
    settings.device = device;
    settings.maxSamples = 1;
    settings.maxDepth = 1;
    settings.pixelFilterScale = 0.0f;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.5f, size, size));
    renderer.resize(size, size);
    onGpu = true;
    while (!renderer.converged()) {
        renderer.renderPass({});
        onGpu = onGpu && renderer.lastPassOnGpu();
    }
    std::vector<float> out(size_t(size) * size_t(size));
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            out[size_t(y) * size_t(size) + size_t(x)] = pixelMean(renderer.film(), x, y).y;
    return out;
}

LightDesc pointLight() {
    LightDesc d;
    d.type = LightType::Sphere;
    d.treatAsPoint = true;
    d.intensity = 1.0f;
    d.transform.m[3][2] = 1.0f; // at (0, 0, 1): its -Z points at the ground
    return d;
}

} // namespace

TEST_CASE("A light's IES profile multiplies its light by the candela of each direction", "[gpu][lights][ies][render]") {
    std::string error;
    const std::shared_ptr<const IesProfile> profile = parseIesProfile(lobeFile(), &error);
    REQUIRE(profile);
    const int size = 48;
    struct Case {
        const char *name;
        float angleScale;
        bool normalize;
    };
    for (const RenderDevice device : {RenderDevice::Cpu, RenderDevice::Xpu}) {
        bool onGpu = false;
        const std::vector<float> plain = ground(pointLight(), device, size, onGpu);
        if (device == RenderDevice::Xpu && !onGpu) {
            WARN("no GPU: the XPU render ran on the CPU");
            continue;
        }
        for (const Case &c : {Case{"the profile", 0.0f, false}, Case{"normalized", 0.0f, true}, Case{"angles halved", 0.5f, false},
                              Case{"angles doubled", 2.0f, false}, Case{"scaled from the zenith", -0.5f, true}}) {
            LightDesc light = pointLight();
            light.ies = profile;
            light.iesAngleScale = c.angleScale;
            light.iesNormalize = c.normalize;
            const std::vector<float> shaped = ground(light, device, size, onGpu);
            double worst = 0.0, largest = 0.0;
            for (int py = 0; py < size; ++py)
                for (int px = 0; px < size; ++px) {
                    // The film's row 0 is the lowest y.
                    const float x = -1.5f + (float(px) + 0.5f) * 3.0f / float(size), y = -1.5f + (float(py) + 0.5f) * 3.0f / float(size);
                    float theta = std::atan(std::sqrt(x * x + y * y)) * 180.0f / kPi; // from the light's -Z
                    if (c.angleScale > 0.0f)
                        theta /= c.angleScale;
                    else if (c.angleScale < 0.0f)
                        theta = 180.0f - (180.0f - theta) / -c.angleScale;
                    theta = std::clamp(theta, 0.0f, 180.0f);
                    const float phi = std::atan2(-y, -x) * 180.0f / kPi; // from -X towards -Y
                    const double expected = double(iesCandela(*profile, theta, phi)) * (c.normalize ? 1.0 / profile->mean : 1.0);
                    const size_t i = size_t(py) * size_t(size) + size_t(px);
                    REQUIRE(plain[i] > 0.0f);
                    worst = std::max(worst, std::fabs(double(shaped[i]) / plain[i] - expected));
                    largest = std::max(largest, expected);
                }
            INFO((device == RenderDevice::Xpu ? "XPU, " : "CPU, ") << c.name << ": the factor is off by up to " << worst << " of up to " << largest);
            CHECK(worst < 2e-3 * largest);
            CHECK(largest > 0.5);
        }
    }
}

TEST_CASE("An IES profile turns with its light and holds on area lights", "[lights][ies][render]") {
    std::string error;
    const std::shared_ptr<const IesProfile> profile = parseIesProfile(lobeFile(), &error);
    REQUIRE(profile);
    // The lobe is 60 degrees from the nadir at the horizontal angle 30: tan 60 = 1.73 units from the
    // light's foot. Turned by 90 degrees about its axis, the bright spot turns with it.
    const int size = 48;
    bool onGpu = false;
    const auto brightest = [&](const LightDesc &light) {
        const std::vector<float> plain = ground(pointLight(), RenderDevice::Cpu, size, onGpu);
        LightDesc wide = light;
        const std::vector<float> img = ground(wide, RenderDevice::Cpu, size, onGpu);
        size_t best = 0;
        for (size_t i = 0; i < img.size(); ++i)
            if (img[i] / plain[i] > img[best] / plain[best])
                best = i;
        return Vec2f{-1.5f + (float(best % size_t(size)) + 0.5f) * 3.0f / float(size),
                     -1.5f + (float(best / size_t(size)) + 0.5f) * 3.0f / float(size)};
    };
    LightDesc light = pointLight();
    light.ies = profile;
    light.iesAngleScale = 0.5f; // the lobe at 30 degrees: 0.577 from the foot, inside the picture
    const Vec2f spot = brightest(light);
    const float r = std::tan(30.0f * kPi / 180.0f);
    // From -X towards -Y by 30 degrees.
    CHECK_THAT(spot.x, WithinAbs(-r * std::cos(30.0f * kPi / 180.0f), 0.07));
    CHECK_THAT(spot.y, WithinAbs(-r * std::sin(30.0f * kPi / 180.0f), 0.07));
    // The light turned a quarter about its own Z (x -> y, y -> -x): the spot from -Y towards +X.
    LightDesc turned = light;
    turned.transform.m[0][0] = 0.0f;
    turned.transform.m[0][1] = 1.0f;
    turned.transform.m[1][0] = -1.0f;
    turned.transform.m[1][1] = 0.0f;
    const Vec2f turnedSpot = brightest(turned);
    CHECK_THAT(turnedSpot.x, WithinAbs(r * std::sin(30.0f * kPi / 180.0f), 0.07));
    CHECK_THAT(turnedSpot.y, WithinAbs(-r * std::cos(30.0f * kPi / 180.0f), 0.07));

    // A small disk light facing the ground: the same factor in every direction, up to the disk's size.
    LightDesc disk;
    disk.type = LightType::Disk;
    disk.radius = 0.02f;
    disk.normalize = true;
    disk.transform.m[3][2] = 1.0f;
    const std::vector<float> plain = ground(disk, RenderDevice::Cpu, size, onGpu);
    disk.ies = profile;
    disk.iesNormalize = true;
    const std::vector<float> shaped = ground(disk, RenderDevice::Cpu, size, onGpu);
    double worst = 0.0, sum = 0.0;
    for (int py = 0; py < size; ++py)
        for (int px = 0; px < size; ++px) {
            const float x = -1.5f + (float(px) + 0.5f) * 3.0f / float(size), y = -1.5f + (float(py) + 0.5f) * 3.0f / float(size);
            const float theta = std::atan(std::sqrt(x * x + y * y)) * 180.0f / kPi, phi = std::atan2(-y, -x) * 180.0f / kPi;
            const size_t i = size_t(py) * size_t(size) + size_t(px);
            const double d = std::fabs(double(shaped[i]) / plain[i] - double(iesCandela(*profile, theta, phi)) / profile->mean);
            worst = std::max(worst, d);
            sum += d;
        }
    const double mean = sum / double(size * size);
    INFO("a disk of radius 0.02: the factor is off by " << mean << " on average and by up to " << worst);
    // Each pixel has one sample of the disk, a degree and more from its centre's direction: of a factor of
    // up to 6.9, steepest (0.4 per degree) at the lobe's flanks.
    CHECK(mean < 0.03);
    CHECK(worst < 0.5);
}
