// Emissive meshes as lights (docs/design/M9.md D14): triangle sampling, and
// treated emitters render the same image as untreated ones with far less
// noise, as Karma's treat_as_lightsource does (research lights.md 3.3).

#include "test_helpers.h"

#include "core/lights/lights.h"
#include "core/math/rng.h"
#include "core/math/sampling.h"
#include "core/scene/scene.h"
#include "core/shading/compiler.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <vector>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace sh = nova::shading;

TEST_CASE("Triangle light samples match their densities", "[lights][emitters]") {
    Rng rng = Rng::make(5u, 9u, 2u);
    const auto point = [&](float s) {
        return Vec3f{s * (2.0f * rng.next() - 1.0f), s * (2.0f * rng.next() - 1.0f), s * (2.0f * rng.next() - 1.0f)};
    };
    int spherical = 0;
    for (int k = 0; k < 400; ++k) {
        const Vec3f v0 = point(1.0f), e1 = point(1.0f), e2 = point(1.0f);
        const Vec3f ref = point(k % 2 ? 0.6f : 6.0f); // near (spherical) and far (area) references
        const Vec3f nc = cross(e1, e2);
        const float area = 0.5f * length(nc);
        if (area < 1e-3f)
            continue;
        const Vec3f n = normalize(nc);
        const float omega = triangleSolidAngle(v0, e1, e2, ref);
        spherical += sphericalTriangleSampled(omega);
        double inverse = 0.0;
        constexpr int kSamples = 4000;
        for (int i = 0; i < kSamples; ++i) {
            Vec2f bary;
            const ShapeSample s = sampleTriangle(v0, e1, e2, ref, rng.next2(), bary);
            REQUIRE(s.pdf > 0.0f);
            REQUIRE(bary.x >= 0.0f);
            REQUIRE(bary.y >= 0.0f);
            REQUIRE(bary.x + bary.y <= 1.0f + 1e-6f);
            const Vec3f p = v0 + e1 * bary.x + e2 * bary.y;
            REQUIRE(length(p - s.p) <= 1e-5f * (1.0f + length(p)));
            // The density pdf() reports is the one the sample came with.
            REQUIRE_THAT(triangleSamplePdf(v0, e1, e2, n, area, ref, s.p), WithinRel(s.pdf, 1e-3f));
            inverse += 1.0 / double(s.pdf);
        }
        // E[1 / pdf] is the solid angle.
        INFO("triangle " << k << ", solid angle " << omega);
        CHECK_THAT(inverse / kSamples, WithinRel(double(omega), 0.03));
    }
    CHECK(spherical > 100);
}

namespace {

// research lights.md 3.3's "pair" scene, scaled down: a grey ground seen
// from above, an emissive polygon sphere (radius 0.5, centre 1 above the
// ground) and an emissive quad facing away from the ground; both emit (2, 1,
// 0.5) with a black BSDF (Standard Surface, base and specular 0).
struct PairScene {
    Scene scene;
    MeshId sphere = kInvalidId, quad = kInvalidId;

    explicit PairScene(const MeshLightSource &treat, bool varyingQuad = false) {
        const MeshId ground = scene.createMesh();
        MeshDesc g = makeQuad(6.0f);
        g.setDisplayColor(spectrum(0.5f));
        scene.updateMesh(ground, g);

        MaterialDesc emissive;
        emissive.inputs = standardSurfaceDefaults();
        emissive.inputs.set(surface_field::baseWeight, 0.0f);
        emissive.inputs.set(surface_field::specularWeight, 0.0f);
        emissive.inputs.set(surface_field::emissionWeight, 1.0f);
        emissive.inputs.set(surface_field::emissionColor, Spectrum{2.0f, 1.0f, 0.5f});
        const MaterialId m = scene.createMaterial();
        scene.updateMaterial(m, emissive);

        sphere = scene.createMesh();
        MeshDesc s = makeSphere(0.5f, 32, 16);
        s.transform = translation({-1.5f, 0.0f, 1.0f});
        scene.updateMesh(sphere, s);
        scene.setMeshMaterials(sphere, {m});
        scene.setMeshLightSource(sphere, treat);

        // The quad faces away from the ground (two-sided emission, Karma).
        quad = scene.createMesh();
        MeshDesc q = makeQuad(0.5f);
        q.transform = translation({1.5f, 0.0f, 1.0f});
        MaterialId mq = m;
        if (varyingQuad) {
            // Emission from a primvar: per-hit, so its power is estimated.
            q.setAttribute({"glow", AttributeRate::Vertex, 1, {1.0f, 3.0f, 1.0f, 3.0f}});
            sh::IrGraph graph;
            graph.model = SurfaceModel::StandardSurface;
            sh::IrBuilder b(graph);
            const sh::Ref glow = b.primvar("glow", 1, b.constant(0.0f));
            b.output(surface_field::emissionColor, 3, b.mul(glow, b.constant(1.0f, 0.5f, 0.25f)));
            const sh::CompileResult compiled = sh::compile(graph);
            REQUIRE(compiled.program);
            MaterialDesc varying = emissive;
            varying.program = compiled.program;
            mq = scene.createMaterial();
            scene.updateMaterial(mq, varying);
            REQUIRE(scene.material(mq).emission == MaterialRecord::Emission::Varying);
        }
        scene.updateMesh(quad, q);
        scene.setMeshMaterials(quad, {mq});
        scene.setMeshLightSource(quad, treat);
        scene.commit();
    }
};

// The ground's red channel, 48 x 48 pixels over [-3, 3]^2.
std::vector<float> renderPair(const Scene &scene, uint32_t samples, uint32_t seed) {
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = samples;
    settings.seed = seed;
    settings.clampIndirect = 0.0f;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 3.0f, 48, 48));
    renderer.resize(48, 48);
    renderToConvergence(renderer);
    std::vector<float> red(48 * 48);
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 48; ++x)
            red[size_t(y) * 48 + size_t(x)] = pixelMean(renderer.film(), x, y).x;
    return red;
}

// World position of a pixel's centre on the ground.
Vec3f pixelPoint(int x, int y) {
    return {-3.0f + 6.0f * (float(x) + 0.5f) / 48.0f, -3.0f + 6.0f * (float(y) + 0.5f) / 48.0f, 0.0f};
}

// Ground pixels (outside the emitters' footprints) near (< 1.2) or far (>= 2)
// from either emitter's centre.
bool groundPixel(int x, int y, bool far) {
    const Vec3f p = pixelPoint(x, y);
    const float ds = length(Vec3f{p.x + 1.5f, p.y, 0.0f}), dq = length(Vec3f{p.x - 1.5f, p.y, 0.0f});
    if (ds < 0.6f || (std::fabs(p.x - 1.5f) < 0.6f && std::fabs(p.y) < 0.6f))
        return false;
    const float d = std::min(ds, dq);
    return far ? d >= 2.0f : d < 1.2f;
}

double regionMean(const std::vector<float> &img, bool far) {
    double sum = 0.0;
    int n = 0;
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 48; ++x)
            if (groundPixel(x, y, far)) {
                sum += img[size_t(y) * 48 + size_t(x)];
                ++n;
            }
    return sum / n;
}

// Noise of a region: the variance of the difference of two renders with
// different seeds, relative to the squared mean.
double regionNoise(const std::vector<float> &a, const std::vector<float> &b, bool far) {
    double diff = 0.0, mean = 0.0;
    int n = 0;
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 48; ++x)
            if (groundPixel(x, y, far)) {
                const size_t i = size_t(y) * 48 + size_t(x);
                diff += double(a[i] - b[i]) * double(a[i] - b[i]);
                mean += 0.5 * double(a[i] + b[i]);
                ++n;
            }
    mean /= n;
    return 0.5 * diff / n / (mean * mean);
}

} // namespace

TEST_CASE("Emissive meshes treated as lights render the same with less noise", "[lights][emitters][render]") {
    MeshLightSource off, on;
    on.mode = 1;
    PairScene plain(off), treated(on);
    REQUIRE(plain.scene.lights().empty());
    // 32 x 16 quads -> 1024 triangles, and the quad's 2.
    REQUIRE(treated.scene.lights().size() == 32u * 16u * 2u + 2u);
    REQUIRE(treated.scene.analyticLightCount() == 0u);
    CHECK(treated.scene.emitterLight(treated.quad, 0, 1) != kInvalidId);

    // The same means (the untreated render converges slowly far away).
    const std::vector<float> a = renderPair(plain.scene, 1024, 1), b = renderPair(treated.scene, 256, 1);
    for (const bool far : {false, true}) {
        INFO((far ? "far" : "near") << ": untreated " << regionMean(a, far) << ", treated " << regionMean(b, far));
        CHECK_THAT(regionMean(b, far), WithinRel(regionMean(a, far), far ? 0.03 : 0.01));
    }
    // At 16 samples the far ground's relative noise drops at least 5x
    // (Karma: 12x for the sphere, 18x for the quad).
    const std::vector<float> a1 = renderPair(plain.scene, 16, 2), a2 = renderPair(plain.scene, 16, 3);
    const std::vector<float> b1 = renderPair(treated.scene, 16, 2), b2 = renderPair(treated.scene, 16, 3);
    const double noisePlain = regionNoise(a1, a2, true), noiseTreated = regionNoise(b1, b2, true);
    INFO("relative variance far: untreated " << noisePlain << ", treated " << noiseTreated);
    CHECK(noiseTreated * 25.0 < noisePlain);
}

TEST_CASE("Emitter light multipliers scale receivers, not the camera's view", "[lights][emitters][render]") {
    MeshLightSource on, doubled;
    on.mode = 1;
    doubled.mode = 2; // auto: the material emits
    doubled.diffuse = 2.0f;
    PairScene base(on), scaled(doubled);
    const std::vector<float> a = renderPair(base.scene, 128, 4), b = renderPair(scaled.scene, 128, 4);
    CHECK_THAT(regionMean(b, false), WithinRel(2.0 * regionMean(a, false), 0.01));
    // The sphere seen from above: its emission, unscaled.
    const size_t centre = size_t(24) * 48 + 11; // (-1.5625, 0.0625)
    CHECK_THAT(a[centre], WithinAbs(2.0, 1e-4));
    CHECK_THAT(b[centre], WithinAbs(2.0, 1e-4));
}

TEST_CASE("Commit times with many emissive triangles", "[.bench][emitters]") {
    // Not run by default: nova_tests "[.bench]" prints the times of the first
    // commit and of a light edit with a 1M-triangle emissive mesh.
    Scene scene;
    MeshDesc grid;
    const int n = 708; // 708^2 * 2 = 1M triangles
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x)
            grid.points.push_back({float(x) / n, float(y) / n, std::sin(0.05f * float(x + y))});
    for (uint32_t y = 0; y < uint32_t(n); ++y)
        for (uint32_t x = 0; x < uint32_t(n); ++x) {
            const uint32_t a = y * uint32_t(n + 1) + x, b = a + uint32_t(n + 1);
            grid.indices.insert(grid.indices.end(), {a, a + 1, b + 1, a, b + 1, b});
        }
    MaterialDesc emissive;
    emissive.inputs = standardSurfaceDefaults();
    emissive.inputs.set(surface_field::emissionWeight, 1.0f);
    const MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, emissive);
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, grid);
    scene.setMeshMaterials(id, {m});
    MeshLightSource on;
    on.mode = 1;
    scene.setMeshLightSource(id, on);
    LightDesc key;
    key.type = LightType::Sphere;
    const LightId light = scene.createLight();
    scene.updateLight(light, key);
    auto t0 = std::chrono::steady_clock::now();
    scene.commit();
    auto t1 = std::chrono::steady_clock::now();
    key.intensity = 2.0f;
    scene.updateLight(light, key);
    scene.commit();
    auto t2 = std::chrono::steady_clock::now();
    using Ms = std::chrono::duration<double, std::milli>;
    const double first = Ms(t1 - t0).count(), edit = Ms(t2 - t1).count();
    WARN("lights " << scene.lights().size() << ": first commit " << first << " ms, light edit " << edit << " ms");
}

TEST_CASE("Varying emission is estimated and stays unbiased", "[lights][emitters][render]") {
    MeshLightSource off, on;
    on.mode = 1;
    PairScene plain(off, true), treated(on, true);
    // The quad's estimate: the primvar's mean at the centroid and corners.
    const LightData &l = treated.scene.lights()[treated.scene.emitterLight(treated.quad, 0, 0)];
    CHECK(l.type == LightType::Triangle);
    CHECK(l.emission.x > 1.0f);
    CHECK(l.emission.x < 3.0f);
    const std::vector<float> a = renderPair(plain.scene, 1024, 5), b = renderPair(treated.scene, 256, 5);
    CHECK_THAT(regionMean(b, false), WithinRel(regionMean(a, false), 0.01));
}
