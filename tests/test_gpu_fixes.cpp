// Regression tests for Nova XPU host-side fixes (buffer sizing, primvar
// rates, film read-back, caches). Skipped without a usable NVIDIA device,
// like test_gpu_render.cpp.

#if defined(_MSC_VER)
#pragma warning(disable : 4702) // unreachable code in NanoVDB's grid builder templates
#endif

#include "test_helpers.h"

#include "core/film/lpe_compile.h"
#include "core/geometry/curves.h"
#include "core/gpu/device.h"
#include "core/integrator/renderer.h"
#include "core/scene/scene.h"
#include "core/shading/compiler.h"
#include "core/texture/memory_sampler.h"

#include <nanovdb/util/CreateNanoGrid.h>
#include <nanovdb/util/GridBuilder.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;

namespace {

bool gpuAvailable(std::string &why) {
    gpu::Device device;
    return device.init(why);
}

struct Render {
    Film film;
    bool gpu = false;
};

Render render(const Scene &scene, const Camera &camera, int width, int height, uint32_t spp, RenderDevice device,
              uint32_t maxDepth, std::shared_ptr<const lpe::Compiled> lpe = nullptr, float clampIndirect = 10.0f) {
    Renderer renderer;
    RenderSettings settings;
    settings.device = device;
    settings.maxSamples = spp;
    settings.maxDepth = maxDepth;
    settings.lpe = std::move(lpe);
    settings.clampIndirect = clampIndirect;
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

// The mean of the film's samples per channel.
Vec3f channelMeans(const Film &film) {
    double sum[3] = {0.0, 0.0, 0.0};
    for (size_t i = 0; i < film.colorSum.size(); ++i) {
        if (film.count[i] == 0)
            continue;
        sum[0] += double(film.colorSum[i].x) / double(film.count[i]);
        sum[1] += double(film.colorSum[i].y) / double(film.count[i]);
        sum[2] += double(film.colorSum[i].z) / double(film.count[i]);
    }
    const double n = double(film.colorSum.size());
    return {float(sum[0] / n), float(sum[1] / n), float(sum[2] / n)};
}

double luminanceSum(const Spectrum *v, size_t n) {
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i)
        sum += double(luminance(v[i]));
    return sum;
}

LightDesc frontLight(float size, float intensity) {
    LightDesc rect;
    rect.type = LightType::Rect;
    rect.width = rect.height = size;
    rect.intensity = intensity;
    rect.transform = translation({0.0f, 0.0f, 3.0f});
    return rect;
}

// A ground, a sphere and a light (the LPE read-back tests).
void simpleScene(Scene &scene) {
    MeshDesc ground = makeQuad(2.0f);
    ground.setDisplayColor(spectrum(0.6f));
    ground.transform = translation({0.0f, 0.0f, -0.5f});
    scene.updateMesh(scene.createMesh(), ground);
    MeshDesc ball = makeSphere(0.5f, 32, 16);
    ball.setDisplayColor(Spectrum{0.8f, 0.4f, 0.2f});
    scene.updateMesh(scene.createMesh(), ball);
    scene.updateLight(scene.createLight(), frontLight(1.5f, 4.0f));
    scene.commit();
}

// A cube of constant value: voxels [-n, n)^3 of size h.
VolumeGridPtr cubeGrid(float value, int n, double h) {
    nanovdb::build::Grid<float> g(0.0f, "field", nanovdb::GridClass::FogVolume);
    g.setTransform(h, nanovdb::Vec3d(0.0));
    g([&](const nanovdb::Coord &) { return value; }, nanovdb::CoordBBox(nanovdb::Coord(-n), nanovdb::Coord(n - 1)));
    const nanovdb::GridHandle<nanovdb::HostBuffer> handle = nanovdb::createNanoGrid(g);
    return VolumeGrid::copyOf(handle.data(), handle.size());
}

// A non-emissive volume material: scattering and absorption in proportion
// to the density field.
MaterialDesc fogMaterial(float scattering, float absorption) {
    using namespace shading;
    const auto field = [](uint16_t f) { return uint16_t(volume_field::kBase + f); };
    IrGraph graph;
    IrBuilder b(graph);
    const Ref d = b.primvar("density", 1, b.constant(0.0f));
    b.output(field(volume_field::scattering), 3, b.mul(d, b.constant(scattering, scattering, scattering)));
    b.output(field(volume_field::absorption), 3, b.mul(d, b.constant(absorption, absorption, absorption)));
    b.output(field(volume_field::emission), 3, b.constant(0.0f, 0.0f, 0.0f));
    b.output(field(volume_field::anisotropy), 1, b.constant(0.0f));
    MaterialDesc m;
    m.program = compile(graph).program;
    return m;
}

} // namespace

// Uniform and Corner primvars of points, curves and analytic spheres reach
// the device: their upload counts are the scene's (Scene::bindName), not
// the triangle counts (0 for them, which dropped the primvars and left the
// default material's 0.18 grey).
TEST_CASE("XPU uniform and corner primvars of points, curves and spheres", "[gpu][render][curves]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    // Points: one colour per point (Hydra's per-point primvars are Uniform).
    MeshDesc points;
    points.shape = MeshShape::Points;
    points.pointStyle = PointStyle::Sphere;
    points.points = {{-0.6f, 0.5f, 0.0f}, {0.0f, 0.5f, 0.0f}, {0.6f, 0.5f, 0.0f}};
    points.pointRadius = {0.25f, 0.25f, 0.25f};
    points.setAttribute({"displayColor", AttributeRate::Uniform, 3, {1, 0, 0, 0, 0, 1, 1, 0, 0}});
    scene.updateMesh(scene.createMesh(), points);
    // Curves with one colour per curve (per segment after prepareCurves).
    MeshDesc perCurve;
    perCurve.shape = MeshShape::Curves;
    perCurve.curveBasis = CurveBasis::Linear;
    for (int c = 0; c < 2; ++c) {
        for (int i = 0; i < 3; ++i)
            perCurve.points.push_back({-0.8f + 0.8f * float(i), -0.25f - 0.25f * float(c), 0.0f});
        perCurve.curveVertexCounts.push_back(3);
    }
    perCurve.curveRadius.assign(perCurve.points.size(), 0.08f);
    perCurve.setAttribute({"displayColor", AttributeRate::Uniform, 3, {1, 0, 0, 0, 0, 1}});
    REQUIRE(prepareCurves(perCurve));
    scene.updateMesh(scene.createMesh(), perCurve);
    // Curves with one colour per control point (Corner after prepareCurves).
    MeshDesc perVertex;
    perVertex.shape = MeshShape::Curves;
    perVertex.curveBasis = CurveBasis::Linear;
    for (int i = 0; i < 4; ++i)
        perVertex.points.push_back({-0.9f + 0.6f * float(i), -0.8f, 0.0f});
    perVertex.curveVertexCounts.push_back(4);
    perVertex.curveRadius.assign(perVertex.points.size(), 0.08f);
    perVertex.setAttribute({"displayColor", AttributeRate::Vertex, 3, {1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1}});
    REQUIRE(prepareCurves(perVertex));
    scene.updateMesh(scene.createMesh(), perVertex);
    // An analytic sphere: one primitive, one Uniform value.
    MeshDesc sphere;
    sphere.shape = MeshShape::Sphere;
    sphere.points = {{0.0f, 0.0f, 0.0f}};
    sphere.sphereRadius = {0.12f};
    sphere.transform = translation({0.0f, 0.05f, 0.0f});
    sphere.setAttribute({"displayColor", AttributeRate::Uniform, 3, {0, 0, 1}});
    scene.updateMesh(scene.createMesh(), sphere);
    scene.updateLight(scene.createLight(), frontLight(3.0f, 3.0f));
    scene.commit();

    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, 48, 48);
    const Render cpu = render(scene, camera, 48, 48, 16, RenderDevice::Cpu, 3);
    const Render gpu = render(scene, camera, 48, 48, 16, RenderDevice::Xpu, 3);
    REQUIRE(gpu.gpu);
    const Vec3f a = channelMeans(cpu.film), b = channelMeans(gpu.film);
    INFO("mean CPU (" << a.x << ", " << a.y << ", " << a.z << "), GPU (" << b.x << ", " << b.y << ", " << b.z << ")");
    REQUIRE(a.x > 0.01f);
    REQUIRE(a.z > 0.01f);
    CHECK_THAT(b.x, WithinAbs(a.x, 0.02 * a.x + 1e-5));
    CHECK_THAT(b.y, WithinAbs(a.y, 0.02 * a.y + 1e-5));
    CHECK_THAT(b.z, WithinAbs(a.z, 0.02 * a.z + 1e-5));
}

// The custom sphere intersector offers the exit hit when the any-hit filter
// ignores the entry (as Embree's sphere points): an analytic sphere whose
// front half is cut out shows its back half to the camera on both devices.
TEST_CASE("XPU cut-out analytic sphere shows its far side", "[gpu][render][opacity]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    using namespace shading;
    Scene scene;
    MeshDesc sphere;
    sphere.shape = MeshShape::Sphere;
    sphere.points = {{0.0f, 0.0f, 0.0f}};
    sphere.sphereRadius = {0.5f};
    const MeshId sphereId = scene.createMesh();
    scene.updateMesh(sphereId, sphere);
    // Opacity floor(1 - z) in object space: 0 on the half facing the camera
    // (z > 0), 1 on the far half.
    IrGraph g;
    g.model = SurfaceModel::StandardSurface;
    IrBuilder b(g);
    const Ref z = b.component(b.global(Global::Pobject), 2);
    b.output(surface_field::opacity, 3, b.unary(Op::Floor, b.sub(b.constant(1.0f), z)));
    MaterialDesc cutout;
    cutout.inputs = standardSurfaceDefaults();
    cutout.program = compile(g).program;
    REQUIRE(cutout.program);
    REQUIRE(cutout.program->opacity);
    const MaterialId cutoutId = scene.createMaterial();
    scene.updateMaterial(cutoutId, cutout);
    scene.setMeshMaterials(sphereId, {cutoutId});
    scene.updateLight(scene.createLight(), frontLight(1.0f, 4.0f));
    scene.commit();
    REQUIRE(scene.anyFiltered());

    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, 32, 32);
    const Render cpu = render(scene, camera, 32, 32, 4, RenderDevice::Cpu, 2);
    const Render gpu = render(scene, camera, 32, 32, 4, RenderDevice::Xpu, 2);
    REQUIRE(gpu.gpu);
    double cpuAlpha = 0.0, gpuAlpha = 0.0;
    int same = 0;
    for (size_t i = 0; i < cpu.film.alphaSum.size(); ++i) {
        cpuAlpha += cpu.film.alphaSum[i];
        gpuAlpha += gpu.film.alphaSum[i];
        same += std::fabs(cpu.film.alphaSum[i] - gpu.film.alphaSum[i]) <= 1e-4f;
    }
    const double pixels = double(cpu.film.alphaSum.size()) * 4.0;
    INFO("alpha mean CPU " << cpuAlpha / pixels << ", GPU " << gpuAlpha / pixels << "; pixels alike " << same);
    // The sphere covers about a fifth of the frame; the CPU sees its far side.
    REQUIRE(cpuAlpha / pixels > 0.1);
    CHECK_THAT(gpuAlpha, WithinAbs(cpuAlpha, 0.02 * cpuAlpha));
    CHECK(same >= int(cpu.film.alphaSum.size()) * 95 / 100);
}

// With subsurface walks and equiangular sampling a path queues three
// shadow rays in one depth; the LPE parts' per-ray arrays hold all of them
// (they held two per path, and the third ray's parts were written past
// them). "C.*L" then sums what the beauty sums, pixel by pixel.
TEST_CASE("XPU LPE parts with walks and equiangular samples", "[gpu][render][aov][lpe][sss][volume]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    namespace sf = surface_field;
    Scene scene;
    MaterialDesc skin;
    skin.inputs = standardSurfaceDefaults();
    skin.inputs.set(sf::subsurfaceWeight, 1.0f);
    skin.inputs.set(sf::subsurfaceColor, Spectrum{0.9f, 0.6f, 0.4f});
    skin.inputs.set(sf::subsurfaceRadius, Spectrum{1.0f, 0.4f, 0.2f});
    skin.inputs.set(sf::subsurfaceScale, 0.2f);
    const MaterialId skinId = scene.createMaterial();
    scene.updateMaterial(skinId, skin);
    MeshDesc ball = makeSphere(0.6f, 32, 16);
    const MeshId ballId = scene.createMesh();
    scene.updateMesh(ballId, ball);
    scene.setMeshMaterials(ballId, {skinId});
    // Fog around everything (every camera ray takes an equiangular sample).
    const MaterialId fog = scene.createMaterial();
    scene.updateMaterial(fog, fogMaterial(0.6f, 0.1f));
    VolumeDesc smoke;
    smoke.fields.push_back({"density", "density", cubeGrid(0.5f, 8, 1.0 / 6.0), Affine3f{}});
    smoke.material = fog;
    const VolumeId smokeId = scene.createVolume();
    scene.updateVolume(smokeId, smoke);
    scene.updateVolumeMotion(smokeId, {Mat4::identity()});
    LightDesc point;
    point.type = LightType::Sphere;
    point.radius = 0.1f;
    point.intensity = 20.0f;
    point.equiangular = true;
    point.transform = translation({0.6f, 0.6f, 0.9f});
    scene.updateLight(scene.createLight(), point);
    scene.commit();

    const auto expressions = std::make_shared<lpe::Compiled>(lpe::compile({"C.*L"}));
    REQUIRE(expressions->ok());
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, 32, 32);
    const Render gpu = render(scene, camera, 32, 32, 8, RenderDevice::Xpu, 4, expressions, 0.0f);
    REQUIRE(gpu.gpu);
    REQUIRE(gpu.film.lpeOutputs == 1);
    const Spectrum *lpe = gpu.film.lpe(0);
    int close = 0;
    for (size_t i = 0; i < gpu.film.colorSum.size(); ++i) {
        const float a = luminance(gpu.film.colorSum[i]), b = luminance(lpe[i]);
        close += std::fabs(a - b) <= 1e-3f * std::max(1.0f, std::fabs(a)) + 1e-6f;
    }
    const double beauty = luminanceSum(gpu.film.colorSum.data(), gpu.film.colorSum.size()),
                 sum = luminanceSum(lpe, gpu.film.colorSum.size());
    INFO("beauty " << beauty << ", C.*L " << sum << "; pixels alike " << close);
    REQUIRE(beauty > 0.0);
    CHECK_THAT(sum, WithinAbs(beauty, 1e-3 * beauty));
    CHECK(close >= int(gpu.film.colorSum.size()) * 99 / 100);
}

// One renderer through a smaller film with fewer LPE outputs (the device
// buffers keep their larger allocations) and a switch of programs with a
// beauty-only pass between: the LPE sums still come back, and they are the
// new program's (the device tables follow the renderer's rebuilds).
TEST_CASE("XPU LPE read-back after the film shrinks and the program changes", "[gpu][render][aov][lpe]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    simpleScene(scene);
    const auto first = std::make_shared<lpe::Compiled>(lpe::compile({"C.*L", "C<RD>L"}));
    const auto second = std::make_shared<lpe::Compiled>(lpe::compile({"C<RD>.*L"}));
    REQUIRE(first->ok());
    REQUIRE(second->ok());
    const Camera big = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, 32, 32);
    const Camera small = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, 16, 16);

    Renderer renderer;
    RenderSettings settings;
    settings.device = RenderDevice::Xpu;
    settings.maxSamples = 4;
    settings.maxDepth = 4;
    settings.lpe = first;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(big);
    renderer.resize(32, 32);
    bool onGpu = true;
    while (!renderer.converged()) {
        renderer.renderPass({});
        onGpu = onGpu && renderer.lastPassOnGpu();
    }
    REQUIRE(renderer.film().lpeOutputs == 2);
    settings.lpe = nullptr; // the beauty only: the first program goes
    renderer.setSettings(settings);
    renderer.reset();
    renderer.renderPass({});
    onGpu = onGpu && renderer.lastPassOnGpu();
    settings.lpe = second;
    renderer.setSettings(settings);
    renderer.setCamera(small);
    renderer.resize(16, 16);
    renderer.reset();
    while (!renderer.converged()) {
        renderer.renderPass({});
        onGpu = onGpu && renderer.lastPassOnGpu();
    }
    REQUIRE(onGpu);
    const Film reused = renderer.film();
    const Render fresh = render(scene, small, 16, 16, 4, RenderDevice::Xpu, 4, second);
    REQUIRE(fresh.gpu);
    REQUIRE(reused.lpeOutputs == 1);
    REQUIRE(fresh.film.lpeOutputs == 1);
    const size_t n = size_t(16 * 16);
    const double a = luminanceSum(fresh.film.lpe(0), n), b = luminanceSum(reused.lpe(0), n);
    INFO("C<RD>.*L fresh " << a << ", reused " << b);
    REQUIRE(a > 0.0);
    CHECK_THAT(b, WithinAbs(a, 1e-4 * a));
}

// A render region that leaves no tiles: GPU list passes (IPR proxy passes)
// render nothing, not the previous region's selected pixels.
TEST_CASE("XPU list passes over an empty region", "[gpu][render][adaptive]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    Scene scene;
    simpleScene(scene);
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, 32, 32);
    Renderer renderer;
    RenderSettings settings;
    settings.device = RenderDevice::Xpu;
    settings.maxSamples = 8;
    settings.maxDepth = 3;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(camera);
    renderer.setProxyDepth(2);
    renderer.resize(32, 32);
    renderer.reset();
    renderer.renderPass({}); // a proxy pass: one pixel per 4 x 4 block
    REQUIRE(renderer.lastPassOnGpu());
    renderer.setRegion(TileRect{});
    renderer.reset();
    for (int pass = 0; pass < 3; ++pass)
        renderer.renderPass({});
    const Film &film = renderer.film();
    uint64_t samples = 0;
    for (uint32_t c : film.count)
        samples += c;
    CHECK(samples == 0);
}

// A texture whose data the host replaced under its id (Hydra prepares a file
// that changed on disk again; the scene's version stays) reaches the device
// on the next pass: XPU kept the first upload until it was recreated.
TEST_CASE("XPU refreshes a texture replaced under its id", "[gpu][render][texture]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    using namespace shading;
    Scene scene;
    MeshDesc quad = makeQuad(1.0f);
    quad.setAttribute({"st", AttributeRate::Vertex, 2, {0, 0, 1, 0, 1, 1, 0, 1}});
    const MeshId mesh = scene.createMesh();
    scene.updateMesh(mesh, quad);
    MemorySampler textures;
    const TextureId tex = textures.add(1, 1, 3, {1.0f, 0.0f, 0.0f});
    // An unlit quad showing the texture at its (varying) st.
    IrGraph g;
    g.model = SurfaceModel::Unlit;
    IrBuilder b(g);
    const uint32_t slot = b.textureSlot(TexSlot{});
    b.output(surface_field::emissionColor, 3, b.tex2D(slot, b.primvar("st", 2, b.constant(0.0f)), b.constant(0.0f)));
    MaterialDesc material;
    material.inputs = unlitDefaults();
    material.program = compile(g).program;
    REQUIRE(material.program);
    material.textures = {tex};
    const MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, material);
    scene.setMeshMaterials(mesh, {m});
    scene.setTextureSampler(&textures);
    scene.commit();

    Renderer renderer;
    RenderSettings settings;
    settings.device = RenderDevice::Xpu;
    settings.maxSamples = 1;
    settings.maxDepth = 1;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 0.5f, 8, 8)); // inside the quad
    renderer.resize(8, 8);
    bool onGpu = true;
    while (!renderer.converged()) {
        renderer.renderPass({});
        onGpu = onGpu && renderer.lastPassOnGpu();
    }
    const Vec3f first = channelMeans(renderer.film());
    textures.replace(tex, 1, 1, 3, {0.0f, 1.0f, 0.0f});
    renderer.reset();
    while (!renderer.converged()) {
        renderer.renderPass({});
        onGpu = onGpu && renderer.lastPassOnGpu();
    }
    const Vec3f second = channelMeans(renderer.film());
    REQUIRE(onGpu);
    INFO("first " << first.x << " " << first.y << ", after the replacement " << second.x << " " << second.y);
    CHECK_THAT(first.x, WithinAbs(1.0, 0.01));
    CHECK_THAT(first.y, WithinAbs(0.0, 0.01));
    CHECK_THAT(second.x, WithinAbs(0.0, 0.01));
    CHECK_THAT(second.y, WithinAbs(1.0, 0.01));
}
