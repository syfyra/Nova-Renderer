// Hydra-side re-sync regressions of the M11 review (docs/bug-audit-2026-11.md,
// "M11 review"): what reaches prims that are already synced when the render
// settings' per-object defaults change (also through a render pass created
// later), instancer samples per shutter under parallel syncs, per-instance
// cryptomatte names after an instance index edit, a mesh that becomes a
// portal, media under Karma's per-object motion blur, the atmosphere count
// per delegate and sample filter warnings. The rig drives a real render index
// through UsdImaging's scene indices with the renderer plugin's resync index,
// so the render pass's own re-dirtying reaches the prims as in Solaris.

#include "hydra/atmosphere.h"
#include "hydra/instancer.h"
#include "hydra/motion.h"
#include "hydra/renderBuffer.h"
#include "hydra/renderDelegate.h"
#include "hydra/renderParam.h"
#include "hydra/resyncSceneIndex.h"
#include "hydra/velocitySceneIndex.h"

#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/work/loops.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using Catch::Matchers::WithinAbs;

namespace {

class WarningCatcher final : public TfDiagnosticMgr::Delegate {
public:
    WarningCatcher() { TfDiagnosticMgr::GetInstance().AddDelegate(this); }
    ~WarningCatcher() override { TfDiagnosticMgr::GetInstance().RemoveDelegate(this); }
    void IssueError(TfError const &) override {}
    void IssueFatalError(TfCallContext const &, std::string const &) override {}
    void IssueStatus(TfStatus const &) override {}
    void IssueWarning(TfWarning const &w) override {
        std::lock_guard lock(_mutex);
        warnings.push_back(w.GetCommentary());
    }
    int count(char const *text) {
        std::lock_guard lock(_mutex);
        int n = 0;
        for (std::string const &w : warnings)
            n += w.find(text) != std::string::npos;
        return n;
    }
    std::vector<std::string> warnings;

private:
    std::mutex _mutex;
};

class SyncTask final : public HdTask {
public:
    SyncTask(HdRenderPassSharedPtr pass) : HdTask(SdfPath::EmptyPath()), _pass(std::move(pass)) {}
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

// An orthographic camera looking down -z with the rig's shutter.
struct ShutterCamera final : HdCamera {
    ShutterCamera() : HdCamera(SdfPath("/resyncCam")) {
        _projection = Orthographic;
        _horizontalAperture = 400.0f;
        _verticalAperture = 400.0f;
        _clippingRange = GfRange1f(0.1f, 100.0f);
        _shutterOpen = -0.25;
        _shutterClose = 0.25;
        _transform = GfMatrix4d(1.0).SetTranslate(GfVec3d(0.0, 0.0, 10.0));
    }
};

// A stage through UsdImaging, Nova's velocity indices and the resync index
// (the renderer plugin's chain) into a named render index, at time 0 with
// the shutter [-0.25, 0.25].
struct Rig {
    UsdStageRefPtr stage;
    std::unique_ptr<HdNovaRenderDelegate> delegate = std::make_unique<HdNovaRenderDelegate>();
    std::unique_ptr<HdRenderIndex> index;
    UsdImagingSceneIndices sceneIndices;
    HdRenderPassSharedPtr pass;
    HdTaskSharedPtrVector tasks;
    HdEngine engine;
    // Rendering through the pass (execute()).
    ShutterCamera camera;
    HdNovaRenderBuffer *buffer = nullptr;
    std::shared_ptr<HdRenderPassState> state;

    explicit Rig(std::string const &body, std::vector<std::pair<char const *, VtValue>> const &settings = {}) {
        static std::atomic<int> counter{0};
        const std::string instance = "novaResyncTest" + std::to_string(++counter);
        SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
        layer->ImportFromString(std::string("#usda 1.0\n(\n    timeCodesPerSecond = 24\n)\n") + body);
        stage = UsdStage::Open(layer);
        delegate->SetRenderSetting(HdNovaSettingsTokens->samples, VtValue(1));
        for (auto const &[key, value] : settings)
            delegate->SetRenderSetting(TfToken(key), value);
        param().SetShutter(HdNovaResolveShutter(-0.25, 0.25, false, false));
        index.reset(HdRenderIndex::New(delegate.get(), HdDriverVector(), instance));
        UsdImagingCreateSceneIndicesInfo info;
        info.stage = stage;
        info.addDrawModeSceneIndex = false;
        sceneIndices = UsdImagingCreateSceneIndices(info);
        sceneIndices.stageSceneIndex->SetTime(UsdTimeCode(0.0));
        index->InsertSceneIndex(
            HdNovaResyncSceneIndex::New(HdNovaVelocityModeSceneIndex::Append(sceneIndices.finalSceneIndex), instance),
            SdfPath::AbsoluteRootPath(), false);
        newPass();
    }
    ~Rig() {
        delegate->GetRenderThread().StopRender();
        if (buffer) {
            buffer->Finalize(delegate->GetRenderParam());
            delegate->DestroyBprim(buffer);
        }
        tasks.clear();
        pass.reset();
        index.reset();
        delegate.reset();
    }
    // A (new) render pass in place of the current one, and a sync.
    void newPass() {
        tasks.clear();
        pass.reset();
        pass = delegate->CreateRenderPass(index.get(),
                                          HdRprimCollection(HdTokens->geometry, HdReprSelector(HdReprTokens->refined)));
        tasks.push_back(std::make_shared<SyncTask>(pass));
        sync();
    }
    void sync() {
        sceneIndices.stageSceneIndex->ApplyPendingUpdates();
        engine.Execute(index.get(), &tasks);
        scene().commit();
    }
    // What a viewport does: execute the pass and sync, a few rounds (each
    // resync of the pass returns before rendering).
    void execute() {
        if (!buffer) {
            buffer = static_cast<HdNovaRenderBuffer *>(
                delegate->CreateBprim(HdPrimTypeTokens->renderBuffer, SdfPath("/resyncBuffer")));
            buffer->Allocate(GfVec3i(4, 4, 1), HdFormatFloat32Vec4, false);
            state = std::make_shared<HdRenderPassState>();
            state->SetCamera(&camera);
            state->SetViewport(GfVec4d(0, 0, 4, 4));
            HdRenderPassAovBinding binding;
            binding.aovName = HdAovTokens->color;
            binding.renderBuffer = buffer;
            binding.clearValue = VtValue(GfVec4f(0.0f));
            state->SetAovBindings({binding});
        }
        for (int i = 0; i < 4; ++i) {
            pass->Execute(state, TfTokenVector());
            sync();
        }
        delegate->GetRenderThread().StopRender();
    }
    HdNovaRenderParam &param() { return *static_cast<HdNovaRenderParam *>(delegate->GetRenderParam()); }
    nova::Scene &scene() { return param().GetScene(); }
    void setPrimvar(char const *prim, char const *name, SdfValueTypeName const &type, VtValue const &value) {
        UsdGeomPrimvarsAPI(stage->GetPrimAtPath(SdfPath(prim)))
            .CreatePrimvar(TfToken(name), type, UsdGeomTokens->constant)
            .Set(value);
    }
    // The live mesh records of a shape kind.
    std::vector<nova::MeshRecord const *> records(nova::MeshShape shape) {
        std::vector<nova::MeshRecord const *> out;
        for (uint32_t id = 0; id < scene().meshCount(); ++id)
            if (scene().meshRecord(id).alive && scene().meshRecord(id).desc.shape == shape)
                out.push_back(&scene().meshRecord(id));
        return out;
    }
    nova::MeshRecord const &record(nova::MeshShape shape) {
        const auto all = records(shape);
        REQUIRE(all.size() == 1);
        return *all[0];
    }
};

const char *kPanel = R"(
def Mesh "panel"
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
    uniform token subdivisionScheme = "none"
}
)";

} // namespace

// Review finding 1: the render settings' Karma light-source values reach a
// mesh that is already synced (its primvars do not change, so its geometry
// stays and its sync used to return before applying them).
TEST_CASE("Render settings' light-source defaults reach synced meshes", "[hydra][karma][resync]") {
    Rig rig(kPanel);
    rig.execute();
    CHECK(rig.record(nova::MeshShape::Triangles).lightSource.mode == 0);
    const uint64_t geometry = rig.record(nova::MeshShape::Triangles).geometryVersion;
    rig.delegate->SetRenderSetting(TfToken("karma:object:treat_as_lightsource"), VtValue(TfToken("Yes")));
    rig.delegate->SetRenderSetting(TfToken("karma:object:lightsource_diffusescale"), VtValue(0.5f));
    rig.delegate->SetRenderSetting(TfToken("karma:object:lightsource_samplingquality"), VtValue(2.0f));
    rig.execute();
    nova::MeshRecord const &r = rig.record(nova::MeshShape::Triangles);
    CHECK(r.lightSource.mode == 1);
    CHECK(r.lightSource.diffuse == 0.5f);
    CHECK(r.lightSource.samplingQuality == 2.0f);
    CHECK(r.geometryVersion == geometry); // no rebuild for it
    rig.delegate->SetRenderSetting(TfToken("karma:object:treat_as_lightsource"), VtValue(TfToken("No")));
    rig.execute();
    CHECK(rig.record(nova::MeshShape::Triangles).lightSource.mode == 0);
}

// Review finding 9: a render pass created after the defaults changed (a
// pass replaced in a session) still re-syncs the prims that used the old ones.
TEST_CASE("A new render pass re-syncs prims for changed defaults", "[hydra][karma][resync]") {
    Rig rig(kPanel);
    rig.execute();
    CHECK(rig.record(nova::MeshShape::Triangles).lightSource.mode == 0);
    rig.delegate->SetRenderSetting(TfToken("karma:object:treat_as_lightsource"), VtValue(TfToken("Yes")));
    rig.newPass(); // its constructor takes the new defaults
    rig.execute();
    CHECK(rig.record(nova::MeshShape::Triangles).lightSource.mode == 1);
}

// Review finding 4: a mesh that becomes a light portal leaves no texture
// baking entry behind (its scene mesh is gone; the id may be reused).
TEST_CASE("A mesh turned portal leaves the bake paths", "[hydra][portal][bake][resync]") {
    Rig rig(kPanel);
    CHECK(rig.param().MeshesUnder(SdfPath("/panel")).size() == 1);
    rig.setPrimvar("/panel", "karma:object:isportal", SdfValueTypeNames->Bool, VtValue(true));
    rig.sync();
    CHECK(rig.records(nova::MeshShape::Triangles).empty());
    CHECK(rig.param().MeshesUnder(SdfPath::AbsoluteRootPath()).empty());
    rig.setPrimvar("/panel", "karma:object:isportal", SdfValueTypeNames->Bool, VtValue(false));
    rig.sync();
    CHECK(rig.param().MeshesUnder(SdfPath("/panel")).size() == 1);
}

namespace {

const char *kInstancer = R"(
def PointInstancer "inst"
{
    rel prototypes = [</inst/protos/a>, </inst/protos/b>]
    int[] protoIndices = [0, 1, 0]
    point3f[] positions.timeSamples = {
        -0.25: [(-0.25, 0, 0), (2.75, 0, 0), (5.75, 0, 0)],
        0.25: [(0.25, 0, 0), (3.25, 0, 0), (6.25, 0, 0)],
    }
    string[] primvars:instname = ["i0", "i1", "i2"] (interpolation = "vertex")
    def Scope "protos"
    {
        def Mesh "a"
        {
            int[] faceVertexCounts = [4]
            int[] faceVertexIndices = [0, 1, 2, 3]
            point3f[] points = [(-0.5, -0.5, 0), (0.5, -0.5, 0), (0.5, 0.5, 0), (-0.5, 0.5, 0)]
            uniform token subdivisionScheme = "none"
        }
        def Mesh "b"
        {
            int[] faceVertexCounts = [4]
            int[] faceVertexIndices = [0, 1, 2, 3]
            point3f[] points = [(-0.25, -0.25, 0), (0.25, -0.25, 0), (0.25, 0.25, 0), (-0.25, 0.25, 0)]
            uniform token subdivisionScheme = "none"
            %s
        }
    }
}
)";

// The instanced record whose first point is at x (the prototypes differ in size).
nova::MeshRecord const &instanced(Rig &rig, float x) {
    for (nova::MeshRecord const *r : rig.records(nova::MeshShape::Triangles))
        if (r->instanced && !r->desc.points.empty() && std::fabs(r->desc.points[0].x - x) < 1e-6f)
            return *r;
    FAIL("no instanced mesh with its first point at x = " << x);
    static nova::MeshRecord none;
    return none;
}

} // namespace

// Review finding 2: prototypes of one instancer with different shutters
// (Karma's per-object motion blur) read samples of their own shutter, also
// when they sync in parallel.
TEST_CASE("Instancer samples per shutter under parallel syncs", "[hydra][instancing][motion][resync]") {
    Rig rig(TfStringPrintf(kInstancer, "bool primvars:karma:object:mblur = 0 (interpolation = \"constant\")"));
    // a blurs over the shutter, b renders at the frame.
    CHECK(instanced(rig, -0.5f).motionSteps > 1);
    CHECK(instanced(rig, -0.25f).motionSteps == 1);
    CHECK(instanced(rig, -0.25f).instances.size() == 1);

    // Many prims computing their instances at once with both shutters: each
    // sees its own shutter's samples (the instancer used to resample its one
    // set of samples for whichever prim asked last, under the readers).
    auto *instancer = static_cast<HdNovaInstancer *>(rig.index->GetInstancer(SdfPath("/inst")));
    REQUIRE(instancer);
    // Prototype a's rprim (UsdImaging may re-path prototypes): instanced by
    // /inst, its points those of a.
    SdfPath proto;
    std::string ids;
    for (SdfPath const &id : rig.index->GetRprimIds()) {
        ids += " " + id.GetString();
        HdSceneDelegate *d = rig.index->GetSceneDelegateForRprim(id);
        const VtValue points = d ? d->Get(id, HdTokens->points) : VtValue();
        if (d && d->GetInstancerId(id) == SdfPath("/inst") && points.IsHolding<VtVec3fArray>() &&
            !points.UncheckedGet<VtVec3fArray>().empty() && points.UncheckedGet<VtVec3fArray>()[0][0] == -0.5f)
            proto = id;
    }
    INFO("rprims:" << ids);
    REQUIRE(!proto.IsEmpty());
    HdSceneDelegate *sd = rig.index->GetSceneDelegateForRprim(proto);
    REQUIRE(sd);
    const HdNovaShutter blurred = rig.param().GetShutter(), frame;
    std::atomic<int> wrong{0};
    std::mutex mutex;
    std::string firstWrong;
    WorkParallelForN(512, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const bool blur = i % 2 == 0;
            std::vector<nova::Xform34> instances;
            std::vector<nova::SrtTransform> keys;
            uint32_t steps = 0;
            HdNovaComputeInstances(sd, proto, *instancer, blur ? blurred : frame, 8, instances, keys, steps);
            bool ok;
            if (blur) // two instances moving 0.5 units over the shutter
                ok = steps >= 2 && keys.size() == 2 * steps && std::fabs(keys[0].tx - -0.25f) < 1e-5f &&
                     std::fabs(keys[size_t(steps - 1) * 2].tx - 0.25f) < 1e-5f;
            else
                ok = steps == 1 && instances.size() == 2 && std::fabs(instances[0].c[3][0]) < 1e-5f &&
                     std::fabs(instances[1].c[3][0] - 6.0f) < 1e-5f;
            if (!ok && wrong++ == 0) {
                std::lock_guard lock(mutex);
                firstWrong = TfStringPrintf("blur %d: steps %u, %zu keys (first tx %g), %zu instances (x %g)",
                                            int(blur), steps, keys.size(), keys.empty() ? 0.0f : keys[0].tx,
                                            instances.size(), instances.empty() ? 0.0f : instances[0].c[3][0]);
            }
        }
    });
    INFO(firstWrong);
    CHECK(wrong.load() == 0);
}

// Review finding 6: per-instance cryptomatte names follow edited instance
// indices (animated protoIndices) on every prim type.
TEST_CASE("Per-instance cryptomatte names follow the instance indices", "[hydra][crypto][instancing][resync]") {
    // Prototypes of three kinds: a mesh, points and curves.
    Rig rig(R"(
def PointInstancer "inst"
{
    rel prototypes = [</inst/protos/a>, </inst/protos/p>, </inst/protos/c>]
    int[] protoIndices = [0, 1, 2, 0]
    point3f[] positions = [(0, 0, 0), (2, 0, 0), (4, 0, 0), (6, 0, 0)]
    string[] primvars:instname = ["i0", "i1", "i2", "i3"] (interpolation = "vertex")
    def Scope "protos"
    {
        def Mesh "a"
        {
            int[] faceVertexCounts = [4]
            int[] faceVertexIndices = [0, 1, 2, 3]
            point3f[] points = [(-0.5, -0.5, 0), (0.5, -0.5, 0), (0.5, 0.5, 0), (-0.5, 0.5, 0)]
            uniform token subdivisionScheme = "none"
        }
        def Points "p"
        {
            point3f[] points = [(0, 0, 0)]
            float[] widths = [0.5] (interpolation = "vertex")
        }
        def BasisCurves "c"
        {
            uniform token type = "linear"
            int[] curveVertexCounts = [2]
            point3f[] points = [(0, 0, 0), (0, 1, 0)]
            float[] widths = [0.1] (interpolation = "constant")
        }
    }
}
)");
    const auto names = [&](nova::MeshShape shape) {
        std::vector<std::string> out;
        for (nova::CryptoAttribute const &a : rig.record(shape).cryptoAttributes)
            if (a.name == "instname" && a.perInstance)
                for (uint32_t id : a.ids)
                    out.push_back(rig.param().GetCryptoNames().Find("instname", id));
        return out;
    };
    using S = std::vector<std::string>;
    CHECK(names(nova::MeshShape::Triangles) == S{"i0", "i3"});
    CHECK(names(nova::MeshShape::Points) == S{"i1"});
    CHECK(names(nova::MeshShape::Curves) == S{"i2"});
    rig.stage->GetAttributeAtPath(SdfPath("/inst.protoIndices")).Set(VtIntArray{1, 2, 0, 0});
    rig.sync();
    CHECK(names(nova::MeshShape::Triangles) == S{"i2", "i3"});
    CHECK(names(nova::MeshShape::Points) == S{"i0"});
    CHECK(names(nova::MeshShape::Curves) == S{"i1"});
}

// Review finding 7: media bounded by a mesh or a sphere sample their motion
// over their own shutter (the frame alone with Karma's mblur off).
TEST_CASE("Media follow Karma's per-object motion blur", "[hydra][karma][motion][medium][resync]") {
    const char *body = R"(
def Mesh "box"
{
    int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
    int[] faceVertexIndices = [0, 3, 2, 1, 0, 1, 5, 4, 3, 7, 6, 2, 0, 4, 7, 3, 1, 2, 6, 5, 4, 5, 6, 7]
    point3f[] points = [(-0.5, -0.5, -0.5), (0.5, -0.5, -0.5), (0.5, 0.5, -0.5), (-0.5, 0.5, -0.5), (-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5)]
    uniform token subdivisionScheme = "none"
    bool primvars:karma:object:volumeuniform = 1 (interpolation = "constant")
    double3 xformOp:translate.timeSamples = { -0.25: (-1, 0, 0), 0.25: (1, 0, 0) }
    uniform token[] xformOpOrder = ["xformOp:translate"]
    %s
}
def Sphere "ball"
{
    double radius = 0.5
    bool primvars:karma:object:volumeuniform = 1 (interpolation = "constant")
    double3 xformOp:translate.timeSamples = { -0.25: (2, 0, 0), 0.25: (4, 0, 0) }
    uniform token[] xformOpOrder = ["xformOp:translate"]
    %s
}
)";
    const auto steps = [](Rig &rig) {
        std::vector<uint32_t> out;
        for (uint32_t id = 0; id < rig.scene().volumeCount(); ++id)
            if (rig.scene().volumeRecord(id).desc.shell)
                out.push_back(rig.scene().volumeRecord(id).motionSteps);
        std::sort(out.begin(), out.end());
        return out;
    };
    {
        Rig rig(TfStringPrintf(body, "", ""));
        const std::vector<uint32_t> s = steps(rig);
        REQUIRE(s.size() == 2);
        CHECK(s[0] > 1); // both blur
    }
    const char *off = "bool primvars:karma:object:mblur = 0 (interpolation = \"constant\")";
    Rig rig(TfStringPrintf(body, off, off));
    CHECK(steps(rig) == std::vector<uint32_t>{1, 1});
}

namespace {

// A scene delegate that hands an atmosphere rprim nothing but visibility.
class BareDelegate final : public HdSceneDelegate {
public:
    explicit BareDelegate(HdRenderIndex *index) : HdSceneDelegate(index, SdfPath("/bare")) {}
    bool GetVisible(SdfPath const &) override { return true; }
    GfMatrix4d GetTransform(SdfPath const &) override { return GfMatrix4d(1.0); }
    VtValue Get(SdfPath const &, TfToken const &) override { return VtValue(); }
};

} // namespace

// Review finding 8: "more than one Karma Sky Atmosphere" counts the
// atmospheres of one delegate, not of every render in the process.
TEST_CASE("Karma Sky Atmospheres are counted per delegate", "[hydra][atmosphere][resync]") {
    WarningCatcher catcher;
    HdNovaRenderDelegate first, second;
    std::unique_ptr<HdRenderIndex> firstIndex(HdRenderIndex::New(&first, HdDriverVector()));
    std::unique_ptr<HdRenderIndex> secondIndex(HdRenderIndex::New(&second, HdDriverVector()));
    BareDelegate firstScene(firstIndex.get()), secondScene(secondIndex.get());
    HdNovaAtmosphere a(SdfPath("/review/skyA")), b(SdfPath("/review/skyB")), c(SdfPath("/review/skyC"));
    const auto sync = [](HdNovaAtmosphere &atmosphere, HdSceneDelegate &sd, HdNovaRenderDelegate &d) {
        HdDirtyBits bits = atmosphere.GetInitialDirtyBitsMask();
        atmosphere.Sync(&sd, d.GetRenderParam(), &bits, HdReprTokens->refined);
    };
    sync(a, firstScene, first);
    sync(b, secondScene, second); // another delegate's: no warning
    CHECK(catcher.count("more than one Karma Sky Atmosphere") == 0);
    sync(c, firstScene, first); // a second one in the first delegate
    CHECK(catcher.count("/review/skyC: more than one Karma Sky Atmosphere") == 1);
    a.Finalize(first.GetRenderParam());
    c.Finalize(first.GetRenderParam());
    b.Finalize(second.GetRenderParam());
}

// Review finding 5: the LPE and cryptomatte resolutions know the PrimId var
// as the others do, so an object coverage filter does not warn falsely.
TEST_CASE("Sample filters with a PrimId var do not warn", "[hydra][aov][resync]") {
    WarningCatcher catcher;
    HdNovaRenderDelegate d;
    const auto var = [](char const *name, char const *type, char const *source, char const *filter) {
        HdRenderPassAovBinding b;
        b.aovName = TfToken(name);
        b.aovSettings[TfToken("sourceName")] = VtValue(std::string(source));
        b.aovSettings[TfToken("sourceType")] = VtValue(TfToken(type));
        if (filter)
            b.aovSettings[TfToken("driver:parameters:aov:karma:filter")] = VtValue(std::string(filter));
        return b;
    };
    const char *omin = R"(["minmax", {"mode": "omin"}])";
    HdRenderPassAovBindingVector bindings = {var("PrimId", "raw", "ray:primid", nullptr),
                                             var("reviewOminZ", "raw", "ray:hitPz", omin),
                                             var("reviewOminLpe", "lpe", "C<RD>.*", omin)};
    d.GetLpeProgram(bindings);
    d.GetUtilityRequests(bindings);
    d.GetCryptoRequests(bindings);
    CHECK(catcher.count("needs a render var named PrimId") == 0);
    // Without the PrimId var Karma averages and reports it: Nova warns.
    bindings.erase(bindings.begin());
    bindings[0].aovName = TfToken("reviewOminZ2");
    d.GetLpeProgram(bindings);
    d.GetUtilityRequests(bindings);
    CHECK(catcher.count("reviewOminZ2 (raw ray:hitPz): minmax mode omin needs a render var named PrimId") == 1);
}
