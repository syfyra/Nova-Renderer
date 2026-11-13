// Karma's render properties as fallbacks of Nova's (docs/design/M11-P1.md):
// Nova's settings win only when authored, Karma's stand in otherwise, Karma
// render settings bring Karma's own defaults where Nova's differ, the
// per-object Karma primvars of the Render Geometry Settings LOP reach every
// prim type, and unsupported image-changing Karma settings warn once.

#include "hydra/karmaSettings.h"
#include "hydra/motion.h"
#include "hydra/renderDelegate.h"
#include "hydra/renderParam.h"
#include "hydra/textureSystem.h"
#include "hydra/velocitySceneIndex.h"

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>

#include "core/film/film.h"
#include "core/integrator/guiding.h"

#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/tf/stringUtils.h>
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
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using Catch::Matchers::WithinAbs;

namespace {

void set(HdNovaRenderDelegate &d, char const *key, VtValue const &v) { d.SetRenderSetting(TfToken(key), v); }

// Collects the Tf warnings issued while it lives.
class WarningCatcher final : public TfDiagnosticMgr::Delegate {
public:
    WarningCatcher() { TfDiagnosticMgr::GetInstance().AddDelegate(this); }
    ~WarningCatcher() override { TfDiagnosticMgr::GetInstance().RemoveDelegate(this); }
    void IssueError(TfError const &) override {}
    void IssueFatalError(TfCallContext const &, std::string const &) override {}
    void IssueStatus(TfStatus const &) override {}
    void IssueWarning(TfWarning const &w) override { warnings.push_back(w.GetCommentary()); }
    int count(char const *text) const {
        int n = 0;
        for (std::string const &w : warnings)
            n += w.find(text) != std::string::npos;
        return n;
    }
    std::vector<std::string> warnings;
};

} // namespace

TEST_CASE("Karma's value forms", "[hydra][karma][settings]") {
    CHECK(HdNovaMenuIndex(VtValue(TfToken("Rounded Curves")), {"Flat Ribbons", "Rounded Curves"}) == 1);
    CHECK(HdNovaMenuIndex(VtValue(std::string(" flat ribbons ")), {"Flat Ribbons", "Rounded Curves"}) == 0);
    CHECK(HdNovaMenuIndex(VtValue(1), {"No", "Yes", "Auto"}) == 1);
    CHECK(HdNovaMenuIndex(VtValue(2.0f), {"No", "Yes", "Auto"}) == 2);
    CHECK(HdNovaMenuIndex(VtValue(TfToken("2")), {"No", "Yes", "Auto"}) == 2);
    CHECK(HdNovaMenuIndex(VtValue(TfToken("Sideways")), {"No", "Yes"}) == -1);
    CHECK(HdNovaMenuIndex(VtValue(7), {"No", "Yes"}) == -1);
    CHECK(HdNovaFlag(VtValue(true)).value());
    CHECK_FALSE(HdNovaFlag(VtValue(0)).value());
    CHECK(HdNovaFlag(VtValue(std::string("on"))).value());
    CHECK_FALSE(HdNovaFlag(VtValue()).has_value());
    CHECK(HdNovaNumber(VtValue(VtArray<float>{3.5f})).value() == 3.5);
    CHECK_FALSE(HdNovaNumber(VtValue(std::string("gauss"))).has_value());

    const HdNovaKarmaOracle v =
        HdNovaParseKarmaPixelOracle(R"(["variance", {"minrays": 4, "plane": "C", "variance": 0.02}])");
    CHECK(v.valid);
    CHECK(v.variance);
    CHECK_THAT(v.threshold, WithinAbs(0.02, 1e-6));
    CHECK(v.minRays == 4);
    const HdNovaKarmaOracle u = HdNovaParseKarmaPixelOracle(R"(["uniform",{}])");
    CHECK((u.valid && !u.variance));
    CHECK(HdNovaParseKarmaPixelOracle("variance").variance);
    CHECK_FALSE(HdNovaParseKarmaPixelOracle("[broken").valid);
    CHECK(HdNovaKarmaFilterName(R"(["blackman", {"width": 3}])") == "blackman");
    CHECK(HdNovaKarmaFilterName("Box") == "box");
}

TEST_CASE("Nova's settings win when authored, also at their defaults", "[hydra][karma][settings]") {
    HdNovaRenderDelegate d;
    CHECK_FALSE(d.IsSettingAuthored(HdNovaSettingsTokens->samples)); // a descriptor default
    set(d, "karma:object:diffuselimit", VtValue(3.0f));
    set(d, "karma:global:randomseed", VtValue(7));
    set(d, "karma:global:pixelfilter", VtValue(std::string("box")));
    set(d, "karma:global:pixelfilterscale", VtValue(2.0f));
    set(d, "karma:global:light_sampling_mode", VtValue(TfToken("Uniform")));
    nova::RenderSettings s = d.GetNovaSettings();
    CHECK(s.limits.limit[int(nova::LimitClass::Diffuse)] == 3.0f);
    CHECK(s.seed == 7u);
    CHECK(s.pixelFilter == nova::PixelFilterType::Box);
    CHECK(s.pixelFilterScale == 2.0f);
    CHECK_FALSE(s.lightTree);
    // Nova's own values, equal to their defaults, win.
    d.SetRenderSetting(HdNovaSettingsTokens->diffuseLimit, VtValue(1.0f));
    d.SetRenderSetting(HdNovaSettingsTokens->seed, VtValue(0));
    d.SetRenderSetting(HdNovaSettingsTokens->pixelFilter, VtValue(TfToken("gauss")));
    d.SetRenderSetting(HdNovaSettingsTokens->pixelFilterScale, VtValue(1.0f));
    d.SetRenderSetting(HdNovaSettingsTokens->lightSamplingMode, VtValue(TfToken("lighttree")));
    CHECK(d.IsSettingAuthored(HdNovaSettingsTokens->seed));
    s = d.GetNovaSettings();
    CHECK(s.limits.limit[int(nova::LimitClass::Diffuse)] == 1.0f);
    CHECK(s.seed == 0u);
    CHECK(s.pixelFilter == nova::PixelFilterType::Gaussian);
    CHECK(s.pixelFilterScale == 1.0f);
    CHECK(s.lightTree);
    // A settings map handed to the constructor is authored too.
    HdRenderSettingsMap map;
    map[HdNovaSettingsTokens->samples] = VtValue(64);
    map[TfToken("karma:global:pathtracedsamples")] = VtValue(16);
    map[TfToken("karma:global:convergence_mode")] = VtValue(TfToken("Path Traced"));
    HdNovaRenderDelegate withMap(map);
    CHECK(withMap.GetNovaSettings().maxSamples == 64u);
}

TEST_CASE("Karma's samples and pixel oracle", "[hydra][karma][settings][sampling]") {
    {
        // The Karma Render Settings LOP's defaults: Variance, 9 primary x up
        // to 9 secondary samples, the variance oracle at 0.01.
        HdNovaRenderDelegate d;
        set(d, "karma:global:convergence_mode", VtValue(TfToken("Variance")));
        set(d, "karma:global:samplesperpixel", VtValue(9));
        set(d, "karma:object:varianceaa_maxsamples", VtValue(9));
        set(d, "karma:global:pixeloracle", VtValue(std::string(R"(["variance", {"minrays": -1, "variance": 0.01}])")));
        const nova::RenderSettings s = d.GetNovaSettings();
        CHECK(s.maxSamples == 81u);
        CHECK(s.adaptive);
        CHECK_THAT(s.oracleThreshold, WithinAbs(0.01, 1e-6));
        CHECK(s.oracleMinSamples == 0u); // automatic
    }
    {
        // Path Traced with the uniform oracle: exactly pathtracedsamples.
        HdNovaRenderDelegate d;
        set(d, "karma:global:convergence_mode", VtValue(TfToken("Path Traced")));
        set(d, "karma:global:pathtracedsamples", VtValue(24));
        set(d, "karma:global:pixeloracle", VtValue(std::string(R"(["uniform", {"variance": 0.01}])")));
        const nova::RenderSettings s = d.GetNovaSettings();
        CHECK(s.maxSamples == 24u);
        CHECK_FALSE(s.adaptive);
    }
    {
        // Variance with a uniform oracle: the secondary threshold, at least
        // the primaries times the minimum secondaries.
        HdNovaRenderDelegate d;
        set(d, "karma:global:samplesperpixel", VtValue(4));
        set(d, "karma:object:varianceaa_minsamples", VtValue(2));
        set(d, "karma:object:varianceaa_maxsamples", VtValue(6));
        set(d, "karma:object:varianceaa_thresh", VtValue(0.05f));
        set(d, "karma:global:pixeloracle", VtValue(std::string(R"(["uniform", {}])")));
        const nova::RenderSettings s = d.GetNovaSettings();
        CHECK(s.maxSamples == 24u);
        CHECK(s.adaptive);
        CHECK_THAT(s.oracleThreshold, WithinAbs(0.05, 1e-6));
        CHECK(s.oracleMinSamples == 8u);
    }
    {
        // Nova's samples and oracle win.
        HdNovaRenderDelegate d;
        set(d, "karma:global:samplesperpixel", VtValue(4));
        d.SetRenderSetting(HdNovaSettingsTokens->samples, VtValue(64));
        const nova::RenderSettings s = d.GetNovaSettings();
        CHECK(s.maxSamples == 64u);
        CHECK_FALSE(s.adaptive);
    }
}

TEST_CASE("Karma render settings bring Karma's defaults", "[hydra][karma][settings]") {
    {
        HdNovaRenderDelegate d; // Nova's own
        const nova::RenderSettings s = d.GetNovaSettings();
        CHECK_FALSE(s.limits.fakeCaustics);
        CHECK(s.limits.internalReflection);
        CHECK(s.limits.causticsRoughnessClamp == 0.0f);
        CHECK_FALSE(s.constrainRoughness);
        CHECK(s.clampIndirect == 10.0f);
        CHECK(s.clampDirect == 0.0f);
        CHECK_FALSE(s.clampEmissionIndirect);
    }
    {
        HdNovaRenderDelegate d; // any karma:global value: a Karma render settings prim
        set(d, "karma:global:bucketsize", VtValue(32));
        const nova::RenderSettings s = d.GetNovaSettings();
        CHECK(s.limits.fakeCaustics);
        CHECK(s.limits.fakeStyle.karma);
        CHECK_FALSE(s.limits.internalReflection);
        CHECK_THAT(s.limits.causticsRoughnessClamp, WithinAbs(0.2, 1e-6));
        CHECK(s.constrainRoughness);
        CHECK(s.clampDirect == 20.0f);
        CHECK(s.clampIndirect == 20.0f);
        CHECK(s.clampEmissionIndirect);
        CHECK(s.maxSamples == 81u); // Variance: 9 x 9
    }
    {
        // Karma's authored values; Nova's switches win.
        HdNovaRenderDelegate d;
        set(d, "karma:object:causticsenable", VtValue(true));
        set(d, "karma:object:internalreflectenable", VtValue(true));
        set(d, "karma:global:constrainmaxrough", VtValue(false));
        set(d, "karma:global:colorlimit", VtValue(4.0f));
        set(d, "karma:global:colorlimitindirect", VtValue(2.0f));
        set(d, "karma:global:colorlimitindlinked", VtValue(false));
        set(d, "karma:object:fakecausticscolor", VtValue(GfVec3f(0.5f, 0.25f, 1.0f)));
        set(d, "karma:object:fakecausticsfresneldarken", VtValue(0.5f));
        nova::RenderSettings s = d.GetNovaSettings();
        CHECK_FALSE(s.limits.fakeCaustics);
        CHECK(s.limits.internalReflection);
        CHECK_FALSE(s.constrainRoughness);
        CHECK(s.clampDirect == 4.0f);
        CHECK(s.clampIndirect == 2.0f);
        CHECK(s.limits.fakeStyle.color[1] == 0.25f);
        CHECK(s.limits.fakeStyle.fresnelDarken == 0.5f);
        set(d, "karma:global:colorlimit", VtValue(0.0f)); // Karma: black, not off
        CHECK(d.GetNovaSettings().clampDirect > 0.0f);
        d.SetRenderSetting(HdNovaSettingsTokens->fakeCaustics, VtValue(true));
        d.SetRenderSetting(HdNovaSettingsTokens->internalReflection, VtValue(true));
        d.SetRenderSetting(HdNovaSettingsTokens->clampIndirect, VtValue(10.0f));
        s = d.GetNovaSettings();
        CHECK(s.limits.fakeCaustics);
        CHECK_FALSE(s.limits.fakeStyle.karma); // Nova's physical factor
        CHECK(s.clampIndirect == 10.0f);
        CHECK(s.clampDirect == 0.0f);
        CHECK_FALSE(s.clampEmissionIndirect);
    }
}

TEST_CASE("Karma's texture, dicing and camera switches", "[hydra][karma][settings]") {
    HdNovaRenderDelegate d;
    set(d, "karma:global:mtlx_imagewidth", VtValue(2.0f));
    set(d, "karma:global:mtlx_imageblur", VtValue(0.1f));
    set(d, "karma:global:abortmissingtexture", VtValue(true));
    set(d, "karma:global:dicingqualityscale", VtValue(2.0f));
    set(d, "karma:global:offscreenquality", VtValue(0.5f));
    set(d, "karma:global:ipr_proxydepth", VtValue(1));
    HdNovaTextureOptions o = d.GetTextureOptions();
    CHECK_THAT(o.filterWidth, WithinAbs(0.5, 1e-6)); // Nova's 0.25 is Karma's 1
    CHECK_THAT(o.blur, WithinAbs(0.1, 1e-6));
    CHECK(o.abortOnMissing);
    CHECK(d.GetDicingQualityScale() == 2.0f);
    CHECK(d.GetOffscreenQuality() == 0.5f);
    CHECK(d.GetIprProxyDepth() == 1);
    d.SetRenderSetting(HdNovaSettingsTokens->textureFilterWidth, VtValue(0.25f));
    d.SetRenderSetting(HdNovaSettingsTokens->abortOnMissingTexture, VtValue(false));
    o = d.GetTextureOptions();
    CHECK(o.filterWidth == 0.25f);
    CHECK_FALSE(o.abortOnMissing);

    CHECK(d.IsDepthOfFieldEnabled());
    set(d, "karma:global:enable_dof", VtValue(false)); // the Karma LOP's Enable Depth of Field
    CHECK_FALSE(d.IsDepthOfFieldEnabled());
    set(d, "karma:global:enable_dof", VtValue(true));
    set(d, "disableDepthOfField", VtValue(true));
    CHECK_FALSE(d.IsDepthOfFieldEnabled());
}

TEST_CASE("Karma's per-object defaults on the render settings", "[hydra][karma][settings]") {
    HdNovaRenderDelegate d;
    CHECK(d.GetObjectDefaults() == HdNovaObjectDefaults{});
    // As the Karma Render Settings LOP authors them: menu labels.
    set(d, "karma:object:curve_style", VtValue(TfToken("Flat Ribbons")));
    set(d, "karma:object:point_style", VtValue(TfToken("Discs")));
    set(d, "karma:object:width_scale", VtValue(2.0f));
    set(d, "karma:object:treat_as_lightsource", VtValue(TfToken("Yes")));
    set(d, "karma:object:truedisplace", VtValue(std::string("Disable Displacement Shader")));
    set(d, "karma:object:dicingquality", VtValue(2.0f));
    set(d, "karma:object:dicingdepthmin", VtValue(3));
    set(d, "karma:object:dicingdepthmax", VtValue(3));
    set(d, "karma:object:mblur", VtValue(false));
    const HdNovaObjectDefaults o = d.GetObjectDefaults();
    CHECK(o.curveStyle == 0);
    CHECK(o.pointStyle == 0);
    CHECK(o.widthScale == 2.0f);
    CHECK(o.lightSource == 1);
    CHECK(o.displacement == 0);
    CHECK(o.dicingQuality == 2.0f);
    CHECK(o.dicingDepth == 3);
    CHECK(o.motionBlur == 0);
    set(d, "karma:object:truedisplace", VtValue(std::string("True Displacement")));
    CHECK(d.GetObjectDefaults().displacement == 1);
}

TEST_CASE("Unsupported Karma settings warn once", "[hydra][karma][settings][warnings]") {
    WarningCatcher catcher;
    HdNovaRenderDelegate d;
    // At the values Nova's image matches: silent.
    set(d, "karma:global:dicingcamera", VtValue(std::string()));
    set(d, "karma:object:blurstyle", VtValue(TfToken("Rotation Blur")));
    set(d, "karma:global:imagefilter", VtValue(std::string(R"([["denoise", {"engine": "optix"}]])")));
    d.GetNovaSettings();
    CHECK(catcher.count("Karma render setting") == 0);
    set(d, "karma:global:dicingcamera", VtValue(std::string("/cameras/dicing")));
    set(d, "karma:object:blurstyle", VtValue(TfToken("Linear Blur")));
    set(d, "karma:global:imagefilter",
        VtValue(std::string(R"([["denoise", {}], ["tonemap", {"operator": "aces"}]])")));
    d.GetNovaSettings();
    d.GetNovaSettings();
    CHECK(catcher.count("karma:global:dicingcamera") == 1);
    CHECK(catcher.count("karma:object:blurstyle") == 1);
    CHECK(catcher.count("'tonemap'") == 1);
    CHECK(catcher.count("'denoise'") == 0);
}

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

// A stage through UsdImaging into the delegate (the rig of
// tests/test_hydra_velocity.cpp); render settings before the first sync.
struct Rig {
    UsdStageRefPtr stage;
    std::unique_ptr<HdNovaRenderDelegate> delegate = std::make_unique<HdNovaRenderDelegate>();
    std::unique_ptr<HdRenderIndex> index;
    UsdImagingSceneIndices sceneIndices;
    HdRenderPassSharedPtr pass;
    HdTaskSharedPtrVector tasks;
    HdEngine engine;

    Rig(std::string const &body, std::vector<std::pair<char const *, VtValue>> const &settings = {}) {
        SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
        layer->ImportFromString(std::string("#usda 1.0\n(\n    timeCodesPerSecond = 24\n)\n") + body);
        stage = UsdStage::Open(layer);
        for (auto const &[key, value] : settings)
            delegate->SetRenderSetting(TfToken(key), value);
        param().SetShutter(HdNovaResolveShutter(-0.25, 0.25, false, false));
        index.reset(HdRenderIndex::New(delegate.get(), HdDriverVector()));
        UsdImagingCreateSceneIndicesInfo info;
        info.stage = stage;
        info.addDrawModeSceneIndex = false;
        sceneIndices = UsdImagingCreateSceneIndices(info);
        sceneIndices.stageSceneIndex->SetTime(UsdTimeCode(0.0));
        index->InsertSceneIndex(HdNovaVelocityModeSceneIndex::Append(sceneIndices.finalSceneIndex),
                                SdfPath::AbsoluteRootPath(), false);
        pass = delegate->CreateRenderPass(index.get(),
                                          HdRprimCollection(HdTokens->geometry, HdReprSelector(HdReprTokens->refined)));
        tasks.push_back(std::make_shared<SyncTask>(pass));
        sceneIndices.stageSceneIndex->ApplyPendingUpdates();
        engine.Execute(index.get(), &tasks);
        scene().setObjectDefaults(delegate->GetNovaSettings().limits);
        scene().commit();
    }
    ~Rig() {
        tasks.clear();
        pass.reset();
        index.reset();
        delegate.reset();
    }
    HdNovaRenderParam &param() { return *static_cast<HdNovaRenderParam *>(delegate->GetRenderParam()); }
    nova::Scene &scene() { return param().GetScene(); }
    // The live record of the shape kind with the given first point x (meshes:
    // the lowest x of their points).
    nova::MeshRecord const &record(nova::MeshShape shape) {
        for (uint32_t id = 0; id < scene().meshCount(); ++id) {
            nova::MeshRecord const &r = scene().meshRecord(id);
            if (r.alive && r.desc.shape == shape)
                return r;
        }
        FAIL("no mesh of that shape");
        static nova::MeshRecord none;
        return none;
    }
};

const char *kKarmaPrimvars = R"(
        float primvars:karma:object:diffuselimit = 0 (interpolation = "constant")
        float primvars:karma:object:reflectlimit = 2 (interpolation = "constant")
        bool primvars:karma:object:causticsenable = 0 (interpolation = "constant")
        bool primvars:karma:object:internalreflectenable = 1 (interpolation = "constant")
        float3 primvars:karma:object:fakecausticscolor = (0.5, 0.5, 0.5) (interpolation = "constant")
        float primvars:karma:object:causticsroughnessclamp = 0.3 (interpolation = "constant")
        bool primvars:karma:object:thinwallatrefractlimit = 0 (interpolation = "constant")
        bool primvars:karma:object:outline_crease = 0 (interpolation = "constant")
        float primvars:karma:object:outline_radius = 0.01 (interpolation = "constant")
)";

void checkKarmaOverrides(nova::RayLimitOverrides const &o, nova::ObjectShading const &s) {
    CHECK(o.limit[int(nova::LimitClass::Diffuse)] == 0.0f);
    CHECK(o.limit[int(nova::LimitClass::Reflect)] == 2.0f);
    CHECK(o.limit[int(nova::LimitClass::Refract)] < 0.0f); // not set
    CHECK(o.thinWallAtRefractLimit == 0);
    CHECK(s.fakeCaustics);
    CHECK(s.fake.karma);
    CHECK(s.fake.color[0] == 0.5f);
    CHECK(s.internalReflection);
    CHECK_THAT(s.causticsRoughnessClamp, WithinAbs(0.3, 1e-6));
}

} // namespace

TEST_CASE("Karma's per-object primvars on every prim type", "[hydra][karma][limits]") {
    const std::string body = std::string(R"(
def Mesh "mesh"
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    uniform token subdivisionScheme = "none")") + kKarmaPrimvars + R"(}
def Points "points"
{
    point3f[] points = [(5, 0, 0)]
    float[] widths = [1] (interpolation = "vertex"))" + kKarmaPrimvars + R"(}
def BasisCurves "curves"
{
    uniform token type = "linear"
    int[] curveVertexCounts = [2]
    point3f[] points = [(8, 0, 0), (8, 1, 0)]
    float[] widths = [0.1] (interpolation = "constant"))" + kKarmaPrimvars + R"(}
def Sphere "sphere"
{
    double radius = 0.5)" + kKarmaPrimvars + R"(}
)";
    Rig rig(body);
    for (nova::MeshShape shape :
         {nova::MeshShape::Triangles, nova::MeshShape::Points, nova::MeshShape::Curves, nova::MeshShape::Sphere}) {
        INFO("shape " << int(shape));
        nova::MeshRecord const &r = rig.record(shape);
        checkKarmaOverrides(r.rayLimits, r.shading);
        // Karma's outlines (docs/design/M11-P9a.md): the prim's own, the rest the defaults.
        CHECK(r.outline.radius == 0.01f);
        CHECK_FALSE(r.outline.crease);
        CHECK(r.outline.contour);
        CHECK(r.outline.depthThreshold == 0.5f);
    }
}

TEST_CASE("Karma's outline and look-dev settings", "[hydra][karma][settings][outline]") {
    WarningCatcher catcher;
    HdNovaRenderDelegate d;
    {
        const nova::RenderSettings s = d.GetNovaSettings();
        CHECK_FALSE(s.outline.enable);
        CHECK(s.look == nova::LookDevSettings{});
        CHECK(s.objectOutline == nova::ObjectOutline{});
    }
    // What the Karma Render Settings LOP authors (bools, ints, floats, a
    // float3) and the Render Settings LOP's Karma tab (the menu label).
    set(d, "karma:global:outline_enable", VtValue(true));
    set(d, "karma:global:outline_mode", VtValue(true));
    set(d, "karma:global:outline_radius", VtValue(0.5f));
    set(d, "karma:global:outline_primary_samples", VtValue(8));
    set(d, "karma:global:outline_primary_stencils", VtValue(2));
    set(d, "karma:global:outline_intensity", VtValue(2.0f));
    set(d, "karma:global:outline_fitmin", VtValue(0.1f));
    set(d, "karma:global:outline_fitmax", VtValue(0.9f));
    set(d, "karma:global:outline_apply", VtValue(false));
    set(d, "karma:global:outline_depthblend", VtValue(0.25f));
    set(d, "karma:global:outline_color", VtValue(GfVec3f(0.0f, 1.0f, 0.0f)));
    set(d, "karma:global:outline_depththreshold", VtValue(2.0f));
    set(d, "karma:object:outline_radius", VtValue(0.01f));
    set(d, "karma:object:outline_crease", VtValue(false));
    set(d, "karma:global:override_lighting", VtValue(TfToken("Dome Light")));
    set(d, "karma:global:disable_lighting", VtValue(true));
    set(d, "karma:global:head_ao_samples", VtValue(0));
    set(d, "karma:global:head_ao_distance", VtValue(5.0f));
    set(d, "karma:global:head_depthcue_z", VtValue(GfVec2f(2.0f, 10.0f)));
    set(d, "karma:global:head_depthcue_color", VtValue(GfVec3f(1.0f, 0.0f, 0.0f)));
    set(d, "karma:global:head_depthcue_alpha", VtValue(0.5f));
    set(d, "karma:global:autoheadlight", VtValue(true));
    const nova::RenderSettings s = d.GetNovaSettings();
    CHECK(s.outline.enable);
    CHECK(s.outline.randomPattern);
    CHECK(s.outline.radius == 0.5f);
    CHECK(s.outline.samples == 8);
    CHECK(s.outline.stencils == 2);
    CHECK(s.outline.intensity == 2.0f);
    CHECK(s.outline.fitMin == 0.1f);
    CHECK(s.outline.fitMax == 0.9f);
    CHECK_FALSE(s.outline.apply);
    CHECK(s.outline.depthBlend == 0.25f);
    CHECK(s.outline.color[1] == 1.0f);
    CHECK(s.outline.color[0] == 0.0f);
    CHECK(s.outline.depthScale == 2.0f);
    CHECK(s.objectOutline.radius == 0.01f);
    CHECK_FALSE(s.objectOutline.crease);
    CHECK(s.objectOutline.depth);
    CHECK(s.look.lights == nova::LightOverride::Dome);
    CHECK(s.look.simplified);
    CHECK(s.look.aoSamples == 0);
    CHECK(s.look.aoDistance == 5.0f);
    CHECK(s.look.fogNear == 2.0f);
    CHECK(s.look.fogFar == 10.0f);
    CHECK(s.look.fogColor[0] == 1.0f);
    CHECK(s.look.fogAlpha == 0.5f);
    CHECK(s.look.autoHeadlight);
    // Menus as indices too.
    set(d, "karma:global:override_lighting", VtValue(2));
    CHECK(d.GetNovaSettings().look.lights == nova::LightOverride::Headlight);
    set(d, "karma:global:override_lighting", VtValue(TfToken("Emissive Objects")));
    CHECK(d.GetNovaSettings().look.lights == nova::LightOverride::EmissiveOnly);
    // Supported now: no warnings (P1 warned about these).
    CHECK(catcher.count("Karma render setting") == 0);
}

TEST_CASE("Nova's per-object primvars win over Karma's", "[hydra][karma][limits]") {
    Rig rig(R"(
def Mesh "mesh"
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    uniform token subdivisionScheme = "none"
    float primvars:karma:object:diffuselimit = 0 (interpolation = "constant")
    float primvars:nova:object:diffuseLimit = 1 (interpolation = "constant")
    bool primvars:karma:object:causticsenable = 0 (interpolation = "constant")
    bool primvars:nova:object:fakeCaustics = 1 (interpolation = "constant")
}
)");
    nova::MeshRecord const &r = rig.record(nova::MeshShape::Triangles);
    CHECK(r.rayLimits.limit[int(nova::LimitClass::Diffuse)] == 1.0f);
    CHECK(r.shading.fakeCaustics);
    CHECK_FALSE(r.shading.fake.karma); // Nova's switch: Nova's factor
}

TEST_CASE("The render settings' Karma styles reach curves and points", "[hydra][karma][curves]") {
    const char *body = R"(
def Points "points"
{
    point3f[] points = [(5, 0, 0)]
    float[] widths = [1] (interpolation = "vertex")
}
def BasisCurves "curves"
{
    uniform token type = "linear"
    int[] curveVertexCounts = [2]
    point3f[] points = [(8, 0, 0), (8, 1, 0)]
    float[] widths = [0.1] (interpolation = "constant")
}
)";
    {
        Rig rig(body);
        CHECK(rig.record(nova::MeshShape::Points).desc.pointStyle == nova::PointStyle::Sphere);
        CHECK(rig.record(nova::MeshShape::Curves).desc.curveStyle == nova::CurveStyle::Round);
    }
    Rig rig(body, {{"karma:object:point_style", VtValue(TfToken("Discs"))},
                   {"karma:object:curve_style", VtValue(TfToken("Flat Ribbons"))},
                   {"karma:object:width_scale", VtValue(3.0f)}});
    nova::MeshRecord const &points = rig.record(nova::MeshShape::Points);
    CHECK(points.desc.pointStyle == nova::PointStyle::Disc);
    CHECK_THAT(points.desc.pointRadius[0], WithinAbs(1.5, 1e-6));
    CHECK(rig.record(nova::MeshShape::Curves).desc.curveStyle == nova::CurveStyle::Flat);
}

// Karma measured: an object with karma:object:mblur off renders as with
// motion blur disabled, at the frame (tests/husk/check_karma_props.py).
TEST_CASE("Karma's per-object motion blur switch", "[hydra][karma][motion]") {
    const char *body = R"(
def Points "moving"
{
    point3f[] points = [(0, 0, 0)]
    float[] widths = [1] (interpolation = "vertex")
    double3 xformOp:translate.timeSamples = {
        -0.25: (-1, 0, 0),
        0: (0, 0, 0),
        0.25: (1, 0, 0),
    }
    uniform token[] xformOpOrder = ["xformOp:translate"]
    %s
}
)";
    {
        Rig rig(TfStringPrintf(body, ""));
        CHECK(rig.record(nova::MeshShape::Points).motionSteps > 1);
    }
    {
        Rig rig(TfStringPrintf(body, "bool primvars:karma:object:mblur = 0 (interpolation = \"constant\")"));
        CHECK(rig.record(nova::MeshShape::Points).motionSteps == 1);
    }
    {
        Rig rig(TfStringPrintf(body, ""), {{"karma:object:mblur", VtValue(false)}});
        CHECK(rig.record(nova::MeshShape::Points).motionSteps == 1);
    }
}

TEST_CASE("Karma's path guiding settings", "[hydra][karma][settings][guiding]") {
    WarningCatcher catcher;
    {
        // Path Traced: training samples are paths; every control read, no
        // warning (Nova guides).
        HdNovaRenderDelegate d;
        set(d, "karma:global:convergence_mode", VtValue(TfToken("Path Traced")));
        set(d, "karma:global:guiding_enable", VtValue(true));
        set(d, "karma:global:guiding_samples", VtValue(8));
        set(d, "karma:global:guiding_components", VtValue(std::string("diffuse volume")));
        set(d, "karma:global:guiding_deterministic", VtValue(true));
        set(d, "karma:global:guiding_clearbuffers", VtValue(true));
        const nova::RenderSettings s = d.GetNovaSettings();
        CHECK(s.guiding.enable);
        CHECK(s.guiding.trainingSamples == 8u);
        CHECK(s.guiding.components == (nova::guide::kDiffuse | nova::guide::kVolume));
        CHECK(s.guiding.deterministic);
        CHECK(s.guiding.clearBuffers);
        CHECK(catcher.count("guiding") == 0);
    }
    {
        // Variance (Karma's default mode): primary samples, each up to
        // varianceaa_maxsamples of Nova's; the default components.
        HdNovaRenderDelegate d;
        set(d, "karma:global:guiding_enable", VtValue(true));
        set(d, "karma:global:guiding_samples", VtValue(4));
        set(d, "karma:object:varianceaa_maxsamples", VtValue(6));
        const nova::RenderSettings s = d.GetNovaSettings();
        CHECK(s.guiding.trainingSamples == 24u);
        CHECK(s.guiding.components == nova::guide::kDefaultComponents);
        CHECK_FALSE(s.guiding.clearBuffers);
    }
    {
        // Nova's settings win, also off over Karma's on; unknown component
        // names warn once.
        HdNovaRenderDelegate d;
        set(d, "karma:global:guiding_enable", VtValue(true));
        set(d, "nova:guiding", VtValue(false));
        CHECK_FALSE(d.GetNovaSettings().guiding.enable);
        set(d, "nova:guiding", VtValue(true));
        set(d, "nova:guidingSamples", VtValue(3));
        set(d, "nova:guidingComponents", VtValue(std::string("reflect,refract glossy")));
        set(d, "karma:global:guiding_samples", VtValue(50));
        nova::RenderSettings s = d.GetNovaSettings();
        s = d.GetNovaSettings();
        CHECK(s.guiding.enable);
        CHECK(s.guiding.trainingSamples == 3u);
        CHECK(s.guiding.components == (nova::guide::kReflect | nova::guide::kRefract));
        CHECK(catcher.count("unknown component 'glossy'") == 1);
    }
    {
        // Off by default.
        HdNovaRenderDelegate d;
        CHECK_FALSE(d.GetNovaSettings().guiding.enable);
    }
}

// Nested dielectrics and light portals (docs/design/M11-P9b.md): the
// priority from Nova's primvar, Karma's, the render settings'; a mesh with
// isportal is no scene mesh but the openings of the dome lights its list
// names (prim paths, * for all); portals on other prims warn.
TEST_CASE("Karma's dielectric priority and light portals through Hydra", "[hydra][karma][nested][portal]") {
    const char *quad = R"(
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(%g, 0, 0), (%g, 0, 0), (%g, 1, 0), (%g, 1, 0)]
    uniform token subdivisionScheme = "none"
)";
    const auto mesh = [&](char const *name, double x, char const *primvars) {
        return TfStringPrintf("def Mesh \"%s\"\n{", name) + TfStringPrintf(quad, x, x + 1, x + 1, x) + primvars + "}\n";
    };
    {
        Rig rig(mesh("karma", 0.0, "    int primvars:karma:object:dielectricpriority = 2 (interpolation = \"constant\")\n") +
                    mesh("nova", 5.0,
                         "    int primvars:karma:object:dielectricpriority = 2 (interpolation = \"constant\")\n"
                         "    int primvars:nova:object:dielectricPriority = 1 (interpolation = \"constant\")\n") +
                    mesh("plain", 10.0, ""),
                {{"karma:object:dielectricpriority", VtValue(3)}});
        std::map<float, int> priority; // by the first point's x
        for (uint32_t id = 0; id < rig.scene().meshCount(); ++id) {
            nova::MeshRecord const &r = rig.scene().meshRecord(id);
            if (r.alive && !r.desc.points.empty())
                priority[r.desc.points[0].x] = r.shading.dielectricPriority;
        }
        CHECK(priority[0.0f] == 2);
        CHECK(priority[5.0f] == 1);
        CHECK(priority[10.0f] == 3); // the render settings' Karma value
        CHECK(rig.scene().nestedDielectrics());
    }
    WarningCatcher catcher;
    Rig rig(mesh("window", 0.0,
                 "    bool primvars:karma:object:isportal = 1 (interpolation = \"constant\")\n"
                 "    string primvars:karma:object:portaldomelights = \"/domeR\" (interpolation = \"constant\")\n") +
            mesh("wall", 5.0, "") + R"(
def DomeLight "domeR"
{
}
def DomeLight "domeB"
{
}
def Points "dots"
{
    point3f[] points = [(9, 0, 0)]
    float[] widths = [1] (interpolation = "vertex")
    bool primvars:karma:object:isportal = 1 (interpolation = "constant")
}
)");
    int meshes = 0;
    for (uint32_t id = 0; id < rig.scene().meshCount(); ++id)
        meshes += rig.scene().meshRecord(id).alive && rig.scene().meshRecord(id).desc.shape == nova::MeshShape::Triangles;
    CHECK(meshes == 1); // the wall; the window is a portal
    std::vector<uint32_t> portals;
    for (nova::LightData const &l : rig.scene().lights())
        if (l.type == nova::LightType::Dome)
            portals.push_back(l.portalCount);
    std::sort(portals.begin(), portals.end());
    CHECK(portals == std::vector<uint32_t>{0, 2}); // domeR through the window, domeB everywhere
    CHECK(catcher.count("light portals must be meshes: /dots") == 1);
}

// ---------------------------------------------------------------------------
// 0.2.0: settings of Nova's own, and Nova's names for what only Karma's
// attributes drove (docs/reference.md, "Render settings")
// ---------------------------------------------------------------------------

TEST_CASE("Sample scale, noise per frame, roulette depth and threads", "[hydra][settings][nova]") {
    HdNovaRenderDelegate d;
    CHECK(d.GetNovaSettings().maxSamples == 64u);
    CHECK(d.GetNovaSettings().rrStartDepth == 3u);
    // The scale multiplies the samples; one sample at least remains.
    set(d, "nova:sampleScale", VtValue(0.25f));
    CHECK(d.GetNovaSettings().maxSamples == 16u);
    set(d, "nova:sampleScale", VtValue(0.001f));
    CHECK(d.GetNovaSettings().maxSamples == 1u);
    set(d, "nova:sampleScale", VtValue(2.0));
    CHECK(d.GetNovaSettings().maxSamples == 128u);
    set(d, "nova:samples", VtValue(10));
    set(d, "nova:sampleScale", VtValue(0.5f));
    CHECK(d.GetNovaSettings().maxSamples == 5u);
    // Values that say nothing leave the samples alone.
    for (VtValue const &v : {VtValue(0.0f), VtValue(-1.0f), VtValue(std::numeric_limits<float>::quiet_NaN()),
                             VtValue(std::string("half"))}) {
        set(d, "nova:sampleScale", v);
        CHECK(d.GetNovaSettings().maxSamples == 10u);
    }
    {
        // Karma's samples too.
        HdNovaRenderDelegate k;
        set(k, "karma:global:convergence_mode", VtValue(TfToken("Path Traced")));
        set(k, "karma:global:pathtracedsamples", VtValue(128));
        set(k, "nova:sampleScale", VtValue(0.5f));
        CHECK(k.GetNovaSettings().maxSamples == 64u);
    }
    {
        // husk's number of the frame varies the seed, unless the noise stands still.
        HdNovaRenderDelegate n;
        set(n, "nova:seed", VtValue(5));
        CHECK(n.GetNovaSettings().seed == 5u);
        set(n, "randomseed", VtValue(2116800000.0)); // frame 3
        const uint32_t frame3 = n.GetNovaSettings().seed;
        CHECK(frame3 != 5u);
        set(n, "nova:animateNoise", VtValue(false));
        CHECK(n.GetNovaSettings().seed == 5u);
        set(n, "nova:animateNoise", VtValue(true));
        CHECK(n.GetNovaSettings().seed == frame3);
    }
    set(d, "nova:rrStartDepth", VtValue(8));
    CHECK(d.GetNovaSettings().rrStartDepth == 8u);
    set(d, "nova:rrStartDepth", VtValue(0));
    CHECK(d.GetNovaSettings().rrStartDepth == 1u);
    set(d, "nova:rrStartDepth", VtValue(std::string("deep")));
    CHECK(d.GetNovaSettings().rrStartDepth == 3u);
    // Threads: all, at most n, all but n; one at least.
    CHECK(d.GetThreadLimit(8) == 8);
    set(d, "nova:threads", VtValue(4));
    CHECK(d.GetThreadLimit(8) == 4);
    set(d, "nova:threads", VtValue(64));
    CHECK(d.GetThreadLimit(8) == 8);
    set(d, "nova:threads", VtValue(-2));
    CHECK(d.GetThreadLimit(8) == 6);
    set(d, "nova:threads", VtValue(-100));
    CHECK(d.GetThreadLimit(8) == 1);
    set(d, "nova:threads", VtValue(0));
    CHECK(d.GetThreadLimit(0) == 1);
}

TEST_CASE("Nova's look-dev and outline settings come before Karma's", "[hydra][settings][nova][outline]") {
    HdNovaRenderDelegate d;
    // Karma's stand in while Nova's are not authored.
    set(d, "karma:global:override_lighting", VtValue(TfToken("Dome Light")));
    set(d, "karma:global:disable_lighting", VtValue(false));
    set(d, "karma:global:head_ao_samples", VtValue(4));
    set(d, "karma:global:head_ao_distance", VtValue(5.0f));
    set(d, "karma:global:head_depthcue_z", VtValue(GfVec2f(2.0f, 10.0f)));
    set(d, "karma:global:head_depthcue_color", VtValue(GfVec3f(1.0f, 0.0f, 0.0f)));
    set(d, "karma:global:head_depthcue_alpha", VtValue(0.5f));
    set(d, "karma:global:outline_enable", VtValue(true));
    set(d, "karma:global:outline_radius", VtValue(0.5f));
    set(d, "karma:global:outline_color", VtValue(GfVec3f(0.0f, 1.0f, 0.0f)));
    set(d, "karma:global:outline_mode", VtValue(true));
    set(d, "karma:global:outline_apply", VtValue(true));
    set(d, "karma:object:outline_radius", VtValue(0.01f));
    set(d, "karma:object:outline_depth", VtValue(false));
    nova::RenderSettings s = d.GetNovaSettings();
    CHECK(s.look.lights == nova::LightOverride::Dome);
    CHECK(s.look.aoSamples == 4u);
    CHECK(s.outline.enable);
    CHECK_FALSE(s.outline.requested); // Karma's switch: lines only in a render with an outline AOV
    CHECK(s.outline.randomPattern);

    set(d, "nova:lightOverride", VtValue(TfToken("headlight")));
    set(d, "nova:autoHeadlight", VtValue(true));
    set(d, "nova:simplifiedShading", VtValue(true));
    set(d, "nova:ambientOcclusion", VtValue(false));
    set(d, "nova:aoDistance", VtValue(0.0f));
    set(d, "nova:fogNear", VtValue(1.0f));
    set(d, "nova:fogFar", VtValue(4.0f));
    set(d, "nova:fogColor", VtValue(GfVec3f(0.0f, 0.0f, 1.0f)));
    set(d, "nova:fogAlpha", VtValue(0.25f));
    set(d, "nova:outline", VtValue(true));
    set(d, "nova:outlineRadius", VtValue(2.0f));
    set(d, "nova:outlineColor", VtValue(GfVec3f(0.0f, 0.0f, 0.0f)));
    set(d, "nova:outlinePattern", VtValue(TfToken("rings")));
    set(d, "nova:outlineSamples", VtValue(16));
    set(d, "nova:outlineStencils", VtValue(3));
    set(d, "nova:outlineIntensity", VtValue(0.5f));
    set(d, "nova:outlineFitMin", VtValue(0.2f));
    set(d, "nova:outlineFitMax", VtValue(0.8f));
    set(d, "nova:outlineDepthThreshold", VtValue(3.0f));
    set(d, "nova:outlineDepthBlend", VtValue(1.0f));
    set(d, "nova:outlineApply", VtValue(false));
    set(d, "nova:object:outlineRadius", VtValue(0.004f));
    set(d, "nova:object:outlineCrease", VtValue(false));
    s = d.GetNovaSettings();
    CHECK(s.look.lights == nova::LightOverride::Headlight);
    CHECK(s.look.autoHeadlight);
    CHECK(s.look.simplified);
    CHECK(s.look.aoSamples == 0u);
    CHECK(s.look.aoDistance == 1e30f); // Nova's 0: at any distance
    CHECK(s.look.fogNear == 1.0f);
    CHECK(s.look.fogFar == 4.0f);
    CHECK(s.look.fogColor[0] == 0.0f);
    CHECK(s.look.fogColor[2] == 1.0f);
    CHECK(s.look.fogAlpha == 0.25f);
    CHECK(s.outline.enable);
    CHECK(s.outline.requested); // Nova's switch: lines in the beauty of any render
    CHECK_FALSE(s.outline.randomPattern);
    CHECK(s.outline.radius == 2.0f);
    CHECK(s.outline.color[1] == 0.0f);
    CHECK(s.outline.samples == 16u);
    CHECK(s.outline.stencils == 3u);
    CHECK(s.outline.intensity == 0.5f);
    CHECK(s.outline.fitMin == 0.2f);
    CHECK(s.outline.fitMax == 0.8f);
    CHECK(s.outline.depthScale == 3.0f);
    CHECK(s.outline.depthBlend == 1.0f);
    CHECK_FALSE(s.outline.apply);
    CHECK(s.objectOutline.radius == 0.004f);
    CHECK_FALSE(s.objectOutline.crease);
    CHECK_FALSE(s.objectOutline.depth); // Karma's, where Nova's says nothing
    CHECK(s.objectOutline.contour);

    set(d, "nova:aoDistance", VtValue(2.5f));
    set(d, "nova:ambientOcclusion", VtValue(true));
    set(d, "nova:outlinePattern", VtValue(TfToken("random")));
    s = d.GetNovaSettings();
    CHECK(s.look.aoDistance == 2.5f);
    CHECK(s.look.aoSamples == 1u);
    CHECK(s.outline.randomPattern);
    // Off is a value too: Karma's cannot turn the lines or its dome on.
    set(d, "nova:outline", VtValue(false));
    set(d, "nova:lightOverride", VtValue(TfToken("off")));
    s = d.GetNovaSettings();
    CHECK_FALSE(s.outline.enable);
    CHECK_FALSE(s.outline.requested);
    CHECK(s.look.lights == nova::LightOverride::Off);
    set(d, "nova:lightOverride", VtValue(TfToken("emissive")));
    CHECK(d.GetNovaSettings().look.lights == nova::LightOverride::EmissiveOnly);
    set(d, "nova:lightOverride", VtValue(TfToken("dome")));
    CHECK(d.GetNovaSettings().look.lights == nova::LightOverride::Dome);
}

TEST_CASE("Nova's ray visibility, holdouts, outlines and LPE tags", "[hydra][settings][nova][visibility]") {
    HdNovaRenderDelegate d;
    set(d, "karma:object:rendervisibility", VtValue(std::string("-shadow")));
    set(d, "karma:object:holdoutmode", VtValue(TfToken("Background")));
    set(d, "karma:global:bgoccludeshadow", VtValue(true));
    nova::RenderSettings s = d.GetNovaSettings();
    CHECK(s.objectVisibility.mask == int16_t(nova::kVisibleAll & ~nova::kVisibleShadow));
    CHECK(s.objectVisibility.holdout == int8_t(nova::HoldoutMode::Background));
    CHECK(s.backgroundOccludesShadows);
    set(d, "nova:object:renderVisibility", VtValue(std::string("-primary")));
    set(d, "nova:object:holdoutMode", VtValue(TfToken("matte")));
    set(d, "nova:backgroundOccludesShadows", VtValue(false));
    s = d.GetNovaSettings();
    CHECK(s.objectVisibility.mask == int16_t(nova::kVisibleAll & ~nova::kVisibleCamera));
    CHECK(s.objectVisibility.holdout == int8_t(nova::HoldoutMode::Matte));
    CHECK_FALSE(s.backgroundOccludesShadows);
    // An empty expression is seen by no ray; the defaults are everything and no holdout.
    set(d, "nova:object:renderVisibility", VtValue(std::string("")));
    set(d, "nova:object:holdoutMode", VtValue(TfToken("none")));
    s = d.GetNovaSettings();
    CHECK(s.objectVisibility.mask == 0);
    CHECK(s.objectVisibility.holdout == int8_t(nova::HoldoutMode::None));
    set(d, "nova:object:renderVisibility", VtValue(std::string("*")));
    CHECK(d.GetNovaSettings().objectVisibility.mask == int16_t(nova::kVisibleAll));

    // On a prim: Nova's primvars before Karma's, each property by itself.
    Rig rig(R"(
def Mesh "mesh"
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    uniform token subdivisionScheme = "none"
    string primvars:karma:object:rendervisibility = "-shadow" (interpolation = "constant")
    string primvars:nova:object:renderVisibility = "-primary" (interpolation = "constant")
    token primvars:nova:object:holdoutMode = "matte" (interpolation = "constant")
    string primvars:karma:object:lpetag = "karma" (interpolation = "constant")
    string primvars:nova:object:lpeTag = "nova" (interpolation = "constant")
    bool primvars:karma:object:outline_contour = 0 (interpolation = "constant")
    bool primvars:nova:object:outlineContour = 1 (interpolation = "constant")
    float primvars:nova:object:outlineRadius = 0.004 (interpolation = "constant")
    bool primvars:karma:object:outline_depth = 0 (interpolation = "constant")
}
def Points "points"
{
    point3f[] points = [(5, 0, 0)]
    float[] widths = [1] (interpolation = "vertex")
    string primvars:karma:object:rendervisibility = "-shadow" (interpolation = "constant")
    string primvars:karma:object:lpetag = "karma" (interpolation = "constant")
}
)");
    nova::MeshRecord const &r = rig.record(nova::MeshShape::Triangles);
    CHECK(r.visibilitySettings.mask == int16_t(nova::kVisibleAll & ~nova::kVisibleCamera));
    CHECK(r.visibilitySettings.holdout == int8_t(nova::HoldoutMode::Matte));
    CHECK(r.lpeTag == "nova");
    CHECK(r.outlineSettings.contour == 1);
    CHECK(r.outlineSettings.radius == 0.004f);
    CHECK(r.outlineSettings.depth == 0);
    nova::MeshRecord const &p = rig.record(nova::MeshShape::Points);
    CHECK(p.visibilitySettings.mask == int16_t(nova::kVisibleAll & ~nova::kVisibleShadow));
    CHECK(p.lpeTag == "karma");
}

TEST_CASE("Per-object properties as defaults on the render settings", "[hydra][settings][nova]") {
    HdNovaRenderDelegate d;
    set(d, "karma:object:curve_style", VtValue(TfToken("Flat Ribbons")));
    set(d, "karma:object:width_scale", VtValue(2.0f));
    set(d, "karma:object:dicingquality", VtValue(2.0f));
    set(d, "karma:object:vblur", VtValue(TfToken("Velocity Blur")));
    set(d, "karma:object:geosamples", VtValue(5));
    set(d, "karma:object:instance_samples", VtValue(3));
    CHECK(d.GetObjectDefaults().curveStyle == 0);
    CHECK(d.GetVelocityDefaults().velocityBlur == 1);
    CHECK(d.GetVelocityDefaults().geoSamples == 5);
    set(d, "nova:object:curveStyle", VtValue(2));
    set(d, "nova:object:pointStyle", VtValue(0));
    set(d, "nova:object:widthScale", VtValue(0.5f));
    set(d, "nova:object:lightSource", VtValue(2));
    set(d, "nova:dicingQuality", VtValue(1.5f));
    set(d, "nova:dicingDepth", VtValue(4));
    set(d, "nova:displacement", VtValue(false));
    set(d, "nova:object:velocityBlur", VtValue(2));
    set(d, "nova:object:instanceVelocityBlur", VtValue(1));
    set(d, "nova:object:accelerationSamples", VtValue(7));
    const HdNovaObjectDefaults o = d.GetObjectDefaults();
    CHECK(o.curveStyle == 2);
    CHECK(o.pointStyle == 0);
    CHECK(o.widthScale == 0.5f);
    CHECK(o.lightSource == 2);
    CHECK(o.dicingQuality == 1.5f);
    CHECK(o.dicingDepth == 4);
    CHECK(o.displacement == 0);
    const HdNovaVelocityDefaults v = d.GetVelocityDefaults();
    CHECK(v.velocityBlur == 2);
    CHECK(v.instanceVelocityBlur == 1);
    CHECK(v.geoSamples == 7);
    CHECK(v.instanceSamples == 7);
    // A dicing depth of -1 is "off", which is a value.
    set(d, "nova:dicingDepth", VtValue(-1));
    CHECK(d.GetObjectDefaults().dicingDepth == -1);

    // They reach the prims that author nothing; a prim's own primvar, Karma's too, still wins.
    Rig rig(R"(
def Points "points"
{
    point3f[] points = [(5, 0, 0)]
    float[] widths = [1] (interpolation = "vertex")
    int primvars:karma:object:point_style = 1 (interpolation = "constant")
}
def BasisCurves "curves"
{
    uniform token type = "linear"
    int[] curveVertexCounts = [2]
    point3f[] points = [(8, 0, 0), (8, 1, 0)]
    float[] widths = [0.1] (interpolation = "constant")
}
)",
            {{"nova:object:pointStyle", VtValue(0)},
             {"nova:object:curveStyle", VtValue(0)},
             {"nova:object:widthScale", VtValue(3.0f)},
             {"karma:object:curve_style", VtValue(TfToken("Rounded Curves"))}});
    nova::MeshRecord const &points = rig.record(nova::MeshShape::Points);
    CHECK(points.desc.pointStyle == nova::PointStyle::Sphere); // its own
    CHECK_THAT(points.desc.pointRadius[0], WithinAbs(1.5, 1e-6));
    CHECK(rig.record(nova::MeshShape::Curves).desc.curveStyle == nova::CurveStyle::Flat); // Nova's before Karma's
}

TEST_CASE("The denoiser's quality and guides", "[hydra][settings][nova][denoise]") {
    HdNovaRenderDelegate d;
    HdNovaDenoiseConfig c = d.GetDenoiseConfig();
    CHECK(c.quality == -1);
    CHECK(c.viewportAlbedo);
    CHECK(c.viewportNormal);
    // The switches of Karma's Denoiser are the final frame's.
    set(d, "karma:global:imagefilter",
        VtValue(std::string(R"([["denoise", {"engine": "oidn", "aovs": ["C"], "use_albedo_input": false, "use_n_input": true}]])")));
    c = d.GetDenoiseConfig();
    CHECK_FALSE(c.final.albedo);
    CHECK(c.final.normal);
    CHECK(c.viewportAlbedo);
    // Nova's hold for both.
    set(d, "nova:denoiseAlbedo", VtValue(true));
    set(d, "nova:denoiseNormal", VtValue(false));
    set(d, "nova:denoiseQuality", VtValue(TfToken("fast")));
    c = d.GetDenoiseConfig();
    CHECK(c.final.albedo);
    CHECK_FALSE(c.final.normal);
    CHECK(c.viewportAlbedo);
    CHECK_FALSE(c.viewportNormal);
    CHECK(c.quality == 0);
    set(d, "nova:denoiseQuality", VtValue(TfToken("balanced")));
    CHECK(d.GetDenoiseConfig().quality == 1);
    set(d, "nova:denoiseQuality", VtValue(std::string(" High ")));
    CHECK(d.GetDenoiseConfig().quality == 2);
    set(d, "nova:denoiseQuality", VtValue(TfToken("auto")));
    CHECK(d.GetDenoiseConfig().quality == -1);
    // A changed quality is another configuration (the render pass restarts on it).
    CHECK_FALSE(c == d.GetDenoiseConfig());
}

TEST_CASE("Nova's switches of the deep camera map", "[hydra][settings][nova][deep]") {
    // husk hands the products the delegate writes itself to it as a setting.
    HdNovaRenderDelegate d;
    CHECK(d.GetDeepRequest() == nullptr); // no deep product
    HdAovSettingsMap product;
    product[TfToken("productType")] = VtValue(TfToken("karma:deep"));
    product[TfToken("productName")] = VtValue(TfToken("deep.exr"));
    product[TfToken("karma:product:dcmcompression")] = VtValue(7);
    set(d, "delegateRenderProducts", VtValue(std::vector<VtValue>{VtValue(product)}));
    std::shared_ptr<const nova::DeepRequest> deep = d.GetDeepRequest();
    REQUIRE(deep != nullptr);
    CHECK(deep->compression == 7);
    CHECK_FALSE(deep->hitDistance);
    CHECK_FALSE(deep->excludeHoldouts);
    // Karma's switches stand in; Nova's come first, also when they say no.
    set(d, "karma:global:dcm_usehitdist", VtValue(true));
    set(d, "karma:global:dcm_excludeholdouts", VtValue(true));
    deep = d.GetDeepRequest();
    REQUIRE(deep != nullptr);
    CHECK(deep->hitDistance);
    CHECK(deep->excludeHoldouts);
    set(d, "nova:deepUseHitDistance", VtValue(false));
    deep = d.GetDeepRequest();
    REQUIRE(deep != nullptr);
    CHECK_FALSE(deep->hitDistance);
    CHECK(deep->excludeHoldouts);
    set(d, "karma:global:dcm_usehitdist", VtValue());
    set(d, "karma:global:dcm_excludeholdouts", VtValue());
    set(d, "nova:deepUseHitDistance", VtValue(true));
    set(d, "nova:deepExcludeHoldouts", VtValue(true));
    deep = d.GetDeepRequest();
    REQUIRE(deep != nullptr);
    CHECK(deep->hitDistance);
    CHECK(deep->excludeHoldouts);
}

TEST_CASE("The texture cache's memory", "[hydra][settings][nova][textures]") {
    HdNovaRenderDelegate d;
    CHECK(d.GetTextureOptions().cacheMemoryMB == 0);
    set(d, "nova:textureCacheMemory", VtValue(3000));
    CHECK(d.GetTextureOptions().cacheMemoryMB == 3000);
    CHECK_FALSE(d.GetTextureOptions() == HdNovaTextureOptions{});
    set(d, "nova:textureCacheMemory", VtValue(-5));
    CHECK(d.GetTextureOptions().cacheMemoryMB == 0);
    // The cache takes the limit and gets its own back.
    HdNovaTextureSystem textures;
    const float before = textures.CacheMemoryMB();
    CHECK(before > 0.0f);
    HdNovaTextureOptions o;
    o.cacheMemoryMB = before == 3000.0f ? 2000 : 3000;
    textures.SetOptions(o);
    CHECK(textures.CacheMemoryMB() == float(o.cacheMemoryMB));
    o.cacheMemoryMB = 0;
    textures.SetOptions(o);
    CHECK(textures.CacheMemoryMB() == before);
}
