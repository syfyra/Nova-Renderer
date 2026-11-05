#include "test_helpers.h"

#include "core/integrator/renderer.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>
#include <tuple>
#include <vector>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

LightId addDistantLight(Scene &scene, float intensity, Mat4 transform = Mat4::identity(), float angleDeg = 0.53f) {
    LightDesc light;
    light.type = LightType::Distant;
    light.intensity = intensity;
    light.normalize = true;
    light.angleDeg = angleDeg;
    light.transform = transform;
    const LightId id = scene.createLight();
    scene.updateLight(id, light);
    return id;
}

Mat4 rotationX(float degrees) {
    const double a = double(degrees) * double(kPi) / 180.0;
    Mat4 m = Mat4::identity();
    m.m[1][1] = std::cos(a);
    m.m[1][2] = std::sin(a);
    m.m[2][1] = -std::sin(a);
    m.m[2][2] = std::cos(a);
    return m;
}

} // namespace

// A white Lambertian sphere inside a white environment of radiance 1 must
// render as exactly 1 everywhere (energy conservation of BSDF + integrator).
TEST_CASE("White furnace", "[render][furnace]") {
    Scene scene;
    const MeshId id = scene.createMesh();
    MeshDesc sphere = makeSphere(1.0f, 128, 64);
    sphere.setDisplayColor(spectrum(1.0f));
    scene.updateMesh(id, sphere);

    LightDesc dome;
    dome.type = LightType::Dome;
    dome.cameraVisible = true; // the background pixels see the dome too
    scene.updateLight(scene.createLight(), dome);
    scene.commit();

    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = 256;
    settings.maxDepth = 64;
    settings.clampIndirect = 0.0f;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.2f, 32, 32));
    renderer.resize(32, 32);
    renderToConvergence(renderer);

    const Film &film = renderer.film();
    double sum = 0.0;
    for (size_t i = 0; i < film.pixelCount(); ++i)
        sum += film.colorSum[i].y / film.samples;
    // Interpolated normals lose a little energy at grazing angles.
    CHECK_THAT(sum / double(film.pixelCount()), WithinAbs(1.0, 0.01));
}

// Direct lighting of a Lambertian plane by a normalized distant light:
// L = albedo / pi * E * cos(theta).
TEST_CASE("Distant light on a Lambertian plane", "[render]") {
    for (const float tilt : {0.0f, 60.0f}) {
        Scene scene;
        const MeshId id = scene.createMesh();
        MeshDesc quad = makeQuad(100.0f);
        quad.setDisplayColor(spectrum(0.5f));
        scene.updateMesh(id, quad);
        addDistantLight(scene, 2.0f, rotationX(tilt));
        scene.commit();

        Renderer renderer;
        RenderSettings settings;
        settings.maxSamples = 16;
        settings.maxDepth = 1; // direct light only
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 1.0f}, 1.0f, 8, 8));
        renderer.resize(8, 8);
        renderToConvergence(renderer);

        const double cosTheta = std::cos(double(tilt) * kPi / 180.0);
        const double expected = 0.5 / kPi * 2.0 * cosTheta;
        // Every pixel sees the same infinite plane; averaging all of them keeps
        // the Monte Carlo error of the tilted cone well below the tolerance.
        double mean = 0.0;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                mean += pixelMean(renderer.film(), x, y).x / 64.0;
        CHECK_THAT(mean, WithinRel(expected, 0.002));
        CHECK_THAT(renderer.film().alphaSum[4 * 8 + 4] / renderer.film().samples, WithinAbs(1.0, 1e-6));
        CHECK(renderer.film().primId[4 * 8 + 4] == -1);
    }
}

// docs/design/M7.md D6: IPR proxy passes render one pixel per 4x4, then per
// 2x2 block first; those samples are the pixels' first samples, so the
// converged film equals a render without proxy passes bit for bit, also
// on a region that is not a multiple of the block size.
TEST_CASE("Proxy passes are the pixels' first samples", "[render][ipr]") {
    Scene scene;
    const MeshId ground = scene.createMesh();
    MeshDesc quad = makeQuad(100.0f);
    quad.setDisplayColor(spectrum(0.5f));
    scene.updateMesh(ground, quad);
    const MeshId ball = scene.createMesh();
    MeshDesc sphere = makeSphere(0.4f, 24, 12);
    for (Vec3f &p : sphere.points)
        p.z += 0.4f;
    sphere.setDisplayColor(Spectrum{0.8f, 0.3f, 0.1f});
    scene.updateMesh(ball, sphere);
    addDistantLight(scene, 2.0f, rotationX(30.0f));
    scene.commit();

    const auto render = [&](int proxyDepth, std::vector<int> *strides, std::vector<size_t> *rendered) {
        auto renderer = std::make_unique<Renderer>();
        RenderSettings settings;
        settings.maxSamples = 5;
        renderer->setScene(&scene);
        renderer->setSettings(settings);
        renderer->setCamera(makeOrthoCamera({0.0f, 0.0f, 3.0f}, 1.0f, 30, 22));
        renderer->resize(30, 22);
        renderer->setRegion(TileRect{1, 2, 27, 21});
        renderer->setProxyDepth(proxyDepth);
        renderer->reset();
        while (!renderer->converged()) {
            REQUIRE(renderer->renderPass({}));
            if (strides)
                strides->push_back(renderer->fillStride());
            if (rendered)
                rendered->push_back(size_t(std::count_if(renderer->film().count.begin(), renderer->film().count.end(),
                                                         [](uint32_t c) { return c > 0; })));
        }
        return renderer;
    };
    std::vector<int> strides;
    std::vector<size_t> rendered;
    const auto plain = render(0, nullptr, nullptr);
    const auto proxied = render(2, &strides, &rendered);
    CHECK(strides == std::vector<int>{4, 2, 1, 1, 1, 1, 1});
    // Region 26 x 19: blocks of 4 -> 7 x 5 pixels, of 2 -> 13 x 10.
    REQUIRE(rendered.size() >= 3);
    CHECK(rendered[0] == 7u * 5u);
    CHECK(rendered[1] == 13u * 10u);
    CHECK(rendered[2] == 26u * 19u);
    const Film &a = plain->film(), &b = proxied->film();
    size_t same = 0, pixels = 0;
    for (size_t i = 0; i < a.colorSum.size(); ++i) {
        if (a.count[i] == 0)
            continue;
        ++pixels;
        same += a.count[i] == 5 && b.count[i] == 5 && a.colorSum[i].x == b.colorSum[i].x &&
                a.colorSum[i].y == b.colorSum[i].y && a.colorSum[i].z == b.colorSum[i].z &&
                a.alphaSum[i] == b.alphaSum[i] && a.depth[i] == b.depth[i] && a.primId[i] == b.primId[i];
    }
    CHECK(pixels == 26u * 19u);
    CHECK(same == pixels);
}

// docs/design/M7.md D4: adaptive sampling. A wide distant light casts a
// noisy penumbra next to smooth, evenly lit ground; quiet pixels stop at the
// minimum, noisy ones go on. Every pixel's sums are exactly those of a
// uniform render at its count (decisions only at power-of-two checkpoints,
// several samples per pixel in one wave when few pixels are left), the
// render does not depend on the thread count, and threshold 0 is uniform.
TEST_CASE("Adaptive sampling stops quiet pixels and keeps their prefix", "[render][adaptive]") {
    Scene scene;
    const MeshId ground = scene.createMesh();
    MeshDesc quad = makeQuad(100.0f);
    quad.setDisplayColor(spectrum(0.5f));
    scene.updateMesh(ground, quad);
    const MeshId ball = scene.createMesh();
    MeshDesc sphere = makeSphere(0.25f, 24, 12);
    for (Vec3f &p : sphere.points)
        p.z += 0.5f;
    scene.updateMesh(ball, sphere);
    addDistantLight(scene, 2.0f, rotationX(20.0f), 30.0f);
    scene.commit();

    const int w = 40, h = 32;
    const auto render = [&](bool adaptive, float threshold, int threads, std::vector<std::vector<Spectrum>> *passes) {
        auto renderer = std::make_unique<Renderer>();
        RenderSettings settings;
        settings.maxSamples = 64;
        settings.adaptive = adaptive;
        settings.oracleMinSamples = 8;
        settings.oracleThreshold = threshold;
        renderer->setThreadLimit(threads);
        renderer->setScene(&scene);
        renderer->setSettings(settings);
        renderer->setCamera(makeOrthoCamera({0.0f, 0.0f, 3.0f}, 1.0f, w, h));
        renderer->resize(w, h);
        renderer->reset();
        while (!renderer->converged()) {
            REQUIRE(renderer->renderPass({}));
            if (passes)
                passes->push_back(renderer->film().colorSum);
        }
        return renderer;
    };
    std::vector<std::vector<Spectrum>> uniformPasses;
    const auto uniform = render(false, 0.01f, 0, &uniformPasses);
    REQUIRE(uniformPasses.size() == 64u);
    const auto adaptive = render(true, 0.01f, 0, nullptr);
    const Film &a = adaptive->film();
    uint32_t lo = 1000, hi = 0;
    size_t prefix = 0;
    for (size_t i = 0; i < a.count.size(); ++i) {
        const uint32_t c = a.count[i];
        lo = std::min(lo, c);
        hi = std::max(hi, c);
        const Spectrum &u = uniformPasses[c - 1][i];
        prefix += (c == 8 || c == 16 || c == 32 || c == 64) && u.x == a.colorSum[i].x && u.y == a.colorSum[i].y &&
                  u.z == a.colorSum[i].z;
    }
    CHECK(lo == 8u);  // the evenly lit ground stops at the minimum
    CHECK(hi == 64u); // the penumbra does not
    CHECK(prefix == a.count.size());
    CHECK(adaptive->activePixels() > 0u); // the penumbra is still noisy at the maximum
    CHECK(adaptive->samplesDone() == 64u);

    // The thread count changes nothing.
    const auto single = render(true, 0.01f, 1, nullptr);
    CHECK(single->film().count == a.count);
    bool same = true;
    for (size_t i = 0; i < a.count.size(); ++i)
        same = same && single->film().colorSum[i].x == a.colorSum[i].x && single->film().alphaSum[i] == a.alphaSum[i];
    CHECK(same);

    // Threshold 0: every pixel is noisy, the render is the uniform one.
    const auto zero = render(true, 0.0f, 0, nullptr);
    bool uniformSame = true;
    for (size_t i = 0; i < a.count.size(); ++i)
        uniformSame = uniformSame && zero->film().count[i] == 64u &&
                      zero->film().colorSum[i].x == uniform->film().colorSum[i].x &&
                      zero->film().colorSum[i].z == uniform->film().colorSum[i].z;
    CHECK(uniformSame);
}

// Wide distant lights are reached by both light and BSDF samples; MIS must
// combine them without bias. Normalized, a 120 degree cone delivers its
// intensity as irradiance to a facing plane, and so does a 300 degree one,
// which acts as 180 degrees (a hemisphere) as in Karma.
TEST_CASE("MIS combines light and BSDF samples of wide distant lights", "[render][mis]") {
    for (const float angle : {120.0f, 300.0f}) {
        Scene scene;
        MeshDesc quad = makeQuad(100.0f);
        quad.setDisplayColor(spectrum(0.5f));
        scene.updateMesh(scene.createMesh(), quad);
        addDistantLight(scene, 2.0f, Mat4::identity(), angle);
        scene.commit();

        Renderer renderer;
        RenderSettings settings;
        settings.maxSamples = 64;
        settings.maxDepth = 1;
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 1.0f}, 1.0f, 8, 8));
        renderer.resize(8, 8);
        renderToConvergence(renderer);

        const double irradiance = 2.0;
        double mean = 0.0;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                mean += pixelMean(renderer.film(), x, y).x / 64.0;
        CHECK_THAT(mean, WithinRel(0.5 / kPi * irradiance, 0.003));
    }
}

// With filter importance sampling, the coverage of a pixel near a straight
// edge is the filter's mass beyond the edge. First-hit AOVs still come from
// the pixel center.
TEST_CASE("Pixel filters weight a straight edge by their profile", "[render][filter]") {
    Scene scene;
    MeshDesc half; // the half-plane x >= 0, facing +Z
    half.points = {{0.0f, -10.0f, 0.0f}, {10.0f, -10.0f, 0.0f}, {10.0f, 10.0f, 0.0f}, {0.0f, 10.0f, 0.0f}};
    half.indices = {0, 1, 2, 0, 2, 3};
    half.userId = 3;
    scene.updateMesh(scene.createMesh(), half);
    scene.commit();

    for (const PixelFilterType type : {PixelFilterType::Box, PixelFilterType::Triangle, PixelFilterType::Gaussian,
                                       PixelFilterType::Blackman, PixelFilterType::BlackmanHarris}) {
        Renderer renderer;
        RenderSettings settings;
        settings.maxSamples = 256;
        settings.maxDepth = 1;
        settings.pixelFilter = type;
        settings.pixelFilterScale = 1.0f;
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 1.0f}, 1.0f, 16, 16)); // the edge is at raster x = 8
        renderer.resize(16, 16);
        renderToConvergence(renderer);
        const Film &film = renderer.film();
        const PixelFilter filter = makePixelFilter(type, 1.0f);

        for (int x = 6; x <= 9; ++x) {
            // Filter mass beyond the edge, which lies at 8 - (x + 0.5) pixels from the center.
            const double d = 8.0 - (x + 0.5), r = filter.radius;
            double beyond = 0.0, total = 0.0;
            for (int i = 0; i < 20000; ++i) {
                const double o = -r + (i + 0.5) * (2.0 * r / 20000.0);
                const double f = pixelFilterProfile(filter, float(o));
                total += f;
                beyond += o > d ? f : 0.0;
            }
            double coverage = 0.0;
            for (int y = 0; y < 16; ++y)
                coverage += film.alphaSum[size_t(y) * 16 + size_t(x)] / film.samples / 16.0;
            CHECK_THAT(coverage, WithinAbs(beyond / total, 0.005));
        }
        CHECK(film.primId[8 * 16 + 7] == -1); // center at x = -0.0625
        CHECK(film.primId[8 * 16 + 8] == 3);  // center at x = +0.0625
    }
}

namespace {

// Mean outgoing radiance of a Lambertian plane (z = 0, albedo 0.5) in a tiny
// orthographic view straight down onto the origin, lit directly by `lights`.
// The view is so small that the irradiance is constant across it. An
// optional black occluder mesh can be added; the camera then sits at
// `cameraZ` below it. `baseZ` moves the whole setup up (coordinates far from
// the origin reduce the float precision).
double planeRadianceBelow(const std::vector<LightDesc> &lights, uint32_t samples, LightStrategy strategy,
                          const MeshDesc *occluder = nullptr, float cameraZ = 5.0f, float baseZ = 0.0f) {
    const Vec3f base{0.0f, 0.0f, baseZ};
    Scene scene;
    MeshDesc quad = makeQuad(100.0f);
    quad.setDisplayColor(spectrum(0.5f));
    for (Vec3f &p : quad.points)
        p += base;
    scene.updateMesh(scene.createMesh(), quad);
    if (occluder) {
        MeshDesc moved = *occluder;
        for (Vec3f &p : moved.points)
            p += base;
        scene.updateMesh(scene.createMesh(), moved);
    }
    for (LightDesc light : lights) {
        light.transform = light.transform * translation(base);
        scene.updateLight(scene.createLight(), light);
    }
    scene.commit();

    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = samples;
    settings.maxDepth = 1;
    settings.lightStrategy = strategy;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera(base + Vec3f{0.0f, 0.0f, cameraZ}, 0.002f, 4, 4)); // looks through the lights
    renderer.resize(4, 4);
    renderToConvergence(renderer);
    double mean = 0.0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            mean += pixelMean(renderer.film(), x, y).x / 16.0;
    return mean;
}

double planeRadianceBelow(const LightDesc &light, uint32_t samples = 256,
                          LightStrategy strategy = LightStrategy::MIS) {
    return planeRadianceBelow(std::vector<LightDesc>{light}, samples, strategy);
}

LightDesc areaLight(LightType type, float intensity, float height) {
    LightDesc light;
    light.type = type;
    light.intensity = intensity;
    light.transform = translation({0.0f, 0.0f, height}); // -Z, the emitting side, faces the plane
    return light;
}

// Parallel-rectangle form factor for a point below one corner (A = a/h, B = b/h).
double cornerFormFactor(double A, double B) {
    const double sa = std::sqrt(1.0 + A * A), sb = std::sqrt(1.0 + B * B);
    return (A / sa * std::atan(B / sa) + B / sb * std::atan(A / sb)) / (2.0 * kPi);
}

} // namespace

// Analytic irradiance at the foot of each light: L_out = albedo / pi * E.
TEST_CASE("Area and point lights match analytic irradiance", "[render][lights]") {
    const double albedoOverPi = 0.5 / kPi;

    SECTION("sphere: E = pi L (r / h)^2") {
        LightDesc light = areaLight(LightType::Sphere, 3.0f, 2.0f);
        light.radius = 0.5f;
        CHECK_THAT(planeRadianceBelow(light), WithinRel(albedoOverPi * kPi * 3.0 * 0.0625, 0.01));
        light.normalize = true; // L = I / (4 pi r^2)
        CHECK_THAT(planeRadianceBelow(light), WithinRel(albedoOverPi * kPi * 3.0 / kPi * 0.0625, 0.01));
    }
    SECTION("point: E = I / h^2, radius ignored") {
        LightDesc light = areaLight(LightType::Sphere, 3.0f, 2.0f);
        light.treatAsPoint = true;
        light.radius = 0.7f;
        CHECK_THAT(planeRadianceBelow(light, 16), WithinRel(albedoOverPi * 3.0 / 4.0, 1e-4));
    }
    SECTION("rect: four corner form factors") {
        LightDesc light = areaLight(LightType::Rect, 2.0f, 1.0f);
        light.width = 2.0f;
        light.height = 1.0f;
        const double formFactor = 4.0 * cornerFormFactor(1.0, 0.5);
        CHECK_THAT(planeRadianceBelow(light), WithinRel(albedoOverPi * kPi * 2.0 * formFactor, 0.01));
        light.normalize = true; // divided by the area, 2
        CHECK_THAT(planeRadianceBelow(light), WithinRel(albedoOverPi * kPi * formFactor, 0.01));
        // A rect scaled by 3 in x has three times the area and form factor of a 6 x 1 rect.
        light.transform = Mat4::identity();
        light.transform.m[0][0] = 3.0;
        light.transform.m[3][2] = 1.0;
        const double scaled = 4.0 * cornerFormFactor(3.0, 0.5);
        CHECK_THAT(planeRadianceBelow(light), WithinRel(albedoOverPi * kPi * (2.0 / 6.0) * scaled, 0.01));
    }
    SECTION("rect: the back side is dark") {
        LightDesc light = areaLight(LightType::Rect, 2.0f, 1.0f);
        light.transform = Mat4::identity();
        light.transform.m[2][2] = -1.0; // mirror: -Z now points up, away from the plane
        light.transform.m[3][2] = 1.0;
        CHECK(planeRadianceBelow(light, 16) == 0.0);
    }
    SECTION("disk: E = pi L R^2 / (h^2 + R^2)") {
        LightDesc light = areaLight(LightType::Disk, 2.0f, 1.0f);
        light.radius = 0.75f;
        CHECK_THAT(planeRadianceBelow(light), WithinRel(albedoOverPi * kPi * 2.0 * 0.5625 / 1.5625, 0.01));
    }
    SECTION("spot: the cone cuts off, its soft edge blends in cos(theta)") {
        LightDesc light = areaLight(LightType::Sphere, 3.0f, 2.0f);
        light.treatAsPoint = true;
        light.shaped = true;
        light.coneAngleDeg = 30.0f;
        CHECK_THAT(planeRadianceBelow(light, 16), WithinRel(albedoOverPi * 3.0 / 4.0, 1e-4)); // on the axis
        // Tilt the spot by 40 degrees about X: the plane point is outside the 30 degree cone.
        Mat4 tilt = Mat4::identity();
        const double a = 40.0 * kPi / 180.0;
        tilt.m[1][1] = std::cos(a);
        tilt.m[1][2] = std::sin(a);
        tilt.m[2][1] = -std::sin(a);
        tilt.m[2][2] = std::cos(a);
        light.transform = tilt * translation({0.0f, 0.0f, 2.0f});
        CHECK(planeRadianceBelow(light, 16) == 0.0);
        // Softness 1 at 40 degrees of a 60 degree cone: t = (cos 40 - cos 60) / (1 - cos 60).
        light.coneAngleDeg = 60.0f;
        light.coneSoftness = 1.0f;
        const double t = (std::cos(a) - 0.5) / 0.5;
        CHECK_THAT(planeRadianceBelow(light, 16), WithinRel(albedoOverPi * 3.0 / 4.0 * t * t * (3.0 - 2.0 * t), 1e-4));
        // Focus: |cos|^focus, tinted towards focusTint off the axis.
        light.coneSoftness = 0.0f;
        light.focus = 3.0f;
        light.focusTint = spectrum(0.25f);
        const double f = std::pow(std::cos(a), 3.0);
        CHECK_THAT(planeRadianceBelow(light, 16), WithinRel(albedoOverPi * 3.0 / 4.0 * (0.25 + 0.75 * f), 1e-4));
    }
}

// Irradiance below a tube along X (radius 0.25, length 2, height 1, L = 2),
// by quadrature over its side: outward normals only, no end caps.
TEST_CASE("Cylinder lights emit from their side only", "[render][lights]") {
    LightDesc light = areaLight(LightType::Cylinder, 2.0f, 1.0f);
    light.radius = 0.25f;
    light.length = 2.0f;
    double expected = 0.0;
    const int n = 2000;
    const double dA = 2.0 * kPi * 0.25 * 2.0 / (double(n) * n);
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            const double x = -1.0 + 2.0 * (i + 0.5) / n, phi = 2.0 * kPi * (j + 0.5) / n;
            const double py = 0.25 * std::cos(phi), pz = 1.0 + 0.25 * std::sin(phi); // surface point
            const double d2 = x * x + py * py + pz * pz, d = std::sqrt(d2);
            const double cosReceiver = pz / d;
            const double cosLight = -(std::cos(phi) * py + std::sin(phi) * pz) / d; // outward normal . (point -> origin)
            if (cosReceiver > 0.0 && cosLight > 0.0)
                expected += 2.0 * cosReceiver * cosLight / d2 * dA;
        }
    }
    CHECK_THAT(planeRadianceBelow(light), WithinRel(0.5 / kPi * expected, 0.01));
}

// A large rect just above the plane is sampled badly by the light (area
// sampling near its plane) and well by the BSDF; MIS must stay unbiased.
TEST_CASE("MIS stays unbiased for a rect light close to the surface", "[render][mis][lights]") {
    LightDesc light = areaLight(LightType::Rect, 1.5f, 0.05f);
    light.width = 4.0f;
    light.height = 4.0f;
    const double formFactor = 4.0 * cornerFormFactor(2.0 / 0.05, 2.0 / 0.05);
    CHECK_THAT(planeRadianceBelow(light, 1024), WithinRel(0.5 / kPi * kPi * 1.5 * formFactor, 0.01));
}

namespace {

Mat4 flipZ() { // 180 degrees about X: local -Z points up (+Z), local +Y to -Y
    Mat4 m = Mat4::identity();
    m.m[1][1] = -1.0;
    m.m[2][2] = -1.0;
    return m;
}

// Irradiance below a tube along X (radius 0.25, length 2, height 1, L = 1)
// by quadrature over its side: outward normals only, no end caps.
double cylinderIrradiance() {
    double e = 0.0;
    const int n = 2000;
    const double dA = 2.0 * kPi * 0.25 * 2.0 / (double(n) * n);
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            const double x = -1.0 + 2.0 * (i + 0.5) / n, phi = 2.0 * kPi * (j + 0.5) / n;
            const double py = 0.25 * std::cos(phi), pz = 1.0 + 0.25 * std::sin(phi);
            const double d2 = x * x + py * py + pz * pz, d = std::sqrt(d2);
            const double cosReceiver = pz / d;
            const double cosLight = -(std::cos(phi) * py + std::sin(phi) * pz) / d;
            if (cosReceiver > 0.0 && cosLight > 0.0)
                e += cosReceiver * cosLight / d2 * dA;
        }
    }
    return e;
}

// Irradiance on the plane (normal +Z) from a dome, by quadrature of its lookup.
double domeIrradiance(const LightDesc &light) {
    const LightData data = makeLightData(light);
    double e = 0.0;
    const int n = 512;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < 4 * n; ++j) {
            const double theta = 0.5 * kPi * (i + 0.5) / n, phi = kTwoPi * (j + 0.5) / (4 * n);
            const Vec3f d{float(std::sin(theta) * std::cos(phi)), float(std::sin(theta) * std::sin(phi)),
                          float(std::cos(theta))};
            e += double(domeRadiance(data, d).x) * std::cos(theta) * std::sin(theta) * (0.5 * kPi / n) *
                 (kTwoPi / (4 * n));
        }
    }
    return e;
}

} // namespace

// Light sampling and BSDF sampling estimate the same integral; each alone and
// MIS must agree with the analytic irradiance. This catches mismatches
// between a light's sampling routine and the pdf used for BSDF hits.
TEST_CASE("Light, BSDF and MIS sampling agree for every light type", "[render][mis][lights]") {
    struct Case {
        const char *name;
        LightDesc light;
        double irradiance;
    };
    std::vector<Case> cases;
    {
        LightDesc wide;
        wide.type = LightType::Distant;
        wide.intensity = 2.0f;
        wide.normalize = true;
        wide.angleDeg = 120.0f;
        cases.push_back({"distant 120 degrees", wide, 2.0});
    }
    {
        LightDesc sphere = areaLight(LightType::Sphere, 3.0f, 2.0f);
        sphere.radius = 0.5f;
        cases.push_back({"sphere", sphere, kPi * 3.0 * 0.0625});
    }
    {
        LightDesc rect = areaLight(LightType::Rect, 2.0f, 1.0f);
        rect.width = 2.0f;
        cases.push_back({"rect", rect, kPi * 2.0 * 4.0 * cornerFormFactor(1.0, 0.5)});
    }
    {
        LightDesc disk = areaLight(LightType::Disk, 2.0f, 1.0f);
        disk.radius = 0.75f;
        cases.push_back({"disk", disk, kPi * 2.0 * 0.5625 / 1.5625});
    }
    {
        LightDesc tube = areaLight(LightType::Cylinder, 1.0f, 1.0f);
        tube.radius = 0.25f;
        tube.length = 2.0f;
        cases.push_back({"cylinder", tube, cylinderIrradiance()});
    }
    {
        // A smooth sky: bright towards the pole, which faces the plane.
        auto sky = std::make_shared<Image>();
        sky->width = 64;
        sky->height = 32;
        sky->pixels.resize(size_t(64) * 32);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 64; ++x)
                sky->pixels[size_t(y) * 64 + size_t(x)] =
                    spectrum(0.1f + 3.0f * std::fmax(0.0f, 0.6f - (float(y) + 0.5f) / 32.0f) *
                                        (1.0f + 0.5f * std::sin(kTwoPi * float(x) / 64.0f)));
        LightDesc dome;
        dome.type = LightType::Dome;
        dome.texture = sky;
        dome.transform.m[1][1] = 0.0; // the pole (+Y) points along +Z, the plane's normal
        dome.transform.m[1][2] = 1.0;
        dome.transform.m[2][1] = -1.0;
        dome.transform.m[2][2] = 0.0;
        cases.push_back({"textured dome", dome, domeIrradiance(dome)});
    }

    for (const Case &c : cases) {
        INFO(c.name);
        const double expected = 0.5 / kPi * c.irradiance;
        CHECK_THAT(planeRadianceBelow(c.light, 256, LightStrategy::MIS), WithinRel(expected, 0.01));
        CHECK_THAT(planeRadianceBelow(c.light, 1024, LightStrategy::LightOnly), WithinRel(expected, 0.01));
        CHECK_THAT(planeRadianceBelow(c.light, 4096, LightStrategy::BsdfOnly), WithinRel(expected, 0.02));
    }
}

// A light lying on a surface (a rect snapped to a ceiling): next-event
// estimation stops shadow rays just short of the light, so BSDF rays must
// see the light up to the same tolerance past the surface they hit, or MIS
// loses part of the light (it was 17% too dark).
//
// Away from the origin the shadow ray's offset origin moves by more than the
// shortening measured from the hit point, so the light is sampled from the
// ray's origin (a receiver at z = 20 under a ceiling at 21 lost all light, at
// z = 2 under 2.5 three quarters). A point light on the ceiling has no BSDF
// counterpart and went black.
TEST_CASE("Lights flush with a surface stay unbiased under MIS", "[render][mis][lights]") {
    struct Case {
        float baseZ, height;
    };
    for (const Case c : {Case{0.0f, 1.0f}, Case{20.0f, 1.0f}, Case{2.0f, 0.5f}, Case{-230.0f, 20.0f}}) {
        INFO("receiver at z = " << c.baseZ << ", ceiling " << c.height << " above");
        LightDesc rect = areaLight(LightType::Rect, 2.0f, c.height); // scaled with the height: same form factor
        rect.width = 2.0f * c.height;
        rect.height = 2.0f * c.height;
        MeshDesc ceiling = makeQuad(100.0f);
        for (Vec3f &p : ceiling.points)
            p.z = c.height;
        ceiling.setDisplayColor(spectrum(0.0f));
        const float cameraZ = 0.5f * c.height;
        const double expected = 0.5 / kPi * kPi * 2.0 * 4.0 * cornerFormFactor(1.0, 1.0);
        const std::vector<LightDesc> lights{rect};
        CHECK_THAT(planeRadianceBelow(lights, 256, LightStrategy::MIS, &ceiling, cameraZ, c.baseZ),
                   WithinRel(expected, 0.01));
        CHECK_THAT(planeRadianceBelow(lights, 1024, LightStrategy::LightOnly, &ceiling, cameraZ, c.baseZ),
                   WithinRel(expected, 0.01));
        CHECK_THAT(planeRadianceBelow(lights, 4096, LightStrategy::BsdfOnly, &ceiling, cameraZ, c.baseZ),
                   WithinRel(expected, 0.02));

        LightDesc point = areaLight(LightType::Sphere, 3.0f, c.height);
        point.treatAsPoint = true;
        CHECK_THAT(planeRadianceBelow(std::vector<LightDesc>{point}, 16, LightStrategy::MIS, &ceiling, cameraZ, c.baseZ),
                   WithinRel(0.5 / kPi * 3.0 / (double(c.height) * c.height), 1e-3));
    }
}

// Karma lights points inside a sphere light (an orb sunk into the floor):
// the inside of the shell emits as well, so a point inside sees radiance L in
// every direction, E = pi L.
TEST_CASE("Sphere lights light points inside them", "[render][mis][lights]") {
    LightDesc sphere = areaLight(LightType::Sphere, 1.0f, 0.3f);
    sphere.radius = 0.5f; // the plane's origin lies inside
    const double expected = 0.5 / kPi * kPi * 1.0;
    CHECK_THAT(planeRadianceBelow(sphere, 256, LightStrategy::MIS), WithinRel(expected, 0.01));
    CHECK_THAT(planeRadianceBelow(sphere, 1024, LightStrategy::LightOnly), WithinRel(expected, 0.01));
    CHECK_THAT(planeRadianceBelow(sphere, 1024, LightStrategy::BsdfOnly), WithinRel(expected, 0.01));
}

// A light with negative intensity subtracts its light (Karma does), so it
// must still be sampled: a point light of -2 at height 2 removes half of a
// normalized sun's irradiance of 1.
TEST_CASE("Negative lights subtract light", "[render][lights]") {
    LightDesc sun;
    sun.type = LightType::Distant;
    sun.intensity = 1.0f;
    sun.normalize = true;
    sun.angleDeg = 0.0f;
    LightDesc point = areaLight(LightType::Sphere, -2.0f, 2.0f);
    point.treatAsPoint = true;
    CHECK_THAT(planeRadianceBelow(std::vector<LightDesc>{sun, point}, 256, LightStrategy::MIS),
               WithinRel(0.5 / kPi * 0.5, 0.01));
}

// The firefly clamp acts on magnitudes: a light of intensity -I renders
// exactly the negative of +I, also where indirect light is clamped (negative
// contributions used to pass the clamp, which biased mixed scenes).
TEST_CASE("The firefly clamp treats negative lights like positive ones", "[render][lights]") {
    const auto render = [](float intensity, float clamp) {
        // A point light 3 cm from a wall lights the floor in front of it
        // directly and through the wall; the wall's light is clamped.
        Scene scene;
        MeshDesc floor = makeQuad(4.0f);
        floor.setDisplayColor(spectrum(0.5f));
        scene.updateMesh(scene.createMesh(), floor);
        MeshDesc wall;
        wall.points = {{0.2f, -4.0f, 0.0f}, {0.2f, 4.0f, 0.0f}, {0.2f, 4.0f, 4.0f}, {0.2f, -4.0f, 4.0f}};
        wall.indices = {0, 1, 2, 0, 2, 3};
        wall.setDisplayColor(spectrum(0.8f));
        scene.updateMesh(scene.createMesh(), wall);
        LightDesc point = areaLight(LightType::Sphere, intensity, 0.5f);
        point.treatAsPoint = true;
        point.transform = translation({0.17f, 0.0f, 0.5f});
        scene.updateLight(scene.createLight(), point);
        scene.commit();

        Renderer renderer;
        RenderSettings settings;
        settings.maxSamples = 32;
        settings.maxDepth = 2;
        settings.clampIndirect = clamp;
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(makeOrthoCamera({-1.0f, 0.0f, 5.0f}, 1.0f, 16, 16));
        renderer.resize(16, 16);
        renderToConvergence(renderer);
        return renderer.film().colorSum;
    };
    const std::vector<Spectrum> positive = render(1.0f, 1.0f), negative = render(-1.0f, 1.0f);
    const std::vector<Spectrum> unclamped = render(1.0f, 0.0f);
    REQUIRE(positive.size() == negative.size());
    size_t clamped = 0, mirrored = 0;
    for (size_t i = 0; i < positive.size(); ++i) {
        clamped += positive[i].x != unclamped[i].x;
        mirrored += negative[i].x == -positive[i].x && negative[i].y == -positive[i].y &&
                    negative[i].z == -positive[i].z;
    }
    CHECK(clamped > 0); // the clamp is active in this scene
    CHECK(mirrored == positive.size());
}

// Karma: an authored cone angle of exactly 90 degrees (the ShapingAPI
// fallback, delivered for every light with the API) does not cut.
TEST_CASE("A 90 degree cone is no cone", "[render][lights]") {
    LightDesc point = areaLight(LightType::Sphere, 3.0f, 2.0f);
    point.treatAsPoint = true;
    point.shaped = true;
    point.transform = flipZ() * translation({0.0f, 0.0f, 2.0f}); // the axis points away from the plane
    point.coneAngleDeg = 90.0f;
    CHECK_THAT(planeRadianceBelow(point, 16), WithinRel(0.5 / kPi * 3.0 / 4.0, 1e-4));
    point.coneAngleDeg = 91.0f; // the plane is 180 degrees off the axis
    CHECK(planeRadianceBelow(point, 16) == 0.0);
}

TEST_CASE("Normalized point lights emit a quarter of their intensity", "[render][lights]") {
    LightDesc point = areaLight(LightType::Sphere, 3.0f, 2.0f);
    point.treatAsPoint = true;
    point.normalize = true;
    CHECK_THAT(planeRadianceBelow(point, 16), WithinRel(0.5 / kPi * 0.75 / 4.0, 1e-4));
}

TEST_CASE("Two-sided rect lights emit from both sides", "[render][lights]") {
    LightDesc rect = areaLight(LightType::Rect, 2.0f, 1.0f);
    rect.transform = flipZ() * translation({0.0f, 0.0f, 1.0f}); // emits away from the plane
    CHECK(planeRadianceBelow(rect, 16) == 0.0);
    rect.twoSided = true;
    CHECK_THAT(planeRadianceBelow(rect), WithinRel(0.5 / kPi * kPi * 2.0 * 4.0 * cornerFormFactor(0.5, 0.5), 0.01));
}

namespace {

// Renders `scene` from an orthographic camera at (0, 0, 5) looking down,
// halfWidth 1, 8 x 8 pixels, with rays through the pixel centers.
Film renderCameraView(const Scene &scene) {
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = 4;
    settings.maxDepth = 1;
    settings.pixelFilterScale = 0.0f; // point filter
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, 8, 8));
    renderer.resize(8, 8);
    renderToConvergence(renderer);
    return renderer.film();
}

Spectrum filmPixel(const Film &film, int x, int y) {
    return film.colorSum[size_t(y) * size_t(film.width) + size_t(x)] / float(film.samples);
}
float filmAlpha(const Film &film, int x, int y) {
    return film.alphaSum[size_t(y) * size_t(film.width) + size_t(x)] / float(film.samples);
}

} // namespace

TEST_CASE("Lights are visible to the camera only when marked", "[render][lights]") {
    LightDesc dome;
    dome.type = LightType::Dome;
    dome.intensity = 0.7f;
    {
        Scene scene;
        scene.updateLight(scene.createLight(), dome);
        scene.commit();
        const Film film = renderCameraView(scene);
        CHECK(filmPixel(film, 3, 3).x == 0.0f);
        CHECK(filmAlpha(film, 3, 3) == 0.0f);
    }
    {
        dome.cameraVisible = true;
        Scene scene;
        scene.updateLight(scene.createLight(), dome);
        scene.commit();
        const Film film = renderCameraView(scene);
        CHECK_THAT(filmPixel(film, 3, 3).x, WithinAbs(0.7, 1e-6));
        CHECK(filmAlpha(film, 3, 3) == 1.0f);
    }
    {
        // A visible rect in front of the dome: its front shows its radiance,
        // its back is black; both are opaque and hide the dome.
        Scene scene;
        scene.updateLight(scene.createLight(), dome);
        LightDesc rect;
        rect.type = LightType::Rect;
        rect.intensity = 2.0f;
        rect.cameraVisible = true;
        rect.transform = flipZ(); // emits up, towards the camera
        const LightId id = scene.createLight();
        scene.updateLight(id, rect);
        scene.commit();
        Film film = renderCameraView(scene);
        CHECK_THAT(filmPixel(film, 3, 3).x, WithinAbs(2.0, 1e-6)); // (-0.125, -0.125) is on the 1 x 1 rect
        CHECK(filmAlpha(film, 3, 3) == 1.0f);
        CHECK_THAT(filmPixel(film, 0, 0).x, WithinAbs(0.7, 1e-6)); // beside the rect: the dome

        rect.transform = Mat4::identity(); // emits down, away from the camera
        scene.updateLight(id, rect);
        scene.commit();
        film = renderCameraView(scene);
        CHECK(filmPixel(film, 3, 3).x == 0.0f);
        CHECK(filmAlpha(film, 3, 3) == 1.0f);
    }
}

// A camera-visible light is opaque in the beauty, so the first-hit AOVs show
// it instead of the surface behind it (Karma's depth and N do the same).
TEST_CASE("First-hit AOVs see camera-visible lights", "[render][lights][aov]") {
    LightDesc rect;
    rect.type = LightType::Rect;
    rect.intensity = 2.0f;
    rect.cameraVisible = true;
    rect.transform = flipZ() * translation({0.0f, 0.0f, 1.0f}); // 1 x 1 at z = 1, emits up
    const size_t onLight = 3 * 8 + 3, beside = 0;               // pixels (3, 3) and (0, 0)
    for (const bool ground : {true, false}) {
        Scene scene;
        if (ground) {
            MeshDesc quad = makeQuad(4.0f);
            quad.userId = 42;
            scene.updateMesh(scene.createMesh(), quad);
        }
        scene.updateLight(scene.createLight(), rect);
        scene.commit();
        const Film film = renderCameraView(scene);
        CHECK_THAT(film.depth[onLight], WithinAbs(4.0, 1e-5));
        CHECK(length(film.normal[onLight] - Vec3f{0.0f, 0.0f, 1.0f}) < 1e-6f);
        CHECK(film.albedo[onLight].x == 0.0f);
        CHECK(film.primId[onLight] == -1);
        CHECK(film.instanceId[onLight] == -1);
        CHECK(film.depth[beside] == (ground ? 5.0f : 0.0f));
        CHECK(film.primId[beside] == (ground ? 42 : -1));
    }
}

// A camera-visible light lying on a wall shows in front of it when it faces
// the camera, from any distance; one a little behind the wall stays hidden.
// The tie tolerance is float precision, not a band relative to the distance
// (1e-4 t let a light 2 mm behind a wall show through it from 50 units).
TEST_CASE("Camera-visible lights on a wall win ties only by precision", "[render][lights]") {
    struct Case {
        const char *name;
        float lightZ;     // the wall is at z = 0, facing the camera above it
        bool facesCamera; // else the light emits into the wall
        float distance;   // camera height
        bool lightWins;
    };
    const Case cases[] = {
        {"flush, facing the camera, near", 0.0f, true, 10.0f, true},
        {"flush, facing the camera, far", 0.0f, true, 1000.0f, true},
        {"2 mm behind, from 10", -0.002f, true, 10.0f, false},
        {"2 mm behind, from 50", -0.002f, true, 50.0f, false},
        {"5 cm behind, from 1000", -0.05f, true, 1000.0f, false},
        {"flush, dark side towards the camera", 0.0f, false, 50.0f, false},
        {"in front, dark side towards the camera", 0.5f, false, 50.0f, true},
    };
    for (const Case &c : cases) {
        INFO(c.name);
        Scene scene;
        MeshDesc wall = makeQuad(100.0f);
        wall.setDisplayColor(spectrum(0.5f));
        wall.userId = 7;
        scene.updateMesh(scene.createMesh(), wall);
        addDistantLight(scene, 1.0f); // the wall shows 0.5 / pi
        LightDesc rect;
        rect.type = LightType::Rect;
        rect.intensity = 5.0f;
        rect.width = 4.0f;
        rect.height = 4.0f;
        rect.cameraVisible = true;
        rect.transform = (c.facesCamera ? flipZ() : Mat4::identity()) * translation({0.0f, 0.0f, c.lightZ});
        scene.updateLight(scene.createLight(), rect);
        scene.commit();

        Renderer renderer;
        RenderSettings settings;
        settings.maxSamples = 4;
        settings.maxDepth = 1;
        settings.pixelFilterScale = 0.0f;
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        // Narrow view; the center pixel looks at the center of the light.
        renderer.setCamera(makePerspectiveCamera({0.0f, 0.0f, c.distance}, 50.0, 1.0, 5, 5, 0.1, 2.0 * c.distance));
        renderer.resize(5, 5);
        renderToConvergence(renderer);
        const Film &film = renderer.film();
        const size_t center = 2 * 5 + 2;
        const float expected = !c.lightWins ? 0.5f / kPi : c.facesCamera ? 5.0f : 0.0f;
        CHECK_THAT(film.colorSum[center].x / film.samples, WithinAbs(expected, 1e-3));
        CHECK(film.alphaSum[center] / film.samples == 1.0f);
        CHECK(film.primId[center] == (c.lightWins ? -1 : 7));
    }
}

namespace {

// Rotation by `degrees` about `axis` (row-vector convention, as Mat4).
Mat4 rotationAbout(Vec3f axis, double degrees) {
    const double l = std::sqrt(double(dot(axis, axis)));
    const double x = axis.x / l, y = axis.y / l, z = axis.z / l;
    const double a = degrees * double(kPi) / 180.0, c = std::cos(a), s = std::sin(a), t = 1.0 - c;
    const double r[3][3] = {{t * x * x + c, t * x * y - s * z, t * x * z + s * y},
                            {t * x * y + s * z, t * y * y + c, t * y * z - s * x},
                            {t * x * z - s * y, t * y * z + s * x, t * z * z + c}}; // column-vector form
    Mat4 m = Mat4::identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m.m[i][j] = r[j][i];
    return m;
}

// A quad of half-size `half` in the local plane z = `z`, baked into world
// space by `rotation` (like SOP output: the corners carry the rotation).
MeshDesc rotatedQuad(float half, float z, const Mat4 &rotation) {
    MeshDesc m = makeQuad(half);
    for (Vec3f &p : m.points)
        p = rotation.transformPoint({p.x, p.y, z});
    return m;
}

struct Orientation {
    Vec3f axis;
    double degrees;
};
const Orientation kOrientations[] = {{{1.0f, 1.0f, 0.0f}, 37.0},  {{0.3f, -0.7f, 0.2f}, 61.0},
                                     {{-0.5f, 0.2f, 0.9f}, 23.0}, {{0.8f, 0.1f, -0.4f}, 77.0},
                                     {{0.1f, 0.9f, 0.3f}, 49.0},  {{-0.6f, -0.6f, 0.5f}, 12.0}};

} // namespace

// The float error of a hit distance grows with the coordinates of the
// triangle hit, not with the camera's or the hit point's: a light flush with a
// large rotated wall seen from close by must not z-fight with it (Embree's
// hit distance on the wall can lie beyond the light), and one 2 mm behind it
// must stay hidden.
TEST_CASE("Camera-visible lights flush with large rotated walls win their ties", "[render][lights]") {
    struct Case {
        float wallHalf, distance, behind;
        int lightPixels; // of 32 x 32
    };
    const Case cases[] = {{100.0f, 1.0f, 0.0f, 1024}, {10.0f, 0.1f, 0.0f, 1024}, {500.0f, 3.0f, 0.0f, 1024},
                          {100.0f, 50.0f, 0.002f, 0}};
    for (const Case &c : cases)
        for (const Orientation &o : kOrientations) {
            INFO("wall half-size " << c.wallHalf << ", camera at " << c.distance << ", light " << c.behind
                                   << " behind, rotated " << o.degrees << " degrees");
            const Mat4 rotation = rotationAbout(o.axis, o.degrees);
            Scene scene;
            MeshDesc wall = rotatedQuad(c.wallHalf, 0.0f, rotation);
            wall.setDisplayColor(spectrum(0.5f));
            scene.updateMesh(scene.createMesh(), wall);
            LightDesc rect;
            rect.type = LightType::Rect;
            rect.intensity = 5.0f;
            rect.width = 0.5f;
            rect.height = 0.5f;
            rect.cameraVisible = true;
            rect.transform = flipZ() * translation({0.0f, 0.0f, -c.behind}) * rotation;
            scene.updateLight(scene.createLight(), rect);
            scene.commit();

            Renderer renderer;
            RenderSettings settings;
            settings.maxSamples = 1;
            settings.maxDepth = 1;
            settings.pixelFilterScale = 0.0f;
            renderer.setScene(&scene);
            renderer.setSettings(settings);
            // Looks at the light's center along the wall normal; the view
            // covers 80 % of the light.
            const double aperture = 1.0, focal = double(c.distance) * aperture / (2.0 * 0.2);
            Camera camera = makePerspectiveCamera({0.0f, 0.0f, 0.0f}, focal, aperture, 32, 32, 0.01, 10.0 * c.distance);
            camera.viewToWorld = translation({0.0f, 0.0f, c.distance}) * rotation;
            camera.worldToView = inverse(camera.viewToWorld);
            renderer.setCamera(camera);
            renderer.resize(32, 32);
            renderToConvergence(renderer);
            int lightPixels = 0;
            for (int i = 0; i < 32 * 32; ++i)
                lightPixels += std::fabs(renderer.film().colorSum[i].x / float(renderer.film().samples) - 5.0f) < 1e-3f;
            CHECK(lightPixels == c.lightPixels);
        }
}

// The same for shadows: a point or rect light flush with a large rotated
// ceiling lights a receiver right below it exactly as under a tiny ceiling,
// although the large ceiling's hit distance can lie beyond the light (shadow
// rays also stop Scene::intersectionTolerance() short of it).
TEST_CASE("Lights flush with large rotated ceilings are not shadowed by them", "[render][lights]") {
    const float h = 0.1f; // ceiling height above the receiver
    uint32_t state = 4242u;
    auto random = [&] {
        state = state * 1664525u + 1013904223u;
        return float(state >> 8) / 16777216.0f;
    };
    for (int t = 0; t < 16; ++t) {
        const Vec3f axis{random() - 0.5f, random() - 0.5f, random() - 0.5f};
        const double degrees = 5.0 + 80.0 * random();
        INFO("rotated " << degrees << " degrees about (" << axis.x << ", " << axis.y << ", " << axis.z << ")");
        const Mat4 rotation = rotationAbout(axis, degrees);
        auto render = [&](float ceilingHalf, const LightDesc &light, uint32_t samples) {
            Scene scene;
            MeshDesc receiver = rotatedQuad(1.0f, 0.0f, rotation), ceiling = rotatedQuad(ceilingHalf, h, rotation);
            receiver.setDisplayColor(spectrum(0.5f));
            ceiling.setDisplayColor(spectrum(0.0f));
            scene.updateMesh(scene.createMesh(), receiver);
            scene.updateMesh(scene.createMesh(), ceiling);
            LightDesc l = light;
            l.transform = l.transform * rotation;
            scene.updateLight(scene.createLight(), l);
            scene.commit();
            Renderer renderer;
            RenderSettings settings;
            settings.maxSamples = samples;
            settings.maxDepth = 1;
            settings.lightStrategy = LightStrategy::LightOnly;
            renderer.setScene(&scene);
            renderer.setSettings(settings);
            Camera camera = makeOrthoCamera({0.0f, 0.0f, 0.0f}, 0.4f * h, 8, 8);
            camera.viewToWorld = translation({0.0f, 0.0f, 0.5f * h}) * rotation; // between the planes, looking down
            camera.worldToView = inverse(camera.viewToWorld);
            renderer.setCamera(camera);
            renderer.resize(8, 8);
            renderToConvergence(renderer);
            std::vector<float> v;
            for (const Spectrum &c : renderer.film().colorSum)
                v.push_back(c.x / float(renderer.film().samples));
            return v;
        };
        LightDesc point = areaLight(LightType::Sphere, 3.0f, h);
        point.treatAsPoint = true;
        LightDesc rect = areaLight(LightType::Rect, 2.0f, h);
        rect.width = 0.5f * h;
        rect.height = 0.5f * h;
        for (const auto &[light, samples] : {std::pair{point, 1u}, std::pair{rect, 64u}}) {
            // Same samples: wherever no shadow ray is blocked the images agree.
            const std::vector<float> small = render(0.3f * h, light, samples), large = render(100.0f, light, samples);
            int darker = 0;
            for (size_t i = 0; i < small.size(); ++i)
                darker += !(large[i] >= small[i] * 0.999f);
            CHECK(darker == 0);
        }
    }
}

// Karma's rect texture orientation (recttextureflip): with the default Flip X
// the image's top-left corner sits at the light's (+X, +Y) corner.
TEST_CASE("Rect light textures are oriented like Karma's", "[render][lights]") {
    auto ramp = std::make_shared<Image>(); // texel (i, j) = ((i + 0.5) / 16, (j + 0.5) / 8, 1), row 0 on top
    ramp->width = 16;
    ramp->height = 8;
    for (int j = 0; j < 8; ++j)
        for (int i = 0; i < 16; ++i)
            ramp->pixels.push_back({(float(i) + 0.5f) / 16.0f, (float(j) + 0.5f) / 8.0f, 1.0f});

    LightDesc rect;
    rect.type = LightType::Rect;
    rect.width = 2.0f;
    rect.texture = ramp;
    rect.cameraVisible = true;
    rect.transform = flipZ(); // seen from its emitting side; local (x, y) appears at (x, -y)
    // Pixel (7, 2) looks at (0.875, -0.375), i.e. the local point (0.875, 0.375).
    for (const auto &[flip, u, v] : {std::tuple{RectTextureFlip::FlipX, 0.0625f, 0.125f},
                                     std::tuple{RectTextureFlip::None, 0.9375f, 0.125f},
                                     std::tuple{RectTextureFlip::FlipY, 0.9375f, 0.875f},
                                     std::tuple{RectTextureFlip::FlipXY, 0.0625f, 0.875f}}) {
        rect.rectFlip = flip;
        Scene scene;
        scene.updateLight(scene.createLight(), rect);
        scene.commit();
        const Film film = renderCameraView(scene);
        const Spectrum c = filmPixel(film, 7, 2);
        CHECK_THAT(c.x, WithinAbs(u, 1e-5));
        CHECK_THAT(c.y, WithinAbs(v, 1e-5));
        CHECK_THAT(c.z, WithinAbs(1.0, 1e-5));
    }
}

namespace {

// Latlong texture: a dim, slightly colored sky with a small bright patch
// near the pole (v small), like a sun in a sky probe.
std::shared_ptr<Image> sunSkyTexture() {
    auto image = std::make_shared<Image>();
    image->width = 128;
    image->height = 64;
    image->pixels.assign(size_t(128) * 64, Spectrum{0.2f, 0.25f, 0.3f});
    for (int y = 10; y < 13; ++y)
        for (int x = 40; x < 44; ++x)
            image->pixels[size_t(y) * 128 + size_t(x)] = spectrum(400.0f);
    return image;
}

// Dome rotated so that its pole (+Y) points along world +Z, the normal of the
// test plane.
Mat4 poleUp() {
    Mat4 m = Mat4::identity();
    m.m[1][1] = 0.0;
    m.m[1][2] = 1.0;
    m.m[2][1] = -1.0;
    m.m[2][2] = 0.0;
    return m;
}

} // namespace

// Irradiance on an upward plane from a textured dome, against quadrature of
// the same radiance lookup over the upper hemisphere. Light samples (bright
// patch) and BSDF samples (dim sky, below the compensation threshold) must
// combine without bias.
TEST_CASE("Textured dome lights are importance sampled without bias", "[render][lights][dome]") {
    LightDesc light;
    light.type = LightType::Dome;
    light.intensity = 0.5f;
    light.texture = sunSkyTexture();
    light.transform = poleUp();

    const LightData data = makeLightData(light);
    double irradiance = 0.0;
    const int n = 1024;
    for (int i = 0; i < n; ++i) {         // theta in (0, pi/2)
        for (int j = 0; j < 4 * n; ++j) { // phi
            const double theta = 0.5 * kPi * (i + 0.5) / n, phi = kTwoPi * (j + 0.5) / (4 * n);
            const Vec3f d{float(std::sin(theta) * std::cos(phi)), float(std::sin(theta) * std::sin(phi)),
                          float(std::cos(theta))};
            const double dOmega = std::sin(theta) * (0.5 * kPi / n) * (kTwoPi / (4 * n));
            irradiance += double(domeRadiance(data, d).x) * std::cos(theta) * dOmega; // red, as planeRadianceBelow
        }
    }
    CHECK_THAT(planeRadianceBelow(light, 1024), WithinRel(0.5 / kPi * irradiance, 0.01));
}

// A constant texture is left to BSDF sampling, exactly like a constant dome.
TEST_CASE("A uniform dome texture equals a constant dome", "[render][lights][dome]") {
    auto image = std::make_shared<Image>();
    image->width = 8;
    image->height = 4;
    image->pixels.assign(32, spectrum(0.75f));
    LightDesc light;
    light.type = LightType::Dome;
    light.texture = image;
    CHECK_THAT(planeRadianceBelow(light, 256), WithinRel(0.5 * 0.75, 0.005));
}

TEST_CASE("Occluders cast shadows from distant lights", "[render]") {
    Scene scene;
    MeshDesc ground = makeQuad(100.0f);
    ground.setDisplayColor(spectrum(0.5f));
    ground.userId = 7;
    scene.updateMesh(scene.createMesh(), ground);
    MeshDesc blocker = makeQuad(0.25f);
    blocker.transform = translation({0.0f, 0.0f, 0.5f});
    scene.updateMesh(scene.createMesh(), blocker);
    addDistantLight(scene, 1.0f);
    scene.commit();

    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = 4;
    settings.maxDepth = 1;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    // Camera between blocker and ground would see the blocker; look from below
    // the blocker plane instead by placing the eye just above the ground.
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 0.25f}, 1.0f, 16, 16));
    renderer.resize(16, 16);
    renderToConvergence(renderer);

    const Film &film = renderer.film();
    CHECK(pixelMean(film, 8, 8).x == 0.0f);   // under the blocker
    CHECK(pixelMean(film, 0, 0).x > 0.1f);    // outside the shadow
    CHECK(film.primId[8 * 16 + 8] == 7);
    CHECK_THAT(film.depth[8 * 16 + 8], WithinAbs(0.25, 1e-5)); // planar camera depth
    CHECK(film.depth[0] > 0.0f);
}

// Direct light from a constant dome on a convex Lambertian: L = albedo * Le.
// maxDepth = 1 means direct light only, which must include the dome.
TEST_CASE("Dome light counts as direct light at maxDepth 1", "[render]") {
    for (const float domeIntensity : {1.0f, 20.0f}) {
        Scene scene;
        MeshDesc sphere = makeSphere(1.0f, 256, 128);
        sphere.setDisplayColor(spectrum(0.8f));
        scene.updateMesh(scene.createMesh(), sphere);
        LightDesc dome;
        dome.type = LightType::Dome;
        dome.intensity = domeIntensity;
        scene.updateLight(scene.createLight(), dome);
        scene.commit();

        Renderer renderer;
        RenderSettings settings;
        settings.maxSamples = 64;
        settings.maxDepth = 1;
        // Default firefly clamp (10): direct dome light (16 for intensity 20) must not be clamped.
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 0.25f, 4, 4));
        renderer.resize(4, 4);
        renderToConvergence(renderer);

        double mean = 0.0;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                mean += pixelMean(renderer.film(), x, y).x / 16.0;
        CHECK_THAT(mean, WithinRel(0.8 * domeIntensity, 0.01));
    }
}

TEST_CASE("Camera rays stop at the far clipping plane", "[render]") {
    Scene scene;
    MeshDesc quad = makeQuad(10.0f);
    quad.transform = translation({0.0f, 0.0f, -30.0f}); // the camera's far plane is at z = -20
    scene.updateMesh(scene.createMesh(), quad);
    scene.commit();

    SurfaceHit hit;
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 0.0f}, 1.0f, 4, 4);
    const Ray ray = camera.generateRay({0.0f, 0.0f});
    CHECK_THAT(ray.tMax, WithinAbs(20.0, 1e-4));
    CHECK_FALSE(scene.intersect(ray, hit));
}

// Neighbouring sub-pixel positions must give distinct directions even far
// from the origin with a tiny near plane (no float cancellation).
TEST_CASE("Primary rays keep their precision at large coordinates", "[render]") {
    const double n = 0.001, f = 10000.0, cot = 1.0 / std::tan(0.5 * 50.0 * kPi / 180.0);
    Mat4 proj{};
    proj.m[0][0] = cot;
    proj.m[1][1] = cot;
    proj.m[2][2] = (f + n) / (n - f);
    proj.m[2][3] = -1.0;
    proj.m[3][2] = 2.0 * f * n / (n - f);
    Camera camera;
    camera.ndcToView = inverse(proj);
    camera.viewToWorld = translation({20000.0f, 5000.0f, 20000.0f});
    camera.worldToView = inverse(camera.viewToWorld);

    const float step = 1e-4f; // about a tenth of a pixel at 2K
    const Ray a = camera.generateRay({0.0f, 0.0f});
    const Ray b = camera.generateRay({step, 0.0f});
    const double expected = double(step) / cot; // d(dir.x) for a small NDC step at the centre
    CHECK_THAT(double(b.dir.x - a.dir.x), WithinRel(expected, 0.05));
    CHECK_THAT(double(a.dir.z), WithinAbs(-1.0, 1e-6));
}

TEST_CASE("Progressive rendering is deterministic", "[render]") {
    auto renderOnce = [] {
        Scene scene;
        MeshDesc sphere = makeSphere(1.0f, 32, 16);
        sphere.setDisplayColor(spectrum(0.7f));
        scene.updateMesh(scene.createMesh(), sphere);
        LightDesc dome;
        dome.type = LightType::Dome;
        scene.updateLight(scene.createLight(), dome);
        addDistantLight(scene, 1.0f, rotationX(30.0f), 5.0f);
        scene.commit();

        Renderer renderer;
        RenderSettings settings;
        settings.maxSamples = 4;
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.2f, 16, 16));
        renderer.resize(16, 16);
        renderToConvergence(renderer);
        return renderer.film().colorSum;
    };
    const auto a = renderOnce();
    const auto b = renderOnce();
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].x == b[i].x);
        CHECK(a[i].y == b[i].y);
        CHECK(a[i].z == b[i].z);
    }
}
