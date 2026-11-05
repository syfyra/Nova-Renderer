// Texture footprints (docs/design/M3.md M3.9, K7, K8): camera cones, the
// footprint ellipse, uv gradients and curvature of hits, and derivatives by
// shifted re-evaluation through the whole shade stage.

#include "test_helpers.h"

#include "core/scene/scene.h"
#include "core/shading/compiler.h"
#include "core/texture/ray_cone.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using namespace nova;
using namespace nova::test;
namespace sh = nova::shading;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("Camera cones", "[texture][cones]") {
    // Pinhole (focal 5, aperture 3.6 x 1.8, 256 x 128): one pixel at the
    // center spans atan(2 tan(fovY / 2) / H).
    const Camera persp = makePerspectiveCamera({0.0f, 0.0f, 0.0f}, 5.0, 3.6, 256, 128);
    float width = 0.0f, spread = 0.0f;
    cameraCone(persp, {128.0f, 64.0f}, width, spread);
    CHECK_THAT(spread, WithinRel(std::atan(2.0 * 0.18 / 128.0), 1e-3));
    CHECK(width >= 0.0f);
    // Orthographic (half width 2, 40 pixels): the pixel size, no spread.
    const Camera ortho = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 2.0f, 40, 40);
    cameraCone(ortho, {20.0f, 20.0f}, width, spread);
    CHECK_THAT(width, WithinRel(0.1, 1e-4));
    CHECK_THAT(spread, WithinAbs(0.0, 1e-7));
}

TEST_CASE("The footprint ellipse of a cone", "[texture][cones]") {
    const Vec3f f{0.0f, 0.0f, 1.0f};
    const float w = 0.3f;
    for (float deg : {0.0f, 60.0f, 85.0f}) {
        const float a = deg * kPi / 180.0f;
        const Vec3f d{std::sin(a), 0.0f, -std::cos(a)};
        Vec3f ax, ay;
        coneFootprint(d, f, w, ax, ay);
        INFO("angle " << deg);
        CHECK_THAT(double(length(ax)), WithinRel(double(w / std::cos(a)), 1e-5));
        CHECK_THAT(double(length(ay)), WithinRel(double(w), 1e-5));
        CHECK_THAT(dot(ax, ay), WithinAbs(0.0, 1e-6));
        CHECK_THAT(dot(ax, f), WithinAbs(0.0, 1e-6));
        CHECK_THAT(dot(ay, f), WithinAbs(0.0, 1e-6));
    }
    // Scattering: a convex mirror (curvature 1/R) at normal incidence widens
    // by 2 W / R; a rough lobe to at least its roughness.
    CHECK_THAT(scatterSpread(0.01f, 0.2f, 0.5f, -1.0f, 0.0f), WithinAbs(0.01 + 2.0 * 0.5 * 0.2, 1e-6));
    CHECK_THAT(scatterSpread(0.01f, 0.2f, 0.0f, -1.0f, 0.3f), WithinAbs(0.3, 1e-7));
}

TEST_CASE("Hits carry uv gradients and curvature", "[texture][cones]") {
    Scene scene;
    const MeshId quad = scene.createMesh();
    MeshDesc q = makeQuad(1.0f);
    q.transform = translation({0.0f, 0.0f, 0.0f});
    scene.updateMesh(quad, q);
    const MeshId ball = scene.createMesh();
    MeshDesc s;
    s.shape = MeshShape::Sphere;
    s.points = {{0.0f, 0.0f, 0.0f}};
    s.sphereRadius = {0.5f};
    s.transform = translation({3.0f, 0.0f, 0.0f});
    scene.updateMesh(ball, s);
    const MeshId tess = scene.createMesh();
    MeshDesc t = makeSphere(0.5f, 64, 32);
    t.transform = translation({-3.0f, 0.0f, 0.0f});
    scene.updateMesh(tess, t);
    scene.commit();

    auto hitAt = [&](Vec3f org) {
        SurfaceHit h;
        Ray r;
        r.org = org;
        r.dir = {0.0f, 0.0f, -1.0f};
        r.tMax = kInf;
        REQUIRE(scene.intersect(r, h));
        return h;
    };
    // Triangles: uv is affine in P, so the gradients predict a moved hit exactly.
    const SurfaceHit a = hitAt({0.2f, 0.3f, 5.0f});
    const SurfaceHit b = hitAt({0.25f, 0.27f, 5.0f});
    REQUIRE(a.prim == b.prim);
    const Vec3f dP = b.P - a.P;
    CHECK_THAT(b.uv.x - a.uv.x, WithinAbs(dot(a.dudP, dP), 1e-6));
    CHECK_THAT(b.uv.y - a.uv.y, WithinAbs(dot(a.dvdP, dP), 1e-6));
    CHECK(scene.curvature(a, 0.5f) == 0.0f);
    // Analytic sphere: first order.
    const SurfaceHit c = hitAt({3.1f, 0.2f, 5.0f});
    const SurfaceHit e = hitAt({3.1005f, 0.2004f, 5.0f});
    const Vec3f dS = e.P - c.P;
    CHECK_THAT(e.uv.x - c.uv.x, WithinAbs(dot(c.dudP, dS), 2e-6));
    CHECK_THAT(e.uv.y - c.uv.y, WithinAbs(dot(c.dvdP, dS), 2e-6));
    CHECK_THAT(scene.curvature(c, 0.5f), WithinRel(2.0, 1e-5));
    // A tessellated sphere with exact normals: about 1/R, convex as seen by
    // the ray (curvature is relative to Ng; makeSphere winds inwards).
    const SurfaceHit h = hitAt({-3.05f, 0.1f, 5.0f});
    const float side = h.Ng.z > 0.0f ? 1.0f : -1.0f; // the ray travels along -z
    CHECK_THAT(scene.curvature(h, 0.5f) * side, WithinRel(2.0, 0.02));
}

namespace {

// Returns the lookup's derivatives as its texels: (dsdx, dtdx, dsdy, dtdy).
class DerivativeSampler final : public ITextureSampler {
public:
    void sample2D(const TexSampler &, const TexQuery &q, float *out, uint8_t *status) const override {
        for (uint32_t i = 0; i < q.count; ++i) {
            out[0 * q.count + i] = q.dsdx ? q.dsdx[i] : -1.0f;
            out[1 * q.count + i] = q.dtdx ? q.dtdx[i] : -1.0f;
            out[2 * q.count + i] = q.dsdy ? q.dsdy[i] : -1.0f;
            out[3 * q.count + i] = q.rnd ? q.rnd[i] : -1.0f;
            status[i] = 0;
        }
    }
};

// An unlit quad over [-1, 1]^2 (st 0..1) showing Tex2D(st * scale) of the
// derivative sampler, rendered orthographically at 32 x 32 pixel centers.
std::vector<Spectrum> renderDerivatives(float scale) {
    Scene scene;
    MeshDesc quad = makeQuad(1.0f);
    quad.setAttribute({"st", AttributeRate::Vertex, 2, {0, 0, 1, 0, 1, 1, 0, 1}});
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, quad);
    sh::IrGraph g;
    g.model = SurfaceModel::Unlit;
    sh::IrBuilder b(g);
    TexSlot slot;
    slot.channels = 4;
    const uint32_t s = b.textureSlot(slot);
    const sh::Ref st = b.primvar("st", 2, b.constant(0.0f));
    const sh::Ref uv = scale == 1.0f ? st : b.mul(st, b.constant(scale));
    b.output(surface_field::emissionColor, 3, b.tex2D(s, uv, b.constant(0.0f)));
    const sh::CompileResult compiled = sh::compile(g);
    REQUIRE(compiled.program);
    MaterialDesc material;
    material.inputs = unlitDefaults();
    material.program = compiled.program;
    material.textures = {0};
    const MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, material);
    scene.setMeshMaterials(id, {m});
    DerivativeSampler sampler;
    scene.setTextureSampler(&sampler);
    scene.commit();

    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = 1;
    settings.pixelFilterScale = 0.0f;
    settings.textureFilterWidth = 1.0f;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    const int size = 32;
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, size, size));
    renderer.resize(size, size);
    renderToConvergence(renderer);
    std::vector<Spectrum> out;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            out.push_back(pixelMean(renderer.film(), x, y));
    return out;
}

} // namespace

TEST_CASE("Texture derivatives by shifted re-evaluation", "[texture][cones]") {
    const std::vector<Spectrum> base = renderDerivatives(1.0f), tiled = renderDerivatives(8.0f);
    // One pixel (2/32 world units) in st (0.5 per unit): 1/32, along the two
    // footprint axes; a uv scaled by 8 has exactly 8 times the derivatives.
    int wrong = 0;
    for (size_t i = 0; i < base.size(); ++i) {
        const Spectrum d = base[i];
        wrong += std::fabs(std::hypot(d.x, d.y) - 1.0f / 32.0f) > 1e-6f;
        wrong += tiled[i].x != 8.0f * d.x || tiled[i].y != 8.0f * d.y || tiled[i].z != 8.0f * d.z;
    }
    CHECK(wrong == 0);
    for (const Spectrum &d : base)
        wrong += std::fabs(std::fabs(d.z) - 1.0f / 32.0f) > 1e-6f; // dsdy: the second axis is along s here
    CHECK(wrong == 0);
}

TEST_CASE("Derivative clones share uniform work and only exist for varying uv", "[texture][cones][vm]") {
    sh::IrGraph g;
    sh::IrBuilder b(g);
    TexSlot slot;
    const uint32_t s = b.textureSlot(slot);
    const sh::Ref st = b.primvar("st", 2, b.constant(0.0f));
    const sh::Ref scale = b.newParam(2);
    const sh::Ref uv = b.add(b.mul(st, scale), b.constant(0.25f));
    b.output(surface_field::baseColor, 3, b.tex2D(s, uv, b.constant(0.5f)));
    const sh::CompileResult with = sh::compile(g);
    sh::CompileOptions noDerivs;
    noDerivs.derivatives = false;
    const sh::CompileResult without = sh::compile(g, noDerivs);
    REQUIRE((with.program && without.program));
    // Shifted primvars (two bindings), their mul/add clones (2 x 2), two
    // subtractions and a concatenation: 7 more instructions, no new prologue
    // work.
    CHECK(with.program->code.size() == without.program->code.size() + 7);
    CHECK(with.program->primvarBindings.size() == 3);
    CHECK(with.program->prologue.size() == without.program->prologue.size());
    // A uniform uv: no derivatives at all.
    sh::IrGraph u;
    sh::IrBuilder ub(u);
    const uint32_t us = ub.textureSlot(slot);
    ub.output(surface_field::baseColor, 3, ub.tex2D(us, ub.newParam(2), ub.constant(0.5f)));
    const sh::CompileResult ur = sh::compile(u);
    REQUIRE(ur.program);
    CHECK(ur.program->code.size() == 1);
}
