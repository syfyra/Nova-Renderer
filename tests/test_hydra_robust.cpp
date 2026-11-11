// Broken input through Hydra: a stage with inconsistent or non-finite data
// (a mesh whose indices run past its points, a NaN light parameter, a
// prototype relationship that leads back to its instancer) must leave out
// what it cannot render, not take the host down. Each case crashed, hung or
// never finished before; found by rendering malformed stages with nova_cli
// (docs/bug-audit-2026-11.md, section 12).

#include "hydra/renderDelegate.h"
#include "hydra/renderParam.h"

#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

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

// A stage from USDA text, synced into the delegate's scene.
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
        REQUIRE(layer->ImportFromString("#usda 1.0\n" + body));
        stage = UsdStage::Open(layer);
        index.reset(HdRenderIndex::New(delegate.get(), HdDriverVector()));
        UsdImagingCreateSceneIndicesInfo info;
        info.stage = stage;
        info.addDrawModeSceneIndex = false;
        sceneIndices = UsdImagingCreateSceneIndices(info);
        index->InsertSceneIndex(sceneIndices.finalSceneIndex, SdfPath::AbsoluteRootPath(), false);
        pass = delegate->CreateRenderPass(index.get(),
                                          HdRprimCollection(HdTokens->geometry, HdReprSelector(HdReprTokens->refined)));
        tasks.push_back(std::make_shared<SyncTask>(pass));
        sceneIndices.stageSceneIndex->ApplyPendingUpdates();
        engine.Execute(index.get(), &tasks);
        scene().commit();
    }
    ~Driver() {
        tasks.clear();
        pass.reset();
        index.reset();
        delegate.reset();
    }
    nova::Scene &scene() { return static_cast<HdNovaRenderParam *>(delegate->GetRenderParam())->GetScene(); }
    // The meshes that render: with triangles and at least one instance.
    size_t meshes() {
        size_t n = 0;
        for (nova::MeshId i = 0; i < scene().meshCount(); ++i) {
            nova::MeshRecord const &record = scene().meshRecord(i);
            n += record.alive && !record.desc.indices.empty() && record.instanceCount() > 0 ? 1 : 0;
        }
        return n;
    }
    size_t instances() {
        size_t n = 0;
        for (nova::MeshId i = 0; i < scene().meshCount(); ++i)
            if (scene().meshRecord(i).alive)
                n += scene().meshRecord(i).instanceCount();
        return n;
    }
};

// A quad named `name` with the given topology (4 points).
std::string quad(char const *name, char const *counts, char const *indices, char const *extra = "") {
    return std::string("def Mesh \"") + name + "\"\n{\n    int[] faceVertexCounts = " + counts +
           "\n    int[] faceVertexIndices = " + indices +
           "\n    point3f[] points = [(-1, 0, 0), (1, 0, 0), (1, 2, 0), (-1, 2, 0)]\n" + extra + "\n}\n";
}

} // namespace

// Hydra's topology helpers trust the arrays. An index of INT_MAX wrapped
// HdMeshTopology::GetNumPoints() (the largest index plus one) to INT_MIN,
// which sized Hd_VertexAdjacency's table: an uncaught std::length_error. A
// face count of INT_MAX made HdMeshUtil triangulate two billion corners past
// the end of the indices: a hang, then memory exhaustion.
TEST_CASE("Meshes with inconsistent topology are left out", "[hydra][robust]") {
    std::string bad;
    SECTION("an index of INT_MAX") { bad = quad("bad", "[4]", "[0, 1, 2147483647, 3]"); }
    SECTION("an index beyond the points") { bad = quad("bad", "[4]", "[0, 1, 2, 99]"); }
    SECTION("a negative index") { bad = quad("bad", "[4]", "[0, -1, 2, 3]"); }
    SECTION("a face count of INT_MAX") { bad = quad("bad", "[2147483647]", "[0, 1, 2, 3]"); }
    SECTION("face counts beyond the indices") { bad = quad("bad", "[4, 4, 3]", "[0, 1, 2, 3]"); }
    SECTION("a negative face count") { bad = quad("bad", "[-4]", "[0, 1, 2, 3]"); }
    SECTION("an index of INT_MAX, polygon mesh") {
        bad = quad("bad", "[4]", "[0, 1, 2147483647, 3]", "    uniform token subdivisionScheme = \"none\"");
    }
    SECTION("a face count of INT_MAX, polygon mesh") {
        bad = quad("bad", "[2147483647]", "[0, 1, 2, 3]", "    uniform token subdivisionScheme = \"none\"");
    }
    Driver driver(quad("ok", "[4]", "[0, 1, 2, 3]") + bad);
    CHECK(driver.meshes() == 1); // the quad beside it
}

// Trailing indices no face uses and faces of fewer than three vertices are
// no inconsistency: the mesh renders its other faces.
TEST_CASE("Meshes with unused indices or degenerate faces still render", "[hydra][robust]") {
    std::string mesh;
    SECTION("unused trailing indices") { mesh = quad("m", "[3]", "[0, 1, 2, 3, 0, 2]"); }
    SECTION("faces of 0, 1 and 2 vertices") { mesh = quad("m", "[0, 1, 2, 4, 0]", "[0, 0, 1, 0, 1, 2, 3]"); }
    SECTION("polygon mesh with faces of 0, 1 and 2 vertices") {
        mesh = quad("m", "[0, 1, 2, 4, 0]", "[0, 0, 1, 0, 1, 2, 3]", "    uniform token subdivisionScheme = \"none\"");
    }
    Driver driver(mesh);
    CHECK(driver.meshes() == 1);
}

// UsdLuxBlackbodyTemperatureAsRgb clamps its argument, but a NaN passes the
// clamp and indexes its table at INT_MIN: an access violation in the sync.
TEST_CASE("A light with a NaN color temperature emits nothing", "[hydra][robust]") {
    Driver driver(R"(
def SphereLight "broken"
{
    bool inputs:enableColorTemperature = 1
    float inputs:colorTemperature = nan
    float inputs:intensity = 10
}
def SphereLight "warm"
{
    bool inputs:enableColorTemperature = 1
    float inputs:colorTemperature = 3000
    float inputs:intensity = 10
    double3 xformOp:translate = (5, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
def SphereLight "clamped"
{
    bool inputs:enableColorTemperature = 1
    float inputs:colorTemperature = -inf
    float inputs:intensity = 10
    double3 xformOp:translate = (10, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}
)");
    const std::vector<nova::LightData> &lights = driver.scene().lights();
    REQUIRE(lights.size() == 3);
    int dark = 0, lit = 0;
    for (nova::LightData const &light : lights) {
        CHECK(nova::isFinite(light.emission));
        const bool emits = light.emission.x > 0.0f || light.emission.y > 0.0f || light.emission.z > 0.0f;
        (emits ? lit : dark) += 1;
        if (emits)
            CHECK(light.emission.x > light.emission.z); // a warm blackbody (USD clamps below 1000 K)
    }
    CHECK(dark == 1); // the NaN one
    CHECK(lit == 2);
}
