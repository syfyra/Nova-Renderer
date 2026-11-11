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
#include "core/gpu/gpu_renderer.h"
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
#include <cstring>
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

namespace {

// Texture budget tests (R1-48): level l of a size^2 RGB float texture is the
// constant color (l / 10, 1 - l / 10, 0.5).
std::vector<float> levelTexels(int size, uint32_t l) {
    std::vector<float> texels;
    texels.reserve(size_t(size) * size_t(size) * 3);
    for (int i = 0; i < size * size; ++i)
        texels.insert(texels.end(), {float(l) / 10.0f, 1.0f - float(l) / 10.0f, 0.5f});
    return texels;
}

// Levels `from` + 1 on of a size^2 chain.
std::vector<std::vector<float>> chainBelow(int size, uint32_t from) {
    std::vector<std::vector<float>> mips;
    for (uint32_t l = from + 1; (size >> l) >= 1; ++l)
        mips.push_back(levelTexels(size >> l, l));
    return mips;
}

// An unlit program showing texture slot 0 at st; with `rows`, also the sum of
// that many primvars (all live at once: a program over the GPU's rows).
shading::ProgramPtr texturedUnlit(int rows = 0) {
    using namespace shading;
    IrGraph g;
    g.model = SurfaceModel::Unlit;
    IrBuilder b(g);
    const uint32_t slot = b.textureSlot(TexSlot{});
    Ref color = b.tex2D(slot, b.primvar("st", 2, b.constant(0.0f)), b.constant(0.0f));
    if (rows > 0) {
        Ref sum = b.constant(1.0f);
        for (int i = 0; i < rows; ++i)
            sum = b.add(sum, b.primvar("pv" + std::to_string(i), 1, b.constant(0.0f)));
        color = b.mul(color, b.convert(sum, 3));
    }
    b.output(surface_field::emissionColor, 3, color);
    return compile(g).program;
}

MaterialId addMaterial(Scene &scene, TextureId tex, shading::ProgramPtr program,
                       SurfaceModel model = SurfaceModel::Unlit) {
    MaterialDesc material;
    material.inputs = unlitDefaults();
    material.inputs.model = model;
    material.program = std::move(program);
    material.textures = {tex};
    const MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, material);
    return m;
}

// An unlit quad over [-1, 1]^2 showing `tex` at its st (materials added
// before it come first in the plan's order).
void texturedQuad(Scene &scene, const MemorySampler &textures, TextureId tex) {
    MeshDesc quad = makeQuad(1.0f);
    quad.setAttribute({"st", AttributeRate::Vertex, 2, {0, 0, 1, 0, 1, 1, 0, 1}});
    const MeshId mesh = scene.createMesh();
    scene.updateMesh(mesh, quad);
    scene.setMeshMaterials(mesh, {addMaterial(scene, tex, texturedUnlit())});
    scene.setTextureSampler(&textures);
    scene.commit();
}

// The quad from the camera, 4 texels of a 1024^2 texture per pixel (level 0
// is sampled when everything fits), with the budget `budgetMb`.
Film renderBudget(Renderer &renderer, const Scene &scene, uint32_t budgetMb, bool &onGpu) {
    RenderSettings settings;
    settings.device = RenderDevice::Xpu;
    settings.maxSamples = 4;
    settings.maxDepth = 1;
    settings.gpuTextureBudget = budgetMb;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 0.5f, 128, 128));
    renderer.resize(128, 128);
    renderer.reset();
    while (!renderer.converged()) {
        renderer.renderPass({});
        onGpu = onGpu && renderer.lastPassOnGpu();
    }
    return renderer.film();
}

Film renderBudget(const Scene &scene, uint32_t budgetMb, bool &onGpu) {
    Renderer renderer;
    return renderBudget(renderer, scene, budgetMb, onGpu);
}

bool sameFilm(const Film &a, const Film &b) {
    return a.colorSum.size() == b.colorSum.size() &&
           std::memcmp(a.colorSum.data(), b.colorSum.data(), a.colorSum.size() * sizeof(a.colorSum[0])) == 0 &&
           a.count == b.count;
}

} // namespace

// R1-48: the texture budget is planned from the textures' shapes before any
// texel is read, and only the kept levels are read and uploaded. A texture
// over the budget renders bit for bit as the same texture holding only the
// kept levels: levels 2 on of a 1024^2 float chain under 2 MB, which the
// per-texture policy before the change kept as well.
TEST_CASE("XPU keeps the texture levels its budget plans", "[gpu][render][texture]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    MemorySampler textures;
    const TextureId full = textures.add(1024, 1024, 3, levelTexels(1024, 0), chainBelow(1024, 0));
    const TextureId from2 = textures.add(256, 256, 3, levelTexels(256, 2), chainBelow(1024, 2));
    Scene a, b, c;
    texturedQuad(a, textures, full);
    texturedQuad(b, textures, from2);
    texturedQuad(c, textures, full);
    bool onGpu = true;
    const uint64_t before = textures.texelsRead();
    const Film budgeted = renderBudget(a, 2, onGpu);
    const uint64_t read = textures.texelsRead() - before;
    const Film kept = renderBudget(b, 0, onGpu);
    const Film everything = renderBudget(c, 0, onGpu);
    REQUIRE(onGpu);
    // Only levels 2 to 10 of the full chain were read.
    CHECK(read == 87381);
    CHECK(sameFilm(budgeted, kept));
    const Vec3f limited = channelMeans(budgeted), all = channelMeans(everything);
    INFO("budgeted " << limited.x << " " << limited.y << ", everything " << all.x << " " << all.y);
    CHECK_THAT(limited.x, WithinAbs(0.2, 0.01)); // level 2
    CHECK_THAT(all.x, WithinAbs(0.0, 0.01));     // level 0
}

// The plan counts exactly the textures the device samples: not those of a
// VEX stand-in or of a program over the GPU's rows (which falls back to its
// uniform inputs), which are not uploaded either. Counted first, a 2048^2
// texture of theirs would have left the quad's texture only levels 3 on.
TEST_CASE("XPU plans only the textures it uploads", "[gpu][render][texture]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    MemorySampler textures;
    const TextureId full = textures.add(1024, 1024, 3, levelTexels(1024, 0), chainBelow(1024, 0));
    const TextureId from2 = textures.add(256, 256, 3, levelTexels(256, 2), chainBelow(1024, 2));
    const TextureId other = textures.add(2048, 2048, 3, levelTexels(2048, 0), chainBelow(2048, 0));
    const shading::ProgramPtr large = texturedUnlit(int(shading::kGpuRowLimit) + 8);
    REQUIRE(large);
    REQUIRE(large->rowCount > shading::kGpuRowLimit);
    Scene scene, kept;
    addMaterial(scene, other, texturedUnlit(), SurfaceModel::External); // a VEX stand-in
    addMaterial(scene, other, large);
    texturedQuad(scene, textures, full);
    texturedQuad(kept, textures, from2);
    bool onGpu = true;
    const uint64_t before = textures.texelsRead();
    const Film budgeted = renderBudget(scene, 2, onGpu);
    const uint64_t read = textures.texelsRead() - before;
    const Film reference = renderBudget(kept, 0, onGpu);
    REQUIRE(onGpu);
    CHECK(read == 87381); // levels 2 to 10 of the quad's texture, nothing of the other
    CHECK(sameFilm(budgeted, reference));
}

// The default budget is measured when the first texture loads, not at the
// first scene without textures; a changed budget applies at the next pass of
// the same scene (IPR), reading again only the textures whose kept levels
// change.
TEST_CASE("XPU applies texture budget changes without a scene edit", "[gpu][render][texture]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    MemorySampler textures;
    const TextureId full = textures.add(1024, 1024, 3, levelTexels(1024, 0), chainBelow(1024, 0));
    const TextureId from2 = textures.add(256, 256, 3, levelTexels(256, 2), chainBelow(1024, 2));
    const TextureId small = textures.add(64, 64, 3, levelTexels(64, 0), chainBelow(64, 0));

    {
        gpu::GpuRenderer device;
        std::string error;
        REQUIRE(device.init(error));
        Scene plain;
        MeshDesc quad = makeQuad(1.0f);
        const MeshId mesh = plain.createMesh();
        plain.updateMesh(mesh, quad);
        plain.setTextureSampler(&textures);
        plain.commit();
        REQUIRE(device.setScene(plain));
        CHECK(device.textureBudgetBytes() == 0); // no texture: not measured
        Scene textured;
        texturedQuad(textured, textures, full);
        REQUIRE(device.setScene(textured));
        CHECK(device.textureBudgetBytes() > 0);
        device.setTextureBudget(5);
        CHECK(device.textureBudgetBytes() == size_t(5) << 20);
        device.setTextureBudget(0);
        CHECK(device.textureBudgetBytes() == 0); // measured again when textures load next
    }

    // The quad's texture and a small one of another material (unused by the
    // camera, under every cap here, so never read again).
    Scene scene, kept;
    texturedQuad(scene, textures, full);
    addMaterial(scene, small, texturedUnlit());
    scene.commit();
    texturedQuad(kept, textures, from2);
    bool onGpu = true;
    Renderer renderer;
    uint64_t before = textures.texelsRead();
    const Film everything = renderBudget(renderer, scene, 0, onGpu);
    CHECK(textures.texelsRead() - before == 1398101 + 5461);
    before = textures.texelsRead();
    const Film budgeted = renderBudget(renderer, scene, 2, onGpu); // no scene edit
    CHECK(textures.texelsRead() - before == 87381);                 // the quad's texture, levels 2 on
    before = textures.texelsRead();
    const Film again = renderBudget(renderer, scene, 0, onGpu);
    CHECK(textures.texelsRead() - before == 1398101);
    before = textures.texelsRead();
    const Film same = renderBudget(renderer, scene, 1000, onGpu); // everything still fits: nothing read
    CHECK(textures.texelsRead() - before == 0);
    const Film reference = renderBudget(kept, 0, onGpu);
    REQUIRE(onGpu);
    CHECK(sameFilm(budgeted, reference));
    CHECK(sameFilm(again, everything));
    CHECK(sameFilm(same, everything));
    CHECK(!sameFilm(budgeted, everything));
}

// The device (docs/design/M10.md, section 8): its description for messages,
// NOVA_XPU_DEVICE (an index in the driver's list; one it lacks is refused
// with the variable's name, so Nova XPU falls back to the CPU and says why),
// and NOVA_XPU_MEMORY_MB (the memory to plan with, as on a smaller card).
TEST_CASE("XPU device: description, selection, memory to plan with", "[gpu][device]") {
    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    const auto set = [](const char *name, const char *value) {
#if defined(_WIN32)
        _putenv_s(name, value);
#else
        if (*value)
            setenv(name, value, 1);
        else
            unsetenv(name);
#endif
    };
    {
        gpu::Device device;
        REQUIRE(device.init(why));
        INFO(device.description());
        CHECK(device.computeCapability() >= 75);
        CHECK(device.processors() > 0);
        CHECK(device.description().rfind(device.name(), 0) == 0);
        CHECK(device.description().find("compute capability " + std::to_string(device.computeCapability() / 10) + "." +
                                        std::to_string(device.computeCapability() % 10)) != std::string::npos);
        CHECK(device.totalMemory() > 0);
        CHECK(device.freeMemory() > 0);
        CHECK(device.freeMemory() <= device.totalMemory());
    }
    for (const char *index : {"99", "-1", "first", "0x0"}) {
        INFO("NOVA_XPU_DEVICE=" << index);
        set("NOVA_XPU_DEVICE", index);
        gpu::Device device;
        std::string error;
        CHECK(!device.init(error));
        CHECK(error.find("NOVA_XPU_DEVICE") != std::string::npos);
        set("NOVA_XPU_DEVICE", "");
    }
    {
        set("NOVA_XPU_DEVICE", "0");
        gpu::Device device;
        std::string error;
        CHECK(device.init(error));
        set("NOVA_XPU_DEVICE", "");
    }
    {
        set("NOVA_XPU_MEMORY_MB", "64");
        gpu::Device device;
        std::string error;
        REQUIRE(device.init(error));
        CHECK(device.freeMemory() <= (size_t(64) << 20));
        // What is allocated since comes off the cap.
        gpu::DeviceBuffer buffer;
        REQUIRE(buffer.reserve(device, size_t(48) << 20));
        CHECK(device.freeMemory() <= (size_t(16) << 20));
        set("NOVA_XPU_MEMORY_MB", "");
    }
    {
        set("NOVA_XPU_MEMORY_MB", "many"); // not a number: no cap
        gpu::Device device;
        std::string error;
        REQUIRE(device.init(error));
        set("NOVA_XPU_MEMORY_MB", "");
        CHECK(device.freeMemory() > (size_t(64) << 20));
    }
}

// GPUs of an architecture Nova XPU is built for but has not run on (every
// one before the RTX 50 series: docs/design/M10.md, section 8) render on the
// GPU only on request, NOVA_XPU_EXPERIMENTAL_GPUS, which the installer's
// option sets. The device here reports another compute capability
// (Device::setCapabilityOverride), so those branches run on this machine.
TEST_CASE("XPU on an untested GPU architecture is experimental", "[gpu][device][experimental]") {
    using Support = gpu::Device::Support;
    // The rule with the release's lists (built for 7.5, 8.0 and 12.0, tested on 12.0).
    if (gpu::Device::support(75) == Support::TooOld) {
        WARN("a development build (NOVA_GPU_ARCHS without 75): the release's rule is not checked");
    } else {
        CHECK(gpu::Device::support(61) == Support::TooOld); // GTX 10 series
        CHECK(gpu::Device::support(70) == Support::TooOld);
        CHECK(gpu::Device::support(75) == Support::Experimental); // RTX 20, GTX 16
        CHECK(gpu::Device::support(86) == Support::Experimental); // RTX 30
        CHECK(gpu::Device::support(89) == Support::Experimental); // RTX 40
        CHECK(gpu::Device::support(90) == Support::Experimental); // no code: the driver compiles
        CHECK(gpu::Device::support(100) == Support::Experimental);
        CHECK(gpu::Device::support(120) == Support::Tested); // RTX 50
        CHECK(gpu::Device::support(121) == Support::Tested);
        CHECK(gpu::Device::support(130) == Support::Experimental);
    }

    std::string why;
    if (!gpuAvailable(why))
        SKIP("no GPU: " << why);
    const auto request = [](const char *value) {
#if defined(_WIN32)
        _putenv_s("NOVA_XPU_EXPERIMENTAL_GPUS", value);
#else
        if (*value)
            setenv("NOVA_XPU_EXPERIMENTAL_GPUS", value, 1);
        else
            unsetenv("NOVA_XPU_EXPERIMENTAL_GPUS");
#endif
    };
    struct Restore {
        ~Restore() {
            gpu::Device::setCapabilityOverride(0);
#if defined(_WIN32)
            _putenv_s("NOVA_XPU_EXPERIMENTAL_GPUS", "");
#else
            unsetenv("NOVA_XPU_EXPERIMENTAL_GPUS");
#endif
        }
    } restore;
    Scene scene;
    simpleScene(scene);
    const Camera camera = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, 32, 32);

    // As an RTX 30 (8.6) without the request: refused with the way to turn
    // it on, and Nova XPU renders on the CPU.
    request("");
    gpu::Device::setCapabilityOverride(86);
    for (const char *off : {"", "0"}) {
        request(off);
        gpu::Device device;
        std::string error;
        CHECK(!device.init(error));
        INFO(error);
        CHECK(error.find("compute capability 8.6") != std::string::npos);
        CHECK(error.find("has not been tested") != std::string::npos);
        CHECK(error.find("NOVA_XPU_EXPERIMENTAL_GPUS=1") != std::string::npos);
        CHECK(error.find("installer") != std::string::npos);
    }
    request("");
    const Render refused = render(scene, camera, 32, 32, 4, RenderDevice::Xpu, 3);
    CHECK(!refused.gpu);

    // With the request it renders on the GPU and says that it is experimental.
    request("1");
    {
        gpu::Device device;
        std::string error;
        REQUIRE(device.init(error));
        CHECK(device.experimental());
        CHECK(device.computeCapability() == 86);
    }
    const Render tried = render(scene, camera, 32, 32, 4, RenderDevice::Xpu, 3);
    CHECK(tried.gpu);
    CHECK(tried.film.count == refused.film.count);

    // As a GTX 10 series card (6.1): too old, request or not.
    gpu::Device::setCapabilityOverride(61);
    {
        gpu::Device device;
        std::string error;
        CHECK(!device.init(error));
        INFO(error);
        CHECK(error.find("compute capability 6.1") != std::string::npos);
        CHECK(error.find("older than Nova XPU supports") != std::string::npos);
    }

    // The device as it is: experimental only if its own architecture is.
    gpu::Device::setCapabilityOverride(0);
    {
        gpu::Device device;
        std::string error;
        REQUIRE(device.init(error));
        CHECK(device.experimental() == (gpu::Device::support(device.computeCapability()) == Support::Experimental));
    }
}
