#include "core/lights/light_sampler.h"
#include "core/lights/lights.h"
#include "core/math/rng.h"
#include "core/math/sampling.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

using namespace nova;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("Rng is deterministic and uniform", "[sampling]") {
    Rng a = Rng::make(42, 7, 1), b = Rng::make(42, 7, 1);
    double sum = 0.0;
    const int n = 100000;
    for (int i = 0; i < n; ++i) {
        const float x = a.next();
        CHECK(x == b.next());
        REQUIRE(x >= 0.0f);
        REQUIRE(x < 1.0f);
        sum += x;
    }
    CHECK_THAT(sum / n, WithinAbs(0.5, 0.005));
    CHECK(Rng::make(42, 7, 1).next() != Rng::make(42, 8, 1).next());
}

TEST_CASE("Cosine hemisphere sampling matches its pdf", "[sampling]") {
    Rng rng = Rng::make(3, 0);
    const int n = 200000;
    double meanCos = 0.0, integral = 0.0;
    for (int i = 0; i < n; ++i) {
        const Vec3f w = sampleCosineHemisphere(rng.next2());
        REQUIRE(w.z >= 0.0f);
        REQUIRE_THAT(length(w), WithinAbs(1.0, 1e-4));
        meanCos += w.z;
        if (w.z > 0.0f)
            integral += 1.0 / cosineHemispherePdf(w.z); // estimates the hemisphere solid angle
    }
    CHECK_THAT(meanCos / n, WithinAbs(2.0 / 3.0, 0.005));
    CHECK_THAT(integral / n, WithinRel(2.0 * kPi, 0.01));
}

TEST_CASE("Uniform cone sampling stays inside the cone", "[sampling]") {
    const float halfAngle = 0.5f * 0.53f * kPi / 180.0f; // the sun
    const float omc = oneMinusCos(halfAngle);
    CHECK_THAT(omc, WithinRel(1.0 - std::cos(double(halfAngle)), 1e-5));

    Rng rng = Rng::make(5, 0);
    double meanOneMinusCos = 0.0;
    const int n = 100000;
    for (int i = 0; i < n; ++i) {
        const Vec3f w = sampleUniformCone(rng.next2(), omc);
        REQUIRE(1.0f - w.z <= omc * 1.0001f);
        meanOneMinusCos += 1.0 - double(w.z);
    }
    // 1 - cos(theta) is uniform on [0, omc].
    CHECK_THAT(meanOneMinusCos / n, WithinRel(0.5 * omc, 0.05));
}

TEST_CASE("Distant light normalization follows UsdLux", "[lights]") {
    LightDesc desc;
    desc.type = LightType::Distant;
    desc.intensity = 3.0f;
    desc.exposure = 1.0f;
    desc.angleDeg = 10.0f;

    // Normalized: irradiance on a facing surface equals intensity * 2^exposure.
    desc.normalize = true;
    LightData light = makeLightData(desc);
    Rng rng = Rng::make(9, 0);
    double irradiance = 0.0;
    const int n = 200000;
    for (int i = 0; i < n; ++i) {
        const LightSample s = sampleDistantLight(light, rng.next2());
        irradiance += s.Li.x * s.wi.z / s.pdf; // light shines straight down onto +Z
    }
    CHECK_THAT(irradiance / n, WithinRel(6.0, 0.002));

    // Unnormalized: intensity is radiance, so the irradiance scales with the cone.
    desc.normalize = false;
    light = makeLightData(desc);
    CHECK_THAT(light.emission.x, WithinRel(6.0, 1e-6));

    // Karma renders angles beyond 180 degrees like 180 (UsdLux would extend
    // the cone with the size factor pi (2 - sin^2)): a hemisphere that still
    // delivers its intensity to a facing surface when normalized.
    desc.normalize = true;
    for (const float angle : {180.0f, 270.0f, 360.0f, 400.0f}) {
        desc.angleDeg = angle;
        light = makeLightData(desc);
        CHECK_THAT(light.oneMinusCosThetaMax, WithinAbs(1.0, 1e-6));
        REQUIRE(isFinite(light.emission));
        double e = 0.0;
        for (int i = 0; i < n; ++i) {
            const LightSample s = sampleDistantLight(light, rng.next2());
            e += s.Li.x * std::fmax(s.wi.z, 0.0f) / s.pdf;
        }
        CHECK_THAT(e / n, WithinRel(6.0, 0.01));
    }

    // Zero angle: a delta light whose emission is the irradiance.
    desc.angleDeg = 0.0f;
    light = makeLightData(desc);
    CHECK(light.delta);
    const LightSample s = sampleDistantLight(light, {0.5f, 0.5f});
    CHECK_THAT(s.wi.z, WithinAbs(1.0, 1e-6));
    CHECK_THAT(s.Li.x / s.pdf, WithinRel(6.0, 1e-6));
}

TEST_CASE("Light sampler selects lights in proportion to their weight", "[lights]") {
    LightSampler sampler;
    sampler.build({1.0f, 0.0f, 3.0f, 4.0f});
    const LightSamplerView view = sampler.view();
    REQUIRE(view.count == 4);
    const double expected[4] = {0.125, 0.0, 0.375, 0.5};
    for (int i = 0; i < 4; ++i)
        CHECK_THAT(view.pmf[i], WithinAbs(expected[i], 1e-7));

    // A stratified sweep over u reproduces the pmf, and every pick reports
    // the probability of the light it picked.
    constexpr int n = 1 << 16;
    int counts[4] = {};
    bool pmfMatches = true;
    for (int i = 0; i < n; ++i) {
        float pmf = 0.0f;
        const uint32_t index = view.sample((float(i) + 0.5f) / float(n), pmf);
        REQUIRE(index < 4);
        pmfMatches &= pmf == view.pmf[index];
        ++counts[index];
    }
    CHECK(pmfMatches);
    for (int i = 0; i < 4; ++i)
        CHECK_THAT(double(counts[i]) / n, WithinAbs(expected[i], 1e-4));

    // Without positive weights nothing is sampled and every pmf is 0.
    sampler.build({0.0f, 0.0f});
    float pmf = 1.0f;
    CHECK(sampler.view().sample(0.5f, pmf) == kInvalidId);
    CHECK(pmf == 0.0f);
    CHECK(sampler.view().pmf[1] == 0.0f);
    sampler.build({});
    CHECK(sampler.view().sample(0.5f, pmf) == kInvalidId);
}

// UsdLux / OpenEXR latlong orientation, as verified against Karma
// (tests/karma/probe_dome_mapping.py).
TEST_CASE("Dome latlong mapping follows UsdLux", "[lights][dome]") {
    LightDesc desc;
    desc.type = LightType::Dome;
    const LightData dome = makeLightData(desc);
    auto uvOf = [&](Vec3f d) { return domeUv(dome, normalize(d)); };
    CHECK_THAT(uvOf({0, 0, 1}).x, WithinAbs(0.5, 1e-6));
    CHECK_THAT(uvOf({0, 0, 1}).y, WithinAbs(0.5, 1e-6));
    CHECK_THAT(uvOf({1, 0, 0}).x, WithinAbs(0.25, 1e-6));
    CHECK_THAT(uvOf({-1, 0, 0}).x, WithinAbs(0.75, 1e-6));
    CHECK_THAT(uvOf({1, 0, 1}).x, WithinAbs(0.375, 1e-6));
    CHECK_THAT(uvOf({0, 1, 0}).y, WithinAbs(0.0, 1e-6));
    CHECK_THAT(uvOf({0, -1, 0}).y, WithinAbs(1.0, 1e-6));

    // Round trip, also through a rotated dome.
    LightDesc rotated = desc;
    rotated.transform.m[1][1] = 0.0;
    rotated.transform.m[1][2] = 1.0;
    rotated.transform.m[2][1] = -1.0;
    rotated.transform.m[2][2] = 0.0; // the pole (+Y) now points along world +Z
    const LightData tilted = makeLightData(rotated);
    float sinTheta = 0.0f;
    CHECK_THAT(domeDirection(tilted, {0.3f, 0.0f}, sinTheta).z, WithinAbs(1.0, 1e-6));
    bool roundTrip = true;
    for (const Vec2f uv : {Vec2f{0.1f, 0.2f}, Vec2f{0.5f, 0.5f}, Vec2f{0.9f, 0.7f}, Vec2f{0.3f, 0.95f}}) {
        for (const LightData *light : {&dome, &tilted}) {
            const Vec2f back = domeUv(*light, domeDirection(*light, uv, sinTheta));
            roundTrip &= std::fabs(back.x - uv.x) < 1e-5f && std::fabs(back.y - uv.y) < 1e-5f;
        }
    }
    CHECK(roundTrip);
}

TEST_CASE("Light power estimates", "[lights]") {
    LightDesc desc;
    desc.type = LightType::Distant;
    desc.intensity = 2.0f;
    desc.normalize = true;
    desc.angleDeg = 10.0f;
    // A normalized distant light delivers its intensity as irradiance, so the
    // flux through a disk of radius 3 is 2 * pi * 9.
    CHECK_THAT(lightPower(makeLightData(desc), 3.0f), WithinRel(2.0 * kPi * 9.0, 1e-4));
    desc.angleDeg = 0.0f;
    CHECK_THAT(lightPower(makeLightData(desc), 3.0f), WithinRel(2.0 * kPi * 9.0, 1e-6));
    // Selection follows the larger of the diffuse and specular multipliers:
    // a light without diffuse still lights highlights.
    desc.diffuse = 0.0f;
    CHECK_THAT(lightPower(makeLightData(desc), 3.0f), WithinRel(2.0 * kPi * 9.0, 1e-6));
    desc.specular = 0.5f;
    CHECK_THAT(lightPower(makeLightData(desc), 3.0f), WithinRel(kPi * 9.0, 1e-6));
    desc.specular = 0.0f;
    CHECK(lightPower(makeLightData(desc), 3.0f) == 0.0f);

    desc.type = LightType::Dome; // left to BSDF sampling
    desc.diffuse = 1.0f;
    desc.specular = 1.0f;
    CHECK(lightPower(makeLightData(desc), 3.0f) == 0.0f);
}

namespace {

Mat4 rotationAboutZ(double degrees) {
    const double a = degrees * kPi / 180.0;
    Mat4 m = Mat4::identity();
    m.m[0][0] = std::cos(a);
    m.m[0][1] = std::sin(a);
    m.m[1][0] = -std::sin(a);
    m.m[1][1] = std::cos(a);
    return m;
}

Mat4 rotationAboutX(double degrees) {
    const double a = degrees * kPi / 180.0;
    Mat4 m = Mat4::identity();
    m.m[1][1] = std::cos(a);
    m.m[1][2] = std::sin(a);
    m.m[2][1] = -std::sin(a);
    m.m[2][2] = std::cos(a);
    return m;
}

Mat4 scaling(double x, double y, double z) {
    Mat4 m = Mat4::identity();
    m.m[0][0] = x;
    m.m[1][1] = y;
    m.m[2][2] = z;
    return m;
}

} // namespace

// A rect rotated about its normal under a non-uniform parent scale is a
// parallelogram; its texture must still map affinely onto it (as in Karma).
TEST_CASE("Rect textures map affinely onto sheared rect lights", "[lights]") {
    LightDesc desc;
    desc.type = LightType::Rect;
    desc.width = 2.0f;
    desc.height = 1.0f;
    desc.rectFlip = RectTextureFlip::None; // u = a, v = 1 - b
    desc.transform = rotationAboutZ(30.0) * scaling(1.0, 3.0, 1.0);
    const LightData d = makeLightData(desc);
    REQUIRE(std::fabs(dot(normalize(d.axisU), normalize(d.axisV))) > 0.5f); // really sheared
    const Vec3f corner = rectCorner(d), ex = d.axisU * 2.0f, ey = d.axisV * 2.0f;
    const std::pair<float, float> points[] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f},
                                              {1.0f, 1.0f}, {0.5f, 0.5f}, {0.25f, 0.75f}};
    for (const auto &[a, b] : points) {
        const Vec2f uv = rectUv(d, corner + ex * a + ey * b);
        CHECK_THAT(uv.x, WithinAbs(a, 1e-5));
        CHECK_THAT(uv.y, WithinAbs(1.0f - b, 1e-5));
    }
}

// The cone test of distant lights must hold for tiny suns, where 1 - cos is
// smaller than the float spacing near 1.
TEST_CASE("Distant light cones are exact for tiny angles", "[lights]") {
    for (const float angle : {0.53f, 0.05f, 0.01f}) {
        LightDesc desc;
        desc.type = LightType::Distant;
        desc.angleDeg = angle;
        desc.transform = rotationAboutX(37.0) * rotationAboutZ(11.0); // a generic direction
        const LightData d = makeLightData(desc);
        REQUIRE_FALSE(d.delta);
        const Frame frame = Frame::fromNormal(d.direction);
        const double half = 0.5 * double(angle) * kPi / 180.0;
        int wrong = 0;
        for (int i = 0; i < 64; ++i) {
            const double phi = kTwoPi * (i + 0.5) / 64.0;
            for (const double scale : {0.98, 1.02}) {
                const double t = half * scale;
                const Vec3f local{float(std::sin(t) * std::cos(phi)), float(std::sin(t) * std::sin(phi)),
                                  float(std::cos(t))};
                wrong += inDistantCone(d, frame.toWorld(local)) != (scale < 1.0);
            }
        }
        INFO("angle " << angle);
        CHECK(wrong == 0);
    }
}

// A rect light with a zero Z scale keeps its shape, and its emission side
// must still follow the rotation (there is no inverse transform).
TEST_CASE("Rect and disk lights keep their normal under a zero Z scale", "[lights]") {
    for (const LightType type : {LightType::Rect, LightType::Disk}) {
        LightDesc desc;
        desc.type = type;
        desc.transform = rotationAboutX(90.0);
        const Vec3f expected = makeLightData(desc).normal;
        desc.transform = scaling(1.0, 1.0, 0.0) * rotationAboutX(90.0);
        const LightData flat = makeLightData(desc);
        CHECK(length(flat.normal - expected) < 1e-6f);
        CHECK(flat.area > 0.0f);
    }
    // Mirrored transforms flip the side, as the inverse transpose does.
    LightDesc desc;
    desc.type = LightType::Rect;
    desc.transform = scaling(1.0, 1.0, -1.0);
    CHECK(length(makeLightData(desc).normal - Vec3f{0.0f, 0.0f, 1.0f}) < 1e-6f);
    desc.transform = scaling(-1.0, 1.0, 1.0);
    CHECK(length(makeLightData(desc).normal - Vec3f{0.0f, 0.0f, -1.0f}) < 1e-6f);
}

// Maps wider than 2048 texels are sampled on a coarser grid. A single bright
// texel must only light up the cells whose bilinear footprint reads it (2 x 2
// cells for 2 x 2 texels per cell), not whole neighbouring blocks.
TEST_CASE("Coarse dome distributions keep bright texels tight", "[lights][dome]") {
    Image image;
    image.width = 4096;
    image.height = 2048;
    image.pixels.assign(size_t(4096) * 2048, Spectrum{0.0f, 0.0f, 0.0f});
    image.pixels[size_t(501) * 4096 + 1001] = Spectrum{1e3f, 1e3f, 1e3f};
    Distribution2D dist;
    REQUIRE(buildDomeDistribution(image, dist));
    const Distribution2DView v = dist.view();
    REQUIRE(v.nu == 2048);
    REQUIRE(v.nv == 1024);
    int lit = 0;
    for (uint32_t y = 0; y < v.nv; ++y)
        for (uint32_t x = 0; x < v.nu; ++x)
            if (v.func[size_t(y) * v.nu + x] > 0.0f) {
                ++lit;
                CHECK((x == 500 || x == 501));
                CHECK((y == 250 || y == 251));
            }
    CHECK(lit == 4);
}

// Invalid input must not poison the paths that sample a light: it emits
// nothing and is never selected.
TEST_CASE("Lights with non-finite emission emit nothing", "[lights]") {
    for (const LightType type : {LightType::Distant, LightType::Dome, LightType::Sphere, LightType::Rect,
                                 LightType::Disk, LightType::Cylinder}) {
        LightDesc desc;
        desc.type = type;
        desc.intensity = std::nanf("");
        LightData d = makeLightData(desc);
        CHECK((d.emission.x == 0.0f && d.emission.y == 0.0f && d.emission.z == 0.0f));
        CHECK(lightPower(d, 3.0f) == 0.0f);

        desc.intensity = 1.0f;
        desc.exposure = 200.0f; // 2^200 overflows float
        d = makeLightData(desc);
        CHECK((d.emission.x == 0.0f && d.emission.y == 0.0f && d.emission.z == 0.0f));
    }
}
