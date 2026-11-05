// IPR latency (docs/PLAN.md: "IPR-Latenz < 100 ms beim Parameterwechsel";
// docs/research/m7): measured, not asserted. Run with the tag [.bench]
// (hidden from the default run):
//
//   nova_hydra_tests "[ipr-latency]"
//
// A scene of about a million triangles, a point instancer and two lights
// renders at 1280x720 through a real render index, as the Solaris viewport
// drives it. Each USD edit is timed from the edit to the end of the sync,
// to the restarted render, and to the first finished pass after it.

#include "hydra/renderBuffer.h"
#include "hydra/renderDelegate.h"
#include "hydra/renderParam.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/range1f.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

using Clock = std::chrono::steady_clock;
double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

class SyncTask final : public HdTask {
public:
    explicit SyncTask(HdRenderPassSharedPtr pass) : HdTask(SdfPath::EmptyPath()), _pass(std::move(pass)) {}
    void Sync(HdSceneDelegate *, HdTaskContext *, HdDirtyBits *dirtyBits) override {
        _pass->Sync();
        *dirtyBits = HdChangeTracker::Clean;
    }
    void Prepare(HdTaskContext *, HdRenderIndex *) override {}
    void Execute(HdTaskContext *) override {}
    const TfTokenVector &GetRenderTags() const override { return _tags; }

private:
    HdRenderPassSharedPtr _pass;
    TfTokenVector _tags{HdRenderTagTokens->geometry, HdRenderTagTokens->render};
};

struct ViewCamera final : HdCamera {
    ViewCamera() : HdCamera(SdfPath("/viewcam")) {
        _projection = Perspective;
        _horizontalAperture = 36.0f;
        _verticalAperture = 20.25f;
        _focalLength = 30.0f;
        _clippingRange = GfRange1f(0.1f, 10000.0f);
        Look(0.0);
    }
    void Look(double yaw) {
        _transform = GfMatrix4d(1.0).SetRotate(GfRotation(GfVec3d(1, 0, 0), -25.0)) *
                     GfMatrix4d(1.0).SetRotate(GfRotation(GfVec3d(0, 1, 0), yaw)) *
                     GfMatrix4d(1.0).SetTranslate(GfVec3d(0.0, 18.0, 40.0));
    }
};

UsdStageRefPtr makeScene(int grid, int instances) {
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    // A displaced-looking ground grid of grid x grid quads.
    UsdGeomMesh ground = UsdGeomMesh::Define(stage, SdfPath("/World/ground"));
    VtVec3fArray points;
    VtIntArray counts, indices;
    points.reserve(size_t(grid + 1) * size_t(grid + 1));
    for (int j = 0; j <= grid; ++j)
        for (int i = 0; i <= grid; ++i) {
            const float x = -40.0f + 80.0f * float(i) / float(grid), z = -40.0f + 80.0f * float(j) / float(grid);
            points.push_back(GfVec3f(x, 0.3f * std::sin(x * 0.7f) * std::cos(z * 0.5f), z));
        }
    for (int j = 0; j < grid; ++j)
        for (int i = 0; i < grid; ++i) {
            const int a = j * (grid + 1) + i;
            counts.push_back(4);
            for (int v : {a, a + grid + 1, a + grid + 2, a + 1})
                indices.push_back(v);
        }
    ground.CreatePointsAttr().Set(points);
    ground.CreateFaceVertexCountsAttr().Set(counts);
    ground.CreateFaceVertexIndicesAttr().Set(indices);
    ground.CreateSubdivisionSchemeAttr().Set(TfToken("none"));

    // A unit cube mesh: the instancer's prototype and a free object.
    const auto cube = [&](SdfPath const &path) {
        UsdGeomMesh m = UsdGeomMesh::Define(stage, path);
        m.CreatePointsAttr().Set(VtVec3fArray{{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f},
                                              {-0.5f, 0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f},
                                              {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}});
        m.CreateFaceVertexCountsAttr().Set(VtIntArray{4, 4, 4, 4, 4, 4});
        m.CreateFaceVertexIndicesAttr().Set(
            VtIntArray{0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 2, 3, 7, 6, 1, 2, 6, 5, 0, 4, 7, 3});
        m.CreateSubdivisionSchemeAttr().Set(TfToken("none"));
        return m;
    };
    UsdGeomMesh hero = cube(SdfPath("/World/hero"));
    UsdGeomXformCommonAPI(hero).SetTranslate(GfVec3d(0.0, 2.0, 0.0));
    UsdGeomXformCommonAPI(hero).SetScale(GfVec3f(4.0f));

    UsdGeomPointInstancer inst = UsdGeomPointInstancer::Define(stage, SdfPath("/World/inst"));
    cube(SdfPath("/World/inst/protos/box"));
    inst.CreatePrototypesRel().SetTargets({SdfPath("/World/inst/protos/box")});
    VtVec3fArray positions;
    VtIntArray protoIndices;
    uint32_t h = 12345u;
    const auto rnd = [&] {
        h = h * 1664525u + 1013904223u;
        return float(h >> 8) / float(1u << 24);
    };
    for (int i = 0; i < instances; ++i) {
        positions.push_back(GfVec3f(-35.0f + 70.0f * rnd(), 0.5f, -35.0f + 70.0f * rnd()));
        protoIndices.push_back(0);
    }
    inst.CreatePositionsAttr().Set(positions);
    inst.CreateProtoIndicesAttr().Set(protoIndices);

    UsdShadeMaterial mtl = UsdShadeMaterial::Define(stage, SdfPath("/mat/red"));
    UsdShadeShader surf = UsdShadeShader::Define(stage, SdfPath("/mat/red/s"));
    surf.CreateIdAttr().Set(TfToken("ND_standard_surface_surfaceshader"));
    surf.CreateInput(TfToken("base_color"), SdfValueTypeNames->Color3f).Set(GfVec3f(0.8f, 0.2f, 0.1f));
    mtl.CreateSurfaceOutput(TfToken("mtlx")).ConnectToSource(surf.ConnectableAPI(), TfToken("out"));
    UsdShadeMaterialBindingAPI::Apply(hero.GetPrim()).Bind(mtl);
    UsdShadeMaterialBindingAPI::Apply(ground.GetPrim()).Bind(mtl);

    UsdPrim key = stage->DefinePrim(SdfPath("/World/key"), TfToken("RectLight"));
    key.CreateAttribute(TfToken("inputs:intensity"), SdfValueTypeNames->Float).Set(20.0f);
    key.CreateAttribute(TfToken("inputs:width"), SdfValueTypeNames->Float).Set(10.0f);
    key.CreateAttribute(TfToken("inputs:height"), SdfValueTypeNames->Float).Set(10.0f);
    UsdGeomXformCommonAPI(key).SetTranslate(GfVec3d(0.0, 20.0, 0.0));
    UsdGeomXformCommonAPI(key).SetRotate(GfVec3f(-90.0f, 0.0f, 0.0f));
    stage->DefinePrim(SdfPath("/World/sky"), TfToken("DomeLight"))
        .CreateAttribute(TfToken("inputs:intensity"), SdfValueTypeNames->Float)
        .Set(0.5f);
    return stage;
}

} // namespace

TEST_CASE("IPR latency of edits", "[.bench][ipr-latency]") {
    const int grid = 700, instances = 20000, width = 1280, height = 720;
    UsdStageRefPtr stage = makeScene(grid, instances);

    auto delegate = std::make_unique<HdNovaRenderDelegate>();
    delegate->SetRenderSetting(HdNovaSettingsTokens->samples, VtValue(1 << 20));
    delegate->SetRenderSetting(TfToken("houdini:viewport"), VtValue(true)); // proxy passes, refits
    std::unique_ptr<HdRenderIndex> index(HdRenderIndex::New(delegate.get(), HdDriverVector()));
    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    info.addDrawModeSceneIndex = false;
    UsdImagingSceneIndices sceneIndices = UsdImagingCreateSceneIndices(info);
    index->InsertSceneIndex(sceneIndices.finalSceneIndex, SdfPath::AbsoluteRootPath(), false);
    HdRenderPassSharedPtr pass =
        delegate->CreateRenderPass(index.get(), HdRprimCollection(HdTokens->geometry, HdReprSelector(HdReprTokens->refined)));
    HdTaskSharedPtrVector tasks{std::make_shared<SyncTask>(pass)};
    HdEngine engine;

    ViewCamera camera;
    auto state = std::make_shared<HdRenderPassState>();
    state->SetCamera(&camera);
    state->SetViewport(GfVec4d(0, 0, width, height));
    auto *color = static_cast<HdNovaRenderBuffer *>(delegate->CreateBprim(HdPrimTypeTokens->renderBuffer, SdfPath("/rb")));
    color->Allocate(GfVec3i(width, height, 1), HdFormatFloat32Vec4, false);
    HdRenderPassAovBinding binding;
    binding.aovName = HdAovTokens->color;
    binding.renderBuffer = color;
    binding.clearValue = VtValue(GfVec4f(0.0f));
    state->SetAovBindings({binding});

    const auto samples = [&] {
        const VtValue v = delegate->GetRenderStats()["passes"];
        return v.IsHolding<int>() ? v.UncheckedGet<int>() : 0;
    };
    struct Times {
        double sync, restart, first;
    };
    // Sync the stage's pending edits, restart through Execute and wait for
    // the first pass after it.
    const auto update = [&]() -> Times {
        const auto t0 = Clock::now();
        sceneIndices.stageSceneIndex->ApplyPendingUpdates();
        engine.Execute(index.get(), &tasks);
        const auto t1 = Clock::now();
        pass->Execute(state, TfTokenVector());
        const auto t2 = Clock::now();
        while (samples() < 1 && ms(t2, Clock::now()) < 20000.0)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        const auto t3 = Clock::now();
        return {ms(t0, t1), ms(t1, t2), ms(t2, t3)};
    };

    const auto setupStart = Clock::now();
    const Times first = update();
    std::printf("IPR latency, %d x %d, %d triangles + %d instances (ms):\n", width, height, 2 * grid * grid, instances);
    std::printf("  %-28s total %8.1f (sync %.1f, restart %.1f, first pass %.1f)\n", "initial", ms(setupStart, Clock::now()),
                first.sync, first.restart, first.first);

    struct Edit {
        const char *name;
        std::function<void(int)> apply;
    };
    UsdPrim hero = stage->GetPrimAtPath(SdfPath("/World/hero"));
    const std::vector<Edit> edits = {
        {"camera move", [&](int i) { camera.Look(i % 2 ? 3.0 : 0.0); }},
        {"light intensity", [&](int i) { stage->GetAttributeAtPath(SdfPath("/World/key.inputs:intensity")).Set(i % 2 ? 30.0f : 20.0f); }},
        {"light transform",
         [&](int i) { UsdGeomXformCommonAPI(stage->GetPrimAtPath(SdfPath("/World/key"))).SetTranslate(GfVec3d(i % 2 ? 2.0 : 0.0, 20.0, 0.0)); }},
        {"material value",
         [&](int i) {
             stage->GetAttributeAtPath(SdfPath("/mat/red/s.inputs:base_color")).Set(i % 2 ? GfVec3f(0.2f, 0.8f, 0.1f) : GfVec3f(0.8f, 0.2f, 0.1f));
         }},
        {"object transform (cube)",
         [&](int i) { UsdGeomXformCommonAPI(hero).SetTranslate(GfVec3d(i % 2 ? 1.0 : 0.0, 2.0, 0.0)); }},
        {"object transform (1M tris)",
         [&](int i) {
             UsdGeomXformCommonAPI(stage->GetPrimAtPath(SdfPath("/World/ground"))).SetTranslate(GfVec3d(0.0, i % 2 ? 0.1 : 0.0, 0.0));
         }},
        {"instancer: one position",
         [&](int i) {
             UsdAttribute a = stage->GetAttributeAtPath(SdfPath("/World/inst.positions"));
             VtVec3fArray p;
             a.Get(&p);
             p[0][1] = i % 2 ? 2.0f : 0.5f;
             a.Set(p);
         }},
        {"deform points (1M tris)",
         [&](int i) {
             UsdAttribute a = stage->GetAttributeAtPath(SdfPath("/World/ground.points"));
             VtVec3fArray p;
             a.Get(&p);
             p[0][1] += i % 2 ? 0.5f : -0.5f;
             a.Set(p);
         }},
        {"visibility (cube)",
         [&](int i) { stage->GetAttributeAtPath(SdfPath("/World/hero.visibility")).Set(i % 2 ? TfToken("invisible") : TfToken("inherited")); }},
        {"add/remove prim",
         [&](int i) {
             if (i % 2) {
                 UsdGeomMesh extra = UsdGeomMesh::Define(stage, SdfPath("/World/extra"));
                 extra.CreateFaceVertexCountsAttr().Set(VtIntArray{3});
                 extra.CreateFaceVertexIndicesAttr().Set(VtIntArray{0, 1, 2});
                 extra.CreatePointsAttr().Set(VtVec3fArray{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}});
             } else {
                 stage->RemovePrim(SdfPath("/World/extra"));
             }
         }},
    };
    for (Edit const &e : edits) {
        std::vector<Times> runs;
        for (int i = 1; i <= 6; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150)); // the render is busy, as in IPR
            const auto t0 = Clock::now();
            e.apply(i);
            const double editMs = ms(t0, Clock::now());
            Times t = update();
            t.sync += editMs;
            runs.push_back(t);
        }
        std::sort(runs.begin(), runs.end(),
                  [](Times const &a, Times const &b) { return a.sync + a.restart + a.first < b.sync + b.restart + b.first; });
        const Times &m = runs[runs.size() / 2];
        std::printf("  %-28s total %8.1f (sync %.1f, restart %.1f, first pass %.1f)\n", e.name, m.sync + m.restart + m.first,
                    m.sync, m.restart, m.first);
    }

    pass.reset();
    tasks.clear();
    color->Finalize(delegate->GetRenderParam());
    delegate->DestroyBprim(color);
    index.reset();
    delegate.reset();
}
