#if defined(_MSC_VER)
#pragma warning(disable : 4702) // unreachable code in NanoVDB's grid builder templates
#endif

// docs/design/M10.md G1: Nova XPU renders the CPU's images. With the same
// Sobol streams both devices make the same decisions per path up to
// floating-point differences (D11), so most pixels agree closely and the
// rest only statistically; two GPU renders are bit-identical. Skipped
// without a usable NVIDIA device.

#include "test_helpers.h"

#include "core/film/crypto.h"
#include "core/film/lpe_compile.h"
#include "core/geometry/curves.h"
#include "core/gpu/device.h"
#include "core/lights/sky.h"
#include "core/math/frame.h"
#include "core/math/rng.h"
#include "core/integrator/renderer.h"
#include "core/scene/scene.h"
#include "core/shading/compiler.h"

#include <nanovdb/util/CreateNanoGrid.h>
#include <nanovdb/util/GridBuilder.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;

namespace {

bool gpuAvailable(std::string &why) {
    gpu::Device device;
    return device.init(why);
}

MeshDesc box(Vec3f lo, Vec3f hi) {
    MeshDesc m;
    m.points = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    const uint32_t quads[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};
    for (const auto &q : quads)
        m.indices.insert(m.indices.end(), {q[0], q[1], q[2], q[0], q[2], q[3]});
    return m;
}

// A quad from a corner and two edges (Lambert shades both sides).
MeshDesc wall(Vec3f o, Vec3f e1, Vec3f e2, Spectrum color) {
    MeshDesc m;
    m.points = {o, o + e1, o + e1 + e2, o + e2};
    m.indices = {0, 1, 2, 0, 2, 3};
    m.setDisplayColor(color);
    return m;
}

// A Cornell box (2 x 2 x 2, open towards +Z) with a white box (the default
// material), a rough Standard Surface metal sphere and a glass sphere, lit
// by a camera-visible rect light under the ceiling.
void cornellBox(Scene &scene) {
    const Spectrum white = spectrum(0.75f);
    scene.updateMesh(scene.createMesh(), wall({-1, 0, -1}, {2, 0, 0}, {0, 0, 2}, white));
    scene.updateMesh(scene.createMesh(), wall({-1, 2, -1}, {0, 0, 2}, {2, 0, 0}, white));
    scene.updateMesh(scene.createMesh(), wall({-1, 0, -1}, {0, 2, 0}, {2, 0, 0}, white));
    scene.updateMesh(scene.createMesh(), wall({-1, 0, -1}, {0, 0, 2}, {0, 2, 0}, {0.63f, 0.065f, 0.05f}));
    scene.updateMesh(scene.createMesh(), wall({1, 0, -1}, {0, 2, 0}, {0, 0, 2}, {0.14f, 0.45f, 0.09f}));
    MeshDesc block = box({-0.7f, 0.0f, -0.6f}, {-0.1f, 1.2f, 0.0f});
    block.setDisplayColor(white);
    scene.updateMesh(scene.createMesh(), block);

    MaterialDesc metal;
    metal.inputs = standardSurfaceDefaults();
    metal.inputs.set(surface_field::baseColor, Spectrum{0.9f, 0.7f, 0.4f});
    metal.inputs.set(surface_field::metalness, 1.0f);
    metal.inputs.set(surface_field::specularRoughness, 0.3f);
    const MaterialId metalId = scene.createMaterial();
    scene.updateMaterial(metalId, metal);
    MeshDesc ball = makeSphere(0.35f, 64, 32);
    ball.transform = translation({0.45f, 0.35f, -0.3f});
    const MeshId ballId = scene.createMesh();
    scene.updateMesh(ballId, ball);
    scene.setMeshMaterials(ballId, {metalId});

    MaterialDesc glass;
    glass.inputs = standardSurfaceDefaults();
    glass.inputs.set(surface_field::transmissionWeight, 1.0f);
    glass.inputs.set(surface_field::specularRoughness, 0.0f);
    const MaterialId glassId = scene.createMaterial();
    scene.updateMaterial(glassId, glass);
    MeshDesc lens = makeSphere(0.25f, 64, 32);
    lens.transform = translation({0.2f, 1.1f, 0.4f});
    const MeshId lensId = scene.createMesh();
    scene.updateMesh(lensId, lens);
    scene.setMeshMaterials(lensId, {glassId});

    LightDesc light;
    light.type = LightType::Rect;
    light.width = light.height = 0.6f;
    light.intensity = 12.0f;
    light.cameraVisible = true;
    Mat4 down = Mat4::identity(); // local -Z (the emission) to world -Y
    down.m[1][1] = 0.0f;
    down.m[1][2] = -1.0f;
    down.m[2][1] = 1.0f;
    down.m[2][2] = 0.0f;
    light.transform = down * translation({0.0f, 1.99f, 0.0f});
    scene.updateLight(scene.createLight(), light);
    scene.commit();
}

struct Render {
    Film film;
    bool gpu = false;
};

Render render(const Scene &scene, const Camera &camera, int width, int height, uint32_t spp, RenderDevice device,
              uint32_t maxDepth = 6, bool lightTree = false, const RenderSettings &base = RenderSettings{}) {
    Renderer renderer;
    RenderSettings settings = base;
    settings.device = device;
    settings.maxSamples = spp;
    settings.maxDepth = maxDepth;
    settings.lightTree = lightTree;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(camera);
    renderer.resize(width, height);
    bool gpu = true;
    while (!renderer.converged()) {
        renderer.renderPass({});
        gpu = gpu && renderer.lastPassOnGpu();
    }
    return {renderer.film(), gpu};
}

// Pixels whose sums agree within 1e-3 (relative above 1), of all.
double closePixels(const Film &a, const Film &b) {
    int close = 0;
    for (size_t i = 0; i < a.colorSum.size(); ++i) {
        const float x = luminance(a.colorSum[i]), y = luminance(b.colorSum[i]);
        close += std::fabs(x - y) <= 1e-3f * std::max(1.0f, std::fabs(x));
    }
    return double(close) / double(a.colorSum.size());
}

// A cube of constant value: voxels [-n, n)^3 of size h.
VolumeGridPtr cubeGrid(float value, int n, double h) {
    nanovdb::build::Grid<float> g(0.0f, "field", nanovdb::GridClass::FogVolume);
    g.setTransform(h, nanovdb::Vec3d(0.0));
    g([&](const nanovdb::Coord &) { return value; }, nanovdb::CoordBBox(nanovdb::Coord(-n), nanovdb::Coord(n - 1)));
    const nanovdb::GridHandle<nanovdb::HostBuffer> handle = nanovdb::createNanoGrid(g);
    return VolumeGrid::copyOf(handle.data(), handle.size());
}

// A volume material: scattering, absorption and emission in proportion to
// the density field, anisotropy g.
MaterialDesc medium(Vec3f s, Vec3f a, Vec3f e, float g) {
    using namespace shading;
    const auto field = [](uint16_t f) { return uint16_t(volume_field::kBase + f); };
    IrGraph graph;
    IrBuilder b(graph);
    const Ref d = b.primvar("density", 1, b.constant(0.0f));
    b.output(field(volume_field::scattering), 3, b.mul(d, b.constant(s.x, s.y, s.z)));
    b.output(field(volume_field::absorption), 3, b.mul(d, b.constant(a.x, a.y, a.z)));
    b.output(field(volume_field::emission), 3, b.mul(d, b.constant(e.x, e.y, e.z)));
    b.output(field(volume_field::anisotropy), 1, b.constant(g));
    MaterialDesc m;
    m.program = compile(graph).program;
    return m;
}

double luminanceMean(const Film &film) {
    double sum = 0.0;
    for (size_t i = 0; i < film.colorSum.size(); ++i)
        sum += double(luminance(film.colorSum[i])) / double(film.count[i]);
    return sum / double(film.colorSum.size());
}

} // namespace

TEST_CASE("XPU renders the CPU's Cornell box", "[gpu][render]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    cornellBox(scene);
    const Camera camera = makePerspectiveCamera({0.0f, 1.0f, 3.6f}, 0.05, 0.036, 48, 48);
    const Render cpu = render(scene, camera, 48, 48, 256, RenderDevice::Cpu);
    const Render gpu = render(scene, camera, 48, 48, 256, RenderDevice::Xpu);
    REQUIRE(!cpu.gpu);
    REQUIRE(gpu.gpu);

    // Per pixel: the same paths up to rounding, so most pixels agree
    // closely; paths whose decisions flip (Russian roulette, lobe choices
    // at a rounding boundary) differ by sampling noise.
    int close = 0;
    const int n = int(cpu.film.colorSum.size());
    for (int i = 0; i < n; ++i) {
        const float a = luminance(cpu.film.colorSum[i]), b = luminance(gpu.film.colorSum[i]);
        close += std::fabs(a - b) <= 1e-3f * std::max(1.0f, std::fabs(a));
        CHECK(gpu.film.count[i] == cpu.film.count[i]);
        CHECK(gpu.film.alphaSum[i] == cpu.film.alphaSum[i]);
    }
    const double cpuMean = luminanceMean(cpu.film), gpuMean = luminanceMean(gpu.film);
    INFO("pixels within 1e-3: " << close << " of " << n << "; mean CPU " << cpuMean << ", GPU " << gpuMean);
    CHECK(close >= n * 97 / 100); // measured: 2302 of 2304
    CHECK_THAT(gpuMean, WithinAbs(cpuMean, 0.001 * cpuMean));
}

// White Lambert (displayColor 1) in a uniform camera-visible dome: every
// pixel converges to 1 (every estimator unbiased on the GPU too).
TEST_CASE("XPU white furnace", "[gpu][render][furnace]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    MeshDesc ball = makeSphere(1.0f, 128, 64);
    ball.setDisplayColor(spectrum(1.0f));
    scene.updateMesh(scene.createMesh(), ball);
    LightDesc dome;
    dome.type = LightType::Dome;
    dome.cameraVisible = true;
    scene.updateLight(scene.createLight(), dome);
    scene.commit();
    const Render gpu = render(scene, makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.2f, 32, 32), 32, 32, 256,
                              RenderDevice::Xpu, 64);
    REQUIRE(gpu.gpu);
    CHECK_THAT(luminanceMean(gpu.film), WithinAbs(1.0, 0.005));
}

// A ground plane of 2000 x 2000 seen from close by under a uniform dome:
// rays leaving it near the origin start a tiny offset above it, while
// OptiX's triangle test errs by the scale of the corners; without a tMin
// (the scene's intersection tolerance) 1/8 of the light was lost.
TEST_CASE("XPU on a large ground plane", "[gpu][render]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    MeshDesc ground = makeQuad(1000.0f);
    ground.setDisplayColor(spectrum(0.5f));
    scene.updateMesh(scene.createMesh(), ground);
    LightDesc dome;
    dome.type = LightType::Dome;
    scene.updateLight(scene.createLight(), dome);
    scene.commit();
    const Render gpu = render(scene, makeOrthoCamera({0.0f, 0.0f, 10.0f}, 2.0f, 16, 16), 16, 16, 256,
                              RenderDevice::Xpu, 1);
    REQUIRE(gpu.gpu);
    CHECK_THAT(luminanceMean(gpu.film), WithinAbs(0.5, 0.005));
}

// docs/design/M10.md G3: the light tree, emissive meshes treated as lights
// (light-sampled, MIS against BSDF hits, emission where unoccluded shadow
// rays end), light linking (receiver sets) and shadow linking (visibility
// masks).
TEST_CASE("XPU lights: tree, emissive meshes, linking", "[gpu][render][lights]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    MeshDesc ground = makeQuad(6.0f);
    ground.setDisplayColor(spectrum(0.5f));
    scene.updateMesh(scene.createMesh(), ground);
    MaterialDesc emissive;
    emissive.inputs = standardSurfaceDefaults();
    emissive.inputs.set(surface_field::baseWeight, 0.0f);
    emissive.inputs.set(surface_field::specularWeight, 0.0f);
    emissive.inputs.set(surface_field::emissionWeight, 1.0f);
    emissive.inputs.set(surface_field::emissionColor, Spectrum{2.0f, 1.0f, 0.5f});
    const MaterialId glow = scene.createMaterial();
    scene.updateMaterial(glow, emissive);
    MeshDesc lamp = makeSphere(0.4f, 32, 16);
    lamp.transform = translation({-1.5f, 0.5f, 1.0f});
    const MeshId lampId = scene.createMesh();
    scene.updateMesh(lampId, lamp);
    scene.setMeshMaterials(lampId, {glow});
    scene.setMeshLightSource(lampId, MeshLightSource{1, 1.0f, 1.0f, 1.0f});
    // A sphere lit by the linked light only, and a blocker in its shadow set.
    MeshDesc ball = makeSphere(0.5f, 32, 16);
    ball.setDisplayColor(spectrum(0.8f));
    ball.transform = translation({1.2f, -0.8f, 0.5f});
    const MeshId ballId = scene.createMesh();
    scene.updateMesh(ballId, ball);
    scene.setMeshCategories(ballId, {"lit"});
    MeshDesc blocker = makeSphere(0.3f, 32, 16);
    blocker.setDisplayColor(spectrum(0.8f));
    blocker.transform = translation({0.8f, 1.2f, 1.5f});
    const MeshId blockerId = scene.createMesh();
    scene.updateMesh(blockerId, blocker);
    scene.setMeshCategories(blockerId, {"blk"});
    LightDesc linked;
    linked.type = LightType::Sphere;
    linked.radius = 0.2f;
    linked.intensity = 30.0f;
    linked.lightLink = "lit";
    linked.shadowLink = "blk";
    linked.transform = translation({1.0f, 2.0f, 2.5f});
    scene.updateLight(scene.createLight(), linked);
    LightDesc rect;
    rect.type = LightType::Rect;
    rect.width = rect.height = 1.0f;
    rect.intensity = 4.0f;
    rect.transform = translation({0.0f, -1.5f, 3.0f});
    scene.updateLight(scene.createLight(), rect);
    scene.commit();
    REQUIRE(scene.anyEmitters());
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 3.0f, 48, 48);
    const Render cpu = render(scene, camera, 48, 48, 128, RenderDevice::Cpu, 6, true);
    const Render gpu = render(scene, camera, 48, 48, 128, RenderDevice::Xpu, 6, true);
    REQUIRE(gpu.gpu);
    const double close = closePixels(cpu.film, gpu.film), cpuMean = luminanceMean(cpu.film),
                 gpuMean = luminanceMean(gpu.film);
    INFO("pixels within 1e-3: " << close << "; mean CPU " << cpuMean << ", GPU " << gpuMean);
    CHECK(close >= 0.95);
    CHECK_THAT(gpuMean, WithinAbs(cpuMean, 0.002 * cpuMean));
}

// docs/design/M11-P4.md: light filters (Karma's blocker) on the GPU as on the
// CPU: a box with rounded corners and a soft tinted edge and an inverted
// sphere on an area light (light samples and BSDF rays), a blocker on a dome
// (BSDF rays only), one on a camera-visible light around part of the camera
// plane, and one limited to a ball by a link category (the core's mechanism,
// which Hydra leaves unused as Karma ignores filterLink); with the light tree and by
// power, and light path expressions with weight-1 parts.
TEST_CASE("XPU light filters", "[gpu][render][lights][filters]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    MeshDesc ground = makeQuad(6.0f);
    ground.setDisplayColor(spectrum(0.5f));
    scene.updateMesh(scene.createMesh(), ground);
    MeshDesc ball = makeSphere(0.6f, 48, 24);
    ball.setDisplayColor(Spectrum{0.8f, 0.6f, 0.4f});
    ball.transform = translation({1.0f, -1.0f, 0.6f});
    const MeshId ballId = scene.createMesh();
    scene.updateMesh(ballId, ball);
    scene.setMeshCategories(ballId, {"ball"});
    LightDesc rect;
    rect.type = LightType::Rect;
    rect.width = rect.height = 2.0f;
    rect.intensity = 3.0f;
    rect.transform = translation({-0.5f, 0.5f, 3.0f});
    rect.filters = {"box", "ball"};
    scene.updateLight(scene.createLight(), rect);
    LightDesc dome;
    dome.type = LightType::Dome;
    dome.intensity = 0.3f;
    dome.filters = {"inv"};
    scene.updateLight(scene.createLight(), dome);
    LightDesc panel; // camera-visible, facing the camera
    panel.type = LightType::Rect;
    panel.width = panel.height = 1.0f;
    panel.intensity = 2.0f;
    panel.cameraVisible = true;
    Mat4 up = Mat4::identity();
    up.m[1][1] = -1.0;
    up.m[2][2] = -1.0;
    panel.transform = up * translation({-2.0f, 2.0f, 0.5f});
    panel.filters = {"cam"};
    scene.updateLight(scene.createLight(), panel);

    LightFilterDesc box;
    box.shape = BlockerShape::Box;
    box.width = 2.0f;
    box.height = 1.5f;
    box.depth = 1.0f;
    box.roundedCorner = 0.4f;
    box.falloff = 0.3f;
    box.intensity = 0.2f;
    box.tint = {1.0f, 0.6f, 0.3f};
    box.spaceFromWorld = inverse(translation({-1.0f, 0.0f, 0.0f}));
    scene.setLightFilter("box", box);
    LightFilterDesc inv;
    inv.radius = 2.0f;
    inv.falloff = 0.5f;
    inv.intensity = 0.3f;
    inv.exposure = 1.0f; // 0.6 outside the sphere
    inv.invert = true;
    scene.setLightFilter("inv", inv);
    LightFilterDesc cam;
    cam.radius = 1.0f;
    cam.intensity = 0.0f;
    cam.spaceFromWorld = inverse(translation({-2.0f, 2.3f, 5.0f}));
    scene.setLightFilter("cam", cam);
    LightFilterDesc linked;
    linked.radius = 3.0f;
    linked.intensity = 0.0f;
    linked.link = "ball";
    scene.setLightFilter("ball", linked);
    scene.commit();
    REQUIRE(scene.lights()[0].filterCount == 2);

    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 3.0f, 48, 48);
    for (const bool tree : {true, false}) {
        INFO("light tree " << tree);
        const Render cpu = render(scene, camera, 48, 48, 128, RenderDevice::Cpu, 4, tree);
        const Render gpu = render(scene, camera, 48, 48, 128, RenderDevice::Xpu, 4, tree);
        REQUIRE(gpu.gpu);
        const double close = closePixels(cpu.film, gpu.film), cpuMean = luminanceMean(cpu.film),
                     gpuMean = luminanceMean(gpu.film);
        INFO("pixels within 1e-3: " << close << "; mean CPU " << cpuMean << ", GPU " << gpuMean);
        CHECK(close >= 0.95);
        CHECK_THAT(gpuMean, WithinAbs(cpuMean, 0.002 * cpuMean));
    }

    // Light path expressions: per-light parts and the weight-1 unoccluded;
    // estimate (the dome is never light-sampled: its continuation carries it).
    const std::vector<std::string> expressions = {"C.*L", "unoccluded;C<RD>L"};
    const auto compiled = std::make_shared<lpe::Compiled>(lpe::compile(expressions));
    REQUIRE(compiled->ok());
    const auto renderLpe = [&](RenderDevice device, bool &onGpu) {
        Renderer renderer;
        RenderSettings settings;
        settings.device = device;
        settings.maxSamples = 128;
        settings.lpe = compiled;
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(camera);
        renderer.resize(32, 32);
        onGpu = true;
        while (!renderer.converged()) {
            renderer.renderPass({});
            onGpu = onGpu && renderer.lastPassOnGpu();
        }
        return renderer.film();
    };
    bool cpuGpu = false, gpuGpu = false;
    const Film cpu = renderLpe(RenderDevice::Cpu, cpuGpu);
    const Film gpu = renderLpe(RenderDevice::Xpu, gpuGpu);
    REQUIRE(gpuGpu);
    REQUIRE(gpu.lpeOutputs == cpu.lpeOutputs);
    for (uint32_t o = 0; o < cpu.lpeOutputs; ++o) {
        double a = 0.0, b = 0.0;
        for (size_t i = 0; i < size_t(32 * 32); ++i) {
            a += double(luminance(cpu.lpe(o)[i]));
            b += double(luminance(gpu.lpe(o)[i]));
        }
        INFO("output " << o << " (" << expressions[o] << "): CPU " << a << ", GPU " << b);
        CHECK(a > 0.0);
        CHECK_THAT(b, WithinAbs(a, 0.003 * a + 1e-3));
    }
}

// M9 D15 (R1-28): light linking with 255 distinct category combinations
// (receiver sets) of a grid of quads, 8 lights linked to one category each
// (test_light_tree.cpp has the exact references): the GPU's links and light
// selection, by the tree and by power, are the CPU's.
TEST_CASE("XPU light linking beyond 63 receiver sets", "[gpu][render][lights][linking]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    for (int k = 0; k < 256; ++k) {
        MeshDesc m = makeQuad(0.45f);
        m.transform = translation({float(k % 16) - 7.5f, float(k / 16) - 7.5f, 0.0f});
        const MeshId id = scene.createMesh();
        scene.updateMesh(id, m);
        std::vector<std::string> categories;
        for (int b = 0; b < 8; ++b)
            if ((k >> b) & 1)
                categories.push_back("c" + std::to_string(b));
        scene.setMeshCategories(id, categories);
    }
    for (int b = 0; b < 8; ++b) {
        LightDesc l;
        l.type = LightType::Sphere;
        l.radius = 0.5f;
        l.intensity = 10.0f + 2.0f * float(b);
        const float a = kTwoPi * float(b) / 8.0f;
        l.transform = translation({6.0f * std::cos(a), 6.0f * std::sin(a), 4.0f});
        l.lightLink = "c" + std::to_string(b);
        scene.updateLight(scene.createLight(), l);
    }
    scene.commit();
    REQUIRE(scene.receiverSetCount() == 256u);
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 10.0f}, 8.0f, 64, 64);
    for (const bool tree : {true, false}) {
        INFO("light tree " << tree);
        const Render cpu = render(scene, camera, 64, 64, 64, RenderDevice::Cpu, 6, tree);
        const Render gpu = render(scene, camera, 64, 64, 64, RenderDevice::Xpu, 6, tree);
        REQUIRE(gpu.gpu);
        const double close = closePixels(cpu.film, gpu.film), cpuMean = luminanceMean(cpu.film),
                     gpuMean = luminanceMean(gpu.film);
        INFO("pixels within 1e-3: " << close << "; mean CPU " << cpuMean << ", GPU " << gpuMean);
        CHECK(close >= 0.95);
        CHECK_THAT(gpuMean, WithinAbs(cpuMean, 0.002 * cpuMean));
    }
}

// M9 D15 (R1-31): a shadow-linked dome (textured: a dim sky light sampling
// never picks and a bright patch, or constant) over a ground, with a wall
// outside its shadow set next to the view and a caster in it: the dome's
// BSDF samples are shadow rays of their own (kind 3) on the GPU as on the
// CPU, beauty and light path expressions (unoccluded;, shadow;).
TEST_CASE("XPU shadow-linked domes", "[gpu][render][lights][linking][lpe]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    for (const bool textured : {false, true}) {
        INFO("textured " << textured);
        Scene scene;
        MeshDesc ground = makeQuad(8.0f);
        ground.setDisplayColor(spectrum(0.5f));
        scene.updateMesh(scene.createMesh(), ground);
        MeshDesc caster = wall({-3.0f, -6.0f, 0.6f}, {0.8f, 0.0f, 0.0f}, {0.0f, 12.0f, 0.0f}, spectrum(0.3f));
        const MeshId casterId = scene.createMesh();
        scene.updateMesh(casterId, caster);
        scene.setMeshCategories(casterId, {"casters"});
        scene.updateMesh(scene.createMesh(),
                         wall({0.2f, -6.0f, 0.0f}, {0.0f, 12.0f, 0.0f}, {0.0f, 0.0f, 4.0f}, spectrum(0.3f)));
        LightDesc dome;
        dome.type = LightType::Dome;
        dome.shadowLink = "casters";
        if (textured) {
            auto sky = std::make_shared<Image>();
            sky->width = 64;
            sky->height = 32;
            sky->pixels.assign(64 * 32, Spectrum{0.6f, 0.7f, 0.9f});
            for (int y = 6; y < 10; ++y)
                for (int x = 36; x < 42; ++x)
                    sky->pixels[size_t(y) * 64 + size_t(x)] = spectrum(12.0f);
            dome.texture = sky;
        }
        scene.updateLight(scene.createLight(), dome);
        scene.commit();
        REQUIRE(scene.shadowLinkedDomes().size() == 1u);
        const Camera camera = makeOrthoCamera({-0.9f, 0.0f, 5.0f}, 1.1f, 32, 32);
        const std::vector<std::string> expressions = {"C.*L", "unoccluded;C<RD>L", "shadow;C<RD>L"};
        const auto compiled = std::make_shared<lpe::Compiled>(lpe::compile(expressions));
        REQUIRE(compiled->ok());
        const auto renderLpe = [&](RenderDevice device, bool &gpu) {
            Renderer renderer;
            RenderSettings settings;
            settings.device = device;
            settings.maxSamples = 128;
            settings.lpe = compiled;
            renderer.setScene(&scene);
            renderer.setSettings(settings);
            renderer.setCamera(camera);
            renderer.resize(32, 32);
            gpu = true;
            while (!renderer.converged()) {
                renderer.renderPass({});
                gpu = gpu && renderer.lastPassOnGpu();
            }
            return renderer.film();
        };
        bool cpuGpu = false, gpuGpu = false;
        const Film cpu = renderLpe(RenderDevice::Cpu, cpuGpu);
        const Film gpu = renderLpe(RenderDevice::Xpu, gpuGpu);
        REQUIRE(gpuGpu);
        const double close = closePixels(cpu, gpu), cpuMean = luminanceMean(cpu), gpuMean = luminanceMean(gpu);
        INFO("pixels within 1e-3: " << close << "; mean CPU " << cpuMean << ", GPU " << gpuMean);
        CHECK(close >= 0.95);
        CHECK_THAT(gpuMean, WithinAbs(cpuMean, 0.002 * cpuMean));
        REQUIRE(gpu.lpeOutputs == cpu.lpeOutputs);
        for (uint32_t o = 0; o < cpu.lpeOutputs; ++o) {
            double a = 0.0, b = 0.0;
            for (size_t i = 0; i < size_t(32 * 32); ++i) {
                a += double(luminance(cpu.lpe(o)[i]));
                b += double(luminance(gpu.lpe(o)[i]));
            }
            INFO("output " << o << " (" << expressions[o] << "): CPU " << a << ", GPU " << b);
            if (o < 2) // shadow;C<RD>L is about 0: nothing in the dome's shadow set is near the view
                CHECK(a > 0.0);
            CHECK_THAT(b, WithinAbs(a, 0.003 * a + 1e-3));
        }
    }
}

// M11 P3: Karma's sky dome is a dome light with a baked sky texture
// (docs/design/M11-P3.md); the GPU renders the CPU's image of a ground and a
// sphere under a low sun's sky seen by the camera, horizon and ground color
// included.
TEST_CASE("XPU Karma sky dome", "[gpu][render][lights][sky]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    MeshDesc ground = makeQuad(3.0f);
    Mat4 yUp = Mat4::identity(); // the quad's +Z (its normal) to world +Y
    yUp.m[1][1] = 0.0f;
    yUp.m[1][2] = -1.0f;
    yUp.m[2][1] = 1.0f;
    yUp.m[2][2] = 0.0f;
    ground.transform = yUp;
    ground.setDisplayColor(spectrum(0.5f));
    scene.updateMesh(scene.createMesh(), ground);
    MeshDesc ball = makeSphere(0.5f, 64, 32);
    ball.transform = translation({0.0f, 0.5f, 0.0f});
    ball.setDisplayColor(spectrum(0.8f));
    scene.updateMesh(scene.createMesh(), ball);
    KarmaSkyDesc sky;
    sky.solarAltitude = 25.0f;
    sky.solarAzimuth = 20.0f;
    sky.groundColor = {0.05f, 0.04f, 0.03f};
    LightDesc dome;
    dome.type = LightType::Dome;
    dome.cameraVisible = true;
    dome.texture = bakeKarmaSky(sky, 512);
    scene.updateLight(scene.createLight(), dome);
    scene.commit();
    // Looks down -Z, towards the sun's side; the ground ends in view, so the
    // sky's ground color and horizon show too.
    const Camera camera = makePerspectiveCamera({0.0f, 1.0f, 4.0f}, 0.03, 0.036, 48, 32);
    const Render cpu = render(scene, camera, 48, 32, 256, RenderDevice::Cpu);
    const Render gpu = render(scene, camera, 48, 32, 256, RenderDevice::Xpu);
    REQUIRE(gpu.gpu);
    const double close = closePixels(cpu.film, gpu.film), cpuMean = luminanceMean(cpu.film),
                 gpuMean = luminanceMean(gpu.film);
    INFO("pixels within 1e-3: " << close << "; mean CPU " << cpuMean << ", GPU " << gpuMean);
    CHECK(cpuMean > 0.01);
    CHECK(close >= 0.95);
    CHECK_THAT(gpuMean, WithinAbs(cpuMean, 0.002 * cpuMean));
}

// M9 D18 (the follow-up of R1-31): lights with sampling quality 0 illuminate
// nothing on the GPU either (a ground and a mirror sphere under a rect, a
// distant light with an angle and a dome, shadow-linked or not), the camera
// still sees a camera-visible one; at quality 1 the GPU renders the CPU's.
TEST_CASE("XPU lights with sampling quality 0", "[gpu][render][lights][linking]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    for (const bool shadowLinked : {false, true})
        for (const float quality : {0.0f, 1.0f}) {
            INFO("shadow-linked " << shadowLinked << ", quality " << quality);
            Scene scene;
            MeshDesc ground = makeQuad(6.0f);
            ground.setDisplayColor(spectrum(0.5f));
            scene.updateMesh(scene.createMesh(), ground);
            MaterialDesc mirror;
            mirror.inputs = standardSurfaceDefaults();
            mirror.inputs.set(surface_field::metalness, 1.0f);
            mirror.inputs.set(surface_field::specularRoughness, 0.0f);
            const MaterialId mirrorId = scene.createMaterial();
            scene.updateMaterial(mirrorId, mirror);
            MeshDesc ball = makeSphere(0.6f, 32, 16);
            ball.transform = translation({0.0f, 0.0f, 0.6f});
            const MeshId ballId = scene.createMesh();
            scene.updateMesh(ballId, ball);
            scene.setMeshMaterials(ballId, {mirrorId});
            scene.setMeshCategories(ballId, {"casters"});
            LightDesc rect;
            rect.type = LightType::Rect;
            rect.width = rect.height = 2.0f;
            rect.intensity = 5.0f;
            rect.transform = translation({1.5f, 0.0f, 3.0f}); // emits down -Z
            LightDesc sun;
            sun.type = LightType::Distant;
            sun.angleDeg = 10.0f;
            sun.intensity = 2.0f;
            LightDesc dome;
            dome.type = LightType::Dome;
            dome.intensity = 0.3f;
            for (LightDesc *l : {&rect, &sun, &dome}) {
                l->samplingQuality = quality;
                l->shadowLink = shadowLinked ? "casters" : "";
                scene.updateLight(scene.createLight(), *l);
            }
            LightDesc seen; // camera-visible, facing the camera, outside the view of the rest
            seen.type = LightType::Rect;
            seen.width = seen.height = 1.0f;
            seen.intensity = 3.0f;
            seen.cameraVisible = true;
            seen.samplingQuality = 0.0f;
            Mat4 up = Mat4::identity(); // local -Z (the emission) to world +Z
            up.m[1][1] = -1.0f;
            up.m[2][2] = -1.0f;
            seen.transform = up * translation({-2.0f, -2.0f, 4.0f});
            scene.updateLight(scene.createLight(), seen);
            scene.commit();
            const Camera camera = makeOrthoCamera({0.0f, 0.0f, 8.0f}, 3.0f, 32, 32);
            const Render cpu = render(scene, camera, 32, 32, 32, RenderDevice::Cpu, 4, true);
            const Render gpu = render(scene, camera, 32, 32, 32, RenderDevice::Xpu, 4, true);
            REQUIRE(gpu.gpu);
            const double cpuMean = luminanceMean(cpu.film), gpuMean = luminanceMean(gpu.film);
            INFO("mean CPU " << cpuMean << ", GPU " << gpuMean);
            CHECK_THAT(gpuMean, WithinAbs(cpuMean, 0.003 * cpuMean));
            // The camera-visible light (x, y in [-2.5, -1.5]: pixel columns
            // and rows 3..7) is seen; at quality 0 nothing else is lit.
            for (const Film *film : {&cpu.film, &gpu.film}) {
                double inside = 0.0, away = 0.0;
                for (int y = 0; y < 32; ++y)
                    for (int x = 0; x < 32; ++x) {
                        const double v = luminance(pixelMean(*film, x, y));
                        if (x >= 4 && x <= 6 && y >= 4 && y <= 6)
                            inside += v;
                        else if (x > 10 || y > 10)
                            away += v;
                    }
                CHECK_THAT(inside / 9.0, WithinAbs(3.0, 1e-3));
                if (quality == 0.0f)
                    CHECK(away == 0.0);
                else
                    CHECK(away > 0.0);
            }
        }
}

// docs/design/M10.md G4: first-hit AOVs (the CPU traces them for GPU passes,
// so they are exact), the ambient occlusion AOV and cryptomattes (the GPU
// records the camera hits, the host accumulates them).
TEST_CASE("XPU AOVs: first hit, ambient occlusion, cryptomatte", "[gpu][render][aov]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    cornellBox(scene);
    const Camera camera = makePerspectiveCamera({0.0f, 1.0f, 3.6f}, 0.05, 0.036, 32, 32);
    const auto renderAovs = [&](RenderDevice device, bool &gpu) {
        Renderer renderer;
        RenderSettings settings;
        settings.device = device;
        settings.maxSamples = 64;
        settings.utility = std::make_shared<std::vector<UtilityRequest>>(std::vector<UtilityRequest>{
            {UtilityKind::Ao, {}}, {UtilityKind::P, {}}, {UtilityKind::N, {}}, {UtilityKind::Face, {}}});
        settings.crypto = std::make_shared<std::vector<CryptoRequest>>(
            std::vector<CryptoRequest>{{CryptoSource::Material, {}}});
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(camera);
        renderer.resize(32, 32);
        gpu = true;
        while (!renderer.converged()) {
            renderer.renderPass({});
            gpu = gpu && renderer.lastPassOnGpu();
        }
        return renderer.film();
    };
    bool cpuGpu = false, gpuGpu = false;
    const Film cpu = renderAovs(RenderDevice::Cpu, cpuGpu);
    const Film gpu = renderAovs(RenderDevice::Xpu, gpuGpu);
    REQUIRE(gpuGpu);
    CHECK(gpu.depth == cpu.depth);
    CHECK(gpu.primId == cpu.primId);
    REQUIRE(gpu.utility.size() == 4);
    for (size_t u = 1; u < 4; ++u) // P, N, Face: first hit
        CHECK(gpu.utility[u].data == cpu.utility[u].data);
    // Ambient occlusion: the same rays up to rounding.
    const std::vector<float> &a = cpu.utility[0].data, &b = gpu.utility[0].data;
    int close = 0;
    double sumA = 0.0, sumB = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        close += std::fabs(a[i] - b[i]) <= 1e-3f * std::max(1.0f, a[i]);
        sumA += a[i];
        sumB += b[i];
    }
    INFO("AO pixels within 1e-3: " << close << " of " << a.size() << "; sums " << sumA << ", " << sumB);
    CHECK(close >= int(a.size()) * 95 / 100);
    CHECK_THAT(sumB, WithinAbs(sumA, 0.01 * sumA));
    // Cryptomatte: each pixel's weight per id (the slot order may differ).
    REQUIRE(gpu.crypto.size() == 1);
    int same = 0;
    for (size_t p = 0; p < size_t(32 * 32); ++p) {
        std::map<uint32_t, float> wa, wb;
        for (int s = 0; s < kCryptoSlots; ++s) {
            const size_t i = p * kCryptoSlots + size_t(s);
            if (cpu.crypto[0].weights[i] > 0.0f)
                wa[cpu.crypto[0].ids[i]] += cpu.crypto[0].weights[i];
            if (gpu.crypto[0].weights[i] > 0.0f)
                wb[gpu.crypto[0].ids[i]] += gpu.crypto[0].weights[i];
        }
        same += wa == wb;
    }
    INFO("cryptomatte pixels with the same weights: " << same);
    CHECK(same >= 32 * 32 * 98 / 100);
}

// docs/design/M10.md G4: light path expressions on the GPU (lpe_path.h): the
// same paths give the same outputs, every kind of output and event.
TEST_CASE("XPU light path expressions", "[gpu][render][aov][lpe]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    cornellBox(scene);
    const Camera camera = makePerspectiveCamera({0.0f, 1.0f, 3.6f}, 0.05, 0.036, 32, 32);
    const std::vector<std::string> expressions = {"C.*L",       "C<RD>L",     "C<RD>.+L", "C<RG>.*L", "C<TS>.*",
                                                  "CO",         "C<RD>A",     "CB",
                                                  "unoccluded;C<RD>L", "shadow;C<RD>L"};
    const auto compiled = std::make_shared<lpe::Compiled>(lpe::compile(expressions));
    REQUIRE(compiled->ok());
    const auto renderLpe = [&](RenderDevice device, bool &gpu) {
        Renderer renderer;
        RenderSettings settings;
        settings.device = device;
        settings.maxSamples = 64;
        settings.lpe = compiled;
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(camera);
        renderer.resize(32, 32);
        gpu = true;
        while (!renderer.converged()) {
            renderer.renderPass({});
            gpu = gpu && renderer.lastPassOnGpu();
        }
        return renderer.film();
    };
    bool unused = false, onGpu = false;
    const Film cpu = renderLpe(RenderDevice::Cpu, unused);
    const Film gpu = renderLpe(RenderDevice::Xpu, onGpu);
    REQUIRE(onGpu);
    REQUIRE(gpu.lpeOutputs == cpu.lpeOutputs);
    for (uint32_t o = 0; o < cpu.lpeOutputs; ++o) {
        double sumA = 0.0, sumB = 0.0;
        int close = 0;
        for (size_t i = 0; i < size_t(32 * 32); ++i) {
            const float a = luminance(cpu.lpe(o)[i]), b = luminance(gpu.lpe(o)[i]);
            sumA += a;
            sumB += b;
            close += std::fabs(a - b) <= 1e-3f * std::max(1.0f, std::fabs(a));
        }
        INFO("output " << o << " (" << expressions[o] << "): sums " << sumA << ", " << sumB << "; pixels close " << close);
        CHECK(close >= 32 * 32 * 95 / 100);
        CHECK_THAT(sumB, WithinAbs(sumA, 0.002 * sumA + 1e-6));
    }
    // shadow;'s lit sums.
    REQUIRE(gpu.lpeLit.size() == cpu.lpeLit.size());
    int litClose = 0;
    for (size_t i = 0; i < cpu.lpeLit.size(); ++i) {
        const float a = luminance(cpu.lpeLit[i]), b = luminance(gpu.lpeLit[i]);
        litClose += std::fabs(a - b) <= 1e-3f * std::max(1.0f, std::fabs(a));
    }
    CHECK(litClose >= int(cpu.lpeLit.size()) * 95 / 100);
}

// docs/design/M10.md G5: volumes on the GPU (medium_track.h): free flights
// and ratio tracking with emission, scattering events with the phase
// function, a volume treated as a light, equiangular sampling of a light.
TEST_CASE("XPU volumes: media, volume lights, equiangular", "[gpu][render][volume]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    MeshDesc ground = makeQuad(3.0f);
    ground.setDisplayColor(spectrum(0.5f));
    scene.updateMesh(scene.createMesh(), ground);
    const MaterialId fire = scene.createMaterial();
    scene.updateMaterial(fire, medium({0.3f, 0.3f, 0.3f}, {0.7f, 0.7f, 0.7f}, {2.0f, 1.0f, 0.5f}, 0.2f));
    VolumeDesc flame;
    flame.fields.push_back({"density", "density", cubeGrid(1.0f, 8, 1.0 / 16.0), Affine3f{}});
    flame.material = fire;
    flame.light = true;
    const VolumeId flameId = scene.createVolume();
    scene.updateVolume(flameId, flame);
    scene.updateVolumeMotion(flameId, {translation({-0.8f, 0.0f, 0.8f})});
    const MaterialId fog = scene.createMaterial();
    scene.updateMaterial(fog, medium({1.5f, 1.5f, 1.5f}, {0.1f, 0.1f, 0.1f}, {0.0f, 0.0f, 0.0f}, 0.5f));
    VolumeDesc smoke;
    smoke.fields.push_back({"density", "density", cubeGrid(1.0f, 8, 1.0 / 16.0), Affine3f{}});
    smoke.material = fog;
    const VolumeId smokeId = scene.createVolume();
    scene.updateVolume(smokeId, smoke);
    scene.updateVolumeMotion(smokeId, {translation({0.9f, 0.0f, 0.8f})});
    LightDesc point;
    point.type = LightType::Sphere;
    point.radius = 0.05f;
    point.intensity = 20.0f;
    point.equiangular = true;
    point.transform = translation({0.9f, 0.6f, 1.2f});
    scene.updateLight(scene.createLight(), point);
    scene.commit();
    REQUIRE(scene.lights().size() == 2);
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 2.0f, 32, 32);
    const Render cpu = render(scene, camera, 32, 32, 256, RenderDevice::Cpu, 4, true);
    const Render gpu = render(scene, camera, 32, 32, 256, RenderDevice::Xpu, 4, true);
    REQUIRE(gpu.gpu);
    const double close = closePixels(cpu.film, gpu.film), cpuMean = luminanceMean(cpu.film),
                 gpuMean = luminanceMean(gpu.film);
    INFO("pixels within 1e-3: " << close << "; mean CPU " << cpuMean << ", GPU " << gpuMean);
    CHECK(close >= 0.9);
    CHECK_THAT(gpuMean, WithinAbs(cpuMean, 0.005 * cpuMean));
}

// docs/design/M11-P5.md: media bounded by surfaces on the GPU (the shells'
// crossings in medium_track.h): a fog box scattering with a heavier shadow
// density (ray:isshadowray), a noise sphere (fractal3d in P), an emitting
// box as a volume light, all inside a large thin fog box around the camera.
TEST_CASE("XPU media bounded by surfaces", "[gpu][render][volume][shell]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    using namespace shading;
    const auto field = [](uint16_t f) { return uint16_t(volume_field::kBase + f); };
    Scene scene;
    MeshDesc ground = makeQuad(3.0f);
    ground.setDisplayColor(spectrum(0.5f));
    scene.updateMesh(scene.createMesh(), ground);
    const auto shellOf = [](const MeshDesc &m) {
        return makeTriangleShell(m.points.data(), m.pointCount(), m.indices.data(), m.triangleCount());
    };
    const auto addShell = [&](MediumShellPtr shell, const MaterialDesc &material, bool light = false) {
        VolumeDesc desc;
        desc.shell = std::move(shell);
        desc.material = scene.createMaterial();
        scene.updateMaterial(desc.material, material);
        desc.light = light;
        const VolumeId id = scene.createVolume();
        scene.updateVolume(id, desc);
        scene.updateVolumeMotion(id, {Mat4::identity()});
    };
    { // fog: scattering 1.5, shadow rays 3 (the Fog Box's switch)
        IrGraph g;
        IrBuilder b(g);
        const Ref shadow = b.binary(Op::Equal, b.global(Global::RayType), b.constant(float(RayClass::Shadow)));
        const Ref d = b.ternary(Op::Select, shadow, b.constant(3.0f), b.constant(1.5f));
        b.output(field(volume_field::scattering), 3, b.convert(d, 3));
        b.output(field(volume_field::anisotropy), 1, b.constant(0.3f));
        MaterialDesc m;
        m.program = compile(g).program;
        addShell(shellOf(box({-1.6f, -0.5f, 0.2f}, {-0.4f, 0.5f, 1.2f})), m);
    }
    { // noise: 2 (fractal3d(3 P) + 0.5), half absorbing
        IrGraph g;
        IrBuilder b(g);
        const Ref n = b.fractal(b.mul(b.global(Global::P), b.constant(3.0f)), b.constant(3.0f), b.constant(2.0f),
                                b.constant(0.5f), 1);
        const Ref d = b.convert(b.mul(b.constant(2.0f), b.add(n, b.constant(0.5f))), 3);
        b.output(field(volume_field::scattering), 3, d);
        b.output(field(volume_field::absorption), 3, d);
        MaterialDesc m;
        m.program = compile(g).program;
        addShell(makeSphereShell({1.0f, 0.0f, 0.7f}, 0.5f), m);
    }
    { // an emitting box, light-sampled
        IrGraph g;
        IrBuilder b(g);
        b.output(field(volume_field::absorption), 3, b.constant(1.0f, 1.0f, 1.0f));
        b.output(field(volume_field::emission), 3, b.constant(3.0f, 1.5f, 0.5f));
        MaterialDesc m;
        m.program = compile(g).program;
        addShell(shellOf(box({-0.3f, 1.0f, 0.1f}, {0.3f, 1.6f, 0.5f})), m, true);
    }
    { // thin fog around everything, the camera inside it
        IrGraph g;
        IrBuilder b(g);
        b.output(field(volume_field::scattering), 3, b.constant(0.08f, 0.08f, 0.08f));
        MaterialDesc m;
        m.program = compile(g).program;
        addShell(shellOf(box({-4.0f, -4.0f, -1.0f}, {4.0f, 4.0f, 6.0f})), m);
    }
    LightDesc sun;
    sun.type = LightType::Distant;
    sun.intensity = 2.0f;
    scene.updateLight(scene.createLight(), sun);
    scene.commit();
    REQUIRE(scene.activeVolumes().size() == 4);
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 2.0f, 32, 32);
    const Render cpu = render(scene, camera, 32, 32, 256, RenderDevice::Cpu, 4, true);
    const Render gpu = render(scene, camera, 32, 32, 256, RenderDevice::Xpu, 4, true);
    REQUIRE(gpu.gpu);
    // Every path scatters in the fog around the camera, so paths part ways at
    // floating-point differences early: compare means (the image's and each
    // quadrant's), not pixels.
    const double cpuMean = luminanceMean(cpu.film), gpuMean = luminanceMean(gpu.film);
    INFO("mean CPU " << cpuMean << ", GPU " << gpuMean);
    CHECK_THAT(gpuMean, WithinAbs(cpuMean, 0.005 * cpuMean));
    for (int q = 0; q < 4; ++q) {
        double a = 0.0, b = 0.0;
        for (int y = (q / 2) * 16; y < (q / 2) * 16 + 16; ++y)
            for (int x = (q % 2) * 16; x < (q % 2) * 16 + 16; ++x) {
                const size_t i = size_t(y) * 32 + size_t(x);
                a += luminance(cpu.film.colorSum[i]) / cpu.film.count[i];
                b += luminance(gpu.film.colorSum[i]) / gpu.film.count[i];
            }
        INFO("quadrant " << q << ": CPU " << a / 256.0 << ", GPU " << b / 256.0);
        CHECK_THAT(b, WithinAbs(a, 0.02 * a));
    }
}

// docs/design/M10.md G6: curves of every basis (round: OptiX's tubes; flat:
// round tubes, the same coverage, D5), points (sphere: a custom sphere;
// ray-facing disc: the built-in sphere; oriented disc: a custom disc).
TEST_CASE("XPU curves and points", "[gpu][render][curves]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    MeshDesc ground = makeQuad(4.0f);
    ground.setDisplayColor(spectrum(0.4f));
    ground.transform = translation({0.0f, 0.0f, -0.5f});
    scene.updateMesh(scene.createMesh(), ground);
    const CurveBasis bases[4] = {CurveBasis::Linear, CurveBasis::Bezier, CurveBasis::BSpline, CurveBasis::CatmullRom};
    for (int b = 0; b < 5; ++b) {
        MeshDesc d;
        d.shape = MeshShape::Curves;
        d.curveBasis = bases[b % 4];
        d.curveStyle = b == 4 ? CurveStyle::Flat : CurveStyle::Round;
        const int n = d.curveBasis == CurveBasis::Bezier ? 10 : 9;
        for (int c = 0; c < 6; ++c) {
            for (int i = 0; i < n; ++i) {
                const float x = -1.6f + 3.2f * float(i) / float(n - 1);
                d.points.push_back({x, -1.5f + 0.6f * float(b) + 0.08f * float(c), 0.15f * std::sin(3.0f * x + float(c))});
            }
            d.curveVertexCounts.push_back(uint32_t(n));
        }
        d.curveRadius.assign(d.points.size(), 0.025f);
        d.setDisplayColor(Spectrum{0.8f, 0.5f - 0.1f * float(b), 0.3f});
        REQUIRE(prepareCurves(d));
        scene.updateMesh(scene.createMesh(), d);
    }
    const PointStyle styles[3] = {PointStyle::Sphere, PointStyle::Disc, PointStyle::OrientedDisc};
    Rng rng = Rng::make(7u, 1u);
    for (int s = 0; s < 3; ++s) {
        MeshDesc d;
        d.shape = MeshShape::Points;
        d.pointStyle = styles[s];
        for (int i = 0; i < 40; ++i) {
            d.points.push_back({-1.8f + 1.2f * float(s) + rng.next(), 1.6f + 0.4f * rng.next(), 0.3f * rng.next()});
            d.pointRadius.push_back(0.03f + 0.04f * rng.next());
            d.normals.push_back(normalize(Vec3f{rng.next() - 0.5f, rng.next() - 0.5f, 1.0f}));
        }
        d.setDisplayColor(spectrum(0.7f));
        scene.updateMesh(scene.createMesh(), d);
    }
    LightDesc dome;
    dome.type = LightType::Dome;
    dome.intensity = 0.5f;
    dome.cameraVisible = true;
    scene.updateLight(scene.createLight(), dome);
    LightDesc rect;
    rect.type = LightType::Rect;
    rect.width = rect.height = 1.5f;
    rect.intensity = 3.0f;
    rect.transform = translation({0.5f, 0.5f, 3.0f});
    scene.updateLight(scene.createLight(), rect);
    scene.commit();
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 2.2f, 64, 64);
    const Render cpu = render(scene, camera, 64, 64, 64, RenderDevice::Cpu, 6, true);
    const Render gpu = render(scene, camera, 64, 64, 64, RenderDevice::Xpu, 6, true);
    REQUIRE(gpu.gpu);
    const double close = closePixels(cpu.film, gpu.film), cpuMean = luminanceMean(cpu.film),
                 gpuMean = luminanceMean(gpu.film);
    INFO("pixels within 1e-3: " << close << "; mean CPU " << cpuMean << ", GPU " << gpuMean);
    // Measured: 61 % of the pixels within 1e-3 (87 % without the flat curves
    // and ray-facing discs, which the GPU renders as tubes and spheres: the
    // rest differs by the intersectors' rounding), the means 0.1 % apart.
    CHECK(close >= 0.55);
    CHECK_THAT(gpuMean, WithinAbs(cpuMean, 0.005 * cpuMean));
}

// Normal-oriented ribbons (D5): twisting blades of the three cubic bases,
// which the GPU tessellates into triangles (hits mapped back to the segment
// and its parameter), one of them moving.
TEST_CASE("XPU oriented ribbons", "[gpu][render][curves]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    MeshDesc ground = makeQuad(4.0f);
    ground.setDisplayColor(spectrum(0.4f));
    ground.transform = translation({0.0f, 0.0f, -0.5f});
    scene.updateMesh(scene.createMesh(), ground);
    const CurveBasis bases[3] = {CurveBasis::Bezier, CurveBasis::BSpline, CurveBasis::CatmullRom};
    for (int b = 0; b < 3; ++b) {
        MeshDesc d;
        d.shape = MeshShape::Curves;
        d.curveBasis = bases[b];
        d.curveStyle = CurveStyle::Oriented;
        const int n = d.curveBasis == CurveBasis::Bezier ? 10 : 9;
        for (int c = 0; c < 4; ++c) {
            for (int i = 0; i < n; ++i) {
                const float x = -1.6f + 3.2f * float(i) / float(n - 1), twist = 1.2f * x + float(c);
                d.points.push_back({x, -1.2f + 1.0f * float(b) + 0.22f * float(c), 0.15f * std::sin(2.0f * x + float(c))});
                d.normals.push_back(normalize(Vec3f{0.0f, std::sin(twist), std::cos(twist)}));
            }
            d.curveVertexCounts.push_back(uint32_t(n));
        }
        d.curveRadius.assign(d.points.size(), 0.08f);
        d.setDisplayColor(Spectrum{0.3f + 0.2f * float(b), 0.7f, 0.3f});
        if (b == 1) { // deforming: a second step moved along y
            const size_t count = d.points.size();
            for (size_t i = 0; i < count; ++i)
                d.points.push_back(d.points[i] + Vec3f{0.0f, 0.12f, 0.0f});
            d.pointSteps = 2;
        }
        REQUIRE(prepareCurves(d));
        scene.updateMesh(scene.createMesh(), d);
    }
    LightDesc dome;
    dome.type = LightType::Dome;
    dome.intensity = 0.5f;
    dome.cameraVisible = true;
    scene.updateLight(scene.createLight(), dome);
    LightDesc rect;
    rect.type = LightType::Rect;
    rect.width = rect.height = 1.5f;
    rect.intensity = 3.0f;
    rect.transform = translation({0.5f, 0.5f, 3.0f});
    scene.updateLight(scene.createLight(), rect);
    scene.commit();
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 2.2f, 64, 64);
    const Render cpu = render(scene, camera, 64, 64, 64, RenderDevice::Cpu, 6, true);
    const Render gpu = render(scene, camera, 64, 64, 64, RenderDevice::Xpu, 6, true);
    REQUIRE(gpu.gpu);
    const double close = closePixels(cpu.film, gpu.film), cpuMean = luminanceMean(cpu.film),
                 gpuMean = luminanceMean(gpu.film);
    INFO("pixels within 1e-3: " << close << "; mean CPU " << cpuMean << ", GPU " << gpuMean);
    // Measured: 74 % of the pixels within 1e-3 (the rest along the blades'
    // edges and twists, where the quads leave Embree's exact ribbon), the
    // means 0.05 % apart.
    CHECK(close >= 0.65);
    CHECK_THAT(gpuMean, WithinAbs(cpuMean, 0.005 * cpuMean));
}

// docs/design/M10.md G6: the any-hit filter (D7): a checker cut-out by an
// opacity program, a half-transparent sphere (stochastic opacity), and the
// shadows they cast.
TEST_CASE("XPU opacity", "[gpu][render][opacity]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    using namespace shading;
    Scene scene;
    MeshDesc ground = makeQuad(3.0f);
    ground.setDisplayColor(spectrum(0.6f));
    scene.updateMesh(scene.createMesh(), ground);
    MeshDesc leaf = makeQuad(0.8f);
    leaf.setAttribute({"st", AttributeRate::Vertex, 2, {0, 0, 1, 0, 1, 1, 0, 1}});
    leaf.transform = translation({-0.8f, 0.0f, 0.6f});
    const MeshId leafId = scene.createMesh();
    scene.updateMesh(leafId, leaf);
    IrGraph g;
    g.model = SurfaceModel::StandardSurface;
    IrBuilder b(g);
    const Ref st = b.primvar("st", 2, b.constant(0.0f));
    const Ref cells = b.unary(Op::Floor, b.mul(st, b.constant(4.0f)));
    const Ref checker = b.binary(Op::Mod, b.add(b.component(cells, 0), b.component(cells, 1)), b.constant(2.0f));
    b.output(surface_field::opacity, 3, checker);
    MaterialDesc cutout;
    cutout.inputs = standardSurfaceDefaults();
    cutout.inputs.set(surface_field::baseColor, Spectrum{0.2f, 0.7f, 0.2f});
    cutout.program = compile(g).program;
    REQUIRE(cutout.program);
    REQUIRE(cutout.program->opacity);
    const MaterialId cutoutId = scene.createMaterial();
    scene.updateMaterial(cutoutId, cutout);
    scene.setMeshMaterials(leafId, {cutoutId});
    MaterialDesc veil;
    veil.inputs = standardSurfaceDefaults();
    veil.inputs.set(surface_field::opacity, spectrum(0.5f));
    const MaterialId veilId = scene.createMaterial();
    scene.updateMaterial(veilId, veil);
    MeshDesc ball = makeSphere(0.5f, 48, 24);
    ball.transform = translation({0.8f, 0.2f, 0.7f});
    const MeshId ballId = scene.createMesh();
    scene.updateMesh(ballId, ball);
    scene.setMeshMaterials(ballId, {veilId});
    LightDesc rect;
    rect.type = LightType::Rect;
    rect.width = rect.height = 1.0f;
    rect.intensity = 6.0f;
    rect.transform = translation({0.0f, 0.0f, 3.0f});
    scene.updateLight(scene.createLight(), rect);
    scene.commit();
    REQUIRE(scene.anyFiltered());
    const Camera camera = makeOrthoCamera({0.3f, 0.3f, 5.0f}, 2.0f, 48, 48);
    const Render cpu = render(scene, camera, 48, 48, 64, RenderDevice::Cpu, 6, true);
    const Render gpu = render(scene, camera, 48, 48, 64, RenderDevice::Xpu, 6, true);
    REQUIRE(gpu.gpu);
    const double close = closePixels(cpu.film, gpu.film), cpuMean = luminanceMean(cpu.film),
                 gpuMean = luminanceMean(gpu.film);
    INFO("pixels within 1e-3: " << close << "; mean CPU " << cpuMean << ", GPU " << gpuMean);
    CHECK(close >= 0.9);
    CHECK_THAT(gpuMean, WithinAbs(cpuMean, 0.005 * cpuMean));
    int alphaSame = 0;
    for (size_t i = 0; i < cpu.film.alphaSum.size(); ++i)
        alphaSame += cpu.film.alphaSum[i] == gpu.film.alphaSum[i];
    CHECK(alphaSame >= int(cpu.film.alphaSum.size()) * 98 / 100);
}

// Fake caustics (M4 3.3): a solid glass ball and a tinted thin-walled pane
// let the light through to the ground as shadow rays' transmittance, and
// paths after a diffuse event pass through them.
TEST_CASE("XPU fake caustics", "[gpu][render][caustics]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    MeshDesc ground = makeQuad(3.0f);
    ground.setDisplayColor(spectrum(0.6f));
    scene.updateMesh(scene.createMesh(), ground);
    MaterialDesc glass;
    glass.inputs = standardSurfaceDefaults();
    glass.inputs.set(surface_field::transmissionWeight, 1.0f);
    glass.inputs.set(surface_field::specularRoughness, 0.0f);
    const MaterialId glassId = scene.createMaterial();
    scene.updateMaterial(glassId, glass);
    MeshDesc ball = makeSphere(0.45f, 48, 24);
    ball.transform = translation({0.6f, 0.1f, 0.9f});
    const MeshId ballId = scene.createMesh();
    scene.updateMesh(ballId, ball);
    scene.setMeshMaterials(ballId, {glassId});
    MaterialDesc tinted = glass;
    tinted.inputs.set(surface_field::transmissionColor, Spectrum{0.9f, 0.5f, 0.2f});
    tinted.inputs.set(surface_field::thinWalled, 1.0f);
    const MaterialId tintedId = scene.createMaterial();
    scene.updateMaterial(tintedId, tinted);
    MeshDesc pane = makeQuad(0.9f);
    pane.transform = translation({-0.7f, -0.2f, 1.2f});
    const MeshId paneId = scene.createMesh();
    scene.updateMesh(paneId, pane);
    scene.setMeshMaterials(paneId, {tintedId});
    LightDesc rect;
    rect.type = LightType::Rect;
    rect.width = rect.height = 1.0f;
    rect.intensity = 6.0f;
    rect.transform = translation({0.0f, 0.0f, 3.0f});
    scene.updateLight(scene.createLight(), rect);
    scene.setDefaultFakeCaustics(true);
    scene.commit();
    REQUIRE(scene.anyFakeCaustics());
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 2.4f, 48, 48);
    const Render cpu = render(scene, camera, 48, 48, 64, RenderDevice::Cpu, 6, true);
    const Render gpu = render(scene, camera, 48, 48, 64, RenderDevice::Xpu, 6, true);
    REQUIRE(gpu.gpu);
    const double close = closePixels(cpu.film, gpu.film), cpuMean = luminanceMean(cpu.film),
                 gpuMean = luminanceMean(gpu.film);
    INFO("pixels within 1e-3: " << close << "; mean CPU " << cpuMean << ", GPU " << gpuMean);
    CHECK(close >= 0.85);
    CHECK_THAT(gpuMean, WithinAbs(cpuMean, 0.01 * cpuMean));
}

// Karma's shading defaults (docs/design/M11-P1.md) on both devices: Karma's
// fake-caustic factor with its controls, no internal reflection, roughness
// constrained along paths, the roughness clamp of true caustics, Karma's
// colour limits. A rough metal ground, a glass ball (fake caustics, Karma's
// style), a second glass ball with true caustics, a mirror and an emissive
// quad that is not a light, under a rect light.
TEST_CASE("XPU Karma's shading defaults", "[gpu][render][caustics][karma]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    MaterialDesc rough;
    rough.inputs = standardSurfaceDefaults();
    rough.inputs.set(surface_field::metalness, 1.0f);
    rough.inputs.set(surface_field::specularRoughness, 0.35f);
    const MaterialId roughId = scene.createMaterial();
    scene.updateMaterial(roughId, rough);
    const MeshId groundId = scene.createMesh();
    scene.updateMesh(groundId, makeQuad(3.0f));
    scene.setMeshMaterials(groundId, {roughId});
    MaterialDesc glass;
    glass.inputs = standardSurfaceDefaults();
    glass.inputs.set(surface_field::transmissionWeight, 1.0f);
    glass.inputs.set(surface_field::specularRoughness, 0.0f);
    glass.inputs.set(surface_field::transmissionColor, Spectrum{0.9f, 0.7f, 0.5f});
    const MaterialId glassId = scene.createMaterial();
    scene.updateMaterial(glassId, glass);
    MaterialDesc mirror;
    mirror.inputs = standardSurfaceDefaults();
    mirror.inputs.set(surface_field::metalness, 1.0f);
    mirror.inputs.set(surface_field::specularRoughness, 0.0f);
    const MaterialId mirrorId = scene.createMaterial();
    scene.updateMaterial(mirrorId, mirror);
    MaterialDesc glow;
    glow.inputs = standardSurfaceDefaults();
    glow.inputs.set(surface_field::baseWeight, 0.0f);
    glow.inputs.set(surface_field::specularWeight, 0.0f);
    glow.inputs.set(surface_field::emissionWeight, 1.0f);
    glow.inputs.set(surface_field::emissionColor, spectrum(30.0f));
    const MaterialId glowId = scene.createMaterial();
    scene.updateMaterial(glowId, glow);
    const auto add = [&](MeshDesc m, Vec3f at, MaterialId mat) {
        m.transform = translation(at);
        const MeshId id = scene.createMesh();
        scene.updateMesh(id, m);
        scene.setMeshMaterials(id, {mat});
        return id;
    };
    add(makeSphere(0.45f, 48, 24), {0.6f, 0.1f, 0.9f}, glassId);
    const MeshId trueGlass = add(makeSphere(0.35f, 48, 24), {-0.7f, 0.6f, 0.7f}, glassId);
    add(makeSphere(0.3f, 48, 24), {-0.5f, -0.8f, 0.6f}, mirrorId);
    add(makeQuad(0.25f), {0.8f, -0.9f, 0.3f}, glowId);
    RayLimitOverrides caustics; // true caustics with the clamp
    caustics.fakeCaustics = 0;
    scene.setMeshRayLimits(trueGlass, caustics);
    LightDesc rect;
    rect.type = LightType::Rect;
    rect.width = rect.height = 1.0f;
    rect.intensity = 6.0f;
    rect.transform = translation({0.0f, 0.0f, 3.0f});
    scene.updateLight(scene.createLight(), rect);
    RayLimits defaults;
    defaults.fakeCaustics = true;
    defaults.fakeStyle.karma = true;
    defaults.fakeStyle.color[1] = 0.8f;
    defaults.fakeStyle.opacity[2] = 0.7f;
    defaults.fakeStyle.fresnelDarken = 0.3f;
    defaults.internalReflection = false;
    defaults.causticsRoughnessClamp = 0.2f;
    scene.setObjectDefaults(defaults);
    scene.commit();
    REQUIRE(scene.anyFakeCaustics());
    RenderSettings base;
    base.constrainRoughness = true;
    base.clampDirect = 20.0f;
    base.clampIndirect = 4.0f;
    base.clampEmissionIndirect = true;
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 2.4f, 48, 48);
    const Render cpu = render(scene, camera, 48, 48, 64, RenderDevice::Cpu, 6, true, base);
    const Render gpu = render(scene, camera, 48, 48, 64, RenderDevice::Xpu, 6, true, base);
    REQUIRE(gpu.gpu);
    const double close = closePixels(cpu.film, gpu.film), cpuMean = luminanceMean(cpu.film),
                 gpuMean = luminanceMean(gpu.film);
    INFO("pixels within 1e-3: " << close << "; mean CPU " << cpuMean << ", GPU " << gpuMean);
    CHECK(close >= 0.8);
    CHECK_THAT(gpuMean, WithinAbs(cpuMean, 0.01 * cpuMean));
    // The settings change the image (the test exercises them).
    const Render plain = render(scene, camera, 48, 48, 64, RenderDevice::Cpu, 6, true);
    CHECK(std::fabs(luminanceMean(plain.film) - cpuMean) > 0.002 * cpuMean);
}

// Ambient occlusion has no fake-caustics phase (M6 D9): an opaque roof
// occludes the floor's AO rays with the render-wide setting on, on both
// devices (the light samples' shadow rays alone pass such objects).
TEST_CASE("XPU ambient occlusion under fake-caustic objects", "[gpu][render][aov][caustics]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    scene.updateMesh(scene.createMesh(), makeQuad(10.0f));
    MeshDesc roof = makeQuad(500.0f);
    roof.transform = translation({0.0f, 0.0f, 0.5f});
    scene.updateMesh(scene.createMesh(), roof);
    scene.setDefaultFakeCaustics(true);
    scene.commit();
    REQUIRE(scene.anyFakeCaustics());
    const auto aoMean = [&](RenderDevice device, bool &gpu) {
        Renderer renderer;
        RenderSettings settings;
        settings.device = device;
        settings.maxSamples = 32;
        settings.maxDepth = 1;
        settings.utility = std::make_shared<std::vector<UtilityRequest>>(
            std::vector<UtilityRequest>{{UtilityKind::Ao, {}}});
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 0.2f}, 1.0f, 8, 8));
        renderer.resize(8, 8);
        gpu = true;
        while (!renderer.converged()) {
            renderer.renderPass({});
            gpu = gpu && renderer.lastPassOnGpu();
        }
        const Film &film = renderer.film();
        double sum = 0.0;
        for (float v : film.utility[0].data)
            sum += v;
        return sum / (64.0 * film.samples);
    };
    bool cpuGpu = false, gpuGpu = false;
    const double cpu = aoMean(RenderDevice::Cpu, cpuGpu);
    const double gpu = aoMean(RenderDevice::Xpu, gpuGpu);
    REQUIRE(gpuGpu);
    CHECK_THAT(cpu, WithinAbs(0.0, 1e-3));
    CHECK_THAT(gpu, WithinAbs(0.0, 1e-3));
}

// Opacity programs run at the render's frame on both devices (the CPU's
// hit filter as the GPU's any-hit program): a quad opaque up to frame 5 is
// cut out at frame 10, the same alpha everywhere.
TEST_CASE("XPU opacity programs at the render's frame", "[gpu][render][opacity]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    using namespace shading;
    Scene scene;
    const MeshId quad = scene.createMesh();
    scene.updateMesh(quad, makeQuad(1.0f));
    IrGraph g;
    g.model = SurfaceModel::StandardSurface;
    IrBuilder b(g);
    b.output(surface_field::opacity, 3, b.binary(Op::GreaterEq, b.constant(5.0f), b.global(Global::Frame)));
    MaterialDesc material;
    material.inputs = standardSurfaceDefaults();
    material.program = compile(g).program;
    REQUIRE(material.program);
    REQUIRE(material.program->opacity);
    const MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, material);
    scene.setMeshMaterials(quad, {m});
    scene.commit();
    const auto alpha = [&](RenderDevice device, float frame, bool &gpu) {
        Renderer renderer;
        RenderSettings settings;
        settings.device = device;
        settings.maxSamples = 4;
        settings.frame = frame;
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 0.5f, 8, 8));
        renderer.resize(8, 8);
        gpu = true;
        while (!renderer.converged()) {
            renderer.renderPass({});
            gpu = gpu && renderer.lastPassOnGpu();
        }
        const Film &film = renderer.film();
        double sum = 0.0;
        for (size_t i = 0; i < film.alphaSum.size(); ++i)
            sum += film.alphaSum[i] / float(film.count[i]);
        return sum / double(film.alphaSum.size());
    };
    for (const float frame : {1.0f, 10.0f}) {
        INFO("frame " << frame);
        bool cpuGpu = false, gpuGpu = false;
        const double cpu = alpha(RenderDevice::Cpu, frame, cpuGpu);
        const double gpu = alpha(RenderDevice::Xpu, frame, gpuGpu);
        REQUIRE(gpuGpu);
        const double expected = frame > 5.0f ? 0.0 : 1.0;
        CHECK_THAT(cpu, WithinAbs(expected, 1e-6));
        CHECK_THAT(gpu, WithinAbs(expected, 1e-6));
    }
}

// Subsurface scattering (M9): random walks through a sphere with a
// chromatic mean free path and a moved, scaled box instance (the walk's
// object space), lit by a rect light; their exits sample the light and
// continue.
TEST_CASE("XPU subsurface walks", "[gpu][render][sss]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    namespace sf = surface_field;
    Scene scene;
    MeshDesc ground = makeQuad(3.0f);
    ground.setDisplayColor(spectrum(0.6f));
    scene.updateMesh(scene.createMesh(), ground);
    MaterialDesc skin;
    skin.inputs = standardSurfaceDefaults();
    skin.inputs.set(sf::subsurfaceWeight, 1.0f);
    skin.inputs.set(sf::subsurfaceColor, Spectrum{0.9f, 0.6f, 0.4f});
    skin.inputs.set(sf::subsurfaceRadius, Spectrum{1.0f, 0.4f, 0.2f});
    skin.inputs.set(sf::subsurfaceScale, 0.2f);
    const MaterialId skinId = scene.createMaterial();
    scene.updateMaterial(skinId, skin);
    MeshDesc ball = makeSphere(0.45f, 48, 24);
    ball.transform = translation({0.5f, 0.2f, 0.5f});
    const MeshId ballId = scene.createMesh();
    scene.updateMesh(ballId, ball);
    scene.setMeshMaterials(ballId, {skinId});
    MaterialDesc wax = skin;
    wax.inputs.set(sf::subsurfaceColor, spectrum(0.8f));
    wax.inputs.set(sf::subsurfaceRadius, spectrum(1.0f));
    wax.inputs.set(sf::subsurfaceScale, 0.1f);
    const MaterialId waxId = scene.createMaterial();
    scene.updateMaterial(waxId, wax);
    MeshDesc block = box({-0.5f, -0.5f, 0.0f}, {0.5f, 0.5f, 1.0f});
    Mat4 squash = Mat4::identity();
    squash.m[0][0] = 0.6f;
    squash.m[1][1] = 0.8f;
    squash.m[2][2] = 0.7f;
    block.transform = squash * translation({-0.6f, -0.3f, 0.0f});
    const MeshId blockId = scene.createMesh();
    scene.updateMesh(blockId, block);
    scene.setMeshMaterials(blockId, {waxId});
    LightDesc rect;
    rect.type = LightType::Rect;
    rect.width = rect.height = 1.0f;
    rect.intensity = 6.0f;
    rect.transform = translation({0.0f, 0.0f, 3.0f});
    scene.updateLight(scene.createLight(), rect);
    scene.commit();
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 2.4f, 48, 48);
    const Render cpu = render(scene, camera, 48, 48, 64, RenderDevice::Cpu, 6, true);
    const Render gpu = render(scene, camera, 48, 48, 64, RenderDevice::Xpu, 6, true);
    REQUIRE(gpu.gpu);
    const double close = closePixels(cpu.film, gpu.film), cpuMean = luminanceMean(cpu.film),
                 gpuMean = luminanceMean(gpu.film);
    INFO("pixels within 1e-3: " << close << "; mean CPU " << cpuMean << ", GPU " << gpuMean);
    CHECK(close >= 0.8);
    CHECK_THAT(gpuMean, WithinAbs(cpuMean, 0.01 * cpuMean));
}

// Passes over pixel lists (G7): IPR proxy passes (one pixel per block, the
// blocks filled in) and adaptive sampling (pixels stop by the variance
// oracle) render on the GPU as on the CPU.
TEST_CASE("XPU proxy passes and adaptive sampling", "[gpu][render][adaptive]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    cornellBox(scene);
    const Camera camera = makePerspectiveCamera({0.0f, 1.0f, 3.6f}, 0.05, 0.036, 48, 48);
    const auto run = [&](RenderDevice device, bool adaptive, int proxy, bool &allGpu) {
        Renderer renderer;
        RenderSettings settings;
        settings.device = device;
        settings.maxSamples = 128;
        settings.maxDepth = 6;
        settings.adaptive = adaptive;
        settings.oracleThreshold = 0.05f;
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(camera);
        renderer.setProxyDepth(proxy);
        renderer.resize(48, 48);
        allGpu = true;
        while (!renderer.converged()) {
            renderer.renderPass({});
            allGpu = allGpu && renderer.lastPassOnGpu();
        }
        return renderer.film();
    };
    for (const bool adaptive : {false, true}) {
        bool cpuGpu = false, gpuAll = false;
        const Film cpu = run(RenderDevice::Cpu, adaptive, 2, cpuGpu);
        const Film gpu = run(RenderDevice::Xpu, adaptive, 2, gpuAll);
        CHECK(gpuAll);
        int sameCount = 0;
        uint64_t cpuSamples = 0, gpuSamples = 0;
        for (size_t i = 0; i < cpu.count.size(); ++i) {
            sameCount += cpu.count[i] == gpu.count[i];
            cpuSamples += cpu.count[i];
            gpuSamples += gpu.count[i];
        }
        const double close = closePixels(cpu, gpu), cpuMean = luminanceMean(cpu), gpuMean = luminanceMean(gpu);
        INFO("adaptive " << adaptive << ": pixels within 1e-3 " << close << ", same counts " << sameCount << " of "
                         << cpu.count.size() << ", samples CPU " << cpuSamples << " GPU " << gpuSamples << "; mean CPU "
                         << cpuMean << ", GPU " << gpuMean);
        CHECK(sameCount >= int(cpu.count.size()) * 95 / 100);
        CHECK(close >= (adaptive ? 0.9 : 0.98));
        CHECK_THAT(gpuMean, WithinAbs(cpuMean, 0.005 * cpuMean));
    }
}

// Adaptive passes put several samples of a pixel into one wave (G7); their
// threads add to the same LPE sums, which must not lose updates: C.*L (every
// path to a light, the Cornell box has nothing else) still sums to the
// beauty, as on the CPU.
TEST_CASE("XPU light path expressions with adaptive sampling", "[gpu][render][aov][lpe][adaptive]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    cornellBox(scene);
    const auto compiled = std::make_shared<lpe::Compiled>(lpe::compile(std::vector<std::string>{"C.*L"}));
    REQUIRE(compiled->ok());
    Renderer renderer;
    RenderSettings settings;
    settings.device = RenderDevice::Xpu;
    settings.maxSamples = 256;
    settings.maxDepth = 6;
    settings.adaptive = true;
    settings.oracleThreshold = 0.05f;
    settings.lpe = compiled;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makePerspectiveCamera({0.0f, 1.0f, 3.6f}, 0.05, 0.036, 32, 32));
    renderer.resize(32, 32);
    bool onGpu = true;
    while (!renderer.converged()) {
        renderer.renderPass({});
        onGpu = onGpu && renderer.lastPassOnGpu();
    }
    REQUIRE(onGpu);
    const Film &film = renderer.film();
    REQUIRE(film.lpeOutputs == 1u);
    double beauty = 0.0, lpeSum = 0.0;
    int close = 0;
    for (size_t i = 0; i < film.colorSum.size(); ++i) {
        const float a = luminance(film.colorSum[i]), b = luminance(film.lpe(0)[i]);
        beauty += a;
        lpeSum += b;
        close += std::fabs(a - b) <= 1e-3f * std::max(1.0f, std::fabs(a));
    }
    INFO("beauty " << beauty << ", C.*L " << lpeSum << "; pixels close " << close);
    CHECK(close >= int(film.colorSum.size()) * 99 / 100);
    CHECK_THAT(lpeSum, WithinAbs(beauty, 1e-3 * beauty));
}

// The fallback (D15): without a usable device (here NOVA_XPU_DISABLE) Nova
// XPU renders on the CPU, the CPU's image. Runs without a GPU too.
TEST_CASE("XPU falls back to the CPU", "[gpu][render][fallback]") {
    const auto setDisable = [](const char *value) {
#if defined(_WIN32)
        _putenv_s("NOVA_XPU_DISABLE", value);
#else
        if (*value)
            setenv("NOVA_XPU_DISABLE", value, 1);
        else
            unsetenv("NOVA_XPU_DISABLE");
#endif
    };
    Scene scene;
    cornellBox(scene);
    const Camera camera = makePerspectiveCamera({0.0f, 1.0f, 3.6f}, 0.05, 0.036, 32, 32);
    setDisable("1");
    const Render xpu = render(scene, camera, 32, 32, 8, RenderDevice::Xpu);
    setDisable("");
    const Render cpu = render(scene, camera, 32, 32, 8, RenderDevice::Cpu);
    CHECK(!xpu.gpu);
    int same = 0;
    for (size_t i = 0; i < cpu.film.colorSum.size(); ++i) {
        const Spectrum a = xpu.film.colorSum[i], b = cpu.film.colorSum[i];
        same += a.x == b.x && a.y == b.y && a.z == b.z;
    }
    CHECK(same == int(cpu.film.colorSum.size()));
    CHECK(xpu.film.count == cpu.film.count);
}

// IPR (D14): the GPU keeps the geometry and GAS of unchanged meshes between
// scene versions; after edits of geometry, a transform, a material, the
// opacity filtering and a removal, a renderer that saw every version renders
// what a fresh one renders, bit for bit.
TEST_CASE("XPU incremental scene updates", "[gpu][render][ipr]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    cornellBox(scene);
    MeshDesc extra = makeSphere(0.2f, 32, 16);
    extra.transform = translation({-0.5f, 1.6f, 0.3f});
    const MeshId extraId = scene.createMesh();
    scene.updateMesh(extraId, extra);
    MeshDesc blob = makeSphere(0.15f, 24, 12);
    blob.transform = translation({0.6f, 1.5f, 0.2f});
    const MeshId blobId = scene.createMesh();
    scene.updateMesh(blobId, blob);
    scene.commit();
    const Camera camera = makePerspectiveCamera({0.0f, 1.0f, 3.6f}, 0.05, 0.036, 40, 40);
    Renderer live;
    RenderSettings settings;
    settings.device = RenderDevice::Xpu;
    settings.maxSamples = 16;
    settings.maxDepth = 6;
    live.setScene(&scene);
    live.setSettings(settings);
    live.setCamera(camera);
    live.resize(40, 40);
    const auto converge = [](Renderer &r) {
        while (!r.converged())
            r.renderPass({});
    };
    converge(live);
    // Geometry, a transform, a material with opacity (the mesh becomes
    // filtered), and a removal.
    MeshDesc bigger = makeSphere(0.3f, 32, 16);
    bigger.transform = translation({-0.5f, 1.5f, 0.3f});
    scene.updateMesh(extraId, bigger);
    scene.updateMeshTransform(extraId, translation({-0.4f, 1.4f, 0.2f}));
    MaterialDesc veil;
    veil.inputs = standardSurfaceDefaults();
    veil.inputs.set(surface_field::opacity, spectrum(0.5f));
    veil.inputs.set(surface_field::baseColor, Spectrum{0.2f, 0.3f, 0.9f});
    const MaterialId veilId = scene.createMaterial();
    scene.updateMaterial(veilId, veil);
    scene.setMeshMaterials(extraId, {veilId});
    scene.removeMesh(blobId);
    scene.commit();
    live.reset();
    converge(live);
    Renderer fresh;
    fresh.setScene(&scene);
    fresh.setSettings(settings);
    fresh.setCamera(camera);
    fresh.resize(40, 40);
    converge(fresh);
    REQUIRE(live.lastPassOnGpu());
    REQUIRE(fresh.lastPassOnGpu());
    int same = 0;
    for (size_t i = 0; i < live.film().colorSum.size(); ++i) {
        const Spectrum a = live.film().colorSum[i], b = fresh.film().colorSum[i];
        same += a.x == b.x && a.y == b.y && a.z == b.z;
    }
    CHECK(same == int(live.film().colorSum.size()));
}

// Material sort (D8, decided in G8), not run by default: 64 spheres with 64
// different per-hit programs (noise of distinct frequencies and models)
// against the same scene with one of them everywhere, GPU pass time.
TEST_CASE("XPU material divergence", "[.][gpu][bench]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    using namespace shading;
    const auto program = [](int i) {
        IrGraph g;
        g.model = i % 2 ? SurfaceModel::StandardSurface : SurfaceModel::OpenPBR;
        IrBuilder b(g);
        const Ref p = b.global(Global::P);
        const Ref scaled = b.mul(p, b.constant(1.0f + float(i)));
        const Ref n = b.unary(Op::Sin, b.add(b.component(scaled, 0), b.mul(b.component(scaled, 1), b.constant(float(i % 7)))));
        const Ref c = b.add(b.mul(n, b.constant(0.4f)), b.constant(0.5f));
        b.output(surface_field::baseColor, 3, b.mul(c, b.constant(0.2f + 0.01f * float(i), 0.6f, 0.9f - 0.01f * float(i))));
        b.output(surface_field::specularRoughness, 1, b.mul(c, b.constant(0.5f)));
        if (i % 3 == 0)
            b.output(surface_field::metalness, 1, c);
        MaterialDesc m;
        m.inputs = i % 2 ? standardSurfaceDefaults() : openPbrDefaults();
        m.program = compile(g).program;
        return m;
    };
    const auto build = [&](Scene &scene, bool many) {
        MeshDesc ground = makeQuad(8.0f);
        ground.setDisplayColor(spectrum(0.5f));
        scene.updateMesh(scene.createMesh(), ground);
        std::vector<MaterialId> mats;
        for (int i = 0; i < (many ? 64 : 1); ++i) {
            mats.push_back(scene.createMaterial());
            scene.updateMaterial(mats.back(), program(i));
        }
        for (int i = 0; i < 64; ++i) {
            MeshDesc ball = makeSphere(0.25f, 48, 24);
            ball.transform = translation({-3.5f + float(i % 8), -3.5f + float(i / 8), 0.25f});
            const MeshId id = scene.createMesh();
            scene.updateMesh(id, ball);
            scene.setMeshMaterials(id, {mats[size_t(i) % mats.size()]});
        }
        LightDesc dome;
        dome.type = LightType::Dome;
        dome.intensity = 1.0f;
        scene.updateLight(scene.createLight(), dome);
        scene.commit();
    };
    const Camera camera = makePerspectiveCamera({0.0f, -7.0f, 6.0f}, 0.05, 0.036, 960, 540);
    for (const bool many : {false, true}) {
        Scene scene;
        build(scene, many);
        Renderer renderer;
        RenderSettings settings;
        settings.device = RenderDevice::Xpu;
        settings.maxSamples = 64;
        renderer.setScene(&scene);
        renderer.setSettings(settings);
        renderer.setCamera(camera);
        renderer.resize(960, 540);
        renderer.renderPass({}); // uploads
        const auto t0 = std::chrono::steady_clock::now();
        while (!renderer.converged())
            renderer.renderPass({});
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        WARN((many ? "64 materials" : "1 material") << ": " << ms / 63.0 << " ms per pass");
    }
}

// IPR latency (D14), not run by default: the first pass after an edit of a
// light, a material and a transform in a scene of about 4M triangles.
TEST_CASE("XPU edit latency", "[.][gpu][ipr]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    MeshDesc ground = makeQuad(6.0f);
    ground.setDisplayColor(spectrum(0.5f));
    scene.updateMesh(scene.createMesh(), ground);
    std::vector<MeshId> balls;
    for (int i = 0; i < 16; ++i) {
        MeshDesc ball = makeSphere(0.3f, 512, 256);
        ball.transform = translation({-2.4f + 1.6f * float(i % 4), -2.4f + 1.6f * float(i / 4), 0.3f});
        ball.setDisplayColor(Spectrum{0.2f + 0.05f * float(i), 0.5f, 0.6f});
        balls.push_back(scene.createMesh());
        scene.updateMesh(balls.back(), ball);
    }
    const MaterialId mat = scene.createMaterial();
    MaterialDesc metal;
    metal.inputs = standardSurfaceDefaults();
    metal.inputs.set(surface_field::metalness, 1.0f);
    scene.updateMaterial(mat, metal);
    scene.setMeshMaterials(balls[0], {mat});
    LightDesc rect;
    rect.type = LightType::Rect;
    rect.width = rect.height = 2.0f;
    rect.intensity = 4.0f;
    rect.transform = translation({0.0f, 0.0f, 4.0f});
    const LightId light = scene.createLight();
    scene.updateLight(light, rect);
    scene.commit();
    const Camera camera = makePerspectiveCamera({0.0f, -6.0f, 4.0f}, 0.05, 0.036, 640, 360);
    Renderer renderer;
    RenderSettings settings;
    settings.device = RenderDevice::Xpu;
    settings.maxSamples = 1000;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(camera);
    renderer.resize(640, 360);
    const auto pass = [&](const char *what) {
        const auto t0 = std::chrono::steady_clock::now();
        renderer.renderPass({});
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        WARN(what << ": " << ms << " ms (GPU " << renderer.lastPassOnGpu() << ")");
    };
    pass("first pass (upload)");
    pass("second pass");
    rect.intensity = 6.0f;
    scene.updateLight(light, rect);
    scene.commit();
    renderer.reset();
    pass("after a light edit");
    metal.inputs.set(surface_field::baseColor, Spectrum{0.9f, 0.2f, 0.1f});
    scene.updateMaterial(mat, metal);
    scene.commit();
    renderer.reset();
    pass("after a material edit");
    scene.updateMeshTransform(balls[3], translation({0.0f, 0.0f, 1.0f}));
    scene.commit();
    renderer.reset();
    pass("after a transform edit");
    pass("next pass");
}

TEST_CASE("XPU renders are bit-identical", "[gpu][render]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    cornellBox(scene);
    const Camera camera = makePerspectiveCamera({0.0f, 1.0f, 3.6f}, 0.05, 0.036, 40, 40);
    const Render a = render(scene, camera, 40, 40, 16, RenderDevice::Xpu);
    const Render b = render(scene, camera, 40, 40, 16, RenderDevice::Xpu);
    REQUIRE(a.gpu);
    REQUIRE(b.gpu);
    int different = 0;
    for (size_t i = 0; i < a.film.colorSum.size(); ++i)
        different += a.film.colorSum[i].x != b.film.colorSum[i].x || a.film.colorSum[i].y != b.film.colorSum[i].y ||
                     a.film.colorSum[i].z != b.film.colorSum[i].z || a.film.alphaSum[i] != b.film.alphaSum[i];
    CHECK(different == 0);
}

// M11 P6 (docs/design/M11-P6.md): the per-sample planes (Karma's sample
// filters), the deep map and a checkpoint resume on the XPU match the CPU.
TEST_CASE("XPU per-sample AOVs, deep maps and checkpoints", "[gpu][render][aov][deep]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    cornellBox(scene);
    const Camera camera = makePerspectiveCamera({0.0f, 1.0f, 3.6f}, 0.05, 0.036, 32, 32);
    using U = UtilityKind;
    using F = SampleFilter;
    RenderSettings settings;
    settings.maxSamples = 16;
    settings.maxDepth = 3;
    settings.utility = std::make_shared<const std::vector<UtilityRequest>>(std::vector<UtilityRequest>{
        {U::Z, {}, F::ZMin}, {U::Z, {}, F::Box}, {U::P, {}, F::Box}, {U::ObjectId, {}, F::Max}, {U::Mask, {}, F::Box},
        {U::Time, {}, F::Var}, {U::HitUv, {}, F::Box}, {U::Beauty, {}, F::Max}});
    DeepRequest deep;
    deep.extras = {{U::Z, {}}};
    settings.deep = std::make_shared<const DeepRequest>(deep);
    const auto run = [&](RenderDevice device, int stopAfter, std::vector<uint8_t> *checkpoint) {
        Renderer renderer;
        RenderSettings s = settings;
        s.device = device;
        renderer.setScene(&scene);
        renderer.setSettings(s);
        renderer.setCamera(camera);
        renderer.resize(32, 32);
        if (checkpoint && stopAfter < 0)
            REQUIRE(renderer.loadCheckpoint(checkpoint->data(), checkpoint->size()));
        bool gpu = true;
        for (int pass = 0; !renderer.converged() && (stopAfter < 0 || pass < stopAfter); ++pass) {
            renderer.renderPass({});
            gpu = gpu && renderer.lastPassOnGpu();
        }
        if (checkpoint && stopAfter >= 0)
            renderer.saveCheckpoint(*checkpoint);
        CHECK(gpu == (device == RenderDevice::Xpu));
        return renderer.film();
    };
    const Film cpu = run(RenderDevice::Cpu, -1, nullptr);
    const Film xpu = run(RenderDevice::Xpu, -1, nullptr);
    REQUIRE(cpu.utility.size() == xpu.utility.size());
    for (size_t k = 0; k < cpu.utility.size(); ++k) {
        const UtilityPlane &a = cpu.utility[k], &b = xpu.utility[k];
        if (a.request.kind == U::Beauty)
            continue; // the path's radiance differs per sample between the devices (float order)
        size_t far = 0;
        for (size_t i = 0; i < a.data.size(); ++i)
            far += std::fabs(a.data[i] - b.data[i]) > 1e-3f * std::max(1.0f, std::fabs(a.data[i]));
        INFO("plane " << k);
        CHECK(far <= a.data.size() / 200); // the same camera hits (rare float ties at edges)
    }
    // The deep map: the same records' depths and coverage per pixel.
    size_t sameDepths = 0;
    for (size_t i = 0; i < cpu.deep[0].pixels.size(); ++i)
        sameDepths += cpu.deep[0].pixels[i].size() == xpu.deep[0].pixels[i].size();
    CHECK(sameDepths >= cpu.deep[0].pixels.size() * 99 / 100);
    // A render saved on the XPU and resumed there equals the uninterrupted one.
    std::vector<uint8_t> bytes;
    run(RenderDevice::Xpu, 6, &bytes);
    const Film resumed = run(RenderDevice::Xpu, -1, &bytes);
    CHECK(resumed.count == xpu.count);
    CHECK(std::memcmp(resumed.colorSum.data(), xpu.colorSum.data(), xpu.colorSum.size() * sizeof(Spectrum)) == 0);
    CHECK(resumed.utility[0].data == xpu.utility[0].data);
    CHECK(resumed.deep[0].pixels == xpu.deep[0].pixels);
}

// docs/design/M11-P7.md: the Karma Sky Atmosphere (its default planet, +Z
// up, the camera 100 m above sea level) lit by a sun disc at 15 degrees, with
// multiple scattering (volume limit 2), a visible ground below the horizon,
// a fog box inside the atmosphere (its events compete with the
// atmosphere's) and a lit quad: the CPU's image means.
TEST_CASE("XPU sky atmosphere", "[gpu][render][atmosphere]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    using namespace shading;
    Scene scene;
    auto atmosphere = std::make_shared<AtmosphereDesc>();
    atmosphere->center[0] = 0.0;
    atmosphere->center[1] = 0.0;
    atmosphere->center[2] = -6371000.0;
    atmosphere->seaLevel = 6370900.0;
    atmosphere->groundVisibility = kVisibleAll;
    atmosphere->groundAlbedo = Spectrum{0.3f, 0.25f, 0.2f};
    {
        VolumeDesc v;
        v.atmosphere = atmosphere;
        v.volumeLimit = 2.0f;
        scene.updateVolume(scene.createVolume(), v);
    }
    { // a fog box 20 m ahead
        IrGraph g;
        IrBuilder b(g);
        b.output(uint16_t(volume_field::kBase + volume_field::scattering), 3, b.constant(0.05f, 0.05f, 0.05f));
        MaterialDesc m;
        m.program = compile(g).program;
        VolumeDesc v;
        const MeshDesc cube = [] {
            MeshDesc c;
            const float x0 = -4.0f, x1 = 4.0f, y0 = 16.0f, y1 = 24.0f, z0 = -3.0f, z1 = 5.0f;
            c.points = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
                        {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
            c.indices = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                         1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
            return c;
        }();
        v.shell = makeTriangleShell(cube.points.data(), cube.pointCount(), cube.indices.data(), cube.triangleCount());
        v.material = scene.createMaterial();
        scene.updateMaterial(v.material, m);
        const VolumeId id = scene.createVolume();
        scene.updateVolume(id, v);
        scene.updateVolumeMotion(id, {Mat4::identity()});
    }
    MeshDesc quad = makeQuad(2.0f);
    quad.setDisplayColor(spectrum(0.6f));
    quad.transform = translation({0.0f, 10.0f, -2.0f});
    scene.updateMesh(scene.createMesh(), quad);
    LightDesc sun;
    sun.type = LightType::Distant;
    sun.normalize = true;
    sun.angleDeg = 0.53f;
    sun.cameraVisible = true;
    {
        const float a = 15.0f * kPi / 180.0f;
        const Frame f = Frame::fromNormal({0.0f, std::cos(a), std::sin(a)});
        const Vec3f axes[3] = {f.t, f.b, f.n};
        for (int r = 0; r < 3; ++r) {
            sun.transform.m[r][0] = axes[r].x;
            sun.transform.m[r][1] = axes[r].y;
            sun.transform.m[r][2] = axes[r].z;
        }
    }
    scene.updateLight(scene.createLight(), sun);
    scene.commit();
    REQUIRE(scene.atmosphere() != nullptr);
    // Looking along +Y, slightly down: sky, sun, horizon, ground, fog and quad.
    Camera camera = makePerspectiveCamera({0.0f, 0.0f, 0.0f}, 10.0, 24.0, 32, 32, 0.01, 1.0e6);
    Mat4 m = Mat4::identity();
    m.m[1][1] = 0.0f; // local -Z to +Y, local +Y to +Z
    m.m[1][2] = 1.0f;
    m.m[2][1] = -1.0f;
    m.m[2][2] = 0.0f;
    camera.viewToWorld = m;
    camera.worldToView = inverse(m);
    const Render cpu = render(scene, camera, 32, 32, 512, RenderDevice::Cpu, 8, true);
    const Render gpu = render(scene, camera, 32, 32, 512, RenderDevice::Xpu, 8, true);
    REQUIRE(gpu.gpu);
    const auto clipped = [](const Film &f) { // the sun disc's few pixels dominate a plain mean
        double sum = 0.0;
        for (size_t i = 0; i < f.colorSum.size(); ++i)
            sum += std::min(double(luminance(f.colorSum[i])) / double(f.count[i]), 1.0);
        return sum / double(f.colorSum.size());
    };
    const double cpuMean = clipped(cpu.film), gpuMean = clipped(gpu.film);
    INFO("mean CPU " << cpuMean << ", GPU " << gpuMean);
    CHECK_THAT(gpuMean, WithinAbs(cpuMean, 0.01 * cpuMean));
    for (int q = 0; q < 4; ++q) {
        double a = 0.0, b = 0.0;
        for (int y = (q / 2) * 16; y < (q / 2) * 16 + 16; ++y)
            for (int x = (q % 2) * 16; x < (q % 2) * 16 + 16; ++x) {
                const size_t i = size_t(y) * 32 + size_t(x);
                a += std::min(double(luminance(cpu.film.colorSum[i])) / cpu.film.count[i], 1.0);
                b += std::min(double(luminance(gpu.film.colorSum[i])) / gpu.film.count[i], 1.0);
            }
        INFO("quadrant " << q << ": CPU " << a / 256.0 << ", GPU " << b / 256.0);
        CHECK_THAT(b, WithinAbs(a, 0.02 * a));
    }
    // Alpha: the atmosphere's opacity above the horizon, 1 on the ground.
    double ca = 0.0, ga = 0.0;
    for (size_t i = 0; i < cpu.film.alphaSum.size(); ++i) {
        ca += cpu.film.alphaSum[i] / cpu.film.count[i];
        ga += gpu.film.alphaSum[i] / gpu.film.count[i];
    }
    CHECK_THAT(ga, WithinAbs(ca, 1e-3 * ca));
}
