// docs/design/M10.md D3: the hit reconstruction shared by the CPU and the
// GPU (core/scene/hit.h) gives bit-identical results on the CPU's records
// and on plain-array views of them (core/scene/scene_view.h), for every
// shape: triangles (static, instanced, moving, deforming), analytic spheres,
// curves of every basis and style, points of every style.

#include "test_helpers.h"

#include "core/geometry/curves.h"
#include "core/math/rng.h"
#include "core/math/sampling.h"
#include "core/scene/scene.h"
#include "core/scene/scene_view.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

using namespace nova;
using namespace nova::test;

namespace {

bool same(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }
bool same(Vec3f a, Vec3f b) { return same(a.x, b.x) && same(a.y, b.y) && same(a.z, b.z); }
bool same(Vec2f a, Vec2f b) { return same(a.x, b.x) && same(a.y, b.y); }

bool sameHit(const SurfaceHit &a, const SurfaceHit &b) {
    return same(a.t, b.t) && same(a.P, b.P) && same(a.Ng, b.Ng) && same(a.N, b.N) && same(a.uv, b.uv) &&
           same(a.Pobject, b.Pobject) && same(a.Nobject, b.Nobject) && same(a.dudP, b.dudP) && same(a.dvdP, b.dvdP) &&
           same(a.extent, b.extent) && a.mesh == b.mesh && a.instance == b.instance && a.prim == b.prim &&
           a.curve == b.curve && same(a.tangent, b.tangent) && same(a.curveRadius, b.curveRadius) &&
           same(a.curveOffset, b.curveOffset) && same(a.strandRandom, b.strandRandom);
}

MeshDesc curveMesh(CurveBasis basis, CurveStyle style, Rng &rng) {
    MeshDesc d;
    d.shape = MeshShape::Curves;
    d.curveBasis = basis;
    d.curveStyle = style;
    for (int c = 0; c < 40; ++c) {
        const Vec3f root{4.0f * rng.next() - 2.0f, 4.0f * rng.next() - 2.0f, 0.0f};
        for (int i = 0; i < 7; ++i) {
            d.points.push_back(root + Vec3f{0.1f * (rng.next() - 0.5f), 0.1f * (rng.next() - 0.5f), 0.25f * float(i)});
            d.normals.push_back(normalize(Vec3f{rng.next() - 0.5f, rng.next() - 0.5f, 0.1f}));
            d.curveRadius.push_back(0.03f + 0.02f * rng.next());
        }
        d.curveVertexCounts.push_back(7);
    }
    REQUIRE(prepareCurves(d));
    return d;
}

MeshDesc pointMesh(PointStyle style, Rng &rng) {
    MeshDesc d;
    d.shape = MeshShape::Points;
    d.pointStyle = style;
    for (int i = 0; i < 300; ++i) {
        d.points.push_back({4.0f * rng.next() - 2.0f, 4.0f * rng.next() - 2.0f, 1.5f * rng.next()});
        d.normals.push_back(normalize(Vec3f{rng.next() - 0.5f, rng.next() - 0.5f, 0.5f}));
        d.pointRadius.push_back(0.05f + 0.05f * rng.next());
    }
    return d;
}

} // namespace

TEST_CASE("Hits on scene views equal hits on the records", "[scene][view]") {
    Scene scene;
    Rng rng = Rng::make(10u, 0u, 3u);
    // Triangles: a static smooth sphere, an instanced quad field, a moving
    // and a deforming mesh.
    MeshDesc ball = makeSphere(0.8f, 24, 12);
    ball.transform = translation({-1.0f, 0.5f, 0.6f});
    scene.updateMesh(scene.createMesh(), ball);
    const MeshId field = scene.createMesh();
    scene.updateMesh(field, makeQuad(0.3f));
    std::vector<Xform34> inst;
    for (int i = 0; i < 16; ++i) {
        Mat4 m = translation({float(i % 4) - 1.5f, float(i / 4) - 1.5f, -0.2f});
        inst.push_back(toXform34(m));
    }
    scene.setMeshInstances(field, inst);
    const MeshId mover = scene.createMesh();
    scene.updateMesh(mover, makeSphere(0.4f, 16, 8));
    scene.updateMeshMotion(mover, {translation({1.0f, -1.0f, 0.4f}), translation({1.4f, -0.8f, 0.6f})});
    MeshDesc blob = makeSphere(0.3f, 16, 8);
    const size_t np = blob.points.size();
    for (size_t i = 0; i < np; ++i)
        blob.points.push_back(blob.points[i] * 1.3f);
    blob.normals.insert(blob.normals.end(), blob.normals.begin(), blob.normals.end());
    blob.pointSteps = 2;
    blob.transform = translation({1.2f, 1.2f, 0.5f});
    scene.updateMesh(scene.createMesh(), blob);
    // An analytic sphere, curves of every basis and style, points.
    MeshDesc analytic;
    analytic.shape = MeshShape::Sphere;
    analytic.points = {{0.0f, 0.0f, 0.0f}};
    analytic.sphereRadius = {0.5f};
    analytic.transform = translation({0.0f, 0.0f, 1.2f});
    scene.updateMesh(scene.createMesh(), analytic);
    for (const CurveBasis basis : {CurveBasis::Linear, CurveBasis::Bezier, CurveBasis::BSpline, CurveBasis::CatmullRom})
        for (const CurveStyle style : {CurveStyle::Flat, CurveStyle::Round, CurveStyle::Oriented})
            scene.updateMesh(scene.createMesh(), curveMesh(basis, style, rng));
    for (const PointStyle style : {PointStyle::Sphere, PointStyle::Disc, PointStyle::OrientedDisc})
        scene.updateMesh(scene.createMesh(), pointMesh(style, rng));
    scene.commit();

    std::vector<MeshRecordView> views;
    for (MeshId id = 0; id < scene.meshCount(); ++id)
        views.push_back(meshRecordView(scene.meshRecord(id)));

    int hits = 0;
    for (int k = 0; k < 40000; ++k) {
        const Vec3f o{6.0f * rng.next() - 3.0f, 6.0f * rng.next() - 3.0f, 3.0f};
        const Vec3f d = normalize(Vec3f{0.6f * (rng.next() - 0.5f), 0.6f * (rng.next() - 0.5f), -1.0f});
        float t = kInf, time = rng.next();
        const float seed = 0.0f;
        const uint8_t type = 0;
        uint32_t mesh = kInvalidId, prim = 0, instance = 0;
        Vec2f uv{0.0f, 0.0f};
        HitBatch hb{&t, &mesh, &prim, &uv, &instance};
        scene.accel().intersect(RayBatch{&o, &d, &t, 1, &time, &seed, &type, 0}, hb);
        if (mesh == kInvalidId)
            continue;
        ++hits;
        const SurfaceHit a = scene.resolveHit(mesh, prim, uv, t, instance, time, o, d);
        const SurfaceHit b = resolveMeshHit(views[mesh], mesh, prim, uv, t, instance, time, o, d);
        INFO("ray " << k << " mesh " << mesh << " prim " << prim);
        REQUIRE(sameHit(a, b));
        REQUIRE(same(scene.curvature(a, time), meshCurvature(views[mesh], b, time)));
    }
    CHECK(hits > 10000);
}
