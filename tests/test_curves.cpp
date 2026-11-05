// M9 curves (docs/design/M9.md): the bases evaluated as Embree evaluates
// them, segments and per-segment attributes (prepareCurves), and hits on
// round tubes and ribbons: distance, normal, tangent and the offset across
// the fibre, for every basis, moving and instanced curves; alpha coverage
// of a tube of known width.

#include "test_helpers.h"

#include "core/geometry/curves.h"
#include "core/integrator/renderer.h"
#include "core/math/frame.h"
#include "core/scene/ray.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;

namespace {

// One curve along x from -1 to 1 at height z = 0 (control points on the
// line, uniformly spaced: every basis gives the straight segment there).
MeshDesc straightCurve(CurveBasis basis, float radius, CurveStyle style = CurveStyle::Round) {
    MeshDesc d;
    d.shape = MeshShape::Curves;
    d.curveBasis = basis;
    d.curveStyle = style;
    const int n = basis == CurveBasis::Linear ? 5 : basis == CurveBasis::Bezier ? 7 : 7;
    // B-spline and Catmull-Rom need a phantom point at each end for the
    // curve to span [-1, 1].
    const bool phantoms = basis == CurveBasis::BSpline || basis == CurveBasis::CatmullRom;
    const float x0 = -1.0f, x1 = 1.0f;
    const int inner = phantoms ? n - 2 : n;
    const float step = (x1 - x0) / float(inner - 1);
    for (int i = 0; i < n; ++i) {
        const float x = x0 + step * float(phantoms ? i - 1 : i);
        d.points.push_back({x, 0.0f, 0.0f});
    }
    d.curveVertexCounts = {uint32_t(n)};
    d.curveRadius.assign(size_t(n), radius);
    return d;
}

MeshId addCurves(Scene &scene, MeshDesc d) {
    REQUIRE(prepareCurves(d));
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, std::move(d));
    return id;
}

} // namespace

TEST_CASE("Curve bases interpolate as Embree's", "[curves]") {
    // The weights sum to 1 and their derivatives to 0; Bezier and
    // Catmull-Rom pass through their end points; B-spline at u = 0 is
    // (p0 + 4 p1 + p2) / 6.
    for (CurveBasis b : {CurveBasis::Linear, CurveBasis::Bezier, CurveBasis::BSpline, CurveBasis::CatmullRom})
        for (float u : {0.0f, 0.3f, 0.7f, 1.0f}) {
            const CurveWeights w = curveWeights(b, u);
            float sw = 0.0f, sd = 0.0f;
            for (int i = 0; i < curveSegmentPoints(b); ++i) {
                sw += w.w[i];
                sd += w.d[i];
            }
            CHECK_THAT(sw, WithinAbs(1.0, 1e-6));
            CHECK_THAT(sd, WithinAbs(0.0, 1e-5));
        }
    CHECK_THAT(curveWeights(CurveBasis::Bezier, 1.0f).w[3], WithinAbs(1.0, 1e-6));
    CHECK_THAT(curveWeights(CurveBasis::CatmullRom, 0.0f).w[1], WithinAbs(1.0, 1e-6));
    CHECK_THAT(curveWeights(CurveBasis::CatmullRom, 1.0f).w[2], WithinAbs(1.0, 1e-6));
    CHECK_THAT(curveWeights(CurveBasis::BSpline, 0.0f).w[1], WithinAbs(4.0 / 6.0, 1e-6));
    CHECK(curveSegmentCount(CurveBasis::Bezier, 7) == 2);
    CHECK(curveSegmentCount(CurveBasis::BSpline, 7) == 4);
    CHECK(curveSegmentCount(CurveBasis::Linear, 1) == 0);
}

TEST_CASE("prepareCurves builds segments and per-segment attributes", "[curves]") {
    MeshDesc d;
    d.shape = MeshShape::Curves;
    d.curveBasis = CurveBasis::Linear;
    d.points = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    d.curveVertexCounts = {3, 2};
    d.curveRadius = {0.1f};
    d.setAttribute({"id", AttributeRate::Uniform, 1, {7.0f, 9.0f}});
    d.setAttribute({"x", AttributeRate::Vertex, 1, {0.0f, 1.0f, 2.0f, 5.0f, 6.0f}});
    d.setAttribute({"vary", AttributeRate::Varying, 1, {10.0f, 11.0f, 12.0f, 20.0f, 21.0f}});
    REQUIRE(prepareCurves(d));
    CHECK(d.curveSegments == std::vector<uint32_t>{0, 1, 3});
    CHECK(d.segmentCurve == std::vector<uint32_t>{0, 0, 1});
    CHECK(d.curveRadius.size() == 5);
    CHECK(d.findAttribute("id")->data == std::vector<float>{7.0f, 7.0f, 9.0f});
    CHECK(d.findAttribute("x")->rate == AttributeRate::Corner);
    CHECK(d.findAttribute("x")->data == std::vector<float>{0, 1, 1, 1, 2, 2, 5, 6, 6});
    CHECK(d.findAttribute("vary")->data == std::vector<float>{10, 11, 11, 11, 12, 12, 20, 21, 21});
    // Counts that do not add up to the points: no segments.
    MeshDesc bad = d;
    bad.curveSegments.clear();
    bad.curveVertexCounts = {4, 2};
    CHECK_FALSE(prepareCurves(bad));
}

TEST_CASE("Rays hit round tubes of every basis", "[curves]") {
    for (CurveBasis b : {CurveBasis::Linear, CurveBasis::Bezier, CurveBasis::BSpline, CurveBasis::CatmullRom}) {
        INFO("basis " << int(b));
        Scene scene;
        addCurves(scene, straightCurve(b, 0.1f));
        scene.commit();
        // Straight down onto the axis, then 0.05 off it.
        SurfaceHit hit;
        REQUIRE(scene.intersect(Ray{{0.3f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}, 100.0f}, hit));
        CHECK_THAT(hit.t, WithinAbs(4.9, 1e-3));
        CHECK(hit.curve);
        CHECK_THAT(hit.N.z, WithinAbs(1.0, 1e-3));
        CHECK_THAT(std::fabs(hit.tangent.x), WithinAbs(1.0, 1e-4));
        CHECK_THAT(hit.curveOffset, WithinAbs(0.0, 1e-3));
        CHECK_THAT(hit.curveRadius, WithinAbs(0.1, 1e-5));
        REQUIRE(scene.intersect(Ray{{0.3f, 0.05f, 5.0f}, {0.0f, 0.0f, -1.0f}, 100.0f}, hit));
        CHECK_THAT(hit.t, WithinAbs(5.0 - std::sqrt(0.01 - 0.0025), 1e-3));
        CHECK_THAT(std::fabs(hit.curveOffset), WithinAbs(0.5, 2e-3));
        CHECK_THAT(hit.N.y, WithinAbs(0.5, 2e-3)); // the tube's normal leans towards +y
        // Past the radius: a miss.
        CHECK_FALSE(scene.intersect(Ray{{0.3f, 0.12f, 5.0f}, {0.0f, 0.0f, -1.0f}, 100.0f}, hit));
    }
}

TEST_CASE("Rays leave long round linear tubes without hitting them again", "[curves]") {
    // One linear segment 6 long and 0.01 thick (a cable): Embree's cone test
    // errs by about length^2 ulps over the radius, far beyond D4's 1e-3 r.
    // The hit lies on the tube, and rays leaving it outwards from
    // tubeRayOrigin with the hit's curveExitEps find nothing (no other
    // geometry); before, a share of them hit the same tube again.
    Scene scene;
    MeshDesc d;
    d.shape = MeshShape::Curves;
    d.curveBasis = CurveBasis::Linear;
    d.curveStyle = CurveStyle::Round;
    d.points = {{-3.0f, 0.0f, 0.0f}, {3.0f, 0.0f, 0.0f}};
    d.curveVertexCounts = {2};
    d.curveRadius = {0.01f, 0.01f};
    addCurves(scene, std::move(d));
    scene.commit();
    int found = 0, again = 0;
    for (int i = 0; i < 16; ++i) { // along the tube, away from the caps
        const float x = -2.8f + 5.6f * (float(i) + 0.5f) / 16.0f;
        for (int j = 0; j < 8; ++j) { // around it, one unit away
            const float a = kTwoPi * (float(j) + 0.25f) / 8.0f;
            const Vec3f from{x, std::cos(a), std::sin(a)};
            const Vec3f to{x + 0.1f, -0.005f * std::sin(a), 0.005f * std::cos(a)}; // half the radius across
            SurfaceHit hit;
            if (!scene.intersect(Ray{from, normalize(to - from)}, hit))
                continue;
            ++found;
            CHECK_THAT(std::sqrt(hit.P.y * hit.P.y + hit.P.z * hit.P.z), WithinAbs(0.01, 1e-6)); // on the tube
            CHECK(hit.curveExitEps > 1e-3f * hit.curveRadius);
            const Frame f = Frame::fromNormal(hit.N);
            for (int k = 0; k < 8; ++k) { // the outer hemisphere, down to 78 degrees from N
                const float c = 0.2f + 0.8f * float(k % 4) / 3.0f, s = std::sqrt(1.0f - c * c);
                const float phi = kTwoPi * (float(k) + 0.5f) / 8.0f;
                const Vec3f dir = normalize(f.toWorld({s * std::cos(phi), s * std::sin(phi), c}));
                const Vec3f org = tubeRayOrigin(hit.P, hit.N, hit.tangent, hit.curveRadius, dir, hit.curveExitEps);
                SurfaceHit back;
                again += scene.intersect(Ray{org, dir}, back) ? 1 : 0;
            }
        }
    }
    CHECK(found == 128);
    CHECK(again == 0);
    // Short segments keep D4's 1e-3 r.
    Scene shortScene;
    addCurves(shortScene, straightCurve(CurveBasis::Linear, 0.1f));
    shortScene.commit();
    SurfaceHit hit;
    REQUIRE(shortScene.intersect(Ray{{0.3f, 0.05f, 5.0f}, {0.0f, 0.0f, -1.0f}}, hit));
    CHECK(hit.curveExitEps < 1e-3f * hit.curveRadius);
    CHECK_THAT(std::sqrt(hit.P.y * hit.P.y + hit.P.z * hit.P.z), WithinAbs(0.1, 1e-6));
}

TEST_CASE("Ribbons face the ray", "[curves]") {
    Scene scene;
    addCurves(scene, straightCurve(CurveBasis::BSpline, 0.1f, CurveStyle::Flat));
    scene.commit();
    SurfaceHit hit;
    const Vec3f dir = normalize(Vec3f{0.0f, 0.6f, -1.0f});
    REQUIRE(scene.intersect(Ray{Vec3f{0.0f, 0.0f, 0.0f} - dir * 5.0f, dir, 100.0f}, hit));
    CHECK_THAT(hit.t, WithinAbs(5.0, 1e-3)); // a flat ribbon through the axis
    CHECK_THAT(dot(hit.N, dir), WithinAbs(-1.0, 1e-3));
}

TEST_CASE("Curves move and instance like meshes", "[curves]") {
    Scene scene;
    const MeshId id = addCurves(scene, straightCurve(CurveBasis::CatmullRom, 0.1f));
    scene.setMeshInstances(id, {toXform34(translation({0.0f, 2.0f, 0.0f})), toXform34(translation({0.0f, -2.0f, 0.0f}))});
    scene.commit();
    SurfaceHit hit;
    REQUIRE(scene.intersect(Ray{{0.0f, -2.0f, 5.0f}, {0.0f, 0.0f, -1.0f}, 100.0f}, hit));
    CHECK(hit.instance == 1u);
    CHECK_THAT(hit.t, WithinAbs(4.9, 1e-3));
    // Deformation motion: the curve moves up by 1 over the shutter.
    Scene moving;
    MeshDesc d = straightCurve(CurveBasis::Linear, 0.1f);
    d.pointSteps = 2;
    const std::vector<Vec3f> p0 = d.points;
    for (const Vec3f &p : p0)
        d.points.push_back(p + Vec3f{0.0f, 0.0f, 1.0f});
    addCurves(moving, d);
    moving.commit();
    REQUIRE(moving.intersect(Ray{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}, 100.0f}, hit, 0.5f));
    CHECK_THAT(hit.t, WithinAbs(4.4, 1e-3));
}

TEST_CASE("A tube's coverage is its width", "[curves]") {
    // A horizontal tube of radius 0.05 across the frame, seen from above
    // orthographically: the alpha of a pixel row band integrates to the
    // tube's width.
    Scene scene;
    MeshDesc d = straightCurve(CurveBasis::Linear, 0.05f);
    for (Vec3f &p : d.points)
        p.x *= 3.0f;
    addCurves(scene, d);
    scene.commit();
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = 256;
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 0.5f, 8, 16));
    renderer.resize(8, 16);
    renderToConvergence(renderer);
    const Film &film = renderer.film();
    // Pixels are 1/8 wide and 1/16 tall: a column's alpha sum times the
    // pixel height is the covered height.
    double covered = 0.0;
    for (int y = 0; y < 16; ++y) {
        const size_t i = size_t(y) * 8 + 3;
        covered += film.alphaSum[i] / film.count[i] * (1.0 / 16.0);
    }
    CHECK_THAT(covered, WithinAbs(0.1, 0.004));
}

TEST_CASE("Curves report the curve as their element", "[curves]") {
    // Three vertical linear curves in one prim (Karma's geo_ids): the face
    // AOV (ray:primid, ray:element) is the curve's index, not the segment's.
    Scene scene;
    MeshDesc d;
    d.shape = MeshShape::Curves;
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 3; ++i)
            d.points.push_back({-0.5f + 0.5f * float(c), -0.45f + 0.45f * float(i), 0.0f});
    d.curveVertexCounts = {3, 3, 3};
    d.curveRadius.assign(9, 0.05f);
    addCurves(scene, d);
    scene.commit();
    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = 1;
    settings.maxDepth = 1;
    settings.utility = std::make_shared<const std::vector<UtilityRequest>>(
        std::vector<UtilityRequest>{{UtilityKind::Face, {}}});
    renderer.setScene(&scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 5.0f}, 1.0f, 40, 40));
    renderer.resize(40, 40);
    renderToConvergence(renderer);
    const UtilityPlane &p = renderer.film().utility[0];
    for (int c = 0; c < 3; ++c) {
        const int x = 10 + 10 * c, y = 20; // x = -0.5, 0, 0.5 at the centre row
        INFO("curve " << c);
        CHECK(p.data[size_t(y) * 40 + size_t(x)] == float(c));
    }
}