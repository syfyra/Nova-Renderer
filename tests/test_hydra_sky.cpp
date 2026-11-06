// Karma's sky dome through Hydra (docs/design/M11-P3.md): Houdini's Karma
// Sky Dome Light authors a KarmaSkyDomeLight prim, which Hydra delivers as a
// generic `light` sprim. Nova recognises it by its sky inputs, bakes the sky
// into a dome texture and re-bakes on edits of the sky's inputs only; other
// generic lights are ignored. The rig drives a real render index through
// UsdImaging's scene indices, as Solaris does.

#include "hydra/renderDelegate.h"
#include "hydra/renderParam.h"

#include "core/lights/lights.h"
#include "core/lights/sky.h"
#include "core/scene/scene.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
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
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <memory>

PXR_NAMESPACE_USING_DIRECTIVE
using Catch::Matchers::WithinRel;

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

// The inputs the Karma Sky Dome Light LOP authors (Render Light Geometry
// on), a generic light that is not a sky, and a quad. Test processes do not
// load Houdini's USD plugins, where the KarmaSkyDomeLight schema (an
// xformable prim with LightAPI built in) lives, so the sky is an Xform that
// applies LightAPI: Hydra delivers both as the same generic light (husk
// renders the real prim, tests/husk/check_karma_sky.py).
const char *kSkyScene = R"(#usda 1.0
def Xform "sky" (
    prepend apiSchemas = ["LightAPI", "KarmaLightAPI"]
)
{
    color3f inputs:color = (1, 0.5, 0.25)
    float inputs:intensity = 2
    color3f inputs:ground_albedo = (0.5, 0.5, 0.5)
    color3f inputs:ground_color = (0.3, 0.2, 0.1)
    float inputs:horizon_blur = 0.5
    float inputs:solar_altitude = 30
    float inputs:solar_azimuth = 0
    float inputs:turbidity = 4
    bool inputs:karma:light:renderlightgeo = 1
}
def Xform "notSky" (
    prepend apiSchemas = ["LightAPI"]
)
{
    float inputs:intensity = 5
}
def Mesh "quad"
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-1, 0, -1), (1, 0, -1), (1, 0, 1), (-1, 0, 1)]
}
)";

struct SkyDriver {
    UsdStageRefPtr stage;
    std::unique_ptr<HdNovaRenderDelegate> delegate = std::make_unique<HdNovaRenderDelegate>();
    std::unique_ptr<HdRenderIndex> index;
    UsdImagingSceneIndices sceneIndices;
    HdRenderPassSharedPtr pass;
    HdTaskSharedPtrVector tasks;
    HdEngine engine;

    SkyDriver() {
        SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
        layer->ImportFromString(kSkyScene);
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
        sync();
    }
    ~SkyDriver() {
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
    // The Karma sky the scene holds (its dome light).
    nova::LightData const *sky() {
        for (nova::LightData const &l : scene().lights())
            if (l.type == nova::LightType::Dome)
                return &l;
        return nullptr;
    }
};

// The texel at (u, v) of the dome against the model at the texel's center.
void checkTexel(nova::LightData const &dome, nova::KarmaSkyDesc const &expect, float u, float v) {
    const int x = int(u * float(dome.texWidth)), y = int(v * float(dome.texHeight));
    float sinTheta = 0.0f;
    const nova::Vec2f uv{(float(x) + 0.5f) / float(dome.texWidth), (float(y) + 0.5f) / float(dome.texHeight)};
    const nova::Vec3f dir = nova::domeDirection(dome, uv, sinTheta);
    const nova::Spectrum want = nova::karmaSkyRadiance(expect, dir);
    const nova::Spectrum got = dome.texels[size_t(y) * dome.texWidth + size_t(x)];
    for (int c = 0; c < 3; ++c)
        CHECK_THAT(got[c], WithinRel(want[c], 1e-5f));
}

} // namespace

TEST_CASE("Karma's sky dome arrives as a generic light", "[hydra][sky]") {
    SkyDriver d;
    // The sky only: the generic light that is not a Karma sky is ignored.
    REQUIRE(d.scene().lights().size() == 1);
    nova::LightData const *sky = d.sky();
    REQUIRE(sky);
    CHECK(sky->cameraVisible);
    REQUIRE(sky->texels);
    CHECK(sky->texWidth == 2048); // the default resolution
    CHECK(sky->texHeight == 1024);
    // The texture holds the final radiance.
    CHECK(sky->emission.x == 1.0f);
    CHECK(sky->emission.z == 1.0f);

    nova::KarmaSkyDesc expect;
    expect.solarAltitude = 30.0f;
    expect.turbidity = 4.0f;
    expect.groundColor = {0.3f, 0.2f, 0.1f};
    expect.lightColor = {2.0f, 1.0f, 0.5f};
    checkTexel(*sky, expect, 0.5f, 0.39f); // low sky along +Z, away from the sun
    checkTexel(*sky, expect, 0.01f, 0.39f); // low sky along -Z, towards the sun
    checkTexel(*sky, expect, 0.1f, 0.2f);  // high sky
    checkTexel(*sky, expect, 0.7f, 0.8f);  // ground (unscaled: computegroundcolor)
    nova::Spectrum const *texels = sky->texels;

    SECTION("a sky input edit re-bakes the sky") {
        d.set("/sky.inputs:solar_altitude", VtValue(10.0f));
        d.sync();
        sky = d.sky();
        REQUIRE(sky);
        CHECK(sky->texels != texels);
        expect.solarAltitude = 10.0f;
        checkTexel(*sky, expect, 0.5f, 0.39f);
    }
    SECTION("an intensity edit re-bakes the sky") {
        d.set("/sky.inputs:intensity", VtValue(4.0f));
        d.sync();
        sky = d.sky();
        REQUIRE(sky);
        expect.lightColor = {4.0f, 2.0f, 1.0f};
        checkTexel(*sky, expect, 0.5f, 0.39f);
        checkTexel(*sky, expect, 0.7f, 0.8f); // the ground stays
    }
    SECTION("computegroundcolor off scales the ground by the light color") {
        d.stage->GetPrimAtPath(SdfPath("/sky"))
            .CreateAttribute(TfToken("inputs:computegroundcolor"), SdfValueTypeNames->Int)
            .Set(0);
        d.sync();
        sky = d.sky();
        REQUIRE(sky);
        expect.computeGroundColor = false;
        checkTexel(*sky, expect, 0.7f, 0.8f);
    }
    SECTION("a transform edit keeps the baked sky") {
        UsdPrim prim = d.stage->GetPrimAtPath(SdfPath("/sky"));
        prim.CreateAttribute(TfToken("xformOp:rotateY"), SdfValueTypeNames->Float).Set(90.0f);
        prim.CreateAttribute(TfToken("xformOpOrder"), SdfValueTypeNames->TokenArray)
            .Set(VtTokenArray{TfToken("xformOp:rotateY")});
        d.sync();
        sky = d.sky();
        REQUIRE(sky);
        CHECK(sky->texels == texels);
        CHECK_THAT(sky->domeX.z, WithinRel(-1.0f, 1e-5f)); // local +X turned to world -Z
    }
    SECTION("removing the sky removes the light") {
        d.stage->RemovePrim(SdfPath("/sky"));
        d.sync();
        CHECK(d.scene().lights().empty());
    }
}
