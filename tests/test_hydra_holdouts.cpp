// Ray visibility and holdouts through Hydra (docs/design/M11-P2.md): Karma's
// primvars:karma:object:rendervisibility and holdoutmode on meshes, spheres,
// curves and points, the render settings' defaults (holdoutmode as Karma's
// menu label there), bgoccludeshadow and the lights' illumbackground.

#include "hydra/denoise.h"
#include "hydra/renderBuffer.h"
#include "hydra/renderDelegate.h"
#include "hydra/renderParam.h"

#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

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

struct Driver {
    UsdStageRefPtr stage;
    std::unique_ptr<HdNovaRenderDelegate> delegate = std::make_unique<HdNovaRenderDelegate>();
    std::unique_ptr<HdRenderIndex> index;
    UsdImagingSceneIndices sceneIndices;
    HdRenderPassSharedPtr pass;
    HdTaskSharedPtrVector tasks;
    HdEngine engine;

    explicit Driver(std::string const &body) {
        SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
        layer->ImportFromString("#usda 1.0\n" + body);
        stage = UsdStage::Open(layer);
        index.reset(HdRenderIndex::New(delegate.get(), HdDriverVector()));
        UsdImagingCreateSceneIndicesInfo info;
        info.stage = stage;
        info.addDrawModeSceneIndex = false;
        sceneIndices = UsdImagingCreateSceneIndices(info);
        sceneIndices.stageSceneIndex->SetTime(UsdTimeCode(1.0));
        index->InsertSceneIndex(sceneIndices.finalSceneIndex, SdfPath::AbsoluteRootPath(), false);
        pass = delegate->CreateRenderPass(index.get(),
                                          HdRprimCollection(HdTokens->geometry, HdReprSelector(HdReprTokens->refined)));
        tasks.push_back(std::make_shared<SyncTask>(pass));
        sync();
    }
    ~Driver() {
        tasks.clear();
        pass.reset();
        index.reset();
        delegate.reset();
    }
    void sync() {
        sceneIndices.stageSceneIndex->ApplyPendingUpdates();
        engine.Execute(index.get(), &tasks);
        scene().commit();
    }
    nova::Scene &scene() { return static_cast<HdNovaRenderParam *>(delegate->GetRenderParam())->GetScene(); }
    // The live mesh record whose user id belongs to `path`'s rprim.
    nova::MeshRecord const &record(char const *path) {
        const int id = index->GetRprim(SdfPath(path)) ? index->GetRprim(SdfPath(path))->GetPrimId() : -2;
        for (uint32_t m = 0; m < scene().meshCount(); ++m)
            if (scene().meshRecord(m).alive && scene().meshRecord(m).desc.userId == id)
                return scene().meshRecord(m);
        FAIL("no mesh for " << path);
        static nova::MeshRecord none;
        return none;
    }
};

std::string mesh(char const *name, std::string const &extra) {
    return std::string("def Mesh \"") + name + R"("
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    uniform token subdivisionScheme = "none"
)" + extra + "\n}\n";
}

std::string visibility(char const *expression) {
    return std::string("    string primvars:karma:object:rendervisibility = \"") + expression +
           "\" (interpolation = \"constant\")";
}

} // namespace

TEST_CASE("Hydra reads Karma's rendervisibility and holdoutmode primvars", "[hydra][holdout]") {
    Driver d(mesh("phantom", visibility("-primary")) + mesh("catcher", visibility("primary") + R"(
    int primvars:karma:object:holdoutmode = 2 (interpolation = "constant"))") +
             mesh("matte", R"(    token primvars:karma:object:holdoutmode = "Matte" (interpolation = "constant"))") +
             mesh("plain", "") + mesh("bad", visibility("primary|")) + R"(
def Sphere "ball"
{
    double radius = 1
    string primvars:karma:object:rendervisibility = "-shadow" (interpolation = "constant")
    int primvars:karma:object:holdoutmode = 1 (interpolation = "constant")
}
def BasisCurves "hair"
{
    int[] curveVertexCounts = [2]
    point3f[] points = [(0, 0, 0), (0, 1, 0)]
    float[] widths = [0.1] (interpolation = "constant")
    uniform token type = "linear"
    string primvars:karma:object:rendervisibility = "" (interpolation = "constant")
}
def Points "dots"
{
    point3f[] points = [(0, 0, 0)]
    float[] widths = [0.1] (interpolation = "constant")
    int primvars:karma:object:holdoutmode = 2 (interpolation = "constant")
}
)");
    using nova::HoldoutMode;
    CHECK(d.record("/phantom").visibility == (nova::kVisibleAll & ~nova::kVisibleCamera));
    CHECK(d.record("/phantom").holdout == HoldoutMode::None);
    CHECK(d.record("/phantom").filtered);
    CHECK(d.record("/catcher").visibility == nova::kVisibleCamera);
    CHECK(d.record("/catcher").holdout == HoldoutMode::Background);
    CHECK(d.record("/matte").holdout == HoldoutMode::Matte);
    CHECK(d.record("/matte").visibility == nova::kVisibleAll);
    CHECK_FALSE(d.record("/matte").filtered);
    CHECK(d.record("/plain").visibility == nova::kVisibleAll);
    CHECK(d.record("/plain").holdout == HoldoutMode::None);
    CHECK_FALSE(d.record("/plain").filtered);
    CHECK(d.record("/bad").visibilitySettings.mask == -1); // a syntax error warns and stays unset
    CHECK(d.record("/bad").visibility == nova::kVisibleAll);
    CHECK(d.record("/ball").visibility == (nova::kVisibleAll & ~nova::kVisibleShadow));
    CHECK(d.record("/ball").holdout == HoldoutMode::Matte);
    CHECK(d.record("/hair").visibility == 0);
    CHECK(d.record("/dots").holdout == HoldoutMode::Background);
    CHECK(d.scene().anyHoldouts());
    CHECK(d.scene().anyBackground());

    // An edit of the primvar reaches the scene.
    d.stage->GetPrimAtPath(SdfPath("/phantom"))
        .GetAttribute(TfToken("primvars:karma:object:rendervisibility"))
        .Set(std::string("*"));
    d.sync();
    CHECK(d.record("/phantom").visibility == nova::kVisibleAll);
    CHECK_FALSE(d.record("/phantom").filtered);
}

TEST_CASE("The render settings give the objects' visibility and holdout defaults", "[hydra][holdout]") {
    HdNovaRenderDelegate delegate;
    nova::RenderSettings s = delegate.GetNovaSettings();
    CHECK(s.objectVisibility.mask == -1);
    CHECK(s.objectVisibility.holdout == -1);
    CHECK_FALSE(s.backgroundOccludesShadows);
    delegate.SetRenderSetting(TfToken("karma:object:rendervisibility"), VtValue(std::string("primary|shadow")));
    delegate.SetRenderSetting(TfToken("karma:object:holdoutmode"), VtValue(TfToken("Background")));
    delegate.SetRenderSetting(TfToken("karma:global:bgoccludeshadow"), VtValue(true));
    s = delegate.GetNovaSettings();
    CHECK(s.objectVisibility.mask == (nova::kVisibleCamera | nova::kVisibleShadow));
    CHECK(s.objectVisibility.holdout == int(nova::HoldoutMode::Background));
    CHECK(s.backgroundOccludesShadows);
    delegate.SetRenderSetting(TfToken("karma:object:holdoutmode"), VtValue(TfToken("None")));
    CHECK(delegate.GetNovaSettings().objectVisibility.holdout == 0);
}

TEST_CASE("Karma's background image filter: the shadow catcher's alpha", "[hydra][holdout]") {
    // As the Karma Render Settings LOP authors it (Add Holdout Shadows To Beauty).
    const HdNovaBackgroundFilter f = HdNovaParseKarmaBackgroundFilter(
        R"([["background", {"destination": "C", "foreground": "C", "shadows": "holdout_shadow", )"
        R"("shadowcatcherenable": true, "shadowcatcheralpha": 0.5}]])");
    CHECK(f.shadowCatcher);
    CHECK(f.destination == "C");
    CHECK(f.shadows == "holdout_shadow");
    CHECK(f.alpha == 0.5f);
    CHECK_FALSE(f.plate);
    // The Background Plate's IPR filter: one filter, a plate to composite.
    const HdNovaBackgroundFilter ipr = HdNovaParseKarmaBackgroundFilter(
        R"(["background", {"destination": "C", "foreground": "C", "shadows": "holdout_shadows", )"
        R"("backgroundfile": "plate.pic", "extras": "holdout_diffuse"}])");
    CHECK(ipr.plate);
    CHECK_FALSE(ipr.shadowCatcher);
    CHECK_FALSE(HdNovaParseKarmaBackgroundFilter(R"([["denoise", {}]])").shadowCatcher);
}

TEST_CASE("Lights read Karma's illumbackground", "[hydra][holdout]") {
    Driver d(R"(
def DistantLight "plain"
{
}
def DistantLight "illum"
{
    bool inputs:karma:light:illumbackground = 1
}
)");
    d.scene().commit();
    int illum = 0, plain = 0;
    for (uint32_t i = 0; i < d.scene().analyticLightCount(); ++i)
        (d.scene().lights()[i].illumBackground ? illum : plain) += 1;
    CHECK(illum == 1);
    CHECK(plain == 1);
}
