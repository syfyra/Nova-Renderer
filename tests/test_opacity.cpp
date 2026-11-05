// Opacity (docs/design/M3.md M3.11, K10): surfaces with opacity are
// filtered in the accelerator. A hit counts with the probability of its
// opacity, decided per (ray seed, mesh, prim, instance), in camera, indirect,
// shadow and first-hit rays. Opacity never reaches shading; alpha is the
// fraction of camera samples that hit something.

#include "test_helpers.h"

#include "core/scene/scene.h"
#include "core/shading/compiler.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using namespace nova;
using namespace nova::test;
namespace sh = nova::shading;
using Catch::Matchers::WithinAbs;

namespace {

MaterialDesc unlit(float opacity) {
    MaterialDesc d;
    d.inputs = unlitDefaults();
    d.inputs.set(surface_field::opacity, spectrum(opacity));
    return d;
}

struct Frame2D {
    std::vector<Spectrum> color;
    std::vector<float> alpha;
    int size = 0;
};

Frame2D render(Scene &scene, int size, uint32_t spp, uint32_t maxDepth = 4, float halfWidth = 1.0f,
               float frame = 1.0f) {
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = spp;
    settings.maxDepth = maxDepth;
    settings.frame = frame;
    settings.pixelFilterScale = 0.0f;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, halfWidth, size, size));
    renderer.resize(size, size);
    renderToConvergence(renderer);
    Frame2D img;
    img.size = size;
    const Film &film = renderer.film();
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            img.color.push_back(pixelMean(film, x, y));
            const size_t i = size_t(y) * size_t(film.width) + size_t(x);
            img.alpha.push_back(film.alphaSum[i] / float(film.samples));
        }
    return img;
}

} // namespace

TEST_CASE("A half-transparent sphere: color and alpha 0.75", "[opacity]") {
    // Unlit white, opacity 0.5: the front surface counts half the time, the
    // back one half of the rest (Karma: 0.7501).
    Scene scene;
    const MeshId sphere = scene.createMesh();
    scene.updateMesh(sphere, makeSphere(1.0f, 64, 32));
    const MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, unlit(0.5f));
    scene.setMeshMaterials(sphere, {m});
    scene.commit();
    const Frame2D img = render(scene, 16, 256);
    double color = 0.0, alpha = 0.0;
    int n = 0;
    for (int y = 4; y < 12; ++y)
        for (int x = 4; x < 12; ++x) {
            color += img.color[size_t(y) * 16 + size_t(x)].y;
            alpha += img.alpha[size_t(y) * 16 + size_t(x)];
            ++n;
        }
    CHECK_THAT(color / n, WithinAbs(0.75, 3e-3));
    CHECK_THAT(alpha / n, WithinAbs(0.75, 3e-3));
}

TEST_CASE("A checker cut-out covers exactly its opaque cells", "[opacity]") {
    // Opacity = checker(st) per hit (an opacity program): pixel centers see
    // the cells exactly; the cut-out cells show the white backdrop.
    Scene scene;
    MeshDesc quad = makeQuad(1.0f);
    quad.setAttribute({"st", AttributeRate::Vertex, 2, {0, 0, 1, 0, 1, 1, 0, 1}});
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, quad);
    sh::IrGraph g;
    g.model = SurfaceModel::Unlit;
    sh::IrBuilder b(g);
    const sh::Ref st = b.primvar("st", 2, b.constant(0.0f));
    const sh::Ref cells = b.unary(sh::Op::Floor, b.mul(st, b.constant(4.0f)));
    const sh::Ref checker = b.binary(sh::Op::Mod, b.add(b.component(cells, 0), b.component(cells, 1)), b.constant(2.0f));
    b.output(surface_field::opacity, 3, checker);
    b.output(surface_field::emissionColor, 3, b.constant(0.25f));
    MaterialDesc material;
    material.inputs = unlitDefaults();
    material.program = sh::compile(g).program;
    REQUIRE(material.program);
    REQUIRE(material.program->opacity);
    const MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, material);
    scene.setMeshMaterials(id, {m});
    const MeshId back = scene.createMesh();
    MeshDesc backdrop = makeQuad(3.0f);
    backdrop.transform = translation({0.0f, 0.0f, -1.0f});
    scene.updateMesh(back, backdrop);
    const MaterialId white = scene.createMaterial();
    scene.updateMaterial(white, unlit(1.0f));
    scene.setMeshMaterials(back, {white});
    scene.commit();
    const int size = 32;
    const Frame2D img = render(scene, size, 4);
    int wrong = 0;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const float s = (float(x) + 0.5f) / float(size), t = (float(y) + 0.5f) / float(size);
            const bool opaque = std::fmod(std::floor(4.0f * s) + std::floor(4.0f * t), 2.0f) > 0.5f;
            wrong += std::fabs(img.color[size_t(y) * size + size_t(x)].y - (opaque ? 0.25f : 1.0f)) > 1e-5f;
        }
    CHECK(wrong == 0);
}

TEST_CASE("Opacity programs see the render's frame", "[opacity]") {
    // Opacity 1 up to frame 5, then 0 (a frame-driven dissolve): the hit
    // filter runs the program at the frame the shading uses (and the GPU's
    // any-hit program), so at frame 10 the quad is cut out of every pixel.
    Scene scene;
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, makeQuad(1.0f));
    sh::IrGraph g;
    g.model = SurfaceModel::Unlit;
    sh::IrBuilder b(g);
    const sh::Ref early = b.binary(sh::Op::GreaterEq, b.constant(5.0f), b.global(sh::Global::Frame));
    b.output(surface_field::opacity, 3, early);
    b.output(surface_field::emissionColor, 3, b.constant(0.25f));
    MaterialDesc material;
    material.inputs = unlitDefaults();
    material.program = sh::compile(g).program;
    REQUIRE(material.program);
    REQUIRE(material.program->opacity);
    const MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, material);
    scene.setMeshMaterials(id, {m});
    const MeshId back = scene.createMesh();
    MeshDesc backdrop = makeQuad(3.0f);
    backdrop.transform = translation({0.0f, 0.0f, -1.0f});
    scene.updateMesh(back, backdrop);
    const MaterialId white = scene.createMaterial();
    scene.updateMaterial(white, unlit(1.0f));
    scene.setMeshMaterials(back, {white});
    scene.commit();
    for (const float frame : {1.0f, 10.0f}) {
        INFO("frame " << frame);
        const Frame2D img = render(scene, 8, 1, 4, 0.5f, frame);
        const float expected = frame > 5.0f ? 1.0f : 0.25f;
        int wrong = 0;
        for (const Spectrum &c : img.color)
            wrong += std::fabs(c.y - expected) > 1e-5f;
        CHECK(wrong == 0);
    }
}

TEST_CASE("A half-transparent plane casts half a shadow", "[opacity]") {
    // Ground z = 0 under a distant light travelling along (-1, 0, -1): a
    // plane of opacity 0.5 at z = 1, x in [2, 3] (outside the view) shades
    // the ground at x in [1, 2] by half.
    Scene scene;
    const MeshId ground = scene.createMesh();
    scene.updateMesh(ground, makeQuad(4.0f));
    const MeshId blocker = scene.createMesh();
    MeshDesc plane;
    plane.points = {{2.0f, -4.0f, 1.0f}, {3.0f, -4.0f, 1.0f}, {3.0f, 4.0f, 1.0f}, {2.0f, 4.0f, 1.0f}};
    plane.indices = {0, 1, 2, 0, 2, 3};
    scene.updateMesh(blocker, plane);
    MaterialDesc half;
    half.inputs = lambertDefaults(spectrum(0.5f));
    half.inputs.set(surface_field::opacity, spectrum(0.5f));
    const MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, half);
    scene.setMeshMaterials(blocker, {m});
    LightDesc sun;
    sun.type = LightType::Distant;
    sun.angleDeg = 0.0f;
    const double c = std::sqrt(0.5), sn = std::sqrt(0.5); // 45 degrees about +Y: -Z goes to (-1, 0, -1) / sqrt 2
    sun.transform.m[0][0] = c;
    sun.transform.m[0][2] = -sn;
    sun.transform.m[2][0] = sn;
    sun.transform.m[2][2] = c;
    scene.updateLight(scene.createLight(), sun);
    scene.commit();
    const int size = 16; // x from -2 to 2: 4 pixels per unit
    const Frame2D img = render(scene, size, 1024, 1, 2.0f);
    double lit = 0.0, shaded = 0.0;
    int nl = 0, ns = 0;
    for (int y = 2; y < 14; ++y)
        for (int x = 0; x < size; ++x) {
            const float wx = (float(x) + 0.5f) / 4.0f - 2.0f;
            const double v = img.color[size_t(y) * size + size_t(x)].y;
            if (wx > 1.1f && wx < 1.9f) {
                shaded += v;
                ++ns;
            } else if (wx < 0.9f) {
                lit += v;
                ++nl;
            }
        }
    CHECK_THAT((shaded / ns) / (lit / nl), WithinAbs(0.5, 0.01));
}

TEST_CASE("Filter decisions repeat for the same ray", "[opacity]") {
    Scene scene;
    const MeshId sphere = scene.createMesh();
    scene.updateMesh(sphere, makeSphere(1.0f, 32, 16));
    const MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, unlit(0.4f));
    scene.setMeshMaterials(sphere, {m});
    scene.commit();
    const int n = 512;
    std::vector<Vec3f> org(n), dir(n, Vec3f{0.0f, 0.0f, -1.0f});
    std::vector<float> tMax(n, kInf), time(n, 0.0f), seed(n);
    for (int i = 0; i < n; ++i) {
        org[size_t(i)] = {float(i % 16) / 8.0f - 1.0f, float(i / 16) / 16.0f - 1.0f, 5.0f};
        seed[size_t(i)] = float(i) * 0.6180339f - std::floor(float(i) * 0.6180339f);
    }
    const RayBatch rays{org.data(), dir.data(), tMax.data(), uint32_t(n), time.data(), seed.data()};
    auto trace = [&] {
        std::vector<float> t(n);
        std::vector<uint32_t> mesh(n), prim(n), inst(n);
        std::vector<Vec2f> uv(n);
        HitBatch hits{t.data(), mesh.data(), prim.data(), uv.data(), inst.data()};
        scene.accel().intersect(rays, hits);
        return std::make_pair(mesh, t);
    };
    const auto a = trace(), b2 = trace();
    CHECK(a.first == b2.first);
    CHECK(a.second == b2.second);
    int hits = 0;
    for (uint32_t id : a.first)
        hits += id != kInvalidId;
    CHECK(hits > 0);
    CHECK(hits < n);
}
