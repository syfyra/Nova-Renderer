// Stage features that Nova did not apply, or not as Karma does
// (docs/bug-audit-2026-11.md, section 16).
//
// Render passes (husk --pass, the pass menu of the Solaris viewport): the
// host names the active UsdRenderPass in the scene globals, and its
// collections are the renderer's to apply. Nova applied none: every pass
// rendered the whole scene. HdNovaRenderPassSceneIndex applies prune,
// renderVisibility, cameraVisibility and matte.
//
// DomeLight_1's pole axis: Karma turns the dome to the axis after the light's
// own transform; Nova turned it before, so a rotated dome on a z-up stage
// pointed elsewhere.

#include "hydra/renderDelegate.h"
#include "hydra/renderParam.h"
#include "hydra/renderPassSceneIndex.h"

#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdsi/sceneGlobalsSceneIndex.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
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

// A stage behind scene globals and Nova's render pass index, synced into
// the delegate's scene. `first` is the pass that is active from the start.
struct Driver {
    UsdStageRefPtr stage;
    std::unique_ptr<HdNovaRenderDelegate> delegate = std::make_unique<HdNovaRenderDelegate>();
    std::unique_ptr<HdRenderIndex> index;
    UsdImagingSceneIndices sceneIndices;
    HdsiSceneGlobalsSceneIndexRefPtr globals;
    HdRenderPassSharedPtr pass;
    HdTaskSharedPtrVector tasks;
    HdEngine engine;

    explicit Driver(std::string const &body, char const *first = nullptr) {
        SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
        REQUIRE(layer->ImportFromString("#usda 1.0\n" + body));
        stage = UsdStage::Open(layer);
        index.reset(HdRenderIndex::New(delegate.get(), HdDriverVector()));
        UsdImagingCreateSceneIndicesInfo info;
        info.stage = stage;
        info.addDrawModeSceneIndex = false;
        sceneIndices = UsdImagingCreateSceneIndices(info);
        globals = HdsiSceneGlobalsSceneIndex::New(sceneIndices.finalSceneIndex);
        if (first)
            globals->SetActiveRenderPassPrimPath(SdfPath(first));
        index->InsertSceneIndex(HdNovaRenderPassSceneIndex::New(globals), SdfPath::AbsoluteRootPath(), false);
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
    void activate(char const *path) {
        globals->SetActiveRenderPassPrimPath(path ? SdfPath(path) : SdfPath());
        sync();
    }
    nova::Scene &scene() { return static_cast<HdNovaRenderParam *>(delegate->GetRenderParam())->GetScene(); }
    nova::LightData const *dome() {
        for (nova::LightData const &l : scene().lights())
            if (l.type == nova::LightType::Dome)
                return &l;
        return nullptr;
    }
    bool has(char const *path) { return index->GetRprim(SdfPath(path)) != nullptr; }
    bool shown(char const *path) { return has(path) && index->GetRprim(SdfPath(path))->IsVisible(); }
    // The live mesh record whose user id belongs to `path`'s rprim.
    nova::MeshRecord const &record(char const *path) {
        const int id = has(path) ? index->GetRprim(SdfPath(path))->GetPrimId() : -2;
        for (uint32_t m = 0; m < scene().meshCount(); ++m)
            if (scene().meshRecord(m).alive && scene().meshRecord(m).desc.userId == id)
                return scene().meshRecord(m);
        FAIL("no mesh for " << path);
        static nova::MeshRecord none;
        return none;
    }
};

std::string mesh(char const *name, char const *extra = "") {
    return std::string("def Mesh \"") + name + R"("
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    uniform token subdivisionScheme = "none"
)" + extra + "\n}\n";
}

// Meshes a to d (d invisible to shadow rays by its own setting, e inside a
// group) and the passes.
const std::string kStage =
    mesh("a") + mesh("b") + mesh("c") +
    mesh("d", "    string primvars:karma:object:rendervisibility = \"-shadow\" (interpolation = \"constant\")") +
    "def Xform \"group\"\n{\n" + mesh("e") + "}\n" + R"(
def Scope "Render"
{
    def RenderPass "prune"
    {
        rel collection:prune:includes = [</b>, </group>]
    }
    def RenderPass "camera"
    {
        uniform bool collection:cameraVisibility:includeRoot = 1
        rel collection:cameraVisibility:excludes = [</c>, </d>]
    }
    def RenderPass "visible"
    {
        uniform bool collection:renderVisibility:includeRoot = 0
        rel collection:renderVisibility:includes = [</b>, </group>]
    }
    def RenderPass "matte"
    {
        rel collection:matte:includes = </a>
    }
    def RenderPass "all"
    {
        rel collection:prune:includes = </b>
        rel collection:matte:includes = [</a>, </group>]
        uniform bool collection:cameraVisibility:includeRoot = 1
        rel collection:cameraVisibility:excludes = [</c>, </d>]
        uniform bool collection:renderVisibility:includeRoot = 1
        rel collection:renderVisibility:excludes = </c>
    }
    def RenderPass "empty"
    {
    }
}
)";

const uint8_t kNoCamera = nova::kVisibleAll & ~nova::kVisibleCamera;

void checkWhole(Driver &d) {
    for (char const *path : {"/a", "/b", "/c", "/d", "/group/e"}) {
        INFO(path);
        CHECK(d.shown(path));
        CHECK(d.record(path).holdout == nova::HoldoutMode::None);
    }
    CHECK(d.record("/a").visibility == nova::kVisibleAll);
    CHECK(d.record("/c").visibility == nova::kVisibleAll);
    CHECK(d.record("/d").visibility == (nova::kVisibleAll & ~nova::kVisibleShadow));
}

} // namespace

TEST_CASE("Without an active render pass the scene is whole", "[hydra][pass]") {
    Driver d(kStage);
    checkWhole(d);
    // A pass that says nothing, and a path that names no pass.
    d.activate("/Render/empty");
    checkWhole(d);
    d.activate("/Render/none");
    checkWhole(d);
}

TEST_CASE("The active render pass's collections are applied", "[hydra][pass]") {
    SECTION("prune: what is in it is not in the scene") {
        Driver d(kStage, "/Render/prune");
        CHECK(!d.has("/b"));
        CHECK(!d.has("/group/e"));
        CHECK(d.shown("/a"));
        CHECK(d.record("/a").visibility == nova::kVisibleAll);
    }
    SECTION("cameraVisibility: what is outside it is hidden from camera rays alone") {
        Driver d(kStage, "/Render/camera");
        CHECK(d.shown("/c"));
        CHECK(d.record("/c").visibility == kNoCamera);
        // Its own expression stays: neither shadow nor camera rays.
        CHECK(d.record("/d").visibility == (kNoCamera & ~nova::kVisibleShadow));
        CHECK(d.record("/a").visibility == nova::kVisibleAll);
        CHECK(d.record("/c").holdout == nova::HoldoutMode::None);
    }
    SECTION("renderVisibility: what is outside it is invisible") {
        Driver d(kStage, "/Render/visible");
        CHECK(!d.shown("/a"));
        CHECK(!d.shown("/c"));
        CHECK(d.shown("/b"));
        CHECK(d.shown("/group/e"));
        CHECK(d.record("/b").visibility == nova::kVisibleAll);
    }
    SECTION("matte: what is in it is a matte") {
        Driver d(kStage, "/Render/matte");
        CHECK(d.record("/a").holdout == nova::HoldoutMode::Matte);
        CHECK(d.record("/a").visibility == nova::kVisibleAll);
        CHECK(d.record("/b").holdout == nova::HoldoutMode::None);
    }
    SECTION("all four at once") {
        Driver d(kStage, "/Render/all");
        CHECK(!d.has("/b"));
        CHECK(d.record("/a").holdout == nova::HoldoutMode::Matte);
        CHECK(d.record("/group/e").holdout == nova::HoldoutMode::Matte);
        CHECK(!d.shown("/c"));
        CHECK(d.record("/d").visibility == (kNoCamera & ~nova::kVisibleShadow));
        CHECK(d.record("/d").holdout == nova::HoldoutMode::None);
    }
}

// The viewport changes the pass while the scene is up, and the pass's
// collections are edited like anything else on the stage.
TEST_CASE("Another pass, no pass and an edited pass change the scene that is up", "[hydra][pass]") {
    Driver d(kStage);
    checkWhole(d);

    d.activate("/Render/all");
    CHECK(!d.has("/b"));
    CHECK(d.record("/a").holdout == nova::HoldoutMode::Matte);
    CHECK(!d.shown("/c"));
    CHECK(d.record("/d").visibility == (kNoCamera & ~nova::kVisibleShadow));

    d.activate("/Render/camera");
    CHECK(d.has("/b"));
    CHECK(d.record("/a").holdout == nova::HoldoutMode::None);
    CHECK(d.shown("/c"));
    CHECK(d.record("/c").visibility == kNoCamera);

    d.activate("/Render/matte");
    CHECK(d.record("/c").visibility == nova::kVisibleAll);
    CHECK(d.record("/a").holdout == nova::HoldoutMode::Matte);
    CHECK(d.record("/c").holdout == nova::HoldoutMode::None);
    // The pass gets another member.
    d.stage->GetPrimAtPath(SdfPath("/Render/matte"))
        .GetRelationship(TfToken("collection:matte:includes"))
        .AddTarget(SdfPath("/c"));
    d.sync();
    CHECK(d.record("/c").holdout == nova::HoldoutMode::Matte);
    CHECK(d.record("/a").holdout == nova::HoldoutMode::Matte);

    d.activate(nullptr);
    checkWhole(d);
}

// A dome light on a stage with the up axis `up`: `type` with `pole` as its
// pole axis ("" for none), rotated by `rotate` degrees about x, y and z.
static std::string domeStage(char const *up, char const *type, char const *pole, char const *rotate) {
    std::string s = std::string("(\n    upAxis = \"") + up + "\"\n)\ndef " + type + " \"sky\"\n{\n";
    s += "    float inputs:intensity = 1\n";
    if (*pole)
        s += std::string("    uniform token poleAxis = \"") + pole + "\"\n";
    if (*rotate)
        s += std::string("    float3 xformOp:rotateXYZ = ") + rotate +
             "\n    uniform token[] xformOpOrder = [\"xformOp:rotateXYZ\"]\n";
    return s + "}\n";
}

static bool along(nova::Vec3f v, float x, float y, float z) {
    return std::fabs(v.x - x) < 1e-5f && std::fabs(v.y - y) < 1e-5f && std::fabs(v.z - z) < 1e-5f;
}

TEST_CASE("A DomeLight_1's pole axis turns the dome after the light's transform", "[hydra][dome]") {
    // The dome's pole is its +y. Without a rotation it is the axis named.
    {
        Driver d(domeStage("Y", "DomeLight", "", ""));
        REQUIRE(d.dome());
        CHECK(along(d.dome()->domeY, 0, 1, 0));
    }
    for (char const *up : {"Y", "Z"}) {
        INFO("up axis " << up);
        Driver y(domeStage(up, "DomeLight_1", "Y", "")), z(domeStage(up, "DomeLight_1", "Z", "")),
            scene(domeStage(up, "DomeLight_1", "scene", "")), classic(domeStage(up, "DomeLight", "", ""));
        REQUIRE((y.dome() && z.dome() && scene.dome() && classic.dome()));
        CHECK(along(y.dome()->domeY, 0, 1, 0));
        CHECK(along(z.dome()->domeY, 0, 0, 1));
        CHECK(along(scene.dome()->domeY, 0, *up == 'Y' ? 1.0f : 0.0f, *up == 'Z' ? 1.0f : 0.0f));
        CHECK(along(classic.dome()->domeY, 0, 1, 0)); // UsdLuxDomeLight has no pole axis
    }
    // A quarter turn about z takes +y to -x and +x to +y. With the pole at z the dome is then turned as the
    // y-up dome is, and stood up after it: its pole along -x, its +x along +z. (Turned to z first, the pole
    // would stay on z, the axis of the turn.)
    for (char const *up : {"Y", "Z"}) {
        INFO("up axis " << up);
        Driver z(domeStage(up, "DomeLight_1", "Z", "(0, 0, 90)"));
        REQUIRE(z.dome());
        CHECK(along(z.dome()->domeY, -1, 0, 0));
        CHECK(along(z.dome()->domeX, 0, 0, 1));
        Driver y(domeStage(up, "DomeLight_1", "Y", "(0, 0, 90)"));
        REQUIRE(y.dome());
        CHECK(along(y.dome()->domeY, -1, 0, 0));
        CHECK(along(y.dome()->domeX, 0, 1, 0));
    }
    {
        Driver scene(domeStage("Z", "DomeLight_1", "scene", "(0, 0, 90)"));
        REQUIRE(scene.dome());
        CHECK(along(scene.dome()->domeY, -1, 0, 0));
        CHECK(along(scene.dome()->domeX, 0, 0, 1));
    }
}
