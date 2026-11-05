// Energy bookkeeping of every light type, independent of the integrator:
//  * flux: the irradiance integrated over a large sphere around the light
//    equals the emitted power (pi L A for one-sided Lambertian emitters,
//    pi I for every normalized light, 4 pi I for points), estimated with
//    the light's own samples, so emission normalization and sampling pdf
//    are checked together; lightPower() must report the same number;
//  * pdf normalization: the density that MIS evaluates for a BSDF ray that
//    hits a light (intersectAreaLight + areaLightPdf, domePdf) integrates
//    to 1 over all directions.

#include "test_helpers.h"

#include "core/lights/lights.h"
#include "core/math/sobol.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>
#include <vector>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

Mat4 rotX(double deg) {
    const double a = deg * kPi / 180.0;
    Mat4 m = Mat4::identity();
    m.m[1][1] = std::cos(a);
    m.m[1][2] = std::sin(a);
    m.m[2][1] = -std::sin(a);
    m.m[2][2] = std::cos(a);
    return m;
}

// Owen-scrambled Sobol points of one stream.
Vec2f u2(uint32_t i, uint32_t streamId) { return Sampler::make(7, 3, i).get2D(streamId); }

// Phi = integral over the sphere |x - c| = R of E(x), E estimated with one
// light sample per receiver point; receiver normals point to the center.
double fluxThroughSphere(const LightData &light, double R, uint32_t n) {
    double sum = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        const Vec3f dir = sampleUniformSphere(u2(i, 101));
        const Vec3f x = light.position + dir * float(R);
        const LightSample s = sampleLight(light, x, u2(i, 102));
        if (!(s.pdf > 0.0f))
            continue;
        const float c = dot(-dir, s.wi);
        if (c > 0.0f)
            sum += double(luminance(s.Li)) * c / s.pdf;
    }
    return 4.0 * kPi * R * R * sum / n;
}

// Integral over all directions from o of the area-light pdf at the hit.
double areaPdfIntegral(const LightData &light, Vec3f o, uint32_t n) {
    double sum = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        const Vec3f d = sampleUniformSphere(u2(i, 201));
        float t = 0.0f;
        Vec3f nrm;
        bool front = false;
        if (intersectAreaLight(light, o, d, kInf, t, nrm, front))
            sum += areaLightPdf(light, o, o + d * t, nrm);
    }
    return 4.0 * kPi * sum / n;
}

LightDesc tiltedLight(LightType type, float intensity) {
    LightDesc d;
    d.type = type;
    d.intensity = intensity;
    d.transform = rotX(25.0) * translation({0.3f, -0.2f, 0.5f});
    return d;
}

} // namespace

TEST_CASE("Every light type emits its UsdLux power", "[lights][energy]") {
    struct Case {
        const char *name;
        LightDesc desc;
        double flux;
    };
    std::vector<Case> cases;
    LightDesc rect = tiltedLight(LightType::Rect, 2.0f);
    rect.width = 1.5f;
    rect.height = 0.5f;
    cases.push_back({"rect: pi L w h", rect, kPi * 2.0 * 0.75});
    rect.twoSided = true;
    cases.push_back({"two-sided rect: 2 pi L w h", rect, 2.0 * kPi * 2.0 * 0.75});
    rect.twoSided = false;
    rect.normalize = true;
    cases.push_back({"normalized rect: pi I", rect, kPi * 2.0});

    LightDesc disk = tiltedLight(LightType::Disk, 2.0f);
    disk.radius = 0.4f;
    cases.push_back({"disk: pi L pi r^2", disk, kPi * 2.0 * kPi * 0.16});
    disk.normalize = true;
    cases.push_back({"normalized disk: pi I", disk, kPi * 2.0});

    LightDesc sphere = tiltedLight(LightType::Sphere, 2.0f);
    sphere.radius = 0.3f;
    cases.push_back({"sphere: pi L 4 pi r^2", sphere, kPi * 2.0 * 4.0 * kPi * 0.09});
    sphere.normalize = true;
    cases.push_back({"normalized sphere: pi I", sphere, kPi * 2.0});

    LightDesc tube = tiltedLight(LightType::Cylinder, 2.0f);
    tube.radius = 0.1f;
    tube.length = 1.2f;
    cases.push_back({"cylinder (no caps): pi L 2 pi r l", tube, kPi * 2.0 * kTwoPi * 0.1 * 1.2});
    tube.normalize = true;
    cases.push_back({"normalized cylinder: pi I", tube, kPi * 2.0});

    LightDesc point = tiltedLight(LightType::Sphere, 2.0f);
    point.treatAsPoint = true;
    cases.push_back({"point: 4 pi I", point, 4.0 * kPi * 2.0});
    point.normalize = true;
    cases.push_back({"normalized point: pi I", point, kPi * 2.0});
    point.normalize = false;
    point.shaped = true;
    point.coneAngleDeg = 30.0f; // hard cone
    cases.push_back({"spot: 2 pi (1 - cos 30) I", point, kTwoPi * (1.0 - std::cos(kPi / 6.0)) * 2.0});

    for (const Case &c : cases) {
        INFO(c.name);
        const LightData light = makeLightData(c.desc);
        CHECK_THAT(fluxThroughSphere(light, 40.0, 1u << 20), WithinRel(c.flux, 0.005));
        CHECK_THAT(double(lightPower(light, 1.0f)), WithinRel(c.flux, 1e-4)); // selection weights = power
    }
}

TEST_CASE("Light pdfs used by MIS integrate to one", "[lights][energy][mis]") {
    constexpr uint32_t n = 1u << 22;
    LightDesc rect = tiltedLight(LightType::Rect, 1.0f);
    rect.width = 2.0f;
    rect.height = 1.0f;
    const LightData rectData = makeLightData(rect);
    // Reference points where the rect subtends a large solid angle, so the
    // uniform-direction estimator has low variance: in front (the rect's
    // emitting normal is (0, sin 25, -cos 25)), 0.3 in front of and behind
    // its center (3.85 sr each; the pdf does not depend on the side).
    for (const Vec3f o : {Vec3f{0.0f, 0.0f, -1.5f}, Vec3f{1.0f, 0.5f, -0.2f}, Vec3f{0.3f, -0.073f, 0.228f},
                          Vec3f{0.3f, -0.327f, 0.772f}}) {
        INFO("rect from " << o.x << ", " << o.y << ", " << o.z);
        CHECK_THAT(areaPdfIntegral(rectData, o, n), WithinRel(1.0, 0.01));
    }

    LightDesc disk = tiltedLight(LightType::Disk, 1.0f);
    disk.radius = 0.8f;
    CHECK_THAT(areaPdfIntegral(makeLightData(disk), {0.1f, 0.2f, -1.0f}, n), WithinRel(1.0, 0.01));

    LightDesc sphere = tiltedLight(LightType::Sphere, 1.0f);
    sphere.radius = 0.5f;
    CHECK_THAT(areaPdfIntegral(makeLightData(sphere), {0.3f, -0.2f, 2.5f}, n), WithinRel(1.0, 0.01));

    // Textured dome with a bright patch: pdf over directions, and the pdf
    // reported by sampleLight equals domePdf of the sampled direction.
    auto sky = std::make_shared<Image>();
    sky->width = 64;
    sky->height = 32;
    sky->pixels.assign(64 * 32, Spectrum{0.2f, 0.3f, 0.5f});
    for (int y = 8; y < 11; ++y)
        for (int x = 20; x < 24; ++x)
            sky->pixels[size_t(y) * 64 + size_t(x)] = spectrum(50.0f);
    LightDesc dome;
    dome.type = LightType::Dome;
    dome.texture = sky;
    LightData domeData = makeLightData(dome);
    Distribution2D dist;
    REQUIRE(buildDomeDistribution(*sky, dist));
    domeData.distribution = dist.view();
    double integral = 0.0;
    for (uint32_t i = 0; i < n; ++i)
        integral += domePdf(domeData, sampleUniformSphere(u2(i, 301)));
    CHECK_THAT(4.0 * kPi * integral / n, WithinRel(1.0, 0.01));
    bool consistent = true;
    for (uint32_t i = 0; i < 4096; ++i) {
        const LightSample s = sampleLight(domeData, {0.0f, 0.0f, 0.0f}, u2(i, 302));
        if (s.pdf > 0.0f)
            consistent &= std::fabs(domePdf(domeData, s.wi) / s.pdf - 1.0f) < 1e-3f;
    }
    CHECK(consistent);
}
