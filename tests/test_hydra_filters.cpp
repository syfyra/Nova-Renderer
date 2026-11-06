// docs/design/M11-P4.md: Hydra's lightFilter sprim. A stage with Karma's
// blocker written as a generic UsdLux LightFilter with its shader id (the
// typed KarmaBlockerLightFilter needs Karma's schema plugin; husk covers it,
// tests/husk/check_karma_filters.py): the filter's inputs, transform and space
// reach the scene, lights list their filters in order, edits and removals
// follow, other filter kinds are left out, and filterLink is ignored as Karma
// ignores it, even where a light-linking scene index gives it a category.

#include "hydra/renderDelegate.h"
#include "hydra/renderParam.h"

#include "core/lights/lights.h"
#include "core/scene/scene.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdsi/lightLinkingSceneIndex.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <memory>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE
using Catch::Matchers::WithinAbs;

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

// pxr's light-linking scene index with light filters among its types: it
// gives filterLink collections categories, which Nova must not apply.
HdContainerDataSourceHandle linkingWithFilters() {
    VtArray<TfToken> lights{HdPrimTypeTokens->distantLight, HdPrimTypeTokens->sphereLight};
    VtArray<TfToken> filters{HdPrimTypeTokens->lightFilter};
    VtArray<TfToken> geometry{HdPrimTypeTokens->mesh};
    using Tokens = HdRetainedTypedSampledDataSource<VtArray<TfToken>>;
    return HdRetainedContainerDataSource::New(HdsiLightLinkingSceneIndexTokens->lightPrimTypes, Tokens::New(lights),
                                              HdsiLightLinkingSceneIndexTokens->lightFilterPrimTypes, Tokens::New(filters),
                                              HdsiLightLinkingSceneIndexTokens->geometryPrimTypes, Tokens::New(geometry));
}

// A stage through UsdImaging's scene indices and that light-linking scene
// index into a Nova render index.
struct Driver {
    UsdStageRefPtr stage;
    std::unique_ptr<HdNovaRenderDelegate> delegate = std::make_unique<HdNovaRenderDelegate>();
    std::unique_ptr<HdRenderIndex> index;
    UsdImagingSceneIndices sceneIndices;
    HdRenderPassSharedPtr pass;
    HdTaskSharedPtrVector tasks;
    HdEngine engine;

    explicit Driver(const char *usda) {
        SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
        layer->ImportFromString(usda);
        stage = UsdStage::Open(layer);
        index.reset(HdRenderIndex::New(delegate.get(), HdDriverVector()));
        UsdImagingCreateSceneIndicesInfo info;
        info.addDrawModeSceneIndex = false;
        sceneIndices = UsdImagingCreateSceneIndices(info);
        index->InsertSceneIndex(HdsiLightLinkingSceneIndex::New(sceneIndices.finalSceneIndex, linkingWithFilters()),
                                SdfPath::AbsoluteRootPath(), false);
        // The stage after the chain exists, as in husk: the light-linking
        // scene index sees the prims arrive.
        sceneIndices.stageSceneIndex->SetStage(stage);
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
    void set(char const *attr, VtValue const &v) { stage->GetAttributeAtPath(SdfPath(attr)).Set(v); }
    // The light-linking categories of the mesh tagged `tag` (its receiver set's).
    uint64_t categories(char const *tag) {
        nova::Scene &s = scene();
        for (nova::MeshId id = 0; id < s.meshCount(); ++id)
            if (s.meshRecord(id).alive && s.meshRecord(id).lpeTag == tag)
                return s.lightLinks().categories(s.receiverSet(id, 0));
        FAIL("no mesh tagged " << tag);
        return 0;
    }
};

const char *kStage = R"(#usda 1.0
def Mesh "a"
{
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    string primvars:karma:object:lpetag = "a" (interpolation = "constant")
}
def Mesh "b"
{
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0, 0, 2), (1, 0, 2), (0, 1, 2)]
    string primvars:karma:object:lpetag = "b" (interpolation = "constant")
}
def Camera "cam"
{
    double3 xformOp:translate = (0, 0, 7)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
def DistantLight "sun"
{
    float inputs:angle = 0
    rel light:filters = [</blk>, </gobo>, </ref>]
}
def SphereLight "lamp"
{
    double3 xformOp:translate = (0, 5, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
def LightFilter "blk" (
    prepend apiSchemas = ["CollectionAPI:filterLink"]
)
{
    uniform token lightFilter:shaderId = "kma_lfilter_blocker"
    token inputs:shape = "box"
    float inputs:width = 4
    float inputs:height = 2
    float inputs:depth = 3
    float inputs:roundedcorner = 0.5
    float inputs:falloff = 2
    float inputs:intensity = 0.25
    float inputs:exposure = 1
    color3f inputs:tint = (1, 0.5, 0.25)
    int inputs:invert = 1
    string inputs:space = "/blk"
    double3 xformOp:translate = (1, 2, 3)
    uniform token[] xformOpOrder = ["xformOp:translate"]
    uniform bool collection:filterLink:includeRoot = 0
    rel collection:filterLink:includes = </a>
}
def LightFilter "gobo"
{
    uniform token lightFilter:shaderId = "kma_lfilter_gobo"
}
def LightFilter "ref"
{
    uniform token lightFilter:shaderId = "kma_lfilter_blocker"
    float inputs:intensity = 0
    string inputs:space = "/blk"
    uniform bool collection:filterLink:includeRoot = 0
}
)";

} // namespace

TEST_CASE("Blocker light filters reach the scene", "[hydra][filters]") {
    Driver d(kStage);
    nova::Scene &scene = d.scene();
    // The sun first (id order), with the blocker and the second blocker; the
    // gobo is not supported and left out.
    const auto sun = [&]() -> const nova::LightData & {
        for (const nova::LightData &l : scene.lights())
            if (l.type == nova::LightType::Distant)
                return l;
        FAIL("no distant light");
        return scene.lights()[0];
    };
    REQUIRE(sun().filterCount == 2);
    for (const nova::LightData &l : scene.lights())
        if (l.type == nova::LightType::Sphere)
            CHECK(l.filterCount == 0);
    const nova::LightFilterData &blk = sun().filters[0];
    CHECK(blk.shape == nova::BlockerShape::Box);
    CHECK_THAT(blk.half.x, WithinAbs(2.0, 1e-6));
    CHECK_THAT(blk.half.y, WithinAbs(1.0, 1e-6));
    CHECK_THAT(blk.half.z, WithinAbs(1.5, 1e-6));
    CHECK_THAT(blk.invSize, WithinAbs(1.0, 1e-6));  // the smallest half extent
    CHECK_THAT(blk.rounded, WithinAbs(0.5, 1e-6));  // its share of it
    CHECK_THAT(blk.falloff, WithinAbs(1.0, 1e-6));  // clamped as Karma
    CHECK(blk.invert == 1u);
    CHECK_THAT(blk.blocked.x, WithinAbs(0.5, 1e-6)); // 0.25 x 2^1 x tint
    CHECK_THAT(blk.blocked.y, WithinAbs(0.25, 1e-6));
    CHECK_THAT(blk.blocked.z, WithinAbs(0.125, 1e-6));
    // Its own space: the inverse of its transform.
    const nova::Vec3f q = blk.toSpace({1.0f, 2.0f, 3.0f});
    CHECK_THAT(q.x, WithinAbs(0.0, 1e-6));
    CHECK_THAT(q.y, WithinAbs(0.0, 1e-6));
    CHECK_THAT(q.z, WithinAbs(0.0, 1e-6));
    // The second blocker's space is the first filter: the same frame.
    const nova::Vec3f q2 = sun().filters[1].toSpace({1.0f, 2.0f, 3.0f});
    CHECK_THAT(q2.x, WithinAbs(0.0, 1e-6));
    CHECK_THAT(q2.z, WithinAbs(0.0, 1e-6));

    // filterLink: ignored as Karma ignores it. The scene index gives the
    // first blocker's collection (naming a) a category and a that category;
    // the filters still act on every object, and the scene has no linking.
    CHECK(blk.link == nova::kLinksEveryone);
    CHECK(sun().filters[1].link == nova::kLinksEveryone);
    CHECK(d.categories("a") == 0);
    CHECK(d.categories("b") == 0);
    CHECK(scene.lightLinks().light == nullptr);

    SECTION("edits follow") {
        d.set("/blk.inputs:intensity", VtValue(0.5f));
        d.set("/blk.inputs:shape", VtValue(TfToken("capsule")));
        d.sync();
        REQUIRE(sun().filterCount == 2);
        CHECK(sun().filters[0].shape == nova::BlockerShape::Capsule);
        CHECK_THAT(sun().filters[0].blocked.x, WithinAbs(1.0, 1e-6));
    }
    SECTION("a removed filter leaves the light") {
        d.stage->RemovePrim(SdfPath("/blk"));
        d.sync();
        REQUIRE(sun().filterCount == 1);
        CHECK(sun().filters[0].link == nova::kLinksEveryone);
    }
    SECTION("the light's filter list follows") {
        d.stage->GetPrimAtPath(SdfPath("/sun")).GetRelationship(TfToken("light:filters")).SetTargets({SdfPath("/ref")});
        d.sync();
        REQUIRE(sun().filterCount == 1);
        CHECK(sun().filters[0].link == nova::kLinksEveryone);
    }
}

// The space a blocker's shape lives in (Karma, measured): another light
// filter (or a camera, tests/husk/check_karma_filters.py) by the inverse of its
// transform, a light by its transform itself, anything else (geometry, an
// Xform, an empty or unknown path) world.
TEST_CASE("Blocker spaces resolve as Karma's", "[hydra][filters]") {
    Driver d(R"(#usda 1.0
def Xform "xf"
{
    double3 xformOp:translate = (5, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
def Sphere "geo"
{
    double3 xformOp:translate = (0, 3, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
def SphereLight "spc"
{
    float inputs:intensity = 0
    double3 xformOp:translate = (-2, 0, 1)
    float3 xformOp:scale = (1.5, 1.5, 1.5)
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]
}
def DistantLight "sun"
{
    rel light:filters = [</f/own>, </f/other>, </f/geo>, </f/light>, </f/xf>, </f/none>, </f/named>]
}
def Scope "f"
{
    def LightFilter "own"
    {
        uniform token lightFilter:shaderId = "kma_lfilter_blocker"
        double3 xformOp:translate = (1, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def LightFilter "other"
    {
        uniform token lightFilter:shaderId = "kma_lfilter_blocker"
        string inputs:space = "/f/own"
    }
    def LightFilter "geo"
    {
        uniform token lightFilter:shaderId = "kma_lfilter_blocker"
        string inputs:space = "/geo"
    }
    def LightFilter "light"
    {
        uniform token lightFilter:shaderId = "kma_lfilter_blocker"
        string inputs:space = "/spc"
    }
    def LightFilter "xf"
    {
        uniform token lightFilter:shaderId = "kma_lfilter_blocker"
        string inputs:space = "/xf"
        double3 xformOp:translate = (7, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def LightFilter "none"
    {
        uniform token lightFilter:shaderId = "kma_lfilter_blocker"
        string inputs:space = ""
        double3 xformOp:translate = (7, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    def LightFilter "named"
    {
        uniform token lightFilter:shaderId = "kma_lfilter_blocker"
        string inputs:space = "space:world"
    }
}
)");
    const nova::LightData *sun = nullptr;
    for (const nova::LightData &l : d.scene().lights())
        if (l.type == nova::LightType::Distant)
            sun = &l;
    REQUIRE(sun);
    REQUIRE(sun->filterCount == 7);
    const auto at = [&](uint32_t i, nova::Vec3f p) { return sun->filters[i].toSpace(p); };
    const auto same = [](nova::Vec3f a, nova::Vec3f b) {
        return std::fabs(a.x - b.x) < 1e-5f && std::fabs(a.y - b.y) < 1e-5f && std::fabs(a.z - b.z) < 1e-5f;
    };
    CHECK(same(at(0, {1, 0, 0}), {0, 0, 0}));        // its own prim (the default)
    CHECK(same(at(1, {1, 0, 0}), {0, 0, 0}));        // another filter
    CHECK(same(at(2, {0, 3, 0}), {0, 3, 0}));        // geometry: world
    CHECK(same(at(3, {0, 0, 0}), {-2, 0, 1}));       // a light: its transform itself
    CHECK(same(at(3, {1, 0, 0}), {-0.5f, 0, 1}));
    CHECK(same(at(4, {5, 0, 0}), {5, 0, 0}));        // an Xform: world
    CHECK(same(at(5, {7, 0, 0}), {7, 0, 0}));        // "": world
    CHECK(same(at(6, {3, 2, 1}), {3, 2, 1}));        // a named space: world (warned)
}
