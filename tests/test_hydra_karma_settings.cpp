// Karma's render properties as fallbacks of Nova's (docs/design/M11-P1.md):
// Nova's settings win only when authored, Karma's stand in otherwise, Karma
// render settings bring Karma's own defaults where Nova's differ, the
// per-object Karma primvars of the Render Geometry Settings LOP reach every
// prim type, and unsupported image-changing Karma settings warn once.

#include "hydra/karmaSettings.h"
#include "hydra/motion.h"
#include "hydra/renderDelegate.h"
#include "hydra/renderParam.h"
#include "hydra/velocitySceneIndex.h"

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
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
    set(d, "karma:global:guiding_enable", VtValue(false));
    set(d, "karma:object:blurstyle", VtValue(TfToken("Rotation Blur")));
    set(d, "karma:global:imagefilter", VtValue(std::string(R"([["denoise", {"engine": "optix"}]])")));
    d.GetNovaSettings();
    CHECK(catcher.count("Karma render setting") == 0);
    set(d, "karma:global:guiding_enable", VtValue(true));
    set(d, "karma:object:blurstyle", VtValue(TfToken("Linear Blur")));
    set(d, "karma:global:imagefilter",
        VtValue(std::string(R"([["denoise", {}], ["tonemap", {"operator": "aces"}]])")));
    d.GetNovaSettings();
    d.GetNovaSettings();
    CHECK(catcher.count("karma:global:guiding_enable") == 1);
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
