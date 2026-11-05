// Velocity and acceleration motion blur through Hydra (docs/design/M8.md
// D19): HdNovaVelocityModeSceneIndex maps Nova's and Karma's settings to the
// mode of pxr's HdsiVelocityMotionResolvingSceneIndex, and the meshes,
// curves, points and instances sample the resolved motion over the shutter.

#include "hydra/motion.h"
#include "hydra/renderDelegate.h"
#include "hydra/renderParam.h"
#include "hydra/velocitySceneIndex.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdsi/velocityMotionResolvingSceneIndex.h>
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

#include <chrono>
#include <cmath>
#include <cstdio>
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

// Everything at frame 1 with 24 time codes per second; the shutter is
// [-0.25, 0.25] frames = 1/48 s, so 240 units per second move 5 units.
const char *kHeader = R"(#usda 1.0
(
    startTimeCode = 1
    endTimeCode = 1
    timeCodesPerSecond = 24
)
)";

std::string quad(char const *name, float y, std::string const &extra, char const *type = "Mesh") {
    const std::string pts = TfStringPrintf("[(0, %g, 0), (2, %g, 0), (2, %g, 0), (0, %g, 0)]", y, y, y + 2, y + 2);
    return TfStringPrintf(R"(
def %s "%s"
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = %s
    uniform token subdivisionScheme = "none"
    vector3f[] velocities = [(240, 0, 0), (240, 0, 0), (240, 0, 0), (240, 0, 0)]
%s
}
)",
                          type, name, pts.c_str(), extra.c_str());
}

std::string primvar(char const *name, int value) {
    return TfStringPrintf("    int primvars:%s = %d (interpolation = \"constant\")\n", name, value);
}

const std::string kAccelerations =
    "    vector3f[] accelerations = [(46080, 0, 0), (46080, 0, 0), (46080, 0, 0), (46080, 0, 0)]\n";

// The test rig of tests/test_hydra_volumes.cpp with Nova's velocity indices
// (the plugin's chain; test rigs run no renderer plugins) and a shutter.
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
        layer->ImportFromString(std::string(kHeader) + body);
        stage = UsdStage::Open(layer);
        param().SetShutter(HdNovaResolveShutter(-0.25, 0.25, false, false));
        index.reset(HdRenderIndex::New(delegate.get(), HdDriverVector()));
        UsdImagingCreateSceneIndicesInfo info;
        info.stage = stage;
        info.addDrawModeSceneIndex = false;
        sceneIndices = UsdImagingCreateSceneIndices(info);
        sceneIndices.stageSceneIndex->SetTime(UsdTimeCode(1.0));
        index->InsertSceneIndex(HdNovaVelocityModeSceneIndex::Append(sceneIndices.finalSceneIndex, ""),
                                SdfPath::AbsoluteRootPath(), false);
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
        param().GetScene().commit();
    }
    // What the render pass does with the render settings' defaults.
    bool applyDefaults() {
        return HdNovaVelocityModeSceneIndex::SetDefaults(index->GetInstanceName(), delegate->GetVelocityDefaults());
    }
    HdNovaRenderParam &param() { return *static_cast<HdNovaRenderParam *>(delegate->GetRenderParam()); }
    nova::Scene &scene() { return param().GetScene(); }
    void setInt(char const *prim, char const *name, int value) {
        UsdGeomPrimvarsAPI(stage->GetPrimAtPath(SdfPath(prim)))
            .CreatePrimvar(TfToken(name), SdfValueTypeNames->Int, UsdGeomTokens->constant)
            .Set(value);
    }
    // The live record whose points start (at their first step) at height y.
    nova::MeshRecord const &record(float y) {
        for (uint32_t id = 0; id < scene().meshCount(); ++id) {
            nova::MeshRecord const &r = scene().meshRecord(id);
            if (!r.alive || r.desc.points.empty())
                continue;
            float lowest = r.desc.points[0].y;
            for (uint32_t i = 0; i < r.desc.pointCount(); ++i)
                lowest = std::min(lowest, r.desc.points[i].y);
            if (std::fabs(lowest - y) < 1e-4f)
                return r;
        }
        FAIL("no mesh at y = " << y);
        static nova::MeshRecord none;
        return none;
    }
    // The x of the first point at every motion step.
    std::vector<float> xSteps(float y) {
        nova::MeshDesc const &d = record(y).desc;
        std::vector<float> xs;
        for (uint32_t s = 0; s < d.pointSteps; ++s)
            xs.push_back(d.points[size_t(s) * d.pointCount()].x);
        return xs;
    }
};

// 2.5 u + 2.5 u^2 at u = -1 .. 1: the left edge under 46080 units/s^2.
float accelerated(float u) { return 2.5f * u + 2.5f * u * u; }

void checkSteps(std::vector<float> const &xs, std::vector<float> const &want) {
    std::string text;
    for (float x : xs)
        text += TfStringPrintf(" %g", x);
    INFO("steps:" << text);
    REQUIRE(xs.size() == want.size());
    for (size_t i = 0; i < xs.size(); ++i)
        CHECK_THAT(xs[i], WithinAbs(want[i], 1e-4));
}

} // namespace

// The sync cost of the velocity indices (run by hand): 22500 meshes without
// velocities through UsdImaging alone, pxr's resolving index, and Nova's
// chain (twice). Measured 266 / 286 / 335 / 327 ms.
TEST_CASE("Velocity scene indices add little to the sync", "[.][velocity][bench]") {
    std::string body;
    for (int i = 0; i < 150; ++i)
        for (int j = 0; j < 150; ++j)
            body += TfStringPrintf("def Mesh \"m%d_%d\"\n{\n    int[] faceVertexCounts = [4]\n    int[] faceVertexIndices = [0, 1, 2, 3]\n"
                                   "    point3f[] points = [(%d, %d, 0), (%d.8, %d, 0), (%d.8, %d.8, 0), (%d, %d.8, 0)]\n"
                                   "    uniform token subdivisionScheme = \"none\"\n}\n",
                                   i, j, i, j, i, j, i, j, i, j);
    for (int variant = 0; variant < 4; ++variant) {
        SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
        layer->ImportFromString(std::string(kHeader) + body);
        UsdStageRefPtr stage = UsdStage::Open(layer);
        auto delegate = std::make_unique<HdNovaRenderDelegate>();
        static_cast<HdNovaRenderParam *>(delegate->GetRenderParam())
            ->SetShutter(HdNovaResolveShutter(-0.25, 0.25, false, false));
        std::unique_ptr<HdRenderIndex> index(HdRenderIndex::New(delegate.get(), HdDriverVector()));
        UsdImagingCreateSceneIndicesInfo info;
        info.stage = stage;
        info.addDrawModeSceneIndex = false;
        UsdImagingSceneIndices sis = UsdImagingCreateSceneIndices(info);
        sis.stageSceneIndex->SetTime(UsdTimeCode(1.0));
        HdSceneIndexBaseRefPtr si = sis.finalSceneIndex;
        if (variant == 1)
            si = HdsiVelocityMotionResolvingSceneIndex::New(si);
        else if (variant >= 2)
            si = HdNovaVelocityModeSceneIndex::Append(si, "");
        index->InsertSceneIndex(si, SdfPath::AbsoluteRootPath(), false);
        HdRenderPassSharedPtr pass = delegate->CreateRenderPass(
            index.get(), HdRprimCollection(HdTokens->geometry, HdReprSelector(HdReprTokens->refined)));
        HdTaskSharedPtrVector tasks{std::make_shared<SyncTask>(pass)};
        HdEngine engine;
        const auto t0 = std::chrono::steady_clock::now();
        engine.Execute(index.get(), &tasks);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        printf("variant %d: sync %.1f ms\n", variant, ms);
        tasks.clear();
        pass.reset();
        index.reset();
        delegate.reset();
    }
}

TEST_CASE("Velocity blur modes parse Karma's labels and numbers", "[hydra][motion][velocity]") {
    CHECK(HdNovaVelocityBlurMode(VtValue()) == -1);
    CHECK(HdNovaVelocityBlurMode(VtValue(0)) == 0);
    CHECK(HdNovaVelocityBlurMode(VtValue(1)) == 1);
    CHECK(HdNovaVelocityBlurMode(VtValue(2.0f)) == 2);
    CHECK(HdNovaVelocityBlurMode(VtValue(7)) == 2);
    CHECK(HdNovaVelocityBlurMode(VtValue(TfToken("No Velocity Blur"))) == 0);
    CHECK(HdNovaVelocityBlurMode(VtValue(TfToken("Velocity Blur"))) == 1);
    CHECK(HdNovaVelocityBlurMode(VtValue(std::string("Acceleration Blur"))) == 2);
    CHECK(HdNovaVelocityBlurMode(VtValue(std::string("1"))) == 1);
    CHECK(HdNovaVelocityBlurMode(VtValue(std::string(""))) == -1);
}

TEST_CASE("Velocities blur as Nova's and Karma's settings say", "[hydra][motion][velocity]") {
    Driver d(quad("still", 0, "") +                                                        //
             quad("nova", 10, primvar("nova:object:velocityBlur", 1)) +                    //
             "def Xform \"group\"\n{\n" + primvar("karma:object:vblur", 1) +               //
             quad("inherited", 20, "") +                                                   //
             quad("overridden", 30, primvar("nova:object:velocityBlur", 0)) + "}\n" +      //
             quad("accel", 40, kAccelerations + primvar("karma:object:vblur", 2) +         //
                                   primvar("karma:object:geosamples", 4)) +                //
             quad("accelNova", 50, kAccelerations + primvar("nova:object:velocityBlur", 2) + //
                                       primvar("nova:object:accelerationSamples", 5) +     //
                                       primvar("karma:object:geosamples", 4)) +            //
             quad("accelDefault", 60, kAccelerations + primvar("karma:object:vblur", 2)) + //
             quad("accelIgnored", 70, kAccelerations + primvar("karma:object:vblur", 1)));
    // No setting: the velocities do not move anything (Karma's default).
    checkSteps(d.xSteps(0), {0.0f});
    // Velocity blur: shutter open and close, 5 units apart.
    checkSteps(d.xSteps(10), {-2.5f, 2.5f});
    // Karma's vblur inherited from the parent; Nova's own wins over it.
    checkSteps(d.xSteps(20), {-2.5f, 2.5f});
    checkSteps(d.xSteps(30), {0.0f});
    // Acceleration blur at Karma's geosamples (4), Nova's own count (5) and
    // Karma's default (2: shutter open and close only).
    checkSteps(d.xSteps(40), {accelerated(-1), accelerated(-1.0f / 3), accelerated(1.0f / 3), accelerated(1)});
    checkSteps(d.xSteps(50), {accelerated(-1), accelerated(-0.5f), accelerated(0), accelerated(0.5f), accelerated(1)});
    checkSteps(d.xSteps(60), {accelerated(-1), accelerated(1)});
    // Velocity blur ignores the accelerations.
    checkSteps(d.xSteps(70), {-2.5f, 2.5f});
    // Nova's Max Motion Steps caps the acceleration samples.
    CHECK(d.record(50).desc.pointSteps <= d.param().GetMaxMotionSteps());
}

TEST_CASE("Velocities win over time samples, which blur without a setting", "[hydra][motion][velocity]") {
    Driver d(R"(
def Mesh "sampled"
{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points.timeSamples = {
        0: [(-10, 0, 0), (-8, 0, 0), (-8, 2, 0), (-10, 2, 0)],
        1: [(0, 0, 0), (2, 0, 0), (2, 2, 0), (0, 2, 0)],
        2: [(10, 0, 0), (12, 0, 0), (12, 2, 0), (10, 2, 0)],
    }
    vector3f[] velocities = [(480, 0, 0), (480, 0, 0), (480, 0, 0), (480, 0, 0)]
    uniform token subdivisionScheme = "none"
}
)");
    checkSteps(d.xSteps(0), {-2.5f, 0.0f, 2.5f}); // deformation: 10 units per frame, the frame's sample
    d.setInt("/sampled", "karma:object:vblur", 1);
    d.sync();
    checkSteps(d.xSteps(0), {-5.0f, 5.0f}); // the velocities: 480 units per second
}

TEST_CASE("Editing the velocity settings re-resolves the motion", "[hydra][motion][velocity][ipr]") {
    Driver d(quad("q", 0, kAccelerations));
    checkSteps(d.xSteps(0), {0.0f});
    d.setInt("/q", "nova:object:velocityBlur", 1);
    d.sync();
    checkSteps(d.xSteps(0), {-2.5f, 2.5f});
    d.setInt("/q", "nova:object:velocityBlur", 2);
    d.sync();
    checkSteps(d.xSteps(0), {accelerated(-1), accelerated(1)});
    d.setInt("/q", "karma:object:geosamples", 3);
    d.sync();
    checkSteps(d.xSteps(0), {accelerated(-1), accelerated(0), accelerated(1)});
    d.setInt("/q", "nova:object:velocityBlur", 0);
    d.sync();
    checkSteps(d.xSteps(0), {0.0f});
}

TEST_CASE("The render settings give the defaults and the frame rate", "[hydra][motion][velocity]") {
    Driver d(quad("q", 0, kAccelerations) + quad("own", 10, primvar("karma:object:vblur", 0)));
    checkSteps(d.xSteps(0), {0.0f});
    CHECK_FALSE(d.applyDefaults()); // nothing set: nothing to do

    // Karma's render settings author the menu label as a token.
    d.delegate->SetRenderSetting(TfToken("karma:object:vblur"), VtValue(TfToken("Acceleration Blur")));
    d.delegate->SetRenderSetting(TfToken("karma:object:geosamples"), VtValue(3));
    REQUIRE(d.applyDefaults());
    CHECK_FALSE(d.applyDefaults());
    d.sync();
    checkSteps(d.xSteps(0), {accelerated(-1), accelerated(0), accelerated(1)});
    checkSteps(d.xSteps(10), {0.0f}); // its own setting wins

    // Velocities are per second: at 48 frames per second the shutter is half as long.
    d.delegate->SetRenderSetting(TfToken("karma:object:vblur"), VtValue(1));
    d.delegate->SetRenderSetting(TfToken("houdini:fps"), VtValue(48.0));
    REQUIRE(d.applyDefaults());
    d.sync();
    checkSteps(d.xSteps(0), {-1.25f, 1.25f});
}

TEST_CASE("Curves and points blur by their velocities", "[hydra][motion][velocity]") {
    Driver d(R"(
def BasisCurves "curve"
{
    uniform token type = "linear"
    int[] curveVertexCounts = [2]
    point3f[] points = [(1, 0, 0), (1, 2, 0)]
    float[] widths = [0.1, 0.1] (interpolation = "vertex")
    vector3f[] velocities = [(240, 0, 0), (0, 0, 0)]
    int primvars:karma:object:vblur = 1 (interpolation = "constant")
}
def Points "particles"
{
    point3f[] points = [(1, 10, 0), (3, 10, 0)]
    float[] widths = [0.5, 0.5] (interpolation = "vertex")
    vector3f[] velocities = [(0, 0, 240), (0, 0, -240)]
    int primvars:nova:object:velocityBlur = 1 (interpolation = "constant")
}
)");
    nova::MeshDesc const &curve = d.record(0).desc;
    REQUIRE(curve.isCurves());
    REQUIRE(curve.pointSteps == 2);
    // Per point: the root moves, the tip stays.
    const uint32_t n = curve.pointCount();
    float rootMin = 1e9f, rootMax = -1e9f, tipMin = 1e9f, tipMax = -1e9f;
    for (uint32_t s = 0; s < 2; ++s)
        for (uint32_t i = 0; i < n; ++i) {
            nova::Vec3f const &p = curve.points[size_t(s) * n + i];
            float &lo = p.y < 1.0f ? rootMin : tipMin, &hi = p.y < 1.0f ? rootMax : tipMax;
            lo = std::min(lo, p.x);
            hi = std::max(hi, p.x);
        }
    CHECK_THAT(rootMax - rootMin, WithinAbs(5.0, 1e-4));
    CHECK_THAT(tipMax - tipMin, WithinAbs(0.0, 1e-6));

    nova::MeshDesc const &points = d.record(10).desc;
    REQUIRE(points.isPoints());
    REQUIRE(points.pointSteps == 2);
    CHECK_THAT(points.points[0].z, WithinAbs(-2.5, 1e-4));
    CHECK_THAT(points.points[1].z, WithinAbs(2.5, 1e-4));
    CHECK_THAT(points.points[2].z, WithinAbs(2.5, 1e-4));
    CHECK_THAT(points.points[3].z, WithinAbs(-2.5, 1e-4));
}

TEST_CASE("Instances move and turn by the instancer's velocities", "[hydra][motion][velocity][instancing]") {
    Driver d(R"(
def PointInstancer "inst"
{
    point3f[] positions = [(0, 0, 0)]
    vector3f[] velocities = [(240, 0, 0)]
    vector3f[] angularVelocities = [(0, 0, 4320)]
    int[] protoIndices = [0]
    rel prototypes = </inst/protos/quad>
    def Scope "protos"
    {
        def Mesh "quad"
        {
            int[] faceVertexCounts = [4]
            int[] faceVertexIndices = [0, 1, 2, 3]
            point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
            uniform token subdivisionScheme = "none"
        }
    }
}
)");
    auto record = [&]() -> nova::MeshRecord const & {
        for (uint32_t id = 0; id < d.scene().meshCount(); ++id)
            if (d.scene().meshRecord(id).alive && d.scene().meshRecord(id).instanced)
                return d.scene().meshRecord(id);
        FAIL("no instanced mesh");
        static nova::MeshRecord none;
        return none;
    };
    CHECK(record().motionSteps == 1); // Karma's default: no instance velocity blur
    // vblur is for the points of geometry, not for instances (Karma, measured).
    d.setInt("/inst", "karma:object:vblur", 1);
    d.sync();
    CHECK(record().motionSteps == 1);

    d.setInt("/inst", "karma:object:instance_vblur", 1);
    d.sync();
    nova::MeshRecord const &r = record();
    // Shutter open and close only (as Karma; pxr samples rotations three
    // times): 5 units apart, turned -45 and +45 degrees about z although
    // the instancer has no orientations (half-precision quaternions).
    REQUIRE(r.motionSteps == 2);
    REQUIRE(r.motionKeys.size() == r.motionSteps);
    const nova::Affine3f open = nova::srtToAffine(r.motionKeys.front());
    const nova::Affine3f close = nova::srtToAffine(r.motionKeys.back());
    CHECK_THAT(open.t.x, WithinAbs(-2.5, 1e-4));
    CHECK_THAT(close.t.x, WithinAbs(2.5, 1e-4));
    CHECK_THAT(std::atan2(open.x.y, open.x.x), WithinAbs(-M_PI / 4, 1e-3));
    CHECK_THAT(std::atan2(close.x.y, close.x.x), WithinAbs(M_PI / 4, 1e-3));

    // Acceleration blur at more samples turns along the arc in between.
    d.setInt("/inst", "karma:object:instance_vblur", 2);
    d.setInt("/inst", "karma:object:instance_samples", 5);
    d.sync();
    CHECK(record().motionSteps == 5);

    // Nova's own instance setting wins.
    d.setInt("/inst", "nova:object:instanceVelocityBlur", 0);
    d.sync();
    CHECK(record().motionSteps == 1);
}
