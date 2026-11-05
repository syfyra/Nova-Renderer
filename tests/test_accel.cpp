#include "test_helpers.h"

#include "core/accel/accel.h"
#include "core/scene/motion.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

using namespace nova;
using namespace nova::test;
using Catch::Matchers::WithinAbs;

TEST_CASE("Scene intersects instanced meshes in world space", "[accel]") {
    Scene scene;
    const MeshId id = scene.createMesh();
    MeshDesc sphere = makeSphere(1.0f, 64, 32);
    sphere.transform = translation({0.0f, 0.0f, -5.0f});
    scene.updateMesh(id, sphere);
    CHECK(scene.commit());

    SurfaceHit hit;
    REQUIRE(scene.intersect(Ray{{0, 0, 0}, {0, 0, -1}}, hit));
    CHECK_THAT(hit.t, WithinAbs(4.0, 1e-2));
    CHECK_THAT(hit.P.z, WithinAbs(-4.0, 1e-2));
    CHECK_THAT(hit.N.z, WithinAbs(1.0, 1e-2));
    CHECK_THAT(std::fabs(hit.Ng.z), WithinAbs(1.0, 1e-2));
    CHECK(hit.mesh == id);
    CHECK_FALSE(scene.intersect(Ray{{0, 0, 0}, {0, 0, 1}}, hit));
    CHECK(scene.occluded(Ray{{0, 0, 0}, {0, 0, -1}}));
    CHECK_FALSE(scene.occluded(Ray{{0, 0, 0}, {0, 0, -1}, 0.0f, 3.0f})); // tMax before the sphere

    SECTION("transform edits move the instance") {
        scene.updateMeshTransform(id, translation({0.0f, 0.0f, -10.0f}));
        scene.commit();
        REQUIRE(scene.intersect(Ray{{0, 0, 0}, {0, 0, -1}}, hit));
        CHECK_THAT(hit.t, WithinAbs(9.0, 1e-2));
    }
    SECTION("hidden meshes are not hit") {
        scene.setMeshVisible(id, false);
        scene.commit();
        CHECK_FALSE(scene.intersect(Ray{{0, 0, 0}, {0, 0, -1}}, hit));
        scene.setMeshVisible(id, true);
        scene.commit();
        CHECK(scene.intersect(Ray{{0, 0, 0}, {0, 0, -1}}, hit));
    }
    SECTION("removed meshes free their id after commit") {
        scene.removeMesh(id);
        scene.commit();
        CHECK_FALSE(scene.intersect(Ray{{0, 0, 0}, {0, 0, -1}}, hit));
        CHECK(scene.createMesh() == id);
    }
}

TEST_CASE("Corner attributes are interpolated at the hit", "[accel][attributes]") {
    Scene scene;
    MeshDesc quad = makeQuad(1.0f);
    // Triangle 0 (0, 1, 2): red, green, blue corners.
    quad.setAttribute({"displayColor", AttributeRate::Corner, 3,
                       {1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}});
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, quad);
    scene.commit();

    SurfaceHit hit;
    // Centroid of triangle 0: (-1 + 1 + 1, -1 - 1 + 1) / 3.
    REQUIRE(scene.intersect(Ray{{1.0f / 3.0f, -1.0f / 3.0f, 1.0f}, {0, 0, -1}}, hit));
    REQUIRE(hit.prim == 0);
    const AttributeRef *refs = scene.attributeRefs(id, kDefaultMaterialId); // the default material reads displayColor
    REQUIRE(refs);
    float c[3];
    REQUIRE(fetchAttribute(refs[0], &quad.indices[0], hit.prim, hit.instance, hit.uv.x, hit.uv.y, 3, c));
    CHECK_THAT(c[0], WithinAbs(1.0 / 3.0, 1e-4));
    CHECK_THAT(c[1], WithinAbs(1.0 / 3.0, 1e-4));
    CHECK_THAT(c[2], WithinAbs(1.0 / 3.0, 1e-4));
}

TEST_CASE("Batched queries match single-ray queries", "[accel]") {
    Scene scene;
    const MeshId id = scene.createMesh();
    scene.updateMesh(id, makeQuad(1.0f));
    scene.commit();

    const Vec3f org[3] = {{0.0f, 0.0f, 1.0f}, {5.0f, 0.0f, 1.0f}, {0.5f, 0.5f, 2.0f}};
    const Vec3f dir[3] = {{0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, -1.0f}};
    const float tMax[3] = {kInf, kInf, 1.0f};
    float t[3];
    uint32_t mesh[3], prim[3];
    Vec2f uv[3];
    const RayBatch rays{org, dir, tMax, 3};
    HitBatch hits{t, mesh, prim, uv};
    scene.accel().intersect(rays, hits);
    CHECK(mesh[0] == id);
    CHECK_THAT(t[0], WithinAbs(1.0, 1e-5));
    CHECK(mesh[1] == kInvalidId); // misses the quad
    CHECK(mesh[2] == kInvalidId); // stops before the quad

    uint8_t blocked[3];
    scene.accel().occluded(rays, blocked);
    CHECK(blocked[0] == 1);
    CHECK(blocked[1] == 0);
    CHECK(blocked[2] == 0);
}

namespace {

// n x n unit quads in z = 0 (two triangles each, prim 2k and 2k + 1), with a
// second deformation step lifted by `lift`; `oddOnly` keeps the odd prims.
MeshDesc filterGrid(int n, bool oddOnly, float lift) {
    MeshDesc m;
    for (int y = 0; y <= n; ++y)
        for (int x = 0; x <= n; ++x)
            m.points.push_back({float(x), float(y), 0.0f});
    if (lift != 0.0f) {
        const size_t count = m.points.size();
        for (size_t i = 0; i < count; ++i)
            m.points.push_back(m.points[i] + Vec3f{0.0f, 0.0f, lift});
        m.pointSteps = 2;
    }
    uint32_t prim = 0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const uint32_t a = uint32_t(y * (n + 1) + x), b = a + 1, c = a + uint32_t(n) + 1, d = c + 1;
            for (const std::array<uint32_t, 3> &tri : {std::array<uint32_t, 3>{a, b, d}, std::array<uint32_t, 3>{a, d, c}}) {
                if (!oddOnly || prim % 2 == 1)
                    m.indices.insert(m.indices.end(), tri.begin(), tri.end());
                ++prim;
            }
        }
    return m;
}

// Rejects the even prims; counts its calls.
struct EvenPrimFilter final : IHitFilter {
    mutable std::atomic<int> calls{0};
    mutable std::atomic<int> occlusionCalls{0};
    bool accept(const HitCandidate &hit, const RayBatch &rays) const override {
        ++calls;
        if (hit.occlusion)
            ++occlusionCalls;
        return hit.ray < rays.count && hit.prim % 2 == 1;
    }
};

} // namespace

// M3.0 spike S2 (docs/design/M3.md K10): hits rejected by the hit filter
// vanish exactly as if the geometry were not there, for plain, instanced,
// moving and deforming meshes, in closest-hit and occlusion queries; meshes
// without filtering never call it.
TEST_CASE("The hit filter removes hits like removed geometry", "[accel][filter]") {
    enum class Kind { Plain, Instances, Moving, Deforming };
    for (const Kind kind : {Kind::Plain, Kind::Instances, Kind::Moving, Kind::Deforming}) {
        INFO("kind " << int(kind));
        const float lift = kind == Kind::Deforming ? 0.5f : 0.0f;
        const MeshDesc full = filterGrid(8, false, lift), odd = filterGrid(8, true, lift);
        std::vector<Xform34> xforms;
        std::vector<SrtTransform> keys;
        for (int i = 0; i < 3; ++i)
            xforms.push_back(toXform34(translation({0.0f, 0.0f, -3.0f * float(i)})));
        for (int s = 0; s < 2; ++s)
            for (int i = 0; i < 3; ++i)
                keys.push_back(decomposeSrt(translation({0.25f * float(s), 0.0f, -3.0f * float(i)})));

        EvenPrimFilter filter;
        std::unique_ptr<IAccel> filtered = createEmbreeAccel(), reference = createEmbreeAccel();
        // A second, unfiltered mesh beside the grid: its hits never reach the filter.
        MeshDesc other = filterGrid(2, false, 0.0f);
        for (Vec3f &p : other.points)
            p.x += 20.0f;
        for (IAccel *a : {filtered.get(), reference.get()}) {
            a->setMesh(0, a == filtered.get() ? full : odd);
            if (kind == Kind::Instances)
                a->setInstances(0, xforms.data(), 3);
            else if (kind == Kind::Moving)
                a->setInstanceMotion(0, keys.data(), 3, 2);
            else
                a->setInstances(0, xforms.data(), 1);
            a->setMesh(1, other);
            a->setInstances(1, xforms.data(), 1);
        }
        filtered->setMeshFiltered(0, true);
        filtered->setHitFilter(&filter);
        filtered->commit();
        reference->commit();

        const uint32_t n = 4000;
        std::vector<Vec3f> org(n), dir(n);
        std::vector<float> tMax(n), time(n);
        uint32_t state = 12345u;
        auto random = [&] {
            state = state * 1664525u + 1013904223u;
            return float(state >> 8) / 16777216.0f;
        };
        for (uint32_t i = 0; i < n; ++i) {
            const bool side = i % 10 == 0; // some rays aim at the unfiltered mesh
            org[i] = {side ? 20.0f + 2.0f * random() : -0.5f + 9.0f * random(), -0.5f + 9.0f * random(), 2.0f};
            dir[i] = normalize(Vec3f{0.2f * (random() - 0.5f), 0.2f * (random() - 0.5f), -1.0f});
            tMax[i] = i % 3 == 0 ? 5.0f : kInf;
            time[i] = random();
        }
        RayBatch rays{org.data(), dir.data(), tMax.data(), n, time.data()};
        std::vector<float> tA(n), tB(n);
        std::vector<uint32_t> meshA(n), meshB(n), primA(n), primB(n), instA(n), instB(n);
        std::vector<Vec2f> uvA(n), uvB(n);
        HitBatch a{tA.data(), meshA.data(), primA.data(), uvA.data(), instA.data()};
        HitBatch b{tB.data(), meshB.data(), primB.data(), uvB.data(), instB.data()};
        filtered->intersect(rays, a);
        reference->intersect(rays, b);
        int mismatches = 0, hits = 0;
        for (uint32_t i = 0; i < n; ++i) {
            if (meshA[i] != meshB[i]) {
                ++mismatches;
                continue;
            }
            if (meshA[i] == kInvalidId)
                continue;
            ++hits;
            const uint32_t expectedPrim = meshA[i] == 0 ? 2 * primB[i] + 1 : primB[i];
            mismatches += primA[i] != expectedPrim || instA[i] != instB[i] || tA[i] != tB[i] ||
                          uvA[i].x != uvB[i].x || uvA[i].y != uvB[i].y;
        }
        CHECK(hits > int(n) / 4);
        CHECK(mismatches == 0);
        CHECK(filter.calls > 0);
        CHECK(filter.occlusionCalls == 0);

        std::vector<uint8_t> occA(n), occB(n);
        filtered->occluded(rays, occA.data());
        reference->occluded(rays, occB.data());
        CHECK(occA == occB);
        CHECK(filter.occlusionCalls > 0);

        // Without a filter every hit counts again; unfiltered meshes never call it.
        filtered->setHitFilter(nullptr);
        filtered->intersect(rays, a);
        int evenHits = 0;
        for (uint32_t i = 0; i < n; ++i)
            evenHits += meshA[i] == 0 && primA[i] % 2 == 0;
        CHECK(evenHits > 0);
        filtered->setHitFilter(&filter);
        filtered->setMeshFiltered(0, false);
        filtered->commit();
        const int before = filter.calls;
        filtered->intersect(rays, a);
        CHECK(filter.calls == before);
    }
}
