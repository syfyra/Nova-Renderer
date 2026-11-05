// Karma's sky dome (docs/design/M11-P3.md): the Hosek-Wilkie sky as Karma
// evaluates it, and its latlong bake for Nova's textured dome light.
//
// The expected values are Karma CPU pixels (Houdini 22.0.466, husk, box
// filter of zero width, so each pixel is the radiance at its center
// direction): tests/husk/karma_sky scenes seen by the 170 degree zenith
// camera, and probes of the horizon (camera along -Z) and the ground color.

#include "core/lights/lights.h"
#include "core/lights/sky.h"
#include "core/math/rng.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <string>

using namespace nova;
using Catch::Matchers::WithinAbs;

namespace {

std::map<std::string, KarmaSkyDesc> karmaConfigs() {
    std::map<std::string, KarmaSkyDesc> c;
    c["sd_default"] = KarmaSkyDesc{};
    KarmaSkyDesc &ground = c["sd_ground"];
    ground.solarAltitude = 20.0f;
    ground.horizonBlur = 0.05f;
    ground.groundAlbedo = {0.8f, 0.3f, 0.1f};
    ground.groundColor = {0.3f, 0.2f, 0.1f};
    KarmaSkyDesc &hazy = c["sd_a15_t10_alb0"];
    hazy.solarAltitude = 15.0f;
    hazy.turbidity = 10.0f;
    hazy.groundAlbedo = spectrum(0.0f);
    KarmaSkyDesc &clear = c["sd_a85_t1_alb1"];
    clear.solarAltitude = 85.0f;
    clear.turbidity = 1.0f;
    clear.groundAlbedo = spectrum(1.0f);
    KarmaSkyDesc &low = c["sd_a5_t2"];
    low.solarAltitude = 5.0f;
    low.turbidity = 2.0f;
    KarmaSkyDesc &east = c["sd_a30_t6_az90"];
    east.solarAltitude = 30.0f;
    east.turbidity = 6.0f;
    east.solarAzimuth = 90.0f;
    KarmaSkyDesc &high = c["sd_a60_t3_az200"];
    high.solarAltitude = 60.0f;
    high.solarAzimuth = 200.0f;
    for (const char *name : {"hz_blur2", "hz_blur0p5", "gc_int2_cg0", "gc_int2_cg1"}) {
        KarmaSkyDesc &d = c[name];
        d.groundColor = {0.3f, 0.2f, 0.1f};
    }
    c["hz_blur2"].horizonBlur = 2.0f;
    c["hz_blur0p5"].horizonBlur = 0.5f;
    c["gc_int2_cg0"].lightColor = {2.0f, 1.0f, 0.5f}; // intensity 2, color (1, 0.5, 0.25)
    c["gc_int2_cg0"].computeGroundColor = false;
    c["gc_int2_cg1"].lightColor = {2.0f, 1.0f, 0.5f};
    return c;
}

struct KarmaPixel {
    const char *config;
    Vec3f dir;
    Spectrum karma;
};

// Directions in the sky's frame (+Y up, the sun at azimuth 0 towards -Z).
const KarmaPixel kKarmaPixels[] = {
    {"sd_default", {0.0710760f, 0.9949354f, -0.0710760f}, {0.02747384f, 0.04787713f, 0.09407558f}},
    {"sd_default", {0.0124631f, 0.1744609f, 0.9845852f}, {0.106064f, 0.1718989f, 0.2555537f}},
    {"sd_default", {0.0098068f, 0.1372770f, -0.9904841f}, {0.2021785f, 0.2721427f, 0.3488571f}},
    {"sd_default", {-0.9949419f, 0.1001970f, -0.0071579f}, {0.1283434f, 0.1883231f, 0.2588592f}},
    {"sd_default", {0.6857442f, 0.0680793f, 0.7246517f}, {0.1562674f, 0.2203974f, 0.2826747f}},
    {"sd_ground", {0.0241265f, 0.3377276f, 0.9409346f}, {0.09925035f, 0.07232089f, 0.1155971f}},
    {"sd_ground", {-0.9161190f, 0.1295354f, -0.3794028f}, {0.1800769f, 0.1025635f, 0.1364406f}},
    {"sd_a15_t10_alb0", {0.0423353f, 0.5926174f, 0.8043707f}, {0.03416153f, 0.04505338f, 0.05959086f}},
    {"sd_a15_t10_alb0", {0.0070573f, 0.0987898f, -0.9950833f}, {1.089709f, 0.7565031f, 0.432203f}},
    {"sd_a85_t1_alb1", {0.0710760f, 0.9949354f, -0.0710760f}, {0.05151444f, 0.09016567f, 0.1550574f}},
    {"sd_a85_t1_alb1", {0.7105616f, 0.0822031f, 0.6988168f}, {0.2207956f, 0.3469538f, 0.4749725f}},
    {"sd_a5_t2", {0.0070573f, 0.0987898f, -0.9950833f}, {0.2690667f, 0.1959014f, 0.09154766f}},
    {"sd_a5_t2", {0.0710760f, 0.9949354f, -0.0710760f}, {0.00833989f, 0.01230002f, 0.01923186f}},
    // Solar azimuth 90 (the sun towards +X) and 200.
    {"sd_a30_t6_az90", {0.9933412f, 0.1149171f, -0.0082094f}, {0.7511198f, 0.699316f, 0.5420547f}},
    {"sd_a30_t6_az90", {-0.9931178f, 0.1168222f, -0.0083455f}, {0.1337949f, 0.1680681f, 0.2078488f}},
    {"sd_a30_t6_az90", {0.3794028f, 0.1295354f, 0.9161190f}, {0.1431929f, 0.170466f, 0.1987838f}},
    {"sd_a60_t3_az200", {-0.6690463f, 0.2401393f, -0.7033564f}, {0.08201985f, 0.1405117f, 0.2298091f}},
    {"sd_a60_t3_az200", {0.7104730f, 0.1227819f, 0.6929305f}, {0.1358636f, 0.2038496f, 0.2945844f}},
    // Horizon blur 2 degrees: in the fade at -0.5 and -1.6 degrees, ground at -3.
    {"hz_blur2", {0.0f, -0.0087296f, -0.9999619f}, {0.2794074f, 0.2544581f, 0.2126443f}},
    {"hz_blur2", {0.0f, -0.0279249f, -0.9996100f}, {0.2979737f, 0.2053594f, 0.1110858f}},
    {"hz_blur2", {0.0f, -0.0523079f, -0.9986310f}, {0.3f, 0.2f, 0.1f}},
    {"hz_blur0p5", {0.0f, -0.0008730f, -0.9999996f}, {0.281275f, 0.2404335f, 0.1835609f}},
    // Light color (2, 1, 0.5): the sky scales; the ground only without
    // computegroundcolor.
    {"gc_int2_cg0", {0.0f, 0.4158833f, -0.9094180f}, {0.30406f, 0.2033011f, 0.1350665f}},
    {"gc_int2_cg0", {0.0f, -0.4158833f, -0.9094180f}, {0.6f, 0.2f, 0.05f}},
    {"gc_int2_cg1", {0.0f, -0.4158833f, -0.9094180f}, {0.3f, 0.2f, 0.1f}},
};

// A dome light holding the baked sky, in the identity frame.
LightData skyDome(const std::shared_ptr<Image> &image) {
    LightDesc desc;
    desc.type = LightType::Dome;
    desc.texture = image;
    return makeLightData(desc);
}

} // namespace

TEST_CASE("Karma sky radiance matches Karma's pixels", "[sky]") {
    const auto configs = karmaConfigs();
    for (const KarmaPixel &p : kKarmaPixels) {
        INFO(p.config << " dir (" << p.dir.x << ", " << p.dir.y << ", " << p.dir.z << ")");
        const Spectrum L = karmaSkyRadiance(configs.at(p.config), p.dir);
        for (int c = 0; c < 3; ++c)
            CHECK_THAT(L[c], WithinAbs(p.karma[c], 2e-4 * std::fabs(p.karma[c]) + 1e-7));
    }
}

TEST_CASE("Karma sky input ranges", "[sky]") {
    // Karma clamps the solar altitude to [0, 90], the turbidity to [1, 10]
    // and the albedo to [0, 1] (measured: altitude -10 renders as 0, 100 as
    // 90, turbidity 0.5 as 1, 12 as 10, albedo (-0.5, 1.5, 2) as (0, 1, 1)).
    const Vec3f dirs[] = {{0.0f, 1.0f, 0.0f}, {0.3f, 0.4f, -0.8f}, {-0.6f, 0.2f, 0.5f}};
    auto same = [&](KarmaSkyDesc a, KarmaSkyDesc b) {
        for (Vec3f d : dirs) {
            const Spectrum x = karmaSkyRadiance(a, d), y = karmaSkyRadiance(b, d);
            CHECK(x.x == y.x);
            CHECK(x.y == y.y);
            CHECK(x.z == y.z);
        }
    };
    KarmaSkyDesc a, b;
    a.solarAltitude = -10.0f;
    b.solarAltitude = 0.0f;
    same(a, b);
    a.solarAltitude = 100.0f;
    b.solarAltitude = 90.0f;
    same(a, b);
    a = b = KarmaSkyDesc{};
    a.turbidity = 0.5f;
    b.turbidity = 1.0f;
    same(a, b);
    a.turbidity = 12.0f;
    b.turbidity = 10.0f;
    same(a, b);
    a = b = KarmaSkyDesc{};
    a.groundAlbedo = {-0.5f, 1.5f, 2.0f};
    b.groundAlbedo = {0.0f, 1.0f, 1.0f};
    same(a, b);
    // Without a horizon blur the ground starts right below the horizon.
    KarmaSkyDesc hard;
    hard.horizonBlur = 0.0f;
    hard.groundColor = {0.3f, 0.2f, 0.1f};
    CHECK(karmaSkyRadiance(hard, {0.0f, -1e-4f, -1.0f}).x == 0.3f);
    CHECK(karmaSkyRadiance(hard, {0.0f, 1e-4f, -1.0f}).y > 0.25f);
}

TEST_CASE("The baked Karma sky reproduces the model", "[sky]") {
    KarmaSkyDesc desc;
    desc.solarAltitude = 30.0f;
    desc.solarAzimuth = 40.0f;
    desc.turbidity = 2.5f;
    desc.groundColor = {0.05f, 0.04f, 0.03f};
    const std::shared_ptr<Image> image = bakeKarmaSky(desc, 1024);
    REQUIRE(image->width == 1024);
    REQUIRE(image->height == 512);
    const LightData dome = skyDome(image);

    // Random directions above the fade (and below the blur, the flat
    // ground): the bilinear lookup in the dome mapping stays within 0.5 %
    // of the exact radiance (measured worst case 0.3 % at this width).
    Rng rng = Rng::make(7, 0);
    double worst = 0.0;
    for (int i = 0; i < 20000; ++i) {
        const float z = 2.0f * rng.next() - 1.0f, phi = kTwoPi * rng.next();
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const Vec3f dir{r * std::cos(phi), z, r * std::sin(phi)};
        if (dir.y > -0.02f && dir.y < 0.03f)
            continue; // the fade and the sharp horizon brightening
        const Spectrum exact = karmaSkyRadiance(desc, dir), baked = domeRadiance(dome, dir);
        for (int c = 0; c < 3; ++c)
            worst = std::max(worst, std::fabs(double(baked[c]) - exact[c]) / std::max(1e-6, double(exact[c])));
    }
    CHECK(worst < 0.005);

    // The irradiance of an upward surface from the bake (texel sums) equals
    // the model's (a fine midpoint quadrature) within 0.1 %.
    double bakedE[3] = {}, exactE[3] = {};
    for (int j = 0; j < image->height / 2; ++j) {
        const double theta = kPi * (j + 0.5) / image->height;
        const double w = std::cos(theta) * std::sin(theta) * (kPi / image->height) * (2.0 * kPi / image->width);
        for (int i = 0; i < image->width; ++i)
            for (int c = 0; c < 3; ++c)
                bakedE[c] += w * image->pixels[size_t(j) * image->width + i][c];
    }
    const int nt = 1000, np = 2000;
    for (int j = 0; j < nt; ++j) {
        const double theta = 0.5 * kPi * (j + 0.5) / nt;
        const double w = std::cos(theta) * std::sin(theta) * (0.5 * kPi / nt) * (2.0 * kPi / np);
        for (int i = 0; i < np; ++i) {
            const double phi = 2.0 * kPi * (i + 0.5) / np;
            const Spectrum L = karmaSkyRadiance(
                desc, {float(std::sin(theta) * std::sin(phi)), float(std::cos(theta)), float(std::sin(theta) * std::cos(phi))});
            for (int c = 0; c < 3; ++c)
                exactE[c] += w * L[c];
        }
    }
    for (int c = 0; c < 3; ++c)
        CHECK_THAT(bakedE[c], WithinAbs(exactE[c], 1e-3 * exactE[c]));
}

TEST_CASE("Karma sky bake time", "[.][sky][bench]") {
    const KarmaSkyDesc desc;
    for (const int width : {1024, 2048, 4096}) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::shared_ptr<Image> image = bakeKarmaSky(desc, width);
        const auto t1 = std::chrono::steady_clock::now();
        Distribution2D distribution;
        buildDomeDistribution(*image, distribution);
        const auto t2 = std::chrono::steady_clock::now();
        using Ms = std::chrono::duration<double, std::milli>;
        const double bake = Ms(t1 - t0).count(), cdf = Ms(t2 - t1).count();
        WARN("width " << width << ": bake " << bake << " ms, distribution " << cdf << " ms");
    }
}
