// Ray queries from shading (docs/design/M4.md 3.5, M4.4) against the
// analytic results of Karma's measurements (docs/research/m4/karma-nodes.md
// 4.3-4.6): ambient occlusion between two parallel planes (cosine law, cone),
// distance (ln 2), curvature at a cube edge and on a sphere, and the rounded
// edge profile.

#include "test_helpers.h"

#include "core/integrator/material_eval.h"
#include "core/integrator/shading_queries.h"
#include "core/scene/scene.h"
#include "core/shading/compiler.h"
#include "core/shading/vm.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <functional>
#include <vector>

using namespace nova;
using namespace nova::test;
namespace sh = nova::shading;
namespace sf = nova::surface_field;
using Catch::Matchers::WithinAbs;

namespace {

// A box [-h, h]^3 with flat faces, outward winding.
MeshDesc makeBox(float h) {
    MeshDesc m;
    const Vec3f n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const Vec3f &f : n) {
        // Two tangents with u x v = f.
        const Vec3f u = std::fabs(f.x) > 0.5f ? Vec3f{0, 1, 0} : Vec3f{1, 0, 0};
        const Vec3f v = cross(f, u);
        const Vec3f c = f * h;
        const uint32_t base = uint32_t(m.points.size());
        m.points.push_back(c - u * h - v * h);
        m.points.push_back(c + u * h - v * h);
        m.points.push_back(c + u * h + v * h);
        m.points.push_back(c - u * h + v * h);
        // Winding: (p1 - p0) x (p2 - p0) = (2hu) x (2hu + 2hv) ~ u x v
        const bool flip = dot(cross(u, v), f) < 0.0f;
        if (flip)
            m.indices.insert(m.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
        else
            m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return m;
}

MeshId addMesh(Scene &scene, MeshDesc m) {
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, std::move(m));
    return id;
}

// Mean of query output `k` over `lanes` lanes at the same hit with
// stratified random numbers.
double meanQuery(const Scene &scene, QueryKind kind, const QueryLane &at, Vec3f direction, const float params[4],
                 int k, int lanes = 2048, const std::function<double(const float *)> &reduce = {}) {
    const SceneShadingQueries queries(scene, 0);
    double sum = 0.0;
    QueryLane chunk[32];
    float dir[32 * 3], par[32 * 4], out[32 * 4];
    for (int base = 0; base < lanes; base += 32) {
        for (int l = 0; l < 32; ++l) {
            const int i = base + l;
            chunk[l] = at;
            const float a = (float(i) + 0.5f) / float(lanes);
            const float b = float(i) * 0.6180340f, c = float(i) * 0.4142136f;
            chunk[l].rnd = {a, b - std::floor(b), c - std::floor(c)};
            dir[l * 3] = direction.x;
            dir[l * 3 + 1] = direction.y;
            dir[l * 3 + 2] = direction.z;
            for (int p = 0; p < 4; ++p)
                par[l * 4 + p] = params[p];
        }
        queries.run(kind, 32, chunk, dir, par, out);
        for (int l = 0; l < 32; ++l)
            sum += reduce ? reduce(out + l * 4) : double(out[l * 4 + k]);
    }
    return sum / double(lanes);
}

// The floor z = 0 (normal +z) under a ceiling at z = 1.
struct Planes {
    Scene scene;
    MeshId floor;
    Planes() {
        floor = addMesh(scene, makeQuad(100.0f));
        MeshDesc ceiling = makeQuad(100.0f);
        ceiling.transform = translation({0.0f, 0.0f, 1.0f});
        addMesh(scene, std::move(ceiling));
        scene.commit();
    }
    QueryLane lane() const {
        QueryLane l{};
        l.P = {0.0f, 0.0f, 0.0f};
        l.N = l.Ng = {0.0f, 0.0f, 1.0f};
        l.I = {0.0f, 0.0f, -1.0f};
        l.mesh = floor;
        return l;
    }
};

} // namespace

TEST_CASE("Ambient occlusion follows Karma's cosine-weighted cone", "[queries]") {
    Planes planes;
    const QueryLane at = planes.lane();
    auto ao = [&](float cone, float maxDistance, float samples = 16.0f, Vec3f n = {0.0f, 0.0f, 0.0f},
                  float front = 1.0f) {
        const float p[4] = {cone, maxDistance, samples, front};
        return meanQuery(planes.scene, QueryKind::Occlusion, at, n, p, 0);
    };
    // Occluded iff the ray's cosine exceeds h / maxdistance: visible c^2.
    CHECK_THAT(ao(90.0f, 2.0f), WithinAbs(0.25, 0.01));
    CHECK_THAT(ao(90.0f, 4.0f), WithinAbs(0.0625, 0.01));
    CHECK_THAT(ao(90.0f, 0.9f), WithinAbs(1.0, 1e-6));
    // Cone half angle a: (c^2 - cos^2 a) / (1 - cos^2 a).
    CHECK_THAT(ao(60.0f, 4.0f / 3.0f), WithinAbs(5.0 / 12.0, 0.01));
    CHECK_THAT(ao(30.0f, 4.0f), WithinAbs(0.0, 1e-6));
    // One sample per hit is stratified across the hits.
    CHECK_THAT(ao(90.0f, 2.0f, 1.0f), WithinAbs(0.25, 0.01));
    // A connected 45-degree normal: rays below the floor see past it
    // (Karma 0.4603).
    const float s = std::sqrt(0.5f);
    CHECK_THAT(ao(90.0f, 2.0f, 16.0f, {s, 0.0f, s}), WithinAbs(0.4608, 0.01));
    // Front face flips a normal pointing away from the viewer.
    CHECK_THAT(ao(90.0f, 2.0f, 16.0f, {0.0f, 0.0f, -1.0f}), WithinAbs(0.25, 0.01));
}

TEST_CASE("Distance reads the mean or the nearest hit", "[queries]") {
    Planes planes;
    const QueryLane at = planes.lane();
    auto dist = [&](int mode, float samples, bool normalize = true) {
        const float p[4] = {90.0f, 2.0f, samples, float(mode | (normalize ? 2 : 0) | 4)};
        return meanQuery(planes.scene, QueryKind::Distance, at, {0.0f, 0.0f, 0.0f}, p, 0);
    };
    // Uniform hemisphere, mean of L / D over the hits: ln 2 (Karma 0.6928).
    CHECK_THAT(dist(1, 64.0f), WithinAbs(std::log(2.0), 0.01));
    // One sample: misses count as 1, 0.5 + 0.5 ln 2 (Karma 0.8459).
    CHECK_THAT(dist(1, 1.0f), WithinAbs(0.5 + 0.5 * std::log(2.0), 0.01));
    CHECK_THAT(dist(1, 64.0f, false), WithinAbs(2.0 * std::log(2.0), 0.02));
    // The minimum tends to h / D = 0.5.
    const double nearest = dist(0, 64.0f);
    CHECK(nearest > 0.5);
    CHECK(nearest < 0.53);
    // The nearest hit's direction points up.
    const float p[4] = {90.0f, 2.0f, 64.0f, 6.0f}; // nearest, normalized, front face
    const double up = meanQuery(planes.scene, QueryKind::Distance, at, {0.0f, 0.0f, 0.0f}, p, 3);
    CHECK(up > 0.9);
}

TEST_CASE("Curvature matches the edge and sphere formulas", "[queries]") {
    // Convex: P on the top face of a unit cube at distance t from an edge,
    // d = 0.1, a = t / d: convex = (1 - a + a ln a) / 2 (Karma's 0.612,
    // 0.544, 0.511 in the 0..1 space).
    Scene scene;
    const MeshId box = addMesh(scene, makeBox(0.5f));
    scene.commit();
    for (const float t : {0.022f, 0.047f, 0.072f}) {
        QueryLane at{};
        at.P = {0.5f - t, 0.0f, 0.5f};
        at.N = at.Ng = {0.0f, 0.0f, 1.0f};
        at.I = {0.0f, 0.0f, -1.0f};
        at.mesh = box;
        const float p[4] = {0.1f, 64.0f, 0.0f, 0.0f};
        const double a = t / 0.1;
        const double convex = 0.5 * ((1.0 - a) + a * std::log(a));
        CAPTURE(t);
        CHECK_THAT(meanQuery(scene, QueryKind::Curvature, at, {0.0f, 0.0f, 0.0f}, p, 0), WithinAbs(convex, 0.01));
        CHECK_THAT(meanQuery(scene, QueryKind::Curvature, at, {0.0f, 0.0f, 0.0f}, p, 1), WithinAbs(0.0, 1e-6));
    }

    // Concave: the floor at distance t from a wall.
    Scene inner;
    const MeshId floor = addMesh(inner, makeQuad(10.0f));
    MeshDesc wall = makeQuad(10.0f); // the plane x = 0.3 above the floor, normal -x
    wall.transform = Mat4::identity();
    wall.transform.m[0][0] = 0.0f;
    wall.transform.m[0][2] = 1.0f; // x axis -> z
    wall.transform.m[2][2] = 0.0f;
    wall.transform.m[2][0] = -1.0f; // z axis -> -x
    wall.transform.m[3][0] = 0.3f;
    wall.transform.m[3][2] = 10.0f;
    addMesh(inner, std::move(wall));
    inner.commit();
    QueryLane at{};
    at.P = {0.3f - 0.047f, 0.0f, 0.0f};
    at.N = at.Ng = {0.0f, 0.0f, 1.0f};
    at.I = {0.0f, 0.0f, -1.0f};
    at.mesh = floor;
    const float p[4] = {0.1f, 64.0f, 0.0f, 0.0f};
    const double a = 0.47;
    CHECK_THAT(meanQuery(inner, QueryKind::Curvature, at, {0.0f, 0.0f, 0.0f}, p, 1),
               WithinAbs(0.5 * ((1.0 - a) + a * std::log(a)), 0.01));
    CHECK_THAT(meanQuery(inner, QueryKind::Curvature, at, {0.0f, 0.0f, 0.0f}, p, 0), WithinAbs(0.0, 1e-6));

    // A solid sphere of radius R, x = d / R <= 2: convex x / 4 (the chord
    // 2 R cos); Karma reads lower for small x (karma-nodes.md 4.5).
    Scene ball;
    MeshDesc sphere;
    sphere.shape = MeshShape::Sphere;
    sphere.points = {{0.0f, 0.0f, 0.0f}};
    sphere.sphereRadius = {1.0f};
    const MeshId sid = addMesh(ball, std::move(sphere));
    ball.commit();
    for (const float x : {0.5f, 1.0f, 2.0f}) {
        QueryLane on{};
        on.P = {0.0f, 0.0f, 1.0f};
        on.N = on.Ng = {0.0f, 0.0f, 1.0f};
        on.I = {0.0f, 0.0f, -1.0f};
        on.mesh = sid;
        const float q[4] = {x, 64.0f, 0.0f, 0.0f};
        CAPTURE(x);
        CHECK_THAT(meanQuery(ball, QueryKind::Curvature, on, {0.0f, 0.0f, 0.0f}, q, 0), WithinAbs(x / 4.0, 0.01));
    }
}

TEST_CASE("Rounded edges bend the normal like Karma's profile", "[queries]") {
    Scene scene;
    const MeshId box = addMesh(scene, makeBox(0.5f));
    scene.commit();
    auto at = [&](float t) {
        QueryLane l{};
        l.P = {0.5f - t, 0.0f, 0.5f};
        l.N = l.Ng = {0.0f, 0.0f, 1.0f};
        l.I = {0.0f, 0.0f, -1.0f};
        l.mesh = box;
        return l;
    };
    // The bend towards the side face (+x), in degrees, and the mask.
    auto angle = [](const float *o) { return std::atan2(double(o[0]), double(o[2])) * 180.0 / 3.14159265358979; };
    auto bend = [&](float t, float radius, int mode = 0) {
        const float p[4] = {radius, 256.0f, float(mode), 30.0f};
        return meanQuery(scene, QueryKind::RoundedEdge, at(t), {0.0f, 0.0f, 0.0f}, p, 0, 256, angle);
    };
    auto mask = [&](float t, float radius, int mode = 0) {
        const float p[4] = {radius, 16.0f, float(mode), 30.0f};
        return meanQuery(scene, QueryKind::RoundedEdge, at(t), {0.0f, 0.0f, 0.0f}, p, 3, 256);
    };
    // Karma (radius 0.1): 0.3, 7.9, 19.9, 33.8 degrees at t = 0.097 ...
    // 0.022; Nova's weight 1 - (d / r)^2 gives 0.1, 6.8, 19.5, 33.4.
    CHECK_THAT(bend(0.097f, 0.1f), WithinAbs(0.3, 1.0));
    CHECK_THAT(bend(0.072f, 0.1f), WithinAbs(7.9, 2.0));
    CHECK_THAT(bend(0.047f, 0.1f), WithinAbs(19.9, 2.0));
    CHECK_THAT(bend(0.022f, 0.1f), WithinAbs(33.8, 2.0));
    // Radius 0.05 at t = 0.022: Karma 21.8 degrees.
    CHECK_THAT(bend(0.022f, 0.05f), WithinAbs(21.8, 2.0));
    // Symmetric at the edge itself.
    CHECK_THAT(bend(0.0005f, 0.1f), WithinAbs(45.0, 2.5));
    // Mask: 1 near the edge, 0 beyond the radius (normal unchanged).
    CHECK(mask(0.022f, 0.1f) > 0.99);
    CHECK(mask(0.15f, 0.1f) == 0.0);
    CHECK_THAT(bend(0.15f, 0.1f), WithinAbs(0.0, 1e-6));
    // Mode 1 rounds concave edges only: this convex edge stays sharp.
    CHECK(mask(0.022f, 0.1f, 1) == 0.0);
    CHECK_THAT(bend(0.022f, 0.1f, 1), WithinAbs(0.0, 1e-6));
    CHECK(mask(0.022f, 0.1f, 2) > 0.99);
}

TEST_CASE("Query ops run per chunk and default without a scene", "[queries][shading]") {
    // visibility = AO(maxdistance 2) as the emission of an unlit surface.
    sh::IrGraph g;
    g.model = SurfaceModel::Unlit;
    sh::IrBuilder b(g);
    const sh::Ref q = b.query(QueryKind::Occlusion, b.global(sh::Global::N), b.constant(90.0f, 2.0f, 16.0f));
    b.output(sf::emissionColor, 3, q);
    const sh::CompileResult r = sh::compile(g);
    REQUIRE(r.program);
    CHECK(r.program->needsQueries);
    // Constant operands do not fold the query away.
    bool found = false;
    for (const sh::Instr &in : r.program->code)
        found |= in.op == sh::Op::Query;
    CHECK(found);

    Planes planes;
    MaterialDesc d;
    d.inputs = unlitDefaults();
    d.program = r.program;
    const MaterialId m = planes.scene.createMaterial();
    planes.scene.updateMaterial(m, d);
    planes.scene.setMeshMaterials(planes.floor, {m});
    planes.scene.commit();

    SurfaceHit hits[32];
    Vec3f dirs[32], rnd[32];
    float times[32];
    for (int l = 0; l < 32; ++l) {
        hits[l].P = {float(l) * 0.1f, 0.0f, 0.0f};
        hits[l].N = hits[l].Ng = {0.0f, 0.0f, 1.0f};
        hits[l].mesh = planes.floor;
        hits[l].prim = 0;
        hits[l].t = 1.0f;
        dirs[l] = {0.0f, 0.0f, -1.0f};
        times[l] = 0.0f;
    }
    const SurfaceInputs *inputs[32];
    MaterialEvalScratch scratch;
    // Without queries: the default, fully visible.
    evaluateSurfaceInputs(planes.scene, m, hits, dirs, times, 32, inputs, scratch);
    for (int l = 0; l < 32; ++l)
        CHECK(inputs[l]->c(sf::emissionColor).x == 1.0f);
    // With the scene's queries: c^2 = 0.25 on average.
    const SceneShadingQueries queries(planes.scene, 0);
    double sum = 0.0;
    const int rounds = 64;
    for (int round = 0; round < rounds; ++round) {
        for (int l = 0; l < 32; ++l) {
            const float a = (float(round * 32 + l) + 0.5f) / float(rounds * 32), c = float(l) * 0.618034f;
            rnd[l] = {a, c - std::floor(c), 0.5f};
        }
        HitRayData ray;
        ray.queries = &queries;
        ray.queryRnd = rnd;
        evaluateSurfaceInputs(planes.scene, m, hits, dirs, times, 32, inputs, scratch, ShadeGlobals{},
                              HitTextureData{}, ray);
        for (int l = 0; l < 32; ++l)
            sum += inputs[l]->c(sf::emissionColor).x;
    }
    CHECK_THAT(sum / (32.0 * rounds), WithinAbs(0.25, 0.01));
}

TEST_CASE("Renders trace shading queries", "[queries][render]") {
    // An orthographic camera between the planes looks down at the floor,
    // whose emission is its AO visibility (maxdistance 2): 0.25.
    Planes planes;
    sh::IrGraph g;
    g.model = SurfaceModel::Unlit;
    sh::IrBuilder b(g);
    b.output(sf::emissionColor, 3,
             b.query(QueryKind::Occlusion, b.global(sh::Global::N), b.constant(90.0f, 2.0f, 1.0f)));
    MaterialDesc d;
    d.inputs = unlitDefaults();
    d.program = sh::compile(g).program;
    REQUIRE(d.program);
    const MaterialId m = planes.scene.createMaterial();
    planes.scene.updateMaterial(m, d);
    planes.scene.setMeshMaterials(planes.floor, {m});
    planes.scene.commit();

    Renderer renderer;
    RenderSettings settings;
    settings.maxSamples = 64;
    settings.maxDepth = 1;
    settings.pixelFilterScale = 0.0f;
    renderer.setScene(&planes.scene);
    renderer.setSettings(settings);
    renderer.setCamera(makeOrthoCamera({0.0f, 0.0f, 0.5f}, 0.5f, 8, 8));
    renderer.resize(8, 8);
    renderToConvergence(renderer);
    double sum = 0.0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            sum += pixelMean(renderer.film(), x, y).y;
    CHECK_THAT(sum / 64.0, WithinAbs(0.25, 0.01));
}
