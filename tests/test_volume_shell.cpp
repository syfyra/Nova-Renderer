// Media bounded by surfaces (docs/design/M11-P5.md: Karma's uniform volumes,
// the Karma Fog Box): the shell's crossings (watertight, one per crossing
// also through edges, the BVH equal to brute force), the spans a ray gets
// inside (camera inside, shadow rays ending inside, several components,
// transforms, holes closed by the bounds), exact transmittance through
// boxes and spheres, the default medium, constant primvars, overlaps, the
// shadow-ray density, noise media tracked without bias on interval
// majorants, and volume lights inside their shell only.

#include "test_helpers.h"

#include "core/integrator/renderer.h"
#include "core/integrator/volume_media.h"
#include "core/math/rng.h"
#include "core/scene/scene.h"
#include "core/shading/compiler.h"
#include "core/shading/interval.h"
#include "core/shading/vm.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace nova;
using namespace nova::test;
using namespace nova::shading;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// A closed box as a triangle mesh (outward winding), or with its +Z face
// left out.
struct BoxMesh {
    std::vector<Vec3f> points;
    std::vector<uint32_t> indices;
};
BoxMesh boxMesh(Vec3f lo, Vec3f hi, bool open = false) {
    BoxMesh m;
    m.points = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    const uint32_t quads[6][4] = {{0, 3, 2, 1}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}, {4, 5, 6, 7}};
    for (int q = 0; q < (open ? 5 : 6); ++q)
        m.indices.insert(m.indices.end(), {quads[q][0], quads[q][1], quads[q][2], quads[q][0], quads[q][2], quads[q][3]});
    return m;
}
MediumShellPtr boxShell(Vec3f lo, Vec3f hi, bool open = false) {
    const BoxMesh m = boxMesh(lo, hi, open);
    return makeTriangleShell(m.points.data(), m.points.size(), m.indices.data(), m.indices.size() / 3);
}
MediumShellPtr meshShell(const MeshDesc &mesh) {
    return makeTriangleShell(mesh.points.data(), mesh.pointCount(), mesh.indices.data(), mesh.triangleCount());
}

// A volume material of constant coefficients.
MaterialDesc constantMedium(Spectrum s, Spectrum a, Spectrum e = spectrum(0.0f)) {
    IrGraph g;
    IrBuilder b(g);
    const auto field = [](uint16_t f) { return uint16_t(volume_field::kBase + f); };
    b.output(field(volume_field::scattering), 3, b.constant(s.x, s.y, s.z));
    b.output(field(volume_field::absorption), 3, b.constant(a.x, a.y, a.z));
    b.output(field(volume_field::emission), 3, b.constant(e.x, e.y, e.z));
    MaterialDesc m;
    m.program = compile(g).program;
    return m;
}

// Absorption from the primvar "density" (fallback `fallback`), as the Fog
// Box reads its geomprop; on shadow rays times `shadow` (ray:isshadowray).
MaterialDesc densityMedium(float fallback, float shadow = 1.0f) {
    IrGraph g;
    IrBuilder b(g);
    const Ref d = b.primvar("density", 1, b.constant(fallback));
    const Ref isShadow = b.binary(Op::Equal, b.global(Global::RayType), b.constant(float(RayClass::Shadow)));
    const Ref value = b.ternary(Op::Select, isShadow, b.mul(d, b.constant(shadow)), d);
    b.output(uint16_t(volume_field::kBase + volume_field::absorption), 3, b.convert(value, 3));
    MaterialDesc m;
    m.program = compile(g).program;
    return m;
}

// The Fog Box's noise: density d * remap(fractal3d(P * freq), -0.5..0.5 ->
// 0..1) absorbing (clamped at 0 by the volume point).
MaterialDesc noiseMedium(float d, float freq) {
    IrGraph g;
    IrBuilder b(g);
    const Ref p = b.mul(b.global(Global::P), b.constant(freq));
    const Ref n = b.fractal(p, b.constant(3.0f), b.constant(2.0f), b.constant(0.5f), 1);
    const Ref density = b.mul(b.constant(d), b.add(n, b.constant(0.5f)));
    b.output(uint16_t(volume_field::kBase + volume_field::absorption), 3, b.convert(density, 3));
    MaterialDesc m;
    m.program = compile(g).program;
    return m;
}

VolumeId addShell(Scene &scene, MediumShellPtr shell, MaterialId material, const Mat4 &xf = Mat4::identity(),
                  std::vector<VolumeConstant> constants = {}) {
    VolumeDesc desc;
    desc.name = "shell";
    desc.shell = std::move(shell);
    desc.material = material;
    desc.constants = std::move(constants);
    const VolumeId id = scene.createVolume();
    scene.updateVolume(id, desc);
    scene.updateVolumeMotion(id, {xf});
    return id;
}

MaterialId addMaterial(Scene &scene, const MaterialDesc &m) {
    const MaterialId id = scene.createMaterial();
    scene.updateMaterial(id, m);
    return id;
}

void addDome(Scene &scene, bool visible) {
    LightDesc dome;
    dome.type = LightType::Dome;
    dome.color = spectrum(1.0f);
    dome.cameraVisible = visible;
    scene.updateLight(scene.createLight(), dome);
}

Mat4 scaled(float s, Vec3f t) {
    Mat4 m = translation(t);
    m.m[0][0] = m.m[1][1] = m.m[2][2] = s;
    return m;
}

struct Mean {
    Spectrum color{0.0f, 0.0f, 0.0f};
    double alpha = 0.0;
};
Mean meanOf(const Film &film, int x0, int y0, int x1, int y1) {
    Mean m;
    const float n = float((x1 - x0) * (y1 - y0));
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const size_t i = size_t(y) * size_t(film.width) + size_t(x);
            m.color += pixelMean(film, x, y) * (1.0f / n);
            m.alpha += film.alphaSum[i] / film.count[i] / double(n);
        }
    return m;
}

Film renderFilm(const Scene &scene, const Camera &camera, int w, int h, uint32_t spp, uint32_t depth = 4) {
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = spp;
    settings.maxDepth = depth;
    settings.clampIndirect = 0.0f;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(camera);
    renderer.resize(w, h);
    renderToConvergence(renderer);
    return renderer.film();
}

int crossings(const MediumShellPtr &s, Vec3f o, Vec3f d) {
    float c[kShellCrossings];
    int8_t sign[kShellCrossings];
    return shellCrossings(s->view(), o, d, 0.0f, c, sign);
}

} // namespace

TEST_CASE("Shells count each crossing once", "[volume][shell]") {
    const MediumShellPtr cube = boxShell({-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f});
    REQUIRE(cube);
    CHECK(cube->openEdges == 0);
    float c[kShellCrossings];
    int8_t sign[kShellCrossings];
    REQUIRE(shellCrossings(cube->view(), {0.1f, 0.2f, 5.0f}, {0.0f, 0.0f, -1.0f}, 0.0f, c, sign) == 2);
    CHECK_THAT(c[0], WithinAbs(4.5, 1e-5));
    CHECK_THAT(c[1], WithinAbs(5.5, 1e-5));
    CHECK(sign[0] == -1); // in against the outward normal
    CHECK(sign[1] == 1);
    // The whole line: also behind the origin.
    REQUIRE(shellCrossings(cube->view(), {0.1f, 0.2f, 0.0f}, {0.0f, 0.0f, -1.0f}, -1e30f, c, sign) == 2);
    CHECK_THAT(c[0], WithinAbs(-0.5, 1e-5));
    CHECK(sign[0] == -1);
    CHECK(crossings(cube, {0.1f, 0.2f, 0.0f}, {0.0f, 0.0f, -1.0f}) == 1); // from inside
    CHECK(crossings(cube, {0.1f, 0.2f, 5.0f}, {0.0f, 0.0f, 1.0f}) == 0);  // away from it
    // Through the faces' diagonals (the triangles' shared edges), an edge of
    // the box and a corner: still one crossing per face.
    CHECK(crossings(cube, {0.25f, 0.25f, 5.0f}, {0.0f, 0.0f, -1.0f}) == 2);
    CHECK(crossings(cube, {-0.25f, 0.25f, 5.0f}, {0.0f, 0.0f, -1.0f}) == 2);
    CHECK(crossings(cube, {0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}) == 2);
    CHECK(crossings(cube, {2.0f, 2.0f, 2.0f}, normalize(Vec3f{-1.0f, -1.0f, -1.0f})) % 2 == 0);
    CHECK(crossings(cube, {0.5f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}) % 2 == 0); // along a side face
    const Vec3f diag = normalize(Vec3f{1.0f, 1.0f, 0.0f});
    CHECK(crossings(cube, Vec3f{0.0f, 0.0f, 0.1f} - diag * 3.0f, diag) == 2); // through two edges of the box

    SECTION("open boxes are counted, not closed") {
        const MediumShellPtr open = boxShell({-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, true);
        CHECK(open->openEdges == 4);
        CHECK(!open->view().closed);
        CHECK(crossings(open, {0.1f, 0.2f, 5.0f}, {0.0f, 0.0f, -1.0f}) == 1);
        // Inside it whichever way the line leaves (through a face or the hole).
        CHECK(shellContains(open->view(), {0.1f, 0.0f, 0.0f}));
        CHECK(shellInsideAt(open->view(), {0.1f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, 0.0f));
        CHECK(shellInsideAt(open->view(), {0.1f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 0.0f));
        CHECK(!shellContains(open->view(), {0.1f, 0.0f, 0.8f}));
        CHECK(!shellInsideAt(open->view(), {0.1f, 0.0f, 0.8f}, {0.0f, 0.0f, -1.0f}, 0.0f));
    }
    SECTION("spheres") {
        const MediumShellPtr ball = makeSphereShell({1.0f, 0.0f, 0.0f}, 0.5f);
        REQUIRE(shellCrossings(ball->view(), {1.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -2.0f}, 0.0f, c, sign) == 2); // d not unit
        CHECK_THAT(c[0], WithinAbs(2.25, 1e-5));
        CHECK_THAT(c[1], WithinAbs(2.75, 1e-5));
        CHECK(crossings(ball, {1.1f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}) == 1);
        CHECK(shellContains(ball->view(), {1.2f, 0.1f, 0.0f}));
        CHECK(!shellContains(ball->view(), {1.6f, 0.0f, 0.0f}));
    }
    SECTION("the BVH finds what a loop over the triangles finds") {
        // A UV sphere whose seam and poles repeat points (welded by position):
        // made exact here (sin(pi) and phi = 2 pi leave tiny gaps in float).
        MeshDesc sphere = makeSphere(0.7f, 96, 48);
        for (Vec3f &p : sphere.points)
            if (std::fabs(p.x) < 1e-6f && std::fabs(p.z) < 1e-6f)
                p.x = p.z = 0.0f;
        for (size_t r = 0; r <= 48; ++r)
            sphere.points[r * 97 + 96] = sphere.points[r * 97];
        const MediumShellPtr s = meshShell(sphere);
        REQUIRE(s);
        CHECK(s->nodes.size() > 1);
        CHECK(s->openEdges == 0); // the seam's and the poles' duplicate points weld
        Rng rng = Rng::make(3, 4, 5);
        int checked = 0;
        for (int r = 0; r < 2000; ++r) {
            const Vec3f o{rng.next() * 3.0f - 1.5f, rng.next() * 3.0f - 1.5f, rng.next() * 3.0f - 1.5f};
            const Vec3f d = normalize(Vec3f{rng.next() - 0.5f, rng.next() - 0.5f, rng.next() - 0.5f});
            int brute = 0;
            const shell_detail::WatertightRay wr(d);
            for (size_t t = 0; t < s->triangles.size() / 3; ++t) {
                float tt;
                bool leaves;
                brute += wr.hit(o, s->triangles[t * 3], s->triangles[t * 3 + 1], s->triangles[t * 3 + 2], tt, leaves) &&
                                 tt > 0.0f
                             ? 1
                             : 0;
            }
            CHECK(crossings(s, o, d) == brute);
            // Inside the sphere: an odd count.
            const bool inside = length(o) < 0.69f, outside = length(o) > 0.71f;
            if (inside)
                CHECK(brute % 2 == 1);
            if (outside)
                CHECK(brute % 2 == 0);
            checked += inside || outside;
        }
        CHECK(checked > 1800);
        CHECK(shellContains(s->view(), {0.1f, 0.2f, 0.3f}));
        CHECK(!shellContains(s->view(), {0.7f, 0.7f, 0.0f}));
    }
}

TEST_CASE("Shells give the spans inside them", "[volume][shell]") {
    Scene scene;
    const MaterialId absorber = addMaterial(scene, constantMedium(spectrum(0.0f), spectrum(1.0f)));
    // Two boxes in one mesh (x in [-0.5, 0.5] and [1, 2]), a sphere at z = -3.
    BoxMesh two = boxMesh({-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f});
    const BoxMesh second = boxMesh({1.0f, -0.5f, -0.5f}, {2.0f, 0.5f, 0.5f});
    for (uint32_t i : second.indices)
        two.indices.push_back(i + uint32_t(two.points.size()));
    two.points.insert(two.points.end(), second.points.begin(), second.points.end());
    const VolumeId boxes =
        addShell(scene, makeTriangleShell(two.points.data(), two.points.size(), two.indices.data(), two.indices.size() / 3),
                 absorber);
    scene.commit();
    REQUIRE(scene.activeVolumes().size() == 1);
    std::vector<VolumeSpan> spans;
    const auto spansOf = [&](Vec3f o, Vec3f d, float tEnd) {
        volumeSpans(scene, o, d, tEnd, 0.5f, spans);
        return spans.size();
    };
    REQUIRE(spansOf({0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}, 100.0f) == 1);
    CHECK_THAT(spans[0].t0, WithinAbs(4.5, 1e-5));
    CHECK_THAT(spans[0].t1, WithinAbs(5.5, 1e-5));
    CHECK(spans[0].id == boxes);
    // Along x through both boxes.
    REQUIRE(spansOf({-3.0f, 0.1f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f) == 2);
    CHECK_THAT(spans[0].t0, WithinAbs(2.5, 1e-5));
    CHECK_THAT(spans[0].t1, WithinAbs(3.5, 1e-5));
    CHECK_THAT(spans[1].t0, WithinAbs(4.0, 1e-5));
    CHECK_THAT(spans[1].t1, WithinAbs(5.0, 1e-5));
    // From inside (the camera in the box): from 0 to the wall; a shadow ray
    // that ends inside: up to its end.
    REQUIRE(spansOf({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f) == 2);
    CHECK(spans[0].t0 == 0.0f);
    CHECK_THAT(spans[0].t1, WithinAbs(0.5, 1e-5));
    REQUIRE(spansOf({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 0.3f) == 1);
    CHECK(spans[0].t0 == 0.0f);
    CHECK(spans[0].t1 == 0.3f);
    // Ending before the box: nothing.
    CHECK(spansOf({0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}, 4.0f) == 0);

    SECTION("transformed shells") {
        Scene moved;
        const MaterialId m = addMaterial(moved, constantMedium(spectrum(0.0f), spectrum(1.0f)));
        addShell(moved, boxShell({-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}), m, scaled(2.0f, {3.0f, 0.0f, 0.0f}));
        moved.commit();
        volumeSpans(moved, {3.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}, 100.0f, 0.5f, spans);
        REQUIRE(spans.size() == 1);
        CHECK_THAT(spans[0].t0, WithinAbs(4.0, 1e-5));
        CHECK_THAT(spans[0].t1, WithinAbs(6.0, 1e-5));
    }
    SECTION("holes behave as closed by the bounds") {
        Scene open;
        const MaterialId m = addMaterial(open, constantMedium(spectrum(0.0f), spectrum(1.0f)));
        addShell(open, boxShell({-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, true), m);
        open.commit();
        // In through the hole (+Z): from the bounds to the back face.
        volumeSpans(open, {0.1f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}, 100.0f, 0.5f, spans);
        REQUIRE(spans.size() == 1);
        CHECK_THAT(spans[0].t0, WithinAbs(4.5, 1e-5));
        CHECK_THAT(spans[0].t1, WithinAbs(5.5, 1e-5));
        // Out through it from inside: to the bounds.
        volumeSpans(open, {0.1f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, 100.0f, 0.5f, spans);
        REQUIRE(spans.size() == 1);
        CHECK_THAT(spans[0].t1, WithinAbs(0.5, 1e-5));
    }
}

TEST_CASE("Media bounded by shells absorb with their exact transmittance", "[volume][shell][render]") {
    // A unit box absorbing 1 per unit length seen from the front: alpha
    // 1 - e^-1 over its whole face (no light, alpha only).
    const Camera front = makeOrthoCamera({0.0f, 0.0f, 5.0f}, 0.4f, 16, 16);
    {
        Scene scene;
        addShell(scene, boxShell({-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}),
                 addMaterial(scene, constantMedium(spectrum(0.0f), spectrum(1.0f))));
        scene.commit();
        const Mean m = meanOf(renderFilm(scene, front, 16, 16, 256), 0, 0, 16, 16);
        CHECK_THAT(m.alpha, WithinRel(1.0 - std::exp(-1.0), 0.005));
    }
    SECTION("a sphere, the camera inside it") {
        // Radius 2, absorption 0.5, the camera at the centre looking at a
        // visible dome: e^-1 in every pixel near the axis.
        Scene scene;
        addShell(scene, makeSphereShell({0.0f, 0.0f, 0.0f}, 2.0f),
                 addMaterial(scene, constantMedium(spectrum(0.0f), spectrum(0.5f))));
        addDome(scene, true);
        scene.commit();
        const Camera inside = makePerspectiveCamera({0.0f, 0.0f, 0.0f}, 0.05, 0.01, 8, 8, 1e-5); // rays start at the near plane
        const Mean m = meanOf(renderFilm(scene, inside, 8, 8, 64), 0, 0, 8, 8);
        CHECK_THAT(m.color.y, WithinRel(std::exp(-1.0), 0.002));
        CHECK_THAT(m.alpha, WithinAbs(1.0, 1e-6)); // the dome is visible
    }
    SECTION("a shell without a volume material: white, density 1 (Karma)") {
        Scene scene;
        addShell(scene, boxShell({-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}), kDefaultMaterialId);
        scene.commit();
        const Mean m = meanOf(renderFilm(scene, front, 16, 16, 256), 0, 0, 16, 16);
        CHECK_THAT(m.alpha, WithinRel(1.0 - std::exp(-1.0), 0.01));
    }
    SECTION("constant primvars feed the shader's reads, else the fallback") {
        Scene scene;
        const MaterialId m = addMaterial(scene, densityMedium(0.25f));
        addShell(scene, boxShell({-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}), m, translation({-1.0f, 0.0f, 0.0f}));
        addShell(scene, boxShell({-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}), m, translation({1.0f, 0.0f, 0.0f}),
                 {{"density", {2.0f}}});
        scene.commit();
        // Pixels of 1/8 unit: the boxes cover columns 4-11 and 20-27, rows 12-19.
        const Film film = renderFilm(scene, makeOrthoCamera({0.0f, 0.0f, 5.0f}, 2.0f, 32, 32), 32, 32, 1024);
        CHECK_THAT(meanOf(film, 5, 13, 11, 19).alpha, WithinAbs(1.0 - std::exp(-0.25), 0.006)); // x = -1
        CHECK_THAT(meanOf(film, 21, 13, 27, 19).alpha, WithinAbs(1.0 - std::exp(-2.0), 0.006)); // x = +1
    }
    SECTION("overlapping and nested shells add") {
        // A box absorbing 0.5 with a box half its size inside, absorbing 0.5
        // as well: 0.5 + 0.25 through the middle, 0.5 beside it.
        Scene scene;
        const MaterialId half = addMaterial(scene, constantMedium(spectrum(0.0f), spectrum(0.5f)));
        addShell(scene, boxShell({-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}), half);
        addShell(scene, boxShell({-0.25f, -0.25f, -0.25f}, {0.25f, 0.25f, 0.25f}), half); // nested
        scene.commit();
        // Pixels of 0.05: the inner box covers 3-12, columns 0-2 only the outer.
        const Film film = renderFilm(scene, front, 16, 16, 1024);
        CHECK_THAT(meanOf(film, 5, 5, 11, 11).alpha, WithinAbs(1.0 - std::exp(-0.75), 0.012));
        CHECK_THAT(meanOf(film, 0, 0, 2, 16).alpha, WithinAbs(1.0 - std::exp(-0.5), 0.012));
    }
}

TEST_CASE("Shadow rays see the shadow density", "[volume][shell][render]") {
    // A ground (z = 0) lit straight down through a box absorbing 1 for the
    // camera and `shadow` for shadow rays (Karma's ray:isshadowray switch);
    // the camera looks down at the ground under the box, through it.
    const auto groundUnder = [](float shadow, bool box) {
        Scene scene;
        MeshDesc ground = makeQuad(4.0f);
        ground.setDisplayColor(spectrum(0.5f));
        scene.updateMesh(scene.createMesh(), ground);
        if (box)
            addShell(scene, boxShell({-1.0f, -1.0f, 1.0f}, {1.0f, 1.0f, 2.0f}), addMaterial(scene, densityMedium(1.0f, shadow)));
        LightDesc sun; // along its local -Z: straight down
        sun.type = LightType::Distant;
        sun.angleDeg = 0.0f;
        scene.updateLight(scene.createLight(), sun);
        scene.commit();
        return double(meanOf(renderFilm(scene, makeOrthoCamera({0.0f, 0.0f, 5.0f}, 0.5f, 8, 8), 8, 8, 256, 1), 0, 0, 8, 8)
                          .color.y);
    };
    const double clear = groundUnder(0.0f, false), none = groundUnder(0.0f, true), twice = groundUnder(2.0f, true);
    REQUIRE(clear > 0.0);
    CHECK_THAT(none / clear, WithinRel(std::exp(-1.0), 0.01)); // seen through the box, lit past it
    CHECK_THAT(twice / none, WithinRel(std::exp(-2.0), 0.02)); // lit through it at twice the density
}

TEST_CASE("Interval bounds of noise hold for every value", "[volume][shell][noise]") {
    // Perlin noise (2D and 3D, scalar and vector) and fBm: the interval rules'
    // bounds against 400 000 values each.
    IrGraph g;
    IrBuilder b(g);
    const Ref p = b.global(Global::P);
    const Ref p2 = b.swizzle(p, 0, 1);
    const Ref terms[5] = {b.noise(p, 1), b.noise(p2, 1), b.noise(p, 3),
                          b.fractal(p, b.constant(4.0f), b.constant(2.0f), b.constant(0.6f), 1),
                          b.fractal(p2, b.constant(3.0f), b.constant(1.9f), b.constant(0.5f), 3)};
    Ref sum = b.constant(0.0f);
    std::vector<uint16_t> fields;
    for (int i = 0; i < 5; ++i) {
        // One volume output per term, so each keeps its own row.
        const uint16_t field = uint16_t(i == 0 ? volume_field::anisotropy
                                        : i == 1 ? volume_field::secondaryAnisotropy
                                        : i == 2 ? volume_field::scattering
                                        : i == 3 ? volume_field::secondaryMix
                                                 : volume_field::emission);
        b.output(uint16_t(volume_field::kBase + field), b.width(terms[i]), terms[i]);
        fields.push_back(field);
    }
    const CompileResult r = compile(g);
    REQUIRE(r.program);
    REQUIRE(r.program->volume);
    const Program &prog = *r.program->volume;
    std::vector<float> U;
    prog.initUniforms(nullptr, U);
    std::vector<Interval> R(std::max<size_t>(prog.rowCount, 1));
    for (const GlobalBinding &gb : prog.globals)
        for (int k = 0; k < globalWidth(gb.global); ++k)
            R[gb.row + k] = Interval::unbounded(); // any position
    executeInterval(prog.code.data(), prog.code.size(), R.data(), U.data());
    std::vector<float> rows(std::max<size_t>(prog.rowCount, 1));
    Rng rng = Rng::make(5, 6, 7);
    int outside = 0, checked = 0;
    float largest[2] = {0.0f, 0.0f}; // |perlin| 3D and 2D seen
    for (int s = 0; s < 400000; ++s) {
        const Vec3f x{rng.next() * 64.0f - 32.0f, rng.next() * 64.0f - 32.0f, rng.next() * 64.0f - 32.0f};
        for (const GlobalBinding &gb : prog.globals) {
            rows[gb.row] = x.x;
            rows[gb.row + 1] = x.y;
            rows[gb.row + 2] = x.z;
        }
        execute<1>(prog.code.data(), prog.code.size(), rows.data(), U.data());
        for (const OutputBinding &o : prog.outputs) {
            if (o.uniform)
                continue;
            for (int k = 0; k < o.width; ++k) {
                const float v = rows[o.index + k];
                const Interval &iv = R[o.index + k];
                outside += !(v >= iv.lo && v <= iv.hi);
                ++checked;
                if (o.field == volume_field::anisotropy)
                    largest[0] = std::max(largest[0], std::fabs(v));
                if (o.field == volume_field::secondaryAnisotropy)
                    largest[1] = std::max(largest[1], std::fabs(v));
            }
        }
    }
    INFO("largest |perlin| 3D " << largest[0] << ", 2D " << largest[1]);
    CHECK(checked >= 3600000);
    CHECK(outside == 0);
    for (const OutputBinding &o : prog.outputs)
        if (!o.uniform)
            CHECK(R[o.index].bounded());
}

TEST_CASE("Noise media are tracked without bias on interval majorants", "[volume][shell][noise]") {
    // The Fog Box's noise in a sphere: density 2 (fractal3d(2 P) + 0.5),
    // clamped at 0. The interval rules of noise bound every cell (no
    // estimates, no violations); ratio tracking agrees with the exact optical
    // depth (a fine quadrature of the shader inside the sphere).
    Scene scene;
    const MaterialId m = addMaterial(scene, noiseMedium(2.0f, 2.0f));
    const VolumeId id = addShell(scene, makeSphereShell({0.0f, 0.0f, 0.0f}, 1.0f), m);
    scene.commit();
    const VolumeRecord &rec = scene.volumeRecord(id);
    CHECK(rec.estimatedCells == 0);
    CHECK(rec.layout.cellCount() == 32 * 32 * 32);
    const uint64_t violationsBefore = majorantViolations();

    VolumeEvaluator exact(scene, id);
    VolumeEvalCache cache;
    Rng pick = Rng::make(11, 0, 1);
    double zSum = 0.0, varSum = 0.0;
    int rays = 0;
    for (int r = 0; r < 60; ++r) {
        const Vec3f o{pick.next() * 2.0f - 1.0f, pick.next() * 2.0f - 1.0f, 3.0f};
        const Vec3f target{pick.next() * 1.6f - 0.8f, pick.next() * 1.6f - 0.8f, -3.0f};
        const Vec3f d = normalize(target - o);
        const float tEnd = length(target - o);
        double tau = 0.0;
        const int steps = 8000;
        for (int i = 0; i < steps; ++i) {
            const Vec3f p = o + d * ((float(i) + 0.5f) * tEnd / float(steps));
            if (dot(p, p) < 1.0f)
                tau += double(exact(p, p, d, 0.5f, 0, 0).absorption.y) * (tEnd / steps);
        }
        const double T = std::exp(-tau);
        if (T < 0.02 || T > 0.98)
            continue;
        const MediumRay ray{o, d, tEnd, 0.5f, 0, 0};
        double mean = 0.0, m2 = 0.0;
        const int n = 300;
        for (int s = 0; s < n; ++s) {
            Rng rng = Rng::make(uint32_t(r), uint32_t(s), 3);
            const double v = trackMedia(scene, cache, ShadeGlobals{}, ray, rng.next(), rng, nullptr).y;
            mean += v / n;
            m2 += v * v / n;
        }
        zSum += mean - T;
        varSum += std::max(m2 - mean * mean, 1e-12) / n;
        ++rays;
    }
    REQUIRE(rays > 20);
    const double z = zSum / std::sqrt(varSum);
    INFO("rays " << rays << ", z " << z);
    CHECK(std::fabs(z) < 3.5);
    CHECK(majorantViolations() == violationsBefore);
}

TEST_CASE("Shell volume lights emit inside their shell only", "[volume][shell][light]") {
    // An emitting sphere (absorption 1, emission 1) as a light lighting a
    // ground: light samples outside the sphere (its cells' corners) emit
    // nothing, so the light-sampled estimate equals the collisions'.
    const auto groundLight = [](bool light) {
        Scene scene;
        MeshDesc ground = makeQuad(3.0f); // z = 0, facing +Z
        ground.setDisplayColor(spectrum(0.5f));
        scene.updateMesh(scene.createMesh(), ground);
        VolumeDesc desc;
        desc.shell = makeSphereShell({0.0f, 0.0f, 0.0f}, 0.5f);
        desc.material = addMaterial(scene, constantMedium(spectrum(0.0f), spectrum(1.0f), spectrum(1.0f)));
        desc.light = light;
        const VolumeId id = scene.createVolume();
        scene.updateVolume(id, desc);
        scene.updateVolumeMotion(id, {translation({0.0f, 0.0f, 0.8f})});
        scene.commit();
        // The ground beside the sphere (the camera does not see it there).
        const Film film = renderFilm(scene, makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.5f, 12, 12), 12, 12, 1024, 2);
        return double(meanOf(film, 0, 0, 12, 3).color.y);
    };
    const double collisions = groundLight(false), sampled = groundLight(true);
    INFO("collisions " << collisions << ", light samples + MIS " << sampled);
    REQUIRE(collisions > 0.0);
    CHECK_THAT(sampled, WithinRel(collisions, 0.04));
}
