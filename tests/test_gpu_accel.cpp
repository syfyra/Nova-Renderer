// docs/design/M10.md D5 (G1): the GPU backend's OptiX acceleration finds the
// same closest hits and occlusions as Embree on triangle meshes (static and
// instanced). Skipped without a usable NVIDIA device.

#include "test_helpers.h"

#include "core/gpu/device.h"
#include "core/gpu/optix_scene.h"
#include "core/math/rng.h"
#include "core/math/sampling.h"
#include "core/scene/scene.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace nova;
using namespace nova::test;

TEST_CASE("OptiX finds Embree's hits", "[gpu][accel]") {
    gpu::Device device;
    std::string error;
    if (!device.init(error))
        SKIP("no GPU: " << error);
    gpu::OptixScene accel(device);
    REQUIRE(accel.init(error));

    Scene scene;
    MeshDesc ball = makeSphere(0.8f, 48, 24);
    ball.transform = translation({-1.0f, 0.5f, 0.6f});
    scene.updateMesh(scene.createMesh(), ball);
    const MeshId field = scene.createMesh();
    scene.updateMesh(field, makeQuad(0.3f));
    std::vector<Xform34> inst;
    for (int i = 0; i < 25; ++i)
        inst.push_back(toXform34(translation({float(i % 5) - 2.0f, float(i / 5) - 2.0f, -0.2f})));
    scene.setMeshInstances(field, inst);
    MeshDesc curve; // not represented yet: skipped
    curve.shape = MeshShape::Curves;
    scene.updateMesh(scene.createMesh(), curve);
    scene.commit();
    uint32_t skipped = 0;
    REQUIRE(accel.build(scene, skipped));
    CHECK(skipped == 1);

    const uint32_t n = 20000;
    Rng rng = Rng::make(3u, 1u, 4u);
    std::vector<Vec3f> org(n), dir(n);
    std::vector<float> tMax(n);
    for (uint32_t k = 0; k < n; ++k) {
        org[k] = {6.0f * rng.next() - 3.0f, 6.0f * rng.next() - 3.0f, 3.0f};
        dir[k] = normalize(Vec3f{rng.next() - 0.5f, rng.next() - 0.5f, -1.0f});
        tMax[k] = k % 3 ? kInf : 1.0f + 4.0f * rng.next();
    }
    gpu::DeviceBuffer dOrg, dDir, dT, dMesh, dPrim, dUv, dInst, dOcc;
    REQUIRE(device.upload(dOrg, org));
    REQUIRE(device.upload(dDir, dir));
    REQUIRE(device.upload(dT, tMax));
    REQUIRE(dMesh.reserve(device, n * 4));
    REQUIRE(dPrim.reserve(device, n * 4));
    REQUIRE(dUv.reserve(device, n * 8));
    REQUIRE(dInst.reserve(device, n * 4));
    REQUIRE(dOcc.reserve(device, n));
    gpu::TraceParams p;
    p.org = dOrg.as<Vec3f>();
    p.dir = dDir.as<Vec3f>();
    p.tMax = dT.as<float>();
    p.mesh = dMesh.as<uint32_t>();
    p.prim = dPrim.as<uint32_t>();
    p.uv = dUv.as<Vec2f>();
    p.instance = dInst.as<uint32_t>();
    p.occluded = dOcc.as<uint8_t>();
    // Occlusion first (it reads tMax, which the closest-hit launch overwrites).
    REQUIRE(accel.launch(gpu::Raygen::Occluded, p, n));
    REQUIRE(accel.launch(gpu::Raygen::Closest, p, n));
    std::vector<uint32_t> gMesh(n), gPrim(n), gInst(n);
    std::vector<Vec2f> gUv(n);
    std::vector<float> gT(n);
    std::vector<uint8_t> gOcc(n);
    REQUIRE(device.download(gMesh.data(), dMesh.ptr(), n * 4));
    REQUIRE(device.download(gPrim.data(), dPrim.ptr(), n * 4));
    REQUIRE(device.download(gInst.data(), dInst.ptr(), n * 4));
    REQUIRE(device.download(gUv.data(), dUv.ptr(), n * 8));
    REQUIRE(device.download(gT.data(), dT.ptr(), n * 4));
    REQUIRE(device.download(gOcc.data(), dOcc.ptr(), n));

    // Embree's answers for the same rays.
    std::vector<float> eT = tMax;
    std::vector<uint32_t> eMesh(n), ePrim(n), eInst(n);
    std::vector<Vec2f> eUv(n);
    std::vector<uint8_t> eOcc(n);
    std::vector<float> times(n, 0.0f), seeds(n, 0.0f);
    std::vector<uint8_t> types(n, 0);
    scene.accel().occluded(RayBatch{org.data(), dir.data(), tMax.data(), n, times.data(), seeds.data(), types.data(), 0},
                           eOcc.data());
    HitBatch hb{eT.data(), eMesh.data(), ePrim.data(), eUv.data(), eInst.data()};
    scene.accel().intersect(RayBatch{org.data(), dir.data(), tMax.data(), n, times.data(), seeds.data(), types.data(), 0},
                            hb);

    int hits = 0, different = 0, occlusionDifferent = 0;
    for (uint32_t k = 0; k < n; ++k) {
        occlusionDifferent += gOcc[k] != eOcc[k];
        if (eMesh[k] == kInvalidId && gMesh[k] == kInvalidId)
            continue;
        ++hits;
        const bool same = eMesh[k] == gMesh[k] && ePrim[k] == gPrim[k] && eInst[k] == gInst[k] &&
                          std::fabs(eT[k] - gT[k]) <= 1e-4f * (1.0f + eT[k]) &&
                          std::fabs(eUv[k].x - gUv[k].x) <= 1e-3f && std::fabs(eUv[k].y - gUv[k].y) <= 1e-3f;
        different += !same;
    }
    INFO("hits " << hits << ", different " << different << ", occlusion different " << occlusionDifferent);
    CHECK(hits > 3000);
    CHECK(different <= hits / 1000); // edges: the two intersectors may pick neighbouring triangles
    CHECK(occlusionDifferent <= int(n) / 1000);
}
