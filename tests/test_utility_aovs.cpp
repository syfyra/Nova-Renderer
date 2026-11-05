// M6.4: utility AOVs (docs/design/M6.md D9) with Karma's conventions, as
// measured: P and P_camera, normals in world and camera space, facing
// ratios, the distance from the eye, face indices, motion vectors (raster
// pixels over the shutter, y up) and velocity (world units over the
// shutter), primvars, the base color and per-sample ambient occlusion.

#include "test_helpers.h"

#include "core/integrator/renderer.h"
#include "core/scene/scene.h"
#include "core/shading/compiler.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>
#include <tuple>
#include <vector>

using namespace nova;
using namespace nova::test;
namespace sh = nova::shading;
using Catch::Matchers::WithinAbs;

namespace {

// Two faces in the z = 0 plane, seen from +Z: a quad (face 0, x in [-1, 0])
// and two triangles (faces 1 and 2, x in [0, 1]), with an st primvar.
MeshDesc faces() {
    MeshDesc m;
    m.points = {{-1, -1, 0}, {0, -1, 0}, {1, -1, 0}, {-1, 1, 0}, {0, 1, 0}, {1, 1, 0}};
    m.indices = {0, 1, 4, 0, 4, 3, 1, 2, 5, 1, 5, 4};
    m.triangleFaces = {0, 0, 1, 2};
    MeshAttribute st;
    st.name = "st";
    st.rate = AttributeRate::Vertex;
    st.width = 2;
    st.data = {0, 0, 0.5f, 0, 1, 0, 0, 1, 0.5f, 1, 1, 1};
    m.attributes.push_back(st);
    m.userId = 7;
    return m;
}

struct UtilityRender {
    Film film;
    const float *at(UtilityKind kind, int x, int y) const {
        for (const UtilityPlane &p : film.utility)
            if (p.request.kind == kind)
                return p.data.data() + (size_t(y) * size_t(film.width) + size_t(x)) * p.width;
        return nullptr;
    }
};

UtilityRender render(const Scene &scene, const Camera &camera, int w, int h, std::vector<UtilityRequest> requests,
                     uint32_t spp = 1, float frame = 1.0f) {
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = spp;
    settings.frame = frame;
    settings.maxDepth = 1;
    settings.utility = std::make_shared<const std::vector<UtilityRequest>>(std::move(requests));
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(camera);
    renderer.resize(w, h);
    renderToConvergence(renderer);
    return {renderer.film()};
}

} // namespace

TEST_CASE("Utility AOVs follow Karma's conventions", "[render][aov][utility]") {
    Scene scene;
    const MeshId mesh = scene.createMesh();
    scene.updateMesh(mesh, faces());
    LightDesc sun;
    sun.type = LightType::Distant;
    scene.updateLight(scene.createLight(), sun);
    scene.commit();
    using U = UtilityKind;
    // Perspective camera at z = 4 looking down -Z; 8x8 pixels over the plane.
    const Camera camera = makePerspectiveCamera({0.0f, 0.0f, 4.0f}, 2.0, 1.0, 8, 8);
    const UtilityRender r = render(scene, camera, 8, 8,
                                   {{U::P, {}}, {U::PCamera, {}}, {U::Ng, {}}, {U::NCamera, {}}, {U::FacingN, {}},
                                    {U::HitDist, {}}, {U::Face, {}}, {U::Primvar, "st"}, {U::BaseColor, {}},
                                    {U::MotionVector, {}}, {U::Velocity, {}}});
    // Pixel (1, 4): the quad; (5, 2): face 1 (below the diagonal x = y); (5, 6): face 2.
    for (const auto &[x, y, face] : {std::tuple{1, 4, 0}, std::tuple{5, 2, 1}, std::tuple{6, 6, 2}}) {
        INFO("pixel " << x << "," << y);
        const float *P = r.at(U::P, x, y);
        REQUIRE(P);
        CHECK_THAT(P[2], WithinAbs(0.0, 1e-5));
        const float *Pc = r.at(U::PCamera, x, y);
        CHECK_THAT(Pc[0], WithinAbs(P[0], 1e-5));
        CHECK_THAT(Pc[2], WithinAbs(-4.0, 1e-5)); // camera space looks down -Z
        CHECK(r.at(U::Ng, x, y)[2] == 1.0f);
        CHECK_THAT(r.at(U::NCamera, x, y)[2], WithinAbs(1.0, 1e-6));
        const float dist = std::sqrt(P[0] * P[0] + P[1] * P[1] + 16.0f);
        CHECK_THAT(r.at(U::HitDist, x, y)[0], WithinAbs(dist, 1e-4)); // from the eye, not the near plane
        CHECK_THAT(r.at(U::FacingN, x, y)[0], WithinAbs(4.0f / dist, 1e-5));
        CHECK(r.at(U::Face, x, y)[0] == float(face));
        const float *st = r.at(U::Primvar, x, y);
        CHECK_THAT(st[0], WithinAbs((P[0] + 1.0f) * 0.5f, 1e-5));
        CHECK_THAT(st[1], WithinAbs((P[1] + 1.0f) * 0.5f, 1e-5));
        CHECK_THAT(r.at(U::BaseColor, x, y)[1], WithinAbs(0.18, 1e-6)); // the default material's
        CHECK(r.at(U::MotionVector, x, y)[0] == 0.0f);
        CHECK(r.at(U::Velocity, x, y)[0] == 0.0f);
    }
    // The background: ids -1 (the buffer keeps its clear value), the rest 0.
    const Camera wide = makePerspectiveCamera({0.0f, 0.0f, 4.0f}, 0.5, 1.0, 8, 8);
    const UtilityRender bg = render(scene, wide, 8, 8, {{U::Face, {}}, {U::P, {}}});
    CHECK(bg.at(U::Face, 0, 0)[0] == -1.0f);
    CHECK(bg.at(U::P, 0, 0)[0] == 0.0f);
}

TEST_CASE("Karma's normal AOVs keep the authored orientation", "[render][aov][utility]") {
    // The quad turned over (its normal down -Z) seen from +Z: Karma's
    // ray:hitN/hitNg stay as authored, Hydra's normal turns to the viewer.
    Scene scene;
    MeshDesc quad = makeQuad(2.0f);
    quad.transform.m[1][1] = -1.0f;
    quad.transform.m[2][2] = -1.0f;
    scene.updateMesh(scene.createMesh(), quad);
    scene.commit();
    using U = UtilityKind;
    const UtilityRender r = render(scene, makeOrthoCamera({0.0f, 0.0f, 4.0f}, 1.0f, 4, 4), 4, 4,
                                   {{U::N, {}}, {U::Ng, {}}, {U::NCamera, {}}, {U::FacingN, {}}});
    CHECK(r.at(U::N, 1, 1)[2] == -1.0f);
    CHECK(r.at(U::Ng, 1, 1)[2] == -1.0f);
    CHECK_THAT(r.at(U::NCamera, 1, 1)[2], WithinAbs(-1.0, 1e-6));
    CHECK_THAT(r.at(U::FacingN, 1, 1)[0], WithinAbs(1.0, 1e-6));
    CHECK(r.film.normal[5].z == 1.0f);
}

TEST_CASE("Motion vectors and velocity span the shutter", "[render][aov][utility]") {
    // A quad moving +1 in x and +0.5 in y over the shutter, seen from z = 4
    // by an orthographic camera 2 units wide over 8 pixels: 4 pixels per unit.
    Scene scene;
    const MeshId mesh = scene.createMesh();
    scene.updateMesh(mesh, makeQuad(3.0f));
    scene.updateMeshMotion(mesh, {translation({-0.5f, -0.25f, 0.0f}), translation({0.5f, 0.25f, 0.0f})});
    scene.commit();
    using U = UtilityKind;
    Camera camera = makeOrthoCamera({0.0f, 0.0f, 4.0f}, 1.0f, 8, 8);
    camera.frameTime = 0.5f;
    const UtilityRender r = render(scene, camera, 8, 8, {{U::MotionVector, {}}, {U::Velocity, {}}});
    const float *mv = r.at(U::MotionVector, 4, 4), *v = r.at(U::Velocity, 4, 4);
    CHECK_THAT(mv[0], WithinAbs(4.0, 1e-4));
    CHECK_THAT(mv[1], WithinAbs(2.0, 1e-4)); // y up, as Karma
    CHECK_THAT(v[0], WithinAbs(1.0, 1e-5));
    CHECK_THAT(v[1], WithinAbs(0.5, 1e-5));
}

TEST_CASE("Ambient occlusion is the unoccluded fraction of cosine rays", "[render][aov][utility]") {
    // A floor under a large roof at height h: occluded at any height (no
    // distance limit, as Karma); without the roof every ray escapes.
    using U = UtilityKind;
    for (const float h : {0.0f, 0.5f, 50.0f}) {
        INFO("roof " << h);
        Scene scene;
        scene.updateMesh(scene.createMesh(), makeQuad(10.0f));
        if (h > 0.0f) {
            MeshDesc roof = makeQuad(h * 1000.0f);
            roof.transform = translation({0.0f, 0.0f, h});
            scene.updateMesh(scene.createMesh(), roof);
        }
        scene.commit();
        const UtilityRender r = render(scene, makeOrthoCamera({0.0f, 0.0f, 0.2f}, 1.0f, 4, 4), 4, 4, {{U::Ao, {}}}, 32);
        double sum = 0.0;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                sum += r.at(U::Ao, x, y)[0] / (16.0 * r.film.samples);
        CHECK_THAT(sum, WithinAbs(h > 0.0f ? 0.0 : 1.0, 1e-3));
    }
}

TEST_CASE("Fake-caustic objects occlude ambient occlusion", "[render][aov][utility][causticsfake]") {
    // Fake caustics let a light sample's shadow ray pass (its transmittance
    // follows, M4 3.3); AO has no such phase, so the opaque roof still
    // occludes every ray with the render-wide setting on.
    using U = UtilityKind;
    for (const bool fake : {false, true}) {
        INFO("fake caustics " << fake);
        Scene scene;
        scene.updateMesh(scene.createMesh(), makeQuad(10.0f));
        MeshDesc roof = makeQuad(500.0f);
        roof.transform = translation({0.0f, 0.0f, 0.5f});
        scene.updateMesh(scene.createMesh(), roof);
        scene.setDefaultFakeCaustics(fake);
        scene.commit();
        REQUIRE(scene.anyFakeCaustics() == fake);
        const UtilityRender r = render(scene, makeOrthoCamera({0.0f, 0.0f, 0.2f}, 1.0f, 4, 4), 4, 4, {{U::Ao, {}}}, 32);
        double sum = 0.0;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                sum += r.at(U::Ao, x, y)[0] / (16.0 * r.film.samples);
        CHECK_THAT(sum, WithinAbs(0.0, 1e-3));
    }
}

TEST_CASE("First-hit AOVs shade at the render's frame", "[render][aov][utility]") {
    // Base color 1 after frame 5, else 0 (a frame-driven material): the base
    // color AOV and the albedo (the denoiser's guide) follow the frame the
    // beauty is shaded at.
    Scene scene;
    const MeshId mesh = scene.createMesh();
    scene.updateMesh(mesh, makeQuad(2.0f));
    sh::IrGraph g;
    sh::IrBuilder b(g);
    const sh::Ref late = b.binary(sh::Op::Greater, b.global(sh::Global::Frame), b.constant(5.0f));
    b.output(surface_field::baseColor, 3, b.convert(late, 3));
    MaterialDesc material;
    material.inputs = lambertDefaults();
    material.program = sh::compile(g).program;
    REQUIRE(material.program);
    const MaterialId m = scene.createMaterial();
    scene.updateMaterial(m, material);
    scene.setMeshMaterials(mesh, {m});
    scene.commit();
    using U = UtilityKind;
    for (const float frame : {1.0f, 10.0f}) {
        INFO("frame " << frame);
        const UtilityRender r =
            render(scene, makeOrthoCamera({0.0f, 0.0f, 4.0f}, 1.0f, 4, 4), 4, 4, {{U::BaseColor, {}}}, 1, frame);
        const bool isLate = frame > 5.0f;
        CHECK(r.at(U::BaseColor, 1, 1)[1] == (isLate ? 1.0f : 0.0f));
        const float albedo = r.film.albedo[size_t(1) * 4 + 1].y;
        CHECK((isLate ? albedo > 0.5f : albedo < 1e-6f));
    }
}
